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
#include "AudioResampleConfig.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>
}

class AudioPlayer : public QObject {
    Q_OBJECT

public:
    explicit AudioPlayer(QUEUE_DATA<AVPacketPtr>* packetQueue,
        QObject* parent = nullptr);
    ~AudioPlayer();

signals:
    void errorOccurred(const QString& errorText);

public slots:
    void setTargetDeviceName(QString& name) { m_audioDeviceName = name; }
    bool init(AVCodecParameters* params, AVRational inputTimeBase);
    void startPlaying();
    void stopPlaying();
    void ChangeDecodingState(bool isDecoding);
private slots:
    void doDecodingWork();

private:
    void clear();
    bool initAudioOutput(AVFrame* frame);

    QUEUE_DATA<AVPacketPtr>* m_packetQueue;
    QUEUE_DATA<AVFramePtr>* m_frameQueue;
    std::atomic<bool> m_isDecoding = { false };
    std::atomic<bool> m_isConfigReady = false;

    AVCodecContext* m_codecCtx = nullptr;
    SwrContext* m_swrCtx = nullptr;
    AVAudioFifo* m_fifo = nullptr;
    AVRational m_inputTimeBase;
    int64_t m_frameBasePts = AV_NOPTS_VALUE;
    int64_t m_fifoBasePts = AV_NOPTS_VALUE;
    AudioResampleConfig m_ResampleConfig;

    QAudioSink* m_audioSink = nullptr;
    QIODevice* m_audioDevice = nullptr;
    QString m_audioDeviceName = nullptr;
    QAudioDevice findDeviceByName(QString& name); // ¸¨Öúº¯Êý
    QIODevice* m_audioIO = nullptr;
    uint8_t** m_resampledData = nullptr;
    int m_resampledDataSize = 0;
    int m_resampledLinesize = 0;


    QMutex m_workMutex;
    QWaitCondition m_workCond;
    std::atomic<bool> m_isDoingWork = { false };
};

#endif // RTMPAUDIOPLAYER_H