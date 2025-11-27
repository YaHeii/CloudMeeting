#include "RTPDepacketizer.h"
#include <winsock2.h>
#include "log_global.h"
#include "logqueue.h"
extern "C" {
#include <libavcodec/avcodec.h>
}

RTPDepacketizer::RTPDepacketizer(int sampleRate, QUEUE_DATA<AVPacketPtr>* outputQueue, bool isH264, QObject* parent)
    : QObject(parent), m_outputQueue(outputQueue),m_isH264(isH264)
{
    // 保存 sample rate，用于将 RTP timestamp 差值转换为毫秒
    m_payloadSampleRate = sampleRate;
    m_lastTimestamp = 0;
    m_hasLastTimestamp = false;

    // 设定缓冲深度，降低到 80ms 以减少首次进入 BUFFERING 的延迟
    const unsigned nominalDepthMs = 80;
    m_jitterBuffer = new RTPJitter(nominalDepthMs, sampleRate);
    if (m_jitterBuffer) {
        WRITE_LOG("jitterBuffer init (nominal=%d ms)", nominalDepthMs);
    }
    // 启动一个定时器来驱动数据 "流出"
    // Jitter Buffer 的核心是 "延时输出"，所以需要定时去取
    m_consumerTimer = new QTimer(this);
    if (m_consumerTimer) {
        WRITE_LOG("Timer init");
    }
    connect(m_consumerTimer, &QTimer::timeout, this, &RTPDepacketizer::processPop);


    m_consumerTimer->start(10); // 每 10ms 尝试取一次数据
}

RTPDepacketizer::~RTPDepacketizer() {
    delete m_jitterBuffer;
}

// 【生产者】网络线程调用：推入数据
void RTPDepacketizer::pushPacket(const uint8_t* data, size_t len) {
    WRITE_LOG("push packet to jitterBuffer");
    if (!data || len < sizeof(RTPHeader)) {
        WRITE_LOG("pushPacket: invalid data or too short for RTP header (len=%d)", (int)len);
        return;
    }
    // 1. 封装成库需要的对象
    // 注意：RTPPacket 构造函数会发生一次内存拷贝，这是必要的
    rawrtp_ptr packet = std::make_shared<RTPPacket>((uint8_t*)data, len);
    if (packet == NULL) {
        WRITE_LOG("Packetizer failed");
        return;
    }

    // 解析 RTP header 的 timestamp（用于估算 payload_ms）
    uint32_t timestamp = 0;
    RTPHeader* rtpHeader = reinterpret_cast<RTPHeader*>(packet->pData);
    timestamp = ntohl(rtpHeader->timestamp);

    uint32_t payload_ms = 0;
    if (!m_hasLastTimestamp) {
        m_hasLastTimestamp = true;
        // 为了避免首次长时间 BUFFERING，给首包一个小的占位时间，等后续包会用 timestamp 差做精确计数
        int nominal = 0;
        if (m_jitterBuffer) nominal = m_jitterBuffer->get_nominal_depth();
        payload_ms = (nominal > 0) ? static_cast<uint32_t>(nominal) : 20;
    }
    else {
        uint32_t delta = timestamp - m_lastTimestamp; // 无符号处理 wrap
        if (m_payloadSampleRate > 0) {
            payload_ms = static_cast<uint32_t>((static_cast<uint64_t>(delta) * 1000ULL) / static_cast<uint64_t>(m_payloadSampleRate));
            if (payload_ms > 10000) payload_ms = 10000;
        } else {
            payload_ms = 0;
        }
    }
    m_lastTimestamp = timestamp;

    // 关键：设置 Payload 时间，供 jitter buffer depth 计算使用。
    packet->payload_ms = static_cast<uint16_t>(payload_ms);

    // 3. 推入 Jitter Buffer
    RTPJitter::RESULT  res = m_jitterBuffer->push(packet);
    if (res == RTPJitter::SUCCESS) {
        WRITE_LOG("push packet in the jitterbuffer (payload_ms=%d)", packet->payload_ms);
    } else if (res == RTPJitter::BUFFER_OVERFLOW) {
        WRITE_LOG("push packet failed: BUFFER_OVERFLOW (payload_ms=%d)", packet->payload_ms);
    } else if (res == RTPJitter::BAD_PACKET) {
        WRITE_LOG("push packet failed: BAD_PACKET (payload_ms=%d)", packet->payload_ms);
    } else {
        WRITE_LOG("push packet failed: result=%d (payload_ms=%d)", (int)res, packet->payload_ms);
    }
}


void RTPDepacketizer ::processPop() {
    rawrtp_ptr packet;

    // 循环取出所有可用的包
    while (true) {
        RTPJitter::RESULT res = m_jitterBuffer->pop(packet);

        if (res == RTPJitter::SUCCESS) {
            WRITE_LOG("pop a packet from jitterBuffer");
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
                    WRITE_LOG("pop a audio packet to decoder");
                    
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
        WRITE_LOG("pop a video packet to decoder (Single NAL Unit)");
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
                WRITE_LOG("pop a video packet to decoder (Fragmentation Unit)");
                // 重置
                m_fuBuffer.clear();
                m_isReassembling = false;
            }
        }
    }
    // 其他类型如 STAP-A (Type 24) 如果需要支持多 NAL 聚合，也需要处理
}

