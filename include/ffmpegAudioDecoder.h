/**
 *madebyYahei
 *设计音频解码，以及重采样逻辑
 */
#ifndef FFMPEGAUDIODECODER_H
#define FFMPEGAUDIODECODER_H
#include <QObject>
#include <QtMultimedia//QAudioSink>
#include <QIODevice>
#include <QMutex>
#include <QWaitCondition>
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"
#include "AudioResampleConfig.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h> // 音频重采样库
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>
}


class ffmpegAudioDecoder : public QObject {
    Q_OBJECT

public:
    explicit ffmpegAudioDecoder(QUEUE_DATA<AVPacketPtr> *packetQueue, QUEUE_DATA<AVFramePtr> *frameQueue,
                                QObject *parent = nullptr);

    ~ffmpegAudioDecoder();

private:
    void clear();

    QUEUE_DATA<AVPacketPtr> *m_packetQueue;
    QUEUE_DATA<AVFramePtr> *m_frameQueue;
    std::atomic<bool> m_isDecoding = false;
    std::atomic<bool> m_isConfigReady = false;

    // FFmpeg-related members
    AVCodecContext *m_codecCtx = nullptr;
    const AVCodec *m_codec = nullptr;
    SwrContext *m_swrCtx = nullptr; // 用于音频重采样
    AVAudioFifo *m_fifo = nullptr;
    AVCodecContext *m_encoderCtx = nullptr;
    int64_t m_fifoBasePts = AV_NOPTS_VALUE;
	AVRational m_inputTimeBase;
    AudioResampleConfig m_ResampleConfig;

    // --- 线程同步 ---
    QMutex m_workMutex;
    QWaitCondition m_workCond;
    bool m_isDoingWork = false;

    // Qt Audiosink test
    // QAudioSink* m_audioSink = nullptr;
    // QIODevice* m_audioDevice = nullptr; // 音频输出设备
public slots:
    bool init(AVCodecParameters *params, AVRational inputTimeBase);

    void startDecoding();

    void stopDecoding();

    void setResampleConfig(const AudioResampleConfig &config);

    void doDecodingPacket();

	void ChangeDecodingState(bool isEncoding);

signals:
    void errorOccurred(const QString &errorText);
};


#endif //FFMPEGAUDIODECODER_H
