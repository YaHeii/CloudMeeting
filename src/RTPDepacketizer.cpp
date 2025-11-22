#include "RTPDepacketizer.h"
#include <winsock2.h>
extern "C" {
#include <libavcodec/avcodec.h>
}

RTPDepacketizer::RTPDepacketizer(int sampleRate, QUEUE_DATA<AVPacketPtr>* outputQueue, bool isH264, QObject* parent)
    : QObject(parent), m_outputQueue(outputQueue),m_isH264(isH264)
{
    // 设定缓冲深度，例如 200ms。这决定了抗抖动能力和延迟。
    m_jitterBuffer = new RTPJitter(200, sampleRate);

    // 启动一个定时器来驱动数据 "流出"
    // Jitter Buffer 的核心是 "延时输出"，所以需要定时去取
    m_consumerTimer = new QTimer(this);

    connect(m_consumerTimer, &QTimer::timeout, this, &RTPDepacketizer::processPop);


    m_consumerTimer->start(10); // 每 10ms 尝试取一次数据
}

RTPDepacketizer::~RTPDepacketizer() {
    delete m_jitterBuffer;
}

// 【生产者】网络线程调用：推入数据
void RTPDepacketizer::pushPacket(const uint8_t* data, size_t len) {
    // 1. 封装成库需要的对象
    // 注意：RTPPacket 构造函数会发生一次内存拷贝，这是必要的
    rawrtp_ptr packet = std::make_shared<RTPPacket>((uint8_t*)data, len);

    // 2. 关键修正：设置 Payload 时间
    // 该库依赖 payload_ms 计算缓冲区深度。
    // H.264 一帧可能分很多包，单个包时间很难界定。
    // 策略：简单给个 0 或 1，主要依赖库的 Timestamp 差值逻辑即可，
    // 或者简单粗暴认为每个包代表 0ms (因为只是分片)，只有完整帧才有时间。
    // 库源码中 _depth_ms 会累加这个值。为了避免 overflow 逻辑误判，建议设为 0。
    packet->payload_ms = 0;

    // 3. 推入 Jitter Buffer
    m_jitterBuffer->push(packet);
}


void RTPDepacketizer ::processPop() {
    rawrtp_ptr packet;

    // 循环取出所有可用的包
    while (true) {
        RTPJitter::RESULT res = m_jitterBuffer->pop(packet);

        if (res == RTPJitter::SUCCESS) {
            // === 成功取出一个有序包 ===
            // packet->pData 是完整的 RTP 包（含 Header）
            // packet->nLen 是长度

            // 1. 跳过 RTP Header 获取 Payload
            // 该库提供了 helper 但我们手动算更稳：RTP Header 最小 12 字节
            // 严谨做法是解析 Header 看是否有 Extension (CC, X bit)
            // 这里简单复用之前的逻辑，或者解析第一个字节
            size_t headerLen = 12;
            if (packet->nLen > 0) {
                uint8_t vpxcc = packet->pData[0];
                int csrcCount = vpxcc & 0x0F;
                headerLen += (csrcCount * 4);
                // 暂不处理 Extension bit (X) 的复杂情况，通常 WebRTC 视频流会有 Extension
                // 建议使用库自带的 helper (虽然它是 private 的，可能需要稍微改一下库暴露出来，或者自己写)
            }

            if (packet->nLen > headerLen) {
                uint32_t timestamp = 0;
                RTPHeader* rtpHeader = (RTPHeader*)packet->pData;
                timestamp = ntohl(rtpHeader->timestamp);
                // 构造 vector 传给 reassembleH264
                                // 指针运算：跳过 Header
                const uint8_t* payloadPtr = packet->pData + headerLen;
                size_t payloadSize = packet->nLen - headerLen;

                std::vector<uint8_t> payloadVec(payloadPtr, payloadPtr + payloadSize);
                if (m_isH264) {
                    // 视频：需要复杂的 FU-A 组帧
                    reassembleH264(payloadVec, timestamp);
                }
                else {
                    // 音频 (Opus)：不需要组帧，直接打包入队！
                    AVPacketPtr packet(av_packet_alloc());

                    av_new_packet(packet.get(), payloadVec.size());
                    memcpy(packet->data, payloadVec.data(), payloadVec.size());

                    packet->pts = timestamp;
                    packet->dts = timestamp;

                    // 直接推给 Audio Packet Queue
                    m_outputQueue->enqueue(move(packet));
                }
            }
        }
        else if (res == RTPJitter::DROPPED_PACKET) {
            // === 丢包处理 ===
            // 库告诉我们要跳过一个包（中间缺货超时了）
            // 这意味着 FU-A 组帧肯定失败了，必须重置组帧器状态
            resetH264Assembler();
            // 继续循环，看后面有没有包
        }
        else {
            // BUFFERING 或 EMPTY，跳出循环等待下次 Timer
            break;
        }
    }
}


void RTPDepacketizer ::resetH264Assembler() {
    m_isReassembling = false;
    m_fuBuffer.clear();
}

// reassembleH264 实现见上一个回答，保持不变
void RTPDepacketizer ::reassembleH264(const std::vector<uint8_t>& payload, uint32_t timestamp) {
    if (payload.empty()) return;

    uint8_t nalHeader = payload[0];
    uint8_t type = nalHeader & 0x1F;

    // H.264 Start Code
    const uint8_t startCode[] = { 0x00, 0x00, 0x00, 0x01 };

    if (type >= 1 && type <= 23) {
        // === Single NAL Unit ===
        // 这是一个完整的包 (如 SPS, PPS, SEI, 或小切片)
        // 只有这种时候才可能是完整的一帧，或者是参数集

        AVPacketPtr packet(av_packet_alloc());

        int totalSize = 4 + payload.size();
        av_new_packet(packet.get(), totalSize);

        // 写入 Start Code
        memcpy(packet->data, startCode, 4);
        // 写入 Payload
        memcpy(packet->data + 4, payload.data(), payload.size());

        packet->pts = timestamp; // 注意：这里的时间戳通常需要转换时间基
        packet->dts = timestamp;

        // 5. 入队
        m_outputQueue->enqueue(move(packet));

        // 如果正在组装 FU-A 却来了个 Single NAL，说明之前的 FU-A 丢包了，重置状态
        m_isReassembling = false;
        m_fuBuffer.clear();
    }
    else if (type == 28) {
        // === FU-A (Fragmentation Unit A) ===
        if (payload.size() < 2) return;

        uint8_t fuHeader = payload[1];
        bool startBit = fuHeader & 0x80;
        bool endBit = fuHeader & 0x40;
        // uint8_t forbidden = nalHeader & 0x80;
        uint8_t nri = nalHeader & 0x60;
        uint8_t originalType = fuHeader & 0x1F;

        if (startBit) {
            // 分片开始
            m_fuBuffer.clear();
            m_isReassembling = true;

            // 还原原始 NAL Header
            uint8_t reconstructedHeader = nri | originalType;

            // 写入 Start Code + 原始 Header
            m_fuBuffer.insert(m_fuBuffer.end(), startCode, startCode + 4);
            m_fuBuffer.push_back(reconstructedHeader);

            // 写入数据 (跳过 FU-A 的两个头字节)
            m_fuBuffer.insert(m_fuBuffer.end(), payload.begin() + 2, payload.end());
        }
        else if (m_isReassembling) {
            // 分片中间 或 结束
            m_fuBuffer.insert(m_fuBuffer.end(), payload.begin() + 2, payload.end());

            if (endBit) {
                // 分片结束，打包发送
                AVPacketPtr packet(av_packet_alloc());

                av_new_packet(packet.get(), m_fuBuffer.size());
                memcpy(packet->data, m_fuBuffer.data(), m_fuBuffer.size());

                packet->pts = timestamp;
                packet->dts = timestamp;

                m_outputQueue->enqueue(move(packet));

                // 重置
                m_fuBuffer.clear();
                m_isReassembling = false;
            }
        }
    }
    // 其他类型如 STAP-A (Type 24) 如果需要支持多 NAL 聚合，也需要处理
}

