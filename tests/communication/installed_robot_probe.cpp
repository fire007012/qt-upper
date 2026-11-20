#include <QtTest>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include "RobotTcpClient.h"
#include "RuntimeService.h"

// Explicit opt-in only. The runner creates a localhost manager with mode=mock.
class InstalledRobotTest : public QObject {
    Q_OBJECT
    QJsonObject lastOperation(Communication::RobotTcpClient &client, const QString &command = {}) {
        QJsonObject latest;
        for (const auto &value : client.controlDiagnostics()["operations"].toArray()) {
            const auto op = value.toObject();
            if (!command.isEmpty() && op["command"] != QJsonValue(command)) continue;
            if (latest.isEmpty() || op["created_at"].toDouble() >= latest["created_at"].toDouble()) latest = op;
        }
        return latest;
    }
    void armCommand(Communication::RobotTcpClient &client, const QString &command, const QJsonObject &params) {
        bool sent=false;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{if(!sent)sent=client.sendSystemCommand(command,params);return sent;})(),15000);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{auto op=lastOperation(client, command);return op["command"]==command &&
            (op["state"]==QJsonValue("succeeded") || op["state"]==QJsonValue("failed") || op["state"]==QJsonValue("result_unknown"));})(),90000);
        QVERIFY2(lastOperation(client, command)["state"]==QJsonValue("succeeded"),
            qPrintable(QString::fromUtf8(QJsonDocument(lastOperation(client, command)).toJson())));
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
        QTRY_COMPARE_WITH_TIMEOUT(lastOperation(client,"reset_global_stop")["state"].toString(),QString("succeeded"),15000);
        QCOMPARE(lastOperation(client,"reset_global_stop")["command"].toString(),QString("reset_global_stop"));
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
        Communication::OperatorInputState input;  // Outlive client's disconnect/reset signals.
        Communication::RobotTcpClient client;
        connect(&client,&Communication::RobotTcpClient::connectionError,&client,[](const QString &error){qWarning().noquote()<<"connection error"<<error;});
        connect(&client,&Communication::RobotTcpClient::rawMessageReceived,&client,[](const QByteArray &bytes){
            const auto message=QJsonDocument::fromJson(bytes).object();
            if(message["type"]==QJsonValue("control_result"))
                qInfo().noquote()<<QJsonDocument(Communication::ControlProtocol::redact(message).toObject()).toJson(QJsonDocument::Compact);
        });
        connect(&client,&Communication::RobotTcpClient::inputSampleRequested,&client,[&]{client.sendOperatorInput(input);},Qt::QueuedConnection);
        connect(&client,&Communication::RobotTcpClient::inputResetRequested,&client,[&]{input={};});
        QTimer diagnostic;
        connect(&diagnostic, &QTimer::timeout, &client, [&] {
            const auto root = qEnvironmentVariable("HOST_INSTALLED_ARM_ROOT");
            if (root.isEmpty()) return;
            QFile file(root+"/qt-diagnostics.json");
            if(file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(Communication::ControlProtocol::redact(client.controlDiagnostics()).toObject()).toJson());
        });
        diagnostic.start(1000);
        client.configureControlV2(robot);
        QVERIFY(client.setPendingPresets(presets));
        QVERIFY(client.connectToROS(host,port));
        QTRY_COMPARE_WITH_TIMEOUT(client.connectionState(),Communication::RobotTcpClient::ConnectionState::Ready,15000);
        QVERIFY(!client.isOperatorControlEnabled());
        if(qEnvironmentVariableIntValue("HOST_INSTALLED_INITIAL_EMERGENCY")) {
            QVERIFY(client.controlDiagnostics()["emergency_active"].toBool()); QVERIFY(!client.startPresets(presets));
            QTRY_VERIFY_WITH_TIMEOUT(client.controlDiagnostics()["server"].toObject()["global_stop"].toObject()["state"]==QJsonValue("succeeded"),5000);
            releaseEmergency(client);
        QVERIFY(!QTest::currentTestFailed());
        }
        QVERIFY(client.startPresets(presets));
        QTRY_VERIFY2_WITH_TIMEOUT((client.isOperatorControlEnabled() && !client.controlDiagnostics()["preset_flow_active"].toBool())
            || lastOperation(client)["state"]==QJsonValue("result_unknown"),
            qPrintable(QString::fromUtf8(QJsonDocument(client.controlDiagnostics()).toJson())),20000);
        QVERIFY2(lastOperation(client)["state"]!=QJsonValue("result_unknown"),
            qPrintable(QString::fromUtf8(QJsonDocument(lastOperation(client)).toJson())));
        QVERIFY(client.isOperatorControlEnabled());
        const auto initial=client.controlDiagnostics();
        QVERIFY2(!initial["preset_flow_active"].toBool(),qPrintable(QString::fromUtf8(QJsonDocument(initial).toJson())));
        QTRY_COMPARE(manager.snapshot()["runtime"].toObject()["diagnostics"].toObject()["configuration_version"].toString(),QString("group-policy-v2"));
        const QString armRoot=qEnvironmentVariable("HOST_INSTALLED_ARM_ROOT");
        if (!armRoot.isEmpty()) {
            QCOMPARE(presets,QJsonArray{"arm"});
            QTest::qWait(400);
            QTRY_VERIFY2_WITH_TIMEOUT(([&]{client.authorizeOperations();return client.controlDiagnostics()["operations_authorized"].toBool();})(),
                qPrintable(QString::fromUtf8(QJsonDocument(Communication::ControlProtocol::redact(client.controlDiagnostics()).toObject()).toJson())),15000);
            armCommand(client,"move_arm_named_target",{{"target","ready"}});
            QVERIFY(!QTest::currentTestFailed());
            QTest::qWait(400);
            QTRY_VERIFY(([&]{client.authorizeOperations();return client.controlDiagnostics()["operations_authorized"].toBool();})());
            armCommand(client,"set_control_mode",{{"mode","arm"}});
            QVERIFY(!QTest::currentTestFailed());
            input.mode="arm";
            QTest::qWait(400);
            QTRY_VERIFY(([&]{client.authorizeOperations();return client.controlDiagnostics()["operations_authorized"].toBool();})());
            QTRY_VERIFY(([&]{client.setOperatorControlEnabled(true);return client.isOperatorControlEnabled();})());
            QFile marker(armRoot+"/arm-jog-start"); QVERIFY(marker.open(QIODevice::WriteOnly)); marker.write("start"); marker.close();
            input.keyboard.pressedKeys=QStringList{"l","f"};
            QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(armRoot+"/arm-motion-confirmed"), qPrintable(QString::fromUtf8(QJsonDocument(Communication::ControlProtocol::redact(client.controlDiagnostics()).toObject()).toJson())),10000);
            input={}; input.mode="arm";
            QFile neutral(armRoot+"/arm-neutral-start"); QVERIFY(neutral.open(QIODevice::WriteOnly)); neutral.write("neutral"); neutral.close();
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(armRoot+"/arm-stop-confirmed"),5000);
        } else {
            input.keyboard.pressedKeys=QStringList{"w"};
        }
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
        QCOMPARE(lastOperation(client,"set_speed_level")["command"].toString(),QString("set_speed_level"));
        QTRY_COMPARE(lastOperation(client,"set_speed_level")["state"].toString(),QString("succeeded"));
        QVERIFY(!client.isOperatorControlEnabled());
        QVERIFY(client.sendEmergencyStop("installed-mock-integration"));
        QTRY_VERIFY_WITH_TIMEOUT(client.controlDiagnostics()["server"].toObject()["runtime_global_inhibit"]==QJsonValue(true)
            && client.controlDiagnostics()["server"].toObject()["global_stop"].toObject()["state"]==QJsonValue("succeeded"),5000);
        releaseEmergency(client);
        QVERIFY(!QTest::currentTestFailed());
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
