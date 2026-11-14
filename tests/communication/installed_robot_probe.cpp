#include <QtTest>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include "RobotTcpClient.h"
#include "RuntimeService.h"

// Explicit opt-in only. The runner creates a localhost manager with mode=mock.
class InstalledRobotTest : public QObject {
    Q_OBJECT
    QJsonObject lastOperation(Communication::RobotTcpClient &client) {
        const auto ops=client.controlDiagnostics()["operations"].toArray(); return ops.isEmpty()?QJsonObject{}:ops.last().toObject();
    }
    void releaseEmergency(Communication::RobotTcpClient &client) {
        bool clearSent=false;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{if(!clearSent)clearSent=client.sendSystemCommand("clear_emergency");return clearSent;})(),15000);
        QTRY_VERIFY2(!client.controlDiagnostics()["emergency_active"].toBool(),
            qPrintable(QString::fromUtf8(QJsonDocument(client.controlDiagnostics()).toJson())));
        bool authorized=false;
        QTRY_VERIFY(([&]{if(!authorized)authorized=client.authorizeOperations();return authorized;})());
        QTRY_VERIFY(client.controlDiagnostics()["operations_authorized"].toBool());
        bool resetSent=false;
        QTRY_VERIFY(([&]{if(!resetSent)resetSent=client.sendSystemCommand("reset_global_stop");return resetSent;})());
        QTRY_COMPARE_WITH_TIMEOUT(lastOperation(client)["state"].toString(),QString("succeeded"),15000);
        QCOMPARE(lastOperation(client)["command"].toString(),QString("reset_global_stop"));
        QTRY_VERIFY_WITH_TIMEOUT(client.controlDiagnostics()["server"].toObject()["runtime_global_inhibit"]==QJsonValue(false),15000);
        QTRY_VERIFY_WITH_TIMEOUT(client.controlDiagnostics()["ready"].toBool(),5000);
    }
private slots:
    void managementAndControlThroughInstalledRos() {
        QTemporaryDir settings; QVERIFY(settings.isValid());
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,settings.path());
        Utils::Logger::instance().setLogToConsole(false);
        Utils::Logger::instance().setLogToFile(false);
        QFile token(qEnvironmentVariable("HOST_INSTALLED_TOKEN_FILE"));
        QVERIFY(token.open(QIODevice::ReadOnly));
        RuntimeService manager;
        bool watchHealth=false,healthStayedReady=true;
        connect(&manager,&RuntimeService::snapshotReceived,&manager,[&](const QJsonObject &s){
            if(watchHealth && s["software_state"]!=QJsonValue("ready")) {healthStayedReady=false;qWarning()<<"unexpected management health"<<QJsonDocument(s["modules"].toObject()).toJson(QJsonDocument::Compact);}
        });
        QJsonArray profiles; QJsonObject preflight;
        QString host, robot; quint16 port=0;
        connect(&manager,&RuntimeService::profilesReceived,&manager,[&](const QJsonArray &v){profiles=v;});
        connect(&manager,&RuntimeService::preflightReceived,&manager,[&](const QJsonObject &v){preflight=v;});
        connect(&manager,&RuntimeService::errorOccurred,&manager,[](const QString &v){qWarning().noquote()<<v;});
        connect(&manager,&RuntimeService::controlEndpointReady,&manager,
            [&](const QString &h,quint16 p,int protocol,const QString &id){if(protocol==2){host=h;port=p;robot=id;}});
        manager.open("127.0.0.1",qEnvironmentVariableIntValue("HOST_INSTALLED_MANAGER_PORT"),QString::fromUtf8(token.readAll()).trimmed());
        QTRY_VERIFY_WITH_TIMEOUT(!profiles.isEmpty() && manager.canStart(),15000);
        QCOMPARE(profiles.size(),1);
        const auto profile=profiles.first().toObject();
        QCOMPARE(profile["mode"].toString(),QString("mock"));
        const auto requested=QJsonDocument::fromJson(qgetenv("HOST_INSTALLED_PRESETS"));
        const QJsonArray presets=requested.isArray()?requested.array():QJsonArray{"flippers","base","arm"};
        QVERIFY(manager.preflight(profile["profile_id"].toString(),profile["config_revision"].toString(),presets));
        QTRY_VERIFY_WITH_TIMEOUT(!preflight.isEmpty(),15000);
        QVERIFY2(preflight["passed"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(preflight).toJson())));
        QTRY_VERIFY(manager.canStart());
        QVERIFY(manager.start(profile["profile_id"].toString(),profile["config_revision"].toString(),presets));
        QTRY_VERIFY_WITH_TIMEOUT(manager.snapshot()["software_state"]==QJsonValue("ready") && port,150000);
        watchHealth=true;
        QCOMPARE(host,QString("127.0.0.1")); QCOMPARE(robot,QString("robot24"));
        Communication::RobotTcpClient client;
        Communication::OperatorInputState input;
        connect(&client,&Communication::RobotTcpClient::rawMessageReceived,&client,[](const QByteArray &bytes){
            const auto message=QJsonDocument::fromJson(bytes).object();
            if(message["type"]==QJsonValue("control_result") &&
                (message["ok"]==QJsonValue(false) || message["result"].toObject().contains("runtime_release_required")))
                qInfo().noquote()<<QJsonDocument(Communication::ControlProtocol::redact(message).toObject()).toJson(QJsonDocument::Compact);
        });
        connect(&client,&Communication::RobotTcpClient::inputSampleRequested,&client,[&]{client.sendOperatorInput(input);},Qt::QueuedConnection);
        connect(&client,&Communication::RobotTcpClient::inputResetRequested,&client,[&]{input={};});
        client.configureControlV2(robot);
        QVERIFY(client.setPendingPresets(presets));
        QVERIFY(client.connectToROS(host,port));
        QTRY_COMPARE_WITH_TIMEOUT(client.connectionState(),Communication::RobotTcpClient::ConnectionState::Ready,15000);
        QVERIFY(!client.isOperatorControlEnabled());
        if(qEnvironmentVariableIntValue("HOST_INSTALLED_INITIAL_EMERGENCY")) {
            QVERIFY(client.controlDiagnostics()["emergency_active"].toBool()); QVERIFY(!client.startPresets(presets));
            QTRY_VERIFY_WITH_TIMEOUT(client.controlDiagnostics()["server"].toObject()["global_stop"].toObject()["state"]==QJsonValue("succeeded"),5000);
            releaseEmergency(client);
        }
        QVERIFY(client.startPresets(presets));
        QTRY_VERIFY2_WITH_TIMEOUT(client.isOperatorControlEnabled() && !client.controlDiagnostics()["preset_flow_active"].toBool(),
            qPrintable(QString::fromUtf8(QJsonDocument(client.controlDiagnostics()).toJson())),90000);
        const auto initial=client.controlDiagnostics();
        QVERIFY2(!initial["preset_flow_active"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(initial).toJson())));
        QTRY_COMPARE(manager.snapshot()["runtime"].toObject()["diagnostics"].toObject()["configuration_version"].toString(),QString("group-policy-v2"));
        input.keyboard.pressedKeys=QStringList{"w"};
        const auto faultRoot=qEnvironmentVariable("HOST_INSTALLED_FAULT_ROOT");
        if(!faultRoot.isEmpty()) {
            const QString epoch=client.controlDiagnostics()["epoch"].toString();
            QFile request(faultRoot+"/fault-request");QVERIFY(request.open(QIODevice::WriteOnly));request.write("inject");request.close();
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(faultRoot+"/fault-result"),20000);
            QFile result(faultRoot+"/fault-result");QVERIFY(result.open(QIODevice::ReadOnly));QCOMPARE(result.readAll(),QByteArray("ok"));
            QTRY_COMPARE(client.controlDiagnostics()["server"].toObject()["presets"].toObject()["arm"].toObject()["diagnostics"].toObject()["arm"].toObject()["state"].toInt(),3);
            QTRY_COMPARE(client.controlDiagnostics()["server"].toObject()["presets"].toObject()["arm"].toObject()["diagnostics"].toObject()["gripper"].toObject()["state"].toInt(),3);
            QCOMPARE(client.controlDiagnostics()["epoch"].toString(),epoch);
            QVERIFY(client.isOperatorControlEnabled());
        }
        const int hold=qEnvironmentVariableIntValue("HOST_INSTALLED_HOLD_MS");
        QTest::qWait(hold?hold:500);
        auto held=client.controlDiagnostics();held.remove("operations");
        QVERIFY2(client.isOperatorControlEnabled(),qPrintable(QString::fromUtf8(QJsonDocument(Communication::ControlProtocol::redact(held).toObject()).toJson())));
        QVERIFY(healthStayedReady);
        QCOMPARE(manager.snapshot()["software_state"].toString(),QString("ready"));
        input={};
        QVERIFY(client.sendSystemCommand("set_speed_level",{{"level",2}}));
        QTRY_VERIFY_WITH_TIMEOUT(!client.controlDiagnostics()["speed_change_pending"].toBool()
            && client.controlDiagnostics()["server"].toObject()["speed_level"]==QJsonValue(2),15000);
        QCOMPARE(lastOperation(client)["command"].toString(),QString("set_speed_level"));
        QCOMPARE(lastOperation(client)["state"].toString(),QString("succeeded"));
        QVERIFY(!client.isOperatorControlEnabled());
        QVERIFY(client.sendEmergencyStop("installed-mock-integration"));
        QTRY_VERIFY_WITH_TIMEOUT(client.controlDiagnostics()["server"].toObject()["runtime_global_inhibit"]==QJsonValue(true)
            && client.controlDiagnostics()["server"].toObject()["global_stop"].toObject()["state"]==QJsonValue("succeeded"),5000);
        releaseEmergency(client);
        QVERIFY(!client.isOperatorControlEnabled());
        client.disconnectFromROS();
        QVERIFY(healthStayedReady);watchHealth=false;
        QTRY_VERIFY(manager.canStop()); QVERIFY(manager.stop());
        QTRY_VERIFY_WITH_TIMEOUT(manager.canStart() && manager.snapshot()["software_state"]==QJsonValue("stopped"),60000);
        manager.close();
    }
};
QTEST_GUILESS_MAIN(InstalledRobotTest)
#include "installed_robot_probe.moc"
