#ifndef CAPTURE_H
#define CAPTURE_H

#include <QObject>
#include "AVSmartPtrs.h"
#include "ThreadSafeQueue.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

enum class MediaType {
    Video,
    Audio
};

class Capture : public QObject {
    Q_OBJECT

public:
    explicit Capture(QUEUE_DATA<AVPacketPtr>* videoQueue, QUEUE_DATA<AVPacketPtr>* audioQueue, QObject *parent = nullptr);

    ~Capture();

private:
    static void initializeFFmpeg();
    AVFormatContext* m_VideoFormatCtx = nullptr;
    AVFormatContext* m_AudioFormatCtx = nullptr;

    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;

    AVCodecParameters *m_vParams = nullptr;
    AVCodecParameters *m_aParams = nullptr;
    AVRational m_vTimeBase = { 0, 1 };
    AVRational m_aTimeBase = { 0, 1 };


    std::atomic<bool> m_isVideoOpen = false; 
    std::atomic<bool> m_isAudioOpen = false; 
    QString m_videoDeviceName;
    QString m_audioDeviceName;

    std::atomic<bool> m_isReadingVideo = false;
    std::atomic<bool> m_isReadingAudio = false;

    int64_t m_videoStartTime = 0;
    int64_t m_audioStartTime = 0;
    // QMutex m_queueMutex;  // 保护队列指针访问的互斥锁
    QUEUE_DATA<AVPacketPtr> *m_videoPacketQueue = nullptr;
    QUEUE_DATA<AVPacketPtr> *m_audioPacketQueue = nullptr;

    // --- 两个独立的线程同步 ---
    QMutex m_videoWorkMutex;
    QWaitCondition m_videoWorkCond;
    std::atomic<bool> m_isDoingVideoWork = false;

    QMutex m_audioWorkMutex;
    QWaitCondition m_audioWorkCond;
    std::atomic<bool> m_isDoingAudioWork = false;

    void startVideoReading();
    void startAudioReading();
    void stopVideoReading();
    void stopAudioReading();

signals:
    void deviceOpenSuccessfully(AVCodecParameters *vparams, AVCodecParameters *aparams, AVRational vTimeBase, AVRational aTimeBase);
    void errorOccurred(const QString &errorText);
    void audioDeviceOpenSuccessfully(AVCodecParameters *aParams,AVRational m_aTimeBase);
    void videoDeviceOpenSuccessfully(AVCodecParameters *m_vParams,AVRational m_vTimeBase);
public slots:
    void closeDevice();

    void configReadingStatus(bool openVideo, bool openAudio);
    void doReadVideoFrame();
    void doReadAudioFrame();

    void stopReading();

    void openAudio(const QString &audioDeviceName);
    void openVideo(const QString &videoDeviceName);

    void closeAudio();
    void closeVideo();
};


#endif //CAPTURE_H
