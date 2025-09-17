/**
 *实现对packet的协议封装并发送至SRS服务器
 *madebyYahei
 */
#ifndef RTMPPUBLISHER_H
#define RTMPPUBLISHER_H

#include <QObject>
#include <QString>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

class RtmpPublisher : public QObject{
    Q_OBJECT
public:
    explicit RtmpPublisher(QUEUE_DATA<AVPacketPtr>* encodedPacketQueue,QObject *parent = nullptr);
    ~RtmpPublisher();
    void clear();
private:
    QUEUE_DATA<AVPacketPtr>* m_encodedPacketQueue; // 编码后数据包的输入队列

    std::atomic<bool> m_isPublishing = false;

    AVFormatContext* m_outputFmtCtx = nullptr;
    AVStream* m_videoStream = nullptr;
    AVStream* m_audioStream = nullptr;

    // 保存编码器的时间基，用于正确的PTS/DTS转换
    AVRational m_videoEncoderTimeBase;
    AVRational m_audioEncoderTimeBase;
    // 线程同步
    QMutex m_workMutex;
    QWaitCondition m_workCond;
    bool m_isDoingWork = false;

signals:
    void errorOccurred(const QString& errorText);
    void publisherStarted();
    void publisherStopped();
public slots:
    void startPublishing();
    void stopPublishing();
    void doPublishingWork();
    bool init(const QString& rtmpUrl, AVCodecContext* vCodecCtx, AVCodecContext* aCodecCtx);

};



#endif //RTMPPUBLISHER_H
