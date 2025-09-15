#include "RtmpPublisher.h"

#include "logqueue.h"
#include "log_global.h"

extern "C"{
#include "libavformat/avformat.h"
#include "libavutil/time.h"
}

RtmpPublisher::RtmpPublisher(QUEUE_DATA<AVPacketPtr>* encodedPacketQueue, QObject *parent)
    : QObject{parent}, m_encodedPacketQueue(encodedPacketQueue){}

RtmpPublisher::~RtmpPublisher()
{
    clear();
}

bool RtmpPublisher::init(const QString& rtmpUrl, AVCodecContext* vCodecCtx, AVCodecContext* aCodecCtx) {
    if (m_outputFmtCtx) {
        clear();
    }
    if (!vCodecCtx && !aCodecCtx) {
        WRITE_LOG("No codec parameters provided.");
        return false;
    }

    int ret = avformat_alloc_output_context2(&m_outputFmtCtx,nullptr,"flv",rtmpUrl.toStdString().c_str());
    if (ret < 0 || !m_outputFmtCtx) {
        WRITE_LOG("Failed to allocate output context.");
        return false;
    }

    if (vCodecCtx) {
        m_videoStream = avformat_new_stream(m_outputFmtCtx, nullptr);
        if (!m_videoStream) {
            WRITE_LOG("PUBLISHER ERROR:Fail to create videoStream");
            clear();
            return false;
        }
        avcodec_parameters_from_context(m_videoStream->codecpar, vCodecCtx);
        m_videoStream->codecpar->codec_tag = 0;
        m_videoEncoderTimeBase = vCodecCtx->time_base;//设置编码器时间基
    }
    if (aCodecCtx) {
        m_audioStream = avformat_new_stream(m_outputFmtCtx, nullptr);
        if (!m_audioStream) {
            WRITE_LOG("Fail to alloc audioStream");
            clear();
            return false;
        }
        avcodec_parameters_from_context(m_audioStream->codecpar, aCodecCtx);
        m_audioStream->codecpar->codec_tag = 0;
        m_audioEncoderTimeBase = aCodecCtx->time_base;
    }
    // --- 打开IO ---
    if (!(m_outputFmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_outputFmtCtx->pb, rtmpUrl.toStdString().c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            char errbuf[1024] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            emit errorOccurred(QString("Publisher Error: Failed to open RTMP URL: %1").arg(errbuf));
            clear();
            return false;
        }
    }


    // --- 写入文件头 ---
    ret = avformat_write_header(m_outputFmtCtx, nullptr);
    if (ret < 0) {
        char errbuf[1024] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit errorOccurred(QString("Publisher Error: Fail to write RTMP header: %1").arg(errbuf));
        clear();
        return false;
    }

    WRITE_LOG("RTMP publisher initialized. Connected to %s", rtmpUrl.toStdString().c_str());
    return true;
}

void RtmpPublisher::startPublishing() {
    m_isPublishing = true;
    publishingLoop();
}
void RtmpPublisher::publishingLoop() {
    if (!m_outputFmtCtx) {
        WRITE_LOG("Publisher NOT initialized");
        return;
    }
    m_isPublishing = true;
    emit publisherStarted();
    WRITE_LOG("Starting RTMP publishing loop...");

    while (m_isPublishing) {
        AVPacketPtr packet;
        if (!m_encodedPacketQueue->dequeue(packet)) {
            continue;
        }

        AVStream* dest_stream = nullptr;
        AVRational source_time_base;

        // 判断包的类型，并设置好目标流和源时间基
        if (packet->stream_index == 0 && m_videoStream) { // 视频流
            dest_stream = m_videoStream;
            source_time_base = m_videoEncoderTimeBase;
        } else if (packet->stream_index == 1 && m_audioStream) { // 音频流
            dest_stream = m_audioStream;
            source_time_base = m_audioEncoderTimeBase;
        } else {
            WRITE_LOG("Unknown packet stream_index: %d", packet->stream_index);
            continue; // 忽略未知来源的包
        }

        // --- 核心：时间基转换 ---
        // 将packet的PTS/DTS从编码器的时间基(source_time_base)转换到输出流的时间基(dest_stream->time_base)
        av_packet_rescale_ts(packet.get(), source_time_base, dest_stream->time_base);
        packet->pos = -1;

        int ret = av_interleaved_write_frame(m_outputFmtCtx, packet.get());
        if (ret < 0) {
            char errbuf[1024] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            WRITE_LOG("Error writing frame to RTMP stream: %s", errbuf);
            // 这里可以根据错误类型决定是否需要重连
            if (m_isPublishing) { // 避免在停止时重复发信号
                emit errorOccurred("Failed to write frame, publisher may be disconnected.");
            }
            break; // 出现错误，跳出循环
        }
    }
    if (m_outputFmtCtx) {
        av_write_trailer(m_outputFmtCtx);
    }
    WRITE_LOG("RTMP publishing loop finished.");
    emit publisherStopped();
}

void RtmpPublisher::stopPublishing() {
    m_isPublishing = false;
}

void RtmpPublisher::clear() {
    stopPublishing();

    if (m_outputFmtCtx) {
        // 关闭IO流（如果已打开）
        if (!(m_outputFmtCtx->oformat->flags & AVFMT_NOFILE) && m_outputFmtCtx->pb) {
            avio_closep(&m_outputFmtCtx->pb);
        }
        // 释放上下文
        avformat_free_context(m_outputFmtCtx);
        m_outputFmtCtx = nullptr;
    }
    m_videoStream = nullptr;
    m_audioStream = nullptr;
    WRITE_LOG("RtmpPublisher cleared.");
}