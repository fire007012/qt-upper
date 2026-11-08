#pragma once
#include "manager_stub.h"

class ConfigurationStub : public ManagerStub {
public:
    QString base = QString(64, 'a'), target = QString(64, 'b');
    QJsonObject pendingCandidate, configOperation;
    bool dropApply = false, oversizedApply = false, failApply = false, malformedStatus = false;
    int saves = 0, applies = 0;
    QStringList applyIds;
    QJsonObject policy = {{"profile_id", "mock"}, {"config_revision", "profile-revision"}, {"startup_timeout", 90}, {"video", false}};
    QJsonObject schema() const {
        return {{"schema_version", 1}, {"capability", "config_transactions_v1"}, {"scope", "manager_profiles"},
            {"fields", QJsonObject{{"startup_timeout", QJsonObject{{"type", "number"}, {"minimum", 1}, {"maximum", 180}, {"unit", "s"}, {"editable", true}}},
                {"video_required", QJsonObject{{"type", "boolean"}, {"editable", true}}}}}, {"readonly_fields", QJsonArray{"devices"}}};
    }
    void respond(QTcpSocket *peer, const QJsonObject &request) override {
        const auto method = request["method"].toString();
        const auto params = request["params"].toObject();
        QJsonValue result;
        QString error;
        if (method == "hello") {
            result = QJsonObject{{"protocol_version", 1}, {"robot_id", "robot24"}, {"auth", "shared_token"},
                {"capabilities", QJsonArray{"profiles", "preflight", "start", "stop", "operation", "snapshot", "logs", "config.schema", "config.status", "config.source", "config.candidate", "config.preview", "config.apply", "config.rollback", "config.operation", "config.audit"}}};
        } else if (method == "config.schema") result = schema();
        else if (method == "config.status") result = QJsonObject{{"source_revision", malformedStatus ? "invalid" : base},
            {"effective_management_revision", base}, {"actual_runtime_revision", QJsonValue(QJsonValue::Null)},
            {"pending_restart", true}, {"applying", false}, {"device_nvm_written", false},
            {"candidate_revisions", pendingCandidate.isEmpty() ? QJsonArray{} : QJsonArray{target}}};
        else if (method == "config.source") result = QJsonObject{{"revision", params.contains("revision") ? params["revision"] : QJsonValue(base)}, {"profile", policy}, {"summary", QJsonObject{{"profile_count", 1}}}};
        else if (method == "config.candidate") {
            ++saves;
            if (params["base_revision"] != QJsonValue(base)) error = "REVISION_CONFLICT";
            else {
                const auto patch = params["patches"].toArray().first().toObject();
                pendingCandidate = {{"base_revision", base}, {"candidate_revision", target},
                    {"diff", QJsonArray{QJsonObject{{"profile_id", "mock"}, {"field", patch["field"]}, {"before", 90}, {"after", patch["value"]}}}},
                    {"affected_groups", QJsonArray{"all"}}, {"maintenance_required", true}, {"pending_restart", true}, {"device_nvm_written", false}};
                result = pendingCandidate;
            }
        } else if (method == "config.preview") result = pendingCandidate;
        else if (method == "config.apply" || method == "config.rollback") {
            const auto id = request["request_id"].toString();
            applyIds.append(id);
            if (!configOperation.isEmpty() && id == configOperation["request_id"].toString()) result = configOperation;
            else if (params["base_revision"] != QJsonValue(base)) error = "REVISION_CONFLICT";
            else {
                ++applies;
                configOperation = {{"operation_id", "config-op"}, {"request_id", id}, {"method", method}, {"base_revision", base},
                    {"target_revision", method == "config.apply" ? params["candidate_revision"] : params["target_revision"]},
                    {"state", failApply ? "failed" : "succeeded"}, {"phase", failApply ? "rolled_back" : "complete"},
                    {"pending_restart", true}, {"verification", failApply ? QJsonValue(QJsonValue::Null) : QJsonValue("manager_source_readback")}};
                if (!failApply) {
                    base = configOperation["target_revision"].toString();
                    policy["startup_timeout"] = pendingCandidate["diff"].toArray().first().toObject()["after"];
                    auto profile = profileList.first().toObject(); profile["config_revision"] = base;
                    profileList[0] = profile;
                }
                if (dropApply) { dropApply = false; peer->disconnectFromHost(); return; }
                if (oversizedApply) { oversizedApply = false; error = "RESPONSE_TOO_LARGE"; }
                result = configOperation;
            }
        } else if (method == "config.operation") result = configOperation;
        else if (method == "config.audit") result = QJsonObject{{"items", QJsonArray{QJsonObject{{"event", "candidate_saved"}, {"revision", target}}}}, {"next_cursor", params["cursor"].toInt() + 1}};
        else { ManagerStub::respond(peer, request); return; }
        QJsonObject frame{{"version", 1}, {"request_id", request["request_id"]}, {"ok", error.isEmpty()}};
        if (error.isEmpty()) frame["result"] = result;
        else frame["error"] = QJsonObject{{"code", error}, {"message", "injected"}};
        peer->write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
    }
};
