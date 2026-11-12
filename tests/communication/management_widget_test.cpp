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
#include "ConfigurationWidget.h"
#include "configuration_stub.h"
#include "RuntimeManagementWidget.h"
#include "manager_stub.h"

class V2ManagerStub : public ManagerStub {
public:
    void respond(QTcpSocket *peer, const QJsonObject &request) override {
        if (request["method"] != QJsonValue("snapshot")) { ManagerStub::respond(peer,request); return; }
        QJsonObject modules{{"core",QJsonObject{{"ok",true}}},{"bridge",QJsonObject{{"ok",true}}}};
        QJsonObject snapshot{{"software_state",software},{"profile_id","mock"},{"config_revision","rev-A"},
            {"operation_id",operationId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(operationId)},
            {"recovery_required",false},{"modules",modules},{"control_endpoint",QJsonObject{{"host","127.0.0.1"},{"port",9090},{"protocol_version",2}}}};
        peer->write(QJsonDocument(QJsonObject{{"version",1},{"request_id",request["request_id"]},{"ok",true},{"result",snapshot}}).toJson(QJsonDocument::Compact)+'\n');
    }
};
class ManagementWidgetTest : public QObject {
    Q_OBJECT
private slots:
    void presetDraftAndCapabilityGate() {
        RuntimeManagementWidget widget;
        QSignalSpy changed(&widget,&RuntimeManagementWidget::pendingPresetsChanged);
        auto *base=widget.findChild<QCheckBox *>("preset_base"); QVERIFY(base);
        base->setChecked(false); QCOMPARE(changed.size(),1); QCOMPARE(widget.pendingPresets(),QJsonArray({"flippers","arm"}));
        auto *start=widget.findChild<QPushButton *>("presetStart"); QVERIFY(!start->isEnabled());
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
        widget.setControlDiagnostics({}); QVERIFY(label->text().contains("未连接")); QVERIFY(!label->text().contains("升级"));
        widget.setControlDiagnostics({{"transport_connected",true},{"capability_discovery","waiting"}});
        QVERIFY(label->text().contains("等待能力声明")); QVERIFY(!label->text().contains("升级"));
        widget.setControlDiagnostics({{"transport_connected",true},{"capability_discovery","declared"}});
        QVERIFY(label->text().contains("正在协商"));
        widget.setControlDiagnostics({{"negotiated",true},{"motion_presets_supported",false}});
        QVERIFY(label->text().contains("服务端未声明")); QVERIFY(label->text().contains("升级"));
        widget.setControlDiagnostics({{"capability_discovery","timeout"}}); QVERIFY(label->text().contains("超时"));
        widget.setControlDiagnostics({{"capability_discovery","invalid"}}); QVERIFY(label->text().contains("声明无效"));
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
        QVERIFY(widget.findChild<QLabel *>("managementSoftwareState")->text().contains(QStringLiteral("待授权")));
        QVERIFY(widget.findChild<QLabel *>("managementSafety")->text().contains(QStringLiteral("未知")));
        QVERIFY(!start->isEnabled());
        stop->click(); QTRY_VERIFY(start->isEnabled()); QCOMPARE(manager.starts, 1); QCOMPARE(manager.stops, 1);
        widget.findChild<QPushButton *>("managementDisconnect")->click();
        QVERIFY(!start->isEnabled()); QVERIFY(!stop->isEnabled());
        QVERIFY(widget.findChild<QLabel *>("managementSoftwareState")->text().contains(QStringLiteral("未知")));
        QSettings settings("HostComputer", "Management");
        settings.sync();
        QFile file(settings.fileName()); QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains(manager.token().toUtf8()));
    }
};
QTEST_MAIN(ManagementWidgetTest)
#include "management_widget_test.moc"
