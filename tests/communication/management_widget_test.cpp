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
    void respond(QTcpSocket *peer, const QJsonObject &request) override {
        if (request["method"] != QJsonValue("snapshot")) { ManagerStub::respond(peer,request); return; }
        QJsonObject modules{{"core",QJsonObject{{"ok",true}}},{"bridge",QJsonObject{{"ok",true}}}};
        QJsonObject snapshot{{"software_state",software},{"profile_id","mock"},{"config_revision","rev-A"},
            {"operation_id",operationId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(operationId)},
            {"recovery_required",recoveryRequired},{"modules",modules},{"control_endpoint",QJsonObject{{"host","127.0.0.1"},{"port",9090},{"protocol_version",2}}}};
        peer->write(QJsonDocument(QJsonObject{{"version",1},{"request_id",request["request_id"]},{"ok",true},{"result",snapshot}}).toJson(QJsonDocument::Compact)+'\n');
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
    void explicitDeploymentConfigPrefillsWithoutConnecting() {
        QTemporaryDir folder; QVERIFY(folder.isValid());
        const auto oldConfig=qgetenv("HOSTCOMPUTER_MANAGEMENT_CONFIG");
        const bool existed=qEnvironmentVariableIsSet("HOSTCOMPUTER_MANAGEMENT_CONFIG");
        const auto restore=qScopeGuard([&]{if(existed)qputenv("HOSTCOMPUTER_MANAGEMENT_CONFIG",oldConfig);else qunsetenv("HOSTCOMPUTER_MANAGEMENT_CONFIG");});
        const QString token(40,'t');
        QFile tokenFile(folder.filePath("token")); QVERIFY(tokenFile.open(QIODevice::WriteOnly));
        tokenFile.write(token.toUtf8()); tokenFile.close();
        QFile config(folder.filePath("manager.json")); QVERIFY(config.open(QIODevice::WriteOnly));
        config.write(QJsonDocument(QJsonObject{{"bind","0.0.0.0"},{"port",19091},
            {"token_file",tokenFile.fileName()},{"control_endpoint",QJsonObject{{"host","127.0.0.1"}}}}).toJson()); config.close();
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
    void presetDraftAndCapabilityGate() {
        RuntimeManagementWidget widget;
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
    void managementPreflightAndStartCarryOnlyTheSelectedPreset() {
        V2ManagerStub manager;QVERIFY(manager.listen());RuntimeManagementWidget widget;
        widget.findChild<QLineEdit *>("managementHost")->setText("127.0.0.1");
        widget.findChild<QSpinBox *>("managementPort")->setValue(manager.port());
        widget.findChild<QLineEdit *>("managementToken")->setText(manager.token());
        widget.findChild<QComboBox *>("motionPreset")->setCurrentIndex(0);
        widget.findChild<QPushButton *>("managementConnect")->click();
        auto *service=widget.findChild<RuntimeService *>();QTRY_VERIFY(service->canStart());
        widget.findChild<QPushButton *>("managementPreflight")->click();
        QTRY_VERIFY(service->canStart());
        widget.findChild<QPushButton *>("managementStart")->click();QTRY_COMPARE(manager.starts,1);
        QSet<QString> seen;
        for(const auto &request:manager.requests) {
            const auto method=request["method"].toString();
            if(method=="start" || method=="preflight") {
                QCOMPARE(request["params"].toObject()["presets"].toArray(),QJsonArray{"base"});seen.insert(method);
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
