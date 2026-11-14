#include "RuntimeManagementWidget.h"
#include "ConfigurationWidget.h"
#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QLineEdit>
#include <QDialog>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QTimer>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
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
    layout->setContentsMargins(18, 18, 18, 18); layout->setSpacing(16);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("一键启动"), this);
    title->setStyleSheet("font-size: 20px; font-weight: 600;"); header->addWidget(title);
    header->addStretch();
    auto *settingsButton = new QPushButton(QStringLiteral("连接设置"), this);
    settingsButton->setObjectName("managementSettings"); header->addWidget(settingsButton);
    layout->addLayout(header);
    auto *connectionRow = new QHBoxLayout;
    m_connection = new QLabel(QStringLiteral("未连接"), this);
    m_connection->setObjectName("managementConnection"); connectionRow->addWidget(m_connection);
    m_profileName = new QLabel(this); connectionRow->addWidget(m_profileName); connectionRow->addStretch();
    auto *connectButton = new QPushButton(QStringLiteral("连接"), this);
    auto *disconnectButton = new QPushButton(QStringLiteral("断开"), this);
    connectionRow->addWidget(connectButton); layout->addLayout(connectionRow);

    m_settingsDialog = new QDialog(this); m_settingsDialog->setWindowTitle(QStringLiteral("连接与维护设置"));
    m_settingsDialog->resize(600, 500); m_settingsDialog->setObjectName("managementSettingsDialog");
    auto *settingsLayout = new QVBoxLayout(m_settingsDialog);
    auto *tabs = new QTabWidget(m_settingsDialog);
    auto *connectionPage = new QWidget(tabs); auto *form = new QFormLayout(connectionPage);
    QSettings settings("HostComputer", "Management");
    m_host = new QLineEdit(settings.value("host", "192.168.1.100").toString(), connectionPage);
    m_port = new QSpinBox(connectionPage); m_port->setRange(1, 65535); m_port->setValue(settings.value("port", 9091).toInt());
    m_token = new QLineEdit(connectionPage); m_token->setEchoMode(QLineEdit::Password);
    m_token->setPlaceholderText(QStringLiteral("部署令牌（仅在内存保留）"));
    m_profiles = new QComboBox(connectionPage);
    m_motionPreset = new QComboBox(connectionPage); m_motionPreset->setObjectName("motionPreset");
    m_motionPreset->setPlaceholderText(QStringLiteral("自定义选择"));
    m_motionPreset->addItem(QStringLiteral("底盘组"), QJsonArray{"base"});
    m_motionPreset->addItem(QStringLiteral("机械臂组"), QJsonArray{"arm"});
    m_motionPreset->addItem(QStringLiteral("全车组"), QJsonArray{"flippers","base","arm"});
    m_motionPreset->addItem(QStringLiteral("底盘和摆臂组"), QJsonArray{"flippers","base"});
    m_motionPreset->setCurrentIndex(2);
    form->addRow(QStringLiteral("下位机地址"), m_host); form->addRow(QStringLiteral("管理端口"), m_port);
    form->addRow(QStringLiteral("认证令牌"), m_token); form->addRow(QStringLiteral("运行环境"), m_profiles);
    form->addRow(QStringLiteral("运动预设"), m_motionPreset);
    auto *presetHint = new QLabel(QStringLiteral("运动预设同步启动页的勾选；模拟或实机由运行环境决定。"), connectionPage);
    presetHint->setWordWrap(true); form->addRow(presetHint);
    m_preflight = new QPushButton(QStringLiteral("检查连接与环境"), connectionPage);
    form->addRow(m_preflight); form->addRow(disconnectButton);
    auto *logDirectory = new QLabel(QStringLiteral("运行记录：%1").arg(m_journal.directory()), connectionPage);
    logDirectory->setWordWrap(true); logDirectory->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(logDirectory);
    m_configuration = new ConfigurationWidget(tabs);
    tabs->addTab(connectionPage, QStringLiteral("连接")); tabs->addTab(m_configuration, QStringLiteral("参数维护"));
    settingsLayout->addWidget(tabs);
    auto *closeSettings = new QPushButton(QStringLiteral("完成"), m_settingsDialog); settingsLayout->addWidget(closeSettings);
    connect(closeSettings, &QPushButton::clicked, m_settingsDialog, &QDialog::hide);
    connect(settingsButton, &QPushButton::clicked, m_settingsDialog, &QDialog::show);

    layout->addWidget(new QLabel(QStringLiteral("选择运动组"), this));
    const QStringList ids{"flippers","base","arm"};
    const QStringList labels{QStringLiteral("摆臂"),QStringLiteral("底盘"),QStringLiteral("机械臂（含夹爪）")};
    for (int i = 0; i < ids.size(); ++i) {
        auto *row = new QHBoxLayout;
        auto *check = new QCheckBox(labels[i], this); check->setObjectName("preset_" + ids[i]);
        check->setProperty("preset_id", ids[i]); check->setChecked(true); m_presetChecks.append(check);
        check->setStyleSheet("font-size: 16px; padding: 8px 0;"); row->addWidget(check); row->addStretch();
        auto *state = new QLabel(QStringLiteral("未连接"), this); state->setObjectName("groupState_" + ids[i]);
        m_groupStates.append(state); row->addWidget(state); layout->addLayout(row);
        connect(check, &QCheckBox::toggled, this, [this] { syncMotionPreset(); record("preset_selection",pendingPresets()); emit pendingPresetsChanged(pendingPresets()); updateActions(); });
    }
    auto *selectionRow = new QHBoxLayout;
    auto *all = new QPushButton(QStringLiteral("全选"), this); all->setObjectName("presetAll");
    selectionRow->addWidget(all); selectionRow->addStretch(); layout->addLayout(selectionRow);
    connect(all, &QPushButton::clicked, this, [this] { for (auto *check : m_presetChecks) check->setChecked(true); });
    m_presetState = new QLabel(QStringLiteral("请先连接下位机"), this);
    m_presetState->setObjectName("presetState"); m_presetState->setWordWrap(true); layout->addWidget(m_presetState);
    auto *buttons = new QHBoxLayout;
    m_start = new QPushButton(QStringLiteral("一键启动"), this); m_start->setMinimumHeight(42);
    m_presetHalt = new QPushButton(QStringLiteral("停止所选组"), this); m_presetHalt->setObjectName("presetHalt");
    m_presetHalt->setMinimumHeight(42); buttons->addWidget(m_start); buttons->addWidget(m_presetHalt);
    layout->addLayout(buttons);
    auto *footer = new QHBoxLayout;
    m_stop = new QPushButton(QStringLiteral("停止系统"), this); footer->addWidget(m_stop);
    m_emergencyHelp = new QPushButton(QStringLiteral("处理急停"), this); m_emergencyHelp->setObjectName("emergencyHelp");
    m_emergencyHelp->hide(); footer->addWidget(m_emergencyHelp); footer->addStretch();
    auto *more = new QPushButton(QStringLiteral("更多控制"), this); more->setObjectName("managementMoreControls");
    footer->addWidget(more); layout->addLayout(footer); layout->addStretch();
    auto *hint = new QLabel(QStringLiteral("启动前释放按键并将手柄归中。"), this); hint->setWordWrap(true); layout->addWidget(hint);

    m_controlsDialog = new QDialog(this); m_controlsDialog->setWindowTitle(QStringLiteral("运动组与速度控制"));
    m_controlsDialog->setObjectName("managementControlsDialog");
    m_controlsDialog->resize(560, 520); m_controlsDialog->setMinimumSize(480, 420);
    auto *controlsLayout = new QVBoxLayout(m_controlsDialog); controlsLayout->setContentsMargins(16,16,16,16);
    auto *scroll = new QScrollArea(m_controlsDialog); scroll->setObjectName("managementControlsScroll");
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto *controlsBody = new QWidget(scroll); auto *controls = new QVBoxLayout(controlsBody);
    controls->setContentsMargins(8,8,8,8); controls->setSpacing(14);
    controls->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(controlsBody); controlsLayout->addWidget(scroll);
    m_recoveryHint = new QLabel(m_controlsDialog); m_recoveryHint->setObjectName("emergencyRecoveryHint");
    m_recoveryHint->setWordWrap(true); controls->addWidget(m_recoveryHint);
    m_clearEmergency = new QPushButton(QStringLiteral("1. 解除急停锁存"),m_controlsDialog); m_clearEmergency->setObjectName("recoveryClearEmergency");
    m_allowRecovery = new QPushButton(QStringLiteral("2. 允许恢复操作"),m_controlsDialog); m_allowRecovery->setObjectName("recoveryAuthorize");
    controls->addWidget(m_clearEmergency); controls->addWidget(m_allowRecovery);
    m_presetSelect = new QPushButton(QStringLiteral("提交当前组选择"), m_controlsDialog); m_presetSelect->setObjectName("presetSelect");
    m_presetRecover = new QPushButton(QStringLiteral("恢复所选组"), m_controlsDialog); m_presetRecover->setObjectName("presetRecover");
    m_presetRecover->setToolTip(QStringLiteral("归中并允许操作后恢复；恢复不会自动运动。"));
    m_presetHaltAll = new QPushButton(QStringLiteral("停止全部运动组"), m_controlsDialog); m_presetHaltAll->setObjectName("presetHaltAll");
    controls->addWidget(m_presetSelect); controls->addWidget(m_presetRecover); controls->addWidget(m_presetHaltAll);
    auto *speedRow = new QHBoxLayout;
    m_speedLevel = new QComboBox(m_controlsDialog); m_speedLevel->setObjectName("speedLevel");
    for (int level = 1; level <= 5; ++level) m_speedLevel->addItem(QStringLiteral("%1 档").arg(level), level);
    m_speedApply = new QPushButton(QStringLiteral("换档"), m_controlsDialog); m_speedApply->setObjectName("speedApply");
    speedRow->addWidget(m_speedLevel); speedRow->addWidget(m_speedApply); controls->addLayout(speedRow);
    m_speedState = new QLabel(QStringLiteral("当前档位：未知"), m_controlsDialog); m_speedState->setObjectName("actualSpeed"); controls->addWidget(m_speedState);
    m_globalReset = new QPushButton(QStringLiteral("复位全局禁止"), m_controlsDialog); m_globalReset->setObjectName("globalReset");
    controls->insertWidget(3,m_globalReset); controls->addStretch();
    auto *closeControls = new QPushButton(QStringLiteral("关闭"), m_controlsDialog); closeControls->setObjectName("managementControlsClose");
    controlsLayout->addWidget(closeControls);
    connect(closeControls, &QPushButton::clicked, m_controlsDialog, &QDialog::hide);
    connect(more, &QPushButton::clicked, m_controlsDialog, &QDialog::show);
    connect(m_emergencyHelp, &QPushButton::clicked, m_controlsDialog, &QDialog::show);
    connect(m_clearEmergency, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this,QStringLiteral("解除急停"),QStringLiteral("确认急停原因已处理，并已释放按键、手柄归中？解除后仍需复位，机器不会自动运动。"),QMessageBox::Yes|QMessageBox::No,QMessageBox::No)==QMessageBox::Yes) {
            record("operator_action","clear_emergency"); emit fieldCommandRequested("clear_emergency",{});
        }
    });
    connect(m_allowRecovery, &QPushButton::clicked, this, [this] { record("operator_action","authorize_recovery"); emit authorizeOperationsRequested(); });
    connect(m_presetRecover, &QPushButton::clicked, this, [this] { emit recoverPresetsRequested(pendingPresets()); });
    connect(m_presetHaltAll, &QPushButton::clicked, this, [this] { emit haltPresetsRequested(m_controlDiagnostics["selected_presets"].toArray()); });
    connect(m_presetHalt, &QPushButton::clicked, this, [this] { m_startWhenReady = false; m_feedback.clear(); record("operator_action", "halt_presets"); emit haltPresetsRequested(pendingPresets()); });
    connect(m_presetSelect, &QPushButton::clicked, this, [this] { emit selectPresetsRequested(pendingPresets()); });
    connect(m_speedApply, &QPushButton::clicked, this, [this] { record("operator_action", "change_speed"); emit fieldCommandRequested("set_speed_level", {{"level",m_speedLevel->currentData().toInt()}}); });
    connect(m_globalReset, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, QStringLiteral("复位全局禁止"), QStringLiteral("确认急停原因已处理，已解除急停并允许操作？复位后不会自动恢复运动。"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
            record("operator_action", "reset_global_stop"); emit fieldCommandRequested("reset_global_stop", {});
        }
    });
    // An explicit local launcher may prefill the deployed endpoint and token.
    // This never connects, starts software, or grants motion automatically.
    const QString managerConfig = qEnvironmentVariable("HOSTCOMPUTER_MANAGEMENT_CONFIG");
    if (!managerConfig.isEmpty()) {
        QFile configFile(managerConfig);
        QJsonObject config;
        if (configFile.open(QIODevice::ReadOnly) && configFile.size() <= 65536)
            config = QJsonDocument::fromJson(configFile.readAll()).object();
        QString host = config["bind"].toString();
        if (host == "0.0.0.0") host = config["control_endpoint"].toObject()["host"].toString();
        const int port = config["port"].toInt();
        QFile tokenFile(config["token_file"].toString());
        QString token;
        if (tokenFile.open(QIODevice::ReadOnly) && tokenFile.size() <= 4096)
            token = QString::fromUtf8(tokenFile.readAll()).trimmed();
        if (!host.isEmpty() && port > 0 && port <= 65535 && token.size() >= 32) {
            m_host->setText(host); m_port->setValue(port); m_token->setText(token);
            append(QStringLiteral("已载入指定管理配置；请明确点击连接。"));
        } else {
            append(QStringLiteral("指定管理配置或令牌无效，请检查文件后手动填写。"));
        }
    }
    connect(connectButton, &QPushButton::clicked, this, [this] {
        QSettings s("HostComputer", "Management");
        s.setValue("host", m_host->text().trimmed()); s.setValue("port", m_port->value());
        m_settingsScope = m_host->text().trimmed() + ':' + QString::number(m_port->value());
        if (!s.value(m_settingsScope + "/pending_request").toString().isEmpty())
            append(QStringLiteral("上次请求可能已被接受，先查询当前操作，不自动重新启动/停止。"));
        m_feedback.clear(); m_activeToken = m_token->text();
        record("operator_action", "connect");
        m_service.open(m_host->text(), static_cast<quint16>(m_port->value()), m_token->text());
        m_configuration->open(m_host->text(), static_cast<quint16>(m_port->value()), m_token->text());
    });
    connect(disconnectButton, &QPushButton::clicked, this, [this] { m_startWhenReady = false; m_service.close(); m_configuration->close(); });
    connect(m_configuration->service(), &ConfigurationService::sourceRevisionChanged, &m_service, &RuntimeService::refreshProfiles);
    connect(m_configuration->service(), &ConfigurationService::changed, this, [this] {
        m_service.setConfigurationBlocked(m_configuration->service()->blocksStart());
        updateActions();
    });
    connect(m_start, &QPushButton::clicked, this, [this] {
        if (pendingPresets().isEmpty()) return;
        m_feedback.clear(); record("operator_action", QJsonObject{{"action","start"},{"presets",pendingPresets()}});
        if (!m_service.canStart()) { emit startPresetsRequested(pendingPresets()); return; }
        m_startTarget = pendingPresets(); m_startWhenReady = true;
        m_expectedStartRobot.clear(); m_expectedStartEndpoint.clear();
        emit pendingPresetsChanged(m_startTarget);
        const auto p = selectedProfile();
        m_startProfile = p["profile_id"].toString(); m_startRevision = p["config_revision"].toString();
        if (!m_service.start(m_startProfile, m_startRevision, m_startTarget)) reportActionFailure(QStringLiteral("管理启动请求未提交"));
    });
    connect(m_stop, &QPushButton::clicked, this, [this] {
        m_startWhenReady = false; m_feedback.clear(); record("operator_action", "stop_system");
        if (!m_service.stop()) reportActionFailure(QStringLiteral("管理停止请求未提交"));
    });
    connect(m_preflight, &QPushButton::clicked, this, [this] {
        m_feedback.clear();
        const auto p = selectedProfile();
        m_service.preflight(p["profile_id"].toString(), p["config_revision"].toString(), pendingPresets());
    });
    connect(&m_service, &RuntimeService::availabilityChanged, this, &RuntimeManagementWidget::updateActions);
    connect(&m_service, &RuntimeService::statusChanged, this, [this](const QString &text) {
        record("connection", text, true);
        m_connection->setText(text.startsWith(QStringLiteral("管理已连接")) ? QStringLiteral("已连接")
            : text.contains(QStringLiteral("正在连接")) ? QStringLiteral("连接中…") : QStringLiteral("未连接"));
        updateSummary();
    });
    connect(&m_service, &RuntimeService::errorOccurred, this, [this](const QString &text) {
        m_startWhenReady = false; record("management_error", text);
        m_feedback = QStringLiteral("操作未完成，请检查连接或查看运行日志。"); updateSummary();
    });
    connect(&m_service, &RuntimeService::controlEndpointReady, this,
            [this](const QString &host, quint16 port, int protocol, const QString &robotId) {
        if (m_startWhenReady) {
            const auto snapshot = m_service.snapshot();
            if (protocol != 2 || snapshot["profile_id"] != QJsonValue(m_startProfile)
                || snapshot["config_revision"] != QJsonValue(m_startRevision)) {
                m_startWhenReady = false;
                append(QStringLiteral("启动档案或控制端点与本次请求不一致，运动预设未执行。"));
            } else {
                m_expectedStartRobot = robotId;
                m_expectedStartEndpoint = host + ':' + QString::number(port);
                m_previousConnectionId = m_controlDiagnostics["connection_id"].toString();
            }
        }
        emit controlEndpointReady(host, port, protocol, robotId);
    });
    connect(&m_service, &RuntimeService::controlUnavailable, this, &RuntimeManagementWidget::controlUnavailable);
    connect(&m_service, &RuntimeService::stateInvalidated, this, [this] {
        m_connection->setText(QStringLiteral("未连接"));
        record("management_state", QJsonObject{{"state","unknown"}}, true); updateSummary();
    });
    connect(&m_service, &RuntimeService::profilesReceived, this, [this](const QJsonArray &profiles) {
        QSettings s("HostComputer", "Management");
        const auto previous = s.value("profile_id").toString();
        m_profiles->clear();
        for (const auto &v : profiles) {
            const auto p = v.toObject();
            m_profiles->addItem(QStringLiteral("%1 · %2").arg(p["profile_id"].toString(), p["mode"] == QJsonValue("mock") ? QStringLiteral("模拟") : QStringLiteral("实机")), p);
            if (p["profile_id"].toString() == previous) m_profiles->setCurrentIndex(m_profiles->count() - 1);
        }
        record("profiles", profiles, true); updateActions();
    });
    connect(m_profiles, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
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
        record("management_state", QJsonObject{{"software_state",snapshot["software_state"]},
            {"recovery_required",snapshot["recovery_required"]},{"safety",snapshot["safety"]},
            {"profile_id",snapshot["profile_id"]},{"modules",snapshot["modules"]}}, true);
        updateSummary();
    });
    connect(&m_service, &RuntimeService::operationReceived, this, [this](const QJsonObject &op) {
        record("management_operation", op, true);
        const auto outcome = op["operation_id"].toString() + '/' + op["state"].toString();
        if (QStringList{"failed","cancelled","result_unknown"}.contains(op["state"].toString())) {
            m_startWhenReady = false;
            m_feedback = op["state"] == QJsonValue("result_unknown") ? QStringLiteral("操作结果待确认，请勿重复启动。")
                : QStringLiteral("操作未完成，请查看运行日志。");
        } else if (op["state"] == QJsonValue("succeeded") && outcome != m_managementOutcome) m_feedback.clear();
        m_managementOutcome = outcome;
        updateSummary();
    });
    connect(&m_service, &RuntimeService::preflightReceived, this, [this](const QJsonObject &result) {
        record("preflight", result);
        m_feedback = result["passed"].toBool() ? QStringLiteral("检查通过，可以启动。") : QStringLiteral("检查未通过，请查看运行日志。");
        updateSummary();
    });
    connect(&m_service, &RuntimeService::logsReceived, this, [this](const QStringList &lines, bool expired) {
        for (const auto &line : lines) append(line);
        if (expired) record("remote_log_limit", QStringLiteral("服务端日志已达到保留上限"), true);
    });
    auto *journalTimer = new QTimer(this); journalTimer->setInterval(1000);
    connect(journalTimer, &QTimer::timeout, this, [this] {
        m_journal.snapshot({{"page_open",true},{"selected_profile",selectedProfile()},{"draft_presets",pendingPresets()},{"management",m_service.snapshot()},
            {"control",m_controlDiagnostics}}, {m_token->text(),m_activeToken});
        updateSummary();
    }); journalTimer->start();
    record("operator_page_opened", QJsonObject{{"log_directory",m_journal.directory()}});
    m_host->setObjectName("managementHost");
    m_port->setObjectName("managementPort");
    m_token->setObjectName("managementToken");
    m_profiles->setObjectName("managementProfiles");
    connectButton->setObjectName("managementConnect");
    disconnectButton->setObjectName("managementDisconnect");
    m_start->setObjectName("managementStart");
    m_stop->setObjectName("managementStop");
    m_preflight->setObjectName("managementPreflight");
    connect(m_motionPreset, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &RuntimeManagementWidget::applyMotionPreset);
    updateActions();
}
RuntimeManagementWidget::~RuntimeManagementWidget()
{
    // Close the member service while this widget and journal are still alive;
    // its final disconnect must not call the already-destroyed main window.
    disconnect(&m_service, nullptr, nullptr, nullptr);
    disconnect(m_configuration->service(), nullptr, this, nullptr);
    m_service.close();
    m_configuration->close();
    record("operator_page_closed", QJsonObject{});
    m_journal.snapshot({{"page_open",false},{"selected_profile",selectedProfile()},
        {"management",m_service.snapshot()},{"control",m_controlDiagnostics}}, {m_token->text(),m_activeToken});
}
QJsonObject RuntimeManagementWidget::selectedProfile() const { return m_profiles->currentData().toJsonObject(); }
void RuntimeManagementWidget::syncMotionPreset()
{
    const auto selected = pendingPresets();
    int match = -1;
    for (int i = 0; i < m_motionPreset->count(); ++i) {
        const auto preset = m_motionPreset->itemData(i).toJsonArray();
        bool same = preset.size() == selected.size();
        for (const auto &id : selected) same &= preset.contains(id);
        if (same) { match = i; break; }
    }
    const QSignalBlocker blocker(m_motionPreset); m_motionPreset->setCurrentIndex(match);
}
void RuntimeManagementWidget::applyMotionPreset(int index)
{
    if (index < 0) return;
    const auto selected = m_motionPreset->itemData(index).toJsonArray();
    for (auto *check : m_presetChecks) {
        const QSignalBlocker blocker(check);
        check->setChecked(selected.contains(check->property("preset_id").toString()));
    }
    record("preset_selection", pendingPresets());
    emit pendingPresetsChanged(pendingPresets()); updateActions();
}
void RuntimeManagementWidget::updateActions()
{
    m_configuration->service()->setMaintenanceReady(m_service.canStop()
        && m_service.snapshot()["software_state"] == QJsonValue("stopped")
        && m_service.snapshot()["recovery_required"] == QJsonValue(false));
    const bool connected = m_controlDiagnostics["negotiated"].toBool() && m_controlDiagnostics["motion_presets_supported"].toBool();
    const bool ready = m_controlDiagnostics["ready"].toBool() && m_controlDiagnostics["motion_presets_supported"].toBool()
        && !m_controlDiagnostics["preset_flow_active"].toBool();
    const auto software = m_service.snapshot()["software_state"].toString();
    const bool canRunGroups = ready && (software.isEmpty() || QStringList{"ready","degraded"}.contains(software))
        && !m_controlDiagnostics["emergency_active"].toBool()
        && m_controlDiagnostics["server"].toObject()["runtime_global_inhibit"] != QJsonValue(true);
    m_start->setEnabled(!pendingPresets().isEmpty()
        && ((m_service.canStart() && !selectedProfile().isEmpty()) || canRunGroups));
    m_presetHalt->setEnabled(connected && !pendingPresets().isEmpty());
    m_presetSelect->setEnabled(ready && !pendingPresets().isEmpty());
    m_presetRecover->setEnabled(ready && !pendingPresets().isEmpty());
    m_presetHaltAll->setEnabled(connected && !m_controlDiagnostics["selected_presets"].toArray().isEmpty());
    const bool fieldReady = m_controlDiagnostics["ready"].toBool() && !m_controlDiagnostics["preset_flow_active"].toBool();
    const auto actual = m_controlDiagnostics["server"].toObject();
    bool busy = false;
    for (const auto &value : m_controlDiagnostics["operations"].toArray()) {
        const auto op = value.toObject();
        busy |= QStringList{"awaiting_response","accepted","running"}.contains(op["state"].toString())
            || (op["state"] == QJsonValue("result_unknown") && !op["unknown_acknowledged"].toBool());
    }
    m_start->setEnabled(m_start->isEnabled() && !busy);
    m_speedApply->setEnabled(fieldReady && m_controlDiagnostics["speed_level_supported"].toBool()
        && !m_controlDiagnostics["speed_change_pending"].toBool() && !actual["emergency_active"].toBool() && !busy);
    m_speedLevel->setEnabled(m_speedApply->isEnabled());
    m_globalReset->setEnabled(fieldReady && m_controlDiagnostics["global_stop_supported"].toBool()
        && actual["global_stop_reset_ready"] != QJsonValue(false)
        && actual["emergency_active"] == QJsonValue(false) && actual["runtime_fresh"].toBool()
        && actual["runtime_global_inhibit"] == QJsonValue(true) && m_controlDiagnostics["operations_authorized"].toBool() && !busy);
    const bool emergency = m_controlDiagnostics["emergency_active"].toBool() || m_controlDiagnostics["emergency_intent_latched"].toBool();
    const bool inhibited = actual["runtime_global_inhibit"] == QJsonValue(true);
    const bool recoveryVisible = emergency || inhibited;
    m_emergencyHelp->setVisible(recoveryVisible);
    m_globalReset->setVisible(recoveryVisible);
    m_globalReset->setText(recoveryVisible ? QStringLiteral("3. 复位全局禁止") : QStringLiteral("复位全局禁止"));
    m_recoveryHint->setVisible(recoveryVisible); m_clearEmergency->setVisible(recoveryVisible); m_allowRecovery->setVisible(recoveryVisible);
    m_clearEmergency->setEnabled(fieldReady && emergency && actual["runtime_fresh"].toBool() && inhibited
        && actual["global_stop_reset_ready"] != QJsonValue(false)
        && actual["global_stop"].toObject()["state"] == QJsonValue("succeeded") && actual["global_stop"].toObject()["ok"] == QJsonValue(true) && !busy);
    m_allowRecovery->setEnabled(fieldReady && !emergency && inhibited && !m_controlDiagnostics["operations_authorized"].toBool() && !busy);
    m_recoveryHint->setText(emergency ? QStringLiteral("上次急停仍然锁存。保持归中，依次解除急停、允许恢复操作、复位全局禁止，再点击一键启动。停止系统不会清除急停。")
        : QStringLiteral("急停锁存已解除。保持归中，允许恢复操作并复位全局禁止，然后重新启动运动组。"));
    m_preflight->setEnabled(m_service.canStart() && !selectedProfile().isEmpty());
    m_stop->setEnabled(m_service.canStop()); updateSummary();
}
void RuntimeManagementWidget::record(const QString &event, const QJsonValue &data, bool changed)
{
    const QStringList secrets{m_token->text(),m_activeToken};
    if (changed) m_journal.changed(event,data,secrets); else m_journal.append(event,data,secrets);
}
void RuntimeManagementWidget::append(const QString &text) { record("runtime_log", text); }

void RuntimeManagementWidget::reportActionFailure(const QString &detail)
{
    record("control_error", detail);
    m_startWhenReady = false;
    m_feedback = QStringLiteral("操作未提交，请检查连接、归中及授权。"); updateSummary();
}
void RuntimeManagementWidget::observeControlEvent(const QJsonObject &event)
{
    const auto type = event["type"].toString();
    if (QStringList{"control_status","control_timing"}.contains(type)) return;
    if (!QStringList{"control_notice","control_operation","emergency_result","connection_state",
        "connection_recovery","protocol_error","ack_timeout","ack_disconnected"}.contains(type)) return;
    record("control_event", event);
    if (type == "control_operation") {
        const auto state = event["operation"].toObject()["state"].toString();
        if (state == "failed") m_feedback = QStringLiteral("运动组操作未完成，请查看运行日志。");
        else if (state == "result_unknown") m_feedback = QStringLiteral("操作结果待确认，请勿重复启动。");
    }
    updateSummary();
}
void RuntimeManagementWidget::updateSummary()
{
    const auto diagnostics = m_controlDiagnostics;
    const auto status = diagnostics["server"].toObject();
    const bool negotiated = diagnostics["negotiated"].toBool();
    const bool fresh = diagnostics["ready"].toBool() && status["runtime_fresh"].toBool();
    const auto software = m_service.snapshot();
    m_profileName->setText(selectedProfile().isEmpty() ? QString()
        : selectedProfile()["mode"] == QJsonValue("mock") ? QStringLiteral("模拟") : QStringLiteral("实机"));
    const QStringList ids{"flippers","base","arm"};
    for (int i = 0; i < ids.size(); ++i) {
        const auto preset = status["presets"].toObject()[ids[i]].toObject();
        const auto states = preset["states"].toObject();
        QString state;
        bool mixed = false;
        for (auto it = states.begin(); it != states.end(); ++it) {
            if (state.isEmpty()) state = it.value().toString(); else mixed |= state != it.value().toString();
        }
        QString text = QStringLiteral("未连接");
        if (negotiated && !fresh) text = QStringLiteral("状态待更新");
        else if (fresh && mixed) text = QStringLiteral("部分就绪");
        else if (fresh) {
            if (state == "Running") text = preset["motion_permitted"].toBool() ? QStringLiteral("运行中") : QStringLiteral("运行受限");
            else if (state == "Stopped") text = QStringLiteral("未启动");
            else if (state == "Halted") text = QStringLiteral("已停止");
            else if (state == "Initialized") text = QStringLiteral("已初始化");
            else if (state == "Armed") text = QStringLiteral("待运行");
            else if (state == "Fault" || state == "Faulted") text = QStringLiteral("故障");
            else text = QStringLiteral("状态未知");
        }
        m_groupStates[i]->setText(text);
    }
    QString summary;
    if (software["recovery_required"].toBool()) summary = QStringLiteral("上次运行未正常结束，请先停止系统完成恢复。");
    else if (diagnostics["emergency_active"].toBool() || diagnostics["emergency_intent_latched"].toBool())
        summary = m_journal.healthy() ? QStringLiteral("上次急停仍然锁存，请点击“处理急停”。") : QStringLiteral("急停仍锁存；运行日志保存失败。");
    else if (!m_journal.healthy()) summary = QStringLiteral("运行日志保存失败，请检查磁盘或目录权限。");
    else if (status["runtime_global_inhibit"] == QJsonValue(true))
        summary = status["global_stop_reset_ready"] == QJsonValue(false)
            ? QStringLiteral("正在停止，请等待清理完成。") : QStringLiteral("运动已禁止，请在更多控制中显式复位。");
    else if (!m_feedback.isEmpty()) summary = m_feedback;
    else if (software["software_state"] == QJsonValue("starting") || m_startWhenReady || diagnostics["preset_flow_active"].toBool())
        summary = QStringLiteral("正在启动所选运动组，请保持归中…");
    else if (software["software_state"] == QJsonValue("stopping")) summary = QStringLiteral("正在停止系统…");
    else if (diagnostics["capability_discovery"] == QJsonValue("timeout")) summary = QStringLiteral("连接超时，请检查下位机。");
    else if (diagnostics["capability_discovery"] == QJsonValue("invalid")) summary = QStringLiteral("设备连接不兼容，请查看运行日志。");
    else if (negotiated && !diagnostics["motion_presets_supported"].toBool()) summary = QStringLiteral("当前设备不支持运动组控制。");
    else if (negotiated && !fresh) summary = QStringLiteral("状态更新中，请等待。");
    else if (diagnostics["transport_connected"].toBool() && !negotiated) summary = QStringLiteral("正在连接控制器…");
    else if (fresh && diagnostics["motion_authorized"].toBool()) summary = QStringLiteral("运行中。");
    else if (fresh) summary = QStringLiteral("已就绪，启动前请释放按键并归中。");
    else if (!software.isEmpty()) summary = QStringLiteral("系统%1，请选择运动组后启动。").arg(softwareName(software["software_state"].toString()));
    else summary = QStringLiteral("请先连接下位机。");
    m_presetState->setText(summary);
}

QJsonArray RuntimeManagementWidget::pendingPresets() const
{
    QJsonArray ids; for (auto *check : m_presetChecks) if (check->isChecked()) ids.append(check->property("preset_id").toString()); return ids;
}
void RuntimeManagementWidget::setControlDiagnostics(const QJsonObject &diagnostics)
{
    m_controlDiagnostics = diagnostics;
    const bool supported = diagnostics["motion_presets_supported"].toBool();
    const auto status = diagnostics["server"].toObject();
    // Keep changing ages/counters in latest.json; append semantic changes only.
    record("control_state", QJsonObject{{"ready",diagnostics["ready"]},{"negotiated",diagnostics["negotiated"]},
        {"discovery",diagnostics["capability_discovery"]},{"epoch",diagnostics["epoch"]},
        {"motion_authorized",diagnostics["motion_authorized"]},{"emergency_active",diagnostics["emergency_active"]},
        {"preset_flow_active",diagnostics["preset_flow_active"]},{"preset_flow_phase",diagnostics["preset_flow_phase"]},
        {"presets",status["presets"]},{"global_stop",status["global_stop"]},{"safety_halt",status["safety_halt"]},
        {"global_stop_reset_ready",status["global_stop_reset_ready"]},{"speed_level",status["speed_level"]},
        {"runtime_fresh",status["runtime_fresh"]},{"runtime_global_inhibit",status["runtime_global_inhibit"]},
        {"operations",diagnostics["operations"]}}, true);
    m_speedState->setText(diagnostics["ready"].toBool() && status["speed_level"].isDouble()
        ? QStringLiteral("当前档位：%1%2").arg(status["speed_level"].toInt()).arg(diagnostics["speed_change_pending"].toBool() ? QStringLiteral("（换档中）") : QString())
        : QStringLiteral("当前档位：未知"));
    updateActions();
    if (m_startWhenReady && !m_expectedStartRobot.isEmpty() && diagnostics["ready"].toBool()
        && diagnostics["robot_id"] == QJsonValue(m_expectedStartRobot)
        && diagnostics["endpoint"] == QJsonValue(m_expectedStartEndpoint)
        && !diagnostics["connection_id"].toString().isEmpty()
        && diagnostics["connection_id"].toString() != m_previousConnectionId) {
        m_startWhenReady = false;
        if (diagnostics["emergency_active"].toBool() || status["runtime_global_inhibit"] == QJsonValue(true)) {
            record("start_blocked",QStringLiteral("软件启动完成，急停锁存或全局禁止尚未解除")); updateSummary();
        } else if (supported) emit startPresetsRequested(m_startTarget);
        else append(QStringLiteral("软件已启动，但服务端缺少运动预设能力；分组启动未执行。"));
    }
}
