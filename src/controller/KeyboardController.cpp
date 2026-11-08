#include "KeyboardController.h"
#include <algorithm>

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
