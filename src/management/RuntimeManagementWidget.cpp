#include "RuntimeManagementWidget.h"
#include "ConfigurationWidget.h"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTextDocument>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
QString safetyValue(const QJsonValue &value)
{
    return value.isBool() ? (value.toBool() ? QStringLiteral("是") : QStringLiteral("否")) : QStringLiteral("未知");
}
QString softwareName(const QString &state)
{
    if (state == "stopped") return QStringLiteral("未启动");
    if (state == "starting") return QStringLiteral("启动中");
    if (state == "ready") return QStringLiteral("软件已就绪，待授权");
    if (state == "degraded") return QStringLiteral("软件降级（查看模块详情）");
    if (state == "stopping") return QStringLiteral("停止中");
    return QStringLiteral("失败/需恢复");
}
}
RuntimeManagementWidget::RuntimeManagementWidget(QWidget *parent) : QWidget(parent), m_service(this)
{
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    QSettings settings("HostComputer", "Management");
    m_host = new QLineEdit(settings.value("host", "192.168.1.100").toString(), this);
    m_port = new QSpinBox(this); m_port->setRange(1, 65535); m_port->setValue(settings.value("port", 9091).toInt());
    m_token = new QLineEdit(this); m_token->setEchoMode(QLineEdit::Password);
    m_token->setPlaceholderText(QStringLiteral("部署令牌（仅在内存保留）"));
    form->addRow(QStringLiteral("下位机地址"), m_host);
    form->addRow(QStringLiteral("管理端口"), m_port);
    form->addRow(QStringLiteral("认证令牌"), m_token);
    layout->addLayout(form);
    auto *connectionRow = new QHBoxLayout;
    auto *connectButton = new QPushButton(QStringLiteral("连接管理服务"), this);
    auto *disconnectButton = new QPushButton(QStringLiteral("断开管理"), this);
    connectionRow->addWidget(connectButton); connectionRow->addWidget(disconnectButton);
    layout->addLayout(connectionRow);
    m_connection = new QLabel(QStringLiteral("管理未连接"), this); m_connection->setWordWrap(true);
    layout->addWidget(m_connection);
    auto *tabs = new QTabWidget(this);
    auto *runtimePage = new QWidget(tabs);
    m_configuration = new ConfigurationWidget(tabs);
    tabs->addTab(runtimePage, QStringLiteral("运行管理"));
    tabs->addTab(m_configuration, QStringLiteral("配置维护"));
    layout->addWidget(tabs);
    layout = new QVBoxLayout(runtimePage);
    m_profiles = new QComboBox(this); layout->addWidget(m_profiles);
    auto *buttons = new QHBoxLayout;
    m_preflight = new QPushButton(QStringLiteral("预检"), this);
    m_start = new QPushButton(QStringLiteral("启动系统"), this);
    m_stop = new QPushButton(QStringLiteral("停止系统"), this);
    buttons->addWidget(m_preflight); buttons->addWidget(m_start); buttons->addWidget(m_stop);
    layout->addLayout(buttons);
    m_state = new QLabel(QStringLiteral("软件状态：未知"), this); m_state->setWordWrap(true);
    m_operation = new QLabel(QStringLiteral("操作：未知"), this); m_operation->setWordWrap(true);
    m_safety = new QLabel(QStringLiteral("硬件初始化/使能/急停：未知；运动授权以控制链路为准"), this);
    m_safety->setWordWrap(true);
    layout->addWidget(m_state); layout->addWidget(m_operation); layout->addWidget(m_safety);
    auto *notice = new QLabel(QStringLiteral("启动完成后仍需显式初始化、使能、恢复及允许遥控。配置维护在独立页中操作。"), this);
    notice->setWordWrap(true); layout->addWidget(notice);
    layout->addWidget(new QLabel(QStringLiteral("档案、模块与实际运行状态（只读）"), this));
    m_details = new QPlainTextEdit(this); m_details->setReadOnly(true); layout->addWidget(m_details, 1);
    layout->addWidget(new QLabel(QStringLiteral("操作进度与日志"), this));
    m_log = new QPlainTextEdit(this); m_log->setReadOnly(true); m_log->document()->setMaximumBlockCount(1500);
    layout->addWidget(m_log, 2);
    connect(connectButton, &QPushButton::clicked, this, [this] {
        QSettings s("HostComputer", "Management");
        s.setValue("host", m_host->text().trimmed()); s.setValue("port", m_port->value());
        m_settingsScope = m_host->text().trimmed() + ':' + QString::number(m_port->value());
        if (!s.value(m_settingsScope + "/pending_request").toString().isEmpty())
            append(QStringLiteral("上次请求可能已被接受，先查询当前操作，不自动重新启动/停止。"));
        m_service.open(m_host->text(), static_cast<quint16>(m_port->value()), m_token->text());
        m_configuration->open(m_host->text(), static_cast<quint16>(m_port->value()), m_token->text());
    });
    connect(disconnectButton, &QPushButton::clicked, this, [this] { m_service.close(); m_configuration->close(); });
    connect(m_configuration->service(), &ConfigurationService::sourceRevisionChanged, &m_service, &RuntimeService::refreshProfiles);
    connect(m_configuration->service(), &ConfigurationService::changed, this, [this] {
        m_service.setConfigurationBlocked(m_configuration->service()->blocksStart());
        updateActions();
    });
    connect(m_start, &QPushButton::clicked, this, [this] {
        const auto p = selectedProfile();
        m_service.start(p["profile_id"].toString(), p["config_revision"].toString());
    });
    connect(m_stop, &QPushButton::clicked, &m_service, &RuntimeService::stop);
    connect(m_preflight, &QPushButton::clicked, this, [this] {
        const auto p = selectedProfile();
        m_service.preflight(p["profile_id"].toString(), p["config_revision"].toString());
    });
    connect(&m_service, &RuntimeService::availabilityChanged, this, &RuntimeManagementWidget::updateActions);
    connect(&m_service, &RuntimeService::statusChanged, m_connection, &QLabel::setText);
    connect(&m_service, &RuntimeService::errorOccurred, this, [this](const QString &text) { append(text); });
    connect(&m_service, &RuntimeService::controlEndpointReady, this, &RuntimeManagementWidget::controlEndpointReady);
    connect(&m_service, &RuntimeService::controlUnavailable, this, &RuntimeManagementWidget::controlUnavailable);
    connect(&m_service, &RuntimeService::stateInvalidated, this, [this] {
        m_state->setText(QStringLiteral("软件状态：未知/同步中"));
        m_operation->setText(QStringLiteral("操作：未知/同步中，等待服务端确认"));
        m_safety->setText(QStringLiteral("硬件初始化/使能/急停：未知；运动授权以控制链路为准"));
        m_details->clear();
    });
    connect(&m_service, &RuntimeService::profilesReceived, this, [this](const QJsonArray &profiles) {
        QSettings s("HostComputer", "Management");
        const auto previous = s.value("profile_id").toString();
        m_profiles->clear();
        for (const auto &v : profiles) {
            const auto p = v.toObject();
            m_profiles->addItem(QStringLiteral("%1 / revision %2 / %3").arg(p["profile_id"].toString(), p["config_revision"].toString(), p["mode"].toString()), p);
            if (p["profile_id"].toString() == previous) m_profiles->setCurrentIndex(m_profiles->count() - 1);
        }
        updateActions();
    });
    connect(m_profiles, &QComboBox::currentIndexChanged, this, [this] {
        QSettings s("HostComputer", "Management"); s.setValue("profile_id", selectedProfile()["profile_id"].toString());
        updateActions();
    });
    connect(&m_service, &RuntimeService::mutationIssued, this, [this](const QString &id, const QString &method) {
        QSettings s("HostComputer", "Management");
        const auto scope = m_settingsScope;
        s.setValue(scope + "/pending_request", id); s.setValue(scope + "/pending_method", method); s.sync();
        append(QStringLiteral("已提交%1，等待操作ID；超时不表示未执行。").arg(method));
    });
    connect(&m_service, &RuntimeService::operationIdChanged, this, [this](const QString &id) {
        QSettings s("HostComputer", "Management");
        const auto scope = m_settingsScope;
        s.setValue(scope + "/operation_id", id);
        // Current operation has now been reconciled; no mutation is ever replayed from these settings.
        s.remove(scope + "/pending_request"); s.remove(scope + "/pending_method");
    });
    connect(&m_service, &RuntimeService::snapshotReceived, this, [this](const QJsonObject &snapshot) {
        m_state->setText(QStringLiteral("软件：%1 / 构建：%2%3").arg(softwareName(snapshot["software_state"].toString()), snapshot["build_version"].toString(),
            snapshot["recovery_required"].toBool() ? QStringLiteral("；需先受控停止/核查恢复") : QString()));
        const auto safety = snapshot["safety"].toObject();
        m_safety->setText(QStringLiteral("初始化：%1；使能：%2；急停：%3。运动许可以实时控制链路为准。")
            .arg(safetyValue(safety["hardware_initialized"]), safetyValue(safety["hardware_enabled"]), safetyValue(safety["emergency"].toObject()["active"])));
        const QJsonObject detail{{"selected_profile", selectedProfile()}, {"snapshot", snapshot}};
        m_details->setPlainText(QString::fromUtf8(QJsonDocument(detail).toJson(QJsonDocument::Indented)));
    });
    connect(&m_service, &RuntimeService::operationReceived, this, [this](const QJsonObject &op) {
        m_operation->setText(QStringLiteral("操作：%1\n%2 / %3").arg(op["operation_id"].toString(), op["state"].toString(), op["phase"].toString()));
        const auto error = op["error"].toObject();
        if (!error.isEmpty()) m_operation->setText(m_operation->text() + QStringLiteral("\n%1: %2\n%3").arg(error["code"].toString(), error["message"].toString(), error["hint"].toString()));
    });
    connect(&m_service, &RuntimeService::preflightReceived, this, [this](const QJsonObject &result) {
        append(QStringLiteral("预检：") + QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    });
    connect(&m_service, &RuntimeService::logsReceived, this, [this](const QStringList &lines, bool expired) {
        for (const auto &line : lines) append(line);
        m_log->setToolTip(expired ? QStringLiteral("服务端日志已达到保留上限，完整日志请查部署日志目录") : QString());
    });
    m_host->setObjectName("managementHost");
    m_port->setObjectName("managementPort");
    m_token->setObjectName("managementToken");
    m_profiles->setObjectName("managementProfiles");
    connectButton->setObjectName("managementConnect");
    disconnectButton->setObjectName("managementDisconnect");
    m_start->setObjectName("managementStart");
    m_stop->setObjectName("managementStop");
    m_preflight->setObjectName("managementPreflight");
    m_state->setObjectName("managementSoftwareState");
    m_safety->setObjectName("managementSafety");
    m_log->setObjectName("managementLog");
    updateActions();
}
QJsonObject RuntimeManagementWidget::selectedProfile() const { return m_profiles->currentData().toJsonObject(); }
void RuntimeManagementWidget::updateActions()
{
    m_configuration->service()->setMaintenanceReady(m_service.canStop()
        && m_service.snapshot()["software_state"] == QJsonValue("stopped")
        && m_service.snapshot()["recovery_required"] == QJsonValue(false));
    m_start->setEnabled(m_service.canStart() && !selectedProfile().isEmpty());
    m_preflight->setEnabled(m_service.canStart() && !selectedProfile().isEmpty());
    m_stop->setEnabled(m_service.canStop());
}
void RuntimeManagementWidget::append(const QString &text)
{
    QString clean = text;
    if (!m_token->text().isEmpty()) clean.replace(m_token->text(), QStringLiteral("[令牌已隐藏]"));
    m_log->appendPlainText(clean);
}
