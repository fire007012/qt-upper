#include "RuntimeJournal.h"
#include "../communication/ControlProtocol.h"
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

RuntimeJournal::RuntimeJournal(const QString &directory, qint64 maxBytes) : m_maxBytes(maxBytes)
{
    m_directory = directory.isEmpty() ? qEnvironmentVariable("HOSTCOMPUTER_RUNTIME_LOG_DIR") : directory;
    if (m_directory.isEmpty())
        m_directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs/runtime";
    m_directory = QDir(m_directory).absolutePath();
    open();
}
bool RuntimeJournal::open()
{
    if (!QDir().mkpath(m_directory)) { m_error = QStringLiteral("无法创建运行日志目录"); return false; }
    m_file.setFileName(m_directory + "/runtime.jsonl");
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) { m_error = m_file.errorString(); return false; }
    m_file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    m_error.clear(); return true;
}
QJsonValue RuntimeJournal::clean(const QJsonValue &value, const QStringList &secrets) const
{
    auto data = Communication::ControlProtocol::redact(value);
    if (data.isObject()) {
        auto object = data.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) it.value() = clean(it.value(), secrets);
        return object;
    }
    if (data.isArray()) {
        QJsonArray array; for (const auto &item : data.toArray()) array.append(clean(item, secrets)); return array;
    }
    if (data.isString()) {
        auto text = data.toString();
        for (const auto &secret : secrets) if (!secret.isEmpty()) text.replace(secret, QStringLiteral("[已隐藏]"));
        return text;
    }
    return data;
}
bool RuntimeJournal::rotate()
{
    m_file.close();
    const QString oldest = m_directory + "/runtime.4.jsonl";
    if (QFile::exists(oldest) && !QFile::remove(oldest)) { m_error = QStringLiteral("无法轮转运行日志"); return false; }
    for (int i = 3; i >= 0; --i) {
        const QString from = m_directory + (i ? QString("/runtime.%1.jsonl").arg(i) : "/runtime.jsonl");
        const QString to = m_directory + QString("/runtime.%1.jsonl").arg(i + 1);
        if (QFile::exists(from) && !QFile::rename(from, to)) { m_error = QStringLiteral("无法轮转运行日志"); return false; }
    }
    return open();
}
bool RuntimeJournal::append(const QString &event, const QJsonValue &data, const QStringList &secrets)
{
    if (!m_file.isOpen() && !open()) return false;
    const auto bytes = QJsonDocument(QJsonObject{
        {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"event", event}, {"data", clean(data, secrets)}}).toJson(QJsonDocument::Compact) + '\n';
    if (m_file.size() && m_file.size() + bytes.size() > m_maxBytes && !rotate()) return false;
    if (m_file.write(bytes) != bytes.size() || !m_file.flush()) { m_error = m_file.errorString(); return false; }
    m_error.clear(); return true;
}
bool RuntimeJournal::changed(const QString &event, const QJsonValue &data, const QStringList &secrets)
{
    const auto bytes = QJsonDocument(QJsonObject{{"data", clean(data, secrets)}}).toJson(QJsonDocument::Compact);
    if (m_previous.value(event) == bytes && healthy()) return true;
    if (!append(event, data, secrets)) return false;
    m_previous[event] = bytes; return true;
}
bool RuntimeJournal::snapshot(const QJsonObject &data, const QStringList &secrets)
{
    auto object = clean(data, secrets).toObject();
    object["recorded_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSaveFile file(m_directory + "/latest.json");
    if (!file.open(QIODevice::WriteOnly)) { m_error = file.errorString(); return false; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) { m_error = file.errorString(); return false; }
    return true;
}
