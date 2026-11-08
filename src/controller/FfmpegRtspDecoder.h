#ifndef FFMPEGRTSPDECODER_H
#define FFMPEGRTSPDECODER_H

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QThread>

#include <atomic>

class FfmpegRtspDecoder : public QObject
{
    Q_OBJECT

public:
    explicit FfmpegRtspDecoder(QObject *parent = nullptr);
    ~FfmpegRtspDecoder() override;

    bool isRunning() const;
    void setFisheyeEnabled(bool enabled);
    bool fisheyeEnabled() const;
    void start(const QString &rtspUrl, int width, int height, int fps);
    void stop();
    bool takeLatestFrame(QImage *frame);

signals:
    void started();
    void stopped();
    void failed(const QString &message);

private:
    QString ffmpegProgram() const;
    QStringList ffmpegArguments(const QString &rtspUrl) const;
    void decoderLoop(const QString &program, const QStringList &arguments,
                     int width, int height, int frameBytes, quint64 generation);
    void postStatus(bool started, const QString &message, quint64 generation);
    void storeLatestFrame(const QByteArray &rawFrame, int width, int height);
    static QString stderrTail(const QString &stderrBuffer);

    QThread *m_workerThread = nullptr;
    std::atomic_bool m_stopRequested = false;
    std::atomic<quint64> m_generation = 0;
    mutable QMutex m_frameMutex;
    QImage m_latestFrame;
    bool m_hasNewFrame = false;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;
    int m_frameBytes = 0;
    bool m_fisheyeEnabled = false;
};

#endif // FFMPEGRTSPDECODER_H
