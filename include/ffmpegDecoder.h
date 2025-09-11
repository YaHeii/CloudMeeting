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

class ffmpegDecoder : public QObject{
    Q_OBJECT
public:
    explicit ffmpegDecoder(QUEUE_DATA<AVPacketPtr>* packetQueue,QUEUE_DATA<std::unique_ptr<QImage>>*frameQueue,QObject *parent = nullptr);
    ~ffmpegDecoder();
signals:
    void newFrameAvailable();
public slots:
    bool init(AVCodecParameters* params);

    void startDecoding();

    void stopDecoding();
private:
    void clear();
    QUEUE_DATA<AVPacketPtr>* m_packetQueue;
    QUEUE_DATA<std::unique_ptr<QImage>>* m_frameQueue;

    volatile bool m_isDecoding = false;

    AVCodecContext* m_codecCtx = nullptr;
    const AVCodec* m_codec = nullptr;
    SwsContext* m_swsCtx = nullptr;
};



#endif //FFMPEGDECODER_H
