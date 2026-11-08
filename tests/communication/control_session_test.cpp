#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QFile>
#include <QCryptographicHash>
#include "ControlSession.h"
#include "../..//src/state/RobotStateStore.h"

using namespace Communication;
class SessionPeer {
public:
    qint64 time = 1000;
    ControlSession session{nullptr, [this] { return time; }};
    QList<QJsonObject> sent;
    SessionPeer() { QObject::connect(&session, &ControlSession::outbound, &session, [this](const QJsonObject &m) { sent.append(m); }); }
    QJsonObject last(const QString &type) const {
        for (auto it=sent.crbegin(); it!=sent.crend(); ++it) if ((*it)["type"] == QJsonValue(type)) return *it;
        return {};
    }
    void reply(const QJsonObject &request, const QJsonObject &result) {
        session.receive({{"type", "control_result"}, {"protocol_version", 2}, {"request_id", request["request_id"]},
            {"seq", request.contains("seq") ? request["seq"] : QJsonValue(QJsonValue::Null)}, {"ok", true}, {"result", result}});
    }
    QJsonObject token() const { return {{"epoch", "epoch-A"}, {"freshness_token", "fixture-token"}, {"valid_for_ms", 300}, {"refresh_ms",100}, {"input_ttl_ms",300}, {"lease_ms",1500}, {"watchdog_ms",500}}; }
    QJsonObject hello() const {
        auto result=token(); result["session_id"]="session-A"; result["robot_id"]="robot24"; result["role"]="control";
        result["capabilities"]=QJsonArray::fromStringList(ControlProtocol::capabilities()); result["authorized"]=false; result["build_version"]="fixture"; return result;
    }
    QJsonObject status(const QString &epoch = "epoch-A", bool authorized = false) const {
        return {{"epoch", epoch}, {"authorized", authorized}, {"motion_permitted",true}, {"emergency_active",false}, {"counts", QJsonObject{}}, {"queue_depth",0}};
    }
    void begin() {
        session.begin("robot24", "fixture:9090"); reply(last("control_hello"), hello()); reply(last("control_status"), status());
    }
    void neutral() { session.input(OperatorInputState{}); reply(last("operator_input"), {{"input_status", "neutral"}, {"authorized", false}}); }
    void arm() { neutral(); session.authorizeOperations(); reply(last("authorize_operations"), {{"operations_authorized",true}, {"authorized",false}}); }
    QJsonObject operation(const QString &id, const QString &state, bool invalidated = false) const {
        return {{"operation_id","op-1"},{"request_id", id},{"robot_id","robot24"},{"epoch","epoch-A"},
            {"command","init"},{"state",state},{"created_at",1234},{"invalidated",invalidated}, {"error", QJsonValue(QJsonValue::Null)}, {"result", QJsonValue(QJsonValue::Null)}};
    }
};
class ControlSessionTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
private slots:
    void initTestCase() {
        QVERIFY(directory.isValid());
        QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,directory.path());
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,directory.path());
    }
    void init() { QSettings("HostComputer","ControlV2").clear(); }
    void handshakeNeedsAllCapabilitiesAndIdentity() {
        SessionPeer peer; QSignalSpy fault(&peer.session,&ControlSession::fault);
        peer.session.begin("robot24","fixture:9090"); auto hello=peer.hello(); hello["capabilities"]=QJsonArray{"session_v2"};
        peer.reply(peer.last("control_hello"),hello); QCOMPARE(fault.size(),1); QVERIFY(!fault[0][1].toBool()); QVERIFY(!peer.session.ready());
    }
    void negotiationTimeoutRequestsConnectionRecovery() {
        SessionPeer peer; QSignalSpy fault(&peer.session,&ControlSession::fault);
        peer.session.begin("robot24","fixture:9090"); peer.time+=2500; peer.session.poll();
        QCOMPARE(fault.size(),1); QVERIFY(fault[0][1].toBool()); QVERIFY(!peer.session.ready());
    }
    void statusNeverGrantsMotionAndTwoAuthorizationsAreSeparate() {
        SessionPeer peer; peer.begin(); QVERIFY(peer.session.ready()); QVERIFY(!peer.session.authorizeMotion());
        peer.arm(); QVERIFY(peer.session.operationsAuthorized()); QVERIFY(!peer.session.motionAuthorized());
        QVERIFY(peer.session.authorizeMotion()); const auto authorize=peer.last("authorize"); QVERIFY(!peer.session.motionAuthorized());
        peer.reply(authorize,{{"authorized",true}}); QVERIFY(peer.session.motionAuthorized());
        peer.session.reset(); peer.begin(); QVERIFY(!peer.session.motionAuthorized()); QVERIFY(!peer.session.operationsAuthorized());
        peer.time+=200; peer.session.poll(); peer.reply(peer.last("control_status"),peer.status("epoch-A",true));
        QVERIFY(!peer.session.motionAuthorized());
    }
    void tokenBudgetSubtractsNetworkTimeAndRequiresResample() {
        SessionPeer peer; peer.begin(); peer.arm();
        OperatorInputState motion; motion.keyboard.pressedKeys=QStringList{"w"};
        QVERIFY(peer.session.authorizeMotion()); peer.reply(peer.last("authorize"),{{"authorized",true}});
        QVERIFY(peer.session.input(motion)); peer.reply(peer.last("operator_input"),{{"input_status","staged"}});
        peer.time+=100; peer.session.poll(); const auto fresh=peer.last("freshness_request");
        const auto count=peer.sent.size(); QSignalSpy sample(&peer.session,&ControlSession::inputSampleRequested);
        peer.time+=190; peer.reply(fresh,peer.token()); QCOMPARE(sample.size(),1); QCOMPARE(peer.sent.size(),count);
        peer.time+=111; peer.session.poll(); QVERIFY(!peer.session.motionAuthorized()); QVERIFY(!peer.session.operationsAuthorized());
        QVERIFY(!peer.session.input(motion));
    }
    void epochAndOldAuthorizationResponseCannotRearm() {
        SessionPeer peer; peer.begin(); peer.neutral(); QVERIFY(peer.session.authorizeMotion()); const auto authorization=peer.last("authorize");
        peer.time+=200; peer.session.poll(); peer.reply(peer.last("control_status"),peer.status("epoch-B",false));
        peer.reply(authorization,{{"authorized",true}}); QVERIFY(!peer.session.motionAuthorized()); QVERIFY(!peer.session.authorizeMotion());
        QCOMPARE(peer.session.diagnostics()["epoch"].toString(),QString("epoch-B"));
    }
    void pausedSyntheticZeroDoesNotProvePhysicalNeutral() {
        SessionPeer peer; peer.begin(); OperatorInputState held; held.keyboard.pressedKeys=QStringList{"w","1","tab"};
        QVERIFY(peer.session.input(held)); const auto input=peer.last("operator_input");
        QVERIFY(input["keyboard"].toObject()["pressed_keys"].toArray().isEmpty());
        QVERIFY(!input["gamepad"].toObject()["buttons"].toObject().contains("start"));
        peer.reply(input,{{"input_status","neutral"},{"authorized",false}}); QVERIFY(!peer.session.authorizeOperations());
        peer.neutral(); QVERIFY(peer.session.authorizeOperations());
    }
    void mismatchedSequenceFailsClosed() {
        SessionPeer peer; peer.begin(); peer.neutral(); QVERIFY(peer.session.authorizeMotion());
        const auto request=peer.last("authorize"); QSignalSpy fault(&peer.session,&ControlSession::fault);
        peer.session.receive({{"type","control_result"},{"protocol_version",2},{"request_id",request["request_id"]},{"seq",9999},{"ok",true},{"result",QJsonObject{{"authorized",true}}}});
        QCOMPARE(fault.size(),1); QVERIFY(!peer.session.motionAuthorized());
    }
    void lostAcceptancePersistsAndOnlyExplicitSameIdRecovers() {
        QString id;
        { SessionPeer peer; peer.begin(); peer.arm(); QVERIFY(peer.session.command("init",{})); id=peer.last("system_command")["request_id"].toString(); peer.session.reset(); }
        SessionPeer restored; restored.begin();
        QVERIFY(restored.last("system_command").isEmpty()); QVERIFY(!restored.session.operationsAuthorized());
        const auto records=restored.session.operations(); QCOMPARE(records.size(),1); QCOMPARE(records[0].toObject()["state"].toString(),QString("result_unknown"));
        QVERIFY(restored.session.recoverRequest(id)); QCOMPARE(restored.last("system_command")["request_id"].toString(),id);
        restored.reply(restored.last("system_command"),restored.operation(id,"running"));
        QCOMPARE(restored.session.operations()[0].toObject()["operation_id"].toString(),QString("op-1"));
        QSettings settings("HostComputer","ControlV2"); QFile file(settings.fileName()); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes=file.readAll();
        QVERIFY(!bytes.contains("fixture-token")); QVERIFY(!bytes.contains("session-A"));
    }
    void acceptedOperationIsNotCompletedAndInvalidationRevokes() {
        SessionPeer peer; peer.begin(); peer.arm(); QVERIFY(peer.session.command("init",{})); const auto request=peer.last("system_command"); const auto id=request["request_id"].toString();
        QSignalSpy business(&peer.session,&ControlSession::businessResult);
        peer.reply(request,peer.operation(id,"accepted")); QCOMPARE(business.size(),0);
        peer.time+=100; peer.session.poll(); const auto query=peer.last("operation_query"); QVERIFY(!query.isEmpty());
        auto result=peer.operation(id,"succeeded",true); result["result"]=QJsonArray{QJsonObject{{"type","system_status"},{"control_mode","arm"}}};
        peer.reply(query,result); QCOMPARE(business.size(),0); QVERIFY(!peer.session.operationsAuthorized());
        QCOMPARE(peer.session.operations()[0].toObject()["invalidated"],QJsonValue(true));
    }
    void revokedAuthorizationReplyCannotRearmAfterFreshNeutral() {
        SessionPeer peer; peer.begin(); peer.neutral(); QVERIFY(peer.session.authorizeMotion());
        const auto authorization=peer.last("authorize"); peer.session.emergencyIntent();
        peer.time+=100; peer.session.poll(); peer.reply(peer.last("freshness_request"),peer.token());
        peer.neutral(); peer.reply(authorization,{{"authorized",true}});
        QVERIFY(!peer.session.motionAuthorized());
    }
    void newerRuntimeDenialPreventsPendingAuthorization() {
        SessionPeer peer; peer.begin(); peer.neutral(); QVERIFY(peer.session.authorizeMotion());
        const auto authorization=peer.last("authorize"); peer.time+=200; peer.session.poll();
        auto status=peer.status(); status["motion_permitted"]=false;
        peer.reply(peer.last("control_status"),status); peer.reply(authorization,{{"authorized",true}});
        QVERIFY(!peer.session.motionAuthorized());
    }
    void runtimeRecoveryCannotReviveGrantFromBeforeDenial() {
        SessionPeer peer; peer.begin(); peer.neutral(); QVERIFY(peer.session.authorizeMotion());
        const auto authorization=peer.last("authorize");
        peer.time+=100; peer.session.poll(); peer.reply(peer.last("freshness_request"),peer.token());
        peer.time+=100; peer.session.poll(); auto denied=peer.status(); denied["motion_permitted"]=false;
        peer.reply(peer.last("control_status"),denied);
        peer.reply(peer.last("freshness_request"),peer.token()); peer.neutral();
        peer.time+=100; peer.session.poll(); peer.reply(peer.last("freshness_request"),peer.token());
        peer.time+=100; peer.session.poll(); peer.reply(peer.last("freshness_request"),peer.token());
        peer.reply(peer.last("control_status"),peer.status()); peer.neutral();
        peer.reply(authorization,{{"authorized",true}});
        QVERIFY(peer.session.ready()); QVERIFY(!peer.session.motionAuthorized());
    }
    void tokenRenewalBeforeDelayedTimerStillRevokesAuthority() {
        SessionPeer peer; peer.begin(); peer.arm(); QVERIFY(peer.session.authorizeMotion());
        peer.reply(peer.last("authorize"),{{"authorized",true}});
        peer.time+=100; peer.session.poll(); const auto refresh=peer.last("freshness_request");
        peer.time+=201; // Old token expired; no timer has run yet.
        peer.reply(refresh,peer.token());
        QVERIFY(peer.session.ready()); QVERIFY(!peer.session.motionAuthorized()); QVERIFY(!peer.session.operationsAuthorized());
        peer.neutral(); QVERIFY(peer.session.authorizeMotion());
    }
    void lateAcceptanceIsDurablyUnknownEvenBeforeTimerPoll() {
        SessionPeer peer; peer.begin(); peer.arm(); QVERIFY(peer.session.command("init",{}));
        const auto request=peer.last("system_command"); peer.time+=1001;
        peer.reply(request,peer.operation(request["request_id"].toString(),"accepted"));
        QCOMPARE(peer.session.operations()[0].toObject()["state"].toString(),QString("result_unknown"));
        QVERIFY(!peer.session.operationsAuthorized());
        peer.session.reset(); SessionPeer restored; restored.begin();
        QCOMPARE(restored.session.operations()[0].toObject()["state"].toString(),QString("result_unknown"));
    }
    void corruptJournalIsPreservedAndBlocksNewOperations() {
        const auto scope=QString::fromLatin1(QCryptographicHash::hash(QByteArray("fixture:9090/robot24"),QCryptographicHash::Sha256).toHex());
        QSettings settings("HostComputer","ControlV2"); settings.setValue(scope+"/records",QByteArray("{corrupt")); settings.sync();
        SessionPeer peer; peer.begin(); peer.arm(); QVERIFY(!peer.session.command("init",{}));
        QVERIFY(!peer.session.diagnostics()["journal_healthy"].toBool()); peer.session.reset();
        settings.sync(); QCOMPARE(settings.value(scope+"/records").toByteArray(),QByteArray("{corrupt"));
    }
    void zeroBudgetTokenExpiresWithoutProtocolFault() {
        SessionPeer peer; peer.begin(); peer.arm(); QSignalSpy fault(&peer.session,&ControlSession::fault);
        peer.time+=100; peer.session.poll(); auto token=peer.token(); token["valid_for_ms"]=0;
        peer.reply(peer.last("freshness_request"),token); QVERIFY(!peer.session.ready()); QCOMPARE(fault.size(),0);
    }
    void stateExpiryRedactionAndAtomicExport() {
        qint64 time=1000; RobotStateStore store(nullptr,[&]{return time;});
        store.observe("co2",850,3000); QVERIFY(store.snapshot()["topics"].toObject()["co2"].toObject()["fresh"].toBool());
        time+=3000; store.expire(); QVERIFY(!store.snapshot()["topics"].toObject()["co2"].toObject()["fresh"].toBool());
        for(int i=0;i<300;++i) store.ingest({{"type","control_notice"},{"freshness_token","sensitive"},{"url","rtsp://user:password@127.0.0.1/a?token=hidden"}});
        const auto path=directory.filePath("diagnostics.json"); QVERIFY(store.exportFile(path));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes=file.readAll();
        QVERIFY(!bytes.contains("sensitive")); QVERIFY(!bytes.contains("user:password")); QVERIFY(!bytes.contains("token=hidden"));
        QCOMPARE(store.diagnosticBundle()["events"].toArray().size(),256);
        store.invalidate(); QVERIFY(store.snapshot()["topics"].toObject()["co2"].toObject()["value"].isNull());
        QVERIFY(!store.exportFile(directory.filePath("protected.json"),{},directory.path()));
        const auto target=directory.filePath("reference.json");
        QFile original(target); QVERIFY(original.open(QIODevice::WriteOnly)); original.write("read-only-reference"); original.close();
        const auto link=directory.filePath("linked.json");
        if (QFile::link(target,link) && QFileInfo(link).isSymLink()) {
            QVERIFY(!store.exportFile(link));
            QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(),QByteArray("read-only-reference"));
        }
    }
};
QTEST_GUILESS_MAIN(ControlSessionTest)
#include "control_session_test.moc"
