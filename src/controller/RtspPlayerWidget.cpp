#include "RtspPlayerWidget.h"

#include "FfmpegRtspDecoder.h"
#include "VideoFrameWidget.h"

#include <QAction>
#include <QTime>

namespace {

QString overlayLabelTextForCamera(int cameraId)
{
    if (cameraId == 3) {
        return QStringLiteral("前摄像头");
    }
    return QString();
}

} // namespace

RtspPlayerWidget::RtspPlayerWidget(int cameraId, QWidget *parent)
    : QWidget(parent)
    , m_cameraId(cameraId)
{
    // 播放器
    m_decoder = new FfmpegRtspDecoder(this);
    m_videoWidget = new VideoFrameWidget(this);
    m_videoWidget->hide();
    m_videoWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 等待标签
    m_waitLabel = new QLabel("等待视频输入...", this);
    m_waitLabel->setAlignment(Qt::AlignCenter);
    m_waitLabel->setStyleSheet("color: #cccccc; font-size: 16px;");
    m_waitLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 标题
    m_titleLabel = new QLabel(QString("摄像头 %1").arg(cameraId + 1), this);
    m_titleLabel->setStyleSheet("color: #ffffff; font-size: 13px; font-weight: bold;");

    // 摄像头信息标签（显示分辨率、编码等）
    m_infoLabel = new QLabel("等待下位机推送...", this);
    m_infoLabel->setStyleSheet("color: #999999; font-size: 11px;");

    // 本地时钟标签 — 叠加在视频右下角，用于和视频帧内时间戳对比测延迟
    m_localClockLabel = new QLabel(this);
    m_localClockLabel->setStyleSheet(
        "color: #00ff00; font-size: 18px; font-weight: bold; font-family: 'Consolas', 'Mono';"
        "background-color: rgba(0, 0, 0, 180); padding: 2px 6px; border-radius: 3px;");
    m_localClockLabel->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    const QString overlayText = overlayLabelTextForCamera(m_cameraId);
    m_localClockLabel->setText(
        overlayText.isEmpty() ? QStringLiteral("LOCAL --:--:--.---") : overlayText);
    m_localClockLabel->hide();

    // 时钟刷新定时器（10ms 刷新一次，精确到毫秒级）
    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(10);
    connect(m_clockTimer, &QTimer::timeout, this, &RtspPlayerWidget::updateLocalClock);

    m_frameTimer = new QTimer(this);
    m_frameTimer->setInterval(30);
    connect(m_frameTimer, &QTimer::timeout, this, &RtspPlayerWidget::updateVideoFrame);

    // 按钮
    m_startBtn = new QPushButton("启动", this);
    m_stopBtn = new QPushButton("停止", this);
    m_startBtn->setEnabled(false);
    m_stopBtn->setEnabled(false);
    m_startBtn->setMinimumHeight(24);
    m_stopBtn->setMinimumHeight(24);

    // 状态标签
    m_statusLabel = new QLabel("离线", this);
    m_statusLabel->setStyleSheet("color: #ff4444; font-size: 12px;");

    // 顶栏: 标题 + 信息
    QHBoxLayout *topLayout = new QHBoxLayout();
    topLayout->addWidget(m_titleLabel);
    topLayout->addWidget(m_infoLabel, 1);
    topLayout->setContentsMargins(4, 2, 4, 0);

    // 底栏: 按钮 + 状态
    QHBoxLayout *btnLayout = new QHBoxLayout();
    btnLayout->addWidget(m_startBtn);
    btnLayout->addWidget(m_stopBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_statusLabel);
    btnLayout->setContentsMargins(4, 0, 4, 2);

    // 主布局
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(2);
    mainLayout->addLayout(topLayout);
    mainLayout->addWidget(m_waitLabel, 1);
    mainLayout->addWidget(m_videoWidget, 1);
    mainLayout->addLayout(btnLayout);

    // 样式
    setStyleSheet(
        "RtspPlayerWidget { background-color: #1a1a1a; border: 1px solid #333; border-radius: 4px; }"
        "QPushButton { background-color: #2d2d2d; color: #ffffff; border: none; "
        "border-radius: 3px; padding: 4px 12px; font-size: 12px; }"
        "QPushButton:hover { background-color: #3d3d3d; }"
        "QPushButton:disabled { background-color: #252525; color: #666666; }"
        "QLabel { background-color: transparent; }");

    connect(m_startBtn, &QPushButton::clicked, this, &RtspPlayerWidget::startVideo);
    connect(m_stopBtn, &QPushButton::clicked, this, &RtspPlayerWidget::stopVideo);
    connect(m_decoder, &FfmpegRtspDecoder::started,
            this, &RtspPlayerWidget::onDecoderStarted);
    connect(m_decoder, &FfmpegRtspDecoder::stopped,
            this, &RtspPlayerWidget::onDecoderStopped);
    connect(m_decoder, &FfmpegRtspDecoder::failed,
            this, &RtspPlayerWidget::onDecoderFailed);

    // 右键菜单
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested,
            this, &RtspPlayerWidget::showContextMenu);
}

RtspPlayerWidget::~RtspPlayerWidget()
{
    m_clockTimer->stop();
    m_frameTimer->stop();
    // The child decoder calls stop() again from its destructor, after this
    // derived widget has been destroyed. Do not leave callbacks to its slots.
    disconnect(m_decoder, nullptr, this, nullptr);
    m_decoder->stop();
}

void RtspPlayerWidget::setCameraInfo(const QString &rtspUrl, bool online,
                                      const QString &codec, int width, int height,
                                      int fps, int bitrateKbps)
{
    const bool streamChanged = m_rtspUrl != rtspUrl
        || m_streamWidth != width
        || m_streamHeight != height;

    m_rtspUrl = rtspUrl;
    m_streamWidth = width;
    m_streamHeight = height;
    m_streamFps = fps;
    m_streamOnline = online;

    // 更新信息标签
    m_infoLabel->setText(QString("%1x%2 %3 %4fps %5kbps")
                         .arg(width).arg(height).arg(codec).arg(fps).arg(bitrateKbps));

    if (online && !rtspUrl.isEmpty()) {
        m_statusLabel->setText("在线");
        m_statusLabel->setStyleSheet("color: #00cc66; font-size: 12px;");
        m_startBtn->setEnabled(true);
        if (streamChanged || !m_decoder->isRunning()) {
            startVideo();
        }
    } else {
        m_statusLabel->setText("离线");
        m_statusLabel->setStyleSheet("color: #ff4444; font-size: 12px;");
        m_startBtn->setEnabled(false);
        stopVideo();
    }
}

void RtspPlayerWidget::startVideo()
{
    if (m_rtspUrl.isEmpty()) {
        return;
    }
    if (m_streamWidth <= 0 || m_streamHeight <= 0) {
        onDecoderFailed(QString("视频尺寸无效: %1x%2").arg(m_streamWidth).arg(m_streamHeight));
        return;
    }

    m_waitLabel->setText("连接视频...");
    m_decoder->setFisheyeEnabled(m_fisheyeEnabled);
    m_decoder->start(m_rtspUrl, m_streamWidth, m_streamHeight, m_streamFps);

    m_waitLabel->hide();
    m_videoWidget->show();
    m_startBtn->setEnabled(false);
    m_stopBtn->setEnabled(true);
    m_statusLabel->setText("播放中");
    m_statusLabel->setStyleSheet("color: #00cc66; font-size: 12px;");

    // 启动本地时钟叠加，用于延迟测量
    m_localClockLabel->show();
    m_localClockLabel->raise();  // 确保在视频之上
    m_clockTimer->start();
}

void RtspPlayerWidget::stopVideo()
{
    m_decoder->stop();
    m_frameTimer->stop();
    m_videoWidget->clearFrame();

    m_videoWidget->hide();
    m_waitLabel->show();
    m_waitLabel->setText("等待视频输入...");
    m_stopBtn->setEnabled(false);
    if (!m_rtspUrl.isEmpty()) {
        m_startBtn->setEnabled(true);
    }
    m_statusLabel->setText("已停止");
    m_statusLabel->setStyleSheet("color: #ffaa00; font-size: 12px;");

    // 停止时钟
    m_clockTimer->stop();
    m_localClockLabel->hide();
}

void RtspPlayerWidget::updateVideoFrame()
{
    QImage frame;
    if (m_decoder->takeLatestFrame(&frame)) {
        m_videoWidget->setFrame(frame);
    }
}

void RtspPlayerWidget::onDecoderStarted()
{
    m_waitLabel->hide();
    m_videoWidget->show();
    m_startBtn->setEnabled(false);
    m_stopBtn->setEnabled(true);
    m_statusLabel->setText("低延迟播放中");
    m_statusLabel->setStyleSheet("color: #00cc66; font-size: 12px;");
    m_frameTimer->start();
}

void RtspPlayerWidget::onDecoderStopped()
{
    if (m_streamOnline && !m_rtspUrl.isEmpty()) {
        m_startBtn->setEnabled(true);
    }
    m_stopBtn->setEnabled(false);
    m_frameTimer->stop();
}

void RtspPlayerWidget::onDecoderFailed(const QString &message)
{
    Q_UNUSED(message)
    m_videoWidget->hide();
    m_waitLabel->show();
    m_waitLabel->setText(QStringLiteral("视频暂不可用"));
    m_startBtn->setEnabled(m_streamOnline && !m_rtspUrl.isEmpty());
    m_stopBtn->setEnabled(false);
    m_statusLabel->setText("播放错误");
    m_statusLabel->setStyleSheet("color: #ff4444; font-size: 12px;");
    m_clockTimer->stop();
    m_frameTimer->stop();
    m_localClockLabel->hide();
}

void RtspPlayerWidget::updateLocalClock()
{
    const QString overlayText = overlayLabelTextForCamera(m_cameraId);
    if (!overlayText.isEmpty()) {
        m_localClockLabel->setText(overlayText);
    } else {
        // 显示本地时间精确到毫秒，和视频帧中的时间戳对比即可测出延迟
        QTime now = QTime::currentTime();
        m_localClockLabel->setText(QString("LOCAL %1").arg(now.toString("HH:mm:ss.zzz")));
    }

    // 定位到视频区域右下角
    if (m_videoWidget->isVisible()) {
        int lw = m_localClockLabel->sizeHint().width();
        int lh = m_localClockLabel->sizeHint().height();
        int x = m_videoWidget->x() + m_videoWidget->width() - lw - 8;
        int y = m_videoWidget->y() + m_videoWidget->height() - lh - 8;
        m_localClockLabel->move(x, y);
    }
}

void RtspPlayerWidget::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);

    QAction *fisheyeAction = menu.addAction(QStringLiteral("鱼眼矫正"));
    fisheyeAction->setCheckable(true);
    fisheyeAction->setChecked(m_fisheyeEnabled);

    QAction *chosen = menu.exec(mapToGlobal(pos));
    if (chosen == fisheyeAction) {
        m_fisheyeEnabled = fisheyeAction->isChecked();
        m_decoder->setFisheyeEnabled(m_fisheyeEnabled);

        // 如果正在播放，重启解码器以应用新滤镜
        if (m_decoder->isRunning()) {
            stopVideo();
            startVideo();
        }
    }
}
