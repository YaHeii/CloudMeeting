
#include "ffmpegAudioDecoder.h"
#include "ffmpegAudioDecoder.h"
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QMediaDevices>
#include <QDebug>

#include "logqueue.h"
#include "log_global.h"

ffmpegAudioDecoder::ffmpegAudioDecoder(QUEUE_DATA<AVPacketPtr>* packetQueue ,QUEUE_DATA<AVFramePtr>* frameQueue , QObject *parent)
    : QObject{parent}, m_packetQueue(packetQueue), m_frameQueue(frameQueue) {}

ffmpegAudioDecoder::~ffmpegAudioDecoder() {
    stopDecoding();
    clear();
}

void ffmpegAudioDecoder::clear() {
    stopDecoding();
    // if (m_audioSink) {
    //     m_audioSink->stop();
    //     delete m_audioSink;
    //     m_audioSink = nullptr;
    // }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
    if (m_fifo) {
        av_audio_fifo_free(m_fifo);
        m_fifo = nullptr;
    }
}

bool ffmpegAudioDecoder::init(AVCodecParameters *params) {
    if (!params) {
        WRITE_LOG("Audio codec not found");
        return false;
    }
    m_codec = avcodec_find_decoder(params->codec_id);
    if (!m_codec) {
        return false;
    }
    m_codecCtx = avcodec_alloc_context3(m_codec);
    if (avcodec_parameters_to_context(m_codecCtx, params) < 0) { WRITE_LOG("avcodec_parameters_to_context failed"); return false; }
    if (avcodec_open2(m_codecCtx, m_codec, nullptr) < 0) { WRITE_LOG("avcodec_open2 failed"); return false; }

    WRITE_LOG("Audio Decoder initialized successfully.");
    return true;
}

void ffmpegAudioDecoder::setResampleConfig(const AudioResampleConfig& config) {
    WRITE_LOG("AudioDecoder received encoder config. Frame size: %d, Sample Rate: %d",
              config.frame_size, config.sample_rate);
    m_ResampleConfig = config;

    // --- 初始化重采样器 ---
    if(m_swrCtx) swr_free(&m_swrCtx);

    swr_alloc_set_opts2(&m_swrCtx,
                        &config.ch_layout,      // 目标通道布局
                        config.sample_fmt,      // 目标采样格式
                        config.sample_rate,     // 目标采样率
                        &m_codecCtx->ch_layout, // 源通道布局
                        m_codecCtx->sample_fmt, // 源采样格式
                        m_codecCtx->sample_rate,  // 源采样率
                        0, nullptr);
    if (!m_swrCtx || swr_init(m_swrCtx) < 0) {
        emit errorOccurred("Failed to initialize audio resampler.");
        return;
    }

    // --- 初始化FIFO ---
    if(m_fifo) av_audio_fifo_free(m_fifo);
    m_fifo = av_audio_fifo_alloc(config.sample_fmt, config.ch_layout.nb_channels, 1);
    if (!m_fifo) {
        emit errorOccurred("Failed to allocate audio FIFO.");
        return;
    }

    m_isConfigReady = true; // 标记为配置就绪
}

void ffmpegAudioDecoder::startDecoding() {
    m_isDecoding = true;
    decodingAudioLoop();
}
void ffmpegAudioDecoder::stopDecoding() {
    m_isDecoding = false;
}

void ffmpegAudioDecoder::decodingAudioLoop() {
    WRITE_LOG("ffmpegAudioDecoder::startDecoding");
    AVFramePtr decoded_frame(av_frame_alloc());
    AVFramePtr resampledFrame(av_frame_alloc());

    if (!decoded_frame || !resampledFrame) {
        emit errorOccurred("ffmpegAudioDecoder::Failed to allocate frame");
        m_isDecoding = false;
        return;
    }

    while (m_isDecoding) {

        if (!m_isConfigReady) { // 如果编码器未就绪，等待
            QThread::msleep(10);
            continue;
        }
        AVPacketPtr packet;
        if (!m_packetQueue->dequeue(packet)) {
            WRITE_LOG("ffmpegAudioDecoder::Deque Packet TimeOut");
            continue;
        }
        
        if (!packet) {
            WRITE_LOG("ffmpegAudioDecoder::Received null packet");
            continue;
        }

        int sendResult = avcodec_send_packet(m_codecCtx, packet.get());
        if (sendResult != 0) {
            char errbuf[1024] = {0};
            av_strerror(sendResult, errbuf, sizeof(errbuf));
            WRITE_LOG("ffmpegAudioDecoder::avcodec_send_packet failed: %s", errbuf);
            continue;
        }

        while (avcodec_receive_frame(m_codecCtx, decoded_frame.get()) == 0) {
            // 解码成功，进行重采样
            resampledFrame->ch_layout = m_ResampleConfig.ch_layout;
            resampledFrame->sample_rate = m_ResampleConfig.sample_rate;
            resampledFrame->format = m_ResampleConfig.sample_fmt;
            int ret = swr_convert_frame(m_swrCtx, resampledFrame.get(), decoded_frame.get());
            if (ret < 0) continue;

            // --- 写入FIFO ---
            av_audio_fifo_write(m_fifo, (void**)resampledFrame->data, resampledFrame->nb_samples);

            // --- 读取固定帧 ---
            while (av_audio_fifo_size(m_fifo) >= m_ResampleConfig.frame_size) {
                AVFramePtr sendFrame(av_frame_alloc());

                // 设置帧参数
                sendFrame->nb_samples = m_ResampleConfig.frame_size;
                sendFrame->ch_layout = m_ResampleConfig.ch_layout;
                sendFrame->format = m_ResampleConfig.sample_fmt;
                sendFrame->sample_rate = m_ResampleConfig.sample_rate;

                if (av_frame_get_buffer(sendFrame.get(), 0) < 0) break;

                av_audio_fifo_read(m_fifo, (void**)sendFrame->data, m_ResampleConfig.frame_size);

                // --- 计算PTS ---
                sendFrame->pts = m_nextPts;
                m_nextPts += sendFrame->nb_samples;

                // --- 入队给编码器 ---
                m_frameQueue->enqueue(std::move(sendFrame));
            }
        }

    }
    WRITE_LOG("Audio decoding loop finished.");
}



