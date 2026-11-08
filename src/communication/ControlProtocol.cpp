#include "ControlProtocol.h"
#include <QRegularExpression>
#include <QUrl>
#include <cmath>

namespace Communication {
QStringList ControlProtocol::capabilities()
{
    return {"session_v2", "freshness_v2", "explicit_authorization", "bounded_operations", "isolated_output", "runtime_permission"};
}
bool ControlProtocol::input(const OperatorInputState &state, QJsonObject *payload, bool *physicalNeutral)
{
    auto legacy = HostProtocol::makeOperatorInput(state, 1);
    auto gamepad = legacy["gamepad"].toObject();
    auto axes = gamepad["axes"].toObject();
    auto buttons = gamepad["buttons"].toObject();
    *physicalNeutral = state.keyboard.pressedKeys.isEmpty();
    if (state.gamepad.connected) {
        for (const auto &value : axes) {
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 1) return false;
            *physicalNeutral &= std::abs(value.toDouble()) <= .04;
        }
        for (const auto &value : buttons) *physicalNeutral &= !value.toBool();
    } else {
        for (auto it = axes.begin(); it != axes.end(); ++it) it.value() = 0;
        for (auto it = buttons.begin(); it != buttons.end(); ++it) it.value() = false;
    }
    // Continuous v2 input excludes every mode/service/speed edge from v1.
    QJsonArray keys;
    const QStringList allowedKeys{"w","a","s","d","u","o","q","e","k","i","j","l","f","h","y"};
    for (const auto &key : state.keyboard.pressedKeys) if (allowedKeys.contains(key)) keys.append(key);
    for (const auto &key : {"start", "back", "l3", "r3", "dpad_left", "dpad_right"}) buttons.remove(key);
    gamepad["axes"] = axes; gamepad["buttons"] = buttons;
    *payload = {{"keyboard", QJsonObject{{"pressed_keys", keys}}}, {"gamepad", gamepad}};
    return state.ttlMs >= 1 && state.ttlMs <= 300;
}
bool ControlProtocol::operation(const QJsonObject &op, const QString &robot)
{
    const auto state = op["state"].toString();
    return !op["operation_id"].toString().isEmpty() && !op["request_id"].toString().isEmpty()
        && op["robot_id"] == QJsonValue(robot) && !op["epoch"].toString().isEmpty()
        && QStringList{"accepted", "running", "succeeded", "failed", "result_unknown"}.contains(state)
        && op["invalidated"].isBool() && op["created_at"].isDouble()
        && (op["error"].isNull() || op["error"].isString())
        && (op["result"].isNull() || op["result"].isArray());
}
QJsonValue ControlProtocol::redact(const QJsonValue &value)
{
    if (value.isObject()) {
        auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto key = it.key().toLower();
            if ((key.contains("token") && !it.value().isBool() && !it.value().isDouble()) || key.contains("password") || key.contains("secret") || key == "session_id" || key == "authorization")
                it.value() = QStringLiteral("[已隐藏]");
            else it.value() = redact(it.value());
        }
        return object;
    }
    if (value.isArray()) { QJsonArray out; for (const auto &item : value.toArray()) out.append(redact(item)); return out; }
    if (value.isString()) {
        auto text = value.toString();
        static const QRegularExpression credentials("([a-zA-Z][a-zA-Z0-9+.-]*://)[^\\s/@]+@") ;
        text.replace(credentials, "\\1[已隐藏]@");
        static const QRegularExpression query("([?&](?:token|password|secret|key)=)[^&\\s]+", QRegularExpression::CaseInsensitiveOption);
        text.replace(query, "\\1[已隐藏]");
        return text;
    }
    return value;
}
}
