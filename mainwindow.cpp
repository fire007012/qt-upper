#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "src/communication/HostProtocol.h"
#include "src/communication/ControlProtocol.h"
#include "src/communication/SharedStructs.h"
#include "src/controller/MotorRuntimeCarouselWidget.h"
#include "src/controller/ProtocolLogFormatter.h"
#include "src/controller/TcpConnectionDialog.h"
#include <QtGlobal>
#include <QDockWidget>
#include <QToolBar>
#include "src/management/RuntimeManagementWidget.h"
#include "src/diagnostics/DiagnosticsWidget.h"

#include <QCloseEvent>
#include <QMessageBox>
#include <QStatusBar>
#include <QDebug>
#include <QDateTime>
#include <QInputDialog>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QApplication>
#include <QShortcut>
#include <QKeySequence>
#include <QSettings>
#include <QFrame>
#include <QTextEdit>
#include <QJsonArray>
#include <QGroupBox>
#include <QGridLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSizePolicy>
#include <QSplitter>
#include <QScrollArea>
#include <QScreen>

namespace {

double normalizeStickAxis(int16_t value)
{
    return qBound(-1.0, static_cast<double>(value) / 32767.0, 1.0);
}

double normalizeTriggerAxis(uint8_t value)
{
    return qBound(0.0, static_cast<double>(value) / 255.0, 1.0);
}

Communication::GamepadInputState makeGamepadInputState(const ControllerState &state, bool connected)
{
    Communication::GamepadInputState gamepad;
    gamepad.connected = connected;

    gamepad.buttons.a = state.buttonA;
    gamepad.buttons.b = state.buttonB;
    gamepad.buttons.x = state.buttonX;
    gamepad.buttons.y = state.buttonY;
    gamepad.buttons.start = state.buttonStart;
    gamepad.buttons.back = state.buttonBack;
    gamepad.buttons.lb = state.leftShoulder;
    gamepad.buttons.rb = state.rightShoulder;
    gamepad.buttons.l3 = state.leftThumb;
    gamepad.buttons.r3 = state.rightThumb;
    gamepad.buttons.dpadUp = state.dpadUp;
    gamepad.buttons.dpadDown = state.dpadDown;
    gamepad.buttons.dpadLeft = state.dpadLeft;
    gamepad.buttons.dpadRight = state.dpadRight;

    gamepad.axes.leftX = connected ? normalizeStickAxis(state.sThumbLX) : 0.0;
    gamepad.axes.leftY = connected ? normalizeStickAxis(state.sThumbLY) : 0.0;
    gamepad.axes.rightX = connected ? normalizeStickAxis(state.sThumbRX) : 0.0;
    gamepad.axes.rightY = connected ? normalizeStickAxis(state.sThumbRY) : 0.0;
    gamepad.axes.lt = connected ? normalizeTriggerAxis(state.bLeftTrigger) : 0.0;
    gamepad.axes.rt = connected ? normalizeTriggerAxis(state.bRightTrigger) : 0.0;

    return gamepad;
}

QString modeNameForProtocol(ControlMode mode)
{
    return mode == ControlMode::Arm ? QStringLiteral("arm") : QStringLiteral("vehicle");
}

QString displayNameForControlMode(ControlMode mode)
{
    return mode == ControlMode::Arm ? QStringLiteral("机械臂操控") : QStringLiteral("车体运动");
}

bool parseControlMode(const QString &modeText, ControlMode *mode)
{
    const QString normalized = modeText.trimmed().toLower();
    if (normalized == QStringLiteral("arm")) {
        *mode = ControlMode::Arm;
        return true;
    }
    if (normalized == QStringLiteral("vehicle") || normalized == QStringLiteral("base")) {
        *mode = ControlMode::Vehicle;
        return true;
    }
    return false;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_controller(nullptr)
    , m_keyboardController(nullptr)
    , m_cameraGridWidget(nullptr)
    , m_telemetryPanel(nullptr)
    , m_controlPanel(nullptr)
    , m_handleKey(nullptr)
{
    ui->setupUi(this);

    // 设置窗口属性
    setWindowTitle("上位机v2");
    setMinimumSize(480, 360);
    m_statusErrorLabel = ui->label_status_error_count;

    // 初始化组件
    setupController();
    setupDisplayLayout();
    setupKeyboardController();
    setupHandleKey();
    setupTimers();

    // 全局Space急停快捷键（不受焦点影响，只需创建一次）
    QShortcut *spaceStop = new QShortcut(QKeySequence(Qt::Key_Space), this);
    spaceStop->setContext(Qt::ApplicationShortcut);
    connect(spaceStop, &QShortcut::activated, this, [this]() {
        triggerEmergencyStop(QStringLiteral("keyboard_space"));
    });

    // 设置连接和状态栏
    setupConnections();
    setupStatusBar();

    auto *runtimeDock = new QDockWidget(QStringLiteral("一键启动"), this);
    runtimeDock->setObjectName("runtimeDock");
    auto *runtimeWidget = new RuntimeManagementWidget(runtimeDock);
    auto *runtimeScroll = new QScrollArea(runtimeDock);
    runtimeScroll->setWidgetResizable(true);
    runtimeScroll->setWidget(runtimeWidget);
    runtimeDock->setWidget(runtimeScroll);
    runtimeDock->setMinimumWidth(320);
    addDockWidget(Qt::RightDockWidgetArea, runtimeDock);
    runtimeDock->hide();
    auto *runtimeToolbar = addToolBar(QStringLiteral("系统管理"));
    auto *runtimeAction = runtimeDock->toggleViewAction();
    runtimeAction->setText(QStringLiteral("一键启动"));
    runtimeToolbar->addAction(runtimeAction);
    runtimeToolbar->addSeparator();
    runtimeToolbar->addWidget(new QLabel(QStringLiteral("输入方式："), this));
    auto *inputSource = new QComboBox(this);
    inputSource->setObjectName("inputSourceSelector");
    inputSource->addItem(QStringLiteral("键盘"));
    inputSource->addItem(QStringLiteral("手柄"));
    runtimeToolbar->addWidget(inputSource);
    auto *keyboardFocus = new QPushButton(QStringLiteral("返回键盘控制"), this);
    keyboardFocus->setObjectName("focusKeyboardButton");
    keyboardFocus->setToolTip(QStringLiteral("结束参数编辑并回到按键接收区域；右侧显示键盘状态和当前按键。不会启动电机或授予遥控权限。"));
    runtimeToolbar->addWidget(keyboardFocus);
    connect(keyboardFocus, &QPushButton::clicked, this, [this] {
        activateWindow();
        if (auto *sidebar = findChild<QScrollArea *>("sidebarScrollArea"))
            sidebar->ensureWidgetVisible(m_controlPanel);
        if (auto *workspace = findChild<QScrollArea *>("workspaceScrollArea"))
            workspace->ensureWidgetVisible(m_controlPanel);
        m_controlPanel->setFocus(Qt::OtherFocusReason);
        QTimer::singleShot(0, m_controlPanel, [panel = m_controlPanel] {
            panel->setFocus(Qt::OtherFocusReason);
        });
        statusBar()->showMessage(QStringLiteral("键盘接收已就绪：按 W/A/S/D 可在右侧查看按键；电机运动仍需启动组并允许遥控。"), 6000);
    });
    connect(inputSource, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, keyboardFocus](int index) {
        m_inputSource = index == 0 ? Communication::OperatorInputState::Source::Keyboard
                                 : Communication::OperatorInputState::Source::Gamepad;
        m_controller->setOperatorControlEnabled(false);
        m_keyboardController->clearPressedKeys();
        m_controlPanel->setKeyboardSource(index == 0);
        keyboardFocus->setVisible(index == 0);
        m_controlPanel->setFocus(Qt::OtherFocusReason);
        addCommand(QStringLiteral("[输入方式] 已切换为%1；请释放按键、手柄归中，重新允许操作/遥控。")
                   .arg(index == 0 ? QStringLiteral("键盘") : QStringLiteral("手柄")));
        sendOperatorInputSnapshot();
    });
    auto *diagnosticsDock = new QDockWidget(QStringLiteral("控制与遥测诊断"), this);
    auto *diagnosticsScroll = new QScrollArea(diagnosticsDock);
    diagnosticsScroll->setWidgetResizable(true);
    diagnosticsScroll->setWidget(new DiagnosticsWidget(m_controller, diagnosticsDock));
    diagnosticsDock->setWidget(diagnosticsScroll);
    diagnosticsDock->setMinimumWidth(320); addDockWidget(Qt::RightDockWidgetArea, diagnosticsDock); diagnosticsDock->hide();
    runtimeToolbar->addAction(diagnosticsDock->toggleViewAction());
    connect(runtimeWidget, &RuntimeManagementWidget::controlUnavailable, this, [this] {
        m_controller->setOperatorControlEnabled(false);
        clearOperatorInputSnapshot();
        m_controller->disconnectTcp();
    });
    connect(runtimeWidget, &RuntimeManagementWidget::controlEndpointReady, this,
            [this](const QString &host, quint16 port, int protocolVersion, const QString &robotId) {
        clearOperatorInputSnapshot();
        m_controller->setOperatorControlEnabled(false);
        m_controller->connectManagedBridge(host, port, protocolVersion, robotId);
        addCommand(QStringLiteral("[管理] 软件可服务，正在同步控制桥；遥控保持暂停"));
    });

    m_controller->setPendingPresets(runtimeWidget->pendingPresets());
    connect(runtimeWidget, &RuntimeManagementWidget::pendingPresetsChanged, m_controller, &Controller::setPendingPresets);
    connect(runtimeWidget, &RuntimeManagementWidget::startPresetsRequested, this, [this, runtimeWidget](const QJsonArray &ids) {
        if (!m_controller->startPresets(ids)) runtimeWidget->reportActionFailure(QStringLiteral("[预设] 启动未提交：需预设能力、新鲜状态、无未决任务且急停已解除。"));
    });
    connect(runtimeWidget, &RuntimeManagementWidget::haltPresetsRequested, this, [this, runtimeWidget](const QJsonArray &ids) {
        if (!m_controller->haltPresets(ids)) runtimeWidget->reportActionFailure(QStringLiteral("[预设] 停止未提交，请检查已确认选择和未决任务。"));
    });
    connect(runtimeWidget, &RuntimeManagementWidget::selectPresetsRequested, this, [this, runtimeWidget](const QJsonArray &ids) {
        if (!m_controller->selectPresets(ids)) runtimeWidget->reportActionFailure(QStringLiteral("[预设] 选择未提交：请归中；取消组须先停止并取得新鲜诊断。"));
    });
    connect(runtimeWidget, &RuntimeManagementWidget::recoverPresetsRequested, this, [this, runtimeWidget](const QJsonArray &ids) {
        if (!m_controller->sendSystemCommand(QStringLiteral("recover"), {{"presets",ids}}))
            runtimeWidget->reportActionFailure(QStringLiteral("[预设] 恢复未提交：请先归中并允许操作；只可恢复已确认选择的非空子集。"));
    });
    connect(runtimeWidget, &RuntimeManagementWidget::fieldCommandRequested, this, [this, runtimeWidget](const QString &command, const QJsonObject &params) {
        if (!m_controller->sendSystemCommand(command, params)) runtimeWidget->reportActionFailure(QStringLiteral("[现场控制] 请求未提交：核对能力、归中、授权、急停和未决操作。"));
    });
    connect(runtimeWidget, &RuntimeManagementWidget::authorizeOperationsRequested, this, [this,runtimeWidget] {
        if (!m_controller->authorizeOperations()) runtimeWidget->reportActionFailure(QStringLiteral("恢复操作授权未提交：请先解除急停、归中并等待中立输入确认。"));
    });
    connect(m_controller, &Controller::protocolMessageReceived, runtimeWidget, &RuntimeManagementWidget::observeControlEvent);
    auto *presetTimer = new QTimer(runtimeWidget);
    presetTimer->setInterval(100);
    connect(presetTimer, &QTimer::timeout, runtimeWidget, [this, runtimeWidget] {
        const auto diagnostics = m_controller->controlDiagnostics();
        runtimeWidget->setControlDiagnostics(diagnostics);
        const auto actual = diagnostics["server"].toObject();
        if (m_controlPanel) m_controlPanel->setKeyboardContext(modeNameForProtocol(m_controlMode),
            diagnostics["selected_presets"].toArray(), diagnostics["negotiated"].toBool() && diagnostics["motion_presets_supported"].toBool());
        const auto globalStop = actual["global_stop"].toObject();
        ui->btn_clear_emergency->setEnabled(diagnostics["global_stop_supported"].toBool() && diagnostics["ready"].toBool()
            && actual["global_stop_reset_ready"] != QJsonValue(false)
            && actual["runtime_fresh"].toBool() && actual["runtime_global_inhibit"] == QJsonValue(true)
            && globalStop["state"] == QJsonValue("succeeded") && globalStop["ok"] == QJsonValue(true));
        ui->btn_clear_emergency->setToolTip(QStringLiteral("确认急停原因已处理，独立禁止成功且反馈新鲜后，只清桥接锁存；随后仍需允许操作并显式复位 C++ 禁止。"));
        const bool anyDraft = !runtimeWidget->pendingPresets().isEmpty();
        if (m_operatorControlButton) m_operatorControlButton->setEnabled(anyDraft || m_operatorControlEnabled);
        if (m_operationsControlButton) m_operationsControlButton->setEnabled(anyDraft || m_operationsAuthorized);
        if (diagnostics["motion_presets_supported"].toBool()) {
            const bool arm = diagnostics["selected_presets"].toArray().contains("arm");
            ui->btn_mode_arm->setEnabled(arm);
            if (m_refreshArmActionButton) m_refreshArmActionButton->setEnabled(arm);
            if (m_executeArmActionButton && !arm) m_executeArmActionButton->setEnabled(false);
        }
    });
    presetTimer->start();

    // 初始化UI状态
    updateConnectionDisplay();
    updateHeartbeatDisplay();
    updateGamepadDisplay();
    ui->label_co2_value->setText(QStringLiteral("未知/过期"));
    if (m_telemetryPanel) m_telemetryPanel->setCO2Stale();

    // 添加系统启动提示
    addCommand("[系统] 主窗口初始化完成");
    addCommand("[系统] 请连接TCP设备开始接收数据");
    setupAdaptiveLayout();
}

void MainWindow::setupAdaptiveLayout()
{
    // Two status rows keep emergency buttons visible without a 1600 px minimum.
    auto *old = ui->frame_status->layout();
    while (auto *item = old->takeAt(0)) {
        if (auto *widget = item->widget()) widget->hide();
        delete item;
    }
    delete old;
    auto *status = new QGridLayout(ui->frame_status);
    status->setContentsMargins(6, 4, 6, 4);
    status->setSpacing(4);
    auto group = [this, status](int row, int col, std::initializer_list<QWidget *> widgets) {
        auto *box = new QWidget(ui->frame_status);
        auto *layout = new QHBoxLayout(box);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        for (auto *widget : widgets) { layout->addWidget(widget); widget->show(); }
        layout->addStretch();
        status->addWidget(box, row, col);
    };
    ui->label_cpu_value->setMinimumWidth(100);
    ui->label_cpu_value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    group(0, 0, {ui->label_connection_status, ui->label_connection_value});
    group(0, 1, {ui->label_heartbeat, ui->label_heartbeat_value});
    group(0, 2, {ui->label_fps, ui->label_fps_value});
    group(0, 3, {ui->label_cpu, ui->label_cpu_value});
    group(1, 0, {ui->label_mode, ui->label_mode_value});
    group(1, 1, {ui->label_status_error_count});
    group(1, 2, {ui->label_co2, ui->label_co2_value});
    group(1, 3, {ui->btn_clear_emergency, ui->btn_emergency_stop});
    ui->frame_status->setMaximumHeight(QWIDGETSIZE_MAX);
    ui->frame_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    ui->group_cameras->setMinimumSize(360, 240);
    ui->widget_right_panel->setMinimumWidth(320);
    ui->motorRuntimeWidget->setMinimumHeight(160);
    ui->group_car_model->setMinimumHeight(180);
    ui->controlPanelWidget->setMinimumHeight(0);
    ui->controlPanelWidget->setMaximumHeight(QWIDGETSIZE_MAX);
    ui->verticalLayout_right->removeWidget(m_controlPanel);
    ui->verticalLayout_right->insertWidget(0, m_controlPanel);
    ui->verticalLayout_right->removeWidget(ui->group_lifecycle_control);
    ui->verticalLayout_right->insertWidget(1, ui->group_lifecycle_control);
    for (int i = 0; i < ui->verticalLayout_right->count(); ++i)
        ui->verticalLayout_right->setStretch(i, i == 3 ? 1 : 0);
    auto *sidebar = new QScrollArea(ui->centralwidget);
    sidebar->setObjectName("sidebarScrollArea");
    sidebar->setMinimumWidth(340);
    sidebar->setWidgetResizable(true);
    sidebar->setFrameShape(QFrame::NoFrame);
    ui->splitter_main->replaceWidget(1, sidebar);
    sidebar->setWidget(ui->widget_right_panel);
    ui->splitter_main->setSizes({800, 340});
    auto *workspace = takeCentralWidget();
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("workspaceScrollArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    workspace->setMinimumSize(960, 460);
    scroll->setWidget(workspace);
    setCentralWidget(scroll);
    for (auto *label : {m_connectionPhaseLabel, m_controlPermissionLabel}) {
        label->setMinimumWidth(0);
        label->setMaximumWidth(320);
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    if (auto *screen = this->screen()) {
        const auto available = screen->availableGeometry();
        const QSize target(qMin(1440, available.width() - 24), qMin(900, available.height() - 64));
        resize(target.expandedTo(minimumSize()));
        move(available.center() - QPoint(width() / 2, height() / 2));
    }
}

MainWindow::~MainWindow()
{
    // QApplication focus notifications can arrive while QWidget tears down children.
    // Stop capture before QMainWindow's status bar and controller are destroyed.
    disconnect(qApp, nullptr, this, nullptr);
    if (m_keyboardController) {
        m_keyboardController->setCaptureWidget(nullptr);
        disconnect(m_keyboardController, nullptr, this, nullptr);
        m_keyboardController->setEnabled(false);
    }
    if (m_statusTimer) {
        m_statusTimer->stop();
    }
    if (m_controller) {
        m_controller->stop();
    }
    delete ui;
}

void MainWindow::setupController()
{
    m_controller = new Controller(this);
    m_controller->initialize();
    m_controller->start();

    // 连接Controller信号到UI更新

    // TCP状态
    connect(m_controller, &Controller::tcpConnected, this, &MainWindow::onTcpConnected);
    connect(m_controller, &Controller::tcpDisconnected, this, &MainWindow::onTcpDisconnected);
    connect(m_controller, &Controller::tcpError, this, &MainWindow::onTcpError);
    connect(m_controller, &Controller::tcpHeartbeatChanged, this, &MainWindow::onTcpHeartbeatChanged);
    connect(m_controller, &Controller::operatorControlChanged, this, [this](bool enabled) {
        m_operatorControlEnabled = enabled;
        if (m_operatorControlButton) {
            m_operatorControlButton->setText(enabled ? QStringLiteral("暂停遥控") : QStringLiteral("允许遥控"));
        }
        addCommand(enabled ? QStringLiteral("[遥控] 已允许发送操作者输入")
                           : QStringLiteral("[遥控] 已暂停，需要释放按键、摇杆归中并重新允许遥控"));
    });

    connect(m_controller, &Controller::operationsAuthorizationChanged, this, [this](bool enabled) {
        m_operationsAuthorized = enabled;
        if (m_operationsControlButton) m_operationsControlButton->setText(enabled ? QStringLiteral("撤销操作授权") : QStringLiteral("允许操作"));
        addCommand(enabled ? QStringLiteral("[授权] 已允许生命周期/模式操作；运动仍需单独授权。") : QStringLiteral("[授权] 操作授权已撤销。"));
    });
    connect(m_controller, &Controller::inputSampleRequested, this, [this] {
        if (m_handleKey) m_latestGamepadState = m_handleKey->sampleNow();
        sendOperatorInputSnapshot();
    }, Qt::QueuedConnection);
    connect(m_controller, &Controller::inputResetRequested, this, [this] {
        clearOperatorInputSnapshot();
        if (m_keyboardController) m_keyboardController->clearPressedKeys();
    });
    connect(m_controller, &Controller::telemetryFreshnessChanged, this, [this](const QString &topic, bool fresh) {
        if (topic == "control" && !fresh && m_controlPermissionLabel) m_controlPermissionLabel->setText(QStringLiteral("运行许可：未知/过期；桥接急停：待查询"));
        if (topic == "co2" && !fresh) {
            ui->label_co2_value->setText(QStringLiteral("未知/过期"));
            ui->label_co2_value->setStyleSheet(QStringLiteral("color: #64748b;"));
            if (m_telemetryPanel) m_telemetryPanel->setCO2Stale();
        }
        if (topic == "joints" && !fresh && m_motorRuntimeWidget) m_motorRuntimeWidget->setRuntimeStates({});
        if (topic == "imu" && m_robotAttitudeWidget) m_robotAttitudeWidget->setDataFresh(fresh);
    });

    // 数据接收
    connect(m_controller, &Controller::motorStateReceived, this, &MainWindow::onMotorStateReceived);
    connect(m_controller, &Controller::jointRuntimeStatesReceived, this, &MainWindow::onJointRuntimeStatesReceived);
    connect(m_controller, &Controller::co2DataReceived, this, &MainWindow::onCO2DataReceived);
    connect(m_controller, &Controller::imuDataReceived, this, &MainWindow::onIMUDataReceived);
    connect(m_controller, &Controller::cameraInfoReceived, this, &MainWindow::onCameraInfoReceived);
    connect(m_controller, &Controller::protocolMessageReceived, this, &MainWindow::onProtocolMessageReceived);

    // 系统错误
    connect(m_controller, &Controller::systemError, this, [this](const QString &error) {
        addError(error);
    });
}

void MainWindow::setupDisplayLayout()
{
    m_cameraGridWidget = ui->cameraGridWidget;
    connect(m_cameraGridWidget, &CameraGridWidget::cameraListRefreshRequested,
            this, [this]() {
        if (m_controller && m_controller->requestCameraList()) {
            addCommand("[视频] 已请求刷新视频源列表");
        }
    });

    m_motorRuntimeWidget = ui->motorRuntimeWidget;
    m_controlPanel = ui->controlPanelWidget;
    m_robotAttitudeWidget = ui->robotAttitudeWidget;
    m_logTabs = ui->logTabs;
    setupBottomActionPanel();
    ui->verticalLayout_left_workspace->setStretch(0, 1);
    ui->verticalLayout_left_workspace->setStretch(1, 0);
    ui->verticalLayout_right->setStretch(0, 1);
    ui->verticalLayout_right->setStretch(1, 1);
    ui->verticalLayout_right->setStretch(2, 2);
    ui->verticalLayout_right->setStretch(3, 0);

    auto connectLifecycleButton = [this](QPushButton *button, const QString &command) {
        connect(button, &QPushButton::clicked, this, [this, command]() {
            addCommand(QString("[生命周期] 请求 %1").arg(command));
            if (m_controller && m_controller->isTcpConnected()) {
                if (!m_controller->sendSystemCommand(command)) addError(QStringLiteral("[生命周期] 请求未发送，请确认v2会话、中立输入及“允许操作”授权。"));
            } else {
                addError(QString("[生命周期] TCP未连接，未发送 %1").arg(command));
            }
        });
    };

    connectLifecycleButton(ui->btn_lifecycle_init, QStringLiteral("init"));
    connectLifecycleButton(ui->btn_lifecycle_enable, QStringLiteral("enable"));
    ui->btn_lifecycle_disable->setText(QStringLiteral("停止"));
    connectLifecycleButton(ui->btn_lifecycle_disable, QStringLiteral("halt"));
    connectLifecycleButton(ui->btn_lifecycle_halt, QStringLiteral("halt"));
    connectLifecycleButton(ui->btn_lifecycle_resume, QStringLiteral("resume"));
    connectLifecycleButton(ui->btn_lifecycle_recover, QStringLiteral("recover"));
    connectLifecycleButton(ui->btn_lifecycle_shutdown, QStringLiteral("shutdown"));

    auto connectControlModeButton = [this](QPushButton *button, ControlMode controlMode,
                                           const QString &protocolMode,
                                           const QString &label) {
        Q_UNUSED(controlMode);
        connect(button, &QPushButton::clicked, this, [this, protocolMode, label]() {
            addCommand(QString("[控制域] 请求切换到 %1").arg(label));
            if (m_controller && m_controller->isTcpConnected()) {
                QJsonObject params;
                params["mode"] = protocolMode;
                if (m_controller->sendSystemCommand(QStringLiteral("set_control_mode"), params)) {
                    addCommand(QStringLiteral("[控制域] 操作已提交，等待任务执行完成后同步实际模式。"));
                }
            } else {
                addError(QString("[控制域] TCP未连接，未发送 %1").arg(label));
            }
        });
    };

    connectControlModeButton(ui->btn_mode_vehicle, ControlMode::Vehicle,
                             QStringLiteral("vehicle"), QStringLiteral("底盘"));
    connectControlModeButton(ui->btn_mode_arm, ControlMode::Arm,
                             QStringLiteral("arm"), QStringLiteral("机械臂"));

    connect(m_controlPanel, &ControlPanelWidget::gamepadConnectRequested,
            this, &MainWindow::toggleGamepadConnection);
    connect(m_controlPanel, &ControlPanelWidget::emergencyStopRequested,
            this, [this]() {
        triggerEmergencyStop(QStringLiteral("control_panel"));
    });
}

void MainWindow::setupBottomActionPanel()
{
    if (!m_logTabs) {
        return;
    }

    auto *bottomContainer = new QWidget(ui->widget_left_workspace);
    bottomContainer->setObjectName(QStringLiteral("widget_bottom_action_container"));
    bottomContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bottomContainer->setFixedHeight(220);

    auto *bottomContainerLayout = new QVBoxLayout(bottomContainer);
    bottomContainerLayout->setContentsMargins(0, 0, 0, 0);
    bottomContainerLayout->setSpacing(0);

    auto *bottomSplit = new QSplitter(Qt::Horizontal, bottomContainer);
    bottomSplit->setObjectName(QStringLiteral("splitter_bottom_logs_actions"));
    bottomSplit->setChildrenCollapsible(false);
    bottomSplit->setHandleWidth(6);
    bottomSplit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    ui->verticalLayout_left_workspace->removeWidget(m_logTabs);
    m_logTabs->setMinimumHeight(0);
    m_logTabs->setMaximumHeight(16777215);
    bottomSplit->addWidget(m_logTabs);

    auto *actionPanel = new QWidget(bottomSplit);
    actionPanel->setObjectName(QStringLiteral("widget_bottom_action_panel"));
    actionPanel->setMinimumWidth(640);
    actionPanel->setStyleSheet(
        "QGroupBox { font-weight: 700; border: 1px solid #dbe3ef; border-radius: 6px; margin-top: 6px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #334155; }"
        "QPushButton { min-height: 24px; padding: 1px 10px; }");

    auto *actionLayout = new QHBoxLayout(actionPanel);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(6);

    auto *armGroup = new QGroupBox(QStringLiteral("机械臂 Action"), actionPanel);
    auto *armLayout = new QVBoxLayout(armGroup);
    armLayout->setContentsMargins(8, 8, 8, 8);
    armLayout->setSpacing(6);

    m_armActionList = new QListWidget(armGroup);
    m_armActionList->setObjectName(QStringLiteral("list_arm_named_targets"));
    m_armActionList->setMinimumHeight(76);
    m_armActionList->addItem(QStringLiteral("等待下位机下发 MoveIt 固定位姿..."));
    m_armActionList->item(0)->setFlags(Qt::NoItemFlags);

    auto *armButtonLayout = new QHBoxLayout();
    armButtonLayout->setSpacing(4);
    m_refreshArmActionButton = new QPushButton(QStringLiteral("刷新位姿"), armGroup);
    m_executeArmActionButton = new QPushButton(QStringLiteral("运动到该位置"), armGroup);
    m_executeArmActionButton->setEnabled(false);
    armButtonLayout->addWidget(m_refreshArmActionButton);
    armButtonLayout->addWidget(m_executeArmActionButton);

    armLayout->addWidget(m_armActionList, 1);
    armLayout->addLayout(armButtonLayout);

    auto *monitorGroup = new QGroupBox(QStringLiteral("任务标志"), actionPanel);
    monitorGroup->setMinimumWidth(332);
    monitorGroup->setMaximumWidth(332);
    auto *monitorLayout = new QGridLayout(monitorGroup);
    monitorLayout->setContentsMargins(6, 4, 6, 4);
    monitorLayout->setHorizontalSpacing(6);
    monitorLayout->setVerticalSpacing(4);

    auto *thermalButton = new QPushButton(QStringLiteral("热源监测"), monitorGroup);
    auto *dynamicButton = new QPushButton(QStringLiteral("动态监测"), monitorGroup);
    auto *dangerButton = new QPushButton(QStringLiteral("危险标志"), monitorGroup);
    auto *qrCodeButton = new QPushButton(QStringLiteral("二维码检测"), monitorGroup);
    auto *resetMonitorButton = new QPushButton(QStringLiteral("重置"), monitorGroup);
    monitorLayout->addWidget(thermalButton, 0, 0);
    monitorLayout->addWidget(dynamicButton, 0, 1);
    monitorLayout->addWidget(dangerButton, 0, 2);
    monitorLayout->addWidget(qrCodeButton, 1, 0);
    monitorLayout->addWidget(resetMonitorButton, 1, 1);
    for (auto *button : {thermalButton,dynamicButton,dangerButton,qrCodeButton,resetMonitorButton}) {
        button->setEnabled(false);
        button->setToolTip(Communication::ControlProtocol::unsupportedCommandReason(QStringLiteral("thermal_monitor")));
    }
    auto *monitorNotice = new QLabel(QStringLiteral("当前控制协议未提供检测管理接口"), monitorGroup);
    monitorNotice->setWordWrap(true); monitorLayout->addWidget(monitorNotice,2,0,1,3);
    monitorLayout->setColumnStretch(0, 1);
    monitorLayout->setColumnStretch(1, 1);
    monitorLayout->setColumnStretch(2, 1);

    actionLayout->addWidget(armGroup, 3);
    actionLayout->addWidget(monitorGroup, 0);

    auto *actionScroll = new QScrollArea(bottomContainer);
    actionScroll->setObjectName("actionScrollArea");
    actionScroll->setWidgetResizable(true);
    actionScroll->setWidget(actionPanel);
    bottomSplit->addWidget(actionScroll);
    bottomSplit->setStretchFactor(0, 4);
    bottomSplit->setStretchFactor(1, 2);
    bottomSplit->setSizes({1040, 360});
    bottomContainerLayout->addWidget(bottomSplit);
    ui->verticalLayout_left_workspace->addWidget(bottomContainer);

    connect(m_refreshArmActionButton, &QPushButton::clicked,
            this, &MainWindow::refreshArmNamedTargets);
    connect(m_executeArmActionButton, &QPushButton::clicked,
            this, &MainWindow::executeSelectedArmNamedTarget);
    connect(m_armActionList, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem *current) {
        m_executeArmActionButton->setEnabled(current && current->flags().testFlag(Qt::ItemIsEnabled)
                                             && !current->data(Qt::UserRole).toString().isEmpty());
    });

    connect(thermalButton, &QPushButton::clicked, this, [this]() {
        handleMonitorCommand(QStringLiteral("热源监测"), QStringLiteral("thermal_monitor"));
    });
    connect(dynamicButton, &QPushButton::clicked, this, [this]() {
        handleMonitorCommand(QStringLiteral("动态监测"), QStringLiteral("dynamic_monitor"));
    });
    connect(dangerButton, &QPushButton::clicked, this, [this]() {
        handleMonitorCommand(QStringLiteral("危险标志"), QStringLiteral("danger_sign"));
    });
    connect(qrCodeButton, &QPushButton::clicked, this, [this]() {
        handleMonitorCommand(QStringLiteral("二维码检测"), QStringLiteral("qr_code_detect"));
    });
    connect(resetMonitorButton, &QPushButton::clicked, this, [this]() {
        handleMonitorCommand(QStringLiteral("重置"), QStringLiteral("reset_monitor"));
    });
}

void MainWindow::handleMonitorCommand(const QString &label, const QString &command)
{
    if (!Communication::ControlProtocol::supportsSystemCommand(command)) {
        addError(QStringLiteral("[监测] %1：%2").arg(label,Communication::ControlProtocol::unsupportedCommandReason(command))); return;
    }
    if (m_controller && m_controller->isTcpConnected()) {
        if (m_controller->sendSystemCommand(command)) addCommand(QStringLiteral("[监测] 已提交 %1，等待结果").arg(label));
        else addError(QStringLiteral("[监测] %1 未提交，请检查会话与授权。").arg(label));
    } else {
        addError(QString("[监测] TCP未连接，未发送 %1").arg(label));
    }
}

void MainWindow::refreshArmNamedTargets()
{
    addCommand("[机械臂] 请求刷新 MoveIt 固定位姿");
    if (m_controller && m_controller->isTcpConnected()) {
        m_controller->sendSystemCommand(QStringLiteral("request_arm_named_targets"));
    } else {
        addError("[机械臂] TCP未连接，未请求固定位姿");
    }
}

void MainWindow::executeSelectedArmNamedTarget()
{
    if (!m_armActionList) {
        return;
    }

    QListWidgetItem *item = m_armActionList->currentItem();
    const QString targetName = item ? item->data(Qt::UserRole).toString() : QString();
    if (targetName.isEmpty()) {
        addError("[机械臂] 未选择有效固定位姿");
        return;
    }

    addCommand(QString("[机械臂] 请求运动到固定位姿: %1").arg(targetName));
    if (m_controller && m_controller->isTcpConnected()) {
        QJsonObject params;
        params["target"] = targetName;
        m_controller->sendSystemCommand(QStringLiteral("move_arm_named_target"), params);
    } else {
        addError(QString("[机械臂] TCP未连接，未发送固定位姿: %1").arg(targetName));
    }
}

void MainWindow::updateArmNamedTargets(const QJsonObject &message)
{
    if (!m_armActionList) {
        return;
    }

    const QStringList targetFields = {
        QStringLiteral("targets"),
        QStringLiteral("named_targets"),
        QStringLiteral("poses"),
        QStringLiteral("actions"),
        QStringLiteral("positions"),
        QStringLiteral("arm_named_targets"),
        QStringLiteral("moveit_named_targets"),
    };

    QJsonArray targets;
    for (const QString &field : targetFields) {
        const QJsonValue value = message.value(field);
        if (value.isArray()) {
            targets = value.toArray();
            if (!targets.isEmpty()) {
                break;
            }
        }
    }

    m_armActionList->clear();

    for (const QJsonValue &value : targets) {
        QString name;
        QString description;
        if (value.isString()) {
            name = value.toString();
        } else if (value.isObject()) {
            const QJsonObject object = value.toObject();
            name = object.value(QStringLiteral("name")).toString();
            if (name.isEmpty()) {
                name = object.value(QStringLiteral("target")).toString();
            }
            if (name.isEmpty()) {
                name = object.value(QStringLiteral("id")).toString();
            }
            if (name.isEmpty()) {
                name = object.value(QStringLiteral("label")).toString();
            }
            description = object.value(QStringLiteral("description")).toString();
            if (description.isEmpty()) {
                description = object.value(QStringLiteral("display_name")).toString();
            }
        }

        if (name.isEmpty()) {
            continue;
        }

        auto *item = new QListWidgetItem(description.isEmpty()
                                             ? name
                                             : QString("%1  -  %2").arg(name, description));
        item->setData(Qt::UserRole, name);
        m_armActionList->addItem(item);
    }

    if (m_armActionList->count() == 0) {
        const QString messageText = message.value(QStringLiteral("message")).toString();
        auto *emptyItem = new QListWidgetItem(messageText.isEmpty()
                                                 ? QStringLiteral("下位机未返回可用固定位姿")
                                                 : messageText);
        emptyItem->setFlags(Qt::NoItemFlags);
        m_armActionList->addItem(emptyItem);
        m_executeArmActionButton->setEnabled(false);
        addError(messageText.isEmpty()
                     ? QStringLiteral("[机械臂] 固定位姿列表为空")
                     : QString("[机械臂] %1").arg(messageText));
        return;
    }

    m_armActionList->setCurrentRow(0);
    addCommand(QString("[机械臂] 已更新固定位姿列表: %1 个").arg(m_armActionList->count()));
}

void MainWindow::setupKeyboardController()
{
    m_keyboardController = new KeyboardController(this);
    m_keyboardController->setCaptureWidget(this);
    connect(m_keyboardController, &KeyboardController::captureStateChanged, this, [this](bool active) {
        if (m_controlPanel) m_controlPanel->setKeyboardCaptureActive(active);
        if (!active && m_controller) m_controller->setOperatorControlEnabled(false);
        if (!active && m_inputSource == Communication::OperatorInputState::Source::Keyboard)
            statusBar()->showMessage(QStringLiteral("键盘输入暂停：正在编辑或切换窗口；完成后点击“返回键盘控制”，归中并重新授权。"), 5000);
    });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) return;
        if (m_controller) m_controller->setOperatorControlEnabled(false);
        m_keyboardController->clearPressedKeys();
        clearOperatorInputSnapshot();
    });
    connect(m_keyboardController, &KeyboardController::operatorInputChanged,
            this, [this](const QStringList &pressedKeys) {
        static qint64 lastLogTime = 0;
        const bool changed = (pressedKeys != m_keyboardPressedKeys);
        m_keyboardPressedKeys = pressedKeys;
        if (m_controlPanel) m_controlPanel->setKeyboardKeys(pressedKeys);

        if (changed) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - lastLogTime >= 500) {
                lastLogTime = now;
                if (!pressedKeys.isEmpty()) {
                    addCommand(QString("[键盘] 按下: %1").arg(pressedKeys.join(",")));
                } else {
                    addCommand("[键盘] 停止");
                }
            }
        }

        sendOperatorInputSnapshot();
    });
    connect(m_keyboardController, &KeyboardController::emergencyStopRequested,
            this, [this]() {
        triggerEmergencyStop(QStringLiteral("keyboard_space"));
    });

    // 默认启用键盘控制
    m_keyboardController->setEnabled(true);

}

void MainWindow::setupHandleKey()
{
    m_handleKey = new HandleKey(this);
    connect(m_handleKey, &HandleKey::getHandleKey,
            this, &MainWindow::onGamepadStateReceived);
    connect(m_handleKey, &HandleKey::connectionChanged,
            this, [this](bool connected) {
        m_gamepadConnected = connected;
        if (!connected) {
            m_latestGamepadState = {};
            m_gamepadStickEmergencyHeld = false;
        }
        updateGamepadDisplay();
        if (connected) {
            addCommand("[手柄] 手柄已连接");
        } else {
            addCommand("[手柄] 手柄已断开");
        }
        sendOperatorInputSnapshot();
    });
}

void MainWindow::setupTimers()
{
    m_statusTimer = new QTimer(this);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateSystemStatus);
    m_statusTimer->start(1000); // 每秒更新一次状态
}

void MainWindow::setupConnections()
{
    // 按钮连接已在UI文件和组件初始化中处理，这里保留给跨区域连接扩展。
}

void MainWindow::setupStatusBar()
{
    statusBar()->showMessage("系统就绪");
    m_connectionPhaseLabel = new QLabel(QStringLiteral("控制链路：未连接"), this);
    m_connectionPhaseLabel->setObjectName("connectionPhaseLabel");
    statusBar()->addPermanentWidget(m_connectionPhaseLabel, 1);
    m_controlPermissionLabel = new QLabel(QStringLiteral("运行许可：未知；桥接急停：未知"), this);
    m_controlPermissionLabel->setObjectName("controlPermissionLabel");
    statusBar()->addPermanentWidget(m_controlPermissionLabel, 1);
    m_operationsControlButton = new QPushButton(QStringLiteral("允许操作"), this);
    m_operationsControlButton->setToolTip(QStringLiteral("中立输入后显式授权初始化、使能、恢复和模式切换；不会授予运动权限。"));
    statusBar()->addPermanentWidget(m_operationsControlButton);
    connect(m_operationsControlButton, &QPushButton::clicked, this, [this] {
        const bool sent = m_operationsAuthorized ? m_controller->setOperatorControlEnabled(false) : m_controller->authorizeOperations();
        if (!sent) addError(QStringLiteral("[授权] 请确认控制v2已就绪、桥接急停已清除、按键释放及摇杆归中，并等待中立输入确认。"));
    });
    m_operatorControlButton = new QPushButton(QStringLiteral("允许遥控"), this);
    m_operatorControlButton->setToolTip(QStringLiteral("仅在心跳正常、状态同步、急停已解除且输入中立时允许遥控；不替代下位机运行授权"));
    statusBar()->addPermanentWidget(m_operatorControlButton);
    connect(m_operatorControlButton, &QPushButton::clicked, this, [this]() {
        if (!m_controller || !m_controller->setOperatorControlEnabled(!m_operatorControlEnabled)) {
            addError(QStringLiteral("[遥控] 未满足授权条件：请确认连接、心跳和状态同步，解除急停，释放按键并将摇杆归中"));
        }
    });
}

void MainWindow::updateConnectionStatus(bool connected)
{
    m_isConnected = connected;
    m_lastOperatorInputStatus.clear();
    if (!connected) {
        m_keyboardPressedKeys.clear();
        if (m_keyboardController) m_keyboardController->clearPressedKeys();
        m_latestGamepadState = {};
    }
    updateConnectionDisplay();

    if (connected) {
        statusBar()->showMessage("设备已连接");
        addCommand("[系统] 设备连接成功");
    } else {
        statusBar()->showMessage("设备已断开");
        addCommand("[系统] 设备连接断开");
    }
}

void MainWindow::updateHeartbeatStatus(bool online)
{
    m_heartbeatOnline = online;
    updateHeartbeatDisplay();

    if (!online && m_isConnected) {
        addError("[警告] 下位机心跳丢失");
    }
}

void MainWindow::updateFPS(int fps)
{
    m_currentFPS = fps;
    const QString fpsText = QString("%1 FPS").arg(fps);
    ui->label_fps_value->setText(fpsText);
    if (m_telemetryPanel) {
        m_telemetryPanel->setFps(fps);
    }
}

void MainWindow::updateBandwidthAndPacketLoss()
{
    if (!m_controller || !m_controller->isTcpConnected()) {
        ui->label_cpu->setText("带宽压力:");
        ui->label_cpu_value->setText("N/A");
        if (m_telemetryPanel) {
            m_telemetryPanel->setBandwidthText("N/A");
        }
        return;
    }

    auto stats = m_controller->getTcpStatistics();

    // 计算带宽压力（本周期收发字节数，定时器间隔见调用频率）
    quint64 deltaBytes = (stats.bytesSent - m_lastBytesSent) + (stats.bytesReceived - m_lastBytesReceived);
    m_lastBytesSent = stats.bytesSent;
    m_lastBytesReceived = stats.bytesReceived;

    // 自动选择单位 KB/s 或 MB/s
    QString pressure;
    double bandwidthKBs = deltaBytes / 1024.0;
    if (bandwidthKBs < 1024.0) {
        if (bandwidthKBs < 10.0)
            pressure = QString("%1 KB/s (低)").arg(bandwidthKBs, 0, 'f', 1);
        else if (bandwidthKBs < 100.0)
            pressure = QString("%1 KB/s (中)").arg(bandwidthKBs, 0, 'f', 1);
        else
            pressure = QString("%1 KB/s (高)").arg(bandwidthKBs, 0, 'f', 1);
    } else {
        double bandwidthMBs = bandwidthKBs / 1024.0;
        pressure = QString("%1 MB/s (高)").arg(bandwidthMBs, 0, 'f', 2);
    }

    ui->label_cpu->setText("带宽压力:");
    ui->label_cpu_value->setText(pressure);

    // 计算丢包概率：发送数 - 接收数的��值比例
    quint64 deltaSent = stats.messagesSent - m_lastMessagesSent;
    quint64 deltaReceived = stats.messagesReceived - m_lastMessagesReceived;
    m_lastMessagesSent = stats.messagesSent;
    m_lastMessagesReceived = stats.messagesReceived;

    double lossRate = 0.0;
    if (deltaSent > 0 && deltaSent > deltaReceived) {
        lossRate = (double)(deltaSent - deltaReceived) / deltaSent * 100.0;
    }
    const QString heartbeatRtt = stats.lastHeartbeatRttMs >= 0
                                     ? QString("%1ms").arg(stats.lastHeartbeatRttMs)
                                     : QStringLiteral("N/A");
    const QString bandwidthText = QString("%1 | 丢包: %2% | ACK: %3/%4 | RTT: %5 | 协议错: %6")
                                      .arg(pressure)
                                      .arg(lossRate, 0, 'f', 1)
                                      .arg(stats.ackPendingCount)
                                      .arg(stats.ackTimeoutCount)
                                      .arg(heartbeatRtt)
                                      .arg(stats.protocolErrorCount);
    ui->label_cpu_value->setText(bandwidthText);
    if (m_telemetryPanel) {
        m_telemetryPanel->setBandwidthText(bandwidthText);
    }
}

void MainWindow::updateMotorMode(const QString& mode)
{
    m_motorMode = mode;
    ui->label_mode_value->setText(mode);
    if (m_controlPanel) {
        m_controlPanel->setModeText(mode);
    }
    if (m_telemetryPanel) {
        m_telemetryPanel->setModeText(mode);
    }
    addCommand(QString("[系统] 电机模式切换: %1").arg(mode));
}

void MainWindow::addCommand(const QString& command)
{
    formatAndAddCommand(command);
}

void MainWindow::addError(const QString& error)
{
    m_errorCount++;
    formatAndAddError(error);
    ui->label_error_count->setText(QString("错误: %1").arg(m_errorCount));
    if (m_statusErrorLabel) {
        m_statusErrorLabel->setText(QString("错误: %1").arg(m_errorCount));
    }
    if (m_telemetryPanel) {
        m_telemetryPanel->setErrorCount(m_errorCount);
    }
}

void MainWindow::updateCarAttitude(double roll, double pitch, double yaw)
{
    m_roll = roll;
    m_pitch = pitch;
    m_yaw = yaw;

    if (m_robotAttitudeWidget) {
        m_robotAttitudeWidget->updateAttitude(roll, pitch, yaw);
    }
}

void MainWindow::updateJointsData(const Communication::MotorState& motorState)
{
    // 更新3D视图腿部角度（关节0-3对应4条腿，位置值直接作为角度度数）
    if (m_robotAttitudeWidget) {
        m_robotAttitudeWidget->updateLegs(
            motorState.joints[0].position / 10.0,
            motorState.joints[1].position / 10.0,
            motorState.joints[2].position / 10.0,
            motorState.joints[3].position / 10.0
        );
    }

    // 添加时间戳
    QString timestamp = getCurrentTimestamp();
    QString header = QString("[%1] 6关节数据接收\n").arg(timestamp);

    // 构建关节数据字符串
    QString jointsData = "=== 6关节数据 ===\n";
    for (int i = 0; i < 6; ++i) {
        jointsData += QString("关节 %1:\n").arg(i + 1);
        jointsData += QString("  位置: %1\n").arg(motorState.joints[i].position / 1000.0f, 0, 'f', 3);
        jointsData += QString("  电流: %1 A\n").arg(motorState.joints[i].current / 1000.0f, 0, 'f', 3);
        jointsData += QString("  执行器位置: %1\n").arg(motorState.executor_position / 1000.0f, 0, 'f', 3);
        jointsData += QString("  执行器扭矩: %1\n").arg(motorState.executor_torque / 1000.0f, 0, 'f', 3);
        jointsData += QString("  执行器标志: %1\n").arg(motorState.executor_flags);
        jointsData += QString("  保留字段: %1\n").arg(motorState.reserved);
        if (i < 5) jointsData += "\n";
    }

    // 添加执行器数据摘要
    jointsData += "\n=== 执行器数据摘要 ===\n";
    jointsData += QString("执行器位置: %1\n").arg(motorState.executor_position / 1000.0f, 0, 'f', 3);
    jointsData += QString("执行器扭矩: %1\n").arg(motorState.executor_torque / 1000.0f, 0, 'f', 3);
    jointsData += QString("执行器标志: 0x%1\n").arg(motorState.executor_flags, 2, 16, QChar('0'));
    jointsData += QString("保留字段: %1\n").arg(motorState.reserved);

    if (m_textData) {
        m_textData->setText(header + jointsData);
        QTextCursor cursor = m_textData->textCursor();
        cursor.movePosition(QTextCursor::End);
        m_textData->setTextCursor(cursor);
        m_textData->ensureCursorVisible();
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_isConnected) {
        updateConnectionStatus(false);
    }
    event->accept();
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat() && event->key() == Qt::Key_Space) {
        triggerEmergencyStop(QStringLiteral("keyboard_space"));
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat() && event->key() == Qt::Key_Space) {
        event->accept();
        return;
    }

    QMainWindow::keyReleaseEvent(event);
}

// 菜单槽函数
void MainWindow::on_action_connect_triggered()
{
    // 显示TCP连接对话框
    showTcpConnectionDialog();
}

void MainWindow::on_action_tcp_connect_triggered()
{
    showTcpConnectionDialog();
}

void MainWindow::on_action_disconnect_triggered()
{
    if (m_controller && m_controller->isTcpConnected()) {
        m_controller->disconnectFromROS();
        updateConnectionStatus(false);
        addCommand("[操作] TCP连接已断开");
    }
}

void MainWindow::on_action_exit_triggered()
{
    close();
}

void MainWindow::on_action_reset_layout_triggered()
{
    // 姿态归零
    m_roll = 0.0;
    m_pitch = 0.0;
    m_yaw = 0.0;
    if (m_robotAttitudeWidget) {
        m_robotAttitudeWidget->resetView();
    }

    // 刷新所有UI显示（基于当前实际状态）
    updateConnectionDisplay();
    updateHeartbeatDisplay();
    updateGamepadDisplay();
    updateBandwidthAndPacketLoss();

    ui->label_fps_value->setText(QString::number(m_currentFPS) + " FPS");
    ui->label_mode_value->setText(displayNameForControlMode(m_controlMode));
    ui->label_error_count->setText(QString("错误: %1").arg(m_errorCount));
    if (m_telemetryPanel) {
        m_telemetryPanel->setFps(m_currentFPS);
        m_telemetryPanel->setModeText(displayNameForControlMode(m_controlMode));
        m_telemetryPanel->setErrorCount(m_errorCount);
    }
    if (m_controlPanel) {
        m_controlPanel->setModeText(displayNameForControlMode(m_controlMode));
    }

    update();

    addCommand("[系统] 视图已刷新，姿态已归零");
}

void MainWindow::cleanupResources()
{
    // 1. 停止定时器
    if (m_statusTimer) {
        m_statusTimer->stop();
        m_statusTimer->disconnect();
    }

    // 2. 停止手柄轮询
    if (m_handleKey) {
        m_handleKey->stopPolling();
        m_handleKey->disconnect();
    }

    // 3. 禁用并断开键盘控制器
    if (m_keyboardController) {
        m_keyboardController->setEnabled(false);
        m_keyboardController->disconnect();
    }

    // 4. 停止Controller（会自动断开TCP）
    if (m_controller) {
        m_controller->stop();
        m_controller->disconnect();
    }

    // 5. 清空命令和错误显示
    if (ui->text_commands) {
        ui->text_commands->clear();
    }
    if (ui->text_errors) {
        ui->text_errors->clear();
    }

    // 6. 处理待处理的事件，确保UI刷新
    QApplication::processEvents();
}

void MainWindow::on_action_about_triggered()
{
    QMessageBox::about(this, "关于",
        "上位机 v2\n\n"
        );
}

void MainWindow::on_btn_clear_commands_clicked()
{
    ui->text_commands->clear();
    addCommand("[系统] 指令记录已清空");
}

void MainWindow::on_btn_clear_errors_clicked()
{
    ui->text_errors->clear();
    m_errorCount = 0;
    ui->label_error_count->setText("错误: 0");
    if (m_statusErrorLabel) {
        m_statusErrorLabel->setText("错误: 0");
    }
    if (m_telemetryPanel) {
        m_telemetryPanel->setErrorCount(0);
    }
    addCommand("[系统] 错误记录已清空");
}

void MainWindow::on_btn_emergency_stop_clicked()
{
    triggerEmergencyStop(QStringLiteral("button"));
}

void MainWindow::on_btn_clear_emergency_clicked()
{
    if (QMessageBox::question(this, QStringLiteral("清除桥接急停锁存"), QStringLiteral("确认急停原因已经处理？此步骤仅清除桥接锁存，C++ 全局禁止仍保持；随后需单独允许操作并显式复位。"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    addCommand("[急停] 请求清除桥接锁存（保留 C++ 全局禁止）");

    clearOperatorInputSnapshot();
    if (m_keyboardController) {
        m_keyboardController->clearPressedKeys();
    }

    if (m_controller && m_controller->isTcpConnected()) {
        if (m_controller->sendSystemCommand(QStringLiteral("clear_emergency"))) {
            addCommand("[急停] 解除急停指令已发送，等待ACK");
        } else {
            addError("[急停] 解除急停指令发送失败");
        }
    } else {
        addError("[急停] TCP未连接，无法解除下位机急停状态");
    }

    statusBar()->showMessage("已请求解除急停", 3000);
}

void MainWindow::clearOperatorInputSnapshot()
{
    m_keyboardPressedKeys.clear();
    m_latestGamepadState = {};
}

void MainWindow::triggerEmergencyStop(const QString &source)
{
    if (m_emergencyDebounce.isValid() && m_emergencyDebounce.elapsed() < 150) return;
    m_emergencyDebounce.start();

    // === 最高优先级急停处理 ===
    addCommand(QString("[急停] 用户触发急停 source=%1").arg(source));

    // Independent emergency connection works even if the ordinary session/lease has failed.
    if (m_controller) {
        if (m_controller->sendEmergencyStop(source)) {
            addCommand("[急停] TCP急停指令已发送，等待ACK");
        } else {
            addError("[急停] TCP急停指令发送失败");
        }
    } else {
        addError("[急停] TCP未连接，急停未下发到下位机");
    }

    // 2. 清空输入快照，确保急停后不会继续发送旧输入意图
    clearOperatorInputSnapshot();
    if (m_keyboardController) {
        m_keyboardController->clearPressedKeys();
        addCommand("[急停] 键盘控制器已重置");
    }

    sendOperatorInputSnapshot();

    // 3. 状态栏提示
    statusBar()->showMessage("⚠️ 急停已触发！所有运动已停止", 5000);

    // 4. 视觉反馈：按钮闪烁效果
    QPushButton* btn = ui->btn_emergency_stop;
    QString originalStyle = btn->styleSheet();
    btn->setStyleSheet("QPushButton { background-color: #FFFF00; color: black; border: 3px solid #FF0000; }");
    QTimer::singleShot(500, this, [btn, originalStyle]() {
        btn->setStyleSheet(originalStyle);
    });
}

void MainWindow::toggleGamepadConnection()
{
    if (m_handleKey && m_handleKey->isConnected()) {
        // 已连接 → 停止轮询（断开）
        m_handleKey->stopPolling();
        addCommand("[手柄] 手动断开手柄轮询");
        updateGamepadDisplay();
    } else {
        // 未连接 → 启动轮询尝试连接
        if (m_handleKey) {
            m_handleKey->startPolling();
            addCommand("[手柄] 正在尝试连接手柄...");
        }
    }
}

void MainWindow::updateSystemStatus()
{
    // 模拟系统状态更新
    // TODO: 实现真实的CPU/GPU监控
    static int counter = 0;
    counter++;

    // 模拟FPS变化
    if (counter % 3 == 0 && m_isConnected) {
        int simulatedFPS = 25 + (counter % 10);
        updateFPS(simulatedFPS);
    }

    // 每秒更新带宽压力与丢包概率
    updateBandwidthAndPacketLoss();
}

void MainWindow::updateConnectionDisplay()
{
    QString statusText = m_isConnected ? "已连接" : "断开";
    QString styleSheet = m_isConnected ?
        "color: green; font-weight: bold;" :
        "color: red; font-weight: bold;";

    ui->label_connection_value->setText(statusText);
    ui->label_connection_value->setStyleSheet(styleSheet);
    if (m_telemetryPanel) {
        m_telemetryPanel->setConnectionStatus(m_isConnected);
    }
}

void MainWindow::updateHeartbeatDisplay()
{
    QString statusText = m_heartbeatOnline ? "在线" : "丢失";
    QString styleSheet = m_heartbeatOnline ?
        "color: green; font-weight: bold;" :
        "color: red; font-weight: bold;";

    ui->label_heartbeat_value->setText(statusText);
    ui->label_heartbeat_value->setStyleSheet(styleSheet);
    if (m_telemetryPanel) {
        m_telemetryPanel->setHeartbeatStatus(m_heartbeatOnline);
    }
}

void MainWindow::updateGamepadDisplay()
{
    bool connected = m_handleKey && m_handleKey->isConnected();
    if (m_controlPanel) {
        m_controlPanel->setGamepadConnected(connected);
    }
    if (m_telemetryPanel) {
        m_telemetryPanel->setGamepadConnected(connected);
    }
}

void MainWindow::sendOperatorInputSnapshot()
{
    if (!m_controller || !m_controller->isTcpConnected()) {
        return;
    }

    Communication::OperatorInputState inputState;
    inputState.mode = modeNameForProtocol(m_controlMode);
    inputState.ttlMs = 300;
    inputState.source = m_inputSource;
    inputState.keyboard.pressedKeys = m_keyboardController ? m_keyboardController->currentKeys() : QStringList{};
    inputState.gamepad = makeGamepadInputState(m_latestGamepadState, m_handleKey && m_handleKey->isConnected());

    m_controller->sendOperatorInput(inputState);
}

void MainWindow::formatAndAddCommand(const QString& command)
{
    QString timestamp = getCurrentTimestamp();
    QString formattedCommand = QString("[%1] %2").arg(timestamp, command);

    QTextCursor cursor = ui->text_commands->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(formattedCommand + "\n");

    // 自动滚动到底部
    if (ui->checkbox_auto_scroll->isChecked()) {
        ui->text_commands->setTextCursor(cursor);
        ui->text_commands->ensureCursorVisible();
    }

    // 限制显示行数，避免内存占用过多
    QStringList lines = ui->text_commands->toPlainText().split('\n');
    if (lines.size() > 1000) {
        lines = lines.mid(-500); // 保留最近500行
        ui->text_commands->setText(lines.join('\n'));
    }
}

void MainWindow::formatAndAddError(const QString& error)
{
    QString timestamp = getCurrentTimestamp();
    QString formattedError = QString("[%1] %2").arg(timestamp, error);

    QTextCursor cursor = ui->text_errors->textCursor();
    cursor.movePosition(QTextCursor::End);

    // 设置错误文本颜色为红色
    QTextCharFormat format;
    format.setForeground(QColor(200, 0, 0));
    cursor.setCharFormat(format);
    cursor.insertText(formattedError + "\n");

    // 自动滚动到底部
    ui->text_errors->setTextCursor(cursor);
    ui->text_errors->ensureCursorVisible();

    // 限制显示行数
    QStringList lines = ui->text_errors->toPlainText().split('\n');
    if (lines.size() > 500) {
        lines = lines.mid(-200); // 保留最近200行错误
        ui->text_errors->setText(lines.join('\n'));
    }
}

QString MainWindow::getCurrentTimestamp() const
{
    return QDateTime::currentDateTime().toString("hh:mm:ss");
}

void MainWindow::applyControlMode(ControlMode mode)
{
    m_controlMode = mode;
    const QString modeText = displayNameForControlMode(mode);
    ui->label_mode_value->setText(modeText);
    if (m_controlPanel) {
        m_controlPanel->setModeText(modeText);
        const auto diagnostics = m_controller->controlDiagnostics();
        m_controlPanel->setKeyboardContext(modeNameForProtocol(mode), diagnostics["selected_presets"].toArray(),
            diagnostics["negotiated"].toBool() && diagnostics["motion_presets_supported"].toBool());
    }
    if (m_telemetryPanel) {
        m_telemetryPanel->setModeText(modeText);
    }
}

void MainWindow::showTcpConnectionDialog()
{
    TcpConnectionDialog::showDialog(
        this,
        m_controller,
        [this](const QString &msg) { addCommand(msg); },
        [this](const QString &err) { addError(err); });
}


// ==================== Controller信号处理 ====================

void MainWindow::onTcpConnected()
{
    updateConnectionStatus(true);
    addCommand("[TCP] 已连接到ROS节点");
    sendOperatorInputSnapshot();
    if (m_controller) {
        statusBar()->showMessage(QString("TCP已连接: %1:%2")
            .arg(m_controller->getROSHost()).arg(m_controller->getROSPort()));
    }
}

void MainWindow::onTcpDisconnected()
{
    updateConnectionStatus(false);
    addCommand("[TCP] 与ROS节点断开连接");
    statusBar()->showMessage("TCP连接断开");
    if (m_robotAttitudeWidget) m_robotAttitudeWidget->setDataFresh(false);
}

void MainWindow::onTcpError(const QString &error)
{
    addError(QString("[TCP] %1").arg(error));
}

void MainWindow::onTcpHeartbeatChanged(bool online)
{
    updateHeartbeatStatus(online);
}

void MainWindow::onMotorStateReceived(const Communication::MotorState &state)
{
    updateJointsData(state);
}

void MainWindow::onJointRuntimeStatesReceived(const Communication::JointRuntimeStateList &states)
{
    if (m_motorRuntimeWidget) {
        m_motorRuntimeWidget->setRuntimeStates(states);
    }
}

void MainWindow::onCO2DataReceived(float ppm)
{
    QString color = QStringLiteral("#15803d");
    if (ppm >= 1500.0f) {
        color = QStringLiteral("#b91c1c");
    } else if (ppm >= 800.0f) {
        color = QStringLiteral("#c2410c");
    }

    ui->label_co2_value->setText(QString("%1 ppm").arg(ppm, 0, 'f', 0));
    ui->label_co2_value->setStyleSheet(
        QString("color: white; background: %1; border: 1px solid %1; "
                "border-radius: 10px; padding: 4px 12px; font-size: 15px; font-weight: 800;")
            .arg(color));

    if (m_telemetryPanel) {
        m_telemetryPanel->setCO2Value(ppm);
    }
}

void MainWindow::onIMUDataReceived(float roll, float pitch, float yaw,
                                    float accelX, float accelY, float accelZ)
{
    Q_UNUSED(accelX)
    Q_UNUSED(accelY)
    Q_UNUSED(accelZ)
    updateCarAttitude(roll, pitch, yaw);
}

void MainWindow::onCameraInfoReceived(int cameraId, bool online, const QString &codec,
                                      int width, int height, int fps, int bitrate,
                                      const QString &rtspUrl)
{
    if (m_cameraGridWidget) {
        m_cameraGridWidget->setCameraInfo(cameraId, rtspUrl, online, codec, width, height, fps, bitrate);
        addCommand(QString("[TCP] 摄像头%1 %2 %3")
                   .arg(cameraId).arg(online ? "上线" : "离线").arg(rtspUrl));
    }
}

void MainWindow::onProtocolMessageReceived(const QJsonObject &message)
{
    const QString type = message["type"].toString();

    if (type == "connection_state") {
        const QString state = message["state"].toString();
        QString text = state;
        if (state == "disconnected") text = QStringLiteral("未连接");
        else if (state == "connecting") text = QStringLiteral("连接中");
        else if (state == "negotiating") text = QStringLiteral("控制v2协商中");
        else if (state == "syncing") text = QStringLiteral("状态同步中");
        else if (state == "ready") text = QStringLiteral("通信已就绪");
        else if (state == "reconnecting") text = QStringLiteral("等待重连");
        if (m_connectionPhaseLabel) m_connectionPhaseLabel->setText(QStringLiteral("控制链路：") + text);
        addCommand(QStringLiteral("[连接] ") + text);
        return;
    }

    if (type == "control_status") {
        const auto status = message["status"].toObject();
        if (m_controlPermissionLabel) m_controlPermissionLabel->setText(QStringLiteral("运行许可：%1；桥接急停：%2")
            .arg(status["motion_permitted"].toBool() ? QStringLiteral("允许") : QStringLiteral("禁止"),
                 status["emergency_active"].toBool() ? QStringLiteral("锁存") : QStringLiteral("已清除")));
        return;
    }

    // Full operation/notice records are persisted by the operator journal.
    if (type == "control_operation" || type == "control_notice") return;

    if (type == "arm_named_targets"
        || type == "moveit_named_targets"
        || type == "moveit_named_poses"
        || type == "arm_action_list") {
        updateArmNamedTargets(message);
        return;
    }

    if (type == "system_snapshot") {
        const QJsonObject emergency = message["emergency"].toObject();
        const QJsonObject motor = message["motor"].toObject();
        const QJsonObject lastError = message["last_error"].toObject();
        ControlMode snapshotMode = m_controlMode;
        if (parseControlMode(message["control_mode"].toString(), &snapshotMode)) {
            applyControlMode(snapshotMode);
        }
        addCommand(QString("[同步] system_snapshot seq=%1 mode=%2 emergency=%3 motor=%4/%5 last_error=%6:%7")
                   .arg(message["seq"].toVariant().toLongLong())
                   .arg(message["control_mode"].toString("unknown"))
                   .arg(emergency["active"].toBool(false) ? "active" : "clear")
                   .arg(motor["initialized"].toBool(false) ? "init" : "not_init")
                   .arg(motor["enabled"].toBool(false) ? "enabled" : "disabled")
                   .arg(lastError["code"].toInt(0))
                   .arg(lastError["message"].toString()));
        return;
    }

    if (type == "system_status") {
        ControlMode statusMode = m_controlMode;
        if (parseControlMode(message["control_mode"].toString(), &statusMode)) {
            applyControlMode(statusMode);
        }
        if (message.contains("targets")
            || message.contains("named_targets")
            || message.contains("poses")
            || message.contains("actions")
            || message.contains("positions")
            || message.contains("arm_named_targets")
            || message.contains("moveit_named_targets")) {
            updateArmNamedTargets(message);
        }
        if (message.contains("last_operator_input_seq")) {
            const QString inputStatus = message["operator_input_status"].toString();
            if (!inputStatus.isEmpty() && inputStatus != m_lastOperatorInputStatus) {
                m_lastOperatorInputStatus = inputStatus;
                const QString line = QString("[控制输入] %1 seq=%2 watchdog=%3")
                    .arg(inputStatus)
                    .arg(message["last_operator_input_seq"].toVariant().toLongLong())
                    .arg(message["watchdog_active"].toBool() ? "active" : "clear");
                if (inputStatus == QStringLiteral("accepted")) addCommand(line);
                else addError(line);
            }
            return;
        }
        addCommand(QString("[状态] system_status seq=%1")
                   .arg(message["seq"].toVariant().toLongLong()));
        return;
    }

    const auto formatted = formatProtocolLogMessage(message);
    if (formatted.kind == FormattedProtocolLog::Kind::Command) {
        addCommand(formatted.text);
    } else if (formatted.kind == FormattedProtocolLog::Kind::Error) {
        addError(formatted.text);
    }
}

void MainWindow::onGamepadStateReceived(const ControllerState &state)
{
    m_latestGamepadState = state;
    m_gamepadConnected = true;

    // === 1. 更新 GamepadDisplayWidget 显示 ===
    if (m_controlPanel) {
        // 摇杆归一化到 -1.0 ~ 1.0
        float lx = state.sThumbLX / 32767.0f;
        float ly = state.sThumbLY / 32767.0f;
        float rx = state.sThumbRX / 32767.0f;
        float ry = state.sThumbRY / 32767.0f;
        float lt = state.bLeftTrigger / 255.0f;
        float rt = state.bRightTrigger / 255.0f;

        m_controlPanel->updateGamepadAxes(lx, ly, rx, ry, lt, rt);

        // 按钮状态显示
        if (state.buttonA) m_controlPanel->updateGamepadButton("A", true);
        else if (state.buttonB) m_controlPanel->updateGamepadButton("B", true);
        else if (state.buttonX) m_controlPanel->updateGamepadButton("X", true);
        else if (state.buttonY) m_controlPanel->updateGamepadButton("Y", true);
        else if (state.leftShoulder) m_controlPanel->updateGamepadButton("LB", true);
        else if (state.rightShoulder) m_controlPanel->updateGamepadButton("RB", true);
        else if (state.buttonBack) m_controlPanel->updateGamepadButton("Back", true);
        else if (state.buttonStart) m_controlPanel->updateGamepadButton("Start", true);
        else if (state.dpadUp) m_controlPanel->updateGamepadButton("DPad↑", true);
        else if (state.dpadDown) m_controlPanel->updateGamepadButton("DPad↓", true);
        else if (state.dpadLeft) m_controlPanel->updateGamepadButton("DPad←", true);
        else if (state.dpadRight) m_controlPanel->updateGamepadButton("DPad→", true);
        else m_controlPanel->updateGamepadButton("--", false);
    }

    const bool stickEmergencyPressed = state.leftThumb && state.rightThumb;
    if (stickEmergencyPressed && !m_gamepadStickEmergencyHeld) {
        m_gamepadStickEmergencyHeld = true;
        addCommand("[手柄] L3+R3 急停!");
        triggerEmergencyStop(QStringLiteral("gamepad_l3_r3"));
        return;
    }
    if (!stickEmergencyPressed) {
        m_gamepadStickEmergencyHeld = false;
    }

    sendOperatorInputSnapshot();
}
