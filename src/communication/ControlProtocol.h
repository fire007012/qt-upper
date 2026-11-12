#pragma once
#include "HostProtocol.h"
#include <QJsonArray>

namespace Communication {
class ControlProtocol {
public:
    static constexpr const char *HostBuildVersion = "host-ros2-v2-presets-1";
    static QStringList capabilities();
    static bool input(const OperatorInputState &state, QJsonObject *payload, bool *physicalNeutral);
    static bool operation(const QJsonObject &object, const QString &robot);
    static QJsonValue redact(const QJsonValue &value);
};
}
