#include "TcpConnectionDialog.h"
#include "controller.h"

#include <QAbstractSocket>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

struct EthernetInfo {
    QString adapterName;
    QString currentIp;
    bool found = false;
};

void detectEthernetAdapter(EthernetInfo &info)
{
    info.found = false;
    info.adapterName.clear();
    info.currentIp.clear();

    const QStringList excludeKeywords = {
        "vmware", "virtual", "bluetooth", "loopback",
        "vethernet", "hyper-v", "vpn", "tunnel",
        "wsl", "docker", "vbox"
    };

    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        if (iface.type() != QNetworkInterface::Ethernet)
            continue;
        if (!(iface.flags() & QNetworkInterface::IsUp))
            continue;
        if (iface.flags() & QNetworkInterface::IsLoopBack)
            continue;

        const QString nameLower = iface.humanReadableName().toLower();
        bool isVirtual = false;
        for (const QString &kw : excludeKeywords) {
            if (nameLower.contains(kw)) {
                isVirtual = true;
                break;
            }
        }
        if (isVirtual) continue;

        info.found = true;
        info.adapterName = iface.humanReadableName();
        const auto entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol
                && !entry.ip().isLoopback()) {
                info.currentIp = entry.ip().toString();
                break;
            }
        }
        break;
    }
}

QString calcLocalIp(const QString &nucIp)
{
    const QStringList parts = nucIp.split('.');
    if (parts.size() != 4) return QString();
    const int lastOctet = parts[3].toInt();
    for (int candidate : {200, 201, 202, 210, 220, 250}) {
        if (candidate != lastOctet) {
            return QString("%1.%2.%3.%4").arg(parts[0], parts[1], parts[2]).arg(candidate);
        }
    }
    return QString();
}

QString getSubnetPrefix(const QString &ip)
{
    const QStringList parts = ip.split('.');
    if (parts.size() != 4) return QString();
    return QString("%1.%2.%3.").arg(parts[0], parts[1], parts[2]);
}

} // namespace

void TcpConnectionDialog::showDialog(QWidget *parent,
                                     Controller *controller,
                                     const std::function<void(const QString &)> &onCommand,
                                     const std::function<void(const QString &)> &onError)
{
    if (controller && controller->isTcpConnected()) {
        const auto reply = QMessageBox::question(parent, QStringLiteral("TCP连接"),
            QStringLiteral("当前已连接到 %1:%2\n是否断开并重新连接？")
                .arg(controller->getROSHost()).arg(controller->getROSPort()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply == QMessageBox::No) {
            return;
        }
        controller->disconnectFromROS();
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("TCP连接到ROS节点"));
    dialog.resize(450, 300);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("请输入ROS节点(Intel NUC)的IP地址和端口:"), &dialog));

    QSettings settings(QStringLiteral("HostComputer"), QStringLiteral("TCPConnection"));
    const QString lastHost = settings.value(QStringLiteral("tcp/host"), QStringLiteral("192.168.1.123")).toString();
    const int lastPort = settings.value(QStringLiteral("tcp/port"), 9090).toInt();

    auto *ipLayout = new QHBoxLayout();
    ipLayout->addWidget(new QLabel(QStringLiteral("IP地址:")));
    auto *ipEdit = new QLineEdit(lastHost);
    ipEdit->setMinimumWidth(200);
    ipEdit->setPlaceholderText(QStringLiteral("下位机IP地址"));
    ipLayout->addWidget(ipEdit);
    layout->addLayout(ipLayout);

    auto *portLayout = new QHBoxLayout();
    portLayout->addWidget(new QLabel(QStringLiteral("端口:")));
    auto *portSpin = new QSpinBox();
    portSpin->setRange(1, 65535);
    portSpin->setValue(lastPort);
    portSpin->setMinimumWidth(200);
    portLayout->addWidget(portSpin);
    layout->addLayout(portLayout);

    auto *robotEdit = new QLineEdit(settings.value(QStringLiteral("tcp/robot_id"), QStringLiteral("robot24")).toString(), &dialog);
    robotEdit->setPlaceholderText(QStringLiteral("须与下位机robot_id精确一致"));
    layout->addWidget(new QLabel(QStringLiteral("机器人身份（控制v2）"), &dialog));
    layout->addWidget(robotEdit);

    auto *separator = new QFrame();
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

    auto *netConfigTitle = new QLabel(QStringLiteral("网络配置"));
    netConfigTitle->setStyleSheet(QStringLiteral("font-weight: bold; color: #555;"));
    layout->addWidget(netConfigTitle);

    auto *networkStatusLabel = new QLabel(QStringLiteral("正在检测网卡..."));
    networkStatusLabel->setWordWrap(true);
    layout->addWidget(networkStatusLabel);

    auto *autoConfigBtn = new QPushButton(QStringLiteral("自动配置网络"));
    autoConfigBtn->setToolTip(QStringLiteral("根据目标NUC的IP，自动为本机网卡分配同网段的不冲突IP"));
    autoConfigBtn->setEnabled(false);
    layout->addWidget(autoConfigBtn);

    auto refreshNetworkStatus = [&](const EthernetInfo &info) {
        if (!info.found) {
            networkStatusLabel->setText(QStringLiteral("未检测到有线网卡"));
            networkStatusLabel->setStyleSheet(QStringLiteral("color: red;"));
            autoConfigBtn->setEnabled(false);
            return;
        }

        const QString nucIp = ipEdit->text().trimmed();
        const QString subnetPrefix = getSubnetPrefix(nucIp);
        const bool isReady = !subnetPrefix.isEmpty()
                             && info.currentIp.startsWith(subnetPrefix)
                             && info.currentIp != nucIp;

        QString statusText = QStringLiteral("有线网卡: %1").arg(info.adapterName);
        if (info.currentIp.isEmpty()) {
            statusText += QStringLiteral(" (未分配IP)");
        } else {
            statusText += QStringLiteral(" (%1)").arg(info.currentIp);
        }

        if (isReady) {
            statusText += QStringLiteral("  ✓ 已就绪");
            networkStatusLabel->setStyleSheet(QStringLiteral("color: green;"));
            autoConfigBtn->setEnabled(false);
            autoConfigBtn->setText(QStringLiteral("网络已就绪"));
        } else {
            statusText += QStringLiteral("  ✗ 需要配置");
            networkStatusLabel->setStyleSheet(QStringLiteral("color: #CC6600;"));
            autoConfigBtn->setEnabled(true);
            autoConfigBtn->setText(QStringLiteral("自动配置网络"));
        }
        networkStatusLabel->setText(statusText);
    };

    EthernetInfo ethInfo;
    detectEthernetAdapter(ethInfo);
    refreshNetworkStatus(ethInfo);

    QObject::connect(ipEdit, &QLineEdit::textChanged, &dialog, [&]() {
        refreshNetworkStatus(ethInfo);
    });

    QObject::connect(autoConfigBtn, &QPushButton::clicked, &dialog, [&]() {
        const QString nucIp = ipEdit->text().trimmed();
        const QString localIp = calcLocalIp(nucIp);
        if (localIp.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("配置失败"),
                QStringLiteral("请先在上方填入有效的NUC IP地址（如 192.168.1.50）"));
            return;
        }

        const QString subnetMask = QStringLiteral("255.255.255.0");
        const auto confirmReply = QMessageBox::question(&dialog, QStringLiteral("确认网络配置"),
            QStringLiteral("将对网卡 \"%1\" 执行以下配置:\n\n"
                           "  本机IP: %2\n"
                           "  子网掩码: %3\n"
                           "  目标NUC: %4\n\n"
                           "此操作需要管理员权限（UAC弹窗），是否继续？")
                .arg(ethInfo.adapterName, localIp, subnetMask, nucIp),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (confirmReply != QMessageBox::Yes) {
            return;
        }

#ifdef Q_OS_WIN
        const QString netshArgs = QStringLiteral("interface ip set address name=\"%1\" static %2 %3")
                                      .arg(ethInfo.adapterName, localIp, subnetMask);
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = reinterpret_cast<HWND>(dialog.winId());
        sei.lpVerb = L"runas";
        sei.lpFile = L"netsh";
        sei.lpParameters = reinterpret_cast<LPCWSTR>(netshArgs.utf16());
        sei.nShow = SW_HIDE;

        autoConfigBtn->setEnabled(false);
        autoConfigBtn->setText(QStringLiteral("正在配置..."));
        networkStatusLabel->setText(QStringLiteral("等待UAC授权..."));
        networkStatusLabel->setStyleSheet(QStringLiteral("color: #0066CC;"));
        QApplication::processEvents();

        if (ShellExecuteExW(&sei)) {
            if (sei.hProcess) {
                WaitForSingleObject(sei.hProcess, 10000);
                CloseHandle(sei.hProcess);
            }
            QThread::msleep(1500);
            QApplication::processEvents();

            detectEthernetAdapter(ethInfo);
            refreshNetworkStatus(ethInfo);

            if (ethInfo.currentIp == localIp) {
                QMessageBox::information(&dialog, QStringLiteral("配置成功"),
                    QStringLiteral("网卡 \"%1\" 已成功配置为:\nIP: %2\n子网掩码: %3")
                        .arg(ethInfo.adapterName, localIp, subnetMask));
                if (onCommand) onCommand(QStringLiteral("[网络] 有线网卡已配置为 %1").arg(localIp));
            } else {
                QMessageBox::warning(&dialog, QStringLiteral("配置结果"),
                    QStringLiteral("netsh命令已执行，当前检测到IP: %1\n如果IP未生效，请稍等几秒后重新打开此对话框。")
                        .arg(ethInfo.currentIp.isEmpty() ? QStringLiteral("未分配") : ethInfo.currentIp));
            }
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_CANCELLED) {
                networkStatusLabel->setText(QStringLiteral("有线网卡: %1 (%2)  ✗ UAC已取消")
                    .arg(ethInfo.adapterName, ethInfo.currentIp.isEmpty() ? QStringLiteral("未分配IP") : ethInfo.currentIp));
                networkStatusLabel->setStyleSheet(QStringLiteral("color: #CC6600;"));
            } else {
                networkStatusLabel->setText(QStringLiteral("配置失败 (错误码: %1)").arg(err));
                networkStatusLabel->setStyleSheet(QStringLiteral("color: red;"));
            }
            autoConfigBtn->setEnabled(true);
            autoConfigBtn->setText(QStringLiteral("自动配置网络"));
        }
#else
        QMessageBox::information(&dialog, QStringLiteral("提示"), QStringLiteral("此功能仅支持Windows系统"));
#endif
    });

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttonBox->button(QDialogButtonBox::Ok)->setText(QStringLiteral("连接"));
    buttonBox->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttonBox);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        const QString host = ipEdit->text().trimmed();
        const quint16 port = static_cast<quint16>(portSpin->value());
        if (host.isEmpty()) {
            if (onError) onError(QStringLiteral("[TCP] IP地址不能为空"));
            return;
        }

        if (onCommand) onCommand(QStringLiteral("[TCP] 正在连接到 %1:%2 ...").arg(host).arg(port));
        const QString robotId = robotEdit->text().trimmed();
        const bool started = controller ? controller->connectToROS(host, port, robotId) : false;
        if (started) {
            settings.setValue(QStringLiteral("tcp/host"), host);
            settings.setValue(QStringLiteral("tcp/port"), port);
            settings.setValue(QStringLiteral("tcp/robot_id"), robotId);
        } else {
            if (onError) onError(QStringLiteral("[TCP] 发起连接失败: %1:%2").arg(host).arg(port));
        }
    }
}
