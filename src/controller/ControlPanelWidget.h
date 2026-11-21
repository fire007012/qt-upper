#ifndef CONTROLPANELWIDGET_H
#define CONTROLPANELWIDGET_H

#include <QString>
#include <QStringList>
#include <QWidget>
#include <QJsonArray>

class GamepadDisplayWidget;
class QLabel;
class QPushButton;

class ControlPanelWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ControlPanelWidget(QWidget *parent = nullptr);

    void setModeText(const QString &modeText);
    void setGamepadConnected(bool connected);
    void updateGamepadAxes(float lx, float ly, float rx, float ry, float lt, float rt);
    void updateGamepadButton(const QString &buttonName, bool pressed);
    void setKeyboardSource(bool keyboard);
    void setKeyboardKeys(const QStringList &keys);
    void setKeyboardCaptureActive(bool active);
    void setKeyboardContext(const QString &mode, const QJsonArray &presets, bool presetAware);

signals:
    void emergencyStopRequested();
    void gamepadConnectRequested();

private:
    QLabel *m_modeValue = nullptr;
    QLabel *m_inputSourceValue = nullptr;
    QLabel *m_gamepadLabel = nullptr;
    QLabel *m_gamepadValue = nullptr;
    QLabel *m_keyboardCaptureLabel = nullptr;
    QLabel *m_keyboardCaptureValue = nullptr;
    QPushButton *m_gamepadButton = nullptr;
    QPushButton *m_emergencyButton = nullptr;
    GamepadDisplayWidget *m_gamepadDisplay = nullptr;
    QLabel *m_keyboardHelp = nullptr;
    QLabel *m_keyboardKeys = nullptr;
};

#endif // CONTROLPANELWIDGET_H
