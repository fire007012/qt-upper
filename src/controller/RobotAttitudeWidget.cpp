#include "RobotAttitudeWidget.h"

#include "RobotViewModel.h"

#include <QObject>
#include <QQuickItem>
#include <QQuickWidget>
#include <QVariant>
#include <QVBoxLayout>
#include <QDebug>
#include <QLabel>

RobotAttitudeWidget::RobotAttitudeWidget(QWidget *parent)
    : QWidget(parent)
    , m_robotView(new QQuickWidget(this))
    , m_viewModel(new RobotViewModel(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_freshnessLabel = new QLabel(this);
    layout->addWidget(m_freshnessLabel);
    layout->addWidget(m_robotView);
    setDataFresh(false);

    m_robotView->setResizeMode(QQuickWidget::SizeRootObjectToView);

    auto bindViewModel = [this]() {
        if (QObject *root = m_robotView->rootObject()) {
            root->setProperty("viewModel", QVariant::fromValue(static_cast<QObject*>(m_viewModel)));
        }
    };

    connect(m_robotView, &QQuickWidget::statusChanged, this, [this](QQuickWidget::Status status) {
        if (status == QQuickWidget::Ready) {
            if (QObject *root = m_robotView->rootObject()) {
                root->setProperty("viewModel", QVariant::fromValue(static_cast<QObject*>(m_viewModel)));
            }
        }
        if (status == QQuickWidget::Error) {
            for (const auto &err : m_robotView->errors()) {
                qWarning() << "[RobotView]" << err.toString();
            }
        }
    });

    m_robotView->setSource(QUrl("qrc:/resources/qml/RobotView.qml"));
    bindViewModel();
}

void RobotAttitudeWidget::updateAttitude(double roll, double pitch, double yaw)
{
    m_viewModel->updateAttitude(roll, pitch, yaw);
}

void RobotAttitudeWidget::setDataFresh(bool fresh)
{
    m_freshnessLabel->setText(fresh ? QStringLiteral("姿态：实时观测") : QStringLiteral("姿态：未知/过期，图形仅保留最后观测"));
    m_freshnessLabel->setStyleSheet(fresh ? QStringLiteral("color: #16a34a;") : QStringLiteral("color: #d97706;"));
}

void RobotAttitudeWidget::updateLegs(double leg1, double leg2, double leg3, double leg4)
{
    m_viewModel->updateLegs(leg1, leg2, leg3, leg4);
}

void RobotAttitudeWidget::resetView()
{
    updateAttitude(0.0, 0.0, 0.0);
    updateLegs(0.0, 0.0, 0.0, 0.0);
}
