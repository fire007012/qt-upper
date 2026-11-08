#include "DiagnosticsWidget.h"
#include "controller.h"
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

DiagnosticsWidget::DiagnosticsWidget(Controller *controller, QWidget *parent) : QWidget(parent), m_controller(controller)
{
    auto *layout = new QVBoxLayout(this);
    auto *notice = new QLabel(QStringLiteral("诊断记录本机观测、拒收计数、任务及遥测年龄。任务接受不等于成功，急停响应不等于物理停止。导出不包含会话令牌和管理认证令牌。"), this);
    notice->setWordWrap(true); layout->addWidget(notice);
    m_status = new QLabel(this); m_status->setWordWrap(true); layout->addWidget(m_status);
    auto *row = new QHBoxLayout; m_requests = new QComboBox(this);
    m_recover = new QPushButton(QStringLiteral("查询/恢复所选请求"), this);
    row->addWidget(m_requests, 1); row->addWidget(m_recover); layout->addLayout(row);
    m_details = new QPlainTextEdit(this); m_details->setReadOnly(true); layout->addWidget(m_details, 1);
    auto *exportButton = new QPushButton(QStringLiteral("导出诊断 JSON"), this); layout->addWidget(exportButton);
    connect(m_recover, &QPushButton::clicked, this, [this] {
        const auto id = m_requests->currentData().toString(); if (id.isEmpty()) return;
        if (QMessageBox::question(this, QStringLiteral("核对原请求"),
            QStringLiteral("已知operation_id时只查询任务；接受响应未知时使用原ID和原参数恢复。若原请求尚未被接受，恢复可能执行该操作，且需要显式操作授权。是否继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes && !m_controller->recoverControlRequest(id))
            m_status->setText(QStringLiteral("会话尚未就绪或请求正在查询，请核对连接及授权。"));
    });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("保存诊断"), "host-diagnostics.json", "JSON (*.json)");
        if (path.isEmpty()) return;
        m_status->setText(m_controller->exportDiagnostics(path) ? QStringLiteral("诊断已导出。") : QStringLiteral("导出失败，请检查目标目录和权限。"));
    });
    m_timer.setInterval(1000); connect(&m_timer, &QTimer::timeout, this, &DiagnosticsWidget::refresh); m_timer.start(); refresh();
}
void DiagnosticsWidget::refresh()
{
    const auto bundle = m_controller->diagnostics();
    m_details->setPlainText(QString::fromUtf8(QJsonDocument(bundle).toJson(QJsonDocument::Indented)));
    const auto previous = m_requests->currentData().toString(); const QSignalBlocker blocker(m_requests); m_requests->clear();
    for (const auto &value : m_controller->controlDiagnostics()["operations"].toArray()) {
        const auto op = value.toObject(); const auto id = op["request_id"].toString();
        m_requests->addItem(QStringLiteral("%1 / %2 / %3").arg(op["command"].toString(), op["state"].toString(), id), id);
        if (id == previous) m_requests->setCurrentIndex(m_requests->count()-1);
    }
    m_recover->setEnabled(m_controller->controlDiagnostics()["ready"].toBool() && m_requests->count());
}
