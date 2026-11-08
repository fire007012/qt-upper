#include "ControlSession.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QSettings>
#include <QUuid>
#include <cmath>

namespace Communication {
namespace {
bool running(const QJsonObject &op) { return QStringList{"awaiting_response", "accepted", "running"}.contains(op["state"].toString()); }
bool finiteNumber(const QJsonValue &value, double minimum, double maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= minimum && value.toDouble() <= maximum;
}
}
ControlSession::ControlSession(QObject *parent, std::function<qint64()> clock) : QObject(parent), m_now(std::move(clock))
{
    m_clock.start(); m_timer.setInterval(20); m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ControlSession::poll);
}
qint64 ControlSession::now() const { return m_now ? m_now() : m_clock.elapsed(); }
bool ControlSession::tokenValid() const { return !m_token.isEmpty() && now() < m_tokenUntil; }
bool ControlSession::ready() const {
    return m_active && negotiated() && tokenValid() && m_lastStatus >= 0 && now() - m_lastStatus < 500;
}
bool ControlSession::neutralReady() const {
    return ready() && m_physicalNeutral && m_inputSampleAt >= 0 && now() - m_inputSampleAt < 250
        && m_neutralAckAt >= 0 && now() - m_neutralAckAt < 250;
}
bool ControlSession::pendingType(const QString &type) const {
    for (const auto &p : m_pending) if (p.type == type) return true;
    return false;
}
void ControlSession::notify(const QString &code, const QString &message) {
    emit event({{"type", "control_notice"}, {"code", code}, {"message", message}});
}
void ControlSession::revoke(const QString &reason, bool clearToken)
{
    ++m_authorizationGeneration;
    const bool hadMotion = m_motionAuthorized, hadOperations = m_operationsAuthorized;
    m_motionAuthorized = false; m_operationsAuthorized = false;
    m_neutralAckAt = -1; m_inputSampleAt = -1; m_physicalNeutral = false;
    if (clearToken) { m_token.clear(); m_tokenUntil = -1; }
    if (hadMotion) emit motionAuthorizationChanged(false);
    if (hadOperations) emit operationsAuthorizationChanged(false);
    if (hadMotion || hadOperations) notify(reason, QStringLiteral("授权已撤销，需重新采样中立输入并显式授权。"));
    emit inputResetRequested(); emit changed();
}
void ControlSession::reset()
{
    m_active = false; m_timer.stop();
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it->type == "system_command" || it->type == "camera_stream_request") {
            auto record = m_records[it.key()].toObject();
            record["state"] = "result_unknown"; record["error"] = "RESPONSE_LOST";
            m_records[it.key()] = record;
        }
    }
    if (!m_scope.isEmpty()) persist();
    m_pending.clear(); m_session.clear(); m_epoch.clear(); m_status = {};
    m_lastStatus = m_lastFresh = -1; m_permitted = false; m_emergency = true;
    revoke("DISCONNECTED");
}
void ControlSession::begin(const QString &robotId, const QString &endpoint)
{
    reset(); m_robot = robotId; m_sequence = 0; m_records = {}; m_operationOrder.clear(); m_operationCursor = 0;
    m_scope = QString::fromLatin1(QCryptographicHash::hash((endpoint + '/' + robotId).toUtf8(), QCryptographicHash::Sha256).toHex());
    if (robotId.isEmpty()) { fail(QStringLiteral("必须填写机器人身份才能协商控制v2。"), false); return; }
    m_journalHealthy = true; restore();
    m_active = true; m_started = now(); m_nextFresh = m_nextStatus = m_nextOperation = now();
    m_tokenExpired = m_statusExpired = false; m_timer.start();
    send("control_hello", {{"robot_id", m_robot}, {"role", "control"}, {"capabilities", QJsonArray::fromStringList(ControlProtocol::capabilities())}});
}
QString ControlSession::send(const QString &type, QJsonObject fields, bool fresh, bool neutral, const QString &requestId)
{
    if (!m_active || (fresh && !tokenValid()) || m_pending.size() >= 32) return {};
    const auto id = requestId.isEmpty() ? QStringLiteral("control-") + QUuid::createUuid().toString(QUuid::WithoutBraces) : requestId;
    if (m_pending.contains(id)) return {};
    Pending pending; pending.type = type; pending.sentAt = now(); pending.epoch = m_epoch; pending.physicalNeutral = neutral;
    pending.authorizationGeneration = m_authorizationGeneration;
    pending.operationId = fields["operation_id"].toString();
    pending.deadline = now() + (type == "operator_input" || type == "freshness_request" ? 300 : 1000);
    fields["type"] = type; fields["protocol_version"] = 2; fields["request_id"] = id;
    if (type != "control_hello") { fields["session_id"] = m_session; fields["epoch"] = m_epoch; }
    if (fresh) {
        if (m_sequence >= 9007199254740991ULL) { fail(QStringLiteral("控制序号已耗尽，需要新会话。")); return {}; }
        pending.seq = ++m_sequence;
        fields["seq"] = static_cast<qint64>(pending.seq); fields["freshness_token"] = m_token; fields["ttl_ms"] = 300;
    }
    if (QJsonDocument(fields).toJson(QJsonDocument::Compact).size() + 1 > 65536) return {};
    m_pending.insert(id, pending); // Before emitting: transport errors may synchronously reset us.
    emit outbound(fields);
    return id;
}
void ControlSession::fail(const QString &message, bool retry)
{
    reset(); emit fault(message, retry);
}
bool ControlSession::acceptToken(const QJsonObject &value, const Pending &request)
{
    if (value["freshness_token"].toString().isEmpty()
        || !finiteNumber(value["valid_for_ms"], 0, 300) || value["refresh_ms"] != QJsonValue(100)
        || value["input_ttl_ms"] != QJsonValue(300) || value["lease_ms"] != QJsonValue(1500)
        || !finiteNumber(value["watchdog_ms"], 1, 500)) return false;
    // Use the request's send time, not receive time: RTT cannot extend a server token.
    const auto until = request.sentAt + static_cast<qint64>(value["valid_for_ms"].toDouble());
    if (until <= now()) { revoke("TOKEN_EXPIRED"); return true; }
    m_token = value["freshness_token"].toString(); m_tokenUntil = until;
    m_lastFresh = now(); m_tokenExpired = false;
    emit inputSampleRequested(); // No retained input is ever retokenized here.
    return true;
}
void ControlSession::expireRequest(const QString &id, const Pending &request)
{
    if (request.type == "system_command" || request.type == "camera_stream_request") {
        auto record = m_records[id].toObject(); record["state"] = "result_unknown"; record["error"] = "RESPONSE_TIMEOUT";
        saveRecord(id, record); notify("RESULT_UNKNOWN", QStringLiteral("操作接受响应超时；不代表未执行，请查询或用原ID恢复。"));
        revoke("RESULT_UNKNOWN");
    } else if (request.type == "authorize" || request.type == "authorize_operations" || request.type == "clear_emergency") {
        revoke("AUTHORIZATION_UNKNOWN");
    }
}
void ControlSession::poll()
{
    if (!m_active) return;
    const auto time = now();
    if (!negotiated()) {
        if (time - m_started >= 2500) fail(QStringLiteral("控制v2协商超时；正在重建连接重新核对版本及能力。"));
        return;
    }
    QList<QString> expired;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) if (time >= it->deadline) expired.append(it.key());
    for (const auto &id : expired) {
        expireRequest(id, m_pending.take(id));
    }
    if (!tokenValid() && !m_tokenExpired) { m_tokenExpired = true; revoke("TOKEN_EXPIRED"); }
    if (m_lastStatus >= 0 && time - m_lastStatus >= 500 && !m_statusExpired) {
        m_statusExpired = true; m_permitted = false; revoke("STATUS_EXPIRED");
    }
    if ((m_lastFresh >= 0 && time - m_lastFresh >= 1500) || (m_lastStatus >= 0 && time - m_lastStatus >= 1000)) {
        fail(QStringLiteral("控制租约/实际状态查询失效，正在建立新会话。")); return;
    }
    if (m_lastStatus < 0 && time - m_started >= 1500) {
        fail(QStringLiteral("控制状态首次同步超时。")); return;
    }
    if (time >= m_nextFresh && !pendingType("freshness_request")) {
        m_nextFresh = time + 100; send("freshness_request");
    }
    if (time >= m_nextStatus && !pendingType("control_status")) {
        m_nextStatus = time + 200; send("control_status");
    }
    if (time >= m_nextOperation && !pendingType("operation_query")) { m_nextOperation = time + 500; queryNextOperation(); }
}
bool ControlSession::input(const OperatorInputState &state)
{
    QJsonObject payload; bool neutral = false;
    if (!ControlProtocol::input(state, &payload, &neutral)) { revoke("INPUT_SCHEMA"); return false; }
    m_physicalNeutral = neutral; m_inputSampleAt = now();
    if (!m_active || !negotiated() || !tokenValid() || pendingType("operator_input")) return false;
    if (!m_motionAuthorized) {
        // Send a zero frame while paused, but never count it as a physical neutral sample.
        bool unused; ControlProtocol::input(OperatorInputState{}, &payload, &unused);
    }
    return !send("operator_input", payload, true, neutral).isEmpty();
}
bool ControlSession::authorizeMotion()
{
    if (!neutralReady() || !m_permitted || m_emergency || pendingType("authorize")) return false;
    return !send("authorize", {}, true).isEmpty();
}
bool ControlSession::authorizeOperations()
{
    if (!neutralReady() || m_emergency || pendingType("authorize_operations")) return false;
    return !send("authorize_operations", {}, true).isEmpty();
}
bool ControlSession::clearEmergency()
{
    if (!neutralReady() || pendingType("clear_emergency")) return false;
    revoke("CLEAR_EMERGENCY", false);
    return !send("clear_emergency", {}, true).isEmpty();
}
void ControlSession::emergencyIntent() { m_emergency = true; revoke("EMERGENCY_ACTIVE"); }
void ControlSession::pause() { if (m_active) fail(QStringLiteral("已暂停控制，正在获取新的未授权会话。")); }
bool ControlSession::persist()
{
    if (!m_journalHealthy) return false; // Preserve corrupt evidence instead of replacing it.
    if (m_scope.isEmpty()) return true;
    QSettings settings("HostComputer", "ControlV2");
    settings.setValue(m_scope + "/records", QJsonDocument(m_records).toJson(QJsonDocument::Compact)); settings.sync();
    if (settings.status() != QSettings::NoError) { notify("LOCAL_JOURNAL", QStringLiteral("无法保存操作恢复记录，本次新操作未发送。")); return false; }
    return true;
}
void ControlSession::restore()
{
    QSettings settings("HostComputer", "ControlV2");
    const auto bytes = settings.value(m_scope + "/records").toByteArray();
    if (bytes.isEmpty() && settings.status() == QSettings::NoError) return;
    const auto document = QJsonDocument::fromJson(bytes);
    const auto records = document.object();
    m_journalHealthy = settings.status() == QSettings::NoError && bytes.size() <= 4 * 1024 * 1024
        && document.isObject() && records.size() <= 64;
    for (auto it = records.begin(); m_journalHealthy && it != records.end(); ++it) {
        auto record = it.value().toObject();
        m_journalHealthy = !it.key().isEmpty() && record["robot_id"] == QJsonValue(m_robot)
            && record["request_id"] == QJsonValue(it.key()) && record["request_type"] == QJsonValue("system_command")
            && record["params"].isObject()
            && QStringList{"init","enable","disable","halt","resume","recover","shutdown","set_control_mode","request_arm_named_targets","move_arm_named_target"}.contains(record["command"].toString())
            && QStringList{"awaiting_response","accepted","running","succeeded","failed","result_unknown"}.contains(record["state"].toString())
            && (record["operation_id"].isNull() || ControlProtocol::operation(record, m_robot));
        if (!m_journalHealthy) break;
        if (record["state"] == QJsonValue("awaiting_response")) { record["state"] = "result_unknown"; record["error"] = "CLIENT_RESTART"; }
        m_records[it.key()] = record; m_operationOrder.append(it.key());
    }
    if (!m_journalHealthy) {
        m_records = {}; m_operationOrder.clear();
        notify("LOCAL_JOURNAL", QStringLiteral("操作恢复记录损坏或超过上限；已阻止新业务操作，请离线核查原记录。"));
    }
}
void ControlSession::saveRecord(const QString &id, const QJsonObject &record)
{
    m_records[id] = record;
    if (!m_operationOrder.contains(id)) m_operationOrder.append(id);
    persist(); emit event({{"type", "control_operation"}, {"operation", ControlProtocol::redact(record)}}); emit changed();
}
bool ControlSession::command(const QString &command, const QJsonObject &params)
{
    if (!ready() || !m_journalHealthy || !m_operationsAuthorized || !QStringList{"init","enable","disable","halt","resume","recover","shutdown","set_control_mode","request_arm_named_targets","move_arm_named_target"}.contains(command)
        || (command == "move_arm_named_target" && (!m_motionAuthorized || !m_permitted))) return false;
    for (auto it = m_records.begin(); it != m_records.end(); ++it) {
        if (it.value().toObject()["state"] == QJsonValue("result_unknown")
            && !QStringList{"halt","disable","shutdown"}.contains(command)) {
            notify("RESULT_UNKNOWN", QStringLiteral("请先核对未知操作结果，再提交新业务操作。")); return false;
        }
    }
    // Keep local history bounded without removing unresolved intent.
    if (m_records.size() >= 64) {
        for (const auto &id : m_operationOrder) {
            const auto record = m_records[id].toObject();
            if (record["state"] == QJsonValue("succeeded") || record["state"] == QJsonValue("failed")) {
                m_records.remove(id); m_operationOrder.removeOne(id); break;
            }
        }
        if (m_records.size() >= 64) { notify("LOCAL_CACHE_FULL"); return false; }
    }
    int active = 0;
    for (auto it = m_records.begin(); it != m_records.end(); ++it) if (running(it.value().toObject())) ++active;
    if (active >= 16 || m_pending.size() >= 30) return false;
    const auto id = QStringLiteral("control-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject record{{"request_id", id}, {"robot_id", m_robot}, {"request_type", "system_command"},
        {"command", command}, {"params", params}, {"state", "awaiting_response"}, {"operation_id", QJsonValue(QJsonValue::Null)},
        {"created_at", QDateTime::currentSecsSinceEpoch()}};
    m_records[id] = record; m_operationOrder.append(id);
    if (!persist()) { m_records.remove(id); m_operationOrder.removeOne(id); return false; }
    const auto sent = send("system_command", {{"command", command}, {"params", params}}, true, false, id);
    if (sent.isEmpty()) { m_records.remove(id); m_operationOrder.removeOne(id); persist(); return false; }
    emit event({{"type", "control_operation"}, {"operation", ControlProtocol::redact(record)}});
    if (QStringList{"halt","disable","shutdown","recover","set_control_mode"}.contains(command)) {
        // Stop local continuous motion immediately; never re-enable it from a status ACK.
        revoke("OPERATION_REAUTHORIZATION", false);
    }
    emit changed(); return true;
}
bool ControlSession::recoverRequest(const QString &id)
{
    if (!ready() || !m_journalHealthy || m_pending.contains(id) || !m_records.contains(id)) return false;
    const auto record = m_records[id].toObject();
    const auto oid = record["operation_id"].toString();
    if (!oid.isEmpty()) return !send("operation_query", {{"operation_id", oid}}).isEmpty();
    // Original ID/command/params. A new server-side intent may require fresh explicit operations authorization.
    return !send(record["request_type"].toString(), {{"command", record["command"]}, {"params", record["params"]}}, true, false, id).isEmpty();
}
void ControlSession::queryNextOperation()
{
    if (m_operationOrder.isEmpty()) return;
    for (int i = 0; i < m_operationOrder.size(); ++i) {
        m_operationCursor %= m_operationOrder.size();
        const auto record = m_records[m_operationOrder[m_operationCursor++]].toObject();
        if ((running(record) || record["state"] == QJsonValue("result_unknown")) && !record["operation_id"].toString().isEmpty()) {
            send("operation_query", {{"operation_id", record["operation_id"]}}); return;
        }
    }
}
bool ControlSession::acceptOperation(const QJsonObject &op, const QString &expectedId)
{
    if (!ControlProtocol::operation(op, m_robot) || op["request_id"] != QJsonValue(expectedId) || !m_records.contains(expectedId)) return false;
    const auto old = m_records[expectedId].toObject();
    if (op["command"] != old["command"] || (!old["operation_id"].toString().isEmpty() && old["operation_id"] != op["operation_id"])) return false;
    auto record = op; record["params"] = old["params"]; record["request_type"] = old["request_type"];
    if (record != old) {
        saveRecord(expectedId, record);
        if (!running(record)) { m_nextStatus = 0; emit stateRefreshRequested(); }
        if (op["invalidated"].toBool() || op["state"] == QJsonValue("result_unknown")) {
            revoke("OPERATION_RECONCILE"); m_nextStatus = 0;
        }
        if (op["result"].isArray() && !op["invalidated"].toBool() && op["state"] == QJsonValue("succeeded")) {
            for (const auto &result : op["result"].toArray()) if (result.isObject()) emit businessResult(result.toObject());
        }
    }
    return true;
}
void ControlSession::rejected(const Pending &request, const QString &id, const QString &code)
{
    notify(code, QStringLiteral("控制请求被拒绝：%1").arg(request.type));
    if (request.type == "system_command" || request.type == "camera_stream_request") {
        auto record = m_records[id].toObject();
        // A recovery attempt being denied cannot prove the original attempt did not run.
        if (record["state"] == QJsonValue("awaiting_response") && code != "PERSISTENCE" && code != "RESPONSE_TOO_LARGE") record["state"] = "failed";
        else record["state"] = "result_unknown";
        record["error"] = code; saveRecord(id, record);
        if (record["state"] == QJsonValue("result_unknown")) revoke("RESULT_UNKNOWN");
    } else if (request.type == "operation_query" && code == "RESULT_UNKNOWN") {
        for (auto it = m_records.begin(); it != m_records.end(); ++it) {
            auto record = it.value().toObject();
            if (record["operation_id"] == QJsonValue(request.operationId)) {
                record["state"] = "result_unknown"; record["error"] = code; saveRecord(it.key(), record); break;
            }
        }
        revoke("RESULT_UNKNOWN");
    }
    if (code == "EPOCH") {
        revoke(code); m_lastStatus = -1; m_nextStatus = 0; send("control_status"); return;
    }
    if (QStringList{"SESSION","LEASE","OWNER_BUSY","NEGOTIATION_TIMEOUT"}.contains(code)) { fail(QStringLiteral("控制会话失效：%1").arg(code)); return; }
    if (request.type == "control_hello" || QStringList{"UPGRADE_REQUIRED","IDENTITY_ROLE","CAPABILITIES","VERSION"}.contains(code)) {
        fail(QStringLiteral("控制v2不兼容或机器人身份不匹配：%1").arg(code), false); return;
    }
    if (QStringList{"STALE_TOKEN","REPLAY","SEQ","RUNTIME_PERMISSION","EMERGENCY_ACTIVE","NOT_AUTHORIZED","OPERATIONS_NOT_AUTHORIZED"}.contains(code)) {
        revoke(code); m_nextFresh = m_nextStatus = 0;
    }
}
void ControlSession::receive(const QJsonObject &message)
{
    if (!m_active) return;
    if (message["type"] != QJsonValue("control_result") || message["protocol_version"] != QJsonValue(2) || !message["ok"].isBool()) {
        fail(QStringLiteral("控制v2响应外壳无效。"), false); return;
    }
    const auto id = message["request_id"].toString();
    if (!m_pending.contains(id)) return; // Late/old connection ACKs cannot grant authorization.
    const auto request = m_pending.value(id);
    if ((request.seq == 0 ? !message["seq"].isNull() : message["seq"] != QJsonValue(static_cast<qint64>(request.seq)))) {
        fail(QStringLiteral("控制v2响应序号不匹配。"), false); return;
    }
    m_pending.remove(id); m_lastRtt = now() - request.sentAt;
    if (request.type == "freshness_request") emit event({{"type", "control_timing"}, {"rtt_ms", m_lastRtt}});
    if (now() >= request.deadline) { expireRequest(id, request); revoke("LATE_RESPONSE"); return; }
    if (request.type != "control_hello" && request.type != "control_status" && request.epoch != m_epoch) {
        expireRequest(id, request); return;
    }
    if (!message["ok"].toBool()) {
        const auto code = message["error"].toObject()["code"].toString();
        if (code.isEmpty()) { fail(QStringLiteral("控制v2错误字段无效。"), false); return; }
        rejected(request, id, code); emit changed(); return;
    }
    if (!message["result"].isObject()) { fail(QStringLiteral("控制v2结果字段无效。"), false); return; }
    const auto result = message["result"].toObject();
    bool valid = true;
    if (request.type == "control_hello") {
        const auto caps = result["capabilities"].toArray();
        valid = result["robot_id"] == QJsonValue(m_robot) && result["role"] == QJsonValue("control")
            && !result["session_id"].toString().isEmpty() && !result["epoch"].toString().isEmpty()
            && result["authorized"] == QJsonValue(false) && !result["build_version"].toString().isEmpty();
        for (const auto &cap : ControlProtocol::capabilities()) valid &= caps.contains(cap);
        if (valid) {
            m_session = result["session_id"].toString(); m_epoch = result["epoch"].toString(); m_build = result["build_version"].toString();
            valid = acceptToken(result, request); m_nextFresh = now() + 100; send("control_status");
        }
    } else if (request.type == "freshness_request") {
        valid = result["epoch"] == QJsonValue(m_epoch) && acceptToken(result, request);
    } else if (request.type == "control_status") {
        valid = !result["epoch"].toString().isEmpty() && result["authorized"].isBool()
            && result["motion_permitted"].isBool() && result["emergency_active"].isBool()
            && result["counts"].isObject() && finiteNumber(result["queue_depth"], 0, 16);
        if (valid) {
            const auto epoch = result["epoch"].toString();
            if (epoch != m_epoch) {
                m_epoch = epoch; revoke("EPOCH"); m_nextFresh = 0;
                // Replies from the previous epoch remain correlated but cannot alter the new session.
            }
            const bool revoked = (!result["authorized"].toBool() && m_motionAuthorized)
                || (!result["motion_permitted"].toBool() && m_motionAuthorized) || result["emergency_active"].toBool();
            if (revoked && (m_motionAuthorized || m_operationsAuthorized)) revoke("SERVER_REVOKED");
            m_status = result; m_lastStatus = now(); m_statusExpired = false;
            m_permitted = result["motion_permitted"].toBool(); m_emergency = result["emergency_active"].toBool();
            emit event({{"type", "control_status"}, {"status", result}});
        }
    } else if (request.type == "operator_input") {
        valid = QStringList{"neutral","staged"}.contains(result["input_status"].toString());
        if (valid && request.physicalNeutral && m_physicalNeutral) m_neutralAckAt = now();
    } else if (request.type == "authorize" || request.type == "authorize_operations") {
        const auto field = request.type == "authorize" ? QStringLiteral("authorized") : QStringLiteral("operations_authorized");
        valid = result[field] == QJsonValue(true);
        if (valid && request.authorizationGeneration == m_authorizationGeneration && neutralReady()
            && !m_emergency && (request.type != "authorize" || m_permitted)) {
            if (request.type == "authorize") { m_motionAuthorized = true; emit motionAuthorizationChanged(true); }
            else { m_operationsAuthorized = true; emit operationsAuthorizationChanged(true); }
        }
    } else if (request.type == "clear_emergency") {
        valid = result["emergency_active"] == QJsonValue(false) && result["authorized"] == QJsonValue(false);
        if (valid) { m_emergency = false; revoke("CLEAR_EMERGENCY"); m_nextFresh = m_nextStatus = 0; }
    } else if (request.type == "system_command" || request.type == "camera_stream_request") {
        valid = acceptOperation(result, id);
    } else if (request.type == "operation_query") {
        valid = result["operation_id"] == QJsonValue(request.operationId) && acceptOperation(result, result["request_id"].toString());
    }
    if (!valid) { fail(QStringLiteral("控制v2响应字段、身份或能力无效。"), false); return; }
    emit changed();
}
QJsonArray ControlSession::operations() const {
    QJsonArray result; for (const auto &id : m_operationOrder) result.append(ControlProtocol::redact(m_records[id])); return result;
}
QJsonObject ControlSession::diagnostics() const {
    return {{"protocol_version", 2}, {"robot_id", m_robot}, {"build_version", m_build}, {"negotiated", negotiated()},
        {"ready", ready()}, {"epoch", m_epoch}, {"token_valid", tokenValid()}, {"motion_authorized", m_motionAuthorized},
        {"operations_authorized", m_operationsAuthorized}, {"journal_healthy", m_journalHealthy}, {"motion_permitted", ready() && m_permitted},
        {"emergency_active", m_emergency}, {"pending_requests", m_pending.size()}, {"last_rtt_ms", m_lastRtt},
        {"status_age_ms", m_lastStatus < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(now() - m_lastStatus)},
        {"input_age_ms", m_inputSampleAt < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(now() - m_inputSampleAt)},
        {"server", m_status}, {"operations", operations()}};
}
}
