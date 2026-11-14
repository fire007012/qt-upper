#pragma once
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <memory>
#include "ControlProtocol.h"
#include "ControlSession.h"

// Independent socket peer. It neither imports nor emulates hardware/ROS.
class ControlPeer : public QObject {
public:
    QTcpServer server;
    QList<QTcpSocket *> peers;
    quint64 acceptedConnections = 0, motionSamples = 0;
    QList<QJsonObject> messages;
    bool freshness = true, status = true, presetsEnabled = false, advertiseCapabilities = true;
    int capabilitiesDelayMs = 0;
    QJsonArray selectedPresets{"base"};
    QString identity = "robot24";
    ~ControlPeer() override {
        server.close();
        // QTcpServer destroys its child sockets after our list members. Stop
        // callbacks now, before disconnected can access destroyed containers.
        for (auto *socket : peers) { socket->disconnect(this); socket->abort(); }
    }
    ControlPeer() {
        connect(&server,&QTcpServer::newConnection,this,[this]{
            while(server.hasPendingConnections()) {
                auto *socket=server.nextPendingConnection(); peers.append(socket); ++acceptedConnections;
                connect(socket,&QTcpSocket::disconnected,this,[this,socket]{peers.removeOne(socket);socket->deleteLater();});
                auto bytes=std::make_shared<QByteArray>();
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,bytes]{
                    *bytes+=socket->readAll();
                    while(bytes->contains('\n')) {
                        const auto end=bytes->indexOf('\n'); const auto message=QJsonDocument::fromJson(bytes->left(end)).object(); bytes->remove(0,end+1);
                        messages.append(message); if(messages.size()>256)messages.removeFirst(); handle(socket,message);
                    }
                });
                if (advertiseCapabilities) QTimer::singleShot(capabilitiesDelayMs, socket, [this,socket] {
                    auto caps=QJsonArray::fromStringList(Communication::ControlProtocol::capabilities());
                    if(presetsEnabled) caps.append("motion_presets_v1");
                    send(socket,{{"type","capabilities"},{"control_protocol_version",2},{"control_capabilities",caps}});
                });
                send(socket,{{"type","hello"},{"protocol_version",1},{"robot_id",identity}});
            }
        });
    }
    void send(QTcpSocket *socket,const QJsonObject &value) { socket->write(QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n'); }
    void reply(QTcpSocket *socket,const QJsonObject &request,const QJsonObject &value) {
        send(socket,{{"type","control_result"},{"protocol_version",2},{"request_id",request["request_id"]},
            {"seq",request.contains("seq")?request["seq"]:QJsonValue(QJsonValue::Null)},{"ok",true},{"result",value}});
    }
    QJsonObject token() const {return {{"epoch","epoch-A"},{"freshness_token","peer-token"},{"valid_for_ms",300},{"refresh_ms",100},{"input_ttl_ms",300},{"lease_ms",1500},{"watchdog_ms",500}};}
    virtual void handle(QTcpSocket *socket,const QJsonObject &m) {
        const auto type=m["type"].toString();
        if(type=="heartbeat") send(socket,{{"type","heartbeat_ack"},{"seq",m["seq"]},{"protocol_version",1}});
        else if(type=="sync_request") send(socket,{{"type","system_snapshot"},{"seq",m["seq"]},{"emergency",QJsonObject{{"active",false}}}});
        else if(type=="control_hello") {
            auto hello=token(); hello["session_id"]="peer-session";hello["robot_id"]=identity;hello["role"]="control";
            hello["capabilities"]=QJsonArray::fromStringList(Communication::ControlProtocol::capabilities());hello["authorized"]=false;hello["build_version"]="independent-peer";
            if(presetsEnabled) {
                selectedPresets=m.contains("presets") ? m["presets"].toArray() : QJsonArray{"base","flippers","arm"}; auto caps=hello["capabilities"].toArray(); caps.append("motion_presets_v1"); hello["capabilities"]=caps;
                hello["selected_presets"]=selectedPresets; hello["selected_groups"]=Communication::ControlSession::groupsFor(selectedPresets);
            }
            reply(socket,m,hello);
        } else if(type=="freshness_request"&&freshness) reply(socket,m,token());
        else if(type=="control_status"&&status) {
            QJsonObject r{{"epoch","epoch-A"},{"authorized",true},{"motion_permitted",true},{"emergency_active",false},{"counts",QJsonObject{}},{"queue_depth",0}};
            if(presetsEnabled) { r["selected_presets"]=selectedPresets; r["selected_groups"]=Communication::ControlSession::groupsFor(selectedPresets); r["presets"]=QJsonObject{}; r["runtime_fresh"]=true; r["operations_authorized"]=false; }
            reply(socket,m,r);
        }
        else if(type=="operator_input") {
            if (!m["keyboard"].toObject()["pressed_keys"].toArray().isEmpty()) ++motionSamples;
            reply(socket,m,{{"input_status","neutral"}});
        }
        else if(type=="authorize") reply(socket,m,{{"authorized",true}});
        else if(type=="authorize_operations") reply(socket,m,{{"operations_authorized",true}});
        else if(type=="emergency_stop") reply(socket,m,{{"emergency_active",true},{"physical_stop_confirmed",false}});
    }
};
