#pragma once
#include "HostProtocol.h"
#include <QJsonArray>

namespace Communication {
class ControlProtocol {
public:
    static constexpr const char *HostBuildVersion = "host-ros2-v2-field-1";
    static QStringList capabilities();
    static bool supportsSystemCommand(const QString &command);
    static QString unsupportedCommandReason(const QString &command);
    static bool input(const OperatorInputState &state, QJsonObject *payload, bool *physicalNeutral);
    static bool operation(const QJsonObject &object, const QString &robot);
    static QJsonValue redact(const QJsonValue &value);
};
}
