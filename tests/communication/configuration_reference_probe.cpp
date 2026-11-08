#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include "ConfigurationService.h"

class ReferenceTest : public QObject {
    Q_OBJECT
private slots:
    void actualManagerContract() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, directory.path());
        ConfigurationService service;
        QStringList messages;
        connect(&service, &ConfigurationService::message, this, [&](const QString &s) { messages.append(s); });
        service.open("127.0.0.1", qEnvironmentVariableIntValue("HOST_CONFIG_PORT"), qEnvironmentVariable("HOST_CONFIG_TOKEN"));
        QTRY_VERIFY_WITH_TIMEOUT(service.canEdit(), 5000);
        const auto base = service.sourceRevision();
        auto save = [&](double value) {
            return service.saveCandidate({QJsonObject{{"profile_id", "mock"}, {"field", "startup_timeout"}, {"value", value}}});
        };
        QVERIFY(save(100)); QTRY_VERIFY(service.canEdit() && !service.candidate().isEmpty());
        QCOMPARE(service.sourceRevision(), base); QCOMPARE(service.source()["profile"].toObject()["startup_timeout"].toInt(), 90);
        const auto candidate = service.candidate()["candidate_revision"].toString();
        QVERIFY(service.preview(candidate)); QTRY_VERIFY(service.canEdit());
        service.setMaintenanceReady(true); QVERIFY(service.apply());
        // Harness deliberately drops the first accepted apply response.
        QTRY_VERIFY(service.status().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(service.canQuery() && !service.source().isEmpty(), 8000);
        QVERIFY(service.needsRecovery()); QVERIFY(!service.canEdit()); QCOMPARE(service.sourceRevision(), candidate);
        QVERIFY(service.recoverRequest()); QTRY_VERIFY(service.canEdit());
        QCOMPARE(service.operation()["state"].toString(), QString("succeeded"));
        QCOMPARE(service.operation()["verification"].toString(), QString("manager_source_readback"));
        QVERIFY(service.status()["pending_restart"].toBool()); QVERIFY(service.status()["actual_runtime_revision"].isNull());
        service.setMaintenanceReady(true);
        QVERIFY(save(110)); QTRY_VERIFY(service.canApply()); QVERIFY(service.apply());
        QTRY_VERIFY(service.canEdit());
        QCOMPARE(service.operation()["state"].toString(), QString("failed"));
        QCOMPARE(service.operation()["phase"].toString(), QString("rolled_back"));
        QCOMPARE(service.sourceRevision(), candidate);
        QVERIFY(service.previewRollback(base)); QTRY_VERIFY(service.canRollback()); QVERIFY(service.rollback());
        QTRY_VERIFY(service.canEdit()); QCOMPARE(service.sourceRevision(), base);
        QCOMPARE(service.source()["profile"].toObject()["startup_timeout"].toInt(), 90);
        QSignalSpy audit(&service, &ConfigurationService::auditReceived);
        QVERIFY(service.audit(true)); QTRY_COMPARE(audit.size(), 1); QVERIFY(!audit[0][0].toJsonArray().isEmpty());
        for (const auto &s : messages) QVERIFY2(!s.contains(QStringLiteral("响应字段无效")), qPrintable(s));
        service.close();
    }
};
QTEST_GUILESS_MAIN(ReferenceTest)
#include "configuration_reference_probe.moc"
