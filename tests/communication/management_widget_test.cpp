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

class ManagementWidgetTest : public QObject {
    Q_OBJECT
private slots:
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
