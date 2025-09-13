/**
 *完成音视频的帧数据编码
 *madebyYahei
 */

#include "ffmpegEncoder.h"

#include "logqueue.h"
#include "log_global.h"
#include "libavutil/opt.h"

ffmpegEncoder::ffmpegEncoder(QUEUE_DATA<AVFramePtr>* frameQueue,QUEUE_DATA<AVPacketPtr>* packetQueue,QObject* parent)
    : m_frameQueue(frameQueue),m_packetQueue(packetQueue){}

ffmpegEncoder::~ffmpegEncoder()
{
    clear();
}

bool ffmpegEncoder::initAudioEncoder(AVCodecParameters* aparams){
    m_mediaType = AVMEDIA_TYPE_AUDIO;
    const AVCodec* codec = avcodec_find_encoder_by_name("aac"); // 使用AAC编码器
    if (!codec) { emit errorOccurred("Codec aac not found."); return false; }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) { emit errorOccurred("Failed to allocate codec context."); return false; }

    // 设置音频编码参数
    m_codecCtx->sample_rate = aparams->sample_rate;
    av_channel_layout_copy(&m_codecCtx->ch_layout, &aparams->ch_layout);
    m_codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP; // AAC常用格式
    m_codecCtx->bit_rate = 128000; // 128 kbps
    m_codecCtx->time_base = {1, m_codecCtx->sample_rate};

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        emit errorOccurred("Failed to open audio codec.");
        return false;
    }

    emit encoderInitialized(m_codecCtx);
    WRITE_LOG("Audio encoder initialized successfully.");
    return true;
}

bool ffmpegEncoder::initVideoEncoder(AVCodecParameters* vparams){
    m_mediaType = AVMEDIA_TYPE_VIDEO;
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264"); // 使用H.264编码器
    if (!codec) { emit errorOccurred("Codec libx264 not found."); return false; }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) { emit errorOccurred("Failed to allocate codec context."); return false; }

    // 设置视频编码参数
    m_codecCtx->width = vparams->width;
    m_codecCtx->height = vparams->height;
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P; // H.264常用格式
    m_codecCtx->time_base = {1, 25}; // 25 fps
    m_codecCtx->framerate = {25, 1};
    m_codecCtx->bit_rate = 2000000; // 2 Mbps
    // 更多参数...
    m_codecCtx->gop_size = 25;
    m_codecCtx->max_b_frames = 1;
    av_opt_set(m_codecCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(m_codecCtx->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        emit errorOccurred("Failed to open video codec.");
        return false;
    }

    emit encoderInitialized(m_codecCtx);
    WRITE_LOG("Video encoder initialized successfully.");
    return true;
}

void ffmpegEncoder::startEncoding(){
    m_isEncoding = true;
    encodingLoop();
}
void ffmpegEncoder::stopEncoding() {
    m_isEncoding = false;
}
void ffmpegEncoder::encodingLoop(){
    WRITE_LOG("Starting encoding loop for %s", (m_mediaType == AVMEDIA_TYPE_VIDEO ? "video" : "audio"));

    while (m_isEncoding) {
        AVFramePtr frame;
        if (!m_frameQueue->dequeue(frame)) {
            continue;
        }

        int ret = avcodec_send_frame(m_codecCtx, frame.get());
        if (ret < 0) {
            emit errorOccurred("Error sending frame to encoder.");
            continue;
        }
        while (ret >= 0) {
            AVPacketPtr packet(av_packet_alloc());
            ret = avcodec_receive_packet(m_codecCtx, packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                emit errorOccurred("Error receiving packet from encoder.");
                break;
            }
            packet->stream_index = (m_mediaType == AVMEDIA_TYPE_VIDEO)? 0 : 1;
            m_packetQueue->enqueue(std::move(packet));
            WRITE_LOG("Encoded %s packet with size %d", (m_mediaType == AVMEDIA_TYPE_VIDEO ? "video" : "audio"), packet->size);
        }
    }
    WRITE_LOG("Encoding loop finished for %s", (m_mediaType == AVMEDIA_TYPE_VIDEO ? "video" : "audio"));
}

void ffmpegEncoder::clear() {
    stopEncoding();
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
}