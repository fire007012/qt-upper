#include <QtTest>
#include "KeyboardController.h"
#include <QWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QVBoxLayout>
class KeyboardSafetyTest : public QObject {
    Q_OBJECT
private slots:
    void capturesMovementFromChildWidgetsButNeverFromEditors() {
        QWidget window;
        auto *layout = new QVBoxLayout(&window);
        auto *button = new QPushButton("Focus", &window);
        auto *editor = new QLineEdit(&window);
        layout->addWidget(button); layout->addWidget(editor);
        KeyboardController keyboard; keyboard.setCaptureWidget(&window); keyboard.setEnabled(true);
        QSignalSpy capture(&keyboard, &KeyboardController::captureStateChanged);
        window.show(); button->setFocus(); QTest::qWait(20);
        QTest::keyPress(button, Qt::Key_W);
        QCOMPARE(keyboard.currentKeys(), QStringList{"w"});
        editor->setFocus(); QTest::qWait(10);
        QVERIFY(!capture.isEmpty());
        QCOMPARE(capture.last()[0].toBool(), false);
        QVERIFY(keyboard.currentKeys().contains("w")); // Focus change must not invent neutral.
        QTest::keyRelease(editor, Qt::Key_W);
        QVERIFY(keyboard.currentKeys().isEmpty());
        QTest::keyClicks(editor, "wasd");
        QCOMPARE(editor->text(), QString("wasd"));
        QVERIFY(keyboard.currentKeys().isEmpty());
        button->setFocus(); QTest::qWait(10);
        QTest::keyClick(button, Qt::Key_A);
        QVERIFY(keyboard.currentKeys().isEmpty());
    }
    void ignoresAnotherWindowAndModifiedMovementShortcuts() {
        QWidget main, other;
        QLineEdit editor(&other);
        KeyboardController keyboard; keyboard.setCaptureWidget(&main); keyboard.setEnabled(true);
        main.show(); other.show();
        QTest::keyClicks(&editor, "wasd");
        QVERIFY(keyboard.currentKeys().isEmpty());
        main.activateWindow(); main.setFocus(); QTest::qWait(10);
        QSignalSpy capture(&keyboard, &KeyboardController::captureStateChanged);
        QTest::keyPress(&main, Qt::Key_Control);
        QTest::keyPress(&main, Qt::Key_W, Qt::ControlModifier);
        QVERIFY(keyboard.currentKeys().contains("ctrl"));
        QVERIFY(!capture.isEmpty());
        QCOMPARE(capture.last()[0].toBool(), false);
        QTest::keyRelease(&main, Qt::Key_Control);
        QVERIFY(keyboard.currentKeys().contains("w"));
        QTest::keyRelease(&main, Qt::Key_W);
        QVERIFY(keyboard.currentKeys().isEmpty());
        QTest::keyClick(&main, Qt::Key_W, Qt::ControlModifier);
        QVERIFY(!keyboard.currentKeys().contains("w"));
    }
    void reconnectAndEmergencyMustNotInventNeutral() {
        KeyboardController keyboard; keyboard.setEnabled(true);
        QKeyEvent press(QEvent::KeyPress,Qt::Key_W,Qt::NoModifier), release(QEvent::KeyRelease,Qt::Key_W,Qt::NoModifier);
        keyboard.handleKeyPress(&press); QVERIFY(keyboard.currentKeys().contains("w"));
        keyboard.clearPressedKeys(); QVERIFY(keyboard.currentKeys().contains("w"));
        keyboard.clearPressedKeys(); QVERIFY(keyboard.currentKeys().contains("w"));
        keyboard.handleKeyRelease(&release); QVERIFY(keyboard.currentKeys().isEmpty());
        keyboard.handleKeyPress(&press);
        QSignalSpy emergency(&keyboard,&KeyboardController::emergencyStopRequested);
        QKeyEvent space(QEvent::KeyPress,Qt::Key_Space,Qt::NoModifier); keyboard.handleKeyPress(&space);
        QCOMPARE(emergency.size(),1); QVERIFY(keyboard.currentKeys().contains("w"));
        keyboard.handleKeyRelease(&release); QVERIFY(keyboard.currentKeys().isEmpty());
    }
};
QTEST_MAIN(KeyboardSafetyTest)
#include "keyboard_safety_test.moc"
