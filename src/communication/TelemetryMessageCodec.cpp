#include "TelemetryMessageCodec.h"
#include "HostProtocol.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>
#include <QVariant>
#include <cmath>

namespace Communication {

namespace {

constexpr int kMaxCameraCount = 8;

bool isIntegerJsonValue(const QJsonValue &value)
{
    if (!value.isDouble()) {
        return false;
    }

    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number;
}

qint64 jsonInteger(const QJsonValue &value)
{
    return value.toVariant().toLongLong();
}

} // namespace

bool TelemetryMessageCodec::validateIncomingMessage(const QJsonObject &message,
                                                    QString *errorMessage,
                                                    int *errorCode)
{
    const QJsonValue typeValue = message.value(QStringLiteral("type"));
    if (!typeValue.isString() || typeValue.toString().isEmpty()) {
        *errorCode = 2101;
        *errorMessage = QStringLiteral("missing or invalid type");
        return false;
    }

    if (message.contains(QStringLiteral("protocol_version"))) {
        const QJsonValue versionValue = message.value(QStringLiteral("protocol_version"));
        if (!isIntegerJsonValue(versionValue)) {
            *errorCode = 2102;
            *errorMessage = QStringLiteral("invalid protocol_version");
            return false;
        }

        const qint64 version = jsonInteger(versionValue);
        if (version != HostProtocol::ProtocolVersion) {
            *errorCode = 2103;
            *errorMessage = QStringLiteral("unsupported protocol_version");
            return false;
        }
    }

    if (message.contains(QStringLiteral("seq")) && !isIntegerJsonValue(message.value(QStringLiteral("seq")))) {
        *errorCode = 2104;
        *errorMessage = QStringLiteral("invalid seq");
        return false;
    }

    const QString type = typeValue.toString();
    if (type == QStringLiteral("heartbeat_ack")) {
        if (!isIntegerJsonValue(message.value(QStringLiteral("seq"))) || jsonInteger(message.value(QStringLiteral("seq"))) <= 0) {
            *errorCode = 2110;
            *errorMessage = QStringLiteral("heartbeat_ack missing valid seq");
            return false;
        }
    } else if (type == QStringLiteral("ack")) {
        if (!message.value(QStringLiteral("ack_type")).isString()) {
            *errorCode = 2120;
            *errorMessage = QStringLiteral("ack missing ack_type");
            return false;
        }
        if (!isIntegerJsonValue(message.value(QStringLiteral("seq"))) || jsonInteger(message.value(QStringLiteral("seq"))) <= 0) {
            *errorCode = 2121;
            *errorMessage = QStringLiteral("ack missing valid seq");
            return false;
        }
        if (!message.value(QStringLiteral("ok")).isBool()) {
            *errorCode = 2122;
            *errorMessage = QStringLiteral("ack missing ok");
            return false;
        }
        if (!isIntegerJsonValue(message.value(QStringLiteral("code")))) {
            *errorCode = 2123;
            *errorMessage = QStringLiteral("ack missing code");
            return false;
        }
    } else if (type == QStringLiteral("camera_info")) {
        if (!validateCameraObject(message, errorMessage, errorCode)) {
            return false;
        }
    } else if (type == QStringLiteral("camera_list_response")) {
        const QJsonValue camerasValue = message.value(QStringLiteral("cameras"));
        if (!camerasValue.isArray()) {
            *errorCode = 2140;
            *errorMessage = QStringLiteral("camera_list_response missing cameras array");
            return false;
        }
        const QJsonArray cameras = camerasValue.toArray();
        for (const QJsonValue &cameraValue : cameras) {
            if (!cameraValue.isObject()) {
                *errorCode = 2141;
                *errorMessage = QStringLiteral("camera_list_response camera item is not object");
                return false;
            }
            if (!validateCameraObject(cameraValue.toObject(), errorMessage, errorCode)) {
                return false;
            }
        }
    } else if (type == QStringLiteral("system_snapshot")) {
        if (message.contains(QStringLiteral("control_mode"))
            && !message.value(QStringLiteral("control_mode")).isString()) {
            *errorCode = 2150;
            *errorMessage = QStringLiteral("system_snapshot invalid control_mode");
            return false;
        }
        if (message.contains(QStringLiteral("emergency"))
            && !message.value(QStringLiteral("emergency")).isObject()) {
            *errorCode = 2151;
            *errorMessage = QStringLiteral("system_snapshot invalid emergency object");
            return false;
        }
        if (message.contains(QStringLiteral("motor"))
            && !message.value(QStringLiteral("motor")).isObject()) {
            *errorCode = 2152;
            *errorMessage = QStringLiteral("system_snapshot invalid motor object");
            return false;
        }
    } else if (type == QStringLiteral("protocol_error")) {
        if (!isIntegerJsonValue(message.value(QStringLiteral("code")))
            || !message.value(QStringLiteral("message")).isString()) {
            *errorCode = 2160;
            *errorMessage = QStringLiteral("protocol_error missing code or message");
            return false;
        }
    } else if (type == QStringLiteral("service_call_result")) {
        if (!message.value(QStringLiteral("request_type")).isString()
            || !message.value(QStringLiteral("command")).isString()
            || !message.value(QStringLiteral("service")).isString()
            || !message.value(QStringLiteral("ok")).isBool()
            || !isIntegerJsonValue(message.value(QStringLiteral("code")))
            || !message.value(QStringLiteral("message")).isString()) {
            *errorCode = 2165;
            *errorMessage = QStringLiteral("service_call_result missing required fields");
            return false;
        }
        if (message.contains(QStringLiteral("duration_ms"))
            && !isIntegerJsonValue(message.value(QStringLiteral("duration_ms")))) {
            *errorCode = 2166;
            *errorMessage = QStringLiteral("service_call_result invalid duration_ms");
            return false;
        }
    } else if (type == QStringLiteral("motor_state")) {
        if (!message.value(QStringLiteral("joints")).isArray()) {
            *errorCode = 2170;
            *errorMessage = QStringLiteral("motor_state missing joints array");
            return false;
        }
    } else if (type == QStringLiteral("joint_runtime_states")) {
        if (!message.value(QStringLiteral("states")).isArray()) {
            *errorCode = 2175;
            *errorMessage = QStringLiteral("joint_runtime_states missing states array");
            return false;
        }
    } else if (type == QStringLiteral("co2_data")) {
        if (!message.value(QStringLiteral("ppm")).isDouble()) {
            *errorCode = 2180;
            *errorMessage = QStringLiteral("co2_data missing ppm");
            return false;
        }
    } else if (type == QStringLiteral("imu_data")) {
        if (!message.value(QStringLiteral("roll")).isDouble()
            || !message.value(QStringLiteral("pitch")).isDouble()
            || !message.value(QStringLiteral("yaw")).isDouble()) {
            *errorCode = 2190;
            *errorMessage = QStringLiteral("imu_data missing attitude fields");
            return false;
        }
    }

    return true;
}

bool TelemetryMessageCodec::validateCameraObject(const QJsonObject &camera,
                                                 QString *errorMessage,
                                                 int *errorCode)
{
    const QJsonValue idValue = camera.value(QStringLiteral("camera_id"));
    if (!isIntegerJsonValue(idValue)) {
        *errorCode = 2130;
        *errorMessage = QStringLiteral("camera missing camera_id");
        return false;
    }

    const qint64 cameraId = jsonInteger(idValue);
    if (cameraId < 0 || cameraId >= kMaxCameraCount) {
        *errorCode = 2131;
        *errorMessage = QStringLiteral("camera_id out of range");
        return false;
    }

    if (!camera.value(QStringLiteral("online")).isBool()) {
        *errorCode = 2132;
        *errorMessage = QStringLiteral("camera missing online");
        return false;
    }

    const bool online = camera.value(QStringLiteral("online")).toBool();
    const QJsonValue urlValue = camera.value(QStringLiteral("rtsp_url"));
    if (!urlValue.isString()) {
        *errorCode = 2133;
        *errorMessage = QStringLiteral("camera missing rtsp_url");
        return false;
    }
    if (online && urlValue.toString().isEmpty()) {
        *errorCode = 2134;
        *errorMessage = QStringLiteral("online camera has empty rtsp_url");
        return false;
    }

    const QStringList optionalIntFields = {
        QStringLiteral("width"),
        QStringLiteral("height"),
        QStringLiteral("fps"),
        QStringLiteral("bitrate_kbps"),
    };
    for (const QString &field : optionalIntFields) {
        if (camera.contains(field) && !isIntegerJsonValue(camera.value(field))) {
            *errorCode = 2135;
            *errorMessage = QStringLiteral("camera field is not integer: %1").arg(field);
            return false;
        }
    }

    if (camera.contains(QStringLiteral("codec")) && !camera.value(QStringLiteral("codec")).isString()) {
        *errorCode = 2136;
        *errorMessage = QStringLiteral("camera codec is not string");
        return false;
    }

    return true;
}

MotorState TelemetryMessageCodec::parseMotorState(const QJsonObject &json)
{
    MotorState state;

    const QJsonArray jointsArray = json[QStringLiteral("joints")].toArray();
    for (int i = 0; i < 6 && i < jointsArray.size(); ++i) {
        const QJsonObject joint = jointsArray[i].toObject();
        state.joints[i].position = static_cast<int16_t>(joint[QStringLiteral("position")].toDouble() * 1000.0f);
        state.joints[i].current = static_cast<int16_t>(joint[QStringLiteral("current")].toDouble() * 1000.0f);
    }

    state.executor_position = static_cast<int16_t>(json[QStringLiteral("executor_position")].toDouble() * 1000.0f);
    state.executor_torque = static_cast<int16_t>(json[QStringLiteral("executor_torque")].toDouble() * 1000.0f);
    state.executor_flags = static_cast<uint8_t>(json[QStringLiteral("executor_flags")].toInt(0));
    state.reserved = static_cast<uint8_t>(json[QStringLiteral("reserved")].toInt(0));

    return state;
}

JointRuntimeStateList TelemetryMessageCodec::parseJointRuntimeStates(const QJsonObject &json)
{
    JointRuntimeStateList states;
    const QJsonArray statesArray = json.value(QStringLiteral("states")).toArray();
    states.reserve(statesArray.size());

    for (const QJsonValue &value : statesArray) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject object = value.toObject();
        JointRuntimeState state;
        state.jointName = object.value(QStringLiteral("joint_name")).toString();
        state.backend = object.value(QStringLiteral("backend")).toString();
        state.lifecycleState = object.value(QStringLiteral("lifecycle_state")).toString();
        state.online = object.value(QStringLiteral("online")).toBool(false);
        state.enabled = object.value(QStringLiteral("enabled")).toBool(false);
        state.fault = object.value(QStringLiteral("fault")).toBool(false);
        states.append(state);
    }

    return states;
}

} // namespace Communication
