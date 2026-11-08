#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonValue>

namespace Communication {
// Management v1 transport. Exactly one in-flight request, no command replay.
class ManagementClient : public QObject {
    Q_OBJECT
public:
    explicit ManagementClient(QObject *parent = nullptr);
    void open(const QString &host, quint16 port, const QString &token);
    void close();
    bool request(const QString &method, const QJsonObject &params = {}, const QString &requestId = {});
    bool idle() const { return m_socket && m_socket->state() == QAbstractSocket::ConnectedState && m_id.isEmpty(); }
    QString host() const { return m_host; }
signals:
    void connected();
    void response(const QString &method, const QJsonValue &result);
    void rejected(const QString &method, const QJsonObject &error);
    void lost(const QString &message, bool outcomeUnknown);
    void requestIssued(const QString &id, const QString &method);
private:
    void fail(const QString &message);
    void read();
    QTcpSocket *m_socket = nullptr;
    QTimer m_deadline;
    QString m_host, m_token, m_id, m_method;
    QByteArray m_buffer;
};
}
