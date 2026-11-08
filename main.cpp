#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QTextStream>
#include "src/communication/ControlProtocol.h"

int main(int argc, char *argv[])
{
    if (argc == 2 && QByteArray(argv[1]) == "--version") {
        QTextStream(stdout) << Communication::ControlProtocol::HostBuildVersion << " Qt " << QT_VERSION_STR << Qt::endl;
        return 0;
    }
    QApplication a(argc, argv);
    a.setApplicationVersion(QString::fromUtf8(Communication::ControlProtocol::HostBuildVersion));
    a.setWindowIcon(QIcon(":/app_icon.png"));
    MainWindow w;
    w.show();

    return a.exec();
}
