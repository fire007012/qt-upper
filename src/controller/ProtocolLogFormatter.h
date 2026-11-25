#ifndef PROTOCOL_LOG_FORMATTER_H
#define PROTOCOL_LOG_FORMATTER_H

#include <QJsonObject>
#include <QString>

/**
 * @brief 将协议事件消息格式化为操作日志条目（纯函数，无 UI 依赖）
 */
struct FormattedProtocolLog {
    enum class Kind { None, Command, Error };
    Kind kind = Kind::None;
    QString text;
};

FormattedProtocolLog formatProtocolLogMessage(const QJsonObject &message);

#endif // PROTOCOL_LOG_FORMATTER_H
