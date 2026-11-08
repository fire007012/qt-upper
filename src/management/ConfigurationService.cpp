#include "ConfigurationService.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QUuid>
#include <cmath>

namespace {
bool revision(const QJsonValue &value) {
    static const QRegularExpression pattern("^[0-9a-f]{64}$");
    return value.isString() && pattern.match(value.toString()).hasMatch();
}
bool active(const QJsonObject &op) {
    return op["state"] == QJsonValue("accepted") || op["state"] == QJsonValue("running");
}
bool mutation(const QString &method) {
    return method == "config.candidate" || method == "config.apply" || method == "config.rollback";
}
}
ConfigurationService::ConfigurationService(QObject *parent) : QObject(parent), m_client(this)
{
    m_poll.setSingleShot(true); m_reconnect.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (m_refreshRequested) beginRefresh();
        else if (canQuery()) send("config.status");
    });
    connect(&m_reconnect, &QTimer::timeout, this, [this] {
        if (m_enabled) m_client.open(m_host, m_port, m_token);
    });
    connect(&m_client, &Communication::ManagementClient::connected, this, [this] { send("hello"); });
    connect(&m_client, &Communication::ManagementClient::response, this, &ConfigurationService::response);
    connect(&m_client, &Communication::ManagementClient::lost, this, [this](const QString &text, bool unknown) {
        invalidate();
        emit message(text + (unknown || !m_pending.isEmpty()
            ? QStringLiteral("；配置请求结果未知。重连查询后可用原请求ID恢复，禁止创建新请求重试。") : QString()));
        if (m_enabled) m_reconnect.start(3000);
    });
    connect(&m_client, &Communication::ManagementClient::rejected, this,
            [this](const QString &method, const QJsonObject &error) {
        const auto code = error["code"].toString();
        emit message(QStringLiteral("%1: %2；%3").arg(code, error["message"].toString(), error["hint"].toString()));
        if (code == "AUTH" || code == "VERSION" || code == "CAPABILITY_DISABLED" || method == "hello") {
            close(); return;
        }
        // An oversized response can follow a committed write. Preserve its exact ID/params.
        if (mutation(method) && code != "RESPONSE_TOO_LARGE") clearPending();
        if (code == "REVISION_CONFLICT") {
            m_candidate = {}; m_rollbackPreview = {};
            emit message(QStringLiteral("源版本已变化，已清除确认；重新加载后再次核对差异。"));
            refresh(); return;
        }
        if (method == "config.schema" || method == "profiles" || method == "config.status" || method == "config.source") {
            m_synced = false;
            m_refreshRequested = true;
        }
        finish();
    });
}
void ConfigurationService::invalidate()
{
    m_poll.stop(); m_supported = false; m_synced = false; m_maintenance = false;
    m_schema = {}; m_status = {}; m_source = {}; m_candidate = {}; m_rollbackPreview = {};
    m_sourceQueryRevision.clear();
    m_forceSource = true;
    emit changed();
}
void ConfigurationService::open(const QString &host, quint16 port, const QString &token)
{
    close();
    m_host = host.trimmed(); m_port = port; m_token = token;
    m_scope.clear(); m_pending = {}; m_operation = {}; m_operationId.clear(); m_auditCursor = 0;
    if (m_host.isEmpty() || !port || token.size() < 32) {
        m_token.clear();
        emit message(QStringLiteral("请填写配置管理地址、端口和至少32字符的部署令牌。"));
        emit changed(); return;
    }
    m_enabled = true;
    m_client.open(m_host, port, token);
}
void ConfigurationService::close()
{
    m_enabled = false; m_reconnect.stop(); m_client.close(); m_token.clear(); invalidate();
}
bool ConfigurationService::canQuery() const { return m_supported && m_client.idle(); }
bool ConfigurationService::canEdit() const {
    return canQuery() && m_synced && m_pending.isEmpty() && !active(m_operation)
        && !m_status["applying"].toBool(true) && !m_source.isEmpty()
        && m_source["profile"].toObject()["profile_id"] == QJsonValue(m_profile);
}
bool ConfigurationService::blocksStart() const {
    return !m_pending.isEmpty() || active(m_operation) || m_operation["state"] == QJsonValue("result_unknown")
        || m_status["applying"].toBool(false);
}
bool ConfigurationService::canApply() const {
    return canEdit() && m_maintenance && !m_candidate.isEmpty()
        && m_candidate["base_revision"] == m_status["source_revision"];
}
bool ConfigurationService::canRollback() const {
    return canEdit() && m_maintenance && !m_rollbackPreview.isEmpty()
        && m_rollbackPreview["base_revision"] == m_status["source_revision"];
}
void ConfigurationService::setMaintenanceReady(bool ready)
{
    if (ready != m_maintenance) { m_maintenance = ready; emit changed(); }
}
bool ConfigurationService::send(const QString &method, const QJsonObject &params)
{
    m_poll.stop();
    const bool sent = m_client.request(method, params);
    emit changed();
    return sent;
}
bool ConfigurationService::persist()
{
    if (m_scope.isEmpty()) return false;
    QSettings settings("HostComputer", "Configuration");
    settings.beginGroup(m_scope);
    settings.setValue("pending", QJsonDocument(m_pending).toJson(QJsonDocument::Compact));
    settings.setValue("operation_id", m_operationId);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        emit message(QStringLiteral("无法保存配置恢复记录，请检查本机设置目录权限和磁盘空间。"));
        return false;
    }
    return true;
}
void ConfigurationService::clearPending() { m_pending = {}; persist(); }
bool ConfigurationService::mutate(const QString &method, const QJsonObject &params)
{
    if (!canEdit() || QJsonDocument(params).toJson(QJsonDocument::Compact).size() > 49152) return false;
    const auto id = QStringLiteral("config-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_pending = {{"request_id", id}, {"method", method}, {"params", params}};
    // Durable intent before bytes; no token is stored.
    if (!persist()) { m_pending = {}; emit changed(); return false; }
    m_poll.stop();
    const bool sent = m_client.request(method, params, id);
    if (!sent && m_client.idle()) clearPending();
    emit changed(); return sent;
}
bool ConfigurationService::recoverRequest()
{
    if (!canQuery() || !m_synced || m_pending.isEmpty()) return false;
    m_poll.stop();
    const bool sent = m_client.request(m_pending["method"].toString(), m_pending["params"].toObject(), m_pending["request_id"].toString());
    emit changed(); return sent;
}
void ConfigurationService::selectProfile(const QString &id)
{
    if (id == m_profile) return;
    m_profile = id; m_source = {}; m_candidate = {}; m_rollbackPreview = {};
    m_forceSource = true; m_refreshRequested = true;
    if (canQuery()) beginRefresh();
    emit changed();
}
bool ConfigurationService::refresh()
{
    m_forceSource = true; m_refreshRequested = true; m_candidate = {}; m_rollbackPreview = {};
    if (canQuery()) beginRefresh();
    emit changed(); return m_supported;
}
void ConfigurationService::beginRefresh()
{
    m_refreshRequested = false; m_synced = false;
    send("profiles");
}
bool ConfigurationService::saveCandidate(const QJsonArray &patches)
{
    if (patches.isEmpty() || patches.size() > qMin(64, m_schema["max_patches"].toInt(64))) return false;
    const auto fields = m_schema["fields"].toObject();
    QStringList seen;
    for (const auto &item : patches) {
        const auto patch = item.toObject();
        const auto field = patch["field"].toString();
        const auto spec = fields[field].toObject();
        const auto value = patch["value"];
        if (patch.size() != 3 || patch["profile_id"] != QJsonValue(m_profile) || seen.contains(field)
            || (field != "startup_timeout" && field != "video_required") || spec["editable"] != QJsonValue(true)) return false;
        if (spec["type"] == QJsonValue("number")) {
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < spec["minimum"].toDouble()
                || value.toDouble() > spec["maximum"].toDouble()) return false;
        } else if (spec["type"] == QJsonValue("boolean")) {
            if (!value.isBool() || (value.toBool() && !m_source["profile"].toObject()["video"].toBool())) return false;
        } else return false;
        seen.append(field);
    }
    m_candidate = {};
    return mutate("config.candidate", {{"base_revision", sourceRevision()}, {"patches", patches}});
}
bool ConfigurationService::preview(const QString &value)
{
    if (!canEdit() || !revision(value)) return false;
    m_candidate = {};
    return send("config.preview", {{"candidate_revision", value}});
}
bool ConfigurationService::apply()
{
    if (!canApply()) return false;
    return mutate("config.apply", {{"base_revision", sourceRevision()}, {"candidate_revision", m_candidate["candidate_revision"]}});
}
bool ConfigurationService::previewRollback(const QString &value)
{
    if (!canEdit() || !revision(value) || value == sourceRevision()) return false;
    m_sourceQueryRevision = value; m_rollbackPreview = {};
    return send("config.source", {{"profile_id", m_profile}, {"revision", value}});
}
bool ConfigurationService::rollback()
{
    if (!canRollback()) return false;
    return mutate("config.rollback", {{"base_revision", sourceRevision()}, {"target_revision", m_rollbackPreview["revision"]}});
}
bool ConfigurationService::audit(bool reset)
{
    if (!canQuery() || !m_synced) return false;
    if (reset) m_auditCursor = 0;
    m_auditReset = reset;
    return send("config.audit", {{"cursor", m_auditCursor}, {"limit", 50}});
}
void ConfigurationService::invalid()
{
    close(); emit message(QStringLiteral("配置响应字段无效，已停止配置连接；请核对文档41与服务端版本。"));
}
void ConfigurationService::response(const QString &method, const QJsonValue &result)
{
    const auto obj = result.toObject();
    if (method == "hello") {
        if (obj["protocol_version"] != QJsonValue(1) || obj["auth"] != QJsonValue("shared_token") || obj["robot_id"].toString().isEmpty()) { invalid(); return; }
        const auto caps = obj["capabilities"].toArray();
        for (const auto &cap : {"config.schema", "config.status", "config.source", "config.candidate", "config.preview", "config.apply", "config.rollback", "config.operation", "config.audit"}) {
            if (!caps.contains(QJsonValue(cap))) {
                close(); emit message(QStringLiteral("此管理服务未启用配置事务，运行管理仍可使用。")); return;
            }
        }
        m_scope = QString::fromLatin1(QCryptographicHash::hash((m_host + ':' + QString::number(m_port) + '/' + obj["robot_id"].toString()).toUtf8(), QCryptographicHash::Sha256).toHex());
        QSettings settings("HostComputer", "Configuration"); settings.beginGroup(m_scope);
        m_pending = QJsonDocument::fromJson(settings.value("pending").toByteArray()).object();
        m_operationId = settings.value("operation_id").toString();
        m_supported = true;
        send("config.schema"); return;
    }
    if (method == "config.schema") {
        const auto fields = obj["fields"].toObject();
        const auto timeout = fields["startup_timeout"].toObject();
        if (obj["schema_version"] != QJsonValue(1) || obj["capability"] != QJsonValue("config_transactions_v1")
            || obj["scope"] != QJsonValue("manager_profiles") || !obj["readonly_fields"].isArray()
            || timeout["type"] != QJsonValue("number") || !timeout["minimum"].isDouble() || !timeout["maximum"].isDouble()
            || timeout["minimum"].toDouble() > timeout["maximum"].toDouble()
            || fields["video_required"].toObject()["type"] != QJsonValue("boolean")) { invalid(); return; }
        m_schema = obj; beginRefresh(); return;
    }
    if (method == "profiles") {
        if (!result.isArray() || result.toArray().isEmpty()) { invalid(); return; }
        bool found = false;
        for (const auto &v : result.toArray()) {
            if (v.toObject()["profile_id"].toString().isEmpty() || v.toObject()["config_revision"].toString().isEmpty()) { invalid(); return; }
            found |= v.toObject()["profile_id"] == QJsonValue(m_profile);
        }
        if (!found) m_profile = result.toArray().first().toObject()["profile_id"].toString();
        emit profilesReceived(result.toArray());
        send("config.status"); return;
    }
    if (method == "config.status") {
        if (!revision(obj["source_revision"]) || !revision(obj["effective_management_revision"])
            || (!obj["actual_runtime_revision"].isNull() && !revision(obj["actual_runtime_revision"]))
            || !obj["candidate_revisions"].isArray() || !obj["pending_restart"].isBool() || !obj["applying"].isBool()
            || (!obj["pending_restart"].toBool() && (obj["actual_runtime_revision"] != obj["source_revision"] || !obj["runtime_observed_at"].isDouble()))
            || obj["device_nvm_written"] != QJsonValue(false)) { invalid(); return; }
        const bool changedRevision = !m_status.isEmpty() && m_status["source_revision"] != obj["source_revision"];
        if (changedRevision) { m_candidate = {}; m_rollbackPreview = {}; m_forceSource = true; emit sourceRevisionChanged(); }
        m_status = obj;
        afterStatus(); return;
    }
    if (method == "config.source") {
        if (!revision(obj["revision"]) || !obj["profile"].isObject() || obj["profile"].toObject()["profile_id"].toString().isEmpty()) { invalid(); return; }
        if (!m_sourceQueryRevision.isEmpty()) {
            if (obj["revision"] != QJsonValue(m_sourceQueryRevision)) { invalid(); return; }
            m_sourceQueryRevision.clear(); m_rollbackPreview = obj;
            m_rollbackPreview["base_revision"] = sourceRevision(); finish(); return;
        }
        if (obj["revision"] != m_status["source_revision"] || obj["profile"].toObject()["profile_id"] != QJsonValue(m_profile)) {
            m_forceSource = true; m_refreshRequested = true; finish(); return;
        }
        m_source = obj; m_forceSource = false; emit sourceReceived(obj);
        emit sourceRevisionChanged(); // Also refresh startup profiles after a lost activation response/reconnect.
        if (!m_operationId.isEmpty()) { send("config.operation", {{"operation_id", m_operationId}}); return; }
        m_synced = true; finish(); return;
    }
    if (method == "config.candidate" || method == "config.preview") {
        if (!revision(obj["candidate_revision"]) || !revision(obj["base_revision"]) || !obj["diff"].isArray()
            || !obj["affected_groups"].isArray() || !obj["maintenance_required"].isBool()
            || !obj["pending_restart"].isBool() || obj["device_nvm_written"] != QJsonValue(false)) { invalid(); return; }
        m_candidate = obj;
        if (method == "config.candidate") clearPending();
        emit message(QStringLiteral("候选已保存/加载，尚未应用；请核对差异与影响范围。"));
    } else if (method == "config.apply" || method == "config.rollback" || method == "config.operation") {
        const auto state = obj["state"].toString();
        if (obj["operation_id"].toString().isEmpty() || !QStringList{"accepted", "running", "succeeded", "failed", "result_unknown"}.contains(state)
            || !QStringList{"validate", "activate", "complete", "rolled_back", "reconcile"}.contains(obj["phase"].toString())
            || !revision(obj["base_revision"]) || !revision(obj["target_revision"])
            || (obj["method"] != QJsonValue("config.apply") && obj["method"] != QJsonValue("config.rollback"))
            || !obj["pending_restart"].isBool()
            || (state == "succeeded" && obj["verification"] != QJsonValue("manager_source_readback"))) { invalid(); return; }
        if (method == "config.operation" && obj["operation_id"] != QJsonValue(m_operationId)) { invalid(); return; }
        const bool newUnknown = state == "result_unknown" && obj != m_operation;
        m_operation = obj; m_operationId = obj["operation_id"].toString();
        if (method != "config.operation") { clearPending(); m_synced = false; send("config.status"); return; }
        persist(); m_synced = true;
        if (newUnknown) emit message(QStringLiteral("配置操作结果未知；以当前源与状态为准，先确认停止，再显式回滚或重新保存候选。"));
    } else if (method == "config.audit") {
        const auto next = obj["next_cursor"].toDouble(-1);
        const auto items = obj["items"].toArray();
        if (!obj["items"].isArray() || items.size() > 50 || next != m_auditCursor + items.size() || next > 2147483647) { invalid(); return; }
        m_auditCursor = static_cast<int>(next); emit auditReceived(items, m_auditReset);
    } else { invalid(); return; }
    finish();
}
void ConfigurationService::afterStatus()
{
    if (m_forceSource || m_source["revision"] != m_status["source_revision"]) {
        m_sourceQueryRevision.clear();
        send("config.source", {{"profile_id", m_profile}, {"revision", sourceRevision()}});
    } else if (!m_operationId.isEmpty()) send("config.operation", {{"operation_id", m_operationId}});
    else { m_synced = true; finish(); }
}
void ConfigurationService::finish()
{
    if (m_enabled && m_supported) m_poll.start(1000);
    emit changed();
}
