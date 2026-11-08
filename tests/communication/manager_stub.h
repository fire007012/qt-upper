#pragma once
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <memory>
class ManagerStub : public QObject {
public:
    QTcpServer server;
    QList<QJsonObject> requests;
    QList<QTcpSocket *> peers;
    bool dropStart = false, dropStop = false, fragmented = false, wrongId = false, authFailure = false;
    QString software = "stopped", taskState = "succeeded", taskError;
    QString operationId;
    bool requiredHealthy = true, recoveryRequired = false;
    int starts = 0, stops = 0;
    QJsonArray profileList = {QJsonObject{{"profile_id", "mock"}, {"config_revision", "rev-A"}, {"mode", "mock"},
                                        {"required_modules", QJsonArray{"core", "bridge"}}, {"optional_modules", QJsonArray{"video"}}}};
    ManagerStub() {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto *peer = server.nextPendingConnection(); peers.append(peer);
                auto buffer = std::make_shared<QByteArray>();
                connect(peer, &QTcpSocket::readyRead, this, [this, peer, buffer] {
                    *buffer += peer->readAll();
                    while (buffer->contains('\n')) {
                        const auto end = buffer->indexOf('\n');
                        const auto request = QJsonDocument::fromJson(buffer->left(end)).object();
                        buffer->remove(0, end + 1); requests.append(request);
                        respond(peer, request);
                    }
                });
            }
        });
    }
    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return server.serverPort(); }
    QString token() const { return QString(40, 'x'); }
    QJsonObject operation() const {
        return {{"operation_id", operationId}, {"state", taskState}, {"phase", "complete"}, {"method", "start"},
                {"modules", QJsonObject{}}, {"error", taskError.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonObject{{"code", taskError}, {"message", "injected failure"}, {"hint", "inspect"}})}};
    }
    virtual void respond(QTcpSocket *peer, const QJsonObject &request) {
        const auto method = request["method"].toString();
        QJsonValue result;
        QString error;
        if (authFailure || request["token"] != QJsonValue(token())) error = "AUTH";
        else if (method == "hello") result = QJsonObject{{"protocol_version", 1}, {"robot_id", "robot24"}, {"auth", "shared_token"},
            {"capabilities", QJsonArray{"profiles", "preflight", "start", "stop", "operation", "snapshot", "logs"}}};
        else if (method == "profiles") result = profileList;
        else if (method == "preflight") result = QJsonObject{{"passed", false}, {"checks", QJsonArray{QJsonObject{{"name", "device"}, {"ok", false}, {"code", "DEVICE"}}}}};
        else if (method == "start") {
            ++starts; operationId = "op-start"; software = "ready";
            if (dropStart) { peer->disconnectFromHost(); return; }
            result = operation();
        } else if (method == "stop") {
            ++stops; operationId = "op-stop"; software = "stopped";
            if (dropStop) { peer->disconnectFromHost(); return; }
            result = operation();
        } else if (method == "operation") {
            if (operationId.isEmpty()) error = "NOT_FOUND";
            else result = operation();
        } else if (method == "snapshot") result = QJsonObject{
            {"software_state", software}, {"profile_id", "mock"}, {"config_revision", "rev-A"},
            {"build_version", "test"}, {"operation_id", operationId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(operationId)},
            {"recovery_required", recoveryRequired}, {"safety", QJsonValue(QJsonValue::Null)}, {"runtime", QJsonValue(QJsonValue::Null)},
            {"control_endpoint", QJsonObject{{"host", "127.0.0.1"}, {"port", 9090}, {"protocol_version", 1}}},
            {"modules", QJsonObject{{"core", QJsonObject{{"ok", requiredHealthy}}}, {"bridge", QJsonObject{{"ok", true}}}, {"video", QJsonObject{{"ok", false}}}}}};
        else if (method == "logs") {
            const int cursor = request["params"].toObject()["cursor"].toInt();
            result = QJsonObject{{"lines", cursor == 0 ? QJsonArray{"中文日志"} : QJsonArray{}}, {"next_cursor", 1}, {"expired", true}};
        }
        QJsonObject reply{{"version", 1}, {"request_id", wrongId ? QJsonValue("old-id") : request["request_id"]}, {"ok", error.isEmpty()}};
        if (error.isEmpty()) reply["result"] = result;
        else reply["error"] = QJsonObject{{"code", error}, {"message", "injected"}, {"retryable", false}, {"hint", "check"}};
        const auto bytes = QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n';
        if (!fragmented) peer->write(bytes);
        else {
            peer->write(bytes.left(7));
            QTimer::singleShot(10, peer, [peer, bytes] { peer->write(bytes.mid(7)); });
        }
    }
};
