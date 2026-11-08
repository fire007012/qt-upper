#ifndef TELEMETRYPANELWIDGET_H
#define TELEMETRYPANELWIDGET_H

#include <QHash>
#include <QString>
#include <QWidget>

class QLabel;

class TelemetryPanelWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TelemetryPanelWidget(QWidget *parent = nullptr);

    void setConnectionStatus(bool connected);
    void setHeartbeatStatus(bool online);
    void setFps(int fps);
    void setBandwidthText(const QString &text);
    void setModeText(const QString &text);
    void setGamepadConnected(bool connected);
    void setErrorCount(int count);
    void setCO2Value(float ppm);
    void setCO2Stale() { setMetricValue(QStringLiteral("co2"), QStringLiteral("未知/过期"), QStringLiteral("#64748b")); }

private:
    void setMetricValue(const QString &key, const QString &value, const QString &color = QString());

private:
    QHash<QString, QLabel*> m_valueLabels;
};

#endif // TELEMETRYPANELWIDGET_H
