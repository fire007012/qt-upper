#include "ConfigurationWidget.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextDocument>
#include <QVBoxLayout>

namespace {
QString json(const QJsonObject &object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)); }
bool confirm(QWidget *parent, const QString &title, const QString &text) {
    return QMessageBox::question(parent, title, text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}
}
ConfigurationWidget::ConfigurationWidget(QWidget *parent) : QWidget(parent), m_service(this)
{
    auto *layout = new QVBoxLayout(this);
    auto *notice = new QLabel(QStringLiteral("仅维护管理启动策略。保存只生成候选；应用/回滚前请在运行管理中停止系统。应用后需显式启动。"), this);
    notice->setWordWrap(true); layout->addWidget(notice);
    m_status = new QLabel(QStringLiteral("配置状态未知/未连接"), this); m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse); m_status->setObjectName("configurationStatus");
    layout->addWidget(m_status);
    m_profiles = new QComboBox(this); m_profiles->setObjectName("configurationProfiles"); layout->addWidget(m_profiles);
    m_timeoutRow = new QWidget(this); auto *timeoutLayout = new QHBoxLayout(m_timeoutRow); timeoutLayout->setContentsMargins(0,0,0,0);
    timeoutLayout->addWidget(new QLabel(QStringLiteral("启动健康等待预算（秒）"), this));
    m_timeout = new QDoubleSpinBox(this); m_timeout->setDecimals(6); m_timeout->setObjectName("configurationTimeout"); timeoutLayout->addWidget(m_timeout);
    layout->addWidget(m_timeoutRow);
    m_videoRow = new QWidget(this); auto *videoLayout = new QHBoxLayout(m_videoRow); videoLayout->setContentsMargins(0,0,0,0);
    m_video = new QCheckBox(QStringLiteral("视频为必需模块（档案须已启用视频）"), this); m_video->setObjectName("configurationVideoRequired"); videoLayout->addWidget(m_video);
    layout->addWidget(m_videoRow);
    auto *buttons = new QHBoxLayout;
    auto button = [this](const QString &title, const char *name) {
        auto *b = new QPushButton(title, this); b->setObjectName(name); return b;
    };
    m_refresh = button(QStringLiteral("重新加载"), "configurationRefresh");
    m_save = button(QStringLiteral("核对并保存候选"), "configurationSave");
    m_recover = button(QStringLiteral("用原ID恢复未知请求"), "configurationRecover");
    buttons->addWidget(m_refresh); buttons->addWidget(m_save); buttons->addWidget(m_recover); layout->addLayout(buttons);
    auto *candidateRow = new QHBoxLayout;
    m_candidates = new QComboBox(this); m_candidates->setObjectName("configurationCandidates");
    m_load = button(QStringLiteral("查看候选差异"), "configurationPreview");
    m_apply = button(QStringLiteral("确认应用候选"), "configurationApply");
    candidateRow->addWidget(m_candidates, 1); candidateRow->addWidget(m_load); candidateRow->addWidget(m_apply); layout->addLayout(candidateRow);
    auto *rollbackRow = new QHBoxLayout;
    m_target = new QLineEdit(this); m_target->setPlaceholderText(QStringLiteral("历史bundle revision（可从审计复制）")); m_target->setObjectName("configurationRollbackTarget");
    m_rollbackPreview = button(QStringLiteral("查看回滚目标"), "configurationRollbackPreview");
    m_rollback = button(QStringLiteral("确认回滚"), "configurationRollback");
    rollbackRow->addWidget(m_target, 1); rollbackRow->addWidget(m_rollbackPreview); rollbackRow->addWidget(m_rollback); layout->addLayout(rollbackRow);
    m_preview = new QPlainTextEdit(this); m_preview->setReadOnly(true); m_preview->setObjectName("configurationDiff");
    m_preview->setPlaceholderText(QStringLiteral("候选差异、受影响组、维护与重启要求；回滚目标摘要")); layout->addWidget(m_preview, 1);
    m_operation = new QLabel(QStringLiteral("配置操作：无"), this); m_operation->setWordWrap(true); m_operation->setObjectName("configurationOperation"); layout->addWidget(m_operation);
    m_source = new QPlainTextEdit(this); m_source->setReadOnly(true); m_source->setObjectName("configurationSource");
    m_source->setPlaceholderText(QStringLiteral("源档案和字段约束（只读）")); layout->addWidget(m_source, 1);
    auto *auditRow = new QHBoxLayout;
    m_audit = button(QStringLiteral("加载审计首页"), "configurationAudit"); m_auditNext = button(QStringLiteral("下一页审计"), "configurationAuditNext");
    auditRow->addWidget(m_audit); auditRow->addWidget(m_auditNext); layout->addLayout(auditRow);
    m_log = new QPlainTextEdit(this); m_log->setReadOnly(true); m_log->setObjectName("configurationLog"); m_log->document()->setMaximumBlockCount(1500); layout->addWidget(m_log, 1);
    connect(&m_service, &ConfigurationService::changed, this, &ConfigurationWidget::update);
    connect(&m_service, &ConfigurationService::sourceReceived, this, &ConfigurationWidget::loadSource);
    connect(&m_service, &ConfigurationService::message, this, &ConfigurationWidget::log);
    connect(&m_service, &ConfigurationService::profilesReceived, this, [this](const QJsonArray &profiles) {
        const QSignalBlocker blocker(m_profiles); m_profiles->clear();
        for (const auto &v : profiles) {
            const auto p = v.toObject(); m_profiles->addItem(p["profile_id"].toString(), p["profile_id"]);
            if (p["profile_id"] == QJsonValue(m_service.profileId())) m_profiles->setCurrentIndex(m_profiles->count()-1);
        }
    });
    connect(m_profiles, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { m_service.selectProfile(m_profiles->currentData().toString()); });
    connect(m_refresh, &QPushButton::clicked, &m_service, &ConfigurationService::refresh);
    connect(m_save, &QPushButton::clicked, this, [this] {
        QJsonArray patches;
        if (m_timeout->isEnabled() && m_timeout->value() != m_timeoutValue)
            patches.append(QJsonObject{{"profile_id", m_service.profileId()}, {"field", "startup_timeout"}, {"value", m_timeout->value()}});
        if (m_video->isEnabled() && m_video->isChecked() != m_videoValue)
            patches.append(QJsonObject{{"profile_id", m_service.profileId()}, {"field", "video_required"}, {"value", m_video->isChecked()}});
        if (patches.isEmpty()) { log(QStringLiteral("没有可保存的改动。")); return; }
        const auto text = QStringLiteral("保存以下候选改动：\n%1\n影响全部组；应用前需停机，应用后需显式重启。保存不激活，不写设备。")
            .arg(QString::fromUtf8(QJsonDocument(patches).toJson(QJsonDocument::Indented)));
        if (confirm(this, QStringLiteral("核对候选差异"), text) && !m_service.saveCandidate(patches)) log(QStringLiteral("状态已变化或字段不满足约束，请重新核对。"));
    });
    connect(m_load, &QPushButton::clicked, this, [this] { m_service.preview(m_candidates->currentText()); });
    connect(m_apply, &QPushButton::clicked, this, [this] {
        const auto candidate = m_service.candidate();
        if (confirm(this, QStringLiteral("确认配置应用"), QStringLiteral("应用以下已核对候选？\n%1\n只激活管理源；不会自动启动或授予运动权限。").arg(json(candidate)))) m_service.apply();
    });
    connect(m_rollbackPreview, &QPushButton::clicked, this, [this] { m_service.previewRollback(m_target->text().trimmed()); });
    connect(m_rollback, &QPushButton::clicked, this, [this] {
        if (confirm(this, QStringLiteral("确认配置回滚"), QStringLiteral("将全部档案恢复到以下历史bundle。当前页显示所选档案和全包摘要，其他档案也会恢复。需停机及显式重启。\n%1").arg(json(m_service.rollbackPreview())))) m_service.rollback();
    });
    connect(m_recover, &QPushButton::clicked, this, [this] {
        if (confirm(this, QStringLiteral("恢复未知请求"), QStringLiteral("已查询当前源和状态。将使用原request_id与原参数查询/恢复原请求；若服务端尚未接受，它可能开始执行。是否继续？"))) m_service.recoverRequest();
    });
    connect(m_audit, &QPushButton::clicked, this, [this] { m_service.audit(true); });
    connect(m_auditNext, &QPushButton::clicked, this, [this] { m_service.audit(); });
    connect(&m_service, &ConfigurationService::auditReceived, this, [this](const QJsonArray &items, bool reset) {
        if (reset) m_log->clear();
        log(QString::fromUtf8(QJsonDocument(items).toJson(QJsonDocument::Indented)));
        if (items.isEmpty()) log(QStringLiteral("已到审计末尾。"));
    });
    update();
}
void ConfigurationWidget::open(const QString &host, quint16 port, const QString &token)
{
    m_log->clear(); m_token = token; m_service.open(host, port, token);
}
void ConfigurationWidget::close() { m_service.close(); m_token.clear(); }
void ConfigurationWidget::loadSource(const QJsonObject &source)
{
    const auto schema = m_service.schema(); const auto fields = schema["fields"].toObject();
    const auto timeout = fields["startup_timeout"].toObject(); const auto video = fields["video_required"].toObject();
    m_timeoutRow->setVisible(timeout["editable"].toBool()); m_videoRow->setVisible(video["editable"].toBool());
    m_timeout->setRange(timeout["minimum"].toDouble(), timeout["maximum"].toDouble());
    m_timeout->setToolTip(json(timeout)); m_video->setToolTip(json(video));
    const auto profile = source["profile"].toObject();
    m_timeout->setValue(profile["startup_timeout"].toDouble(90)); m_timeoutValue = m_timeout->value();
    m_video->setChecked(profile["video_required"].toBool()); m_videoValue = m_video->isChecked();
    m_source->setPlainText(json({{"source", source}, {"schema", schema}}));
}
void ConfigurationWidget::update()
{
    const auto status = m_service.status(); const bool edit = m_service.canEdit();
    m_refresh->setEnabled(m_service.canQuery()); m_profiles->setEnabled(m_service.canQuery());
    m_save->setEnabled(edit);
    m_timeout->setEnabled(edit && m_service.schema()["fields"].toObject()["startup_timeout"].toObject()["editable"].toBool());
    m_video->setEnabled(edit && m_service.schema()["fields"].toObject()["video_required"].toObject()["editable"].toBool()
        && m_service.source()["profile"].toObject()["video"].toBool());
    m_apply->setEnabled(m_service.canApply()); m_rollback->setEnabled(m_service.canRollback());
    m_recover->setEnabled(m_service.canQuery() && !status.isEmpty() && m_service.needsRecovery() && !m_service.source().isEmpty());
    m_audit->setEnabled(m_service.canQuery()); m_auditNext->setEnabled(m_service.canQuery());
    m_load->setEnabled(edit && !m_candidates->currentText().isEmpty()); m_rollbackPreview->setEnabled(edit);
    if (status.isEmpty()) {
        m_status->setText(QStringLiteral("配置状态未知/未连接；已有操作结果需重连查询。")); m_source->clear(); m_preview->clear();
    } else {
        const auto actual = status["actual_runtime_revision"].toString();
        m_status->setText(QStringLiteral("源：%1\n管理源读回：%2\n运行已消费版本：%3；观测时间：%4\n%5；设备非易失写入：无%6")
            .arg(status["source_revision"].toString(), status["effective_management_revision"].toString(),
                actual.isEmpty() ? QStringLiteral("未知/未运行") : actual,
                status["runtime_observed_at"].isDouble() ? QString::number(status["runtime_observed_at"].toDouble(), 'f', 3) : QStringLiteral("未知"),
                status["pending_restart"].toBool() ? QStringLiteral("待显式启动/重启") : QStringLiteral("管理启动策略已被运行消费"),
                m_service.needsRecovery() ? QStringLiteral("；有未知请求待原ID恢复") : QString()));
        const auto selected = m_candidates->currentText();
        const QSignalBlocker blocker(m_candidates); m_candidates->clear();
        for (const auto &v : status["candidate_revisions"].toArray()) m_candidates->addItem(v.toString());
        const auto index = m_candidates->findText(selected); if (index >= 0) m_candidates->setCurrentIndex(index);
        const QJsonObject detail{{"candidate", m_service.candidate()}, {"rollback_target", m_service.rollbackPreview()}};
        m_preview->setPlainText(json(detail));
    }
    const auto op = m_service.operation();
    m_operation->setText(op.isEmpty() ? QStringLiteral("配置操作：无") : QStringLiteral("配置操作（连接失效时须重新查询）：\n%1").arg(json(op)));
}
void ConfigurationWidget::log(const QString &text)
{
    QString clean = text; if (!m_token.isEmpty()) clean.replace(m_token, QStringLiteral("[令牌已隐藏]"));
    m_log->appendPlainText(clean);
}
