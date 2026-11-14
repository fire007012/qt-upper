#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>
#include <QDateTime>
#include <QLabel>
#include <QStringList>
#include <QElapsedTimer>
#include <QHash>
#include "src/communication/SharedStructs.h"

#include "src/controller/controller.h"
#include "src/controller/KeyboardController.h"
#include "src/controller/CameraGridWidget.h"
#include "src/controller/ControlPanelWidget.h"
#include "src/controller/handlekey.h"
#include "src/controller/RobotAttitudeWidget.h"
#include "src/controller/TelemetryPanelWidget.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class QTextEdit;
class QTabWidget;
class QListWidget;
class QPushButton;
class MotorRuntimeCarouselWidget;

// 控制模式枚举
enum class ControlMode {
    Vehicle = 0,
    Arm = 2
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 状态更新接口
    void updateConnectionStatus(bool connected);
    void updateHeartbeatStatus(bool online);
    void updateFPS(int fps);
    void updateBandwidthAndPacketLoss();
    void updateMotorMode(const QString& mode);
    void addCommand(const QString& command);
    void addError(const QString& error);
    void updateCarAttitude(double roll, double pitch, double yaw);
    void updateJointsData(const Communication::MotorState& motorState);


protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private slots:
    void on_action_connect_triggered();
    void on_action_tcp_connect_triggered();
    void on_action_disconnect_triggered();
    void on_action_exit_triggered();
    void on_action_reset_layout_triggered();
    void on_action_about_triggered();

    void on_btn_clear_commands_clicked();
    void on_btn_clear_errors_clicked();
    void on_btn_emergency_stop_clicked();
    void on_btn_clear_emergency_clicked();
    void toggleGamepadConnection();

    void updateSystemStatus();  // 定时更新系统状态

private:
    void setupController();
    void setupConnections();
    void setupStatusBar();
    void setupDisplayLayout();
    void setupKeyboardController();
    void setupHandleKey();
    void setupTimers();
    void cleanupResources();
    void updateConnectionDisplay();
    void updateHeartbeatDisplay();
    void updateGamepadDisplay();
    void sendOperatorInputSnapshot();
    void clearOperatorInputSnapshot();
    void formatAndAddCommand(const QString& command);
    void formatAndAddError(const QString& error);
    QString getCurrentTimestamp() const;
    void applyControlMode(ControlMode mode);
    void triggerEmergencyStop(const QString &source);
    void setupBottomActionPanel();
    void handleMonitorCommand(const QString &label, const QString &command);
    void refreshArmNamedTargets();
    void executeSelectedArmNamedTarget();
    void updateArmNamedTargets(const QJsonObject &message);

private slots:
    // Controller层信号处理
    void onTcpConnected();
    void onTcpDisconnected();
    void onTcpError(const QString &error);
    void onTcpHeartbeatChanged(bool online);
    void onMotorStateReceived(const Communication::MotorState &state);
    void onJointRuntimeStatesReceived(const Communication::JointRuntimeStateList &states);
    void onCO2DataReceived(float ppm);
    void onIMUDataReceived(float roll, float pitch, float yaw, float accelX, float accelY, float accelZ);
    void onCameraInfoReceived(int cameraId, bool online, const QString &codec,
                              int width, int height, int fps, int bitrate,
                              const QString &rtspUrl);
    void onProtocolMessageReceived(const QJsonObject &message);

    // 手柄信号处理
    void onGamepadStateReceived(const ControllerState &state);

    // UI交互
    void showTcpConnectionDialog();

private:
    Ui::MainWindow *ui;

    // 业务逻辑控制器
    Controller* m_controller;

    // 键盘控制器
    KeyboardController* m_keyboardController;

    // 摄像头网格
    CameraGridWidget* m_cameraGridWidget;

    // 遥测状态面板
    TelemetryPanelWidget* m_telemetryPanel;

    // 控制面板
    ControlPanelWidget* m_controlPanel;
    MotorRuntimeCarouselWidget* m_motorRuntimeWidget = nullptr;
    QTabWidget* m_logTabs = nullptr;
    QListWidget* m_armActionList = nullptr;
    QPushButton* m_executeArmActionButton = nullptr;
    QPushButton* m_refreshArmActionButton = nullptr;
    QPushButton* m_operatorControlButton = nullptr;
    QPushButton* m_operationsControlButton = nullptr;
    QLabel* m_controlPermissionLabel = nullptr;
    bool m_operationsAuthorized = false;
    QElapsedTimer m_emergencyDebounce;
    QLabel* m_connectionPhaseLabel = nullptr;
    bool m_operatorControlEnabled = false;
    QString m_lastOperatorInputStatus;

    // 手柄输入驱动
    HandleKey* m_handleKey;

    // 3D机器人姿态视图
    RobotAttitudeWidget* m_robotAttitudeWidget = nullptr;

    // 数据显示
    QTextEdit* m_textData = nullptr;
    QLabel* m_statusErrorLabel = nullptr;

    // 状态管理
    bool m_isConnected = false;
    bool m_heartbeatOnline = false;
    int m_currentFPS = 0;
    // 带宽与丢包统计
    quint64 m_lastBytesSent = 0;
    quint64 m_lastBytesReceived = 0;
    quint64 m_lastMessagesSent = 0;
    quint64 m_lastMessagesReceived = 0;
    QString m_motorMode = "待机";
    int m_errorCount = 0;
    qint64 m_lastEmergencyStopMs = 0;

    // 协议兼容字段；普通输入语意由下位机解析
    ControlMode m_controlMode = ControlMode::Vehicle;
    QStringList m_keyboardPressedKeys;
    ControllerState m_latestGamepadState = {};
    bool m_gamepadConnected = false;
    bool m_gamepadStickEmergencyHeld = false;

    // 定时器
    QTimer* m_statusTimer;

    // 姿态数据
    double m_roll = 0.0;
    double m_pitch = 0.0;
    double m_yaw = 0.0;
};

#endif // MAINWINDOW_H
