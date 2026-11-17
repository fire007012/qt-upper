#include "KeyboardController.h"
#include <algorithm>
#include <QApplication>
#include <QWidget>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QComboBox>

KeyboardController::KeyboardController(QObject *parent)
    : QObject(parent)
    , m_enabled(false)
{
    m_sendTimer = new QTimer(this);
    m_sendTimer->setInterval(SEND_INTERVAL_MS);
    connect(m_sendTimer, &QTimer::timeout, this, &KeyboardController::publishOperatorInput);
}

KeyboardController::~KeyboardController()
{
    m_sendTimer->stop();
}

void KeyboardController::setCaptureWidget(QWidget *window)
{
    if (m_captureWindow) qApp->removeEventFilter(this);
    disconnect(m_focusConnection);
    m_captureWindow = window;
    if (!window) return;
    qApp->installEventFilter(this);
    m_focusConnection = connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *next) {
        if (m_captureWindow) updateCaptureState(next);
    });
}

bool KeyboardController::canCapture(QWidget *widget) const
{
    if (!m_captureWindow || !widget || widget->window() != m_captureWindow->window()
        || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
    for (QWidget *current = widget; current; current = current->parentWidget()) {
        if (qobject_cast<QLineEdit *>(current) || qobject_cast<QAbstractSpinBox *>(current)
            || qobject_cast<QComboBox *>(current)) return false;
        if (auto *edit = qobject_cast<QTextEdit *>(current); edit && !edit->isReadOnly()) return false;
        if (auto *edit = qobject_cast<QPlainTextEdit *>(current); edit && !edit->isReadOnly()) return false;
    }
    return true;
}

void KeyboardController::updateCaptureState(QWidget *widget)
{
    const bool active = canCapture(widget);
    if (active == m_captureActive) return;
    m_captureActive = active;
    if (!active) clearPressedKeys();
    emit captureStateChanged(active);
}

bool KeyboardController::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_captureWindow || !m_enabled) return QObject::eventFilter(watched, event);
    auto *widget = qobject_cast<QWidget *>(watched);
    if (event->type() == QEvent::KeyRelease) {
        auto *key = static_cast<QKeyEvent *>(event);
        // Release held keys even when focus has moved into an editor or dialog.
        if (m_pressedKeys.contains(key->key()) || m_waitingForRelease.contains(key->key()))
            handleKeyRelease(key);
    } else if (event->type() == QEvent::KeyPress && canCapture(widget)) {
        auto *key = static_cast<QKeyEvent *>(event);
        // Space remains the application's independent emergency shortcut.
        if (key->key() == Qt::Key_Space || key->isAutoRepeat()) return false;
        if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
            // Shortcuts must not turn into motion when a modifier is released first.
            if (m_captureActive) {
                m_captureActive = false;
                emit captureStateChanged(false);
            }
            handleKeyPress(key);
            clearPressedKeys();
            return false;
        }
        handleKeyPress(key);
        updateCaptureState(widget);
        const QStringList movement{"w","a","s","d","u","o","q","e","k","i","j","l","f","h","y"};
        if (movement.contains(protocolKeyName(key->key())) && key->modifiers() == Qt::NoModifier)
            return true;
    }
    return QObject::eventFilter(watched, event);
}

void KeyboardController::handleKeyPress(QKeyEvent *event)
{
    if (event->isAutoRepeat()) return;

    int key = event->key();

    // 急停（无论是否启用都响应）
    if (key == Qt::Key_Space) {
        clearPressedKeys();
        emit emergencyStopRequested();
        return;
    }

    if (!m_enabled) return;

    m_pressedKeys.insert(key);
    emit operatorInputChanged(pressedKeyNames());
}

void KeyboardController::handleKeyRelease(QKeyEvent *event)
{
    if (!m_enabled || event->isAutoRepeat()) return;

    m_pressedKeys.remove(event->key());
    m_waitingForRelease.remove(event->key());
    emit operatorInputChanged(pressedKeyNames());
}

void KeyboardController::clearPressedKeys()
{
    m_waitingForRelease.unite(m_pressedKeys);
    m_pressedKeys.clear();
    emit operatorInputChanged(pressedKeyNames());
}

void KeyboardController::publishOperatorInput()
{
    emit operatorInputChanged(pressedKeyNames());
}

void KeyboardController::setEnabled(bool enabled)
{
    if (m_enabled == enabled) return;
    m_enabled = enabled;

    if (enabled) {
        m_sendTimer->start();
    } else {
        m_sendTimer->stop();
        clearPressedKeys();
    }

    emit enabledChanged(enabled);
}

bool KeyboardController::isEnabled() const { return m_enabled; }

QStringList KeyboardController::pressedKeyNames() const
{
    QStringList keys;
    keys.reserve(m_pressedKeys.size());

    for (int key : m_pressedKeys | m_waitingForRelease) {
        const QString name = protocolKeyName(key);
        if (!name.isEmpty()) {
            keys.append(name);
        }
    }

    keys.removeDuplicates();
    keys.sort(Qt::CaseInsensitive);
    return keys;
}

QString KeyboardController::protocolKeyName(int key) const
{
    switch (key) {
    case Qt::Key_W: return QStringLiteral("w");
    case Qt::Key_A: return QStringLiteral("a");
    case Qt::Key_S: return QStringLiteral("s");
    case Qt::Key_D: return QStringLiteral("d");
    case Qt::Key_Up: return QStringLiteral("up");
    case Qt::Key_Down: return QStringLiteral("down");
    case Qt::Key_Left: return QStringLiteral("left");
    case Qt::Key_Right: return QStringLiteral("right");
    case Qt::Key_Space: return QStringLiteral("space");
    case Qt::Key_Shift: return QStringLiteral("shift");
    case Qt::Key_Control: return QStringLiteral("ctrl");
    case Qt::Key_Alt: return QStringLiteral("alt");
    case Qt::Key_Meta: return QStringLiteral("meta");
    case Qt::Key_Tab: return QStringLiteral("tab");
    case Qt::Key_Return:
    case Qt::Key_Enter: return QStringLiteral("enter");
    case Qt::Key_Escape: return QStringLiteral("escape");
    default:
        break;
    }

    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        return QString::number(key - Qt::Key_0);
    }
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        return QString(QChar(static_cast<char>('a' + key - Qt::Key_A)));
    }

    return QString();
}
