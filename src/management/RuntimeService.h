#pragma once
#include "ManagementClient.h"
#include <QJsonArray>

// Independent of the control socket: ready here never grants motion permission.
class RuntimeService : public QObject {
    Q_OBJECT
public:
    explicit RuntimeService(QObject *parent = nullptr);
    void open(const QString &host, quint16 port, const QString &token);
    void close();
    bool start(const QString &profile, const QString &revision, const QJsonArray &presets = {});
    bool stop();
    bool preflight(const QString &profile, const QString &revision, const QJsonArray &presets = {});
    bool canStart() const;
    bool canStop() const;
    void refreshProfiles() { m_refreshProfiles = true; }
    void setConfigurationBlocked(bool blocked) { if (m_configurationBlocked != blocked) { m_configurationBlocked = blocked; emit availabilityChanged(); } }
    bool canQuery() const { return m_online && m_client.idle(); }
    QString robotId() const { return m_robotId; }
    QJsonObject snapshot() const { return m_snapshot; }
signals:
    void statusChanged(const QString &text);
    void availabilityChanged();
    void profilesReceived(const QJsonArray &profiles);
    void snapshotReceived(const QJsonObject &snapshot);
    void operationReceived(const QJsonObject &operation);
    void preflightReceived(const QJsonObject &result);
    void logsReceived(const QStringList &lines, bool expired);
    void errorOccurred(const QString &message);
    void mutationIssued(const QString &id, const QString &method);
    void operationIdChanged(const QString &id);
    void controlEndpointReady(const QString &host, quint16 port, int protocolVersion, const QString &robotId);
    void controlUnavailable();
    void stateInvalidated();
private:
    void handleResponse(const QString &method, const QJsonValue &result);
    void poll();
    void queryOperation();
    void queryLogs();
    void finishCycle();
    void invalidate();
    bool acceptOperation(const QJsonObject &operation);
    void invalidResponse();
    Communication::ManagementClient m_client;
    QTimer m_poll, m_reconnect;
    QString m_host, m_token, m_robotId, m_operationId, m_endpoint;
    quint16 m_port = 9091, m_candidatePort = 0;
    int m_candidateProtocol = 0;
    QString m_candidateHost;
    bool m_operationSucceeded = false;
    QJsonObject m_snapshot;
    QJsonArray m_profiles;
    int m_cursor = 0;
    bool m_enabled = false, m_online = false, m_running = false, m_reconciled = false;
    bool m_refreshProfiles = false, m_configurationBlocked = false;
};
