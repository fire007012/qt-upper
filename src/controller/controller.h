
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <memory>
#include "HostProtocol.h"
#include "SharedStructs.h"

namespace Communication {
    class RobotTcpClient;
}
class RobotStateStore;

/**
 * @brief Controller层 - 业务逻辑处理核心
 *
 * 职责：
 * 1. 协调Communication层
 * 2. 处理用户交互逻辑
 * 3. 管理通信状态和数据流转
 * 4. 提供统一的业务接口给UI层
 */
class Controller : public QObject
{
    Q_OBJECT

public:
    explicit Controller(QObject *parent = nullptr);
    ~Controller();

    // 初始化和启动
    bool initialize();
    void start();
    void stop();

    // TCP通信接口
    bool connectTcp(const QString &host, quint16 port);
    void disconnectTcp();
    bool isTcpConnected() const;
    bool setOperatorControlEnabled(bool enabled);
    bool authorizeOperations();
    bool recoverControlRequest(const QString &id);
    QJsonObject controlDiagnostics() const;
    QJsonObject diagnostics() const;
    bool exportDiagnostics(const QString &path);

    // 命令发送接口
    bool sendMotorCommand(const Communication::MotorState &state);
    bool sendOperatorInput(const Communication::OperatorInputState &inputState);
    bool sendControlCommand(const Communication::Command &command);
    bool sendJointControl(int jointId, float position, float velocity);
    bool sendEmergencyStop(const QString &source);
    bool sendSystemCommand(const QString &command, const QJsonObject &params = QJsonObject());
    bool sendEndEffectorControl(float x, float y, float z, float roll, float pitch, float yaw);
    bool requestBridgeSync(const QString &reason);
    bool requestCameraList();

    // TCP连接管理（供UI层对话框使用）
    bool connectToROS(const QString &host, quint16 port, const QString &robotId = QStringLiteral("robot24"));
    bool connectManagedBridge(const QString &host, quint16 port, int protocolVersion, const QString &robotId);
    void disconnectFromROS();
    QString getROSHost() const;
    quint16 getROSPort() const;

    // 获取统计信息
    struct Statistics {
        quint64 messagesSent;
        quint64 messagesReceived;
        quint64 bytesSent;
        quint64 bytesReceived;
        quint64 connectionCount;
        quint64 reconnectCount;
        quint64 ackPendingCount;
        quint64 ackReceivedCount;
        quint64 ackTimeoutCount;
        quint64 protocolErrorCount;
        quint64 heartbeatTimeoutCount;
        qint64 lastHeartbeatRttMs;
        qint64 lastHeartbeatAckMs;
    };
    Statistics getTcpStatistics() const;

signals:
    // TCP状态信号
    void tcpConnected();
    void tcpDisconnected();
    void tcpError(const QString &error);
    void tcpHeartbeatChanged(bool online);
    void operatorControlChanged(bool enabled);
    void operationsAuthorizationChanged(bool enabled);
    void inputSampleRequested();
    void inputResetRequested();
    void telemetryFreshnessChanged(const QString &topic, bool fresh);

    // 数据接收信号
    void motorStateReceived(const Communication::MotorState &state);
    void jointRuntimeStatesReceived(const Communication::JointRuntimeStateList &states);
    void co2DataReceived(float ppm);
    void imuDataReceived(float roll, float pitch, float yaw,
                         float accelX, float accelY, float accelZ);
    void cameraInfoReceived(int cameraId, bool online, const QString &codec,
                           int width, int height, int fps, int bitrate,
                           const QString &rtspUrl);
    void protocolMessageReceived(const QJsonObject &message);

    // 系统状态信号
    void systemError(const QString &error);
    void operationCompleted(const QString &operation);

private slots:
    // TCP数据处理
    void onTcpConnected();
    void onTcpDisconnected();
    void onTcpError(const QString &error);
    void onTcpMotorStateReceived(const Communication::MotorState &state);
    void onTcpJointRuntimeStatesReceived(const Communication::JointRuntimeStateList &states);
    void onTcpCO2DataReceived(float ppm);
    void onTcpIMUDataReceived(float roll, float pitch, float yaw,
                              float accelX, float accelY, float accelZ);
    void onTcpCameraInfoReceived(int cameraId, const QString &rtspUrl, bool online,
                                 const QString &codec, int width, int height, int fps, int bitrate);
    void onTcpSystemStatusReceived(const QJsonObject &status);

private:
    // 内部辅助方法
    void setupConnections();
    void handleError(const QString &error);

private:
    // 通信组件
    Communication::RobotTcpClient* m_tcpClient;
    RobotStateStore *m_stateStore;

    // 状态变量
    bool m_initialized;
    bool m_running;
};

#endif // CONTROLLER_H
