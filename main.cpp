#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QTextStream>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QEventLoop>
#include <QTimer>
#include <memory>
#include "src/communication/ControlProtocol.h"

int main(int argc, char *argv[])
{
    if (argc == 2 && QByteArray(argv[1]) == "--version") {
        QTextStream(stdout) << Communication::ControlProtocol::HostBuildVersion << " Qt " << QT_VERSION_STR << Qt::endl;
        return 0;
    }
    QApplication a(argc, argv);
    a.setApplicationVersion(QString::fromUtf8(Communication::ControlProtocol::HostBuildVersion));
    if (a.arguments().contains("--check-ui-runtime") || a.arguments().contains("--check-render-runtime")) {
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl("qrc:/resources/qml/RobotView.qml"));
        std::unique_ptr<QObject> object(component.create());
        QJsonArray errors;
        for (const auto &error : component.errors()) errors.append(error.toString());
        bool passed = object && errors.isEmpty();
        const bool renderRequested = a.arguments().contains("--check-render-runtime");
        bool renderingValidated = false;
        int graphicsApi = -1, renderedFrames = 0;
        if (passed && renderRequested) {
            QQuickWindow window;
            window.setTitle(QStringLiteral("上位机 Linux 渲染自检")); window.resize(640,480);
            auto *item = qobject_cast<QQuickItem *>(object.get());
            if (item) {
                item->setWidth(640); item->setHeight(480);
                item->setParentItem(window.contentItem()); item->setParent(&window); object.release();
                QEventLoop loop; QTimer deadline; deadline.setSingleShot(true);
                QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
                QObject::connect(&window,&QQuickWindow::frameSwapped,&loop,[&] {
                    if (++renderedFrames >= 2) loop.quit(); else window.update();
                });
                deadline.start(5000); window.show(); loop.exec();
                const auto image = window.grabWindow();
                graphicsApi = static_cast<int>(window.rendererInterface()->graphicsApi());
                renderingValidated = renderedFrames >= 2 && !image.isNull()
                    && window.rendererInterface()->graphicsApi() != QSGRendererInterface::Software;
            }
            passed = renderingValidated;
        }
        const QJsonObject result{{"passed", passed}, {"platform_plugin", a.platformName()},
            {"qt_version", QT_VERSION_STR}, {"qml_errors", errors},
            {"scope", renderRequested ? "local_qml_render_and_readback" : "platform_plugin_and_qml_creation"},
            {"rendering_validated", renderingValidated}, {"graphics_api", graphicsApi}, {"rendered_frames", renderedFrames},
            {"physical_acceptance", false}};
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
        return passed ? 0 : 1;
    }
    a.setWindowIcon(QIcon(":/app_icon.png"));
    const bool smoke = a.arguments().contains("--smoke-ui-runtime");
    int result;
    {
        MainWindow w;
        w.show();
        if (smoke) QTimer::singleShot(3000,&a,&QCoreApplication::quit);
        result = a.exec();
    } // Exercise window/controller/worker destruction before reporting success.
    if (smoke) {
        const QJsonObject report{{"passed", result == 0}, {"scope", "full_window_lifecycle_smoke"},
            {"platform_plugin", a.platformName()}, {"physical_acceptance", false}};
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << Qt::endl;
    }
    return result;
}
