#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QPlainTextEdit>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QMessageBox>
#include <QAbstractButton>
#include <QFile>
#include <QScopeGuard>
#include <QDialog>
#include <QComboBox>
#include "ConfigurationWidget.h"
#include "configuration_stub.h"
#include "RuntimeManagementWidget.h"
#include "manager_stub.h"

class V2ManagerStub : public ManagerStub {
public:
    bool recoveryRequired=false;
    int snapshotDelayMs = 0;
    bool keepActiveProfileOnStart = false;
    void respond(QTcpSocket *peer, const QJsonObject &request) override {
        if (request["method"] != QJsonValue("snapshot")) {
            const auto profile = activeProfile, revision = activeRevision;
            ManagerStub::respond(peer,request);
            if (keepActiveProfileOnStart && request["method"] == QJsonValue("start")) { activeProfile = profile; activeRevision = revision; }
            return;
        }
        QJsonObject modules{{"core",QJsonObject{{"ok",true}}},{"bridge",QJsonObject{{"ok",true}}}};
        QJsonObject snapshot{{"software_state",software},{"profile_id",activeProfile},{"config_revision",activeRevision},
            {"operation_id",operationId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(operationId)},
            {"recovery_required",recoveryRequired},{"modules",modules},{"control_endpoint",QJsonObject{{"host","127.0.0.1"},{"port",9090},{"protocol_version",2}}}};
        const auto bytes = QJsonDocument(QJsonObject{{"version",1},{"request_id",request["request_id"]},{"ok",true},{"result",snapshot}}).toJson(QJsonDocument::Compact)+'\n';
        QTimer::singleShot(snapshotDelayMs, peer, [peer, bytes] { peer->write(bytes); });
    }
};
class RecoveryStopStub : public V2ManagerStub {
public:
    void respond(QTcpSocket *peer, const QJsonObject &request) override {
        V2ManagerStub::respond(peer, request);
        if (request["method"] == QJsonValue("stop")) software = "stopping";
    }
};
class ManagementWidgetTest : public QObject {
    Q_OBJECT
    QTemporaryDir testFiles;
    QByteArray previousLogDirectory;
    bool hadLogDirectory = false;
private slots:
    void initTestCase() {
        QVERIFY(testFiles.isValid());
        hadLogDirectory=qEnvironmentVariableIsSet("HOSTCOMPUTER_RUNTIME_LOG_DIR");
        previousLogDirectory=qgetenv("HOSTCOMPUTER_RUNTIME_LOG_DIR");
        qputenv("HOSTCOMPUTER_RUNTIME_LOG_DIR",testFiles.filePath("runtime").toUtf8());
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,testFiles.path());
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,testFiles.path());
    }
    void cleanupTestCase() {
        if(hadLogDirectory)qputenv("HOSTCOMPUTER_RUNTIME_LOG_DIR",previousLogDirectory);else qunsetenv("HOSTCOMPUTER_RUNTIME_LOG_DIR");
    }
    void init() {
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,testFiles.path());
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,testFiles.path());
        QSettings settings("HostComputer", "Management"); settings.clear(); settings.setValue("desired_mode", "mock");
    }
    void minimalPageKeepsDetailedInformationInFiles() {
        RuntimeManagementWidget widget; widget.resize(400,520); widget.show();
        auto *settings=widget.findChild<QDialog *>("managementSettingsDialog");
        auto *controls=widget.findChild<QDialog *>("managementControlsDialog");
        QVERIFY(settings); QVERIFY(controls); QVERIFY(!settings->isVisible()); QVERIFY(!controls->isVisible());
        QVERIFY(!widget.findChild<QLineEdit *>("managementToken")->isVisible());
        for(auto *details:widget.findChildren<QPlainTextEdit *>()) QVERIFY(!details->isVisible());
        QVERIFY(!widget.findChild<QPlainTextEdit *>("managementLog"));
        QVERIFY(!widget.findChild<QLabel *>("fieldControlState"));
        widget.findChild<QPushButton *>("managementSettings")->click(); QVERIFY(settings->isVisible()); settings->hide();
        widget.findChild<QPushButton *>("managementMoreControls")->click(); QVERIFY(controls->isVisible());
        if(!qEnvironmentVariable("HOST_TEST_CONTROLS_SCREENSHOT").isEmpty())
            QVERIFY(controls->grab().save(qEnvironmentVariable("HOST_TEST_CONTROLS_SCREENSHOT")));
        controls->hide();
        widget.reportActionFailure("TEST_ERROR: detailed protocol failure");
        QVERIFY(widget.findChild<QLabel *>("presetState")->text().contains("操作未提交"));
        QVERIFY(!widget.findChild<QLabel *>("presetState")->text().contains("TEST_ERROR"));
        QFile log(testFiles.filePath("runtime/runtime.jsonl")); QVERIFY(log.open(QIODevice::ReadOnly)); QVERIFY(log.readAll().contains("TEST_ERROR"));
        QTRY_VERIFY(QFile::exists(testFiles.filePath("runtime/latest.json")));
        if(!qEnvironmentVariable("HOST_TEST_WIDGET_SCREENSHOT").isEmpty()) {
            RuntimeManagementWidget preview; preview.resize(400,520); preview.show();
            QVERIFY(preview.grab().save(qEnvironmentVariable("HOST_TEST_WIDGET_SCREENSHOT")));
        }
    }
    void unavailableLogStorageShowsOneShortWarning() {
        const auto previous=qgetenv("HOSTCOMPUTER_RUNTIME_LOG_DIR");
        const auto restore=qScopeGuard([&]{qputenv("HOSTCOMPUTER_RUNTIME_LOG_DIR",previous);});
        QFile blocker(testFiles.filePath("blocked")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        qputenv("HOSTCOMPUTER_RUNTIME_LOG_DIR",blocker.fileName().toUtf8());
        RuntimeManagementWidget widget;
        const auto text=widget.findChild<QLabel *>("presetState")->text();
        QVERIFY(text.contains("日志保存失败")); QVERIFY(text.size()<50);
    }
    void motionPresetsUpdateTheCompleteDraftWithoutStartingAnything() {
        RuntimeManagementWidget widget;
        auto *preset=widget.findChild<QComboBox *>("motionPreset"); QVERIFY(preset);
        QCOMPARE(preset->count(),4);
        const QList<QJsonArray> expected{{"base"},{"arm"},{"flippers","base","arm"},{"flippers","base"}};
        QSignalSpy changes(&widget,&RuntimeManagementWidget::pendingPresetsChanged);
        QSignalSpy starts(&widget,&RuntimeManagementWidget::startPresetsRequested);
        QSignalSpy selections(&widget,&RuntimeManagementWidget::selectPresetsRequested);
        for(int i=0;i<expected.size();++i) {
            preset->setCurrentIndex(i); QCOMPARE(widget.pendingPresets(),expected[i]);
            QCOMPARE(changes.size(),i+1); QCOMPARE(changes.last()[0].value<QJsonArray>(),expected[i]);
        }
        QCOMPARE(starts.size(),0); QCOMPARE(selections.size(),0);
        widget.findChild<QCheckBox *>("preset_base")->setChecked(false);
        QCOMPARE(widget.pendingPresets(),QJsonArray{"flippers"}); QCOMPARE(preset->currentIndex(),-1);
        widget.findChild<QPushButton *>("presetAll")->click(); QCOMPARE(preset->currentIndex(),2);
        const auto services=widget.findChildren<RuntimeService *>(); QCOMPARE(services.size(),1);
        QVERIFY(!services.first()->canQuery());
    }
    void emergencyRecoveryRemainsExplicitAndSeparateFromSoftwareRecovery() {
        RuntimeManagementWidget widget;
        QJsonObject status{{"emergency_active",true},{"runtime_global_inhibit",true},{"runtime_fresh",true},
            {"global_stop_reset_ready",true},{"global_stop",QJsonObject{{"state","succeeded"},{"ok",true}}}};
        QJsonObject diagnostics{{"ready",true},{"negotiated",true},{"motion_presets_supported",true},
            {"global_stop_supported",true},{"emergency_active",true},{"server",status}};
        QSignalSpy commands(&widget,&RuntimeManagementWidget::fieldCommandRequested);
        QSignalSpy authorizations(&widget,&RuntimeManagementWidget::authorizeOperationsRequested);
        widget.setControlDiagnostics(diagnostics);
        QCOMPARE(commands.size(),0);QCOMPARE(authorizations.size(),0);
        QVERIFY(widget.findChild<QLabel *>("presetState")->text().contains("急停仍然锁存"));
        auto *clear=widget.findChild<QPushButton *>("recoveryClearEmergency");QVERIFY(clear->isEnabled());
        QTimer::singleShot(0,[] {auto *box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget());if(box)box->button(QMessageBox::Yes)->click();});
        clear->click();QCOMPARE(commands.size(),1);QCOMPARE(commands[0][0].toString(),QString("clear_emergency"));
        status["emergency_active"]=false;diagnostics["emergency_active"]=false;diagnostics["server"]=status;
        widget.setControlDiagnostics(diagnostics);
        widget.findChild<QPushButton *>("recoveryAuthorize")->click();QCOMPARE(authorizations.size(),1);
        QVERIFY(!widget.findChild<QPushButton *>("globalReset")->isEnabled());
        diagnostics["operations_authorized"]=true;widget.setControlDiagnostics(diagnostics);
        QVERIFY(widget.findChild<QPushButton *>("globalReset")->isEnabled());QCOMPARE(commands.size(),1);
    }
    void unknownEmergencyIsNotReportedAsLatched() {
        RuntimeManagementWidget widget;
        widget.setControlDiagnostics({{"negotiated",true},{"motion_presets_supported",true},{"emergency_active",true}});
        QVERIFY(widget.findChild<QLabel *>("presetState")->text().contains("急停状态未知"));
        QVERIFY(widget.findChild<QPushButton *>("emergencyHelp")->isHidden());
        QVERIFY(!widget.findChild<QPushButton *>("recoveryClearEmergency")->isEnabled());
        widget.setControlDiagnostics({{"ready",true},{"emergency_active",true},{"server",QJsonObject{
            {"emergency_active",true},{"runtime_fresh",true},{"runtime_global_inhibit",true},
            {"global_stop",QJsonObject{{"state","result_unknown"},{"code","SERVICE_UNAVAILABLE"}}}}}});
        QVERIFY(widget.findChild<QLabel *>("emergencyRecoveryHint")->text().contains("SERVICE_UNAVAILABLE"));
    }
    void missingHardwareProfileExplainsWhyMotorsCannotRun() {
        V2ManagerStub manager; QVERIFY(manager.listen()); RuntimeManagementWidget widget; widget.show();
        auto *mode = widget.findChild<QComboBox *>("managementMode"); mode->setCurrentIndex(mode->findData("hardware"));
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *hint=widget.findChild<QLabel *>("hardwareDeploymentHint");
        QTRY_VERIFY(hint->isVisible());
        QVERIFY(hint->text().contains("未提供实机档案"));
        QCOMPARE(widget.findChild<QComboBox *>("managementProfiles")->count(),0);
        QCOMPARE(mode->currentData().toString(), QString("hardware"));
        QVERIFY(!widget.findChild<QPushButton *>("managementStart")->isEnabled());
        QCOMPARE(manager.starts,0);
        manager.profileList.append(QJsonObject{{"profile_id","hardware"},{"mode","hardware"},
            {"config_revision","rev-H"},{"required_modules",QJsonArray{"core","bridge"}}});
        widget.findChild<RuntimeService *>()->refreshProfiles();
        QTRY_COMPARE(widget.findChild<QComboBox *>("managementProfiles")->count(),1);
        QTRY_VERIFY(hint->isHidden());
    }
    void selectedEnvironmentDoesNotRelabelRunningMock() {
        V2ManagerStub manager; manager.software="ready"; QVERIFY(manager.listen());
        manager.profileList.append(QJsonObject{{"profile_id","hardware"},{"mode","hardware"},
            {"config_revision","rev-H"},{"required_modules",QJsonArray{"core","bridge"}}});
        QSettings settings("HostComputer","Management");
        settings.setValue("desired_mode", "hardware");
        RuntimeManagementWidget widget;
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *profiles=widget.findChild<QComboBox *>("managementProfiles");
        QTRY_COMPARE(profiles->count(),1);
        QCOMPARE(profiles->currentData().toJsonObject()["profile_id"].toString(),QString("hardware"));
        QTRY_VERIFY(widget.findChild<QLabel *>("managementEnvironment")->text().contains("当前：mock"));
        QTRY_VERIFY(widget.findChild<QLabel *>("hardwareDeploymentHint")->text().contains("先点击“停止系统”"));
        QVERIFY(widget.findChild<QLabel *>("managementEnvironment")->text().contains("准备启动：hardware"));
        QTRY_VERIFY(widget.findChild<QComboBox *>("managementMode")->isEnabled());
        QVERIFY(!profiles->isEnabled());
        widget.setControlDiagnostics({{"ready",true},{"motion_presets_supported",true}});
        QVERIFY(!widget.findChild<QPushButton *>("managementStart")->isEnabled());
        QCOMPARE(manager.starts,0);
    }
    void chooseEnvironmentBeforeConnecting_data() {
        QTest::addColumn<QString>("targetMode"); QTest::addColumn<bool>("reverseOrder");
        QTest::newRow("mock-first") << QString("mock") << false;
        QTest::newRow("mock-reordered") << QString("mock") << true;
        QTest::newRow("hardware-first") << QString("hardware") << false;
        QTest::newRow("hardware-reordered") << QString("hardware") << true;
    }
    void chooseEnvironmentBeforeConnecting() {
        QFETCH(QString, targetMode); QFETCH(bool, reverseOrder);
        QSettings settings("HostComputer", "Management"); settings.remove("desired_mode");
        V2ManagerStub manager; QVERIFY(manager.listen());
        const QJsonObject hardware{{"profile_id","field-vehicle"},{"mode","hardware"},
            {"config_revision","rev-H"},{"required_modules",QJsonArray{"core","bridge"}}};
        if (reverseOrder) manager.profileList.prepend(hardware); else manager.profileList.append(hardware);
        RuntimeManagementWidget widget;
        auto *mode = widget.findChild<QComboBox *>("managementMode");
        auto *start = widget.findChild<QPushButton *>("managementStart");
        QVERIFY(mode->isEnabled()); QCOMPARE(mode->currentIndex(), -1); QVERIFY(!start->isEnabled());
        mode->setCurrentIndex(mode->findData(targetMode));
        QVERIFY(widget.findChild<QLabel *>("managementEnvironment")->text().contains(targetMode == "mock" ? "模拟" : "实机"));
        QCOMPARE(manager.requests.size(), 0);
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        QTRY_VERIFY(start->isEnabled()); QCOMPARE(mode->currentData().toString(), targetMode);
        auto *profiles = widget.findChild<QComboBox *>("managementProfiles");
        QCOMPARE(profiles->count(), 1);
        QCOMPARE(profiles->currentData().toJsonObject()["mode"].toString(), targetMode);
        QCOMPARE(manager.starts, 0);
        widget.findChild<QPushButton *>("managementPreflight")->click();
        QTRY_VERIFY(start->isEnabled()); start->click(); QTRY_COMPARE(manager.starts, 1);
        QSet<QString> checked;
        for (const auto &request : manager.requests) {
            const auto method = request["method"].toString();
            if (method != "start" && method != "preflight") continue;
            const auto params = request["params"].toObject();
            QCOMPARE(params["profile_id"].toString(), targetMode == "mock" ? QString("mock") : QString("field-vehicle"));
            QCOMPARE(params["config_revision"].toString(), targetMode == "mock" ? QString("rev-A") : QString("rev-H"));
            checked.insert(method);
        }
        QCOMPARE(checked, QSet<QString>({"start", "preflight"}));
        QTRY_VERIFY(widget.findChild<QLabel *>("managementEnvironment")->text().contains("当前："));
    }
    void runningEnvironmentRequiresExplicitStopBeforeSwitch_data() {
        QTest::addColumn<QString>("targetMode");
        QTest::newRow("mock-to-hardware") << QString("hardware");
        QTest::newRow("hardware-to-mock") << QString("mock");
    }
    void runningEnvironmentRequiresExplicitStopBeforeSwitch() {
        QFETCH(QString, targetMode);
        V2ManagerStub manager; manager.software = "ready";
        manager.profileList.append(QJsonObject{{"profile_id","hardware"},{"mode","hardware"},
            {"config_revision","rev-H"},{"required_modules",QJsonArray{"core","bridge"}}});
        if (targetMode == "mock") { manager.activeProfile = "hardware"; manager.activeRevision = "rev-H"; }
        QVERIFY(manager.listen()); RuntimeManagementWidget widget;
        QSignalSpy endpoints(&widget, &RuntimeManagementWidget::controlEndpointReady);
        auto *mode = widget.findChild<QComboBox *>("managementMode"); mode->setCurrentIndex(mode->findData(targetMode));
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *stop = widget.findChild<QPushButton *>("managementStop");
        auto *start = widget.findChild<QPushButton *>("managementStart");
        QTRY_VERIFY(stop->isEnabled()); QVERIFY(mode->isEnabled()); QCOMPARE(endpoints.size(), 0);
        widget.setControlDiagnostics({{"ready",true},{"motion_presets_supported",true}});
        QSignalSpy groups(&widget, &RuntimeManagementWidget::startPresetsRequested);
        QVERIFY(!start->isEnabled()); start->click(); QCOMPARE(groups.size(), 0); QCOMPARE(manager.starts, 0);
        QVERIFY(widget.findChild<QLabel *>("managementEnvironment")->text().contains("准备启动：" + targetMode));
        stop->click(); QTRY_VERIFY(start->isEnabled());
        QCOMPARE(mode->currentData().toString(), targetMode); QCOMPARE(manager.starts, 0); QCOMPARE(manager.stops, 1);
        start->click(); QTRY_COMPARE(manager.starts, 1);
        QTRY_COMPARE(endpoints.size(), 1);
        QCOMPARE(manager.activeProfile, targetMode);
        QCOMPARE(mode->currentData().toString(), targetMode);
        QCOMPARE(groups.size(), 0);
    }
    void connectionWithoutEnvironmentDoesNotAdoptRunningHardware() {
        QSettings settings("HostComputer", "Management"); settings.remove("desired_mode");
        V2ManagerStub manager; manager.software = "ready"; manager.activeProfile = "hardware"; manager.activeRevision = "rev-H";
        manager.profileList = {QJsonObject{{"profile_id","hardware"},{"mode","hardware"},
            {"config_revision","rev-H"},{"required_modules",QJsonArray{"core","bridge"}}}};
        QVERIFY(manager.listen()); RuntimeManagementWidget widget;
        QSignalSpy endpoints(&widget, &RuntimeManagementWidget::controlEndpointReady);
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        QTRY_VERIFY(widget.findChild<RuntimeService *>()->canStop());
        auto *mode = widget.findChild<QComboBox *>("managementMode");
        QCOMPARE(mode->currentIndex(), -1); QCOMPARE(endpoints.size(), 0);
        QVERIFY(!widget.findChild<QPushButton *>("managementStart")->isEnabled());
        mode->setCurrentIndex(mode->findData("mock"));
        QCOMPARE(mode->currentData().toString(), QString("mock")); QCOMPARE(endpoints.size(), 0);
        QVERIFY(!widget.findChild<QPushButton *>("managementStart")->isEnabled());
        mode->setCurrentIndex(mode->findData("hardware")); QCOMPARE(endpoints.size(), 1);
        QSignalSpy disconnected(&widget, &RuntimeManagementWidget::controlUnavailable);
        mode->setCurrentIndex(mode->findData("mock")); QCOMPARE(disconnected.size(), 1);
        QCOMPARE(manager.starts, 0); QCOMPARE(manager.stops, 0);
    }
    void wrongRuntimeAfterStartNeverConnectsOrStartsGroups() {
        V2ManagerStub manager; manager.keepActiveProfileOnStart = true;
        manager.activeProfile = "hardware"; manager.activeRevision = "rev-H";
        manager.profileList.append(QJsonObject{{"profile_id","hardware"},{"mode","hardware"},
            {"config_revision","rev-H"},{"required_modules",QJsonArray{"core","bridge"}}});
        QVERIFY(manager.listen()); RuntimeManagementWidget widget;
        QSignalSpy endpoints(&widget, &RuntimeManagementWidget::controlEndpointReady), groups(&widget, &RuntimeManagementWidget::startPresetsRequested);
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *start = widget.findChild<QPushButton *>("managementStart"); QTRY_VERIFY(start->isEnabled());
        start->click(); QTRY_VERIFY(widget.findChild<RuntimeService *>()->canStop());
        widget.setControlDiagnostics({{"ready",true},{"motion_presets_supported",true},{"robot_id","robot24"},
            {"endpoint","127.0.0.1:9090"},{"connection_id","unexpected-hardware"}});
        QCOMPARE(endpoints.size(), 0); QCOMPARE(groups.size(), 0); QVERIFY(!start->isEnabled());
        QCOMPARE(widget.findChild<QComboBox *>("managementMode")->currentData().toString(), QString("mock"));
    }
    void pollingAndProfileRefreshKeepTheChosenEnvironment() {
        V2ManagerStub manager; QVERIFY(manager.listen()); RuntimeManagementWidget widget;
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *service = widget.findChild<RuntimeService *>();
        auto *mode = widget.findChild<QComboBox *>("managementMode");
        auto *profiles = widget.findChild<QComboBox *>("managementProfiles");
        auto *start = widget.findChild<QPushButton *>("managementStart");
        QTRY_VERIFY(start->isEnabled()); manager.snapshotDelayMs = 300;
        QTRY_VERIFY(!service->canQuery()); QVERIFY(mode->isEnabled());
        QTRY_VERIFY(start->isEnabled());
        manager.profileList.prepend(QJsonObject{{"profile_id","hardware"},{"mode","hardware"},
            {"config_revision","rev-H"},{"required_modules",QJsonArray{"core","bridge"}}});
        service->refreshProfiles();
        QTRY_VERIFY(service->canStart());
        QCOMPARE(mode->currentData().toString(), QString("mock"));
        QCOMPARE(profiles->currentData().toJsonObject()["profile_id"].toString(), QString("mock"));
        manager.profileList.removeLast(); service->refreshProfiles();
        QTRY_COMPARE(profiles->count(), 0); QTRY_VERIFY(service->canStart());
        QCOMPARE(mode->currentData().toString(), QString("mock")); QVERIFY(!start->isEnabled());
        manager.profileList.append(QJsonObject{{"profile_id","replacement-mock"},{"mode","mock"},
            {"config_revision","rev-B"},{"required_modules",QJsonArray{"core","bridge"}}});
        service->refreshProfiles(); QTRY_COMPARE(profiles->count(), 1); QTRY_VERIFY(service->canStart());
        QCOMPARE(profiles->currentIndex(), -1); QVERIFY(!start->isEnabled());
        QVERIFY(profiles->isEnabled()); profiles->setCurrentIndex(0); QVERIFY(start->isEnabled());
        widget.findChild<QPushButton *>("managementDisconnect")->click();
        QVERIFY(mode->isEnabled()); QCOMPARE(mode->currentData().toString(), QString("mock"));
        QCOMPARE(manager.starts, 0);
    }
    void unfinishedPreviousRunHasAnAccurateRecoveryPrompt() {
        V2ManagerStub manager; manager.recoveryRequired=true;manager.software="failed";QVERIFY(manager.listen());
        RuntimeManagementWidget widget;
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *summary=widget.findChild<QLabel *>("presetState");
        QTRY_VERIFY(summary->text().contains("上次运行未正常结束"));
        QVERIFY(!widget.findChild<QPushButton *>("managementStart")->isEnabled());
        QTRY_VERIFY(widget.findChild<QPushButton *>("managementStop")->isEnabled());
    }
    void stopProgressAndFailureOverrideTheRecoveryPrompt_data() {
        QTest::addColumn<QString>("diagnostic"); QTest::addColumn<QString>("expected");
        QTest::newRow("ownership") << QString("{\"halt_result\":\"runtime ownership reconciliation in progress\"}") << QString("核对资源归属");
        QTest::newRow("unavailable") << QString("{\"halt_result\":\"service unavailable\"}") << QString("停止服务不可用");
        QTest::newRow("unconfirmed") << QString("{\"halt_result\":\"no confirmed stopped state\"}") << QString("残留进程尚未清理");
    }
    void stopProgressAndFailureOverrideTheRecoveryPrompt() {
        QFETCH(QString, diagnostic); QFETCH(QString, expected);
        RecoveryStopStub manager; manager.recoveryRequired = true; manager.software = "failed";
        manager.operationId = "interrupted-start"; manager.taskState = "failed"; manager.taskError = "RECOVERY_REQUIRED";
        QVERIFY(manager.listen()); RuntimeManagementWidget widget;
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *stop = widget.findChild<QPushButton *>("managementStop");
        auto *start = widget.findChild<QPushButton *>("managementStart");
        auto *summary = widget.findChild<QLabel *>("presetState");
        QTRY_VERIFY(stop->isEnabled()); QVERIFY(summary->text().contains("上次运行未正常结束"));
        manager.taskState = "running"; manager.taskError.clear(); stop->click();
        QVERIFY(summary->text().contains("正在停止"));
        QTRY_COMPARE(manager.stops, 1); QTest::qWait(1100);
        QVERIFY(summary->text().contains("正在停止")); QVERIFY(!stop->isEnabled()); QVERIFY(!start->isEnabled());
        manager.taskState = "failed"; manager.taskError = "STOP_UNCONFIRMED"; manager.taskDiagnostic = diagnostic; manager.software = "failed";
        QTRY_VERIFY(summary->text().contains("STOP_UNCONFIRMED")); QVERIFY(summary->text().contains(expected));
        QVERIFY(!summary->text().contains("先停止系统完成恢复")); QVERIFY(!start->isEnabled());
        QTRY_VERIFY(stop->isEnabled()); QCOMPARE(stop->text(), QString("重试停止系统")); QCOMPARE(manager.stops, 1);
        manager.taskState = "running"; manager.taskError.clear(); manager.taskDiagnostic.clear(); stop->click();
        QTRY_COMPARE(manager.stops, 2); QVERIFY(summary->text().contains("正在停止"));
        manager.taskState = "succeeded"; manager.software = "stopped"; manager.recoveryRequired = false;
        QTRY_VERIFY(start->isEnabled()); QVERIFY(!summary->text().contains("STOP_UNCONFIRMED"));
        QCOMPARE(stop->text(), QString("停止系统")); QCOMPARE(manager.starts, 0);
    }
    void deployedManagerEnvironmentAndPreflight() {
        const auto config=qgetenv("HOST_TEST_DEPLOYED_CONFIG");
        if(config.isEmpty()) QSKIP("Explicit installed-manager opt-in required");
        const auto previous=qgetenv("HOSTCOMPUTER_MANAGEMENT_CONFIG");
        const bool existed=qEnvironmentVariableIsSet("HOSTCOMPUTER_MANAGEMENT_CONFIG");
        const auto restore=qScopeGuard([&]{if(existed)qputenv("HOSTCOMPUTER_MANAGEMENT_CONFIG",previous);else qunsetenv("HOSTCOMPUTER_MANAGEMENT_CONFIG");});
        qputenv("HOSTCOMPUTER_MANAGEMENT_CONFIG",config);
        RuntimeManagementWidget widget; widget.show();
        auto *mode=widget.findChild<QComboBox *>("managementMode");
        auto *profiles=widget.findChild<QComboBox *>("managementProfiles");
        auto *service=widget.findChild<RuntimeService *>();
        QVERIFY(mode->isEnabled()); QVERIFY(!service->canQuery());
        mode->setCurrentIndex(mode->findData("hardware"));
        widget.findChild<QComboBox *>("motionPreset")->setCurrentIndex(2);
        QSignalSpy mutations(service,&RuntimeService::mutationIssued);
        QSignalSpy preflight(service,&RuntimeService::preflightReceived);
        widget.findChild<QPushButton *>("managementConnect")->click();
        QTRY_VERIFY_WITH_TIMEOUT(service->canStart(),15000);
        QCOMPARE(profiles->count(),1);
        QCOMPARE(profiles->currentData().toJsonObject()["mode"].toString(),QString("hardware"));
        mode->setCurrentIndex(mode->findData("mock"));
        QTRY_COMPARE(profiles->currentData().toJsonObject()["mode"].toString(),QString("mock"));
        auto *check=widget.findChild<QPushButton *>("managementPreflight");
        QTRY_VERIFY(check->isEnabled()); check->click();
        QTRY_COMPARE_WITH_TIMEOUT(preflight.size(),1,15000);
        QVERIFY2(preflight.first()[0].toJsonObject()["passed"].toBool(),"Installed mock preflight failed");
        QTest::qWait(1500);
        QCOMPARE(mode->currentData().toString(),QString("mock")); QVERIFY(mode->isEnabled());
        QCOMPARE(service->snapshot()["software_state"].toString(),QString("stopped"));
        QVERIFY(!service->snapshot()["recovery_required"].toBool());
        QCOMPARE(mutations.size(),0);
        const auto screenshot=qEnvironmentVariable("HOST_TEST_DEPLOYED_SCREENSHOT");
        if(!screenshot.isEmpty()) QVERIFY(widget.grab().save(screenshot));
        service->close();
    }
    void explicitDeploymentConfigPrefillsWithoutConnecting_data() {
        QTest::addColumn<bool>("relativeToken");
        QTest::newRow("absolute-token") << false;
        QTest::newRow("relative-token") << true;
    }
    void explicitDeploymentConfigPrefillsWithoutConnecting() {
        QFETCH(bool, relativeToken);
        QTemporaryDir folder; QVERIFY(folder.isValid());
        const auto oldConfig=qgetenv("HOSTCOMPUTER_MANAGEMENT_CONFIG");
        const bool existed=qEnvironmentVariableIsSet("HOSTCOMPUTER_MANAGEMENT_CONFIG");
        const auto restore=qScopeGuard([&]{if(existed)qputenv("HOSTCOMPUTER_MANAGEMENT_CONFIG",oldConfig);else qunsetenv("HOSTCOMPUTER_MANAGEMENT_CONFIG");});
        const QString token(40,'t');
        QFile tokenFile(folder.filePath("token")); QVERIFY(tokenFile.open(QIODevice::WriteOnly));
        tokenFile.write(token.toUtf8()); tokenFile.close();
        QFile config(folder.filePath("manager.json")); QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(QJsonDocument(QJsonObject{{"bind","0.0.0.0"},{"port",19091},
            {"token_file",relativeToken ? QString("token") : tokenFile.fileName()},
            {"control_endpoint",QJsonObject{{"host","192.0.2.99"}}}}).toJson()); config.close();
        qputenv("HOSTCOMPUTER_MANAGEMENT_CONFIG",config.fileName().toUtf8());
        RuntimeManagementWidget widget;
        QCOMPARE(widget.findChild<QLineEdit *>("managementHost")->text(),QString("127.0.0.1"));
        QCOMPARE(widget.findChild<QSpinBox *>("managementPort")->value(),19091);
        QCOMPARE(widget.findChild<QLineEdit *>("managementToken")->text(),token);
        QTest::qWait(100);
        const auto services=widget.findChildren<RuntimeService *>();
        QCOMPARE(services.size(),1); QVERIFY(!services.first()->canQuery());
        for(auto *log:widget.findChildren<QPlainTextEdit *>()) QVERIFY(!log->toPlainText().contains(token));
    }
    void vehicleEntryOverridesLocalAddressAndTokenWithoutConnecting() {
        const auto previous=qgetenv("HOSTCOMPUTER_MANAGEMENT_HOST");
        const bool existed=qEnvironmentVariableIsSet("HOSTCOMPUTER_MANAGEMENT_HOST");
        const auto restore=qScopeGuard([&]{if(existed)qputenv("HOSTCOMPUTER_MANAGEMENT_HOST",previous);else qunsetenv("HOSTCOMPUTER_MANAGEMENT_HOST");});
        qputenv("HOSTCOMPUTER_MANAGEMENT_HOST","192.0.2.24");
        RuntimeManagementWidget widget;
        QCOMPARE(widget.findChild<QLineEdit *>("managementHost")->text(),QString("192.0.2.24"));
        QCOMPARE(widget.findChild<QSpinBox *>("managementPort")->value(),9091);
        QVERIFY(widget.findChild<QLineEdit *>("managementToken")->text().isEmpty());
        QVERIFY(!widget.findChild<RuntimeService *>()->canQuery());
    }
    void fieldControlsStayAvailableWithoutDiagnosticClutter() {
        RuntimeManagementWidget widget;
        auto *speed=widget.findChild<QPushButton *>("speedApply"), *reset=widget.findChild<QPushButton *>("globalReset");
        auto *label=widget.findChild<QLabel *>("actualSpeed");
        QJsonObject diagnostics{{"ready",true},{"negotiated",true}};
        widget.setControlDiagnostics(diagnostics); QVERIFY(!speed->isEnabled()); QVERIFY(!reset->isEnabled());
        diagnostics["speed_level_supported"]=true; diagnostics["global_stop_supported"]=true;
        diagnostics["operations_authorized"]=true;
        QJsonObject actual{{"speed_level",3},{"runtime_fresh",true},{"runtime_global_inhibit",true},{"emergency_active",false},
            {"global_stop",QJsonObject{{"state","succeeded"}}},{"safety_halt",QJsonObject{{"state","failed"}}}};
        diagnostics["server"]=actual; widget.setControlDiagnostics(diagnostics);
        QVERIFY(speed->isEnabled()); QVERIFY(reset->isEnabled()); QVERIFY(label->text().contains("3"));
        actual["global_stop_reset_ready"]=false; diagnostics["server"]=actual; widget.setControlDiagnostics(diagnostics);
        QVERIFY(!reset->isEnabled()); QVERIFY(widget.findChild<QLabel *>("presetState")->text().contains("等待清理"));
        actual["global_stop_reset_ready"]=true; diagnostics["server"]=actual; widget.setControlDiagnostics(diagnostics); QVERIFY(reset->isEnabled());
        QSignalSpy commands(&widget,&RuntimeManagementWidget::fieldCommandRequested); speed->click(); QCOMPARE(commands.size(),1);
        QCOMPARE(commands[0][0].toString(),QString("set_speed_level"));
        actual["runtime_fresh"]=false; diagnostics["server"]=actual; widget.setControlDiagnostics(diagnostics); QVERIFY(!reset->isEnabled());
        actual["runtime_fresh"]=true; actual["runtime_global_inhibit"]=false; diagnostics["server"]=actual; widget.setControlDiagnostics(diagnostics); QVERIFY(!reset->isEnabled());
        diagnostics["speed_change_pending"]=true; widget.setControlDiagnostics(diagnostics); QVERIFY(!speed->isEnabled());
        widget.setControlDiagnostics({}); QCOMPARE(commands.size(),1); QVERIFY(!reset->isEnabled());
    }
    void selectedFlipperFaultShowsBackendReasonAndExpires() {
        RuntimeManagementWidget widget;
        const QString reason = "left_front_arm_joint: stable feedback timeout";
        QJsonObject flippers{{"label", "摆臂"}, {"selected", true},
            {"states", QJsonObject{{"flippers", "Fault"}}},
            {"diagnostics", QJsonObject{{"flippers", QJsonObject{{"state", 3}, {"reason", reason}}}}}};
        QJsonObject status{{"runtime_fresh", true}, {"presets", QJsonObject{{"flippers", flippers}}}};
        QJsonObject diagnostics{{"ready", true}, {"negotiated", true},
            {"selected_presets", QJsonArray{"flippers"}}, {"server", status}};
        widget.setControlDiagnostics(diagnostics);
        auto *state = widget.findChild<QLabel *>("groupState_flippers");
        auto *summary = widget.findChild<QLabel *>("presetState");
        QVERIFY(state->text().contains(reason));
        QVERIFY(state->toolTip().contains(reason));
        QVERIFY(summary->text().contains("下位机运动组故障"));
        QVERIFY(summary->text().contains(reason));
        status["runtime_fresh"] = false; diagnostics["server"] = status;
        widget.setControlDiagnostics(diagnostics);
        QVERIFY(!state->text().contains(reason));
        QVERIFY(state->toolTip().isEmpty());
        QVERIFY(!summary->text().contains(reason));
        widget.setControlDiagnostics({});
        QCOMPARE(state->text(), QString("未连接"));
    }
    void presetDraftAndCapabilityGate() {
        V2ManagerStub manager; manager.software = "ready"; QVERIFY(manager.listen()); RuntimeManagementWidget widget;
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        QTRY_VERIFY(widget.findChild<RuntimeService *>()->canStop());
        QSignalSpy changed(&widget,&RuntimeManagementWidget::pendingPresetsChanged);
        auto *base=widget.findChild<QCheckBox *>("preset_base"); QVERIFY(base);
        base->setChecked(false); QCOMPARE(changed.size(),1); QCOMPARE(widget.pendingPresets(),QJsonArray({"flippers","arm"}));
        auto *start=widget.findChild<QPushButton *>("managementStart"); QVERIFY(!start->isEnabled());
        widget.setControlDiagnostics({{"ready",true},{"motion_presets_supported",false}}); QVERIFY(!start->isEnabled());
        widget.setControlDiagnostics({{"ready",true},{"motion_presets_supported",true}}); QVERIFY(start->isEnabled());
        for(auto *check:widget.findChildren<QCheckBox *>()) if(check->objectName().startsWith("preset_")) check->setChecked(false);
        QVERIFY(widget.pendingPresets().isEmpty()); QVERIFY(!start->isEnabled());
        QSignalSpy requested(&widget,&RuntimeManagementWidget::startPresetsRequested); start->click(); QCOMPARE(requested.size(),0);
        widget.setControlDiagnostics({{"ready",true},{"negotiated",true},{"motion_presets_supported",true},{"selected_presets",QJsonArray{"base","arm"}}});
        auto *stopAll=widget.findChild<QPushButton *>("presetHaltAll"); QVERIFY(stopAll->isEnabled());
        QSignalSpy stopped(&widget,&RuntimeManagementWidget::haltPresetsRequested); stopAll->click(); QCOMPARE(stopped.size(),1);
        QCOMPARE(stopped[0][0].value<QJsonArray>(),QJsonArray({"base","arm"}));
        base->setChecked(true); QSignalSpy recovered(&widget,&RuntimeManagementWidget::recoverPresetsRequested);
        widget.findChild<QPushButton *>("presetRecover")->click(); QCOMPARE(recovered.size(),1); QCOMPARE(recovered[0][0].value<QJsonArray>(),QJsonArray{"base"});
    }
    void capabilityStatusDistinguishesDisconnectedWaitingAndUnsupported() {
        RuntimeManagementWidget widget; auto *label=widget.findChild<QLabel *>("presetState"); QVERIFY(label);
        widget.setControlDiagnostics({}); QVERIFY(label->text().contains("连接")); QVERIFY(!label->text().contains("升级"));
        widget.setControlDiagnostics({{"transport_connected",true},{"capability_discovery","waiting"}});
        QVERIFY(label->text().contains("正在连接")); QVERIFY(!label->text().contains("升级"));
        widget.setControlDiagnostics({{"transport_connected",true},{"capability_discovery","declared"}});
        QVERIFY(label->text().contains("正在连接"));
        widget.setControlDiagnostics({{"negotiated",true},{"motion_presets_supported",false}});
        QVERIFY(label->text().contains("不支持运动组")); QVERIFY(!label->text().contains("motion_presets"));
        widget.setControlDiagnostics({{"capability_discovery","timeout"}}); QVERIFY(label->text().contains("超时"));
        widget.setControlDiagnostics({{"capability_discovery","invalid"}}); QVERIFY(label->text().contains("不兼容"));
    }
    void automaticStartRequiresManagedEndpointAndNewConnection() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,directory.path());
        V2ManagerStub manager; QVERIFY(manager.listen()); RuntimeManagementWidget widget;
        widget.findChild<QComboBox *>("managementMode")->setCurrentIndex(0);
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *start=widget.findChild<QPushButton *>("managementStart"); QTRY_VERIFY(start->isEnabled());
        QJsonObject ready{{"ready",true},{"motion_presets_supported",true},{"robot_id","robot24"},{"endpoint","127.0.0.1:9090"},{"connection_id","old-connection"}};
        widget.setControlDiagnostics(ready);
        QSignalSpy requested(&widget,&RuntimeManagementWidget::startPresetsRequested), endpoint(&widget,&RuntimeManagementWidget::controlEndpointReady);
        start->click(); widget.setControlDiagnostics(ready); QCOMPARE(requested.size(),0);
        QTRY_COMPARE(endpoint.size(),1);
        widget.setControlDiagnostics(ready); QCOMPARE(requested.size(),0);
        auto wrong=ready; wrong["connection_id"]="new-connection"; wrong["robot_id"]="other-robot";
        widget.setControlDiagnostics(wrong); QCOMPARE(requested.size(),0);
        wrong["robot_id"]="robot24"; wrong["endpoint"]="127.0.0.1:9999";
        widget.setControlDiagnostics(wrong); QCOMPARE(requested.size(),0);
        ready["connection_id"]="new-connection"; widget.setControlDiagnostics(ready); QCOMPARE(requested.size(),1);
        widget.setControlDiagnostics(ready); QCOMPARE(requested.size(),1);
        QVERIFY(start->isEnabled()); start->click(); QCOMPARE(requested.size(),2); QCOMPARE(manager.starts,1);
    }
    void managementPreflightAndStartCarryOnlyTheSelectedPreset_data() {
        QTest::addColumn<QString>("preset");
        for (const auto &id : QStringList{"base", "flippers", "arm"}) QTest::newRow(qPrintable(id)) << id;
    }
    void managementPreflightAndStartCarryOnlyTheSelectedPreset() {
        QFETCH(QString, preset);
        V2ManagerStub manager;QVERIFY(manager.listen());RuntimeManagementWidget widget;
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        for (const auto &id : QStringList{"base", "flippers", "arm"})
            widget.findChild<QCheckBox *>("preset_" + id)->setChecked(id == preset);
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *service=widget.findChild<RuntimeService *>();QTRY_VERIFY(service->canStart());
        widget.findChild<QPushButton *>("managementPreflight")->click();
        QTRY_VERIFY(service->canStart());
        widget.findChild<QPushButton *>("managementStart")->click();QTRY_COMPARE(manager.starts,1);
        QSet<QString> seen;
        for(const auto &request:manager.requests) {
            const auto method=request["method"].toString();
            if(method=="start" || method=="preflight") {
                QCOMPARE(request["params"].toObject()["presets"].toArray(),QJsonArray{preset});seen.insert(method);
            }
        }
        QCOMPARE(seen,QSet<QString>({"start","preflight"}));
    }
    void configurationBlocksStartAndRefreshesProfile() {
        QTemporaryDir settingsDirectory; QVERIFY(settingsDirectory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDirectory.path());
        ConfigurationStub manager; QVERIFY(manager.listen()); RuntimeManagementWidget widget;
        widget.findChild<QComboBox *>("managementMode")->setCurrentIndex(0);
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *configuration = widget.findChild<ConfigurationWidget *>()->service();
        auto *start = widget.findChild<QPushButton *>("managementStart");
        QTRY_VERIFY(configuration->canEdit() && start->isEnabled());
        QVERIFY(configuration->saveCandidate({QJsonObject{{"profile_id", "mock"}, {"field", "startup_timeout"}, {"value", 100}}}));
        QVERIFY(!start->isEnabled());
        QTRY_VERIFY(configuration->canApply()); QVERIFY(configuration->apply()); QVERIFY(!start->isEnabled());
        QTRY_VERIFY(configuration->canEdit()); QTRY_VERIFY(start->isEnabled());
        start->click(); QTRY_COMPARE(manager.starts, 1);
        bool currentRevision = false;
        for (const auto &request : manager.requests)
            if (request["method"] == QJsonValue("start")) currentRevision = request["params"].toObject()["config_revision"] == QJsonValue(manager.target);
        QVERIFY(currentRevision);
    }
    void candidateAndExplicitApply() {
        QTemporaryDir settingsDirectory; QVERIFY(settingsDirectory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDirectory.path());
        ConfigurationStub manager; QVERIFY(manager.listen());
        ConfigurationWidget widget;
        widget.open("127.0.0.1", manager.port(), manager.token());
        QTRY_VERIFY(widget.service()->canEdit());
        auto *timeout = widget.findChild<QDoubleSpinBox *>("configurationTimeout");
        QCOMPARE(timeout->minimum(), 1.0); QCOMPARE(timeout->maximum(), 180.0);
        QVERIFY(!widget.findChild<QCheckBox *>("configurationVideoRequired")->isEnabled());
        timeout->setValue(100);
        auto accept = [] {
            QTimer::singleShot(0, [] {
                auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (box) box->button(QMessageBox::Yes)->click();
            });
        };
        accept(); widget.findChild<QPushButton *>("configurationSave")->click();
        QTRY_VERIFY(!widget.service()->candidate().isEmpty());
        QCOMPARE(manager.applies, 0);
        QVERIFY(widget.findChild<QPlainTextEdit *>("configurationDiff")->toPlainText().contains("startup_timeout"));
        auto *apply = widget.findChild<QPushButton *>("configurationApply"); QVERIFY(!apply->isEnabled());
        widget.service()->setMaintenanceReady(true); QVERIFY(apply->isEnabled());
        accept(); apply->click(); QTRY_VERIFY(widget.service()->canEdit());
        QVERIFY(widget.findChild<QLabel *>("configurationStatus")->text().contains(QStringLiteral("待显式启动")));
        QCOMPARE(manager.starts, 0); QCOMPARE(manager.applies, 1);
        widget.close(); QVERIFY(!apply->isEnabled());
        QVERIFY(widget.findChild<QPlainTextEdit *>("configurationSource")->toPlainText().isEmpty());
    }
    void startStopAndStaleState() {
        QTemporaryDir settingsDirectory; QVERIFY(settingsDirectory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDirectory.path());
        ManagerStub manager; QVERIFY(manager.listen());
        RuntimeManagementWidget widget;
        widget.findChild<QComboBox *>("managementMode")->setCurrentIndex(0);
        QSignalSpy ready(&widget, &RuntimeManagementWidget::controlEndpointReady);
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        auto *token = widget.findChild<QLineEdit *>("managementToken");
        QCOMPARE(token->echoMode(), QLineEdit::Password); token->setText(manager.token());
        auto *start = widget.findChild<QPushButton *>("managementStart");
        auto *stop = widget.findChild<QPushButton *>("managementStop");
        QVERIFY(!start->isEnabled()); QVERIFY(!stop->isEnabled());
        widget.findChild<QPushButton *>("managementConnect")->click();
        QTRY_VERIFY(start->isEnabled()); QCOMPARE(manager.starts, 0); QCOMPARE(ready.count(), 0);
        start->click(); QVERIFY(!start->isEnabled());
        QTRY_COMPARE(ready.count(), 1); QTRY_VERIFY(stop->isEnabled());
        QVERIFY(widget.findChild<QLabel *>("presetState")->text().contains(QStringLiteral("待授权")));
        QVERIFY(!start->isEnabled());
        stop->click(); QTRY_VERIFY(start->isEnabled()); QCOMPARE(manager.starts, 1); QCOMPARE(manager.stops, 1);
        widget.findChild<QPushButton *>("managementDisconnect")->click();
        QVERIFY(!start->isEnabled()); QVERIFY(!stop->isEnabled());
        QVERIFY(widget.findChild<QLabel *>("presetState")->text().contains(QStringLiteral("连接")));
        QSettings settings("HostComputer", "Management");
        settings.sync();
        QFile file(settings.fileName()); QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains(manager.token().toUtf8()));
    }
};
QTEST_MAIN(ManagementWidgetTest)
#include "management_widget_test.moc"
