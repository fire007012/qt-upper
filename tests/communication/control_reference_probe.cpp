#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QTcpSocket>
#include "RobotTcpClient.h"
using namespace Communication;
class ControlReferenceTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
    quint16 port() const { return static_cast<quint16>(qEnvironmentVariableIntValue("HOST_CONTROL_PORT")); }
    void start(RobotTcpClient &client, OperatorInputState &input) {
        client.configureControlV2("robot24");
        connect(&client,&RobotTcpClient::inputSampleRequested,&client,[&client,&input]{client.sendOperatorInput(input);},Qt::QueuedConnection);
        QVERIFY(client.connectToROS("127.0.0.1",port()));
        QTRY_COMPARE(client.connectionState(),RobotTcpClient::ConnectionState::Ready);
    }
    QJsonObject lastOperation(RobotTcpClient &client) {
        const auto operations=client.controlDiagnostics()["operations"].toArray(); return operations.isEmpty()?QJsonObject{}:operations.last().toObject();
    }
    void authorize(RobotTcpClient &client, bool motion = true) {
        bool operationsSent=false;
        QTRY_VERIFY(([&]{if(!operationsSent) operationsSent=client.authorizeOperations(); return operationsSent;})());
        QTRY_VERIFY(client.controlDiagnostics()["operations_authorized"].toBool());
        if(motion) { bool sent=false; QTRY_VERIFY(([&]{if(!sent)sent=client.setOperatorControlEnabled(true);return sent;})()); QTRY_VERIFY(client.isOperatorControlEnabled()); }
    }
private slots:
    void initTestCase() {
        QVERIFY(settings.isValid());
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,settings.path());
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
        Utils::Logger::instance().setLogToConsole(false); Utils::Logger::instance().setLogToFile(false);
    }
    void epochAndEmergency() {
        RobotTcpClient client; OperatorInputState input; start(client,input);
        QVERIFY(!client.isOperatorControlEnabled()); QVERIFY(!client.sendSystemCommand("init"));
        authorize(client); const auto epoch=client.controlDiagnostics()["epoch"].toString();
        input.keyboard.pressedKeys=QStringList{"w"}; QVERIFY(client.sendOperatorInput(input));
        QTRY_VERIFY(client.controlDiagnostics()["epoch"].toString()!=epoch);
        QVERIFY(!client.isOperatorControlEnabled()); QVERIFY(!client.controlDiagnostics()["operations_authorized"].toBool());
        input={}; QTRY_VERIFY(client.controlDiagnostics()["ready"].toBool());
        authorize(client); input.keyboard.pressedKeys=QStringList{"w"}; QVERIFY(client.sendOperatorInput(input));
        QSignalSpy events(&client,&RobotTcpClient::systemStatusReceived);
        QVERIFY(client.sendEmergencyStop("reference-probe"));
        QTRY_VERIFY(!client.isOperatorControlEnabled());
        bool stopped=false;
        QTRY_VERIFY(([&]{for(const auto &args:events){const auto e=args[0].toJsonObject(); if(e["type"]==QJsonValue("emergency_result")&&e["ok"]==QJsonValue(true))stopped=true;}return stopped;})());
        QVERIFY(client.controlDiagnostics()["emergency_active"].toBool());
        client.disconnectFromROS(); input={}; QVERIFY(client.connectToROS("127.0.0.1",port()));
        QTRY_COMPARE(client.connectionState(),RobotTcpClient::ConnectionState::Ready);
        QVERIFY(client.controlDiagnostics()["emergency_active"].toBool()); QVERIFY(!client.setOperatorControlEnabled(true));
        bool clearSent=false; QTRY_VERIFY(([&]{if(!clearSent)clearSent=client.sendSystemCommand("clear_emergency");return clearSent;})());
        QTRY_VERIFY(!client.controlDiagnostics()["emergency_active"].toBool());
        QVERIFY(!client.isOperatorControlEnabled()); QVERIFY(!client.controlDiagnostics()["operations_authorized"].toBool());
    }
    void acceptedResponseLost() {
        RobotTcpClient client; OperatorInputState input; start(client,input); authorize(client,false);
        QVERIFY(client.sendSystemCommand("init")); const auto id=lastOperation(client)["request_id"].toString(); QVERIFY(!id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(client.getStats().connectionCount>=2,8000);
        QTRY_COMPARE(client.connectionState(),RobotTcpClient::ConnectionState::Ready);
        QCOMPARE(lastOperation(client)["state"].toString(),QString("result_unknown"));
        QVERIFY(!client.isOperatorControlEnabled()); QVERIFY(!client.controlDiagnostics()["operations_authorized"].toBool());
        QVERIFY(client.recoverControlRequest(id));
        QTRY_VERIFY(!lastOperation(client)["operation_id"].toString().isEmpty());
        QTRY_COMPARE(lastOperation(client)["state"].toString(),QString("succeeded"));
        QCOMPARE(lastOperation(client)["request_id"].toString(),id);
    }
    void slowBackendAndIndependentEmergency() {
        RobotTcpClient client; OperatorInputState input; start(client,input); authorize(client);
        input.keyboard.pressedKeys=QStringList{"w"}; QVERIFY(client.sendOperatorInput(input));
        QVERIFY(client.sendSystemCommand("init"));
        QTRY_COMPARE(lastOperation(client)["state"].toString(),QString("running"));
        const auto before=client.getStats().messagesReceived;
        QTest::qWait(3000);
        QVERIFY(client.getStats().messagesReceived>before+20); QCOMPARE(client.connectionState(),RobotTcpClient::ConnectionState::Ready);
        QCOMPARE(lastOperation(client)["state"].toString(),QString("running"));
        QElapsedTimer elapsed; elapsed.start(); QSignalSpy events(&client,&RobotTcpClient::systemStatusReceived);
        QVERIFY(client.sendEmergencyStop("slow-worker-probe")); bool accepted=false;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{for(const auto &args:events){const auto e=args[0].toJsonObject();if(e["type"]==QJsonValue("emergency_result")&&e["ok"]==QJsonValue(true))accepted=true;}return accepted;})(),1000);
        QVERIFY(elapsed.elapsed()<1000); QVERIFY(!client.isOperatorControlEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(lastOperation(client)["state"].toString(),QString("succeeded"),35000);
        QVERIFY(lastOperation(client)["invalidated"].toBool()); QVERIFY(!client.isOperatorControlEnabled());
    }
    void repeatedReconnectsNeverRestoreAuthorization() {
        RobotTcpClient client; OperatorInputState input;
        client.configureControlV2("robot24");
        connect(&client,&RobotTcpClient::inputSampleRequested,&client,[&]{client.sendOperatorInput(input);},Qt::QueuedConnection);
        const int cycles=qEnvironmentVariableIntValue("HOST_CONTROL_CYCLES"); QVERIFY(cycles>0);
        for(int cycle=0;cycle<cycles;++cycle) {
            input={}; QVERIFY(client.connectToROS("127.0.0.1",port()));
            QTRY_COMPARE(client.connectionState(),RobotTcpClient::ConnectionState::Ready);
            QVERIFY2(!client.isOperatorControlEnabled(),qPrintable(QString::number(cycle)));
            QVERIFY(!client.controlDiagnostics()["operations_authorized"].toBool());
            bool sent=false; QTRY_VERIFY(([&]{if(!sent)sent=client.setOperatorControlEnabled(true);return sent;})()); QTRY_VERIFY(client.isOperatorControlEnabled());
            input.keyboard.pressedKeys=QStringList{"w"}; bool inputSent=false;
            QTRY_VERIFY(([&]{if(!inputSent)inputSent=client.sendOperatorInput(input);return inputSent;})());
            QTest::qWait(100); QVERIFY(client.controlDiagnostics()["pending_requests"].toInt()<=4);
            client.disconnectFromROS(); QVERIFY(!client.isOperatorControlEnabled());
            QTest::qWait(150); // Let the real receiver/writer retire before the next new connection.
            if ((cycle+1)%10==0) qInfo() << "completed reconnect cycles" << cycle+1;
        }
        QVERIFY(client.getStats().connectionCount>=static_cast<quint64>(cycles));
    }
    void multipleClientsAndInputWatchdog() {
        RobotTcpClient client; OperatorInputState input; bool sample=true;
        client.configureControlV2("robot24");
        connect(&client,&RobotTcpClient::inputSampleRequested,&client,[&]{if(sample)client.sendOperatorInput(input);},Qt::QueuedConnection);
        QVERIFY(client.connectToROS("127.0.0.1",port()));
        QTRY_COMPARE(client.connectionState(),RobotTcpClient::ConnectionState::Ready); authorize(client);
        input.keyboard.pressedKeys=QStringList{"w"}; QTest::qWait(200);
        QTcpSocket monitor; QByteArray bytes;
        connect(&monitor,&QTcpSocket::readyRead,&monitor,[&]{bytes+=monitor.readAll();});
        monitor.connectToHost("127.0.0.1",port()); QTRY_COMPARE(monitor.state(),QAbstractSocket::ConnectedState);
        monitor.write(QJsonDocument(QJsonObject{{"type","control_hello"},{"protocol_version",2},{"request_id","monitor-test"},
            {"robot_id","robot24"},{"role","monitor"},{"capabilities",QJsonArray::fromStringList(ControlProtocol::capabilities())}}).toJson(QJsonDocument::Compact)+'\n');
        QTRY_VERIFY(bytes.contains("monitor-test")); QVERIFY(bytes.contains("\"ok\":true"));
        QVERIFY(client.isOperatorControlEnabled());
        RobotTcpClient contender; contender.configureControlV2("robot24");
        QSignalSpy notices(&contender,&RobotTcpClient::systemStatusReceived);
        QVERIFY(contender.connectToROS("127.0.0.1",port())); bool busy=false;
        QTRY_VERIFY(([&]{for(const auto &args:notices)if(args[0].toJsonObject()["code"]==QJsonValue("OWNER_BUSY"))busy=true;return busy;})());
        QVERIFY(!contender.isOperatorControlEnabled()); QVERIFY(client.isOperatorControlEnabled()); contender.disconnectFromROS();
        const auto before=client.getStats().messagesReceived;
        const auto connections=client.getStats().connectionCount; sample=false;
        QTRY_VERIFY_WITH_TIMEOUT(!client.isOperatorControlEnabled(),1000);
        QVERIFY(client.getStats().messagesReceived>before+5);
        QTRY_COMPARE(client.connectionState(),RobotTcpClient::ConnectionState::Ready);
        QCOMPARE(client.getStats().connectionCount,connections);
        QVERIFY(!client.controlDiagnostics()["operations_authorized"].toBool());
    }
};
QTEST_GUILESS_MAIN(ControlReferenceTest)
#include "control_reference_probe.moc"
