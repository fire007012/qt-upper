#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include "../../src/management/RuntimeJournal.h"

class RuntimeJournalTest : public QObject {
    Q_OBJECT
private slots:
    void recordsSurviveReopenAndSecretsAreRemoved() {
        QTemporaryDir folder; QVERIFY(folder.isValid());
        const QString token(40,'z');
        { RuntimeJournal journal(folder.path());
          QVERIFY(journal.append("error",QJsonObject{{"token",token},{"message","failed " + token}}, {token}));
          QVERIFY(journal.snapshot({{"session_id","secret-session"},{"state","failed"}}, {token})); }
        RuntimeJournal reopened(folder.path()); QVERIFY(reopened.append("recovered", "ready"));
        QFile file(folder.filePath("runtime.jsonl")); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes=file.readAll();
        QVERIFY(bytes.contains("failed")); QVERIFY(bytes.contains("recovered")); QVERIFY(!bytes.contains(token.toUtf8()));
        QCOMPARE(bytes.count('\n'),2);
        QFile snapshot(folder.filePath("latest.json")); QVERIFY(snapshot.open(QIODevice::ReadOnly));
        const auto state=QJsonDocument::fromJson(snapshot.readAll()).object();
        QCOMPARE(state["state"].toString(),QString("failed")); QVERIFY(state["session_id"]!=QJsonValue("secret-session"));
    }
    void semanticDedupAndRotationAreBounded() {
        QTemporaryDir folder; RuntimeJournal journal(folder.path(),300);
        QVERIFY(journal.changed("state", "ready")); QVERIFY(journal.changed("state", "ready"));
        QFile first(folder.filePath("runtime.jsonl")); QVERIFY(first.open(QIODevice::ReadOnly)); QCOMPARE(first.readAll().count('\n'),1); first.close();
        for(int i=0;i<30;++i) QVERIFY(journal.append("event",QString(120,'x')+QString::number(i)));
        const auto files=QDir(folder.path()).entryList({"*.jsonl"},QDir::Files);
        QCOMPARE(files.size(),5);
        QFile latest(folder.filePath("runtime.jsonl")); QVERIFY(latest.open(QIODevice::ReadOnly)); QVERIFY(latest.readAll().contains("29"));
    }
    void unavailableStorageIsReported() {
        QTemporaryDir folder;
        QFile blocker(folder.filePath("not-a-directory")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        RuntimeJournal journal(blocker.fileName()); QVERIFY(!journal.healthy());
        QVERIFY(!journal.append("error","must not silently succeed")); QVERIFY(!journal.error().isEmpty());
    }
};
QTEST_GUILESS_MAIN(RuntimeJournalTest)
#include "runtime_journal_test.moc"
