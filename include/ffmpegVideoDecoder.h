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
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
class ffmpegVideoDecoder : public QObject{
    Q_OBJECT
public:
    explicit ffmpegVideoDecoder(QUEUE_DATA<AVPacketPtr>* packetQueue,QUEUE_DATA<std::unique_ptr<QImage>>*imageQueue,QUEUE_DATA<AVFramePtr>* frameQueue,QObject *parent = nullptr);
    ~ffmpegVideoDecoder();

private:
    void clear();
    QUEUE_DATA<AVPacketPtr>* m_packetQueue;//采集队列
    QUEUE_DATA<std::unique_ptr<QImage>>* m_QimageQueue;//QT显示队列
    QUEUE_DATA<AVFramePtr>* m_frameQueue;//网络传输帧队列

    uint8_t* rgbBuffer = nullptr;
    std::atomic<bool> m_isDecoding = false;

    AVCodecContext* m_codecCtx = nullptr;
    const AVCodec* m_codec = nullptr;
    SwsContext* m_swsCtx = nullptr;
    AVFramePtr m_rgbFrame = nullptr;
    // AVFramePtr m_decodedFrame = nullptr;

signals:
    void newFrameAvailable();
    void errorOccurred(const QString &errorText);
public slots:
    bool init(AVCodecParameters* params);

    void startDecoding();
    void doDecodingPacket();
    void stopDecoding();
};



#endif //FFMPEGDECODER_H
