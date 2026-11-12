#pragma once
#include "ControlProtocol.h"
#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <functional>

namespace Communication {
// V2 policy/session layer. Owns no socket and retains no continuous input snapshot.
class ControlSession : public QObject {
    Q_OBJECT
public:
    explicit ControlSession(QObject *parent = nullptr, std::function<qint64()> clock = {});
    void begin(const QString &robotId, const QString &endpoint, bool motionPresets = false, const QString &role = QStringLiteral("control"));
    void reset();
    bool setPendingPresets(const QJsonArray &presets);
    bool startPresets(const QJsonArray &presets);
    bool selectPresets(const QJsonArray &presets);
    bool haltPresets(const QJsonArray &presets);
    static bool validPresets(const QJsonArray &presets);
    static QJsonArray groupsFor(const QJsonArray &presets);
    void receive(const QJsonObject &message);
    void poll();
    bool input(const OperatorInputState &state);
    bool authorizeMotion();
    bool authorizeOperations();
    bool clearEmergency();
    void emergencyIntent();
    void pause();
    bool command(const QString &command, QJsonObject params);
    bool recoverRequest(const QString &requestId);
    bool ready() const;
    bool motionAuthorized() const { return m_motionAuthorized; }
    bool operationsAuthorized() const { return m_operationsAuthorized; }
    bool negotiated() const { return !m_session.isEmpty(); }
    QJsonObject diagnostics() const;
    QJsonArray operations() const;
    QString robotId() const { return m_robot; }
signals:
    void outbound(const QJsonObject &message);
    void fault(const QString &message, bool retry);
    void event(const QJsonObject &message);
    void changed();
    void inputSampleRequested();
    void inputResetRequested();
    void stateRefreshRequested();
    void motionAuthorizationChanged(bool enabled);
    void operationsAuthorizationChanged(bool enabled);
    void businessResult(const QJsonObject &message);
private:
    struct Pending {
        QString type, epoch, operationId;
        quint64 seq = 0;
        quint64 authorizationGeneration = 0;
        qint64 sentAt = 0, deadline = 0;
        bool physicalNeutral = false;
    };
    void advancePresets();
    void stopPresetFlow(const QString &reason);
    bool busy() const;
    bool acceptSelection(const QJsonObject &result);
    qint64 now() const;
    bool tokenValid() const;
    bool neutralReady() const;
    bool pendingType(const QString &type) const;
    QString send(const QString &type, QJsonObject fields = {}, bool fresh = false,
                 bool neutral = false, const QString &requestId = {});
    void revoke(const QString &reason, bool clearToken = true);
    void expireValidity();
    void fail(const QString &message, bool retry = true);
    bool acceptToken(const QJsonObject &value, const Pending &request);
    void rejected(const Pending &request, const QString &id, const QString &code);
    void expireRequest(const QString &id, const Pending &request);
    bool acceptOperation(const QJsonObject &op, const QString &expectedId);
    bool persist();
    void restore();
    void saveRecord(const QString &id, const QJsonObject &record);
    void queryNextOperation();
    void notify(const QString &code, const QString &message = {});
    bool m_presetAware = false, m_flow = false, m_flowStart = false;
    QJsonArray m_desiredPresets{QStringLiteral("flippers"), QStringLiteral("base"), QStringLiteral("arm")};
    QJsonArray m_selectedPresets, m_selectedGroups, m_flowTarget;
    QString m_flowRequest, m_flowPhase;
    qint64 m_flowDeadline = 0, m_flowStatusAfter = -1;
    QElapsedTimer m_clock;
    std::function<qint64()> m_now;
    QTimer m_timer;
    QString m_role = QStringLiteral("control");
    QString m_robot, m_scope, m_session, m_epoch, m_token, m_build;
    quint64 m_sequence = 0;
    quint64 m_authorizationGeneration = 0;
    bool m_journalHealthy = true;
    qint64 m_started = -1, m_tokenUntil = -1, m_lastFresh = -1, m_lastStatus = -1;
    qint64 m_nextFresh = 0, m_nextStatus = 0, m_nextOperation = 0, m_inputSampleAt = -1, m_neutralAckAt = -1;
    bool m_active = false, m_motionAuthorized = false, m_operationsAuthorized = false;
    bool m_physicalNeutral = false, m_permitted = false, m_emergency = true;
    bool m_tokenExpired = false, m_statusExpired = false;
    QHash<QString, Pending> m_pending;
    QJsonObject m_records, m_status;
    QStringList m_operationOrder;
    int m_operationCursor = 0;
    qint64 m_lastRtt = -1;
};
}
