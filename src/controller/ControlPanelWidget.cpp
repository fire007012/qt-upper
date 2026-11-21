#include "ControlPanelWidget.h"

#include "GamepadDisplayWidget.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

ControlPanelWidget::ControlPanelWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    auto *statusGrid = new QGridLayout();
    statusGrid->setHorizontalSpacing(8);
    statusGrid->setVerticalSpacing(4);

    auto *modeLabel = new QLabel("控制对象", this);
    m_modeValue = new QLabel("车体运动", this);
    m_modeValue->setObjectName("controlTargetValue");
    m_inputSourceValue = new QLabel(QStringLiteral("键盘"), this);
    m_inputSourceValue->setObjectName("panelInputSourceValue");
    m_gamepadLabel = new QLabel("手柄连接", this);
    m_gamepadValue = new QLabel("未连接", this);
    m_gamepadValue->setObjectName("gamepadConnectionValue");
    m_keyboardCaptureLabel = new QLabel(QStringLiteral("键盘接收"), this);
    m_keyboardCaptureValue = new QLabel(this);
    m_keyboardCaptureValue->setObjectName("keyboardCaptureValue");
    m_keyboardCaptureValue->setWordWrap(true);
    setKeyboardCaptureActive(true);

    statusGrid->addWidget(modeLabel, 0, 0);
    statusGrid->addWidget(m_modeValue, 0, 1);
    statusGrid->addWidget(new QLabel(QStringLiteral("输入方式"), this), 1, 0);
    statusGrid->addWidget(m_inputSourceValue, 1, 1);
    statusGrid->addWidget(m_keyboardCaptureLabel, 2, 0);
    statusGrid->addWidget(m_keyboardCaptureValue, 2, 1);
    statusGrid->addWidget(m_gamepadLabel, 3, 0);
    statusGrid->addWidget(m_gamepadValue, 3, 1);

    auto *buttonLayout = new QHBoxLayout();
    m_gamepadButton = new QPushButton("连接手柄", this);
    m_gamepadButton->setObjectName("panelGamepadConnect");
    m_emergencyButton = new QPushButton("急停", this);
    m_emergencyButton->setObjectName("controlPanelEmergencyButton");
    buttonLayout->addWidget(m_gamepadButton);
    buttonLayout->addWidget(m_emergencyButton);

    m_gamepadDisplay = new GamepadDisplayWidget(this);
    m_keyboardHelp = new QLabel(this);
    m_keyboardHelp->setObjectName("keyboardControlHelp");
    m_keyboardHelp->setWordWrap(true);
    setKeyboardContext("vehicle", {}, false);
    m_keyboardKeys = new QLabel(QStringLiteral("当前按键：无"), this);
    m_keyboardKeys->setObjectName("keyboardPressedKeys");
    m_keyboardKeys->setWordWrap(true);

    mainLayout->addLayout(statusGrid);
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(m_keyboardHelp);
    mainLayout->addWidget(m_keyboardKeys);
    mainLayout->addWidget(m_gamepadDisplay, 1);
    setKeyboardSource(true);

    connect(m_gamepadButton, &QPushButton::clicked, this, &ControlPanelWidget::gamepadConnectRequested);
    connect(m_emergencyButton, &QPushButton::clicked, this, &ControlPanelWidget::emergencyStopRequested);

    setStyleSheet(
        "ControlPanelWidget { background: #ffffff; border: 1px solid #dbe3ef; border-radius: 6px; }"
        "ControlPanelWidget:focus { border: 2px solid #2563eb; }"
        "ControlPanelWidget QLabel { color: #111827; font-size: 13px; }"
        "ControlPanelWidget QPushButton { min-height: 28px; padding: 4px 10px; }"
        "QPushButton#controlPanelEmergencyButton { background: #b91c1c; color: white; font-weight: 700; }"
        "QPushButton#controlPanelEmergencyButton:hover { background: #991b1b; }");
}

void ControlPanelWidget::setKeyboardSource(bool keyboard)
{
    m_inputSourceValue->setText(keyboard ? QStringLiteral("键盘") : QStringLiteral("手柄"));
    m_gamepadLabel->setVisible(!keyboard);
    m_gamepadValue->setVisible(!keyboard);
    m_gamepadButton->setVisible(!keyboard);
    m_keyboardCaptureLabel->setVisible(keyboard);
    m_keyboardCaptureValue->setVisible(keyboard);
    m_keyboardHelp->setVisible(keyboard);
    m_keyboardKeys->setVisible(keyboard);
    m_gamepadDisplay->setVisible(!keyboard);
}

void ControlPanelWidget::setKeyboardCaptureActive(bool active)
{
    m_keyboardCaptureValue->setText(active ? QStringLiteral("已就绪（运动仍需授权）")
                                         : QStringLiteral("已暂停，请返回键盘控制"));
}

void ControlPanelWidget::setKeyboardKeys(const QStringList &keys)
{
    m_keyboardKeys->setText(QStringLiteral("当前按键：%1").arg(keys.isEmpty() ? QStringLiteral("无") : keys.join(", ")));
}

void ControlPanelWidget::setKeyboardContext(const QString &mode, const QJsonArray &presets, bool presetAware)
{
    QStringList lines{QStringLiteral("键盘控制 · %1").arg(mode == "arm" ? QStringLiteral("机械臂") : QStringLiteral("车体 / 摆臂"))};
    if (!presetAware) lines << QStringLiteral("运动组尚未同步；以下为当前控制域映射，运动仍需启动与授权。");
    if (mode == "arm") {
        if (!presetAware || presets.contains("arm")) {
            lines << QStringLiteral("末端平移：U/O X+/−，A/D Y+/−，W/S Z+/−")
                  << QStringLiteral("末端旋转：Q/E 绕X+/−，K/I 绕Y+/−，J/L 绕Z+/−")
                  << QStringLiteral("夹爪：F 打开，H 闭合");
        } else lines << QStringLiteral("机械臂组未选中；请选组，或切换到车体模式控制摆臂/底盘。");
    } else {
        if (!presetAware || presets.contains("base")) lines << QStringLiteral("底盘：W/S 前进后退，A/D 转向");
        if (!presetAware || presets.contains("flippers")) {
            lines << QStringLiteral("摆臂：Y/H 左前，U/J 右前，I/K 左后，O/L 右后（正/负方向）");
        }
        if (presetAware && !presets.contains("base") && !presets.contains("flippers"))
            lines << QStringLiteral("底盘/摆臂组未选中；机械臂需切换到机械臂模式。");
    }
    lines << QStringLiteral("空格：全车急停") << QStringLiteral("先启动运动组，释放按键并归中，再允许遥控。");
    m_keyboardHelp->setText(lines.join('\n'));
}

void ControlPanelWidget::setModeText(const QString &modeText)
{
    m_modeValue->setText(modeText);
}

void ControlPanelWidget::setGamepadConnected(bool connected)
{
    m_gamepadValue->setText(connected ? "已连接" : "未连接");
    m_gamepadValue->setStyleSheet(connected ? "color: #15803d; font-weight: 600;" : "color: #b91c1c; font-weight: 600;");
    m_gamepadButton->setText(connected ? "断开手柄" : "连接手柄");
}

void ControlPanelWidget::updateGamepadAxes(float lx, float ly, float rx, float ry, float lt, float rt)
{
    m_gamepadDisplay->updateAll(lx, ly, rx, ry, lt, rt);
}

void ControlPanelWidget::updateGamepadButton(const QString &buttonName, bool pressed)
{
    m_gamepadDisplay->updateButton(buttonName, pressed);
}
