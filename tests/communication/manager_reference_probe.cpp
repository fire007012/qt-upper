#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include "RuntimeService.h"

// Only used by manager_reference_integration.py, against its ephemeral mock backend.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    RuntimeService service;
    int phase = 0, cycles = 0, endpoints = 0;
    const int requestedCycles = qEnvironmentVariableIsSet("HOST_MANAGER_PROBE_CYCLES")
        ? qEnvironmentVariableIntValue("HOST_MANAGER_PROBE_CYCLES") : 2;
    if (requestedCycles < 1 || requestedCycles > 1000) return 6;
    const int expectedEndpoints = qEnvironmentVariableIsSet("HOST_MANAGER_PROBE_EXPECTED_ENDPOINTS")
        ? qEnvironmentVariableIntValue("HOST_MANAGER_PROBE_EXPECTED_ENDPOINTS") : 2;
    bool safetyUnknown = true;
    QObject::connect(&service, &RuntimeService::errorOccurred, &app, [&](const QString &message) {
        QTextStream(stderr) << message << '\n'; app.exit(1);
    });
    QObject::connect(&service, &RuntimeService::controlEndpointReady, &app,
                     [&](const QString &, quint16) { ++endpoints; });
    QObject::connect(&service, &RuntimeService::snapshotReceived, &app, [&](const QJsonObject &snapshot) {
        safetyUnknown = safetyUnknown && snapshot["safety"].isNull();
    });
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        if (phase == 0 && service.canStart()) {
            phase = 1;
            if (!service.start("mock", "revision-A")) app.exit(2);
        } else if (phase == 1 && service.canStop() && service.snapshot()["software_state"] == QJsonValue("ready")) {
            phase = 2;
            if (!service.stop()) app.exit(3);
        } else if (phase == 2 && service.canStart()) {
            ++cycles;
            if (cycles < requestedCycles) { phase = 0; return; }
            const QJsonObject report{{"cycles", cycles}, {"endpoints", endpoints}, {"expected_endpoints", expectedEndpoints}, {"unknown_safety_preserved", safetyUnknown}};
            QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
            service.close(); app.exit(endpoints == expectedEndpoints && safetyUnknown ? 0 : 4);
        }
    });
    tick.start(25);
    QTimer::singleShot(qMax(30000,requestedCycles*10000), &app, [&] { app.exit(5); });
    service.open("127.0.0.1", qEnvironmentVariableIntValue("HOST_MANAGER_PROBE_PORT"), qEnvironmentVariable("HOST_MANAGER_PROBE_TOKEN"));
    return app.exec();
}
