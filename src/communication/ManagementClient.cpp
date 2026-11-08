#include "ManagementClient.h"
#include <QJsonDocument>
#include <QUuid>

namespace Communication {
ManagementClient::ManagementClient(QObject *parent) : QObject(parent)
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { fail(QStringLiteral("管理请求/连接超时")); });
}
void ManagementClient::close()
{
    m_deadline.stop();
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_buffer.clear();
    m_id.clear();
    m_method.clear();
    m_token.clear();
}
void ManagementClient::open(const QString &host, quint16 port, const QString &token)
{
    close();
    m_host = host.trimmed();
    if (m_host.isEmpty() || !port || token.size() < 32) {
        emit lost(QStringLiteral("请填写管理地址、端口和至少32字符的部署令牌"), false);
        return;
    }
    m_token = token;
    m_socket = new QTcpSocket(this);
    auto *socket = m_socket;
    socket->setReadBufferSize(65537);
    connect(socket, &QTcpSocket::connected, this, [this, socket] {
        if (socket != m_socket) return;
        m_deadline.stop();
        emit connected();
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { if (socket == m_socket) read(); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (socket == m_socket) fail(QStringLiteral("管理连接已断开"));
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
        if (socket == m_socket) fail(QStringLiteral("管理网络错误（检查地址和服务）"));
    });
    m_deadline.start(15000);
    socket->connectToHost(m_host, port);
}
bool ManagementClient::request(const QString &method, const QJsonObject &params, const QString &requestId)
{
    if (!idle()) return false;
    if (method.startsWith("config.") && QJsonDocument(params).toJson(QJsonDocument::Compact).size() > 49152) return false;
    const QString id = requestId.isEmpty()
        ? (method.startsWith("config.") ? QStringLiteral("config-") : QString()) + QUuid::createUuid().toString(QUuid::WithoutBraces)
        : requestId;
    const QJsonObject object{{"version", 1}, {"request_id", id}, {"token", m_token},
                             {"method", method}, {"params", params}};
    const QByteArray frame = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (frame.size() > 65536) return false;
    m_id = id;
    m_method = method;
    // Persist intent before any bytes can reach the server. Never log request bodies/tokens.
    emit requestIssued(id, method);
    m_deadline.start(15000);
    if (m_socket->write(frame) != frame.size()) {
        fail(QStringLiteral("管理请求发送失败"));
        return false;
    }
    return true;
}
void ManagementClient::fail(const QString &message)
{
    const bool unknown = m_method == "start" || m_method == "stop" || m_method == "config.candidate"
        || m_method == "config.apply" || m_method == "config.rollback";
    close();
    emit lost(message, unknown);
}
void ManagementClient::read()
{
    auto *socket = m_socket;
    m_buffer += socket->readAll();
    while (m_buffer.contains('\n')) {
        const auto end = m_buffer.indexOf('\n');
        if (end + 1 > 65536) { fail(QStringLiteral("管理响应超过64KiB")); return; }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(m_buffer.left(end), &error);
        m_buffer.remove(0, end + 1);
        const auto obj = doc.object();
        if (error.error != QJsonParseError::NoError || !doc.isObject()
            || m_id.isEmpty() || obj["version"] != QJsonValue(1)
            || obj["request_id"] != QJsonValue(m_id) || !obj["ok"].isBool()
            || (obj["ok"].toBool() ? !obj.contains("result") : !obj["error"].isObject())) {
            fail(QStringLiteral("管理响应协议/请求ID不匹配")); return;
        }
        const QString method = m_method;
        m_deadline.stop();
        m_id.clear();
        m_method.clear();
        if (obj["ok"].toBool()) emit response(method, obj["result"]);
        else emit rejected(method, obj["error"].toObject());
        if (socket != m_socket) return;
    }
    if (m_buffer.size() >= 65536) fail(QStringLiteral("管理响应缺少换行或超过64KiB"));
}
}
