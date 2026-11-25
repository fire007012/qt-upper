#include "ProtocolLogFormatter.h"

#include <QJsonArray>
#include <QVariant>

FormattedProtocolLog formatProtocolLogMessage(const QJsonObject &message)
{
    const QString type = message[QStringLiteral("type")].toString();

    if (type == QLatin1String("emergency_result")) {
        const bool ok = message[QStringLiteral("ok")].toBool();
        return {
            ok ? FormattedProtocolLog::Kind::Command : FormattedProtocolLog::Kind::Error,
            ok ? QStringLiteral("[急停] 桥接已锁存并发起停止；物理停止仍需独立确认。")
               : QStringLiteral("[急停] 请求结果未知：%1；请使用独立停止保障。")
                     .arg(message[QStringLiteral("error")].toString())
        };
    }

    if (type == QLatin1String("ack_pending")) {
        return {
            FormattedProtocolLog::Kind::Command,
            QStringLiteral("[ACK] waiting %1 seq=%2 timeout=%3ms")
                .arg(message[QStringLiteral("ack_type")].toString(QStringLiteral("unknown")))
                .arg(message[QStringLiteral("seq")].toVariant().toLongLong())
                .arg(message[QStringLiteral("timeout_ms")].toInt())
        };
    }

    if (type == QLatin1String("connection_recovery")) {
        return {
            FormattedProtocolLog::Kind::Command,
            QStringLiteral("[连接] 将在 %1ms 后自动重试").arg(message[QStringLiteral("retry_in_ms")].toInt())
        };
    }

    if (type == QLatin1String("ack")) {
        const bool ok = message[QStringLiteral("ok")].toBool(false);
        const QString line = QStringLiteral("[ACK] %1 seq=%2 %3 code=%4 elapsed=%5ms %6")
                                 .arg(message[QStringLiteral("ack_type")].toString(QStringLiteral("unknown")))
                                 .arg(message[QStringLiteral("seq")].toVariant().toLongLong())
                                 .arg(ok ? QStringLiteral("OK") : QStringLiteral("FAIL"))
                                 .arg(message[QStringLiteral("code")].toInt(-1))
                                 .arg(message[QStringLiteral("elapsed_ms")].toVariant().toLongLong())
                                 .arg(message[QStringLiteral("message")].toString());
        return {ok ? FormattedProtocolLog::Kind::Command : FormattedProtocolLog::Kind::Error, line};
    }

    if (type == QLatin1String("ack_timeout")
        || type == QLatin1String("ack_disconnected")
        || type == QLatin1String("ack_superseded")) {
        return {
            FormattedProtocolLog::Kind::Error,
            QStringLiteral("[ACK] %1 %2 seq=%3 elapsed=%4ms")
                .arg(type)
                .arg(message[QStringLiteral("ack_type")].toString(QStringLiteral("unknown")))
                .arg(message[QStringLiteral("seq")].toVariant().toLongLong())
                .arg(message[QStringLiteral("elapsed_ms")].toVariant().toLongLong())
        };
    }

    if (type == QLatin1String("ack_unmatched")) {
        return {
            FormattedProtocolLog::Kind::Error,
            QStringLiteral("[ACK] UNMATCHED %1 seq=%2 code=%3 %4")
                .arg(message[QStringLiteral("ack_type")].toString(QStringLiteral("unknown")))
                .arg(message[QStringLiteral("seq")].toVariant().toLongLong())
                .arg(message[QStringLiteral("code")].toInt(-1))
                .arg(message[QStringLiteral("message")].toString())
        };
    }

    if (type == QLatin1String("ack_mismatch")) {
        return {
            FormattedProtocolLog::Kind::Error,
            QStringLiteral("[ACK] MISMATCH actual=%1 expected=%2 seq=%3 elapsed=%4ms code=%5 %6")
                .arg(message[QStringLiteral("ack_type")].toString(QStringLiteral("unknown")))
                .arg(message[QStringLiteral("expected_ack_type")].toString(QStringLiteral("unknown")))
                .arg(message[QStringLiteral("seq")].toVariant().toLongLong())
                .arg(message[QStringLiteral("elapsed_ms")].toVariant().toLongLong())
                .arg(message[QStringLiteral("code")].toInt(-1))
                .arg(message[QStringLiteral("message")].toString())
        };
    }

    if (type == QLatin1String("service_call_result")) {
        const bool ok = message[QStringLiteral("ok")].toBool(false);
        const QString detail = QStringLiteral("[ROS service] %1 %2 seq=%3 code=%4 %5ms %6 service=%7 request=%8")
                                   .arg(message[QStringLiteral("command")].toString(QStringLiteral("unknown")))
                                   .arg(ok ? QStringLiteral("OK") : QStringLiteral("FAIL"))
                                   .arg(message[QStringLiteral("seq")].toVariant().toLongLong())
                                   .arg(message[QStringLiteral("code")].toInt(-1))
                                   .arg(message[QStringLiteral("duration_ms")].toVariant().toLongLong())
                                   .arg(message[QStringLiteral("message")].toString())
                                   .arg(message[QStringLiteral("service")].toString(QStringLiteral("unknown")))
                                   .arg(message[QStringLiteral("request_type")].toString(QStringLiteral("unknown")));
        return {ok ? FormattedProtocolLog::Kind::Command : FormattedProtocolLog::Kind::Error, detail};
    }

    if (type == QLatin1String("emergency_state")) {
        return {
            FormattedProtocolLog::Kind::Command,
            QStringLiteral("[急停状态] %1 source=%2 %3")
                .arg(message[QStringLiteral("active")].toBool(false) ? QStringLiteral("ACTIVE") : QStringLiteral("CLEAR"))
                .arg(message[QStringLiteral("source")].toString())
                .arg(message[QStringLiteral("message")].toString())
        };
    }

    if (type == QLatin1String("hello")) {
        return {
            FormattedProtocolLog::Kind::Command,
            QStringLiteral("[Bridge] hello name=%1 version=%2 robot=%3")
                .arg(message[QStringLiteral("bridge_name")].toString(QStringLiteral("unknown")))
                .arg(message[QStringLiteral("bridge_version")].toString(QStringLiteral("unknown")))
                .arg(message[QStringLiteral("robot_id")].toString(QStringLiteral("unknown")))
        };
    }

    if (type == QLatin1String("capabilities")) {
        return {
            FormattedProtocolLog::Kind::Command,
            QStringLiteral("[Bridge] capabilities supports=%1 max_frame=%2")
                .arg(message[QStringLiteral("supports")].toArray().size())
                .arg(message[QStringLiteral("max_frame_bytes")].toVariant().toLongLong())
        };
    }

    if (type == QLatin1String("sync_request_sent")) {
        return {
            FormattedProtocolLog::Kind::Command,
            QStringLiteral("[同步] request reason=%1 sync=%2 cameras=%3")
                .arg(message[QStringLiteral("reason")].toString(QStringLiteral("unknown")))
                .arg(message[QStringLiteral("sync_sent")].toBool(false) ? QStringLiteral("sent") : QStringLiteral("failed"))
                .arg(message[QStringLiteral("camera_list_sent")].toBool(false) ? QStringLiteral("sent") : QStringLiteral("failed"))
        };
    }

    if (type == QLatin1String("camera_list_response")) {
        return {
            FormattedProtocolLog::Kind::Command,
            QStringLiteral("[同步] camera_list_response seq=%1 cameras=%2")
                .arg(message[QStringLiteral("seq")].toVariant().toLongLong())
                .arg(message[QStringLiteral("cameras")].toArray().size())
        };
    }

    if (type == QLatin1String("param_response")) {
        return {
            FormattedProtocolLog::Kind::Command,
            QStringLiteral("[参数] %1=%2")
                .arg(message[QStringLiteral("name")].toString(QStringLiteral("unknown")))
                .arg(message[QStringLiteral("value")].toVariant().toString())
        };
    }

    if (type == QLatin1String("protocol_error")) {
        return {
            FormattedProtocolLog::Kind::Error,
            QStringLiteral("[协议错误] seq=%1 code=%2 %3")
                .arg(message[QStringLiteral("seq")].toVariant().toLongLong())
                .arg(message[QStringLiteral("code")].toInt(-1))
                .arg(message[QStringLiteral("message")].toString())
        };
    }

    return {};
}
