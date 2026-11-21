#include <QtTest>
#include <QComboBox>
#include <QDockWidget>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include "mainwindow.h"

class MainWindowUiTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
private slots:
    void initTestCase() {
        QVERIFY(directory.isValid());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, directory.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        qputenv("HOSTCOMPUTER_RUNTIME_LOG_DIR", directory.filePath("runtime").toUtf8());
        qunsetenv("HOSTCOMPUTER_MANAGEMENT_CONFIG");
        qunsetenv("HOSTCOMPUTER_MANAGEMENT_HOST");
    }
    void explicitSourceAndKeyboardFeedbackWorkWithoutGamepad() {
        MainWindow window; window.show(); QTest::qWait(60);
        auto *source = window.findChild<QComboBox *>("inputSourceSelector");
        auto *keyboard = window.findChild<KeyboardController *>();
        auto *focus = window.findChild<QPushButton *>("focusKeyboardButton");
        auto *keys = window.findChild<QLabel *>("keyboardPressedKeys");
        QVERIFY(source && keyboard && focus && keys);
        QCOMPARE(source->currentText(), QString("键盘"));
        QVERIFY(window.findChild<QLabel *>("keyboardControlHelp")->isVisible());
        QCOMPARE(window.findChild<QLabel *>("panelInputSourceValue")->text(), QString("键盘"));
        QVERIFY(window.findChild<QPushButton *>("panelGamepadConnect")->isHidden());
        QVERIFY(window.findChild<QLabel *>("gamepadConnectionValue")->isHidden());
        window.findChild<QPushButton *>("managementSettings")->click();
        auto *address = window.findChild<QLineEdit *>("managementHost");
        address->setFocus();
        QTRY_VERIFY(window.findChild<QLabel *>("keyboardCaptureValue")->text().contains("已暂停"));
        QTest::keyClicks(address, "wasd");
        QVERIFY(keyboard->currentKeys().isEmpty());
        window.findChild<QDialog *>("managementSettingsDialog")->hide();
        focus->click();
        QTRY_VERIFY(window.findChild<ControlPanelWidget *>()->hasFocus());
        QVERIFY(window.findChild<QLabel *>("keyboardCaptureValue")->text().contains("已就绪"));
        QTest::keyPress(window.findChild<ControlPanelWidget *>(), Qt::Key_W);
        QCOMPARE(keyboard->currentKeys(), QStringList{"w"});
        QVERIFY(keys->text().contains("w"));
        QTest::keyRelease(window.findChild<ControlPanelWidget *>(), Qt::Key_W);
        auto *child = window.findChild<QPushButton *>("btn_lifecycle_init");
        child->setFocus(); QTest::qWait(10);
        QTest::keyPress(child, Qt::Key_W);
        QCOMPARE(keyboard->currentKeys(), QStringList{"w"});
        QVERIFY(keys->text().contains("w"));
        QTest::keyRelease(child, Qt::Key_W);
        QVERIFY(keyboard->currentKeys().isEmpty());
        source->setCurrentIndex(1);
        QCOMPARE(window.findChild<QLabel *>("panelInputSourceValue")->text(), QString("手柄"));
        QVERIFY(window.findChild<QPushButton *>("panelGamepadConnect")->isVisible());
        QVERIFY(window.findChild<QLabel *>("keyboardControlHelp")->isHidden());
        QVERIFY(focus->isHidden());
        source->setCurrentIndex(0);
        QCOMPARE(window.findChild<QLabel *>("panelInputSourceValue")->text(), QString("键盘"));
        window.findChild<ControlPanelWidget *>()->setGamepadConnected(true);
        QVERIFY(window.findChild<QPushButton *>("panelGamepadConnect")->isHidden());
        QVERIFY(window.findChild<QLabel *>("keyboardControlHelp")->isVisible());
        QVERIFY(!window.findChild<Controller *>()->isTcpConnected());
    }
    void smallScreenKeepsControlsAccessible() {
        MainWindow window; window.show(); QTest::qWait(60);
        const auto available = window.screen()->availableGeometry();
        QVERIFY2(window.width() <= available.width(), qPrintable(QString::number(window.width())));
        QVERIFY(window.height() <= available.height());
        auto *scroll = window.findChild<QScrollArea *>("workspaceScrollArea");
        auto *sidebar = window.findChild<QScrollArea *>("sidebarScrollArea");
        QVERIFY(scroll && sidebar);
        for (const QSize size : {QSize(1280, 680), QSize(1366, 700), QSize(900, 580)}) {
            window.resize(size); QTest::qWait(30);
            QVERIFY(window.width() <= size.width());
            QVERIFY(window.height() <= size.height());
            QVERIFY(window.findChild<QLabel *>("connectionPhaseLabel")->width() >= 40);
            QVERIFY(window.findChild<QLabel *>("controlPermissionLabel")->width() >= 40);
            auto *source = window.findChild<QComboBox *>("inputSourceSelector");
            QVERIFY(window.rect().contains(source->mapTo(&window, source->rect().center())));
            auto *emergency = window.findChild<QPushButton *>("btn_emergency_stop");
            scroll->ensureWidgetVisible(emergency); QTest::qWait(10);
            QVERIFY(scroll->viewport()->rect().contains(emergency->mapTo(scroll->viewport(), emergency->rect().center())));
            auto *help = window.findChild<QLabel *>("keyboardControlHelp");
            sidebar->ensureWidgetVisible(help); QTest::qWait(10);
            QVERIFY(sidebar->viewport()->rect().contains(help->mapTo(sidebar->viewport(), help->rect().center())));
            QVERIFY(sidebar->width() >= 340);
            if (!qEnvironmentVariable("HOST_TEST_WINDOW_SCREENSHOT").isEmpty() && size.width() == 1366) {
                scroll->horizontalScrollBar()->setValue(0);
                scroll->verticalScrollBar()->setValue(0);
                QVERIFY(window.grab().save(qEnvironmentVariable("HOST_TEST_WINDOW_SCREENSHOT") + ".wide.png", "PNG"));
            }
        }
        auto *dock = window.findChild<QDockWidget *>("runtimeDock");
        dock->show(); QTest::qWait(20);
        QVERIFY(window.width() <= 900);
        QVERIFY(window.findChild<QPushButton *>("managementStart")->isVisible());
        if (!qEnvironmentVariable("HOST_TEST_WINDOW_SCREENSHOT").isEmpty())
            QVERIFY(window.grab().save(qEnvironmentVariable("HOST_TEST_WINDOW_SCREENSHOT"), "PNG"));
    }
    void keyboardHelpTracksModeAndSelectedGroups() {
        MainWindow window;
        auto *panel = window.findChild<ControlPanelWidget *>();
        auto *help = window.findChild<QLabel *>("keyboardControlHelp");
        QVERIFY(panel && help);
        panel->setKeyboardContext("vehicle", {"flippers"}, true);
        QVERIFY(help->text().contains("Y/H 左前"));
        QVERIFY(help->text().contains("U/J 右前"));
        QVERIFY(help->text().contains("I/K 左后"));
        QVERIFY(help->text().contains("O/L 右后"));
        QVERIFY(!help->text().contains("W/S 前进后退"));
        panel->setKeyboardContext("vehicle", {"base"}, true);
        QVERIFY(help->text().contains("W/S 前进后退"));
        QVERIFY(!help->text().contains("Y/H 左前"));
        panel->setKeyboardContext("vehicle", {"arm"}, true);
        QVERIFY(help->text().contains("需切换到机械臂模式"));
        const QJsonObject snapshot{{"type", "system_snapshot"}, {"control_mode", "arm"}};
        QVERIFY(QMetaObject::invokeMethod(&window, "onProtocolMessageReceived", Q_ARG(QJsonObject, snapshot)));
        QVERIFY(help->text().contains("U/O X+/−"));
        QVERIFY(help->text().contains("K/I 绕Y+/−"));
        QVERIFY(help->text().contains("F 打开，H 闭合"));
        QVERIFY(!help->text().contains("W/S 前进后退"));
        panel->setKeyboardContext("arm", {"flippers"}, true);
        QVERIFY(help->text().contains("机械臂组未选中"));
        QVERIFY(!help->text().contains("F 打开"));
        panel->setKeyboardContext("vehicle", {}, false);
        QVERIFY(help->text().contains("运动组尚未同步"));
        QVERIFY(help->text().contains("空格：全车急停"));
    }
};
QTEST_MAIN(MainWindowUiTest)
#include "mainwindow_ui_test.moc"
