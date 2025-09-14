//
// Created by lenovo on 25-9-13.
//

#ifndef FFMPEGENCODER_H
#define FFMPEGENCODER_H

#include <QObject>
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"
#include "AudioResampleConfig.h"

extern "C"{
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

class ffmpegEncoder :public QObject{
    Q_OBJECT
public:
    explicit ffmpegEncoder(QUEUE_DATA<AVFramePtr>* frameQueue,QUEUE_DATA<AVPacketPtr>* packetQueue,QObject* parent = nullptr);
    ~ffmpegEncoder();
private:
    void clear();
    void encodingLoop();

    QUEUE_DATA<AVFramePtr>* m_frameQueue;
    QUEUE_DATA<AVPacketPtr>* m_packetQueue;

    std::atomic<bool> m_isEncoding = false;

    AVCodecContext* m_codecCtx = nullptr;
    AVMediaType m_mediaType;
signals:
    void audioEncoderReady(const AudioResampleConfig& config);
    void errorOccurred(const QString& errorText);
    void encoderInitialized(AVCodecContext* codecCtx);
    // void initializationSuccess();
public slots:
    bool initVideoEncoder(AVCodecParameters* vparams);
    bool initAudioEncoder(AVCodecParameters* aparams);

    void startEncoding();
    void stopEncoding();
};



#endif //FFMPEGENCODER_H
