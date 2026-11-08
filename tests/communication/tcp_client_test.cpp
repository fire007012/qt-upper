#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <memory>
#include "RobotTcpClient.h"

using Communication::RobotTcpClient;

// Exercise the real client over TCP; no ROS installation or reference-tree writes.
class BridgeStub : public QObject
{
public:
    QTcpServer server;
    QList<QTcpSocket *> peers;
    QList<QJsonObject> messages;
    bool acknowledgeHeartbeats = true;
    bool acknowledgeCommands = true;
    bool wrongHeartbeatSequence = false;

    BridgeStub()
    {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (server.hasPendingConnections()) {
                auto *peer = server.nextPendingConnection();
                peers.append(peer);
                auto buffer = std::make_shared<QByteArray>();
                connect(peer, &QTcpSocket::readyRead, this, [this, peer, buffer]() {
                    buffer->append(peer->readAll());
                    while (buffer->contains('\n')) {
                        int end = buffer->indexOf('\n');
                        const auto msg = QJsonDocument::fromJson(buffer->left(end)).object();
                        buffer->remove(0, end + 1);
                        messages.append(msg);
                        const auto type = msg["type"].toString();
                        if (type == "heartbeat" && acknowledgeHeartbeats) {
                            auto seq = msg["seq"].toVariant().toLongLong();
                            if (wrongHeartbeatSequence) seq += 100000;
                            send(peer, {{"type", "heartbeat_ack"}, {"seq", seq}, {"protocol_version", 1}});
                        } else if (type == "system_command" && acknowledgeCommands) {
                            send(peer, ack(msg, type));
                        } else if (type == "sync_request") {
                            send(peer, {{"type", "system_snapshot"}, {"seq", msg["seq"]},
                                        {"emergency", QJsonObject{{"active", false}}}});
                        }
                    }
                });
            }
        });
    }

    bool listen(quint16 port = 0) { return server.listen(QHostAddress::LocalHost, port); }
    quint16 port() const { return server.serverPort(); }
    static void send(QTcpSocket *peer, const QJsonObject &msg)
    {
        peer->write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n');
    }
    static QJsonObject ack(const QJsonObject &msg, const QString &type)
    {
        return {{"type", "ack"}, {"protocol_version", 1}, {"seq", msg["seq"]},
                {"ack_type", type}, {"ok", true}, {"code", 0}};
    }
    QJsonObject last(const QString &type) const
    {
        for (auto it = messages.crbegin(); it != messages.crend(); ++it)
            if ((*it)["type"].toString() == type) return *it;
        return {};
    }
};

class TcpClientTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        Utils::Logger::instance().setLogToConsole(false);
        Utils::Logger::instance().setLogToFile(false);
    }

    void healthyHeartbeatAndCommand()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        QSignalSpy heartbeat(&client, &RobotTcpClient::heartbeatChanged);
        QVERIFY(client.connectToROS("127.0.0.1", bridge.port()));
        QTRY_COMPARE(heartbeat.count(), 1);
        QVERIFY(heartbeat.first().at(0).toBool());
        QVERIFY(client.getStats().lastHeartbeatRttMs >= 0);
        QVERIFY(client.getStats().lastHeartbeatRttMs < 1000);
        QVERIFY(client.getStats().lastHeartbeatAckMs > 1000000000000LL);
        QCOMPARE(client.connectionState(), RobotTcpClient::ConnectionState::Ready);
        QVERIFY(client.sendSystemCommand("init"));
        QTRY_COMPARE(client.getStats().ackReceivedCount, quint64(1));
        QCOMPARE(client.getStats().ackPendingCount, quint64(0));
        client.disconnectFromROS();
        QVERIFY(!client.isConnected());
    }

    void missingFirstAckRebuildsConnection()
    {
        BridgeStub bridge;
        bridge.acknowledgeHeartbeats = false;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        QSignalSpy connected(&client, &RobotTcpClient::connectedToROS);
        QSignalSpy disconnected(&client, &RobotTcpClient::disconnectedFromROS);
        QVERIFY(client.connectToROS("127.0.0.1", bridge.port()));
        QTRY_COMPARE(connected.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(disconnected.count() >= 1, 5000);
        bridge.acknowledgeHeartbeats = true;
        QTRY_VERIFY_WITH_TIMEOUT(connected.count() >= 2, 5000);
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(client.getStats().heartbeatTimeoutCount >= 1);
    }

    void lostAckAfterHealthyConnection()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        QSignalSpy disconnected(&client, &RobotTcpClient::disconnectedFromROS);
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        bridge.acknowledgeHeartbeats = false;
        // Telemetry still arriving must not keep a broken bidirectional link online.
        QTimer telemetry;
        connect(&telemetry, &QTimer::timeout, &bridge, [&bridge]() {
            BridgeStub::send(bridge.peers.last(), {{"type", "heartbeat"}});
        });
        telemetry.start(100);
        QTRY_VERIFY_WITH_TIMEOUT(disconnected.count() >= 1, 5000);
        telemetry.stop();
    }

    void unmatchedAckNeverMarksOnline()
    {
        BridgeStub bridge;
        bridge.wrongHeartbeatSequence = true;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        QSignalSpy heartbeat(&client, &RobotTcpClient::heartbeatChanged);
        QSignalSpy disconnected(&client, &RobotTcpClient::disconnectedFromROS);
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY_WITH_TIMEOUT(disconnected.count() >= 1, 5000);
        QCOMPARE(heartbeat.count(), 0);
        QCOMPARE(client.getStats().lastHeartbeatAckMs, qint64(0));
    }

    void initialRefusalRecovers()
    {
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto port = reservation.serverPort();
        reservation.close();
        RobotTcpClient client;
        QSignalSpy error(&client, &RobotTcpClient::connectionError);
        QVERIFY(client.connectToROS("127.0.0.1", port));
        QTRY_VERIFY(!error.isEmpty());
        BridgeStub bridge;
        QVERIFY(bridge.listen(port));
        QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 5000);
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
    }

    void manualDisconnectCancelsOfflineRetry()
    {
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto port = reservation.serverPort();
        reservation.close();
        RobotTcpClient client;
        QSignalSpy error(&client, &RobotTcpClient::connectionError);
        client.connectToROS("127.0.0.1", port);
        QTRY_VERIFY(!error.isEmpty());
        client.disconnectFromROS();
        BridgeStub bridge;
        QVERIFY(bridge.listen(port));
        QTest::qWait(3600);
        QVERIFY(!client.isConnected());
        QVERIFY(bridge.peers.isEmpty());
    }

    void retriesContinueAfterFiveFailures()
    {
        QTcpServer reservation;
        QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
        const auto port = reservation.serverPort();
        reservation.close();
        RobotTcpClient client;
        QSignalSpy errors(&client, &RobotTcpClient::connectionError);
        client.connectToROS("127.0.0.1", port);
        QTRY_VERIFY_WITH_TIMEOUT(errors.count() >= 6, 50000);
        BridgeStub bridge;
        QVERIFY(bridge.listen(port));
        QTRY_VERIFY_WITH_TIMEOUT(client.getStats().lastHeartbeatAckMs > 0, 12000);
    }

    void mismatchedCommandAckDoesNotConsumeRequest()
    {
        BridgeStub bridge;
        bridge.acknowledgeCommands = false;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(client.sendSystemCommand("init"));
        QTRY_VERIFY(!bridge.last("system_command").isEmpty());
        const auto request = bridge.last("system_command");
        BridgeStub::send(bridge.peers.last(), BridgeStub::ack(request, "emergency_stop"));
        QTest::qWait(50);
        QCOMPARE(client.getStats().ackPendingCount, quint64(1));
        QCOMPARE(client.getStats().ackReceivedCount, quint64(0));
        BridgeStub::send(bridge.peers.last(), BridgeStub::ack(request, "system_command"));
        QTRY_COMPARE(client.getStats().ackReceivedCount, quint64(1));
        QCOMPARE(client.getStats().ackPendingCount, quint64(0));
    }

    void commandTimeoutIsNotRetried()
    {
        BridgeStub bridge;
        bridge.acknowledgeCommands = false;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(client.sendSystemCommand("init"));
        QTRY_COMPARE_WITH_TIMEOUT(client.getStats().ackTimeoutCount, quint64(1), 3000);
        QCOMPARE(client.getStats().ackPendingCount, quint64(0));
        int commands = 0;
        for (const auto &msg : bridge.messages)
            commands += msg["type"].toString() == "system_command";
        QCOMPARE(commands, 1);
    }

    void reconnectClearsPartialFramesAndOldHeartbeats()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        QSignalSpy connected(&client, &RobotTcpClient::connectedToROS);
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        const auto oldHeartbeat = bridge.last("heartbeat");
        bridge.peers.last()->write("{\"type\":\"old_partial");
        QTest::qWait(50);
        bridge.peers.last()->disconnectFromHost();
        bridge.acknowledgeHeartbeats = false;
        QTRY_VERIFY_WITH_TIMEOUT(connected.count() >= 2, 5000);
        QTRY_COMPARE(bridge.peers.size(), 2);
        BridgeStub::send(bridge.peers.last(), {{"type", "heartbeat_ack"}, {"seq", oldHeartbeat["seq"]}});
        QTest::qWait(50);
        QCOMPARE(client.getStats().lastHeartbeatAckMs, qint64(0));
        bridge.acknowledgeHeartbeats = true;
        QTRY_VERIFY_WITH_TIMEOUT(client.getStats().lastHeartbeatAckMs > 0, 2000);
        QCOMPARE(client.getStats().protocolErrorCount, quint64(0));
    }

    void oversizedOutputAbortsInsteadOfQueuing()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(!client.sendSystemCommand("init", {{"oversized", QString(70000, QChar('x'))}}));
        QVERIFY(!client.isConnected());
        QCOMPARE(client.getStats().ackPendingCount, quint64(0));
        QVERIFY(bridge.last("system_command").isEmpty());
    }

    void reconnectRequiresNeutralAndExplicitAuthorization()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        QSignalSpy connected(&client, &RobotTcpClient::connectedToROS);
        QSignalSpy authorization(&client, &RobotTcpClient::operatorControlChanged);
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(!client.setOperatorControlEnabled(true));
        Communication::OperatorInputState moving;
        moving.keyboard.pressedKeys = QStringList{"w"};
        QVERIFY(client.sendOperatorInput(moving));
        QVERIFY(!client.setOperatorControlEnabled(true));
        QTRY_VERIFY(!bridge.last("operator_input").isEmpty());
        QVERIFY(bridge.last("operator_input")["keyboard"].toObject()["pressed_keys"].toArray().isEmpty());
        QVERIFY(client.sendOperatorInput({}));
        QVERIFY(client.setOperatorControlEnabled(true));
        QVERIFY(client.sendOperatorInput(moving));
        QTRY_VERIFY(!bridge.last("operator_input")["keyboard"].toObject()["pressed_keys"].toArray().isEmpty());
        bridge.peers.last()->disconnectFromHost();
        QTRY_VERIFY_WITH_TIMEOUT(connected.count() >= 2, 5000);
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(!client.isOperatorControlEnabled());
        QVERIFY(!client.setOperatorControlEnabled(true));
        QVERIFY(client.sendOperatorInput(moving));
        QTRY_VERIFY(bridge.last("operator_input")["keyboard"].toObject()["pressed_keys"].toArray().isEmpty());
        QVERIFY(!client.setOperatorControlEnabled(true));
        QCOMPARE(authorization.count(), 2); // 一次授权，一次断线撤销。
        QVERIFY(!client.sendJointControl(0, 1, 0));
        QVERIFY(!client.sendSystemCommand("move_arm_named_target", {{"target", "home"}}));
    }

    void emergencyRevokesOperatorControl()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(client.sendOperatorInput({}));
        QVERIFY(client.setOperatorControlEnabled(true));
        BridgeStub::send(bridge.peers.last(), {{"type", "emergency_state"}, {"active", true}});
        QTRY_VERIFY(!client.isOperatorControlEnabled());
        QVERIFY(!client.setOperatorControlEnabled(true));
        BridgeStub::send(bridge.peers.last(), {{"type", "emergency_state"}, {"active", false}});
        QTest::qWait(50);
        QVERIFY(!client.isOperatorControlEnabled());
        QVERIFY(client.sendOperatorInput({}));
        QVERIFY(client.setOperatorControlEnabled(true));
    }

    void rejectedInputRevokesWithoutFeedbackLoop()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(client.sendOperatorInput({}));
        QVERIFY(client.setOperatorControlEnabled(true));
        const QJsonObject rejected{{"type", "system_status"}, {"operator_input_status", "dropped_expired"}};
        BridgeStub::send(bridge.peers.last(), rejected);
        QTRY_VERIFY(!client.isOperatorControlEnabled());
        QTRY_VERIFY(!bridge.last("operator_input").isEmpty());
        QTest::qWait(50);
        const auto count = bridge.messages.size();
        BridgeStub::send(bridge.peers.last(), rejected);
        QTest::qWait(50);
        // 一个撤权零帧即可；持续拒收不能触发零帧/拒收的无限往返。
        QCOMPARE(bridge.messages.size(), count);
    }

    void emergencyBypassesFullCommandTracking()
    {
        BridgeStub bridge;
        bridge.acknowledgeCommands = false;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        for (int i = 0; i < 128; ++i) QVERIFY(client.sendSystemCommand("init"));
        QCOMPARE(client.getStats().ackPendingCount, quint64(128));
        QVERIFY(!client.sendSystemCommand("init"));
        QVERIFY(client.sendEmergencyStop("test"));
        QTRY_VERIFY(!bridge.last("emergency_stop").isEmpty());
        QCOMPARE(client.getStats().ackPendingCount, quint64(128));
    }

    void managedBridgeRequiresCapabilities()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        client.setRequiredCapabilities({"sync_request", "readonly_monitor", "monotonic_watchdog", "connection_control_owner"});
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(client.sendOperatorInput({}));
        QVERIFY(!client.setOperatorControlEnabled(true));
        QVERIFY(client.connectionState() != RobotTcpClient::ConnectionState::Ready);
        BridgeStub::send(bridge.peers.last(), {{"type", "capabilities"}, {"protocol_version", 1},
            {"supports", QJsonArray{"sync_request", "readonly_monitor", "monotonic_watchdog", "connection_control_owner"}}});
        QTRY_VERIFY(client.connectionState() == RobotTcpClient::ConnectionState::Ready);
        QVERIFY(client.sendOperatorInput({}));
        QVERIFY(client.setOperatorControlEnabled(true));
        client.disconnectFromROS();
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_COMPARE(client.getStats().connectionCount, quint64(2));
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(client.sendOperatorInput({}));
        QVERIFY(!client.setOperatorControlEnabled(true));
    }

    void managedBridgeMissingCapabilityCloses()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        client.setRequiredCapabilities({"monotonic_watchdog"});
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        BridgeStub::send(bridge.peers.last(), {{"type", "capabilities"}, {"protocol_version", 1}, {"supports", QJsonArray{"watchdog"}}});
        QTRY_VERIFY(!client.isConnected());
        QVERIFY(!client.setOperatorControlEnabled(true));
    }

    void controlOwnerConflictRevokesAuthorization()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(client.sendOperatorInput({}));
        QVERIFY(client.setOperatorControlEnabled(true));
        BridgeStub::send(bridge.peers.last(), {{"type", "protocol_error"}, {"protocol_version", 1}, {"code", 2110}, {"message", "control owned"}});
        QTRY_VERIFY(!client.isOperatorControlEnabled());
    }

    void incompatibleVersionDisablesControl()
    {
        BridgeStub bridge;
        QVERIFY(bridge.listen());
        RobotTcpClient client;
        client.connectToROS("127.0.0.1", bridge.port());
        QTRY_VERIFY(client.getStats().lastHeartbeatAckMs > 0);
        QVERIFY(client.sendOperatorInput({}));
        QVERIFY(client.setOperatorControlEnabled(true));
        BridgeStub::send(bridge.peers.last(), {{"type", "hello"}, {"protocol_version", 99}});
        QTRY_VERIFY(!client.isConnected());
        QVERIFY(!client.isOperatorControlEnabled());
        QVERIFY(client.getStats().protocolErrorCount > 0);
    }
};

QTEST_GUILESS_MAIN(TcpClientTest)
#include "tcp_client_test.moc"
