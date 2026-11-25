#ifndef TCP_CONNECTION_DIALOG_H
#define TCP_CONNECTION_DIALOG_H

#include <functional>

class QString;
class QWidget;
class Controller;

/**
 * @brief 手动 TCP 连接与网卡同网段辅助配置对话框
 */
class TcpConnectionDialog
{
public:
    static void showDialog(QWidget *parent,
                           Controller *controller,
                           const std::function<void(const QString &)> &onCommand,
                           const std::function<void(const QString &)> &onError);
};

#endif // TCP_CONNECTION_DIALOG_H
