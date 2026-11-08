#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QJsonDocument>
#include <memory>
#include "ManagementClient.h"
#include "RuntimeService.h"
using Communication::ManagementClient;

#include "manager_stub.h"

class ManagementClientTest : public QObject {
    Q_OBJECT
private slots:
    void transportSerialAndFragmented() {
        ManagerStub stub; QVERIFY(stub.listen()); stub.fragmented = true;
        ManagementClient client; QSignalSpy connected(&client, &ManagementClient::connected);
        QSignalSpy responses(&client, &ManagementClient::response);
        client.open("127.0.0.1", stub.port(), stub.token()); QTRY_COMPARE(connected.count(), 1);
        QVERIFY(client.request("hello")); QVERIFY(!client.request("profiles"));
        QTRY_COMPARE(responses.count(), 1); QVERIFY(client.idle());
        QCOMPARE(responses[0][0].toString(), QString("hello"));
        QCOMPARE(stub.requests[0]["version"].toInt(), 1);
        QVERIFY(client.request("profiles")); QTRY_COMPARE(responses.count(), 2);
    }
    void wrongRequestIdClosesConnection() {
        ManagerStub stub; QVERIFY(stub.listen()); stub.wrongId = true;
        ManagementClient client; QSignalSpy connected(&client, &ManagementClient::connected), lost(&client, &ManagementClient::lost);
        client.open("127.0.0.1", stub.port(), stub.token()); QTRY_COMPARE(connected.count(), 1);
        QVERIFY(client.request("start")); QTRY_COMPARE(lost.count(), 1);
        QVERIFY(lost[0][1].toBool()); QVERIFY(!client.idle());
    }
    void oversizedAndMalformed_data() {
        QTest::addColumn<QByteArray>("frame");
        QTest::newRow("oversized") << QByteArray(65536, 'a');
        QTest::newRow("malformed") << QByteArray("not json\n");
        QTest::newRow("array") << QByteArray("[]\n");
    }
    void oversizedAndMalformed() {
        QFETCH(QByteArray, frame);
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        ManagementClient client; QSignalSpy connected(&client, &ManagementClient::connected), lost(&client, &ManagementClient::lost);
        client.open("127.0.0.1", server.serverPort(), QString(40, 'x')); QTRY_COMPARE(connected.count(), 1);
        QVERIFY(client.request("hello"));
        QVERIFY(server.hasPendingConnections()); auto *peer = server.nextPendingConnection(); peer->write(frame);
        QTRY_COMPARE(lost.count(), 1); QVERIFY(!client.idle());
    }
    void responseTimeoutIsUncertain()
    {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        ManagementClient client;
        QSignalSpy connected(&client, &ManagementClient::connected), lost(&client, &ManagementClient::lost);
        client.open("127.0.0.1", server.serverPort(), QString(40, 'x'));
        QTRY_COMPARE(connected.count(), 1);
        auto *peer = server.nextPendingConnection(); QVERIFY(peer);
        QVERIFY(client.request("start"));
        QTRY_COMPARE_WITH_TIMEOUT(lost.count(), 1, 17000);
        QVERIFY(lost[0][1].toBool()); QVERIFY(!client.idle());
    }

    void failedTaskCannotConnectEvenWithReadySnapshot()
    {
        ManagerStub stub; QVERIFY(stub.listen()); stub.software = "ready";
        stub.operationId = "failed-start"; stub.taskState = "failed"; stub.taskError = "HEALTH";
        RuntimeService service; QSignalSpy endpoint(&service, &RuntimeService::controlEndpointReady);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canStop());
        QCOMPARE(endpoint.count(), 0); QVERIFY(!service.canStart());
    }

    void runningTaskCannotConnectEvenWithReadySnapshot()
    {
        ManagerStub stub; QVERIFY(stub.listen()); stub.software = "ready";
        stub.operationId = "still-starting"; stub.taskState = "running";
        RuntimeService service; QSignalSpy endpoint(&service, &RuntimeService::controlEndpointReady);
        QSignalSpy operations(&service, &RuntimeService::operationReceived);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(!operations.isEmpty());
        QCOMPARE(endpoint.count(), 0); QVERIFY(!service.canStart()); QVERIFY(!service.canStop());
    }

    void authenticationFailureDoesNotRetry() {
        ManagerStub stub; QVERIFY(stub.listen()); stub.authFailure = true;
        RuntimeService service; QSignalSpy errors(&service, &RuntimeService::errorOccurred);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_COMPARE(errors.count(), 1);
        QTest::qWait(3200); QCOMPARE(stub.requests.size(), 1); QVERIFY(!service.canStart());
    }
    void coldStartAndStop() {
        ManagerStub stub; QVERIFY(stub.listen()); RuntimeService service;
        QSignalSpy endpoint(&service, &RuntimeService::controlEndpointReady), snapshots(&service, &RuntimeService::snapshotReceived);
        QSignalSpy logs(&service, &RuntimeService::logsReceived), operations(&service, &RuntimeService::operationReceived);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canStart());
        QCOMPARE(stub.requests[0]["method"].toString(), QString("hello"));
        QCOMPARE(stub.requests[1]["method"].toString(), QString("profiles"));
        QVERIFY(service.start("mock", "rev-A")); QVERIFY(!service.start("mock", "rev-A"));
        QTRY_COMPARE(endpoint.count(), 1); QTRY_VERIFY(service.canStop());
        QVERIFY(!service.canStart()); QCOMPARE(stub.starts, 1);
        QVERIFY(service.snapshot()["safety"].isNull()); QVERIFY(!logs.isEmpty());
        QCOMPARE(logs.last()[0].toStringList(), QStringList{"中文日志"}); QVERIFY(logs.last()[1].toBool());
        QVERIFY(service.stop()); QTRY_VERIFY(service.canStart()); QCOMPARE(stub.stops, 1);
        service.close(); QVERIFY(!service.canStart()); QVERIFY(service.snapshot().isEmpty());
    }
    void lostMutationResponse_data() {
        QTest::addColumn<bool>("stop"); QTest::newRow("start") << false; QTest::newRow("stop") << true;
    }
    void lostMutationResponse() {
        QFETCH(bool, stop);
        ManagerStub stub; QVERIFY(stub.listen()); stub.dropStart = !stop; stub.dropStop = stop;
        if (stop) { stub.software = "ready"; stub.operationId = "prior-start"; }
        RuntimeService service; QSignalSpy errors(&service, &RuntimeService::errorOccurred), operations(&service, &RuntimeService::operationReceived);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canStop());
        if (stop) QVERIFY(service.stop()); else QVERIFY(service.start("mock", "rev-A"));
        QTRY_VERIFY(!errors.isEmpty()); QVERIFY(!service.canStop());
        QTRY_VERIFY_WITH_TIMEOUT(service.canStop(), 7000);
        QCOMPARE(stop ? stub.stops : stub.starts, 1);
        QVERIFY(!operations.isEmpty());
        QCOMPARE(operations.last()[0].toJsonObject()["operation_id"].toString(), QString(stop ? "op-stop" : "op-start"));
        QTest::qWait(1200); QCOMPARE(stop ? stub.stops : stub.starts, 1);
        service.close(); const auto count = stub.requests.size(); QTest::qWait(3200); QCOMPARE(stub.requests.size(), count);
    }
    void missingRequiredModuleNeverConnects() {
        ManagerStub stub; QVERIFY(stub.listen()); stub.requiredHealthy = false; stub.software = "ready";
        RuntimeService service; QSignalSpy endpoint(&service, &RuntimeService::controlEndpointReady);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canStop());
        QCOMPARE(endpoint.count(), 0); QVERIFY(!service.canStart());
    }
    void optionalModuleDegradedCanConnect() {
        ManagerStub stub; QVERIFY(stub.listen()); stub.software = "degraded";
        RuntimeService service; QSignalSpy endpoint(&service, &RuntimeService::controlEndpointReady);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canStop());
        QCOMPARE(endpoint.count(), 1); QCOMPARE(endpoint[0][0].toString(), QString("127.0.0.1"));
        QCOMPARE(service.snapshot()["software_state"].toString(), QString("degraded")); QVERIFY(!service.canStart());
    }
    void recoveryRequiresStop() {
        ManagerStub stub; QVERIFY(stub.listen()); stub.software = "failed"; stub.recoveryRequired = true;
        stub.operationId = "unknown-start"; stub.taskState = "failed"; stub.taskError = "RECOVERY_REQUIRED";
        RuntimeService service; QSignalSpy operations(&service, &RuntimeService::operationReceived), endpoint(&service, &RuntimeService::controlEndpointReady);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canStop());
        QVERIFY(!service.canStart()); QVERIFY(!service.start("mock", "rev-A")); QCOMPARE(endpoint.count(), 0);
        QCOMPARE(operations.last()[0].toJsonObject()["error"].toObject()["code"].toString(), QString("RECOVERY_REQUIRED"));
        QVERIFY(service.stop()); QTRY_COMPARE(stub.stops, 1);
    }
    void resumeRunningOperationWithoutStart() {
        ManagerStub stub; QVERIFY(stub.listen()); stub.software = "starting"; stub.operationId = "op-existing"; stub.taskState = "running";
        RuntimeService service; QSignalSpy operations(&service, &RuntimeService::operationReceived), endpoint(&service, &RuntimeService::controlEndpointReady);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(!operations.isEmpty());
        QVERIFY(!service.canStart()); QVERIFY(!service.canStop()); QCOMPARE(endpoint.count(), 0); QCOMPARE(stub.starts, 0);
        stub.software = "ready"; stub.taskState = "succeeded";
        QTRY_VERIFY(service.canStop()); QCOMPARE(endpoint.count(), 1); QCOMPARE(stub.starts, 0);
    }
    void preflightIsReadOnly() {
        ManagerStub stub; QVERIFY(stub.listen()); RuntimeService service;
        QSignalSpy results(&service, &RuntimeService::preflightReceived);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canStart());
        QVERIFY(service.preflight("mock", "rev-A")); QTRY_COMPARE(results.count(), 1);
        QVERIFY(!results[0][0].toJsonObject()["passed"].toBool()); QCOMPARE(stub.starts, 0); QCOMPARE(stub.stops, 0);
    }
};
QTEST_GUILESS_MAIN(ManagementClientTest)
#include "management_client_test.moc"
