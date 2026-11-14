#pragma once
#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QStringList>

// Local operator history, independent of the on-screen presentation.
class RuntimeJournal {
public:
    explicit RuntimeJournal(const QString &directory = {}, qint64 maxBytes = 5 * 1024 * 1024);
    bool append(const QString &event, const QJsonValue &data, const QStringList &secrets = {});
    bool changed(const QString &event, const QJsonValue &data, const QStringList &secrets = {});
    bool snapshot(const QJsonObject &data, const QStringList &secrets = {});
    QString directory() const { return m_directory; }
    bool healthy() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }
private:
    QJsonValue clean(const QJsonValue &data, const QStringList &secrets) const;
    bool open();
    bool rotate();
    QString m_directory, m_error;
    QFile m_file;
    qint64 m_maxBytes;
    QHash<QString, QByteArray> m_previous;
};
