#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSettings>
#include <memory>
#include "RobotTcpClient.h"
using namespace Communication;

// Independent socket peer. It neither imports nor emulates hardware/ROS.
class ControlPeer : public QObject {
public:
    QTcpServer server;
    QList<QTcpSocket *> peers;
    QList<QJsonObject> messages;
    bool freshness = true, status = true;
    QString identity = "robot24";
    ControlPeer() {
        connect(&server,&QTcpServer::newConnection,this,[this]{
            while(server.hasPendingConnections()) {
                auto *socket=server.nextPendingConnection(); peers.append(socket);
                auto bytes=std::make_shared<QByteArray>();
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,bytes]{
                    *bytes+=socket->readAll();
                    while(bytes->contains('\n')) {
                        const auto end=bytes->indexOf('\n'); const auto message=QJsonDocument::fromJson(bytes->left(end)).object(); bytes->remove(0,end+1);
                        messages.append(message); handle(socket,message);
                    }
                });
                send(socket,{{"type","hello"},{"protocol_version",1},{"robot_id",identity}});
            }
        });
    }
    void send(QTcpSocket *socket,const QJsonObject &value) { socket->write(QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n'); }
    void reply(QTcpSocket *socket,const QJsonObject &request,const QJsonObject &value) {
        send(socket,{{"type","control_result"},{"protocol_version",2},{"request_id",request["request_id"]},
            {"seq",request.contains("seq")?request["seq"]:QJsonValue(QJsonValue::Null)},{"ok",true},{"result",value}});
    }
    QJsonObject token() const {return {{"epoch","epoch-A"},{"freshness_token","peer-token"},{"valid_for_ms",300},{"refresh_ms",100},{"input_ttl_ms",300},{"lease_ms",1500},{"watchdog_ms",500}};}
    void handle(QTcpSocket *socket,const QJsonObject &m) {
        const auto type=m["type"].toString();
        if(type=="heartbeat") send(socket,{{"type","heartbeat_ack"},{"seq",m["seq"]},{"protocol_version",1}});
        else if(type=="sync_request") send(socket,{{"type","system_snapshot"},{"seq",m["seq"]},{"emergency",QJsonObject{{"active",false}}}});
        else if(type=="control_hello") {
            auto hello=token(); hello["session_id"]="peer-session";hello["robot_id"]=identity;hello["role"]="control";
            hello["capabilities"]=QJsonArray::fromStringList(ControlProtocol::capabilities());hello["authorized"]=false;hello["build_version"]="independent-peer";reply(socket,m,hello);
        } else if(type=="freshness_request"&&freshness) reply(socket,m,token());
        else if(type=="control_status"&&status) reply(socket,m,{{"epoch","epoch-A"},{"authorized",true},{"motion_permitted",true},{"emergency_active",false},{"counts",QJsonObject{}},{"queue_depth",0}});
        else if(type=="operator_input") reply(socket,m,{{"input_status","neutral"}});
        else if(type=="authorize") reply(socket,m,{{"authorized",true}});
        else if(type=="authorize_operations") reply(socket,m,{{"operations_authorized",true}});
        else if(type=="emergency_stop") reply(socket,m,{{"emergency_active",true},{"physical_stop_confirmed",false}});
    }
};
class ControlTransportTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    void start(ControlPeer &peer,RobotTcpClient &client) {
        QVERIFY(peer.server.listen(QHostAddress::LocalHost)); client.configureControlV2("robot24");
        connect(&client,&RobotTcpClient::inputSampleRequested,&client,[&client]{client.sendOperatorInput({});},Qt::QueuedConnection);
        QVERIFY(client.connectToROS("127.0.0.1",peer.server.serverPort()));
        QTRY_COMPARE(client.connectionState(),RobotTcpClient::ConnectionState::Ready);
    }
    void authorize(RobotTcpClient &client) {
        bool sent=false; QTRY_VERIFY(([&]{if(!sent)sent=client.setOperatorControlEnabled(true);return sent;})());
        QTRY_VERIFY(client.isOperatorControlEnabled());
    }
private slots:
    void initTestCase() {
        QVERIFY(settings.isValid()); QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,settings.path());
        Utils::Logger::instance().setLogToConsole(false); Utils::Logger::instance().setLogToFile(false);
    }
    void legacyMessagesCannotGrantV2Authorization() {
        ControlPeer peer; RobotTcpClient client; start(peer,client);
        QVERIFY(!client.isOperatorControlEnabled());
        peer.send(peer.peers.first(),{{"type","input_status"},{"authorized",true},{"motion_permitted",true},{"status","accepted"}});
        QTest::qWait(100); QVERIFY(!client.isOperatorControlEnabled());
        QVERIFY(!client.sendJointControl(1,1,1)); QVERIFY(!client.sendSystemCommand("init"));
        authorize(client); QVERIFY(!client.controlDiagnostics()["operations_authorized"].toBool());
    }
    void tokenLossRevokesDespiteHealthyHeartbeatAndStatus() {
        ControlPeer peer; RobotTcpClient client; start(peer,client); authorize(client);
        peer.freshness=false;
        QTRY_VERIFY_WITH_TIMEOUT(!client.isOperatorControlEnabled(),1000);
        QVERIFY(!client.controlDiagnostics()["token_valid"].toBool());
        QVERIFY(client.getStats().lastHeartbeatAckMs>0);
        peer.freshness=true; QTRY_VERIFY(client.controlDiagnostics()["ready"].toBool());
        QVERIFY(!client.isOperatorControlEnabled());
    }
    void expiredRuntimeRevokesAndReconnects() {
        ControlPeer peer; RobotTcpClient client; start(peer,client); authorize(client); peer.status=false;
        QTRY_VERIFY_WITH_TIMEOUT(!client.isOperatorControlEnabled(),1000);
        QTRY_VERIFY_WITH_TIMEOUT(client.connectionState()!=RobotTcpClient::ConnectionState::Ready,1500);
    }
    void emergencyWorksOnSeparateSocketAfterOrdinaryDisconnect() {
        ControlPeer peer; RobotTcpClient client; start(peer,client); client.disconnectFromROS();
        QSignalSpy events(&client,&RobotTcpClient::systemStatusReceived);
        QVERIFY(client.sendEmergencyStop("independent-test")); bool accepted=false;
        QTRY_VERIFY(([&]{for(const auto &args:events){const auto e=args[0].toJsonObject();if(e["type"]==QJsonValue("emergency_result")&&e["ok"]==QJsonValue(true))accepted=true;}return accepted;})());
        QCOMPARE(peer.peers.size(),2); QVERIFY(!client.isOperatorControlEnabled());
    }
    void mismatchedRobotStopsNegotiation() {
        ControlPeer peer; peer.identity="another-robot"; QVERIFY(peer.server.listen(QHostAddress::LocalHost));
        RobotTcpClient client; client.configureControlV2("robot24"); QSignalSpy errors(&client,&RobotTcpClient::connectionError);
        QVERIFY(client.connectToROS("127.0.0.1",peer.server.serverPort()));
        QTRY_VERIFY(!errors.isEmpty()); QVERIFY(!client.isOperatorControlEnabled());
        QVERIFY(client.connectionState()!=RobotTcpClient::ConnectionState::Ready);
    }
};
QTEST_GUILESS_MAIN(ControlTransportTest)
#include "control_transport_test.moc"
