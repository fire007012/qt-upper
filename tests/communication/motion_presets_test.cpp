#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include "ControlSession.h"
using namespace Communication;

// Independent protocol peer: no imports, ROS, bus master, or hardware processes.
class PresetPeer {
public:
    qint64 time = 1000;
    ControlSession client{nullptr,[this]{return time;}};
    QJsonArray selected{"flippers","base","arm"};
    QHash<QString,QString> states{{"flippers","Stopped"},{"base","Stopped"},{"arm","Stopped"},{"gripper","Stopped"}};
    QList<QJsonObject> queue, wire, commands;
    QHash<QString,QJsonObject> ops;
    QString epoch = "e1", failure;
    int generation = 1, selections = 0;
    bool fresh = true, invalidated = false, unknown = false, drop = false;
    PresetPeer() { QObject::connect(&client,&ControlSession::outbound,&client,[this](const QJsonObject &m){ queue.append(m); wire.append(m); }); }
    QJsonObject token() const { return {{"epoch",epoch},{"freshness_token","t-"+epoch},{"valid_for_ms",300},{"refresh_ms",100},{"input_ttl_ms",300},{"lease_ms",1500},{"watchdog_ms",500}}; }
    QJsonObject selection() const { return {{"selected_presets",selected},{"selected_groups",ControlSession::groupsFor(selected)}}; }
    QJsonObject status() const {
        auto result = selection(); QJsonObject presets; bool all = fresh;
        for (const auto &v : QJsonArray{"flippers","base","arm"}) {
            const auto id = v.toString(); QJsonObject actual, diagnostics; bool permitted = fresh;
            for (const auto &g : ControlSession::groupsFor(QJsonArray{v})) {
                const auto state = states[g.toString()]; actual[g.toString()] = state; permitted &= state == "Running";
                diagnostics[g.toString()] = QJsonObject{{"state",state == "Running" ? 2 : state == "Armed" ? 1 : state == "Fault" ? 3 : 0}, {"ready", state == "Running"}, {"blockers",QJsonArray{}}};
            }
            presets[id] = QJsonObject{{"label",id},{"groups",ControlSession::groupsFor(QJsonArray{v})},{"selected",selected.contains(v)},{"states",actual},{"diagnostics",diagnostics},{"motion_permitted",permitted}};
            if (selected.contains(v)) all &= permitted;
        }
        result["presets"] = presets; result["epoch"] = epoch; result["runtime_fresh"] = fresh;
        result["authorized"] = client.motionAuthorized(); result["operations_authorized"] = client.operationsAuthorized();
        result["motion_permitted"] = all; result["emergency_active"] = false;
        result["counts"] = QJsonObject{}; result["queue_depth"] = 0; return result;
    }
    void reply(const QJsonObject &m, const QJsonObject &result) {
        client.receive({{"type","control_result"},{"protocol_version",2},{"request_id",m["request_id"]},{"seq",m.contains("seq")?m["seq"]:QJsonValue(QJsonValue::Null)},{"ok",true},{"result",result}});
    }
    void drain() {
        int budget = 100;
        while (!queue.isEmpty() && --budget) {
            const auto m = queue.takeFirst(); const auto type = m["type"].toString();
            if (type == "control_hello") {
                auto r = token(); selected = m.contains("presets") ? m["presets"].toArray() : selected;
                r["session_id"] = "session"; r["robot_id"] = "robot24"; r["role"] = m["role"]; r["authorized"] = false; r["build_version"] = "independent-preset-peer";
                auto caps = QJsonArray::fromStringList(ControlProtocol::capabilities()); caps.append("motion_presets_v1"); r["capabilities"] = caps;
                auto sel = selection(); for (auto it = sel.begin(); it != sel.end(); ++it) r[it.key()] = it.value();
                reply(m,r);
            } else if (type == "freshness_request") reply(m,token());
            else if (type == "control_status") reply(m,status());
            else if (type == "operator_input") reply(m,{{"input_status","neutral"}});
            else if (type == "authorize_operations") reply(m,{{"operations_authorized",true}});
            else if (type == "authorize") reply(m,{{"authorized",true}});
            else if (type == "select_presets") {
                ++selections; selected = m["presets"].toArray(); epoch = "e"+QString::number(++generation);
                auto r = selection(); r["epoch"] = epoch; r["authorized"] = false; r["operations_authorized"] = false; reply(m,r);
            } else if (type == "system_command") {
                if (drop) continue;
                commands.append(m); const auto id = m["request_id"].toString();
                QJsonObject op{{"operation_id","outer-"+id},{"request_id",id},{"robot_id","robot24"},{"epoch",epoch},{"command",m["command"]},{"state","accepted"},{"created_at",123},{"invalidated",false},{"error",QJsonValue(QJsonValue::Null)},{"result",QJsonValue(QJsonValue::Null)}};
                ops[id] = op; reply(m,op);
            } else if (type == "operation_query") {
                for (auto it = ops.begin(); it != ops.end(); ++it) if (it.value()["operation_id"] == m["operation_id"]) {
                    auto op = it.value();
                    if (op["state"] == QJsonValue("accepted")) {
                        const auto command = op["command"].toString(); QJsonArray targets;
                        for (const auto &request : commands) if (request["request_id"] == op["request_id"]) targets = request["params"].toObject()["presets"].toArray();
                        for (const auto &g : ControlSession::groupsFor(targets)) states[g.toString()] = g.toString() == failure ? "Fault" : command == "init" ? "Initialized" : command == "enable" || command == "recover" ? "Armed" : command == "resume" ? "Running" : "Halted";
                        op["state"] = unknown ? "result_unknown" : failure.isEmpty() ? "succeeded" : "failed";
                        op["invalidated"] = invalidated;
                        op["result"] = QJsonArray{QJsonObject{{"success",failure.isEmpty()},{"operation_id","inner-runtime-id"},{"presets",targets},{"requested_groups",ControlSession::groupsFor(targets)},{"message","{\"reason\":\"partial details\"}"}}};
                        epoch = "e"+QString::number(++generation); it.value() = op;
                    }
                    reply(m,op); break;
                }
            }
        }
    }
    void begin(const QJsonArray &ids, const QString &role = "control") { client.setPendingPresets(ids); client.begin("robot24","preset-fixture",true,role); drain(); }
    void tick() { time += 20; client.poll(); drain(); client.input(OperatorInputState{}); drain(); }
    void finish() { for (int i=0;i<1500 && client.diagnostics()["preset_flow_active"].toBool();++i) tick(); }
};
class MotionPresetsTest : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
private slots:
    void initTestCase() {
        QVERIFY(settings.isValid());
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,settings.path());
    }
    void init() { QSettings("HostComputer","ControlV2").clear(); }
    void combinations_data() {
        QTest::addColumn<QJsonArray>("ids");
        for (int mask=1;mask<8;++mask) { QJsonArray ids; for(int i=0;i<3;++i) if(mask&(1<<i)) ids.append(QJsonArray{"flippers","base","arm"}[i]); QTest::newRow(qPrintable(QString::number(mask))) << ids; }
    }
    void combinations() {
        QFETCH(QJsonArray,ids); PresetPeer peer; peer.begin(ids);
        QCOMPARE(peer.wire.first()["presets"].toArray(),ids); QVERIFY(peer.client.startPresets(ids)); peer.finish();
        QVERIFY2(peer.client.motionAuthorized(),qPrintable(QString::fromUtf8(QJsonDocument(peer.client.diagnostics()).toJson())));
        for(const auto &g : ControlSession::groupsFor(ids)) QCOMPARE(peer.states[g.toString()],QString("Running"));
        for(const auto &m : peer.commands) QCOMPARE(m["params"].toObject().size(),1);
    }
    void additionsOnlyStartNewPreset() {
        PresetPeer peer; peer.begin({"base"}); peer.states["base"]="Running"; peer.tick();
        QVERIFY(peer.client.startPresets({"base","arm"})); peer.finish(); QVERIFY(peer.client.motionAuthorized()); QCOMPARE(peer.selections,1);
        for (const auto &m : peer.commands) QCOMPARE(m["params"].toObject()["presets"].toArray(),QJsonArray{"arm"});
    }
    void removalStopsBeforeSelection() {
        PresetPeer peer; peer.begin({"base","arm"}); for(auto &state:peer.states) state="Running"; peer.tick();
        peer.client.input(OperatorInputState{}); peer.drain(); QVERIFY(!peer.client.selectPresets({"base"}));
        QVERIFY(peer.client.startPresets({"base"})); peer.finish(); QVERIFY(peer.client.motionAuthorized());
        QCOMPARE(peer.commands.size(),1); QCOMPARE(peer.commands[0]["command"].toString(),QString("halt")); QCOMPARE(peer.selected,QJsonArray{"base"});
        QCOMPARE(peer.states["arm"],QString("Halted")); QCOMPARE(peer.states["gripper"],QString("Halted"));
        QVERIFY(peer.client.diagnostics()["epoch"] != QJsonValue("e1"));
    }
    void explicitStopStillWorksWithEmptyDraft() {
        PresetPeer peer; peer.begin({"base"}); peer.states["base"]="Running";
        for(int i=0;i<12;++i) peer.tick(); peer.client.setPendingPresets({}); QVERIFY(!peer.client.authorizeMotion());
        QVERIFY(peer.client.haltPresets({"base"})); peer.finish(); QCOMPARE(peer.states["base"],QString("Halted"));
        QVERIFY(!peer.client.motionAuthorized()); QCOMPARE(peer.commands.size(),1); QCOMPARE(peer.commands[0]["command"].toString(),QString("halt"));
    }
    void partialFailurePreservesBothStatesAndNoRollback() {
        PresetPeer peer; peer.begin({"arm"}); peer.failure="gripper";
        QVERIFY(peer.client.startPresets({"arm"})); peer.finish(); QVERIFY(!peer.client.motionAuthorized());
        QCOMPARE(peer.commands.size(),1); QCOMPARE(peer.states["arm"],QString("Initialized")); QCOMPARE(peer.states["gripper"],QString("Fault"));
        QCOMPARE(peer.client.operations()[0].toObject()["state"].toString(),QString("failed"));
    }
    void invalidatedSuccessReconcilesNewEpochWithoutReplay() {
        PresetPeer peer; peer.begin({"base"}); peer.invalidated=true;
        QVERIFY(peer.client.startPresets({"base"})); peer.finish(); QVERIFY(peer.client.motionAuthorized()); QCOMPARE(peer.commands.size(),3);
    }
    void unknownNeverReplaysEnableOrResume() {
        PresetPeer peer; peer.begin({"base"}); peer.unknown=true;
        QVERIFY(peer.client.startPresets({"base"})); peer.finish(); QVERIFY(!peer.client.motionAuthorized()); QCOMPARE(peer.commands.size(),1);
        QVERIFY(!peer.client.startPresets({"base"}));
        const auto op=peer.client.operations()[0].toObject(); QVERIFY(peer.client.recoverRequest(op["request_id"].toString())); peer.drain(); QCOMPARE(peer.commands.size(),1);
    }
    void staleFeedbackAndUnselectedFault() {
        PresetPeer peer; peer.begin({"base"}); peer.states["arm"]="Fault";
        QVERIFY(peer.client.startPresets({"base"})); peer.finish(); QVERIFY(peer.client.motionAuthorized());
        peer.fresh=false; for(int i=0;i<12;++i) peer.tick(); QVERIFY(!peer.client.motionAuthorized());
        QVERIFY(!peer.client.diagnostics()["motion_permitted"].toBool());
    }
    void freshPermissionLossRevokesWithoutRestart() {
        PresetPeer peer; peer.begin({"base"}); QVERIFY(peer.client.startPresets({"base"})); peer.finish(); QVERIFY(peer.client.motionAuthorized());
        const auto count=peer.commands.size(); peer.states["base"]="Halted"; for(int i=0;i<12;++i) peer.tick();
        QVERIFY(!peer.client.motionAuthorized()); QVERIFY(!peer.client.operationsAuthorized()); QCOMPARE(peer.commands.size(),count);
    }
    void invalidSelectionsAndRawGroupsRejected() {
        QVERIFY(!ControlSession::validPresets({})); QVERIFY(!ControlSession::validPresets({"base","base"})); QVERIFY(!ControlSession::validPresets({"gripper"})); QVERIFY(!ControlSession::validPresets({1}));
        PresetPeer peer; peer.begin({"base"}); peer.tick(); QVERIFY(peer.client.authorizeOperations()); peer.drain();
        QVERIFY(!peer.client.command("init",{{"groups",QJsonArray{"base"}}}));
        QVERIFY(!peer.client.command("init",{{"presets",QJsonArray{"arm"}}}));
        QVERIFY(!peer.client.command("set_control_mode",{{"mode","arm"}}));
        QVERIFY(peer.client.setPendingPresets({})); QVERIFY(!peer.client.startPresets({}));
    }
    void monitorNeverSelectsOrAuthorizes() {
        PresetPeer peer; peer.begin({},"monitor");
        QVERIFY(!peer.wire.first().contains("presets")); QCOMPARE(peer.selected,QJsonArray({"flippers","base","arm"}));
        QVERIFY(!peer.client.authorizeMotion()); QVERIFY(!peer.client.authorizeOperations()); QVERIFY(!peer.client.selectPresets({"base"})); QVERIFY(!peer.client.startPresets({"base"})); QVERIFY(!peer.client.haltPresets({"base"}));
    }
    void unselectedInputsAreFiltered() {
        PresetPeer peer; peer.begin({"base"}); QVERIFY(peer.client.startPresets({"base"})); peer.finish();
        OperatorInputState input; input.keyboard.pressedKeys={"w","y","f"}; input.gamepad.connected=true; input.gamepad.axes.lt=1; input.gamepad.buttons.lb=true;
        QVERIFY(peer.client.input(input)); auto m=peer.wire.last(); QCOMPARE(m["keyboard"].toObject()["pressed_keys"].toArray(),QJsonArray{"w"});
        QCOMPARE(m["gamepad"].toObject()["axes"].toObject()["lt"].toDouble(),0.0); QVERIFY(!m["gamepad"].toObject()["buttons"].toObject()["lb"].toBool());
        peer.drain(); input.mode="arm"; QVERIFY(peer.client.input(input)); m=peer.wire.last(); QVERIFY(m["keyboard"].toObject()["pressed_keys"].toArray().isEmpty());
    }
    void selectionCannotReviveOldEpochTokenOrAuthorization() {
        PresetPeer peer; peer.begin({"base"}); peer.states["base"]="Running";
        for(int i=0;i<12;++i) peer.tick();
        peer.time+=200; peer.client.poll(); QJsonObject oldStatus;
        for(int i=0;i<peer.queue.size();++i) if(peer.queue[i]["type"]==QJsonValue("control_status")) { oldStatus=peer.queue.takeAt(i); break; }
        const auto previous=peer.status(); peer.drain(); peer.client.input({}); peer.drain();
        QVERIFY(peer.client.authorizeMotion()); const auto oldGrant=peer.queue.takeFirst();
        QVERIFY(peer.client.selectPresets({"base","arm"})); peer.drain();
        QVERIFY(!peer.client.diagnostics()["token_valid"].toBool()); const auto epoch=peer.client.diagnostics()["epoch"].toString();
        peer.reply(oldStatus,previous); peer.reply(oldGrant,{{"authorized",true}});
        QCOMPARE(peer.client.diagnostics()["epoch"].toString(),epoch); QVERIFY(!peer.client.motionAuthorized()); QVERIFY(!peer.client.operationsAuthorized());
        QVERIFY(!peer.client.input({})); peer.tick(); QVERIFY(!peer.client.motionAuthorized());
    }
    void lostAcceptanceRecoveryKeepsExactSubsetAndId() {
        PresetPeer peer; peer.begin({"base"}); peer.drop=true; QVERIFY(peer.client.startPresets({"base"})); peer.finish();
        QJsonObject original; for(const auto &m:peer.wire) if(m["type"]==QJsonValue("system_command")) original=m;
        QVERIFY(!original.isEmpty()); QCOMPARE(peer.client.operations()[0].toObject()["state"].toString(),QString("result_unknown"));
        peer.client.setPendingPresets({"arm"}); peer.drop=false; for(int i=0;i<12;++i) peer.tick();
        QVERIFY(peer.client.recoverRequest(original["request_id"].toString()));
        const auto retry=peer.wire.last(); QCOMPARE(retry["request_id"],original["request_id"]); QCOMPARE(retry["params"],original["params"]);
        QCOMPARE(retry["params"].toObject()["presets"].toArray(),QJsonArray{"base"});
    }
    void emergencyCancelsStartupAndRevokesAll() {
        PresetPeer peer; peer.begin({"base"}); QVERIFY(peer.client.startPresets({"base"})); peer.client.emergencyIntent();
        QVERIFY(!peer.client.diagnostics()["preset_flow_active"].toBool()); QVERIFY(!peer.client.motionAuthorized());
        for(int i=0;i<30;++i) peer.tick(); QVERIFY(peer.commands.isEmpty());
    }
};
QTEST_GUILESS_MAIN(MotionPresetsTest)
#include "motion_presets_test.moc"
