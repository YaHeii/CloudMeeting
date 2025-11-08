//
// Created by lenovo on 25-9-13.
//

#ifndef FFMPEGENCODER_H
#define FFMPEGENCODER_H

#include <QObject>
#include <QMutex>
#include <QWaitCondition>
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"
#include "AudioResampleConfig.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

class ffmpegEncoder : public QObject {
    Q_OBJECT

public:
    explicit ffmpegEncoder(QUEUE_DATA<AVFramePtr> *frameQueue, QUEUE_DATA<AVPacketPtr> *packetQueue,
                           QObject *parent = nullptr);

    ~ffmpegEncoder();

    AVCodecContext *getCodecContext() const { return m_codecCtx; }

private:
    void clear();

    void flushEncoder(); // 清空编码器缓存

    QUEUE_DATA<AVFramePtr> *m_frameQueue;
    QUEUE_DATA<AVPacketPtr> *m_packetQueue;

    std::atomic<bool> m_isEncoding = false;

    AVCodecContext *m_codecCtx = nullptr;
    AVMediaType m_mediaType;

    // Keep counters for assigning PTS in encoder time_base
    int64_t m_videoFrameCounter =0;
    int64_t m_audioSamplesCount =0;

    // --- 用于线程同步 ---
    QMutex m_workMutex;
    QWaitCondition m_workCond;
    bool m_isDoingWork = false;
signals:
    void audioEncoderReady(const AudioResampleConfig &config);

    void errorOccurred(const QString &errorText);

    void encoderInitialized(AVCodecContext *codecCtx);

    void initializationSuccess();

public slots:
    bool initVideoEncoderH264(AVCodecParameters *vparams);

    bool initAudioEncoderAAC(AVCodecParameters *aparams);
    bool initAudioEncoderOpus(AVCodecParameters* aparams);

    void startEncoding();

    void stopEncoding();

    void doEncodingWork();
};


#endif //FFMPEGENCODER_H
