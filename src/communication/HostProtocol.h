#ifndef HOST_PROTOCOL_H
#define HOST_PROTOCOL_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace Communication {

struct KeyboardInputState {
    QStringList pressedKeys;
};

struct GamepadButtonState {
    bool a = false;
    bool b = false;
    bool x = false;
    bool y = false;
    bool start = false;
    bool back = false;
    bool lb = false;
    bool rb = false;
    bool l3 = false;
    bool r3 = false;
    bool dpadUp = false;
    bool dpadDown = false;
    bool dpadLeft = false;
    bool dpadRight = false;
};

struct GamepadAxisState {
    double leftX = 0.0;
    double leftY = 0.0;
    double rightX = 0.0;
    double rightY = 0.0;
    double lt = 0.0;
    double rt = 0.0;
};

struct GamepadInputState {
    bool connected = false;
    GamepadButtonState buttons;
    GamepadAxisState axes;
};

struct OperatorInputState {
    enum class Source { Combined, Keyboard, Gamepad };
    QString mode = QStringLiteral("vehicle");
    int ttlMs = 300;
    KeyboardInputState keyboard;
    GamepadInputState gamepad;
    // Local routing only. Physical neutral checks still inspect both devices.
    Source source = Source::Combined;
};

class HostProtocol
{
public:
    static constexpr int ProtocolVersion = 1;

    static qint64 nowMs();

    static QJsonObject makeOperatorInput(const OperatorInputState &state,
                                         quint64 seq,
                                         qint64 timestampMs = nowMs());
    static QJsonObject makeHeartbeat(quint64 seq,
                                     qint64 timestampMs = nowMs());
    static QJsonObject makeSyncRequest(quint64 seq,
                                       const QString &reason,
                                       qint64 timestampMs = nowMs());
    static QJsonObject makeCameraListRequest(quint64 seq,
                                             qint64 timestampMs = nowMs());
    static QJsonObject makeCommand(const QString &type,
                                   quint64 seq,
                                   const QJsonObject &params = QJsonObject(),
                                   qint64 timestampMs = nowMs());

private:
    static QJsonObject makeEnvelope(const QString &type, quint64 seq, qint64 timestampMs);
};

} // namespace Communication

#endif // HOST_PROTOCOL_H
