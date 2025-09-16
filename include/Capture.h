/**
*     madebyYahei
*    捕捉packet，生成视频音频packet队列，返回音视频param
*    QUEUE_DATA<AVPacketPtr>* m_VideopacketQueue = nullptr;
     QUEUE_DATA<AVPacketPtr>* m_AudiopacketQueue = nullptr;
 */
//// TODO:对这个类进行重构
#ifndef FFMPEGINPUT_H
#define FFMPEGINPUT_H
#include <QObject>
#include "AVSmartPtrs.h"
#include "ThreadSafeQueue.h"

extern "C"{
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

enum class MediaType {
    Video,
    Audio
};

class Capture : public QObject{
    Q_OBJECT
public:
    explicit Capture(QObject* parent = nullptr);
    ~Capture();


    void setPacketQueue(QUEUE_DATA<AVPacketPtr>* videoQueue, QUEUE_DATA<AVPacketPtr>* audioQueue);
signals:
    void deviceOpenSuccessfully(AVCodecParameters* vparams,AVCodecParameters* aparams);

    void errorOccurred(const QString &errorText);

public slots:
    void openDevice(const QString &videoDeviceName, const QString &audioDeviceName);
    void closeDevice();
    void startReading();
    void doReadFrame();
    void stopReading();
    void openAudio(const QString &audioDeviceName);
    void openVideo(const QString &videoDeviceName);
private:
    AVFormatContext* m_FormatCtx = nullptr;
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;

    AVCodecParameters* vParams = nullptr;
    AVCodecParameters* aParams = nullptr;

    std::atomic<bool> m_isReading = false;
    static void initializeFFmpeg();
    static int64_t startTime;
    QMutex m_queueMutex;  // 保护队列指针访问的互斥锁
    QUEUE_DATA<AVPacketPtr>* m_videoPacketQueue = nullptr;
    QUEUE_DATA<AVPacketPtr>* m_audioPacketQueue = nullptr;
};



#endif //FFMPEGINPUT_H
