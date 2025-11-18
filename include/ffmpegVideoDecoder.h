/**
 *madebyYahei
 *对传入的packetQUEUE进行解码完成渲染等操作
 *这里不使用摄像头传输的原始数据，而是FFmpeg的封装packet，
 *保证在远端视频解码的代码可复用性
 */

#ifndef FFMPEGDECODER_H
#define FFMPEGDECODER_H

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QWaitCondition>
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

class ffmpegVideoDecoder : public QObject {
    Q_OBJECT

public:
    explicit ffmpegVideoDecoder(QUEUE_DATA<AVPacketPtr> *packetQueue, QUEUE_DATA<std::unique_ptr<QImage> > *imageQueue,
                                QUEUE_DATA<AVFramePtr> *frameQueue, QObject *parent = nullptr);

    ~ffmpegVideoDecoder();

private:
    void clear();

    QUEUE_DATA<AVPacketPtr> *m_packetQueue; //采集队列
    QUEUE_DATA<std::unique_ptr<QImage> > *m_QimageQueue; //QT显示队列
    QUEUE_DATA<AVFramePtr> *m_frameQueue; //网络传输帧队列

    uint8_t *rgbBuffer = nullptr;
    std::atomic<bool> m_isDecoding = false;

    AVCodecContext *m_codecCtx = nullptr;
    const AVCodec *m_codec = nullptr;
    SwsContext *m_swsCtx = nullptr;
    AVFramePtr m_rgbFrame = nullptr;
    // AVFramePtr m_decodedFrame = nullptr;

    // 解决竞态条件问题
    QMutex m_workMutex;
    QWaitCondition m_workCond;
    bool m_isDoingWork = false; // 标志是否正在执行解码核心工作

    // 解决视频参数动态变化问题
    int m_swsSrcWidth = 0;
    int m_swsSrcHeight = 0;
    AVPixelFormat m_swsSrcPixFmt = AV_PIX_FMT_NONE;

    // base pts for video frames (matches audio's m_fifoBasePts logic)
    int64_t m_frameBasePts = AV_NOPTS_VALUE;
	AVRational m_inputTimeBase;

signals:
    void newFrameAvailable();

    void errorOccurred(const QString &errorText);

public slots:
    bool init(AVCodecParameters *params, AVRational inputTimeBase);

    void startDecoding();

    void doDecodingPacket();

    void stopDecoding();
    
    void ChangeDecodingState(bool isEncoding);
};


#endif //FFMPEGDECODER_H
