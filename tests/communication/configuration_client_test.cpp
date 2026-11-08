#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include "ConfigurationService.h"
#include "configuration_stub.h"

class ConfigurationTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QJsonArray patches(double value = 100) {
        return {QJsonObject{{"profile_id", "mock"}, {"field", "startup_timeout"}, {"value", value}}};
    }
private slots:
    void initTestCase() {
        QVERIFY(directory.isValid()); QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, directory.path());
    }
    void init() { QSettings("HostComputer", "Configuration").clear(); }
    void saveApplyAndStatusAreSeparate() {
        ConfigurationStub stub; QVERIFY(stub.listen()); ConfigurationService service;
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canEdit());
        const auto base = service.sourceRevision();
        QVERIFY(!service.saveCandidate(patches(181)));
        QVERIFY(!service.saveCandidate({QJsonObject{{"profile_id", "mock"}, {"field", "video_required"}, {"value", true}}}));
        QVERIFY(service.saveCandidate(patches())); QTRY_VERIFY(!service.candidate().isEmpty());
        QCOMPARE(service.sourceRevision(), base); QCOMPARE(stub.applies, 0);
        QVERIFY(!service.canApply()); service.setMaintenanceReady(true); QVERIFY(service.canApply());
        QVERIFY(service.apply()); QVERIFY(service.blocksStart()); QTRY_VERIFY(service.canEdit());
        QCOMPARE(service.sourceRevision(), stub.target); QCOMPARE(service.operation()["state"].toString(), QString("succeeded"));
        QVERIFY(service.status()["actual_runtime_revision"].isNull()); QVERIFY(service.status()["pending_restart"].toBool());
        QSignalSpy pages(&service, &ConfigurationService::auditReceived);
        QVERIFY(service.audit(true)); QTRY_COMPARE(pages.size(), 1); QVERIFY(service.audit()); QTRY_COMPARE(pages.size(), 2);
        QCOMPARE(stub.starts, 0); QCOMPARE(stub.stops, 0);
    }
    void revisionConflictInvalidatesConfirmation() {
        ConfigurationStub stub; QVERIFY(stub.listen()); ConfigurationService service;
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canEdit()); service.setMaintenanceReady(true);
        QVERIFY(service.saveCandidate(patches())); QTRY_VERIFY(service.canApply());
        stub.base = QString(64, 'c'); // Another client wins before this client's poll.
        QVERIFY(service.apply()); QTRY_VERIFY(service.canEdit());
        QVERIFY(service.candidate().isEmpty()); QVERIFY(!service.canApply()); QCOMPARE(stub.applies, 0);
        QCOMPARE(service.sourceRevision(), stub.base);
    }
    void failedAndUnknownOperationsDoNotClaimSuccess() {
        ConfigurationStub stub; stub.failApply = true; QVERIFY(stub.listen()); ConfigurationService service;
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canEdit()); service.setMaintenanceReady(true);
        const auto base = service.sourceRevision();
        QVERIFY(service.saveCandidate(patches())); QTRY_VERIFY(service.canApply()); QVERIFY(service.apply()); QTRY_VERIFY(service.canEdit());
        QCOMPARE(service.operation()["state"].toString(), QString("failed")); QCOMPARE(service.sourceRevision(), base);
        stub.configOperation["state"] = "result_unknown"; stub.configOperation["phase"] = "reconcile"; stub.configOperation["error"] = "MANAGER_RESTART";
        QTRY_COMPARE(service.operation()["state"].toString(), QString("result_unknown"));
        QVERIFY(service.blocksStart()); QVERIFY(service.canEdit()); // Explicit reconciliation remains available.
        QCOMPARE(service.sourceRevision(), base); QCOMPARE(stub.starts, 0);
    }
    void lostResponsePersistsExactRequestAcrossClientRestart() {
        ConfigurationStub stub; stub.dropApply = true; QVERIFY(stub.listen());
        {
            ConfigurationService service;
            service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canEdit()); service.setMaintenanceReady(true);
            QVERIFY(service.saveCandidate(patches())); QTRY_VERIFY(service.canApply()); QVERIFY(service.apply());
            QTRY_VERIFY(service.status().isEmpty()); QVERIFY(service.needsRecovery());
        }
        ConfigurationService restored; restored.open("127.0.0.1", stub.port(), stub.token());
        QTRY_VERIFY(restored.canQuery() && !restored.source().isEmpty());
        QVERIFY(restored.needsRecovery()); QVERIFY(!restored.canEdit()); QCOMPARE(stub.applies, 1); QCOMPARE(stub.applyIds.size(), 1);
        QVERIFY(restored.recoverRequest()); QTRY_VERIFY(restored.canEdit());
        QCOMPARE(stub.applies, 1); QCOMPARE(stub.applyIds.size(), 2); QCOMPARE(stub.applyIds[0], stub.applyIds[1]);
        QVERIFY(stub.applyIds[0].startsWith("config-"));
        QSettings settings("HostComputer", "Configuration"); QFile file(settings.fileName()); QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains(stub.token().toUtf8()));
    }
    void oversizedResponseKeepsIdAndRollbackIsExplicit() {
        ConfigurationStub stub; stub.oversizedApply = true; QVERIFY(stub.listen()); ConfigurationService service;
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canEdit()); service.setMaintenanceReady(true);
        const auto base = service.sourceRevision(); QVERIFY(service.saveCandidate(patches())); QTRY_VERIFY(service.canApply());
        QVERIFY(service.apply()); QTRY_VERIFY(service.canQuery()); QVERIFY(service.needsRecovery()); QVERIFY(!service.canEdit());
        QTRY_COMPARE(service.sourceRevision(), stub.target); QVERIFY(service.recoverRequest()); QTRY_VERIFY(service.canEdit());
        QVERIFY(service.previewRollback(base)); QTRY_VERIFY(service.canRollback());
        QVERIFY(service.rollback()); QTRY_VERIFY(service.canEdit()); QCOMPARE(service.sourceRevision(), base); QCOMPARE(stub.applies, 2);
    }
    void malformedStatusClosesWithoutMutation() {
        ConfigurationStub stub; stub.malformedStatus = true; QVERIFY(stub.listen()); ConfigurationService service;
        QSignalSpy messages(&service, &ConfigurationService::message);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(!messages.isEmpty());
        QVERIFY(!service.canEdit()); QVERIFY(service.status().isEmpty()); QCOMPARE(stub.applies, 0);
    }
    void capabilityDisabledLeavesLegacyManagementUsable() {
        ManagerStub stub; QVERIFY(stub.listen()); ConfigurationService service;
        QSignalSpy messages(&service, &ConfigurationService::message);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(!messages.isEmpty()); QVERIFY(!service.canQuery());
        for (const auto &request : stub.requests) QVERIFY(!request["method"].toString().startsWith("config."));
    }
    void oversizedParamsNeverReachServer() {
        ManagerStub stub; QVERIFY(stub.listen()); Communication::ManagementClient client;
        QSignalSpy connected(&client, &Communication::ManagementClient::connected);
        client.open("127.0.0.1", stub.port(), stub.token()); QTRY_COMPARE(connected.size(), 1);
        QVERIFY(!client.request("config.candidate", {{"oversized", QString(49152, 'x')}}));
        QVERIFY(client.idle()); QCOMPARE(stub.requests.size(), 0);
    }
    void failedRecoveryJournalPreventsWrite() {
        QTemporaryDir blocked; QVERIFY(blocked.isValid());
        const auto path = blocked.filePath("not-a-directory"); QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, path);
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, path);
        ConfigurationStub stub; QVERIFY(stub.listen()); ConfigurationService service;
        QSignalSpy messages(&service, &ConfigurationService::message);
        service.open("127.0.0.1", stub.port(), stub.token()); QTRY_VERIFY(service.canEdit());
        QVERIFY(!service.saveCandidate(patches())); QCOMPARE(stub.saves, 0);
        QVERIFY(!service.needsRecovery()); QVERIFY(!messages.isEmpty());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, directory.path());
    }
};
QTEST_GUILESS_MAIN(ConfigurationTest)
#include "configuration_client_test.moc"
