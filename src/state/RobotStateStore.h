#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <functional>

class RobotStateStore : public QObject {
    Q_OBJECT
public:
    explicit RobotStateStore(QObject *parent = nullptr, std::function<qint64()> clock = {});
    void observe(const QString &topic, const QJsonValue &value, int ttlMs);
    void invalidate();
    void ingest(const QJsonObject &event);
    void expire();
    QJsonObject snapshot() const;
    QJsonObject diagnosticBundle(const QJsonObject &context = {}) const;
    bool exportFile(const QString &path, const QJsonObject &context = {}, const QString &protectedRoot = {});
signals:
    void freshnessChanged(const QString &topic, bool fresh);
private:
    struct Value { QJsonValue data; qint64 at = -1; int ttl = 0; bool fresh = false; };
    qint64 now() const;
    QElapsedTimer m_clock;
    std::function<qint64()> m_now;
    QTimer m_timer;
    QHash<QString, Value> m_values;
    QJsonArray m_events;
    QList<qint64> m_rtts;
    quint64 m_generation = 0;
};
