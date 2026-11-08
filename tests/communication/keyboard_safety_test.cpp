#include <QtTest>
#include "KeyboardController.h"
class KeyboardSafetyTest : public QObject {
    Q_OBJECT
private slots:
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
QTEST_GUILESS_MAIN(KeyboardSafetyTest)
#include "keyboard_safety_test.moc"
