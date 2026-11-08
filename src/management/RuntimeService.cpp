#include "RuntimeService.h"
#include <QHostAddress>
#include <QStringList>
#include <cmath>

namespace {
QString errorText(const QJsonObject &error)
{
    return QStringLiteral("%1: %2 — %3").arg(error["code"].toString(), error["message"].toString(), error["hint"].toString());
}
bool nonempty(const QJsonValue &v) { return v.isString() && !v.toString().isEmpty(); }
bool contains(const QJsonArray &a, const QString &s) { return a.contains(QJsonValue(s)); }
}
RuntimeService::RuntimeService(QObject *parent) : QObject(parent), m_client(this)
{
    m_poll.setSingleShot(true);
    m_reconnect.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, &RuntimeService::poll);
    connect(&m_reconnect, &QTimer::timeout, this, [this] {
        if (m_enabled) m_client.open(m_host, m_port, m_token);
    });
    connect(&m_client, &Communication::ManagementClient::connected, this, [this] {
        m_client.request("hello");
    });
    connect(&m_client, &Communication::ManagementClient::response, this, &RuntimeService::handleResponse);
    connect(&m_client, &Communication::ManagementClient::requestIssued, this,
            [this](const QString &id, const QString &method) {
        if (method == "start" || method == "stop") emit mutationIssued(id, method);
    });
    connect(&m_client, &Communication::ManagementClient::lost, this, [this](const QString &message, bool unknown) {
        invalidate();
        emit errorOccurred(message + (unknown ? QStringLiteral("；操作结果未知，重连后查询，不自动重发") : QString()));
        if (m_enabled) {
            emit statusChanged(QStringLiteral("管理离线，等待重连；显示数据已过期"));
            m_reconnect.start(3000);
        }
    });
    connect(&m_client, &Communication::ManagementClient::rejected, this,
            [this](const QString &method, const QJsonObject &error) {
        const auto code = error["code"].toString();
        if (code == "AUTH" || code == "VERSION" || method == "hello" || method == "profiles") {
            close(); // User must correct credentials/version; no authentication retry storm.
            emit errorOccurred(errorText(error));
            return;
        }
        if (!(method == "operation" && code == "NOT_FOUND")) emit errorOccurred(errorText(error));
        if (method == "operation") {
            if (code == "NOT_FOUND") {
                m_operationId.clear();
                m_running = false;
                m_reconciled = true;
                m_operationSucceeded = m_snapshot["operation_id"].toString().isEmpty();
                emit operationIdChanged({});
            }
            finishCycle();
        } else if (method == "logs") finishCycle();
        else { m_poll.start(1000); emit availabilityChanged(); }
    });
}
void RuntimeService::invalidate()
{
    m_online = false;
    m_reconciled = false;
    m_snapshot = {};
    m_endpoint.clear();
    m_candidateHost.clear();
    m_poll.stop();
    emit stateInvalidated();
    emit controlUnavailable();
    emit availabilityChanged();
}
void RuntimeService::open(const QString &host, quint16 port, const QString &token)
{
    close();
    m_host = host.trimmed(); m_port = port; m_token = token;
    if (m_host.isEmpty() || port == 0 || token.size() < 32) {
        emit errorOccurred(QStringLiteral("请填写地址、端口和至少32字符的令牌")); return;
    }
    m_enabled = true;
    m_operationId.clear(); m_cursor = 0; m_running = false;
    emit statusChanged(QStringLiteral("正在连接管理服务"));
    m_client.open(m_host, m_port, m_token);
}
void RuntimeService::close()
{
    m_enabled = false;
    m_reconnect.stop();
    m_client.close();
    m_token.clear();
    invalidate();
    emit statusChanged(QStringLiteral("管理未连接；数据未知/已过期"));
}
bool RuntimeService::canStart() const
{
    const auto state = m_snapshot["software_state"].toString();
    return canQuery() && !m_configurationBlocked && !m_refreshProfiles && m_reconciled && !m_running && !m_snapshot["recovery_required"].toBool(true)
        && (state == "stopped" || state == "failed");
}
bool RuntimeService::canStop() const { return canQuery() && m_reconciled && !m_running; }
bool RuntimeService::start(const QString &profile, const QString &revision)
{
    if (!canStart() || profile.isEmpty() || revision.isEmpty()) return false;
    // The server performs preflight within start. Do not duplicate cold-only preflight on running systems.
    m_poll.stop();
    m_reconciled = false;
    m_endpoint.clear();
    emit controlUnavailable();
    const bool sent = m_client.request("start", {{"robot_id", m_robotId}, {"profile_id", profile}, {"config_revision", revision}});
    emit availabilityChanged();
    return sent;
}
bool RuntimeService::stop()
{
    if (!canStop()) return false;
    m_poll.stop();
    m_reconciled = false;
    m_endpoint.clear();
    emit controlUnavailable();
    const bool sent = m_client.request("stop");
    emit availabilityChanged();
    return sent;
}
bool RuntimeService::preflight(const QString &profile, const QString &revision)
{
    if (!canStart()) return false;
    m_poll.stop();
    const bool sent = m_client.request("preflight", {{"robot_id", m_robotId}, {"profile_id", profile}, {"config_revision", revision}});
    emit availabilityChanged();
    return sent;
}
void RuntimeService::invalidResponse()
{
    close();
    emit errorOccurred(QStringLiteral("管理响应字段无效或能力不足，请核对文档39及匹配版本"));
}
bool RuntimeService::acceptOperation(const QJsonObject &op)
{
    const auto state = op["state"].toString();
    if (!nonempty(op["operation_id"]) || !nonempty(op["phase"])
        || !QStringList{"accepted", "running", "succeeded", "failed", "cancelled"}.contains(state)
        || !op["modules"].isObject()) return false;
    const auto id = op["operation_id"].toString();
    if (id != m_operationId) { m_operationId = id; m_cursor = 0; }
    m_running = state == "accepted" || state == "running";
    m_reconciled = true;
    m_operationSucceeded = state == "succeeded";
    emit operationIdChanged(id);
    emit operationReceived(op);
    return true;
}
void RuntimeService::handleResponse(const QString &method, const QJsonValue &result)
{
    const auto obj = result.toObject();
    if (method == "hello") {
        const auto caps = obj["capabilities"].toArray();
        if (obj["protocol_version"] != QJsonValue(1) || !nonempty(obj["robot_id"]) || obj["auth"] != QJsonValue("shared_token")) {
            invalidResponse(); return;
        }
        for (const auto &cap : {"profiles", "preflight", "start", "stop", "operation", "snapshot", "logs"}) {
            if (!contains(caps, cap)) { invalidResponse(); return; }
        }
        m_robotId = obj["robot_id"].toString();
        m_client.request("profiles");
    } else if (method == "profiles") {
        if (!result.isArray() || result.toArray().isEmpty()) { invalidResponse(); return; }
        for (const auto &v : result.toArray()) {
            const auto profile = v.toObject();
            if (!nonempty(profile["profile_id"]) || !nonempty(profile["config_revision"])
                || !profile["required_modules"].isArray()) { invalidResponse(); return; }
        }
        m_profiles = result.toArray();
        m_refreshProfiles = false;
        emit profilesReceived(m_profiles);
        m_client.request("snapshot"); // Reconcile before any new mutation, including after reconnect.
    } else if (method == "snapshot") {
        const auto state = obj["software_state"].toString();
        if (!QStringList{"stopped", "starting", "ready", "degraded", "stopping", "failed"}.contains(state)
            || !obj["recovery_required"].isBool() || !obj["modules"].isObject()) { invalidResponse(); return; }
        m_snapshot = obj;
        m_online = true;
        const auto id = obj["operation_id"].toString();
        if (id != m_operationId) { m_operationId = id; m_cursor = 0; emit operationIdChanged(id); }
        emit snapshotReceived(obj);
        emit statusChanged(QStringLiteral("管理已连接：%1").arg(state));
        // Require profile revision, required modules, and a compatible endpoint, independently of safety.
        bool healthy = false;
        for (const auto &v : m_profiles) {
            const auto p = v.toObject();
            if (p["profile_id"] == obj["profile_id"] && p["config_revision"] == obj["config_revision"]) {
                healthy = true;
                for (const auto &module : p["required_modules"].toArray()) {
                    if (!module.isString() || obj["modules"].toObject()[module.toString()].toObject()["ok"] != QJsonValue(true)) healthy = false;
                }
            }
        }
        const auto endpoint = obj["control_endpoint"].toObject();
        QString host = endpoint["host"].toString();
        const int port = endpoint["port"].toInt();
        QHostAddress address(host);
        // Manager examples advertise localhost: resolve it relative to the robot, not this PC.
        if (address.isLoopback() || host.compare("localhost", Qt::CaseInsensitive) == 0) host = m_host;
        if ((state == "ready" || state == "degraded") && !obj["recovery_required"].toBool()
            && healthy && !host.isEmpty() && port > 0 && port <= 65535
            && endpoint["port"].toDouble() == port
            && (endpoint["protocol_version"] == QJsonValue(1) || endpoint["protocol_version"] == QJsonValue(2))) {
            m_candidateHost = host;
            m_candidatePort = static_cast<quint16>(port);
            m_candidateProtocol = endpoint["protocol_version"].toInt();
        } else {
            m_candidateHost.clear();
        }
        queryOperation();
    } else if (method == "start" || method == "stop" || method == "operation") {
        if (!acceptOperation(obj)) { invalidResponse(); return; }
        if (method == "operation") queryLogs();
        else m_client.request("snapshot");
    } else if (method == "logs") {
        const auto next = obj["next_cursor"].toDouble(-1);
        if (!obj["lines"].isArray() || !obj["expired"].isBool() || next < m_cursor
            || next > 2147483647 || std::floor(next) != next) { invalidResponse(); return; }
        QStringList lines;
        for (const auto &v : obj["lines"].toArray()) {
            if (!v.isString()) { invalidResponse(); return; }
            lines.append(v.toString());
        }
        m_cursor = static_cast<int>(next);
        emit logsReceived(lines, obj["expired"].toBool());
        finishCycle();
    } else if (method == "preflight") {
        if (!obj["passed"].isBool() || !obj["checks"].isArray()) { invalidResponse(); return; }
        emit preflightReceived(obj);
        finishCycle();
    }
}
void RuntimeService::poll()
{
    if (m_online && m_client.idle()) m_client.request(m_refreshProfiles ? "profiles" : "snapshot");
    emit availabilityChanged();
}
void RuntimeService::queryOperation()
{
    // Query the server's current operation when no local ID survived (e.g. a lost start response).
    m_client.request("operation", m_operationId.isEmpty() ? QJsonObject{} : QJsonObject{{"operation_id", m_operationId}});
}
void RuntimeService::queryLogs()
{
    if (m_operationId.isEmpty()) { finishCycle(); return; }
    m_client.request("logs", {{"operation_id", m_operationId}, {"cursor", m_cursor}, {"limit", 100}});
}
void RuntimeService::finishCycle()
{
    if (m_online && m_reconciled && m_operationSucceeded && !m_running && !m_candidateHost.isEmpty()) {
        const auto key = m_candidateHost + ':' + QString::number(m_candidatePort) + '/' + QString::number(m_candidateProtocol) + '/' + m_operationId;
        if (key != m_endpoint) {
            m_endpoint = key;
            emit controlEndpointReady(m_candidateHost, m_candidatePort, m_candidateProtocol, m_robotId);
        }
    } else if (!m_endpoint.isEmpty()) {
        m_endpoint.clear();
        emit controlUnavailable();
    }
    if (m_enabled && m_online) m_poll.start(1000);
    emit availabilityChanged();
}
