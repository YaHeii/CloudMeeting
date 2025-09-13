/**
 *madebyYahei
 *设计音频解码，以及重采样逻辑
 */
#ifndef FFMPEGAUDIODECODER_H
#define FFMPEGAUDIODECODER_H
#include <QObject>
#include <QtMultimedia//QAudioSink>
#include <QIODevice>
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h> // 音频重采样库
#include <libavutil/opt.h>
}


class ffmpegAudioDecoder : public QObject {
    Q_OBJECT
public:
    explicit ffmpegAudioDecoder(QUEUE_DATA<AVPacketPtr>* packetQueue,QUEUE_DATA<AVFramePtr>* frameQueue, QObject *parent = nullptr);
    ~ffmpegAudioDecoder();
public slots:

bool init(AVCodecParameters* params);
    void startDecoding();
    void stopDecoding();
private:
    void clear();

    QUEUE_DATA<AVPacketPtr>* m_packetQueue;
    QUEUE_DATA<AVFramePtr>* m_frameQueue;
    std::atomic<bool> m_isDecoding = false;

    // FFmpeg-related members
    AVCodecContext* m_codecCtx = nullptr;
    const AVCodec* m_codec = nullptr;
    SwrContext* m_swrCtx = nullptr; // 用于音频重采样

    // Qt Audiosink test
    // QAudioSink* m_audioSink = nullptr;
    QIODevice* m_audioDevice = nullptr; // 音频输出设备
};



#endif //FFMPEGAUDIODECODER_H
