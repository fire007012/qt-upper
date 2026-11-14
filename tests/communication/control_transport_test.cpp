#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSettings>
#include <memory>
#include "RobotTcpClient.h"
using namespace Communication;

#include "control_peer.h"
class BusyControlPeer : public ControlPeer {
public:
    QJsonObject operation;
    int queries = 0;
    bool stopped = false;
    void handle(QTcpSocket *socket,const QJsonObject &m) override {
        const auto type=m["type"].toString();
        if(type=="system_command") {
            operation={{"operation_id","busy-op"},{"request_id",m["request_id"]},{"robot_id",identity},{"epoch","epoch-A"},
                {"command",m["command"]},{"state","accepted"},{"created_at",123},{"invalidated",false},
                {"error",QJsonValue(QJsonValue::Null)},{"result",QJsonValue(QJsonValue::Null)}};
            reply(socket,m,operation);
        } else if(type=="operation_query") { ++queries; auto op=operation; op["state"]="running"; reply(socket,m,op); }
        else if(type=="emergency_stop") {
            stopped=true; reply(socket,m,{{"emergency_active",true},{"global_stop_requested",true},{"physical_stop_confirmed",false}});
        } else if(type=="control_status" && stopped) {
            reply(socket,m,{{"epoch","epoch-A"},{"authorized",false},{"motion_permitted",false},{"emergency_active",true},{"counts",QJsonObject{}},{"queue_depth",1},
                {"runtime_fresh",true},{"runtime_global_inhibit",true},
                {"global_stop",QJsonObject{{"state","result_unknown"},{"ok",false}}},
                {"safety_halt",QJsonObject{{"state","failed"},{"message","OPERATIONS_BUSY"}}}});
        } else ControlPeer::handle(socket,m);
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
    void busyLifecycleDoesNotBlockEmergencyHeartbeatOrQueries_data() {
        QTest::addColumn<QString>("command"); QTest::newRow("init") << QString("init"); QTest::newRow("enable") << QString("enable");
    }
    void busyLifecycleDoesNotBlockEmergencyHeartbeatOrQueries() {
        QFETCH(QString,command); BusyControlPeer peer; RobotTcpClient client; start(peer,client);
        bool authorized=false; QTRY_VERIFY(([&]{if(!authorized)authorized=client.authorizeOperations();return authorized;})());
        QTRY_VERIFY(client.controlDiagnostics()["operations_authorized"].toBool());
        QVERIFY(client.sendSystemCommand(command)); QTRY_VERIFY(!peer.operation.isEmpty());
        QTRY_VERIFY(peer.queries>0); const int before=peer.queries;
        QSignalSpy events(&client,&RobotTcpClient::systemStatusReceived); QVERIFY(client.sendEmergencyStop("busy-operation"));
        QTRY_VERIFY(peer.stopped); QTRY_VERIFY(peer.acceptedConnections>=2);
        QTRY_VERIFY(peer.queries>before); QTRY_VERIFY(client.getStats().lastHeartbeatAckMs>0);
        QTRY_VERIFY(client.controlDiagnostics()["server"].toObject()["global_stop"].toObject()["state"]==QJsonValue("result_unknown"));
        QVERIFY(!client.isOperatorControlEnabled()); QVERIFY(!client.sendSystemCommand("clear_emergency"));
        bool physical=false;
        for(const auto &args:events) physical |= args[0].toJsonObject()["physical_stop_confirmed"].toBool();
        QVERIFY(!physical);
    }
    void advertisedPresetsAreSentInFirstHandshake() {
        ControlPeer peer; peer.presetsEnabled=true; RobotTcpClient client; client.setPendingPresets({"base","arm"}); start(peer,client);
        QVERIFY(client.controlDiagnostics()["motion_presets_supported"].toBool());
        QCOMPARE(peer.selectedPresets,QJsonArray({"base","arm"}));
        int hellos=0; for(const auto &m:peer.messages) if(m["type"]==QJsonValue("control_hello")) { ++hellos; QVERIFY(m["capabilities"].toArray().contains("motion_presets_v1")); }
        QCOMPARE(hellos,1);
        client.sendEmergencyStop("preset-global-emergency");
        QTRY_VERIFY(([&]{for(const auto &m:peer.messages) if(m["type"]==QJsonValue("emergency_stop")) return !m.contains("presets")&&!m.contains("groups"); return false;})());
    }
    void delayedCapabilitiesStillNegotiateExactPresets() {
        ControlPeer peer; peer.presetsEnabled=true; peer.capabilitiesDelayMs=450;
        RobotTcpClient client; client.setPendingPresets({"base"}); start(peer,client);
        QVERIFY(client.controlDiagnostics()["motion_presets_supported"].toBool()); QCOMPARE(peer.selectedPresets,QJsonArray{"base"});
        int hellos=0; for (const auto &m:peer.messages) if(m["type"]==QJsonValue("control_hello")) {
            ++hellos; QCOMPARE(m["presets"].toArray(),QJsonArray{"base"}); QVERIFY(m["capabilities"].toArray().contains("motion_presets_v1"));
        }
        QCOMPARE(hellos,1);
    }
    void missingCapabilitiesDoesNotObtainFullVehicleSession() {
        ControlPeer peer; peer.advertiseCapabilities=false; QVERIFY(peer.server.listen(QHostAddress::LocalHost));
        RobotTcpClient client; client.configureControlV2("robot24"); QSignalSpy errors(&client,&RobotTcpClient::connectionError);
        QVERIFY(client.connectToROS("127.0.0.1",peer.server.serverPort())); QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(),3500);
        QVERIFY(!client.controlDiagnostics()["negotiated"].toBool()); QCOMPARE(client.controlDiagnostics()["capability_discovery"].toString(),QString("timeout"));
        for(const auto &m:peer.messages) QVERIFY(m["type"] != QJsonValue("control_hello"));
    }
    void oldServerNeverStartsPresetFlow() {
        ControlPeer peer; RobotTcpClient client; start(peer,client);
        QVERIFY(!client.controlDiagnostics()["motion_presets_supported"].toBool()); QVERIFY(!client.startPresets({"base"}));
        for(const auto &m:peer.messages) if(m["type"]==QJsonValue("control_hello")) QVERIFY(!m.contains("presets"));
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
        QCOMPARE(peer.acceptedConnections,quint64(2)); QVERIFY(!client.isOperatorControlEnabled());
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
