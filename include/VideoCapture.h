/**
*     madebyYahei
*    捕捉packet，生成视频音频packet队列，返回音视频param
*    QUEUE_DATA<AVPacketPtr>* m_VideopacketQueue = nullptr;
     QUEUE_DATA<AVPacketPtr>* m_AudiopacketQueue = nullptr;
 */

#ifndef FFMPEGINPUT_H
#define FFMPEGINPUT_H
#include <QObject>
#include "AVSmartPtrs.h"
#include "ThreadSafeQueue.h"

extern "C"{
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

enum class MediaType {
    Video,
    Audio
};

class VideoCapture : public QObject{
    Q_OBJECT
public:
    explicit VideoCapture(QObject* parent = nullptr);
    ~VideoCapture();

    void setPacketQueue(QUEUE_DATA<AVPacketPtr>* videoQueue, QUEUE_DATA<AVPacketPtr>* audioQueue);
    AVCodecParameters* getVideoCodecParameters();
    AVCodecParameters* getAudioCodecParameters();

    void closeDevice();
signals:
    void deviceOpenSuccessfully(AVCodecParameters* vparams,AVCodecParameters* aParams);

    void errorOccurred(const QString &errorText);

public slots:
    void openDevice(const QString &videoDeviceName, const QString &audioDeviceName);

    void startReading();
    void stopReading();
private:
    AVFormatContext* m_FormatCtx = nullptr;
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;

    volatile bool m_isReading = false;
    static void initializeFFmpeg();

    QUEUE_DATA<AVPacketPtr>* m_videoPacketQueue = nullptr;
    QUEUE_DATA<AVPacketPtr>* m_audioPacketQueue = nullptr;
};



#endif //FFMPEGINPUT_H
