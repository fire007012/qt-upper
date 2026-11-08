#include "RobotStateStore.h"
#include "ControlProtocol.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

RobotStateStore::RobotStateStore(QObject *parent, std::function<qint64()> clock) : QObject(parent), m_now(std::move(clock))
{
    m_clock.start(); m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &RobotStateStore::expire); m_timer.start();
    for (const auto &topic : {"co2", "imu", "joints", "control"}) m_values.insert(topic, {});
}
qint64 RobotStateStore::now() const { return m_now ? m_now() : m_clock.elapsed(); }
void RobotStateStore::observe(const QString &topic, const QJsonValue &value, int ttlMs)
{
    if (ttlMs <= 0 || (!m_values.contains(topic) && m_values.size() >= 32)) return;
    const bool wasFresh = m_values.value(topic).fresh;
    m_values[topic] = {Communication::ControlProtocol::redact(value), now(), ttlMs, true};
    if (!wasFresh) emit freshnessChanged(topic, true);
}
void RobotStateStore::invalidate()
{
    ++m_generation;
    for (auto it = m_values.begin(); it != m_values.end(); ++it) {
        const bool fresh = it->fresh; it->fresh = false; it->at = -1; it->data = QJsonValue(QJsonValue::Null);
        if (fresh) emit freshnessChanged(it.key(), false);
    }
}
void RobotStateStore::expire()
{
    for (auto it = m_values.begin(); it != m_values.end(); ++it) {
        if (it->fresh && now() - it->at >= it->ttl) { it->fresh = false; emit freshnessChanged(it.key(), false); }
    }
}
void RobotStateStore::ingest(const QJsonObject &event)
{
    const auto type = event["type"].toString();
    if (type == "connection_state" && QStringList{"disconnected", "connecting"}.contains(event["state"].toString())) invalidate();
    if (type == "control_status") { observe("control", event["status"], 500); return; }
    if (type == "control_timing") {
        const auto rtt = event["rtt_ms"].toVariant().toLongLong();
        if (rtt >= 0) { m_rtts.append(rtt); if (m_rtts.size() > 512) m_rtts.removeFirst(); }
        return;
    }
    if (!QStringList{"connection_state","connection_recovery","control_notice","control_operation","emergency_result","protocol_error","ack_timeout","ack_disconnected"}.contains(type)) return;
    auto record = Communication::ControlProtocol::redact(event).toObject();
    record["observed_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    record["monotonic_ms"] = now(); record["connection_generation"] = static_cast<qint64>(m_generation);
    m_events.append(record); if (m_events.size() > 256) m_events.removeFirst();
}
QJsonObject RobotStateStore::snapshot() const
{
    QJsonObject values;
    for (auto it = m_values.cbegin(); it != m_values.cend(); ++it) {
        const bool fresh = it->fresh && it->at >= 0 && now() - it->at < it->ttl;
        values[it.key()] = QJsonObject{{"fresh", fresh}, {"age_ms", it->at < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(now()-it->at)},
            {"ttl_ms", it->ttl}, {"value", it->data}};
    }
    return {{"connection_generation", static_cast<qint64>(m_generation)}, {"topics", values}};
}
QJsonObject RobotStateStore::diagnosticBundle(const QJsonObject &context) const
{
    auto rtts = m_rtts; std::sort(rtts.begin(), rtts.end());
    const QJsonObject timing{{"samples", rtts.size()},
        {"p50_ms", rtts.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(rtts[(rtts.size()-1)/2])},
        {"p99_ms", rtts.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(rtts[qMin(rtts.size()-1, static_cast<decltype(rtts.size())>(rtts.size()*.99))])}};
    return {{"format_version", 1}, {"generated_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"qt_version", QT_VERSION_STR}, {"scope", "host_observations_only"}, {"physical_acceptance", false},
        {"context", Communication::ControlProtocol::redact(context)}, {"state", snapshot()}, {"freshness_rtt", timing}, {"events", m_events}};
}
bool RobotStateStore::exportFile(const QString &path, const QJsonObject &context, const QString &protectedRoot)
{
    const QFileInfo file(path);
    const auto parent = QFileInfo(file.absolutePath()).canonicalFilePath();
    const auto root = QFileInfo(protectedRoot).canonicalFilePath();
    if (parent.isEmpty() || (!root.isEmpty() && (parent == root || parent.startsWith(root + '/')))) return false;
    QSaveFile out(path); if (!out.open(QIODevice::WriteOnly)) return false;
    const auto bytes = QJsonDocument(diagnosticBundle(context)).toJson(QJsonDocument::Indented);
    return out.write(bytes) == bytes.size() && out.commit();
}
