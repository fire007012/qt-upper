#include "RobotTcpClient.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <cmath>

static const QString MODULE = "RobotTcpClient";

namespace Communication {

RobotTcpClient::RobotTcpClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_port(9090)
    , m_heartbeatTimer(new QTimer(this))
    , m_reconnectTimer(new QTimer(this))
    , m_ackTimer(new QTimer(this))
    , m_connectTimer(new QTimer(this))
    , m_isConnected(false)
    , m_autoReconnect(true)
    , m_manualDisconnectRequested(false)
    , m_heartbeatOnline(false)
    , m_reconnectAttempts(0)
    , m_lastMessageReceivedMs(0)
    , m_lastHeartbeatAckMs(-1)
    , m_nextSequence(0)
    , m_session(this)
    , m_emergencyChannel(this)
{
    m_clock.start();
    // 初始化统计信息
    m_stats.messagesSent = 0;
    m_stats.messagesReceived = 0;
    m_stats.bytesSent = 0;
    m_stats.bytesReceived = 0;
    m_stats.connectionCount = 0;
    m_stats.reconnectCount = 0;
    m_stats.ackPendingCount = 0;
    m_stats.ackReceivedCount = 0;
    m_stats.ackTimeoutCount = 0;
    m_stats.protocolErrorCount = 0;
    m_stats.heartbeatTimeoutCount = 0;
    m_stats.lastHeartbeatRttMs = -1;
    m_stats.lastHeartbeatAckMs = 0;

    setupConnection();

    connect(&m_session, &ControlSession::outbound, this, [this](const QJsonObject &message) { sendMessage(message); });
    connect(&m_session, &ControlSession::fault, this, [this](const QString &message, bool retry) {
        if (retry) recoverConnection(message);
        else { disconnectFromROS(); emit connectionError(message); }
    });
    connect(&m_session, &ControlSession::event, this, &RobotTcpClient::systemStatusReceived);
    connect(&m_session, &ControlSession::changed, this, [this] {
        if (!m_controlV2 || !isConnected()) return;
        m_stateSynchronized = m_session.ready();
        setConnectionState(!m_session.negotiated() ? ConnectionState::Negotiating
            : m_session.ready() && m_heartbeatOnline ? ConnectionState::Ready : ConnectionState::Syncing);
    });
    connect(&m_session, &ControlSession::motionAuthorizationChanged, this, [this](bool enabled) {
        m_operatorControlEnabled = enabled; emit operatorControlChanged(enabled);
    });
    connect(&m_session, &ControlSession::operationsAuthorizationChanged, this, &RobotTcpClient::operationsAuthorizationChanged);
    connect(&m_session, &ControlSession::inputSampleRequested, this, &RobotTcpClient::inputSampleRequested);
    connect(&m_session, &ControlSession::inputResetRequested, this, &RobotTcpClient::inputResetRequested);
    connect(&m_session, &ControlSession::stateRefreshRequested, this, [this] { requestBridgeSync("control_operation_reconcile"); });
    connect(&m_session, &ControlSession::businessResult, this, [this](const QJsonObject &message) {
        processLegacyMessage(message, m_clock.elapsed());
    });
    connect(&m_emergencyChannel, &EmergencyChannel::event, this, &RobotTcpClient::systemStatusReceived);

    LOG_INFO(MODULE, "TCP客户端已创建");
}

RobotTcpClient::~RobotTcpClient()
{
    disconnectFromROS();
    LOG_INFO(MODULE, "TCP客户端已销毁");
}

void RobotTcpClient::setupConnection()
{
    bindSocketSignals();

    // 设置心跳定时器
    m_heartbeatTimer->setInterval(HEARTBEAT_INTERVAL_MS);
    m_heartbeatTimer->setTimerType(Qt::PreciseTimer);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &RobotTcpClient::checkConnection);

    // 设置重连定时器
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, [this]() {
        if (m_autoReconnect && !m_manualDisconnectRequested && !m_isConnected) {
            // 饱和计数避免长时间离线后溢出；计数只用于退避，不限制重试次数。
            m_reconnectAttempts = qMin(m_reconnectAttempts + 1, 30);
            beginConnectionAttempt();
        }
    });

    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, [this]() {
        recoverConnection(QStringLiteral("TCP connection attempt timed out"));
    });

    m_ackTimer->setInterval(ACK_CHECK_INTERVAL_MS);
    connect(m_ackTimer, &QTimer::timeout, this, &RobotTcpClient::checkAckTimeouts);
}

void RobotTcpClient::bindSocketSignals()
{
    // 每次尝试使用独立 socket；旧连接的延迟回调不能改变新连接。
    auto *socket = m_socket;
    // 连接socket信号
    connect(socket, &QTcpSocket::connected, this, [this, socket]() {
        if (socket == m_socket) handleConnected();
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        if (socket == m_socket) handleDisconnected();
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        if (socket == m_socket) handleReadyRead();
    });
    connect(socket, &QAbstractSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError error) {
        if (socket == m_socket) handleError(error);
    });
}

bool RobotTcpClient::connectToROS(const QString &hostAddress, quint16 port)
{
    if (hostAddress.trimmed().isEmpty() || port == 0) {
        emit connectionError(QStringLiteral("Invalid bridge address or port"));
        return false;
    }
    if (!m_manualDisconnectRequested && hostAddress == m_hostAddress && port == m_port
        && m_socket->state() != QAbstractSocket::UnconnectedState) {
        return true;
    }

    disconnectFromROS();
    m_hostAddress = hostAddress;
    m_port = port;
    m_reconnectAttempts = 0;
    m_manualDisconnectRequested = false;

    beginConnectionAttempt();
    return true;
}

void RobotTcpClient::beginConnectionAttempt()
{
    if (m_manualDisconnectRequested || m_socket->state() != QAbstractSocket::UnconnectedState) {
        return;
    }
    m_receivedData.clear();
    m_pendingHeartbeats.clear();
    m_socket->disconnect(this);
    m_socket->deleteLater();
    m_socket = new QTcpSocket(this);
    m_socket->setReadBufferSize(MAX_FRAME_BYTES + 1);
    bindSocketSignals();
    LOG_INFO(MODULE, QString("连接桥接: %1:%2 (retry=%3)")
                        .arg(m_hostAddress).arg(m_port).arg(m_reconnectAttempts));
    setConnectionState(ConnectionState::Connecting);
    m_connectTimer->start(CONNECT_TIMEOUT_MS);
    m_socket->connectToHost(m_hostAddress, m_port);
}

void RobotTcpClient::scheduleReconnect()
{
    if (!m_autoReconnect || m_manualDisconnectRequested || m_hostAddress.isEmpty()
        || m_isConnected || m_reconnectTimer->isActive()
        || m_socket->state() != QAbstractSocket::UnconnectedState) {
        return;
    }
    const int base = qMin(MAX_RECONNECT_INTERVAL_MS,
                         RECONNECT_INTERVAL_MS * (1 << qMin(m_reconnectAttempts, 2)));
    const int delay = qMin(MAX_RECONNECT_INTERVAL_MS,
                          base + QRandomGenerator::global()->bounded(250));
    m_reconnectTimer->start(delay);
    setConnectionState(ConnectionState::Reconnecting);
    QJsonObject event;
    event["type"] = "connection_recovery";
    event["timestamp_ms"] = HostProtocol::nowMs();
    event["retry_in_ms"] = delay;
    emit systemStatusReceived(event);
}

void RobotTcpClient::recoverConnection(const QString &reason)
{
    LOG_WARNING(MODULE, reason);
    m_socket->disconnect(this);
    m_socket->abort();
    // abort 在 Connecting/HostLookup 状态不保证发出 disconnected。
    handleDisconnected();
    emit connectionError(reason);
}

void RobotTcpClient::disconnectFromROS()
{
    m_manualDisconnectRequested = true;
    m_reconnectTimer->stop();
    m_connectTimer->stop();
    m_socket->disconnect(this);
    m_socket->abort();
    handleDisconnected();
}

bool RobotTcpClient::isConnected() const
{
    return m_isConnected && m_socket->state() == QAbstractSocket::ConnectedState;
}

void RobotTcpClient::configureControlV2(const QString &robotId)
{
    disconnectFromROS();
    m_controlV2 = true; m_expectedRobot = robotId.trimmed(); m_requiredCapabilities.clear();
}

void RobotTcpClient::setConnectionState(ConnectionState state)
{
    if (m_connectionState == state) return;
    m_connectionState = state;
    QString name;
    switch (state) {
    case ConnectionState::Disconnected: name = QStringLiteral("disconnected"); break;
    case ConnectionState::Negotiating: name = QStringLiteral("negotiating"); break;
    case ConnectionState::Connecting: name = QStringLiteral("connecting"); break;
    case ConnectionState::Syncing: name = QStringLiteral("syncing"); break;
    case ConnectionState::Ready: name = QStringLiteral("ready"); break;
    case ConnectionState::Reconnecting: name = QStringLiteral("reconnecting"); break;
    }
    emit systemStatusReceived(QJsonObject{{"type", "connection_state"}, {"state", name},
                                         {"timestamp_ms", HostProtocol::nowMs()}});
}

bool RobotTcpClient::setOperatorControlEnabled(bool enabled)
{
    if (m_controlV2) {
        if (enabled) return m_heartbeatOnline && m_session.authorizeMotion();
        if (m_session.negotiated()) m_session.pause();
        return true;
    }
    if (enabled && (!isConnected() || !m_heartbeatOnline || !m_stateSynchronized || (!m_requiredCapabilities.isEmpty() && !m_capabilitiesVerified) || m_emergencyActive
                    || !m_latestInputNeutral || m_lastInputSampleMs < 0
                    || m_clock.elapsed() - m_lastInputSampleMs > 300)) {
        return false;
    }
    if (m_operatorControlEnabled == enabled) return true;
    m_operatorControlEnabled = enabled;
    if (!enabled) {
        m_latestInputNeutral = false;
        m_lastInputSampleMs = -1;
    }
    emit operatorControlChanged(enabled);
    if (!enabled && isConnected()) {
        // 立即撤销遥控时发送中立快照；断网时由下位机独立 watchdog 停止运动。
        sendMessage(HostProtocol::makeOperatorInput(OperatorInputState{}, nextSequence()));
    }
    return true;
}

bool RobotTcpClient::sendMotorCommand(const MotorState &motorState)
{
    if (m_controlV2) return false;
    if (!m_operatorControlEnabled) return false;
    if (!isConnected()) {
        HANDLE_ERROR(Utils::ErrorCode::NetworkDisconnected, MODULE, "未连接到控制桥，无法发送电机命令");
        return false;
    }

    QJsonObject command;
    command["type"] = "motor_command";
    command["protocol_version"] = HostProtocol::ProtocolVersion;
    command["seq"] = static_cast<qint64>(nextSequence());
    command["timestamp_ms"] = HostProtocol::nowMs();

    // 添加6关节数据
    QJsonArray jointsArray;
    for (int i = 0; i < 6; ++i) {
        QJsonObject joint;
        joint["position"] = motorState.joints[i].position / 1000.0f;
        joint["current"] = motorState.joints[i].current / 1000.0f;
        jointsArray.append(joint);
    }
    command["joints"] = jointsArray;

    // 添加执行器数据
    command["executor_position"] = motorState.executor_position / 1000.0f;
    command["executor_torque"] = motorState.executor_torque / 1000.0f;
    command["executor_flags"] = motorState.executor_flags;
    command["reserved"] = motorState.reserved;

    return sendMessage(command);
}

bool RobotTcpClient::sendOperatorInput(const OperatorInputState &inputState)
{
    if (m_controlV2) return isConnected() && m_session.input(inputState);
    if (!isConnected()) {
        HANDLE_ERROR(Utils::ErrorCode::NetworkDisconnected, MODULE, "未连接到控制桥，无法发送操作者输入状态");
        return false;
    }

    const auto snapshot = HostProtocol::makeOperatorInput(inputState, nextSequence());
    const auto gamepad = snapshot["gamepad"].toObject();
    m_latestInputNeutral = inputState.keyboard.pressedKeys.isEmpty();
    if (inputState.gamepad.connected) {
        const auto buttons = gamepad["buttons"].toObject();
        for (const auto &value : buttons)
            m_latestInputNeutral = m_latestInputNeutral && !value.toBool();
        const auto axes = gamepad["axes"].toObject();
        for (const auto &value : axes)
            m_latestInputNeutral = m_latestInputNeutral && value.isDouble()
                && std::isfinite(value.toDouble()) && std::abs(value.toDouble()) <= 0.04;
    }
    m_lastInputSampleMs = m_clock.elapsed();
    if (!m_heartbeatOnline || !m_stateSynchronized) return false;
    if (!m_operatorControlEnabled) {
        OperatorInputState neutral;
        neutral.mode = inputState.mode;
        return sendMessage(HostProtocol::makeOperatorInput(neutral, nextSequence()));
    }
    return sendMessage(snapshot);
}

bool RobotTcpClient::sendJointControl(int jointId, float position, float velocity)
{
    if (m_controlV2) return false;
    if (!m_operatorControlEnabled) return false;
    if (!isConnected()) {
        HANDLE_ERROR(Utils::ErrorCode::NetworkDisconnected, MODULE, "未连接到控制桥，无法发送关节控制命令");
        return false;
    }

    if (jointId < 0 || jointId >= 6) {
        HANDLE_ERROR_DETAIL(Utils::ErrorCode::InvalidParameter, MODULE,
                            "关节ID无效", QString("jointId=%1，必须在0-5范围内").arg(jointId));
        return false;
    }

    QJsonObject command = HostProtocol::makeCommand(QStringLiteral("joint_control"), nextSequence());
    command["joint_id"] = jointId;
    command["position"] = position;
    command["velocity"] = velocity;

    return sendMessage(command);
}

bool RobotTcpClient::sendEmergencyStop(const QString &source)
{
    if (m_controlV2) {
        m_session.emergencyIntent();
        return m_emergencyChannel.send(m_hostAddress, m_port, source);
    }
    if (!isConnected()) {
        HANDLE_ERROR(Utils::ErrorCode::NetworkDisconnected, MODULE, "未连接到控制桥，无法发送急停命令");
        return false;
    }

    LOG_WARNING(MODULE, "发送急停命令");
    setOperatorControlEnabled(false);

    QJsonObject params;
    params["source"] = source.isEmpty() ? QStringLiteral("upper_computer") : source;
    return sendTrackedMessage(
        HostProtocol::makeCommand(QStringLiteral("emergency_stop"), nextSequence(), params),
        QStringLiteral("emergency_stop"));
}

bool RobotTcpClient::sendSystemCommand(const QString &command, const QJsonObject &params)
{
    if (m_controlV2) return command == "clear_emergency" ? m_session.clearEmergency() : m_session.command(command, params);
    if (command == QStringLiteral("move_arm_named_target") && !m_operatorControlEnabled) return false;
    if (command == QStringLiteral("halt") || command == QStringLiteral("disable")
        || command == QStringLiteral("shutdown") || command == QStringLiteral("recover")) {
        setOperatorControlEnabled(false);
    }
    if (!isConnected()) {
        HANDLE_ERROR(Utils::ErrorCode::NetworkDisconnected, MODULE, "未连接到控制桥，无法发送系统命令");
        return false;
    }

    QJsonObject msg = HostProtocol::makeCommand(QStringLiteral("system_command"), nextSequence());
    msg["command"] = command;
    msg["params"] = params;

    LOG_INFO(MODULE, QString("发送系统命令: %1").arg(command));

    return sendTrackedMessage(msg, QStringLiteral("system_command"));
}

bool RobotTcpClient::sendEndEffectorControl(float x, float y, float z, float roll, float pitch, float yaw)
{
    if (m_controlV2) return false;
    if (!m_operatorControlEnabled) return false;
    if (!isConnected()) {
        HANDLE_ERROR(Utils::ErrorCode::NetworkDisconnected, MODULE, "未连接到控制桥，无法发送末端控制命令");
        return false;
    }

    QJsonObject command = HostProtocol::makeCommand(QStringLiteral("cartesian_control"), nextSequence());
    command["x"] = x;
    command["y"] = y;
    command["z"] = z;
    command["roll"] = roll;
    command["pitch"] = pitch;
    command["yaw"] = yaw;

    return sendMessage(command);
}

bool RobotTcpClient::sendControlCommand(const Command &command)
{
    if (m_controlV2) return false;
    if (!m_operatorControlEnabled) return false;
    if (!isConnected()) {
        HANDLE_ERROR(Utils::ErrorCode::NetworkDisconnected, MODULE, "未连接到控制桥，无法发送控制命令");
        return false;
    }

    QJsonObject msg = HostProtocol::makeCommand(QStringLiteral("control_command"), nextSequence());

    // IMU数据
    QJsonObject imuData;
    imuData["yaw"] = command.imu_yaw;
    imuData["roll"] = command.imu_roll;
    imuData["pitch"] = command.imu_pitch;
    msg["imu"] = imuData;

    // 4个摆臂电流
    QJsonArray swingArmCurrents;
    for (int i = 0; i < 4; ++i) {
        swingArmCurrents.append(command.swing_arm_current[i]);
    }
    msg["swing_arm_current"] = swingArmCurrents;

    // 机械臂末端位置
    QJsonObject armEndPos;
    armEndPos["x"] = command.arm_end_x;
    armEndPos["y"] = command.arm_end_y;
    armEndPos["z"] = command.arm_end_z;
    armEndPos["roll"] = command.arm_end_roll;
    armEndPos["pitch"] = command.arm_end_pitch;
    armEndPos["yaw"] = command.arm_end_yaw;
    msg["arm_end_position"] = armEndPos;

    // 标志位
    msg["command_flags"] = command.command_flags;

    return sendMessage(msg);
}

bool RobotTcpClient::requestBridgeSync(const QString &reason)
{
    if (!isConnected()) {
        HANDLE_ERROR(Utils::ErrorCode::NetworkDisconnected, MODULE, "未连接到控制桥，无法请求状态同步");
        return false;
    }

    m_syncRequestSeq = nextSequence();
    return sendMessage(HostProtocol::makeSyncRequest(m_syncRequestSeq, reason));
}

bool RobotTcpClient::requestCameraList()
{
    if (!isConnected()) {
        HANDLE_ERROR(Utils::ErrorCode::NetworkDisconnected, MODULE, "未连接到控制桥，无法请求摄像头列表");
        return false;
    }

    return sendMessage(HostProtocol::makeCameraListRequest(nextSequence()));
}

QString RobotTcpClient::getConnectionStatus() const
{
    if (isConnected()) {
        return QString("已连接到 %1:%2").arg(m_hostAddress).arg(m_port);
    } else if (m_socket->state() == QAbstractSocket::ConnectingState) {
        return QString("正在连接到 %1:%2...").arg(m_hostAddress).arg(m_port);
    } else {
        return QString("未连接 (上次连接: %1:%2)").arg(m_hostAddress).arg(m_port);
    }
}

QString RobotTcpClient::getROSHost() const
{
    return m_hostAddress;
}

quint16 RobotTcpClient::getROSPort() const
{
    return m_port;
}

RobotTcpClient::Stats RobotTcpClient::getStats() const
{
    Stats stats = m_stats;
    stats.ackPendingCount = static_cast<quint64>(m_pendingAcks.size());
    return stats;
}

void RobotTcpClient::resetStats()
{
    m_stats.messagesSent = 0;
    m_stats.messagesReceived = 0;
    m_stats.bytesSent = 0;
    m_stats.bytesReceived = 0;
    m_stats.connectionCount = 0;
    m_stats.reconnectCount = 0;
    m_stats.ackReceivedCount = 0;
    m_stats.ackTimeoutCount = 0;
    m_stats.protocolErrorCount = 0;
    m_stats.heartbeatTimeoutCount = 0;
    m_stats.lastHeartbeatRttMs = -1;
    // 对外统计仍为墙上时间；内部超时不使用该字段。
    emitStatsUpdate();
    LOG_INFO(MODULE, "统计信息已重置");
}

// ==================== 跨线程槽函数 ====================

void RobotTcpClient::slotConnectToROS(const QString &hostAddress, quint16 port)
{
    connectToROS(hostAddress, port);
}

void RobotTcpClient::slotDisconnectFromROS()
{
    disconnectFromROS();
}

void RobotTcpClient::slotSendMotorCommand(const MotorState &motorState)
{
    sendMotorCommand(motorState);
}

void RobotTcpClient::slotSendJointControl(int jointId, float position, float velocity)
{
    sendJointControl(jointId, position, velocity);
}

void RobotTcpClient::slotSendEmergencyStop()
{
    sendEmergencyStop(QStringLiteral("slot"));
}

void RobotTcpClient::slotSendSystemCommand(const QString &command, const QString &paramsJson)
{
    QJsonDocument doc = QJsonDocument::fromJson(paramsJson.toUtf8());
    QJsonObject params = doc.object();
    sendSystemCommand(command, params);
}

// ==================== 事件处理 ====================

void RobotTcpClient::handleConnected()
{
    m_connectTimer->stop();
    m_isConnected = true;
    m_connectedAtMs = m_clock.elapsed();
    m_lastMessageReceivedMs = m_connectedAtMs;
    m_lastHeartbeatAckMs = -1;
    m_stats.lastHeartbeatAckMs = 0;
    m_stats.lastHeartbeatRttMs = -1;
    setHeartbeatOnline(false);
    m_pendingHeartbeats.clear();
    m_manualDisconnectRequested = false;
    m_reconnectAttempts = 0;
    m_reconnectTimer->stop();
    m_heartbeatTimer->start();
    setConnectionState(m_controlV2 ? ConnectionState::Negotiating : ConnectionState::Syncing);

    m_stats.connectionCount++;

    if (m_stats.connectionCount > 1) {
        m_stats.reconnectCount++;
    }

    LOG_INFO(MODULE, QString("成功连接到控制桥: %1").arg(getConnectionStatus()));
    auto *socket = m_socket;
    m_controlHelloStarted = false; m_capabilityDiscovery = "waiting";
    if (m_controlV2) {
        // The server allows three seconds for hello. Never infer full-vehicle
        // semantics from a missing or delayed capabilities frame.
        QTimer::singleShot(2500, socket, [this, socket] {
            if (socket != m_socket || !isConnected() || m_controlHelloStarted) return;
            m_capabilityDiscovery = "timeout";
            disconnectFromROS();
            emit connectionError(QStringLiteral("等待控制能力声明超时，未建立控制会话；请检查桥接版本、网络及部署日志。"));
        });
    }
    if (socket != m_socket || !isConnected()) return;
    emit connectedToROS();
    if (socket != m_socket || !isConnected()) return;
    requestBridgeState(m_stats.connectionCount > 1 ? QStringLiteral("reconnected")
                                                   : QStringLiteral("connected"));
    emitStatsUpdate();
    checkConnection();
}

void RobotTcpClient::handleDisconnected()
{
    bool wasConnected = m_isConnected;
    m_isConnected = false;
    if (m_controlV2) m_session.reset();
    m_connectTimer->stop();
    m_receivedData.clear();
    m_pendingHeartbeats.clear();
    m_lastHeartbeatAckMs = -1;
    m_connectedAtMs = -1;
    m_stateSynchronized = false;
    m_capabilitiesVerified = false;
    m_emergencyActive = true;
    m_latestInputNeutral = false;
    m_lastInputSampleMs = -1;
    m_syncRequestSeq = 0;
    // m_isConnected 已置 false，不尝试向已失效连接发送中立帧。
    setOperatorControlEnabled(false);
    setHeartbeatOnline(false);
    setConnectionState(ConnectionState::Disconnected);
    m_heartbeatTimer->stop();
    failAllPendingAcks(QStringLiteral("ack_disconnected"),
                       QStringLiteral("connection lost before ACK"), -2);

    if (wasConnected) {
        LOG_WARNING(MODULE, "与控制桥断开连接");
        HANDLE_ERROR(Utils::ErrorCode::NetworkDisconnected, MODULE, "与控制桥断开连接");
        emit disconnectedFromROS();

        if (m_manualDisconnectRequested) {
            LOG_INFO(MODULE, "主动断开连接，不启动自动重连");
        }
    }
    scheduleReconnect();
}

void RobotTcpClient::handleReadyRead()
{
    QByteArray chunk = m_socket->readAll();
    m_stats.bytesReceived += chunk.size();
    m_receivedData.append(chunk);

    if (m_receivedData.size() > MAX_FRAME_BYTES && !m_receivedData.contains('\n')) {
        closeForProtocolError(0, 2001, QStringLiteral("TCP frame exceeds max length before newline"));
        return;
    }

    processReceivedData();
}

void RobotTcpClient::handleError(QAbstractSocket::SocketError error)
{
    Utils::ErrorCode errorCode = Utils::ErrorCode::NetworkConnectionFailed;
    QString errorMsg;

    switch (error) {
    case QAbstractSocket::ConnectionRefusedError:
        errorMsg = "连接被拒绝";
        errorCode = Utils::ErrorCode::NetworkConnectionFailed;
        break;
    case QAbstractSocket::RemoteHostClosedError:
        errorMsg = "远程主机关闭连接";
        errorCode = Utils::ErrorCode::NetworkDisconnected;
        break;
    case QAbstractSocket::HostNotFoundError:
        errorMsg = "找不到主机";
        errorCode = Utils::ErrorCode::NetworkConnectionFailed;
        break;
    case QAbstractSocket::SocketTimeoutError:
        errorMsg = "连接超时";
        errorCode = Utils::ErrorCode::NetworkTimeout;
        break;
    case QAbstractSocket::NetworkError:
        errorMsg = "网络错误";
        errorCode = Utils::ErrorCode::NetworkConnectionFailed;
        break;
    default:
        errorMsg = QString("网络错误: %1").arg(m_socket->errorString());
    }

    HANDLE_ERROR_DETAIL(errorCode, MODULE, errorMsg,
                        QString("host=%1:%2, socketError=%3").arg(m_hostAddress).arg(m_port).arg(error));
    recoverConnection(errorMsg);
}

void RobotTcpClient::checkConnection()
{
    if (!isConnected()) {
        LOG_DEBUG(MODULE, "心跳检测发现连接断开");
        handleDisconnected();
    } else {
        const qint64 now = m_clock.elapsed();
        trimExpiredHeartbeats(now);

        const qint64 reference = m_lastHeartbeatAckMs >= 0 ? m_lastHeartbeatAckMs : m_connectedAtMs;
        if (reference >= 0 && now - reference >= HEARTBEAT_TIMEOUT_MS) {
            m_stats.heartbeatTimeoutCount++;
            recoverConnection(QStringLiteral("Heartbeat ACK timeout; reconnecting bridge"));
            return;
        }
        if ((!m_stateSynchronized || (!m_requiredCapabilities.isEmpty() && !m_capabilitiesVerified)) && now - m_connectedAtMs >= CONNECT_TIMEOUT_MS) {
            recoverConnection(QStringLiteral("Bridge state synchronization timed out"));
            return;
        }

        const quint64 seq = nextSequence();
        if (sendMessage(HostProtocol::makeHeartbeat(seq))) {
            m_pendingHeartbeats.insert(seq, now);
        }
    }
}

void RobotTcpClient::checkAckTimeouts()
{
    if (m_pendingAcks.isEmpty()) {
        m_ackTimer->stop();
        return;
    }

    const qint64 now = m_clock.elapsed();
    QList<PendingAck> expired;
    for (auto it = m_pendingAcks.cbegin(); it != m_pendingAcks.cend(); ++it) {
        if (now >= it.value().deadlineMs) {
            expired.append(it.value());
        }
    }

    for (const PendingAck &pending : expired) {
        if (!m_pendingAcks.remove(pending.seq)) {
            continue;
        }
        m_stats.ackTimeoutCount++;
        failPendingAck(pending, QStringLiteral("ack_timeout"),
                       QStringLiteral("ACK timeout"), -1, now);
    }

    if (m_pendingAcks.isEmpty()) {
        m_ackTimer->stop();
    }

    emitStatsUpdate();
}

bool RobotTcpClient::sendMessage(const QJsonObject &message)
{
    if (!isConnected()) {
        return false;
    }

    QJsonDocument doc(message);
    QByteArray data = doc.toJson(QJsonDocument::Compact) + "\n";

    if (data.size() > MAX_FRAME_BYTES || m_socket->bytesToWrite() + data.size() > MAX_PENDING_SEND_BYTES) {
        recoverConnection(QStringLiteral("TCP send backlog exceeded limit; discarding stale output"));
        return false;
    }

    qint64 bytesWritten = m_socket->write(data);
    if (bytesWritten != data.size()) {
        HANDLE_ERROR_DETAIL(Utils::ErrorCode::NetworkSendFailed, MODULE,
                            "发送消息失败", m_socket->errorString());
        recoverConnection(QStringLiteral("TCP frame write failed"));
        return false;
    }

    m_stats.messagesSent++;
    m_stats.bytesSent += bytesWritten;

    m_socket->flush();
    emitStatsUpdate();

    return bytesWritten == data.size();
}

bool RobotTcpClient::sendTrackedMessage(const QJsonObject &message, const QString &ackType)
{
    if (m_pendingAcks.size() >= MAX_PENDING_ACKS) {
        if (ackType != QStringLiteral("emergency_stop")) {
            emit connectionError(QStringLiteral("Too many pending commands; request not sent"));
            return false;
        }
        // 急停不能被普通请求占满 ACK 表阻挡；移出的请求结果仍视为未知。
        auto it = m_pendingAcks.begin();
        const auto pending = it.value();
        m_pendingAcks.erase(it);
        failPendingAck(pending, QStringLiteral("ack_superseded"),
                       QStringLiteral("Tracking superseded by emergency stop; outcome uncertain"), -6, m_clock.elapsed());
    }
    if (!sendMessage(message)) {
        return false;
    }

    registerPendingAck(message, ackType);
    return true;
}

quint64 RobotTcpClient::nextSequence()
{
    ++m_nextSequence;
    if (m_nextSequence == 0) {
        m_nextSequence = 1;
    }
    return m_nextSequence;
}

void RobotTcpClient::registerPendingAck(const QJsonObject &message, const QString &ackType)
{
    const qint64 seqValue = message["seq"].toVariant().toLongLong();
    if (seqValue <= 0) {
        LOG_WARNING(MODULE, QString("无法跟踪ACK，缺少有效seq: %1").arg(ackType));
        return;
    }

    const qint64 now = m_clock.elapsed();
    PendingAck pending;
    pending.seq = static_cast<quint64>(seqValue);
    pending.ackType = ackType;
    pending.sentAtMs = now;
    pending.deadlineMs = now + ACK_TIMEOUT_MS;
    m_pendingAcks.insert(pending.seq, pending);

    QJsonObject event;
    event["type"] = "ack_pending";
    event["protocol_version"] = HostProtocol::ProtocolVersion;
    event["ack_type"] = ackType;
    event["seq"] = static_cast<qint64>(pending.seq);
    event["timestamp_ms"] = HostProtocol::nowMs();
    event["deadline_ms"] = HostProtocol::nowMs() + ACK_TIMEOUT_MS;
    event["timeout_ms"] = ACK_TIMEOUT_MS;
    emit systemStatusReceived(event);

    if (!m_ackTimer->isActive()) {
        m_ackTimer->start();
    }

    emitStatsUpdate();
}

void RobotTcpClient::handleAckMessage(const QJsonObject &message, qint64 receivedAtMs)
{
    const qint64 seqValue = message["seq"].toVariant().toLongLong();
    const QString ackType = message["ack_type"].toString("unknown");

    if (seqValue <= 0) {
        QJsonObject event;
        event["type"] = "ack_unmatched";
        event["protocol_version"] = HostProtocol::ProtocolVersion;
        event["ack_type"] = ackType;
        event["seq"] = seqValue;
        event["code"] = -3;
        event["message"] = QStringLiteral("ACK missing valid seq");
        event["timestamp_ms"] = HostProtocol::nowMs();
        event["ack"] = message;
        emit systemStatusReceived(event);
        return;
    }

    const quint64 seq = static_cast<quint64>(seqValue);
    auto it = m_pendingAcks.find(seq);
    if (it == m_pendingAcks.end()) {
        QJsonObject event;
        event["type"] = "ack_unmatched";
        event["protocol_version"] = HostProtocol::ProtocolVersion;
        event["ack_type"] = ackType;
        event["seq"] = static_cast<qint64>(seq);
        event["code"] = -4;
        event["message"] = QStringLiteral("ACK has no pending command");
        event["timestamp_ms"] = HostProtocol::nowMs();
        event["ack"] = message;
        emit systemStatusReceived(event);
        return;
    }

    const PendingAck pending = it.value();
    if (ackType != pending.ackType) {
        QJsonObject mismatch;
        mismatch["type"] = "ack_mismatch";
        mismatch["protocol_version"] = HostProtocol::ProtocolVersion;
        mismatch["ack_type"] = ackType;
        mismatch["expected_ack_type"] = pending.ackType;
        mismatch["seq"] = static_cast<qint64>(pending.seq);
        mismatch["code"] = -5;
        mismatch["message"] = QStringLiteral("ACK type does not match pending command");
        mismatch["timestamp_ms"] = HostProtocol::nowMs();
        mismatch["elapsed_ms"] = receivedAtMs - pending.sentAtMs;
        mismatch["ack"] = message;
        emit systemStatusReceived(mismatch);

        emitStatsUpdate();
        return;
    }

    m_pendingAcks.erase(it);
    if (receivedAtMs >= pending.deadlineMs) {
        m_stats.ackTimeoutCount++;
        failPendingAck(pending, QStringLiteral("ack_timeout"),
                       QStringLiteral("ACK arrived after deadline; outcome uncertain"), -1, receivedAtMs);
        emitStatsUpdate();
        return;
    }
    m_stats.ackReceivedCount++;
    QJsonObject resolved = message;
    resolved["elapsed_ms"] = receivedAtMs - pending.sentAtMs;
    emit systemStatusReceived(resolved);

    if (m_pendingAcks.isEmpty()) {
        m_ackTimer->stop();
    }

    emitStatsUpdate();
}

void RobotTcpClient::handleHeartbeatAck(const QJsonObject &message, qint64 receivedAtMs)
{
    const qint64 seqValue = message["seq"].toVariant().toLongLong();
    if (seqValue > 0) {
        const quint64 seq = static_cast<quint64>(seqValue);
        auto it = m_pendingHeartbeats.find(seq);
        if (it != m_pendingHeartbeats.end()) {
            const qint64 rtt = receivedAtMs - it.value();
            m_pendingHeartbeats.erase(it);
            if (rtt >= HEARTBEAT_TIMEOUT_MS) {
                return;
            }
            m_stats.lastHeartbeatRttMs = rtt;
            m_lastHeartbeatAckMs = receivedAtMs;
            m_stats.lastHeartbeatAckMs = HostProtocol::nowMs();
            setHeartbeatOnline(true);
            if (m_stateSynchronized && (!m_controlV2 || m_session.ready()) && (m_requiredCapabilities.isEmpty() || m_capabilitiesVerified)) setConnectionState(ConnectionState::Ready);
        }
    }
}

void RobotTcpClient::failPendingAck(const PendingAck &pending, const QString &eventType,
                                   const QString &message, int code, qint64 nowMs)
{
    QJsonObject event;
    event["type"] = eventType;
    event["protocol_version"] = HostProtocol::ProtocolVersion;
    event["ack_type"] = pending.ackType;
    event["seq"] = static_cast<qint64>(pending.seq);
    event["code"] = code;
    event["message"] = message;
    event["timestamp_ms"] = HostProtocol::nowMs();
    event["elapsed_ms"] = nowMs - pending.sentAtMs;
    event["timeout_ms"] = ACK_TIMEOUT_MS;
    emit systemStatusReceived(event);
}

void RobotTcpClient::failAllPendingAcks(const QString &eventType, const QString &message, int code)
{
    if (m_pendingAcks.isEmpty()) {
        m_ackTimer->stop();
        return;
    }

    const qint64 now = m_clock.elapsed();
    const QList<PendingAck> pendingAcks = m_pendingAcks.values();
    m_pendingAcks.clear();
    m_ackTimer->stop();

    for (const PendingAck &pending : pendingAcks) {
        failPendingAck(pending, eventType, message, code, now);
    }

    emitStatsUpdate();
}

static bool isIntegerJsonValue(const QJsonValue &value)
{
    if (!value.isDouble()) {
        return false;
    }

    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number;
}

static qint64 jsonInteger(const QJsonValue &value)
{
    return value.toVariant().toLongLong();
}

bool RobotTcpClient::validateIncomingMessage(const QJsonObject &message,
                                            QString *errorMessage,
                                            int *errorCode) const
{
    const QJsonValue typeValue = message.value(QStringLiteral("type"));
    if (!typeValue.isString() || typeValue.toString().isEmpty()) {
        *errorCode = 2101;
        *errorMessage = QStringLiteral("missing or invalid type");
        return false;
    }

    if (message.contains(QStringLiteral("protocol_version"))) {
        const QJsonValue versionValue = message.value(QStringLiteral("protocol_version"));
        if (!isIntegerJsonValue(versionValue)) {
            *errorCode = 2102;
            *errorMessage = QStringLiteral("invalid protocol_version");
            return false;
        }

        const qint64 version = jsonInteger(versionValue);
        if (version != HostProtocol::ProtocolVersion) {
            *errorCode = 2103;
            *errorMessage = QStringLiteral("unsupported protocol_version");
            return false;
        }
    }

    if (message.contains(QStringLiteral("seq")) && !isIntegerJsonValue(message.value(QStringLiteral("seq")))) {
        *errorCode = 2104;
        *errorMessage = QStringLiteral("invalid seq");
        return false;
    }

    const QString type = typeValue.toString();
    if (type == QStringLiteral("heartbeat_ack")) {
        if (!isIntegerJsonValue(message.value(QStringLiteral("seq"))) || jsonInteger(message.value(QStringLiteral("seq"))) <= 0) {
            *errorCode = 2110;
            *errorMessage = QStringLiteral("heartbeat_ack missing valid seq");
            return false;
        }
    } else if (type == QStringLiteral("ack")) {
        if (!message.value(QStringLiteral("ack_type")).isString()) {
            *errorCode = 2120;
            *errorMessage = QStringLiteral("ack missing ack_type");
            return false;
        }
        if (!isIntegerJsonValue(message.value(QStringLiteral("seq"))) || jsonInteger(message.value(QStringLiteral("seq"))) <= 0) {
            *errorCode = 2121;
            *errorMessage = QStringLiteral("ack missing valid seq");
            return false;
        }
        if (!message.value(QStringLiteral("ok")).isBool()) {
            *errorCode = 2122;
            *errorMessage = QStringLiteral("ack missing ok");
            return false;
        }
        if (!isIntegerJsonValue(message.value(QStringLiteral("code")))) {
            *errorCode = 2123;
            *errorMessage = QStringLiteral("ack missing code");
            return false;
        }
    } else if (type == QStringLiteral("camera_info")) {
        if (!validateCameraObject(message, errorMessage, errorCode)) {
            return false;
        }
    } else if (type == QStringLiteral("camera_list_response")) {
        const QJsonValue camerasValue = message.value(QStringLiteral("cameras"));
        if (!camerasValue.isArray()) {
            *errorCode = 2140;
            *errorMessage = QStringLiteral("camera_list_response missing cameras array");
            return false;
        }
        const QJsonArray cameras = camerasValue.toArray();
        for (const QJsonValue &cameraValue : cameras) {
            if (!cameraValue.isObject()) {
                *errorCode = 2141;
                *errorMessage = QStringLiteral("camera_list_response camera item is not object");
                return false;
            }
            if (!validateCameraObject(cameraValue.toObject(), errorMessage, errorCode)) {
                return false;
            }
        }
    } else if (type == QStringLiteral("system_snapshot")) {
        if (message.contains(QStringLiteral("control_mode"))
            && !message.value(QStringLiteral("control_mode")).isString()) {
            *errorCode = 2150;
            *errorMessage = QStringLiteral("system_snapshot invalid control_mode");
            return false;
        }
        if (message.contains(QStringLiteral("emergency"))
            && !message.value(QStringLiteral("emergency")).isObject()) {
            *errorCode = 2151;
            *errorMessage = QStringLiteral("system_snapshot invalid emergency object");
            return false;
        }
        if (message.contains(QStringLiteral("motor"))
            && !message.value(QStringLiteral("motor")).isObject()) {
            *errorCode = 2152;
            *errorMessage = QStringLiteral("system_snapshot invalid motor object");
            return false;
        }
    } else if (type == QStringLiteral("protocol_error")) {
        if (!isIntegerJsonValue(message.value(QStringLiteral("code")))
            || !message.value(QStringLiteral("message")).isString()) {
            *errorCode = 2160;
            *errorMessage = QStringLiteral("protocol_error missing code or message");
            return false;
        }
    } else if (type == QStringLiteral("service_call_result")) {
        if (!message.value(QStringLiteral("request_type")).isString()
            || !message.value(QStringLiteral("command")).isString()
            || !message.value(QStringLiteral("service")).isString()
            || !message.value(QStringLiteral("ok")).isBool()
            || !isIntegerJsonValue(message.value(QStringLiteral("code")))
            || !message.value(QStringLiteral("message")).isString()) {
            *errorCode = 2165;
            *errorMessage = QStringLiteral("service_call_result missing required fields");
            return false;
        }
        if (message.contains(QStringLiteral("duration_ms"))
            && !isIntegerJsonValue(message.value(QStringLiteral("duration_ms")))) {
            *errorCode = 2166;
            *errorMessage = QStringLiteral("service_call_result invalid duration_ms");
            return false;
        }
    } else if (type == QStringLiteral("motor_state")) {
        if (!message.value(QStringLiteral("joints")).isArray()) {
            *errorCode = 2170;
            *errorMessage = QStringLiteral("motor_state missing joints array");
            return false;
        }
    } else if (type == QStringLiteral("joint_runtime_states")) {
        if (!message.value(QStringLiteral("states")).isArray()) {
            *errorCode = 2175;
            *errorMessage = QStringLiteral("joint_runtime_states missing states array");
            return false;
        }
    } else if (type == QStringLiteral("co2_data")) {
        if (!message.value(QStringLiteral("ppm")).isDouble()) {
            *errorCode = 2180;
            *errorMessage = QStringLiteral("co2_data missing ppm");
            return false;
        }
    } else if (type == QStringLiteral("imu_data")) {
        if (!message.value(QStringLiteral("roll")).isDouble()
            || !message.value(QStringLiteral("pitch")).isDouble()
            || !message.value(QStringLiteral("yaw")).isDouble()) {
            *errorCode = 2190;
            *errorMessage = QStringLiteral("imu_data missing attitude fields");
            return false;
        }
    }

    return true;
}

bool RobotTcpClient::validateCameraObject(const QJsonObject &camera,
                                         QString *errorMessage,
                                         int *errorCode) const
{
    const QJsonValue idValue = camera.value(QStringLiteral("camera_id"));
    if (!isIntegerJsonValue(idValue)) {
        *errorCode = 2130;
        *errorMessage = QStringLiteral("camera missing camera_id");
        return false;
    }

    const qint64 cameraId = jsonInteger(idValue);
    if (cameraId < 0 || cameraId >= MAX_CAMERA_COUNT) {
        *errorCode = 2131;
        *errorMessage = QStringLiteral("camera_id out of range");
        return false;
    }

    if (!camera.value(QStringLiteral("online")).isBool()) {
        *errorCode = 2132;
        *errorMessage = QStringLiteral("camera missing online");
        return false;
    }

    const bool online = camera.value(QStringLiteral("online")).toBool();
    const QJsonValue urlValue = camera.value(QStringLiteral("rtsp_url"));
    if (!urlValue.isString()) {
        *errorCode = 2133;
        *errorMessage = QStringLiteral("camera missing rtsp_url");
        return false;
    }
    if (online && urlValue.toString().isEmpty()) {
        *errorCode = 2134;
        *errorMessage = QStringLiteral("online camera has empty rtsp_url");
        return false;
    }

    const QStringList optionalIntFields = {
        QStringLiteral("width"),
        QStringLiteral("height"),
        QStringLiteral("fps"),
        QStringLiteral("bitrate_kbps"),
    };
    for (const QString &field : optionalIntFields) {
        if (camera.contains(field) && !isIntegerJsonValue(camera.value(field))) {
            *errorCode = 2135;
            *errorMessage = QStringLiteral("camera field is not integer: %1").arg(field);
            return false;
        }
    }

    if (camera.contains(QStringLiteral("codec")) && !camera.value(QStringLiteral("codec")).isString()) {
        *errorCode = 2136;
        *errorMessage = QStringLiteral("camera codec is not string");
        return false;
    }

    return true;
}

void RobotTcpClient::processCameraInfoMessage(const QJsonObject &message)
{
    const int cameraId = message["camera_id"].toInt();
    const QString rtspUrl = message["rtsp_url"].toString();
    const bool online = message["online"].toBool();
    const QString codec = message["codec"].toString(QStringLiteral("unknown"));
    const int width = message["width"].toInt();
    const int height = message["height"].toInt();
    const int fps = message["fps"].toInt();
    const int bitrateKbps = message["bitrate_kbps"].toInt();
    emit cameraInfoReceived(cameraId, rtspUrl, online, codec, width, height, fps, bitrateKbps);
}

void RobotTcpClient::processCameraListResponse(const QJsonObject &message)
{
    emit systemStatusReceived(message);

    const QJsonArray cameras = message["cameras"].toArray();
    for (const QJsonValue &cameraValue : cameras) {
        processCameraInfoMessage(cameraValue.toObject());
    }
}

void RobotTcpClient::requestBridgeState(const QString &reason)
{
    const bool syncSent = requestBridgeSync(reason);
    const bool cameraListSent = requestCameraList();

    QJsonObject event;
    event["type"] = "sync_request_sent";
    event["protocol_version"] = HostProtocol::ProtocolVersion;
    event["reason"] = reason;
    event["sync_sent"] = syncSent;
    event["camera_list_sent"] = cameraListSent;
    event["timestamp_ms"] = HostProtocol::nowMs();
    emit systemStatusReceived(event);
}

void RobotTcpClient::emitProtocolError(qint64 seq, int code, const QString &message)
{
    m_stats.protocolErrorCount++;

    QJsonObject event;
    event["type"] = "protocol_error";
    event["protocol_version"] = HostProtocol::ProtocolVersion;
    event["seq"] = seq;
    event["code"] = code;
    event["message"] = message;
    event["source"] = QStringLiteral("upper_computer");
    event["timestamp_ms"] = HostProtocol::nowMs();
    emit systemStatusReceived(event);
    emitStatsUpdate();
}

void RobotTcpClient::closeForProtocolError(qint64 seq, int code, const QString &message)
{
    emitProtocolError(seq, code, message);
    m_receivedData.clear();
    m_socket->abort();
}

void RobotTcpClient::trimExpiredHeartbeats(qint64 nowMs)
{
    QList<quint64> expired;
    for (auto it = m_pendingHeartbeats.cbegin(); it != m_pendingHeartbeats.cend(); ++it) {
        if (nowMs - it.value() > HEARTBEAT_INTERVAL_MS * 3) {
            expired.append(it.key());
        }
    }

    for (quint64 seq : expired) {
        if (m_pendingHeartbeats.remove(seq)) {
            m_stats.heartbeatTimeoutCount++;
        }
    }
}

void RobotTcpClient::processReceivedData()
{
    auto *socket = m_socket;
    while (socket == m_socket && isConnected()) {
        int index = m_receivedData.indexOf('\n');
        if (index < 0) {
            if (m_receivedData.size() > MAX_FRAME_BYTES) {
                closeForProtocolError(0, 2001, QStringLiteral("TCP frame exceeds max length"));
            }
            break;
        }

        if (index + 1 > MAX_FRAME_BYTES) {
            closeForProtocolError(0, 2001, QStringLiteral("TCP frame exceeds max length"));
            break;
        }

        QByteArray line = m_receivedData.left(index);
        m_receivedData = m_receivedData.mid(index + 1);

        if (line.isEmpty()) continue;

        if (line.size() > MAX_FRAME_BYTES) {
            closeForProtocolError(0, 2001, QStringLiteral("TCP frame exceeds max length"));
            break;
        }

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(line, &error);

        if (error.error != QJsonParseError::NoError) {
            HANDLE_ERROR_DETAIL(Utils::ErrorCode::ProtocolParseError, MODULE,
                                "JSON解析错误", QString("%1 | 原始数据: %2")
                                    .arg(error.errorString())
                                    .arg(QString::fromUtf8(line.left(200))));
            continue;
        }

        if (!doc.isObject()) {
            emitProtocolError(0, 2100, QStringLiteral("JSON frame is not an object"));
            continue;
        }

        QJsonObject msg = doc.object();
        emit rawMessageReceived(line);
        if (socket != m_socket || !isConnected()) return;

        m_stats.messagesReceived++;
        const qint64 receivedAtMs = m_clock.elapsed();
        m_lastMessageReceivedMs = receivedAtMs;

        if (m_controlV2 && msg["type"] == QJsonValue("capabilities") && !m_controlHelloStarted) {
            if (msg["control_protocol_version"] != QJsonValue(2) || !msg["control_capabilities"].isArray()) {
                m_capabilityDiscovery = "invalid"; disconnectFromROS();
                emit connectionError(QStringLiteral("桥接控制能力声明无效或版本不兼容，未建立控制会话。")); return;
            }
            m_controlHelloStarted = true; m_capabilityDiscovery = "declared";
            const bool presets = msg["control_capabilities"].toArray().contains("motion_presets_v1");
            m_session.begin(m_expectedRobot, m_hostAddress + ':' + QString::number(m_port), presets);
        }
        if (m_controlV2 && msg["type"] == QJsonValue("control_result")) {
            m_session.receive(msg);
            continue;
        }
        if (m_controlV2 && msg["type"] == QJsonValue("hello") && msg["robot_id"] != QJsonValue(m_expectedRobot)) {
            disconnectFromROS(); emit connectionError(QStringLiteral("控制桥机器人身份与所选档案不一致。")); return;
        }
        QString schemaError;
        int schemaCode = 0;
        if (!validateIncomingMessage(msg, &schemaError, &schemaCode)) {
            if (schemaCode == 2102 || schemaCode == 2103) {
                closeForProtocolError(msg["seq"].toVariant().toLongLong(), schemaCode, schemaError);
                return;
            }
            emitProtocolError(msg["seq"].toVariant().toLongLong(), schemaCode, schemaError);
            continue;
        }

        processLegacyMessage(msg, receivedAtMs);
    }

    emitStatsUpdate();
}

void RobotTcpClient::processLegacyMessage(const QJsonObject &msg, qint64 receivedAtMs)
{
    QString msgType = msg["type"].toString();
    if (m_controlV2 && msgType == "ack") return; // Business ACKs belong to the completed operation, not the legacy ACK table.
    if (msgType == "heartbeat_ack") {
        handleHeartbeatAck(msg, receivedAtMs);
    } else if (msgType == "heartbeat") {
        // 服务端单向流量不能证明客户端发出的心跳已抵达。
        emit systemStatusReceived(msg);
    } else if (msgType == "ack") {
        handleAckMessage(msg, receivedAtMs);
    } else if (msgType == "camera_list_response") {
        processCameraListResponse(msg);
    } else if (msgType == "system_snapshot") {
        if (!m_controlV2 && m_syncRequestSeq != 0 && msg["seq"].toVariant().toULongLong() == m_syncRequestSeq
            && msg["emergency"].toObject()["active"].isBool()) {
            m_stateSynchronized = true;
            m_emergencyActive = msg["emergency"].toObject()["active"].toBool();
            if (m_emergencyActive) setOperatorControlEnabled(false);
            if (m_heartbeatOnline && (m_requiredCapabilities.isEmpty() || m_capabilitiesVerified)) setConnectionState(ConnectionState::Ready);
        }
        emit systemStatusReceived(msg);
    } else if (msgType == "emergency_state") {
        if (!m_controlV2 && msg["active"].isBool()) {
            m_emergencyActive = msg["active"].toBool();
            if (m_emergencyActive) setOperatorControlEnabled(false);
        }
        emit systemStatusReceived(msg);
    } else if (msgType == "capabilities" && !m_controlV2 && !m_requiredCapabilities.isEmpty()) {
        const auto supports = msg["supports"].toArray();
        for (const auto &required : m_requiredCapabilities) {
            if (!supports.contains(required)) {
                closeForProtocolError(0, 2103, QStringLiteral("Bridge missing required capability: %1").arg(required));
                return;
            }
        }
        m_capabilitiesVerified = true;
        if (m_heartbeatOnline && m_stateSynchronized) setConnectionState(ConnectionState::Ready);
        emit systemStatusReceived(msg);
    } else if (msgType == "protocol_error" && !m_controlV2 && msg["code"].toInt() == 2110) {
        setOperatorControlEnabled(false);
        emit systemStatusReceived(msg);
    } else if (msgType == "param_response"
               || msgType == "protocol_error"
               || msgType == "service_call_result"
               || msgType == "hello" || msgType == "capabilities"
               || msgType == "arm_named_targets"
               || msgType == "moveit_named_targets"
               || msgType == "moveit_named_poses"
               || msgType == "arm_action_list") {
        emit systemStatusReceived(msg);
    } else if (msgType == "motor_state") {
        MotorState state = parseMotorState(msg);
        emit motorStateReceived(state);
    } else if (msgType == "joint_runtime_states") {
        emit jointRuntimeStatesReceived(parseJointRuntimeStates(msg));
    } else if (msgType == "joint_data") {
        int jointId = msg["joint_id"].toInt();
        float position = msg["position"].toDouble();
        float current = msg["current"].toDouble();
        float torque = msg["torque"].toDouble();
        emit jointDataReceived(jointId, position, current, torque);
    } else if (msgType == "system_status") {
        if (!m_controlV2 && msg["emergency_active"].isBool()) {
            m_emergencyActive = msg["emergency_active"].toBool();
        }
        if (!m_controlV2 && (msg["emergency_active"].toBool() || msg["watchdog_active"].toBool()
            || msg["operator_input_status"].toString().startsWith(QStringLiteral("dropped_")))) {
            setOperatorControlEnabled(false);
        }
        emit systemStatusReceived(msg);
    } else if (msgType == "co2_data") {
        float ppm = msg["ppm"].toDouble();
        emit co2DataReceived(ppm);
    } else if (msgType == "imu_data") {
        float roll = msg["roll"].toDouble();
        float pitch = msg["pitch"].toDouble();
        float yaw = msg["yaw"].toDouble();
        float accelX = msg["accel_x"].toDouble();
        float accelY = msg["accel_y"].toDouble();
        float accelZ = msg["accel_z"].toDouble();
        emit imuDataReceived(roll, pitch, yaw, accelX, accelY, accelZ);
    } else if (msgType == "camera_info") {
        processCameraInfoMessage(msg);
    } else {
        LOG_DEBUG(MODULE, QString("收到未处理的消息类型: %1").arg(msgType));
    }
}

MotorState RobotTcpClient::parseMotorState(const QJsonObject &json)
{
    MotorState state;

    QJsonArray jointsArray = json["joints"].toArray();
    for (int i = 0; i < 6 && i < jointsArray.size(); ++i) {
        QJsonObject joint = jointsArray[i].toObject();
        state.joints[i].position = static_cast<int16_t>(joint["position"].toDouble() * 1000.0f);
        state.joints[i].current = static_cast<int16_t>(joint["current"].toDouble() * 1000.0f);
    }

    state.executor_position = static_cast<int16_t>(json["executor_position"].toDouble() * 1000.0f);
    state.executor_torque = static_cast<int16_t>(json["executor_torque"].toDouble() * 1000.0f);
    state.executor_flags = static_cast<uint8_t>(json["executor_flags"].toInt(0));
    state.reserved = static_cast<uint8_t>(json["reserved"].toInt(0));

    return state;
}

JointRuntimeStateList RobotTcpClient::parseJointRuntimeStates(const QJsonObject &json)
{
    JointRuntimeStateList states;
    const QJsonArray statesArray = json.value(QStringLiteral("states")).toArray();
    states.reserve(statesArray.size());

    for (const QJsonValue &value : statesArray) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject object = value.toObject();
        JointRuntimeState state;
        state.jointName = object.value(QStringLiteral("joint_name")).toString();
        state.backend = object.value(QStringLiteral("backend")).toString();
        state.lifecycleState = object.value(QStringLiteral("lifecycle_state")).toString();
        state.online = object.value(QStringLiteral("online")).toBool(false);
        state.enabled = object.value(QStringLiteral("enabled")).toBool(false);
        state.fault = object.value(QStringLiteral("fault")).toBool(false);
        states.append(state);
    }

    return states;
}

void RobotTcpClient::emitStatsUpdate()
{
    emit statsUpdated(getStats());
}

void RobotTcpClient::setHeartbeatOnline(bool online)
{
    if (m_heartbeatOnline == online) {
        return;
    }

    m_heartbeatOnline = online;
    emit heartbeatChanged(online);
}

} // namespace Communication
