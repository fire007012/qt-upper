#pragma once
#include <QWidget>
#include "RuntimeService.h"
#include "RuntimeJournal.h"
class QCheckBox;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QPushButton;
class QLabel;
class QDialog;
class ConfigurationWidget;

class RuntimeManagementWidget : public QWidget {
    Q_OBJECT
public:
    explicit RuntimeManagementWidget(QWidget *parent = nullptr);
    ~RuntimeManagementWidget() override;
    QJsonArray pendingPresets() const;
    void setControlDiagnostics(const QJsonObject &diagnostics);
    void reportActionFailure(const QString &detail);
    void observeControlEvent(const QJsonObject &event);
signals:
    void fieldCommandRequested(const QString &command, const QJsonObject &params);
    void authorizeOperationsRequested();
    void pendingPresetsChanged(const QJsonArray &ids);
    void startPresetsRequested(const QJsonArray &ids);
    void haltPresetsRequested(const QJsonArray &ids);
    void selectPresetsRequested(const QJsonArray &ids);
    void recoverPresetsRequested(const QJsonArray &ids);
    void controlEndpointReady(const QString &host, quint16 port, int protocolVersion, const QString &robotId);
    void controlUnavailable();
private:
    void updateActions();
    void append(const QString &text);
    void record(const QString &event, const QJsonValue &data, bool changed = false);
    void updateSummary();
    void syncMotionPreset();
    void applyMotionPreset(int index);
    QJsonObject selectedProfile() const;
    RuntimeService m_service;
    QList<QCheckBox *> m_presetChecks;
    QLabel *m_presetState;
    QPushButton *m_presetHalt, *m_presetSelect, *m_presetRecover, *m_presetHaltAll;
    QList<QLabel *> m_groupStates;
    QComboBox *m_speedLevel;
    QPushButton *m_speedApply, *m_globalReset, *m_clearEmergency, *m_allowRecovery, *m_emergencyHelp;
    QLabel *m_speedState, *m_recoveryHint;
    QDialog *m_settingsDialog, *m_controlsDialog;
    bool m_startWhenReady = false;
    QJsonArray m_startTarget;
    QString m_startProfile, m_startRevision, m_expectedStartRobot, m_expectedStartEndpoint, m_previousConnectionId;
    QJsonObject m_controlDiagnostics;
    ConfigurationWidget *m_configuration;
    QString m_settingsScope;
    QString m_activeToken, m_feedback, m_managementOutcome;
    RuntimeJournal m_journal;
    QLineEdit *m_host, *m_token;
    QSpinBox *m_port;
    QComboBox *m_profiles, *m_motionPreset;
    QPushButton *m_start, *m_stop, *m_preflight;
    QLabel *m_connection, *m_profileName;
};
