#include "EmergencyChannel.h"
#include <QJsonDocument>
#include <QUuid>

namespace Communication {
EmergencyChannel::EmergencyChannel(QObject *parent) : QObject(parent)
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { finish(false, "EMERGENCY_ACK_TIMEOUT"); });
}
void EmergencyChannel::close()
{
    m_deadline.stop();
    if (m_socket) { m_socket->disconnect(this); m_socket->abort(); m_socket->deleteLater(); m_socket = nullptr; }
    m_buffer.clear();
}
bool EmergencyChannel::send(const QString &host, quint16 port, const QString &source)
{
    if (host.isEmpty() || !port) return false;
    if (m_socket && m_host == host && m_port == port) return true; // Coalesce an in-flight stop.
    close(); m_host = host; m_port = port;
    m_id = QStringLiteral("emergency-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_socket = new QTcpSocket(this); auto *socket = m_socket;
    socket->setReadBufferSize(65537);
    connect(socket, &QTcpSocket::connected, this, [this, socket, source] {
        if (socket != m_socket) return;
        const auto frame = QJsonDocument(QJsonObject{{"type", "emergency_stop"}, {"protocol_version", 2}, {"request_id", m_id},
            {"params", QJsonObject{{"source", source.left(128)}}}}).toJson(QJsonDocument::Compact) + '\n';
        if (socket->write(frame) != frame.size()) finish(false, "EMERGENCY_SEND_FAILED");
    });
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        if (socket != m_socket) return;
        m_buffer += socket->readAll();
        while (m_buffer.contains('\n')) {
            const auto end = m_buffer.indexOf('\n');
            if (end + 1 > 65536) { finish(false, "EMERGENCY_FRAME"); return; }
            const auto document = QJsonDocument::fromJson(m_buffer.left(end)); m_buffer.remove(0, end + 1);
            if (!document.isObject()) { finish(false, "EMERGENCY_FRAME"); return; }
            const auto object = document.object();
            if (object["type"] == QJsonValue("hello") || object["type"] == QJsonValue("capabilities")) continue;
            if (object["type"] != QJsonValue("control_result") || object["protocol_version"] != QJsonValue(2)
                || object["request_id"] != QJsonValue(m_id) || object["ok"] != QJsonValue(true)
                || object["result"].toObject()["emergency_active"] != QJsonValue(true)
                || object["result"].toObject()["physical_stop_confirmed"] != QJsonValue(false)) {
                finish(false, "EMERGENCY_UNCONFIRMED"); return;
            }
            finish(true); return;
        }
        if (m_buffer.size() >= 65536) finish(false, "EMERGENCY_FRAME");
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] { if (socket == m_socket) finish(false, "EMERGENCY_DISCONNECTED"); });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](QAbstractSocket::SocketError) {
        if (socket == m_socket) finish(false, "EMERGENCY_NETWORK");
    });
    m_deadline.start(2000); socket->connectToHost(host, port); return true;
}
void EmergencyChannel::finish(bool ok, const QString &error)
{
    const auto id = m_id; close();
    emit event({{"type", "emergency_result"}, {"request_id", id}, {"ok", ok}, {"error", error}, {"physical_stop_confirmed", false}});
}
}
