#ifndef RTMPAUDIOPLAYER_H
#define RTMPAUDIOPLAYER_H

#include <QObject>
#include <QMutex>
#include <QWaitCondition>
#include <QAudioSink>
#include <QIODevice>
#include <memory>
#include <atomic>
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>
}

class RtmpAudioPlayer : public QObject {
    Q_OBJECT

public:
    explicit RtmpAudioPlayer(std::shared_ptr<QUEUE_DATA<AVPacketPtr>> packetQueue,
        QObject* parent = nullptr);
    ~RtmpAudioPlayer();

signals:
    void errorOccurred(const QString& errorText);

public slots:
    // 由 RtmpPuller 在 streamOpened 信号中调用
    bool init(AVCodecParameters* params, AVRational inputTimeBase);
    void startPlaying();
    void stopPlaying();

private slots:
    void doDecodingWork();

private:
    void cleanup();
    bool initAudioOutput(AVFrame* frame); // 初始化 QAudioSink

    std::shared_ptr<QUEUE_DATA<AVPacketPtr>> m_packetQueue;
    std::atomic<bool> m_isDecoding = { false };

    AVCodecContext* m_codecCtx = nullptr;
    SwrContext* m_swrCtx = nullptr;
    AVRational m_inputTimeBase;

    // QAudioSink 用于播放
    QAudioSink* m_audioSink = nullptr;
    QIODevice* m_audioDevice = nullptr;

    // 重采样缓冲区
    uint8_t** m_resampledData = nullptr;
    int m_resampledDataSize = 0;
    int m_resampledLinesize = 0;

    // 线程同步
    QMutex m_workMutex;
    QWaitCondition m_workCond;
    std::atomic<bool> m_isDoingWork = { false };
};

#endif // RTMPAUDIOPLAYER_H