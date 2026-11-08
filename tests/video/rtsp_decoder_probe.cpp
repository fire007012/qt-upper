#include <QtTest>
#include <climits>
#include "FfmpegRtspDecoder.h"

class RtspDecoderTest : public QObject {
    Q_OBJECT
private slots:
    void invalidDimensionsAndCancelledCallbacks() {
        FfmpegRtspDecoder decoder;
        QSignalSpy failures(&decoder,&FfmpegRtspDecoder::failed);
        connect(&decoder,&FfmpegRtspDecoder::failed,this,[](const QString &message){qWarning().noquote()<<message;});
        decoder.start("rtsp://127.0.0.1/unused",50000,50000,30);
        QCOMPARE(failures.size(),1); QVERIFY(!decoder.isRunning());
        decoder.start("rtsp://127.0.0.1/unused",INT_MAX,INT_MAX,30);
        QCOMPARE(failures.size(),2); QVERIFY(!decoder.isRunning());
        const auto original=qgetenv("HOSTCOMPUTER_FFMPEG_PATH");
        qputenv("HOSTCOMPUTER_FFMPEG_PATH","/host-test/nonexistent-ffmpeg");
        int delivered=0;
        connect(&decoder,&FfmpegRtspDecoder::failed,this,[&]{++delivered;});
        decoder.start("rtsp://127.0.0.1/unused",160,120,15);
        QTest::qSleep(100); // Let worker queue a failure without servicing the UI.
        decoder.stop(); QCoreApplication::processEvents();
        QCOMPARE(delivered,0); QVERIFY(!decoder.isRunning());
        if(original.isNull())qunsetenv("HOSTCOMPUTER_FFMPEG_PATH"); else qputenv("HOSTCOMPUTER_FFMPEG_PATH",original);
    }
    void actualRtspFramesAndRestart() {
        const auto url=qEnvironmentVariable("HOST_TEST_RTSP_URL"); QVERIFY(!url.isEmpty());
        FfmpegRtspDecoder decoder;
        QSignalSpy failures(&decoder,&FfmpegRtspDecoder::failed);
        for(int cycle=0;cycle<3;++cycle) {
            decoder.setFisheyeEnabled(cycle==1);
            decoder.start(url,160,120,15);
            QImage first,second;
            QTRY_VERIFY_WITH_TIMEOUT(!first.isNull() || decoder.takeLatestFrame(&first),10000);
            QCOMPARE(first.size(),QSize(160,120)); QCOMPARE(first.format(),QImage::Format_RGB888);
            bool changed=false;
            QTRY_VERIFY_WITH_TIMEOUT(([&]{if(!changed)changed=decoder.takeLatestFrame(&second)&&first!=second;return changed;})(),5000);
            QElapsedTimer stopped; stopped.start(); decoder.stop();
            QVERIFY(stopped.elapsed()<2000); QVERIFY(!decoder.isRunning());
            QVERIFY(!decoder.takeLatestFrame(&second));
        }
        QCOMPARE(failures.size(),0);
    }
};
QTEST_GUILESS_MAIN(RtspDecoderTest)
#include "rtsp_decoder_probe.moc"
