#pragma once
#include <QWidget>
#include "ConfigurationService.h"
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class ConfigurationWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConfigurationWidget(QWidget *parent = nullptr);
    ConfigurationService *service() { return &m_service; }
    void open(const QString &host, quint16 port, const QString &token);
    void close();
private:
    void update();
    void loadSource(const QJsonObject &source);
    void log(const QString &text);
    ConfigurationService m_service;
    QString m_token;
    double m_timeoutValue = 90;
    bool m_videoValue = false;
    QComboBox *m_profiles, *m_candidates;
    QDoubleSpinBox *m_timeout;
    QCheckBox *m_video;
    QWidget *m_timeoutRow, *m_videoRow;
    QLineEdit *m_target;
    QLabel *m_status, *m_operation;
    QPlainTextEdit *m_source, *m_preview, *m_log;
    QPushButton *m_refresh, *m_save, *m_load, *m_apply, *m_rollbackPreview, *m_rollback, *m_recover, *m_audit, *m_auditNext;
};
