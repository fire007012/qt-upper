#pragma once
#include "ManagementClient.h"
#include <QJsonArray>

// Management policy transactions only; never writes ROS/device configuration.
class ConfigurationService : public QObject {
    Q_OBJECT
public:
    explicit ConfigurationService(QObject *parent = nullptr);
    void open(const QString &host, quint16 port, const QString &token);
    void close();
    void selectProfile(const QString &id);
    bool refresh();
    bool saveCandidate(const QJsonArray &patches);
    bool preview(const QString &revision);
    bool apply();
    bool previewRollback(const QString &revision);
    bool rollback();
    bool recoverRequest();
    bool audit(bool reset = false);
    void setMaintenanceReady(bool ready);
    bool canQuery() const;
    bool canEdit() const;
    bool canApply() const;
    bool canRollback() const;
    bool needsRecovery() const { return !m_pending.isEmpty(); }
    bool blocksStart() const;
    QJsonObject schema() const { return m_schema; }
    QJsonObject status() const { return m_status; }
    QJsonObject source() const { return m_source; }
    QJsonObject candidate() const { return m_candidate; }
    QJsonObject operation() const { return m_operation; }
    QJsonObject rollbackPreview() const { return m_rollbackPreview; }
    QString profileId() const { return m_profile; }
    QString sourceRevision() const { return m_status["source_revision"].toString(); }
signals:
    void changed();
    void sourceReceived(const QJsonObject &source);
    void profilesReceived(const QJsonArray &profiles);
    void auditReceived(const QJsonArray &items, bool reset);
    void message(const QString &text);
    void sourceRevisionChanged();
private:
    void response(const QString &method, const QJsonValue &result);
    void invalid();
    void invalidate();
    void beginRefresh();
    void afterStatus();
    void finish();
    bool send(const QString &method, const QJsonObject &params = {});
    bool mutate(const QString &method, const QJsonObject &params);
    bool persist();
    void clearPending();
    Communication::ManagementClient m_client;
    QTimer m_poll, m_reconnect;
    QString m_host, m_token, m_scope, m_profile, m_operationId, m_sourceQueryRevision;
    quint16 m_port = 0;
    bool m_enabled = false, m_supported = false, m_synced = false, m_maintenance = false;
    bool m_forceSource = true, m_refreshRequested = false, m_auditReset = false;
    int m_auditCursor = 0;
    QJsonObject m_schema, m_status, m_source, m_candidate, m_operation, m_pending, m_rollbackPreview;
};
