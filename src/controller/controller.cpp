#include "controller.h"
#include "RobotTcpClient.h"
#include "../state/RobotStateStore.h"
#include <QJsonArray>
#include <QDebug>

Controller::Controller(QObject *parent)
    : QObject(parent)
    , m_tcpClient(nullptr)
    , m_stateStore(new RobotStateStore(this))
    , m_initialized(false)
    , m_running(false)
{
    connect(m_stateStore, &RobotStateStore::freshnessChanged, this, &Controller::telemetryFreshnessChanged);
}

Controller::~Controller()
{
    stop();

    delete m_tcpClient;
}

bool Controller::initialize()
{
    if (m_initialized) {
        return true;
    }

    // 初始化TCP客户端
    m_tcpClient = new Communication::RobotTcpClient(this);

    // 设置信号连接
    setupConnections();

    m_initialized = true;
    qDebug() << "Controller initialized successfully";
    return true;
}

void Controller::start()
{
    if (!m_initialized) {
        qWarning() << "Controller not initialized";
        return;
    }

    m_running = true;
    qDebug() << "Controller started";
}

void Controller::stop()
{
    m_running = false;

    disconnectTcp();

    qDebug() << "Controller stopped";
}

// ==================== TCP通信接口 ====================

bool Controller::connectTcp(const QString &host, quint16 port)
{
    if (!m_tcpClient) {
        qCritical() << "RobotTcpClient not initialized";
        return false;
    }

    return connectToROS(host, port);
}

void Controller::disconnectTcp()
{
    if (m_tcpClient) {
        m_tcpClient->disconnectFromROS();
    }
}

bool Controller::isTcpConnected() const
{
    return m_tcpClient && m_tcpClient->isConnected();
}

bool Controller::setOperatorControlEnabled(bool enabled)
{
    return m_tcpClient && m_tcpClient->setOperatorControlEnabled(enabled);
}

bool Controller::setPendingPresets(const QJsonArray &ids) { return m_tcpClient && m_tcpClient->setPendingPresets(ids); }
bool Controller::startPresets(const QJsonArray &ids) { return m_tcpClient && m_tcpClient->startPresets(ids); }
bool Controller::haltPresets(const QJsonArray &ids) { return m_tcpClient && m_tcpClient->haltPresets(ids); }
bool Controller::selectPresets(const QJsonArray &ids) { return m_tcpClient && m_tcpClient->selectPresets(ids); }
bool Controller::authorizeOperations() { return m_tcpClient && m_tcpClient->authorizeOperations(); }
bool Controller::recoverControlRequest(const QString &id) { return m_tcpClient && m_tcpClient->recoverControlRequest(id); }
QJsonObject Controller::controlDiagnostics() const { return m_tcpClient ? m_tcpClient->controlDiagnostics() : QJsonObject{}; }

QJsonObject Controller::diagnostics() const
{
    const auto s = getTcpStatistics();
    const QJsonObject transport{{"host", getROSHost()}, {"port", getROSPort()}, {"connected", isTcpConnected()},
        {"messages_sent", static_cast<qint64>(s.messagesSent)}, {"messages_received", static_cast<qint64>(s.messagesReceived)},
        {"bytes_sent", static_cast<qint64>(s.bytesSent)}, {"bytes_received", static_cast<qint64>(s.bytesReceived)},
        {"connection_count", static_cast<qint64>(s.connectionCount)}, {"reconnect_count", static_cast<qint64>(s.reconnectCount)},
        {"heartbeat_rtt_ms", s.lastHeartbeatRttMs}, {"heartbeat_timeouts", static_cast<qint64>(s.heartbeatTimeoutCount)},
        {"protocol_errors", static_cast<qint64>(s.protocolErrorCount)}};
    return m_stateStore->diagnosticBundle({{"host_build_version", Communication::ControlProtocol::HostBuildVersion}, {"control", controlDiagnostics()}, {"transport", transport}});
}
bool Controller::exportDiagnostics(const QString &path)
{
#ifdef HOST_REFERENCE_ROOT
    const QString protectedRoot = QString::fromUtf8(HOST_REFERENCE_ROOT);
#else
    const QString protectedRoot;
#endif
    return m_stateStore->exportFile(path, diagnostics()["context"].toObject(), protectedRoot);
}

// ==================== 命令发送接口 ====================

bool Controller::sendMotorCommand(const Communication::MotorState &state)
{
    if (!m_tcpClient || !m_tcpClient->isConnected()) {
        qWarning() << "TCP not connected, cannot send motor command";
        return false;
    }

    return m_tcpClient->sendMotorCommand(state);
}

bool Controller::sendOperatorInput(const Communication::OperatorInputState &inputState)
{
    if (!m_tcpClient || !m_tcpClient->isConnected()) {
        qWarning() << "TCP not connected, cannot send operator input";
        return false;
    }

    return m_tcpClient->sendOperatorInput(inputState);
}

bool Controller::sendControlCommand(const Communication::Command &command)
{
    if (!m_tcpClient || !m_tcpClient->isConnected()) {
        qWarning() << "TCP not connected, cannot send control command";
        return false;
    }

    return m_tcpClient->sendControlCommand(command);
}

bool Controller::sendJointControl(int jointId, float position, float velocity)
{
    if (!m_tcpClient || !m_tcpClient->isConnected()) {
        qWarning() << "TCP not connected, cannot send joint control";
        return false;
    }

    return m_tcpClient->sendJointControl(jointId, position, velocity);
}

bool Controller::sendEmergencyStop(const QString &source)
{
    if (!m_tcpClient) {
        return false;
    }

    return m_tcpClient->sendEmergencyStop(source);
}

bool Controller::sendSystemCommand(const QString &command, const QJsonObject &params)
{
    if (!m_tcpClient || !m_tcpClient->isConnected()) {
        qWarning() << "TCP not connected, cannot send system command";
        return false;
    }

    return m_tcpClient->sendSystemCommand(command, params);
}

bool Controller::sendEndEffectorControl(float x, float y, float z, float roll, float pitch, float yaw)
{
    if (!m_tcpClient || !m_tcpClient->isConnected()) {
        qWarning() << "TCP not connected, cannot send end effector control";
        return false;
    }

    return m_tcpClient->sendEndEffectorControl(x, y, z, roll, pitch, yaw);
}

bool Controller::requestBridgeSync(const QString &reason)
{
    if (!m_tcpClient || !m_tcpClient->isConnected()) {
        qWarning() << "TCP not connected, cannot request bridge sync";
        return false;
    }

    return m_tcpClient->requestBridgeSync(reason);
}

bool Controller::requestCameraList()
{
    if (!m_tcpClient || !m_tcpClient->isConnected()) {
        qWarning() << "TCP not connected, cannot request camera list";
        return false;
    }

    return m_tcpClient->requestCameraList();
}

// ==================== TCP连接管理（供UI层对话框使用） ====================

bool Controller::connectManagedBridge(const QString &host, quint16 port, int protocolVersion, const QString &robotId)
{
    if (!m_tcpClient) return false;
    if (protocolVersion != 2) {
        m_tcpClient->disconnectFromROS();
        emit tcpError(QStringLiteral("管理端点未声明控制v2，请使用匹配的下位机构建。")); return false;
    }
    m_tcpClient->configureControlV2(robotId);
    return m_tcpClient->connectToROS(host, port);
}

bool Controller::connectToROS(const QString &host, quint16 port, const QString &robotId)
{
    if (!m_tcpClient) {
        qCritical() << "RobotTcpClient not initialized";
        return false;
    }

    m_tcpClient->configureControlV2(robotId);
    return m_tcpClient->connectToROS(host, port);
}

void Controller::disconnectFromROS()
{
    if (m_tcpClient) {
        m_tcpClient->disconnectFromROS();
    }
}

QString Controller::getROSHost() const
{
    if (m_tcpClient) {
        return m_tcpClient->getROSHost();
    }
    return QString();
}

quint16 Controller::getROSPort() const
{
    if (m_tcpClient) {
        return m_tcpClient->getROSPort();
    }
    return 0;
}

// ==================== 统计信息 ====================

Controller::Statistics Controller::getTcpStatistics() const
{
    Statistics stats = {};
    stats.lastHeartbeatRttMs = -1;

    if (m_tcpClient) {
        auto tcpStats = m_tcpClient->getStats();
        stats.messagesSent = tcpStats.messagesSent;
        stats.messagesReceived = tcpStats.messagesReceived;
        stats.bytesSent = tcpStats.bytesSent;
        stats.bytesReceived = tcpStats.bytesReceived;
        stats.connectionCount = tcpStats.connectionCount;
        stats.reconnectCount = tcpStats.reconnectCount;
        stats.ackPendingCount = tcpStats.ackPendingCount;
        stats.ackReceivedCount = tcpStats.ackReceivedCount;
        stats.ackTimeoutCount = tcpStats.ackTimeoutCount;
        stats.protocolErrorCount = tcpStats.protocolErrorCount;
        stats.heartbeatTimeoutCount = tcpStats.heartbeatTimeoutCount;
        stats.lastHeartbeatRttMs = tcpStats.lastHeartbeatRttMs;
        stats.lastHeartbeatAckMs = tcpStats.lastHeartbeatAckMs;
    }

    return stats;
}

// ==================== 内部方法 ====================

void Controller::setupConnections()
{
    // TCP信号连接
    if (m_tcpClient) {
        connect(m_tcpClient, &Communication::RobotTcpClient::connectedToROS,
                this, &Controller::onTcpConnected);
        connect(m_tcpClient, &Communication::RobotTcpClient::disconnectedFromROS,
                this, &Controller::onTcpDisconnected);
        connect(m_tcpClient, &Communication::RobotTcpClient::connectionError,
                this, &Controller::onTcpError);
        connect(m_tcpClient, &Communication::RobotTcpClient::heartbeatChanged,
                this, &Controller::tcpHeartbeatChanged);
        connect(m_tcpClient, &Communication::RobotTcpClient::operatorControlChanged,
                this, &Controller::operatorControlChanged);
        connect(m_tcpClient, &Communication::RobotTcpClient::operationsAuthorizationChanged, this, &Controller::operationsAuthorizationChanged);
        connect(m_tcpClient, &Communication::RobotTcpClient::inputSampleRequested, this, &Controller::inputSampleRequested);
        connect(m_tcpClient, &Communication::RobotTcpClient::inputResetRequested, this, &Controller::inputResetRequested);
        connect(m_tcpClient, &Communication::RobotTcpClient::motorStateReceived,
                this, &Controller::onTcpMotorStateReceived);
        connect(m_tcpClient, &Communication::RobotTcpClient::jointRuntimeStatesReceived,
                this, &Controller::onTcpJointRuntimeStatesReceived);
        connect(m_tcpClient, &Communication::RobotTcpClient::co2DataReceived,
                this, &Controller::onTcpCO2DataReceived);
        connect(m_tcpClient, &Communication::RobotTcpClient::imuDataReceived,
                this, &Controller::onTcpIMUDataReceived);
        connect(m_tcpClient, &Communication::RobotTcpClient::cameraInfoReceived,
                this, &Controller::onTcpCameraInfoReceived);
        connect(m_tcpClient, &Communication::RobotTcpClient::systemStatusReceived,
                this, &Controller::onTcpSystemStatusReceived);
    }

    qDebug() << "Signal connections established";
}

void Controller::onTcpConnected()
{
    qDebug() << "TCP connected";
    emit tcpConnected();
}

void Controller::onTcpDisconnected()
{
    qDebug() << "TCP disconnected";
    emit tcpDisconnected();
}

void Controller::onTcpError(const QString &error)
{
    qCritical() << QString("TCP error: %1").arg(error);
    emit tcpError(error);
}

void Controller::onTcpMotorStateReceived(const Communication::MotorState &state)
{
    emit motorStateReceived(state);
}

void Controller::onTcpJointRuntimeStatesReceived(const Communication::JointRuntimeStateList &states)
{
    QJsonArray values;
    for (const auto &s : states) values.append(QJsonObject{{"joint_name", s.jointName}, {"backend", s.backend}, {"lifecycle_state", s.lifecycleState},
        {"online", s.online}, {"enabled", s.enabled}, {"fault", s.fault}});
    m_stateStore->observe("joints", values, 3000);
    emit jointRuntimeStatesReceived(states);
}

void Controller::onTcpCO2DataReceived(float ppm)
{
    m_stateStore->observe("co2", ppm, 3000);
    emit co2DataReceived(ppm);
}

void Controller::onTcpIMUDataReceived(float roll, float pitch, float yaw,
                                      float accelX, float accelY, float accelZ)
{
    m_stateStore->observe("imu", QJsonObject{{"roll", roll}, {"pitch", pitch}, {"yaw", yaw}, {"accel_x", accelX}, {"accel_y", accelY}, {"accel_z", accelZ}}, 3000);
    emit imuDataReceived(roll, pitch, yaw, accelX, accelY, accelZ);
}

void Controller::onTcpCameraInfoReceived(int cameraId, const QString &rtspUrl, bool online,
                                         const QString &codec, int width, int height, int fps, int bitrate)
{
    emit cameraInfoReceived(cameraId, online, codec, width, height, fps, bitrate, rtspUrl);
}

void Controller::onTcpSystemStatusReceived(const QJsonObject &status)
{
    m_stateStore->ingest(status);
    emit protocolMessageReceived(status);
}

void Controller::handleError(const QString &error)
{
    qCritical() << error;
    emit systemError(error);
}
