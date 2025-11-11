#ifndef RTMPPULLER_H
#define RTMPPULLER_H
#include <QObject>
#include <QString>
#include <QWaitCondition>
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"
#include "RtmpAudioPlayer.h"
#include "ffmpegVideoDecoder.h"
#include "netheader.h"
#include <QMessageBox>
#include "AudioResampleConfig.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
}
class RtmpPuller : public QObject {
    Q_OBJECT

public:
    explicit RtmpPuller(QString rtmpPullerLink, QUEUE_DATA<std::unique_ptr<QImage> >* MainQimageQueue,QObject* parent = nullptr);

    ~RtmpPuller();

    void startPulling();
    
    void stopPulling();
private:
    bool initialize();
    void clear();
    void doPullingWork();

    QUEUE_DATA<std::unique_ptr<QImage> >* m_MainQimageQueue;
    QUEUE_DATA<AVPacketPtr>* m_videoPacketQueue;
	QUEUE_DATA<AVPacketPtr>* m_audioPacketQueue;
	QUEUE_DATA<AVFramePtr>* m_dummyVideoFrameQueue;

    AVFormatContext* m_fmtCtx = nullptr;
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;

    QString m_rtmpPullerLink;
    std::atomic<bool> m_isPulling = false;
    AVCodecContext* m_codecCtx = nullptr;

    QThread* m_videoDecodeThread = nullptr;
    ffmpegVideoDecoder* m_videoDecoder = nullptr;
    QThread* m_audioPlayThread = nullptr;
    RtmpAudioPlayer* m_audioPlayer = nullptr;

signals:
    void errorOccurred(const QString& errorText);
    void streamOpened(AVCodecParameters* vParams, AVCodecParameters* aParams,
        AVRational vTimeBase, AVRational aTimeBase);
    void streamClosed();

public slots:
	void ChangePullingState(bool isDecoding);

    void onStreamOpened_initVideo(AVCodecParameters* vParams, AVCodecParameters* aParams, 
                                  AVRational vTimeBase, AVRational aTimeBase);

    void onStreamOpened_initAudio(AVCodecParameters* vParams, AVCodecParameters* aParams, 
                                  AVRational vTimeBase, AVRational aTimeBase);
};

#endif // RTMPPULLER_H
