#pragma once
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QJsonObject>

namespace Communication {
// One independent bounded connection; no lease, authorization, or ordinary output queue.
class EmergencyChannel : public QObject {
    Q_OBJECT
public:
    explicit EmergencyChannel(QObject *parent = nullptr);
    bool send(const QString &host, quint16 port, const QString &source);
    void close();
signals:
    void event(const QJsonObject &message);
private:
    void finish(bool ok, const QString &error = {});
    QTcpSocket *m_socket = nullptr;
    QTimer m_deadline;
    QByteArray m_buffer;
    QString m_id, m_host;
    quint16 m_port = 0;
};
}
