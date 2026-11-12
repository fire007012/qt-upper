#include "RuntimeManagementWidget.h"
#include "ConfigurationWidget.h"
#include <QComboBox>
#include <QCheckBox>
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
    layout->addWidget(new QLabel(QStringLiteral("运动预设（勾选仅编辑，不使能；实机/Mock由上方软件档案决定）"), this));
    auto *presetRow = new QHBoxLayout;
    const QStringList ids{"flippers","base","arm"};
    const QStringList labels{QStringLiteral("摆臂"),QStringLiteral("底盘"),QStringLiteral("机械臂（含夹爪）")};
    for (int i = 0; i < ids.size(); ++i) {
        auto *check = new QCheckBox(labels[i], this); check->setObjectName("preset_" + ids[i]);
        check->setProperty("preset_id", ids[i]); check->setChecked(true);
        m_presetChecks.append(check); presetRow->addWidget(check);
        connect(check, &QCheckBox::toggled, this, [this] { emit pendingPresetsChanged(pendingPresets()); updateActions(); });
    }
    auto *all = new QPushButton(QStringLiteral("全车"), this); presetRow->addWidget(all);
    connect(all, &QPushButton::clicked, this, [this] { for (auto *check : m_presetChecks) check->setChecked(true); });
    layout->addLayout(presetRow);
    auto *presetButtons = new QHBoxLayout;
    m_presetStart = new QPushButton(QStringLiteral("启动所选运动组并授权"), this);
    m_presetHalt = new QPushButton(QStringLiteral("停止所选组"), this);
    m_presetSelect = new QPushButton(QStringLiteral("仅提交选择"), this);
    m_presetStart->setObjectName("presetStart"); m_presetHalt->setObjectName("presetHalt"); m_presetSelect->setObjectName("presetSelect");
    presetButtons->addWidget(m_presetStart); presetButtons->addWidget(m_presetHalt); presetButtons->addWidget(m_presetSelect);
    layout->addLayout(presetButtons);
    auto *recoveryButtons = new QHBoxLayout;
    m_presetRecover = new QPushButton(QStringLiteral("恢复所选组到 Armed"), this);
    m_presetRecover->setObjectName("presetRecover");
    m_presetRecover->setToolTip(QStringLiteral("先归中并点“允许操作”。恢复不进入 Running；之后仍需显式启动及新授权。"));
    m_presetHaltAll = new QPushButton(QStringLiteral("停止全部已确认组"), this); m_presetHaltAll->setObjectName("presetHaltAll");
    recoveryButtons->addWidget(m_presetRecover); recoveryButtons->addWidget(m_presetHaltAll); layout->addLayout(recoveryButtons);
    connect(m_presetRecover, &QPushButton::clicked, this, [this] { emit recoverPresetsRequested(pendingPresets()); });
    connect(m_presetHaltAll, &QPushButton::clicked, this, [this] { emit haltPresetsRequested(m_controlDiagnostics["selected_presets"].toArray()); });
    m_presetState = new QLabel(QStringLiteral("等待能力协商；旧服务端不支持分组启动"), this); m_presetState->setWordWrap(true); m_presetState->setObjectName("presetState"); layout->addWidget(m_presetState);
    m_presetDetails = new QPlainTextEdit(this); m_presetDetails->setReadOnly(true); m_presetDetails->setMaximumHeight(170); layout->addWidget(m_presetDetails);
    connect(m_presetStart, &QPushButton::clicked, this, [this] { emit startPresetsRequested(pendingPresets()); });
    connect(m_presetHalt, &QPushButton::clicked, this, [this] { emit haltPresetsRequested(pendingPresets()); });
    connect(m_presetSelect, &QPushButton::clicked, this, [this] { emit selectPresetsRequested(pendingPresets()); });
    auto *buttons = new QHBoxLayout;
    m_preflight = new QPushButton(QStringLiteral("预检"), this);
    m_start = new QPushButton(QStringLiteral("启动系统及所选预设"), this);
    m_stop = new QPushButton(QStringLiteral("停止系统"), this);
    buttons->addWidget(m_preflight); buttons->addWidget(m_start); buttons->addWidget(m_stop);
    layout->addLayout(buttons);
    m_state = new QLabel(QStringLiteral("软件状态：未知"), this); m_state->setWordWrap(true);
    m_operation = new QLabel(QStringLiteral("操作：未知"), this); m_operation->setWordWrap(true);
    m_safety = new QLabel(QStringLiteral("硬件初始化/使能/急停：未知；运动授权以控制链路为准"), this);
    m_safety->setWordWrap(true);
    layout->addWidget(m_state); layout->addWidget(m_operation); layout->addWidget(m_safety);
    auto *notice = new QLabel(QStringLiteral("启动所选预设将依次初始化、使能、运行并授权。需释放按键、摇杆归中；故障请显式恢复。急停始终停止全车。"), this);
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
    connect(disconnectButton, &QPushButton::clicked, this, [this] { m_startWhenReady = false; m_service.close(); m_configuration->close(); });
    connect(m_configuration->service(), &ConfigurationService::sourceRevisionChanged, &m_service, &RuntimeService::refreshProfiles);
    connect(m_configuration->service(), &ConfigurationService::changed, this, [this] {
        m_service.setConfigurationBlocked(m_configuration->service()->blocksStart());
        updateActions();
    });
    connect(m_start, &QPushButton::clicked, this, [this] {
        if (pendingPresets().isEmpty()) return;
        m_startTarget = pendingPresets(); m_startWhenReady = true;
        m_expectedStartRobot.clear(); m_expectedStartEndpoint.clear();
        emit pendingPresetsChanged(m_startTarget);
        const auto p = selectedProfile();
        m_startProfile = p["profile_id"].toString(); m_startRevision = p["config_revision"].toString();
        if (!m_service.start(m_startProfile, m_startRevision)) m_startWhenReady = false;
    });
    connect(m_stop, &QPushButton::clicked, this, [this] { m_startWhenReady = false; m_service.stop(); });
    connect(m_preflight, &QPushButton::clicked, this, [this] {
        const auto p = selectedProfile();
        m_service.preflight(p["profile_id"].toString(), p["config_revision"].toString());
    });
    connect(&m_service, &RuntimeService::availabilityChanged, this, &RuntimeManagementWidget::updateActions);
    connect(&m_service, &RuntimeService::statusChanged, m_connection, &QLabel::setText);
    connect(&m_service, &RuntimeService::errorOccurred, this, [this](const QString &text) { m_startWhenReady = false; append(text); });
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
        if (QStringList{"failed","cancelled","result_unknown"}.contains(op["state"].toString())) m_startWhenReady = false;
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
    m_start->setEnabled(m_service.canStart() && !selectedProfile().isEmpty() && !pendingPresets().isEmpty());
    const bool connected = m_controlDiagnostics["negotiated"].toBool() && m_controlDiagnostics["motion_presets_supported"].toBool();
    const bool ready = m_controlDiagnostics["ready"].toBool() && m_controlDiagnostics["motion_presets_supported"].toBool()
        && !m_controlDiagnostics["preset_flow_active"].toBool();
    m_presetStart->setEnabled(ready && !pendingPresets().isEmpty());
    m_presetHalt->setEnabled(connected && !pendingPresets().isEmpty());
    m_presetSelect->setEnabled(ready && !pendingPresets().isEmpty());
    m_presetRecover->setEnabled(ready && !pendingPresets().isEmpty());
    m_presetHaltAll->setEnabled(connected && !m_controlDiagnostics["selected_presets"].toArray().isEmpty());
    m_preflight->setEnabled(m_service.canStart() && !selectedProfile().isEmpty());
    m_stop->setEnabled(m_service.canStop());
}
void RuntimeManagementWidget::append(const QString &text)
{
    QString clean = text;
    if (!m_token->text().isEmpty()) clean.replace(m_token->text(), QStringLiteral("[令牌已隐藏]"));
    m_log->appendPlainText(clean);
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
    const bool fresh = diagnostics["ready"].toBool() && status["runtime_fresh"].toBool();
    const bool negotiated = diagnostics["negotiated"].toBool();
    const auto discovery = diagnostics["capability_discovery"].toString();
    if (discovery == "timeout") m_presetState->setText(QStringLiteral("能力发现超时，未取得控制会话；请检查网络、桥接状态和部署日志。"));
    else if (discovery == "invalid") m_presetState->setText(QStringLiteral("控制能力声明无效或版本不兼容；未取得控制会话。"));
    else if (!negotiated && !diagnostics["transport_connected"].toBool()) m_presetState->setText(QStringLiteral("控制桥未连接；请先连接管理服务并启动软件，或连接控制桥。"));
    else if (!negotiated) m_presetState->setText(discovery == "declared"
        ? QStringLiteral("能力声明已收到，正在协商控制会话并同步状态…") : QStringLiteral("控制桥已连接，等待能力声明…"));
    else if (!supported) m_presetState->setText(QStringLiteral("服务端未声明 motion_presets_v1；分组操作不可用，请升级桥接后重新连接。"));
    else m_presetState->setText(QStringLiteral("机器人确认：%1；反馈：%2；遥控：%3\n%4")
            .arg(QString::fromUtf8(QJsonDocument(diagnostics["selected_presets"].toArray()).toJson(QJsonDocument::Compact)),
                 fresh ? QStringLiteral("新鲜") : QStringLiteral("过期/未知，禁止运动"),
                 diagnostics["motion_authorized"].toBool() ? QStringLiteral("已授权") : QStringLiteral("暂停"), diagnostics["preset_flow_phase"].toString()));
    QJsonArray operations = diagnostics["operations"].toArray();
    for (int i = 0; i < operations.size(); ++i) {
        auto op = operations[i].toObject(); auto results = op["result"].toArray();
        for (int j = 0; j < results.size(); ++j) {
            auto result = results[j].toObject(); const auto doc = QJsonDocument::fromJson(result["message"].toString().toUtf8());
            if (doc.isObject()) result["parsed_message"] = doc.object();
            results[j] = result;
        }
        op["result"] = results; operations[i] = op;
    }
    if (supported && negotiated) {
        QStringList lines;
        const auto presets = status["presets"].toObject();
        for (const auto &id : QStringList{"flippers","base","arm"}) {
            const auto preset = presets[id].toObject(); const auto states = preset["states"].toObject(); QStringList groups, values;
            const QStringList names = id == "arm" ? QStringList{"arm","gripper"} : QStringList{id};
            for (const auto &g : names) { const auto state = states[g].toString("Unknown"); groups.append(g + ": " + state); values.append(state); }
            bool partial = false; for (const auto &v : values) partial |= v != values.first();
            lines.append(QStringLiteral("%1 [%2] %3；%4%5")
                .arg(preset["label"].toString(id), preset["selected"].toBool() ? QStringLiteral("已确认选择") : QStringLiteral("未选择"), groups.join(" / "),
                     fresh && preset["motion_permitted"].toBool() ? QStringLiteral("运行许可允许") : QStringLiteral("禁止/未知，查看阻断诊断"),
                     partial ? QStringLiteral("；部分状态不同，逐组核对") : QString()));
        }
        m_presetState->setText(m_presetState->text() + '\n' + lines.join('\n'));
    }
    m_presetDetails->setPlainText(QString::fromUtf8(QJsonDocument(QJsonObject{{"presets", status["presets"]}, {"operations", operations}}).toJson(QJsonDocument::Indented)));
    updateActions();
    if (m_startWhenReady && !m_expectedStartRobot.isEmpty() && diagnostics["ready"].toBool()
        && diagnostics["robot_id"] == QJsonValue(m_expectedStartRobot)
        && diagnostics["endpoint"] == QJsonValue(m_expectedStartEndpoint)
        && !diagnostics["connection_id"].toString().isEmpty()
        && diagnostics["connection_id"].toString() != m_previousConnectionId) {
        m_startWhenReady = false;
        if (supported) emit startPresetsRequested(m_startTarget);
        else append(QStringLiteral("软件已启动，但服务端缺少运动预设能力；分组启动未执行。"));
    }
}
