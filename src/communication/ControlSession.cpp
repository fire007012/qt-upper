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
    return m_active && negotiated() && (m_role == "monitor" || tokenValid()) && m_lastStatus >= 0 && now() - m_lastStatus < 500 && m_status["epoch"] == QJsonValue(m_epoch);
}
bool ControlSession::neutralReady() const {
    return m_role == "control" && ready() && m_physicalNeutral && m_inputSampleAt >= 0 && now() - m_inputSampleAt < 250
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
    m_active = false; m_timer.stop(); m_flow = false; m_flowPhase.clear(); m_presetAware = false; m_selectedPresets = {}; m_selectedGroups = {};
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
void ControlSession::begin(const QString &robotId, const QString &endpoint, bool motionPresets, const QString &role)
{
    reset(); m_role = role; m_robot = robotId; m_sequence = 0; m_records = {}; m_operationOrder.clear(); m_operationCursor = 0;
    m_scope = QString::fromLatin1(QCryptographicHash::hash((endpoint + '/' + robotId).toUtf8(), QCryptographicHash::Sha256).toHex());
    if (robotId.isEmpty()) { fail(QStringLiteral("必须填写机器人身份才能协商控制v2。"), false); return; }
    if (role != "control" && role != "monitor") { fail(QStringLiteral("控制角色无效。"), false); return; }
    m_journalHealthy = true; if (role == "control") restore();
    m_active = true; m_started = now(); m_nextFresh = m_nextStatus = m_nextOperation = now();
    m_tokenExpired = m_statusExpired = false; m_timer.start();
    m_presetAware = motionPresets;
    if (m_presetAware && m_role == "control" && !validPresets(m_desiredPresets)) { fail(QStringLiteral("请选择至少一个运动预设后重新连接。"), false); return; }
    auto caps = QJsonArray::fromStringList(ControlProtocol::capabilities());
    QJsonObject hello{{"robot_id", m_robot}, {"role", m_role}};
    if (m_presetAware) { caps.append("motion_presets_v1"); if (m_role == "control") hello["presets"] = m_desiredPresets; }
    hello["capabilities"] = caps; send("control_hello", hello);
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
        if (m_role != "control") return {};
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
    if (request.type == "select_presets") { if (m_flow) stopPresetFlow("SELECTION_RESULT_UNKNOWN"); revoke("SELECTION_RESULT_UNKNOWN"); m_nextStatus = 0; }
    if (request.type == "system_command" || request.type == "camera_stream_request") {
        if (m_flow) stopPresetFlow("RESULT_UNKNOWN");
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
    expireValidity();
    if ((m_lastFresh >= 0 && time - m_lastFresh >= 1500) || (m_lastStatus >= 0 && time - m_lastStatus >= 1000)) {
        fail(QStringLiteral("控制租约/实际状态查询失效，正在建立新会话。")); return;
    }
    if (m_lastStatus < 0 && time - m_started >= 1500) {
        fail(QStringLiteral("控制状态首次同步超时。")); return;
    }
    if (m_role == "control" && time >= m_nextFresh && !pendingType("freshness_request")) {
        m_nextFresh = time + 100; send("freshness_request");
    }
    if (time >= m_nextStatus && !pendingType("control_status")) {
        m_nextStatus = time + 200; send("control_status");
    }
    if (time >= m_nextOperation && !pendingType("operation_query")) { m_nextOperation = time + 200; queryNextOperation(); }
    advancePresets();
}
void ControlSession::expireValidity()
{
    if (!tokenValid() && !m_tokenExpired) { m_tokenExpired = true; revoke("TOKEN_EXPIRED"); }
    if (m_lastStatus >= 0 && now() - m_lastStatus >= 500 && !m_statusExpired) {
        m_statusExpired = true; m_permitted = false; revoke("STATUS_EXPIRED");
    }
}
bool ControlSession::input(const OperatorInputState &state)
{
    QJsonObject payload; bool neutral = false;
    if (!ControlProtocol::input(state, &payload, &neutral)) { revoke("INPUT_SCHEMA"); return false; }
    m_physicalNeutral = neutral; m_inputSampleAt = now();
    if (m_role != "control" || !m_active || !negotiated() || !tokenValid() || pendingType("operator_input")) return false;
    if (!m_motionAuthorized) {
        // Send a zero frame while paused, but never count it as a physical neutral sample.
        bool unused; ControlProtocol::input(OperatorInputState{}, &payload, &unused);
    }
    if (m_presetAware && m_motionAuthorized) {
        auto keys = payload["keyboard"].toObject()["pressed_keys"].toArray();
        auto gamepad = payload["gamepad"].toObject(); auto axes = gamepad["axes"].toObject(); auto buttons = gamepad["buttons"].toObject();
        QStringList allowed;
        if (state.mode == "arm" && m_selectedPresets.contains("arm")) allowed = {"u","o","a","d","w","s","q","e","k","i","j","l","f","h"};
        else if (state.mode == "vehicle") {
            if (m_selectedPresets.contains("base")) allowed += QStringList{"w","a","s","d"};
            else for (const auto &axis : {"left_x","left_y","right_x"}) axes[axis] = 0;
            if (m_selectedPresets.contains("flippers")) allowed += QStringList{"y","h","u","j","i","k","o","l"};
            else { axes["lt"] = 0; axes["rt"] = 0; for (auto it = buttons.begin(); it != buttons.end(); ++it) it.value() = false; }
        }
        if ((state.mode == "arm" && !m_selectedPresets.contains("arm")) || (state.mode != "arm" && state.mode != "vehicle")) {
            for (auto it = axes.begin(); it != axes.end(); ++it) it.value() = 0;
            for (auto it = buttons.begin(); it != buttons.end(); ++it) it.value() = false;
        }
        QJsonArray filtered; for (const auto &key : keys) if (allowed.contains(key.toString())) filtered.append(key);
        payload["keyboard"] = QJsonObject{{"pressed_keys",filtered}};
        gamepad["axes"] = axes; gamepad["buttons"] = buttons; payload["gamepad"] = gamepad;
    }
    return !send("operator_input", payload, true, neutral).isEmpty();
}
bool ControlSession::authorizeMotion()
{
    if (!neutralReady() || busy() || (m_presetAware && (m_selectedPresets.isEmpty() || m_desiredPresets.isEmpty())) || !m_permitted || m_emergency || pendingType("authorize")) return false;
    return !send("authorize", {}, true).isEmpty();
}
bool ControlSession::authorizeOperations()
{
    if (!neutralReady() || busy() || (m_presetAware && (m_selectedPresets.isEmpty() || (m_desiredPresets.isEmpty() && !(m_flow && !m_flowStart)))) || m_emergency || pendingType("authorize_operations")) return false;
    return !send("authorize_operations", {}, true).isEmpty();
}
bool ControlSession::clearEmergency()
{
    if (!neutralReady() || pendingType("clear_emergency")) return false;
    revoke("CLEAR_EMERGENCY", false);
    return !send("clear_emergency", {}, true).isEmpty();
}
void ControlSession::emergencyIntent() { m_flow = false; m_flowPhase = "急停：全车停止，需显式重新启动"; m_emergency = true; revoke("EMERGENCY_ACTIVE"); }
void ControlSession::pause() { if (m_active) fail(QStringLiteral("已暂停控制，正在获取新的未授权会话。")); }
bool ControlSession::validPresets(const QJsonArray &presets)
{
    if (presets.isEmpty()) return false;
    QJsonArray seen;
    for (const auto &id : presets) {
        if (!id.isString() || !QJsonArray{"flippers","base","arm"}.contains(id) || seen.contains(id)) return false;
        seen.append(id);
    }
    return true;
}
QJsonArray ControlSession::groupsFor(const QJsonArray &presets)
{
    QJsonArray groups;
    for (const auto &id : presets) {
        groups.append(id);
        if (id == QJsonValue("arm")) groups.append("gripper");
    }
    return groups;
}
bool ControlSession::busy() const
{
    for (const auto &value : m_records) if (running(value.toObject())) return true;
    return pendingType("select_presets");
}
bool ControlSession::acceptSelection(const QJsonObject &result)
{
    const auto presets = result["selected_presets"].toArray();
    const auto groups = result["selected_groups"].toArray();
    const auto expected = groupsFor(presets);
    if (!validPresets(presets) || groups.size() != expected.size()) return false;
    for (const auto &g : expected) if (!groups.contains(g)) return false;
    m_selectedPresets = presets; m_selectedGroups = groups; return true;
}
bool ControlSession::setPendingPresets(const QJsonArray &presets)
{
    // Empty is a valid draft, but can never be sent as a run request.
    if (!presets.isEmpty() && !validPresets(presets)) return false;
    m_desiredPresets = presets; emit changed(); return true;
}
bool ControlSession::selectPresets(const QJsonArray &presets)
{
    if (!m_presetAware || !validPresets(presets) || !neutralReady() || busy()) return false;
    if (!m_status["runtime_fresh"].toBool()) return false;
    for (const auto &id : m_selectedPresets) if (!presets.contains(id)) {
        const auto preset = m_status["presets"].toObject()[id.toString()].toObject();
        for (const auto &g : groupsFor(QJsonArray{id})) {
            const auto state = preset["states"].toObject()[g.toString()].toString();
            const auto diagnostic = preset["diagnostics"].toObject()[g.toString()].toObject();
            if (!QStringList{"Stopped","Halted"}.contains(state) || !diagnostic["state"].isDouble()
                || (diagnostic["state"].toInt(-1) != 0 && diagnostic["state"].toInt(-1) != 3)) {
                notify("HALT_REMOVED_PRESETS", QStringLiteral("取消前需停止该预设全部底层组，并取得新鲜停止反馈和诊断。")); return false;
            }
        }
    }
    revoke("PRESETS_CHANGED", false);
    return !send("select_presets", {{"presets",presets}}, true).isEmpty();
}
void ControlSession::stopPresetFlow(const QString &reason)
{
    m_flow = false; m_flowPhase = reason; revoke("PRESET_FLOW_PAUSED", false);
    notify(reason, QStringLiteral("预设流程已暂停；请查看各组状态和任务详情。失败或结果未知不会自动回滚、重发或恢复遥控。"));
}
bool ControlSession::startPresets(const QJsonArray &presets)
{
    if (m_role != "control" || !m_presetAware || !ready() || !validPresets(presets) || m_flow || busy() || m_emergency) return false;
    for (const auto &value : m_records) if (value.toObject()["state"] == QJsonValue("result_unknown")) return false;
    m_desiredPresets = presets; m_flowTarget = presets; m_flow = true; m_flowStart = true;
    m_flowRequest.clear(); m_flowPhase = "等待中立输入 / 同步选择"; m_flowDeadline = now() + 60000;
    revoke("PRESET_START", false); emit inputSampleRequested(); emit changed(); return true;
}
bool ControlSession::haltPresets(const QJsonArray &presets)
{
    if (m_role != "control" || !m_presetAware || !ready() || !validPresets(presets) || m_flow || busy()) return false;
    for (const auto &id : presets) if (!m_selectedPresets.contains(id)) return false;
    m_flowTarget = presets; m_flow = true; m_flowStart = false;
    m_flowRequest.clear(); m_flowPhase = "等待中立输入 / 停止"; m_flowDeadline = now() + 60000;
    revoke("PRESET_HALT", false); emit inputSampleRequested(); emit changed(); return true;
}
void ControlSession::advancePresets()
{
    if (!m_flow) return;
    if (now() >= m_flowDeadline) {
        if (!m_flowRequest.isEmpty() && running(m_records[m_flowRequest].toObject())) {
            auto record = m_records[m_flowRequest].toObject(); record["state"] = "result_unknown"; record["error"] = "LOCAL_OPERATION_TIMEOUT"; saveRecord(m_flowRequest,record);
        }
        stopPresetFlow("RESULT_UNKNOWN"); return;
    }
    if (!ready() || !m_status["runtime_fresh"].toBool() || m_emergency) return;
    if (!m_flowRequest.isEmpty()) {
        const auto op = m_records[m_flowRequest].toObject();
        if (running(op)) return;
        if (op["state"] != QJsonValue("succeeded")) { stopPresetFlow("PRESET_OPERATION_RECONCILE"); return; }
        // A terminal operation must be followed by a new status response.
        if (m_lastStatus <= m_flowStatusAfter) return;
        const auto requested = op["params"].toObject()["presets"].toArray();
        const auto action = op["command"].toString();
        const auto expected = action == "init" ? "Initialized" : action == "enable" ? "Armed" : action == "resume" ? "Running" : "Halted";
        for (const auto &id : requested) for (const auto &g : groupsFor(QJsonArray{id})) {
            const auto state = m_status["presets"].toObject()[id.toString()].toObject()["states"].toObject()[g.toString()].toString();
            if (state != expected && !(action == "halt" && state == "Stopped")) return;
        }
        m_flowRequest.clear();
    }
    if (busy() || !neutralReady()) return;
    QJsonArray removed;
    if (m_flowStart) for (const auto &id : m_selectedPresets) if (!m_flowTarget.contains(id)) removed.append(id);
    bool same = m_selectedPresets.size() == m_flowTarget.size();
    for (const auto &id : m_flowTarget) same &= m_selectedPresets.contains(id);
    if (m_flowStart && removed.isEmpty() && !same) {
        if (!selectPresets(m_flowTarget)) stopPresetFlow("PRESET_SELECTION_BLOCKED");
        return;
    }
    const auto targets = !removed.isEmpty() ? removed : m_flowTarget;
    QString action; QJsonArray subset;
    for (const auto &id : targets) {
        const auto preset = m_status["presets"].toObject()[id.toString()].toObject();
        QStringList states;
        for (const auto &g : groupsFor(QJsonArray{id})) states.append(preset["states"].toObject()[g.toString()].toString("Unknown"));
        QString next;
        if (!m_flowStart || !removed.isEmpty()) {
            bool stopped = true; for (const auto &state : states) stopped &= QStringList{"Stopped","Halted"}.contains(state);
            if (!stopped) next = "halt";
        } else {
            bool allRunning = true; for (const auto &state : states) allRunning &= state == "Running";
            if (allRunning) continue;
            // Mixed arm/gripper states require explicit operator reconciliation.
            for (const auto &state : states) if (state != states.first()) { stopPresetFlow("PARTIAL_PRESET_STATE"); return; }
            const auto state = states.first();
            if (state == "Stopped") next = "init";
            else if (state == "Initialized" || state == "Halted") next = "enable";
            else if (state == "Armed") next = "resume";
            else { stopPresetFlow(state == "Fault" ? "RECOVER_REQUIRED" : "RUNTIME_STATE_UNKNOWN"); return; }
        }
        if (!next.isEmpty() && (action.isEmpty() || action == next)) { action = next; subset.append(id); }
    }
    if (!action.isEmpty()) {
        if (!m_operationsAuthorized) { authorizeOperations(); return; }
        m_flowPhase = action; m_flowStatusAfter = m_lastStatus;
        if (!command(action, {{"presets",subset}})) { stopPresetFlow("PRESET_COMMAND_BLOCKED"); return; }
        m_flowRequest = m_operationOrder.last(); return;
    }
    if (m_flowStart && !same) {
        if (!selectPresets(m_flowTarget)) stopPresetFlow("PRESET_SELECTION_BLOCKED");
        return;
    }
    if (m_flowStart) {
        if (!m_permitted) return;
        if (!m_motionAuthorized) { m_flowPhase = "等待新的遥控授权"; authorizeMotion(); return; }
    }
    m_flow = false; m_flowPhase = m_flowStart ? "所选组 Running，已授权遥控" : "所选组已停止，遥控暂停"; emit changed();
}

bool ControlSession::persist()
{
    if (m_role == "monitor") return true;
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
bool ControlSession::command(const QString &command, QJsonObject params)
{
    if (m_presetAware) {
        if ((command == "set_control_mode" && params["mode"] == QJsonValue("arm")) || command.contains("arm_named_target")) {
            if (!m_selectedPresets.contains("arm")) { notify("PRESET_NOT_SELECTED", QStringLiteral("请先选择机械臂预设；遥控还需部署 enable_servo=true 并成功切换 arm 模式。")); return false; }
        }
        if (QStringList{"init","enable","disable","halt","resume","recover","shutdown"}.contains(command)) {
            const auto requested = params.contains("presets") ? params["presets"].toArray() : m_selectedPresets;
            if (params.size() > (params.contains("presets") ? 1 : 0) || !validPresets(requested)) { notify("PRESETS"); return false; }
            for (const auto &id : requested) if (!m_selectedPresets.contains(id)) { notify("PRESET_NOT_SELECTED"); return false; }
            params["presets"] = requested; // Persist the exact subset for same-ID recovery.
        }
        if (busy()) { notify("OPERATIONS_BUSY"); return false; }
    }
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
    if (m_presetAware) {
        record["preset_aware"] = true;
        record["presets"] = params.contains("presets") ? params["presets"] : QJsonValue(m_selectedPresets);
        record["groups"] = groupsFor(record["presets"].toArray());
    }
    m_records[id] = record; m_operationOrder.append(id);
    if (!persist()) { m_records.remove(id); m_operationOrder.removeOne(id); return false; }
    // Pause before the transport can deliver synchronous replies.
    revoke("OPERATION_REAUTHORIZATION", false);
    const auto sent = send("system_command", {{"command", command}, {"params", params}}, true, false, id);
    if (sent.isEmpty()) { m_records.remove(id); m_operationOrder.removeOne(id); persist(); return false; }
    emit event({{"type", "control_operation"}, {"operation", ControlProtocol::redact(record)}});
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
        if (!running(record)) { if (expectedId == m_flowRequest) m_flowStatusAfter = m_lastStatus; m_nextStatus = 0; emit stateRefreshRequested(); }
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
    if (m_flow && code != "EPOCH" && code != "STALE_TOKEN" && code != "NEUTRAL_REQUIRED") stopPresetFlow(code);
    if (code == "EPOCH") {
        revoke(code); m_nextStatus = 0; send("control_status"); return;
    }
    if (QStringList{"SESSION","LEASE","OWNER_BUSY","NEGOTIATION_TIMEOUT"}.contains(code)) { fail(QStringLiteral("控制会话失效：%1").arg(code)); return; }
    if (request.type == "control_hello" || QStringList{"UPGRADE_REQUIRED","IDENTITY_ROLE","CAPABILITIES","VERSION"}.contains(code)) {
        fail(QStringLiteral("控制v2不兼容或机器人身份不匹配：%1").arg(code), false); return;
    }
    if (QStringList{"STALE_TOKEN","REPLAY","SEQ","RUNTIME_PERMISSION","EMERGENCY_ACTIVE","NOT_AUTHORIZED","OPERATIONS_NOT_AUTHORIZED","OPERATIONS_BUSY","NEUTRAL_REQUIRED","HALT_REMOVED_PRESETS","PRESET_NOT_SELECTED","PRESETS"}.contains(code)) {
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
    // A readyRead callback can precede a delayed timer after an event-loop stall.
    // Renewed tokens/status must not conceal an interval of expired authority.
    if (negotiated()) expireValidity();
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
        valid = result["robot_id"] == QJsonValue(m_robot) && result["role"] == QJsonValue(m_role)
            && !result["session_id"].toString().isEmpty() && !result["epoch"].toString().isEmpty()
            && result["authorized"] == QJsonValue(false) && !result["build_version"].toString().isEmpty();
        for (const auto &cap : ControlProtocol::capabilities()) valid &= caps.contains(cap);
        if (m_presetAware) valid &= caps.contains("motion_presets_v1") && acceptSelection(result);
        if (valid) {
            m_session = result["session_id"].toString(); m_epoch = result["epoch"].toString(); m_build = result["build_version"].toString();
            valid = m_role == "monitor" || acceptToken(result, request); m_nextFresh = now() + 100; send("control_status");
        }
    } else if (request.type == "freshness_request") {
        valid = result["epoch"] == QJsonValue(m_epoch) && acceptToken(result, request);
    } else if (request.type == "control_status") {
        // An in-flight status from before select_presets cannot restore its old epoch.
        if (request.epoch != m_epoch && result["epoch"] == QJsonValue(request.epoch)) { m_nextStatus = 0; return; }
        valid = !result["epoch"].toString().isEmpty() && result["authorized"].isBool()
            && result["motion_permitted"].isBool() && result["emergency_active"].isBool()
            && result["counts"].isObject() && finiteNumber(result["queue_depth"], 0, 16);
        if (m_presetAware) valid &= result["runtime_fresh"].isBool() && result["operations_authorized"].isBool()
            && result["presets"].isObject() && acceptSelection(result);
        if (valid) {
            const auto epoch = result["epoch"].toString();
            if (epoch != m_epoch) {
                m_epoch = epoch; revoke("EPOCH"); m_nextFresh = 0;
                // Replies from the previous epoch remain correlated but cannot alter the new session.
            }
            const bool revoked = (!result["authorized"].toBool() && m_motionAuthorized)
                || (m_presetAware && !result["operations_authorized"].toBool() && m_operationsAuthorized)
                || (!(result["motion_permitted"].toBool() && (!m_presetAware || result["runtime_fresh"].toBool())) && m_permitted)
                || (result["emergency_active"].toBool() && !m_emergency);
            // Invalidate pending grants as well as grants already acknowledged.
            // Otherwise a denial followed by recovery can revive a delayed grant.
            if (revoked) revoke("SERVER_REVOKED");
            if (result["emergency_active"].toBool() && m_flow) stopPresetFlow("EMERGENCY_ACTIVE");
            m_status = result; m_lastStatus = now(); m_statusExpired = false;
            m_permitted = result["motion_permitted"].toBool() && (!m_presetAware || result["runtime_fresh"].toBool()); m_emergency = result["emergency_active"].toBool();
            emit event({{"type", "control_status"}, {"status", result}});
        }
    } else if (request.type == "select_presets") {
        valid = !result["epoch"].toString().isEmpty() && result["authorized"] == QJsonValue(false)
            && result["operations_authorized"] == QJsonValue(false) && acceptSelection(result);
        if (valid) { m_epoch = result["epoch"].toString(); m_permitted = false; revoke("PRESETS_CHANGED"); m_nextFresh = m_nextStatus = 0; }
    } else if (request.type == "operator_input") {
        valid = QStringList{"neutral","staged"}.contains(result["input_status"].toString());
        if (valid && request.authorizationGeneration == m_authorizationGeneration && request.physicalNeutral && m_physicalNeutral) m_neutralAckAt = now();
    } else if (request.type == "authorize" || request.type == "authorize_operations") {
        const auto field = request.type == "authorize" ? QStringLiteral("authorized") : QStringLiteral("operations_authorized");
        valid = result[field] == QJsonValue(true);
        if (valid && request.authorizationGeneration == m_authorizationGeneration && neutralReady()
            && (!m_presetAware || !m_desiredPresets.isEmpty() || (request.type == "authorize_operations" && m_flow && !m_flowStart)) && !m_emergency && (request.type != "authorize" || m_permitted)) {
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
    return {{"motion_presets_supported", m_presetAware}, {"pending_presets", m_desiredPresets}, {"selected_presets", m_selectedPresets}, {"selected_groups", m_selectedGroups}, {"preset_flow_active", m_flow}, {"preset_flow_phase", m_flowPhase}, {"role",m_role}, {"protocol_version", 2}, {"robot_id", m_robot}, {"build_version", m_build}, {"negotiated", negotiated()},
        {"ready", ready()}, {"epoch", m_epoch}, {"token_valid", tokenValid()}, {"motion_authorized", m_motionAuthorized},
        {"operations_authorized", m_operationsAuthorized}, {"journal_healthy", m_journalHealthy}, {"motion_permitted", ready() && m_permitted},
        {"emergency_active", m_emergency}, {"pending_requests", m_pending.size()}, {"last_rtt_ms", m_lastRtt},
        {"status_age_ms", m_lastStatus < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(now() - m_lastStatus)},
        {"input_age_ms", m_inputSampleAt < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(now() - m_inputSampleAt)},
        {"server", m_status}, {"operations", operations()}};
}
}
