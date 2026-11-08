#pragma once
#include <QWidget>
#include "RuntimeService.h"
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
signals:
    void controlEndpointReady(const QString &host, quint16 port, int protocolVersion, const QString &robotId);
    void controlUnavailable();
private:
    void updateActions();
    void append(const QString &text);
    QJsonObject selectedProfile() const;
    RuntimeService m_service;
    ConfigurationWidget *m_configuration;
    QString m_settingsScope;
    QLineEdit *m_host, *m_token;
    QSpinBox *m_port;
    QComboBox *m_profiles;
    QPushButton *m_start, *m_stop, *m_preflight;
    QLabel *m_connection, *m_state, *m_operation, *m_safety;
    QPlainTextEdit *m_log, *m_details;
};
