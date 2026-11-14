#ifndef KEYBOARDCONTROLLER_H
#define KEYBOARDCONTROLLER_H

#include <QObject>
#include <QSet>
#include <QTimer>
#include <QKeyEvent>
#include <QStringList>

/**
 * @brief 键盘输入驱动 - 采集协议层稳定按键名
 *
 * 只负责采集键盘当前按下集合，不把按键解释成速度、关节或执行器动作。
 * 下位机 host_bridge_node 根据当前模式和映射表解释这些协议层按键名。
 */
class KeyboardController : public QObject
{
    Q_OBJECT

public:
    explicit KeyboardController(QObject *parent = nullptr);
    ~KeyboardController();

    void handleKeyPress(QKeyEvent *event);
    void handleKeyRelease(QKeyEvent *event);
    void clearPressedKeys();

    void setEnabled(bool enabled);
    bool isEnabled() const;
    QStringList currentKeys() const { return pressedKeyNames(); }

signals:
    void operatorInputChanged(const QStringList &pressedKeys);
    void emergencyStopRequested();
    void enabledChanged(bool enabled);

private slots:
    void publishOperatorInput();

private:
    QStringList pressedKeyNames() const;
    QString protocolKeyName(int key) const;

    QSet<int> m_pressedKeys;
    QSet<int> m_waitingForRelease;
    QTimer *m_sendTimer;

    bool m_enabled;
    static const int SEND_INTERVAL_MS = 33;  // approximately 30 Hz
};

#endif // KEYBOARDCONTROLLER_H
