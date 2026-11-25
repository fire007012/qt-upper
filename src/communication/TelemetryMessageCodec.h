#ifndef TELEMETRY_MESSAGE_CODEC_H
#define TELEMETRY_MESSAGE_CODEC_H

#include <QJsonObject>
#include <QString>
#include "SharedStructs.h"

namespace Communication {

/**
 * @brief TCP/JSON 消息结构校验与遥测载荷解析器（无 socket/状态依赖）
 */
class TelemetryMessageCodec
{
public:
    static bool validateIncomingMessage(const QJsonObject &message,
                                        QString *errorMessage,
                                        int *errorCode);
    static bool validateCameraObject(const QJsonObject &camera,
                                     QString *errorMessage,
                                     int *errorCode);
    static MotorState parseMotorState(const QJsonObject &json);
    static JointRuntimeStateList parseJointRuntimeStates(const QJsonObject &json);
};

} // namespace Communication

#endif // TELEMETRY_MESSAGE_CODEC_H
