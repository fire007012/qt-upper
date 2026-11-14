#include <QtTest>
#include <QPointer>
#include "RtspPlayerWidget.h"
#include "FfmpegRtspDecoder.h"

class RtspWidgetTest : public QObject
{
    Q_OBJECT
private slots:
    void destroyIdleAndStoppedPlayers()
    {
        for (bool explicitlyStopped : {false, true}) {
            auto *player = new RtspPlayerWidget;
            QPointer<FfmpegRtspDecoder> decoder = player->findChild<FfmpegRtspDecoder *>();
            QVERIFY(decoder);
            if (explicitlyStopped) player->stopVideo();
            // The decoder emits stopped() during child destruction. Qt debug
            // builds assert if it still calls a slot on the destroyed widget.
            delete player;
            QVERIFY(decoder.isNull());
            QCoreApplication::processEvents();
        }
    }
};
QTEST_MAIN(RtspWidgetTest)
#include "rtsp_widget_test.moc"
