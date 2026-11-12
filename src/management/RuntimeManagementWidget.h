#pragma once
#include <QWidget>
#include "RuntimeService.h"
class QCheckBox;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class ConfigurationWidget;

class RuntimeManagementWidget : public QWidget {
    Q_OBJECT
public:
    explicit RuntimeManagementWidget(QWidget *parent = nullptr);
    QJsonArray pendingPresets() const;
    void setControlDiagnostics(const QJsonObject &diagnostics);
signals:
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
    QJsonObject selectedProfile() const;
    RuntimeService m_service;
    QList<QCheckBox *> m_presetChecks;
    QLabel *m_presetState;
    QPushButton *m_presetStart, *m_presetHalt, *m_presetSelect, *m_presetRecover, *m_presetHaltAll;
    QPlainTextEdit *m_presetDetails;
    bool m_startWhenReady = false;
    QJsonArray m_startTarget;
    QString m_startProfile, m_startRevision, m_expectedStartRobot, m_expectedStartEndpoint, m_previousConnectionId;
    QJsonObject m_controlDiagnostics;
    ConfigurationWidget *m_configuration;
    QString m_settingsScope;
    QLineEdit *m_host, *m_token;
    QSpinBox *m_port;
    QComboBox *m_profiles;
    QPushButton *m_start, *m_stop, *m_preflight;
    QLabel *m_connection, *m_state, *m_operation, *m_safety;
    QPlainTextEdit *m_log, *m_details;
};
