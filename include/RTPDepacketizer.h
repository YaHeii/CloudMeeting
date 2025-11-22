#ifndef RTPDEPACKETIZER_H
#define RTPDEPACKETIZER_H


#include "rtp_jitter.h" // 引入库头文件
#include <QObject>
#include <QTimer>
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"

extern "C" {
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
}

class RTPDepacketizer : public QObject {
    Q_OBJECT
public:
    // sample_rate: 视频 90000, 音频 48000
    RTPDepacketizer(int sampleRate, QUEUE_DATA<AVPacketPtr>* outputQueue, bool isH264, QObject* parent = nullptr);

    ~RTPDepacketizer();
    // 【生产者】网络线程调用：推入数据
    void pushPacket(const uint8_t* data, size_t len);

public slots:
    // 【消费者】定时器调用：取出数据并组帧
    void processPop();

private:
    bool m_isH264;
    RTPJitter* m_jitterBuffer;
    QTimer* m_consumerTimer;
    QUEUE_DATA<AVPacketPtr>* m_outputQueue;

    // H.264 组帧状态
    std::vector<uint8_t> m_fuBuffer;
    bool m_isReassembling = false;

    void resetH264Assembler();

    void reassembleH264(const std::vector<uint8_t>& payload, uint32_t timestamp);
};

#endif