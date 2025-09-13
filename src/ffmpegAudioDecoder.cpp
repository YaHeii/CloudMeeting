
#include "ffmpegAudioDecoder.h"
#include "ffmpegAudioDecoder.h"
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QMediaDevices>
#include <QDebug>

#include "logqueue.h"
#include "log_global.h"

ffmpegAudioDecoder::ffmpegAudioDecoder(QUEUE_DATA<AVPacketPtr>* packetQueue, QObject *parent)
    : QObject{parent}, m_packetQueue(packetQueue) {}

ffmpegAudioDecoder::~ffmpegAudioDecoder() {
    stopDecoding();
    clear();
}

void ffmpegAudioDecoder::clear() {
    stopDecoding();
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
    }
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);
    if (m_swrCtx) swr_free(&m_swrCtx);
    m_swrCtx = nullptr;
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

    QAudioFormat format;
    format.setSampleRate(m_codecCtx->sample_rate);
    // 使用输入音频的通道数而不是固定值2
    format.setChannelCount(m_codecCtx->ch_layout.nb_channels); 
    format.setSampleFormat(QAudioFormat::Int16); // 16位有符号整数

    m_audioSink = new QAudioSink(QMediaDevices::defaultAudioOutput(), format);

    //初始化重采样
    m_swrCtx = swr_alloc();
    av_opt_set_chlayout(m_swrCtx, "in_chlayout", &m_codecCtx->ch_layout, 0);
    av_opt_set_int(m_swrCtx, "in_sample_rate", m_codecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(m_swrCtx, "in_sample_fmt", m_codecCtx->sample_fmt, 0);

    // 设置正确的输出通道布局
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, format.channelCount());
    av_opt_set_chlayout(m_swrCtx, "out_chlayout", &out_ch_layout, 0);
    av_channel_layout_uninit(&out_ch_layout);
    
    av_opt_set_int(m_swrCtx, "out_sample_rate", format.sampleRate(), 0);
    av_opt_set_sample_fmt(m_swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    if (swr_init(m_swrCtx) < 0) {
        WRITE_LOG("swr_init failed");
        return false;
    }
    WRITE_LOG("Audio Decoder initialized successfully.");
    return true;
}

void ffmpegAudioDecoder::startDecoding() {
    m_isDecoding = true;
    m_audioDevice = m_audioSink->start(); // 开始播放，获取写入设备
    if (!m_audioDevice) {
        WRITE_LOG("Failed to start audio sink");
        m_isDecoding = false;
        return;
    }

    WRITE_LOG("ffmpegAudioDecoder::startDecoding");

    AVFramePtr frame(av_frame_alloc());
    if (!frame) {
        WRITE_LOG("ffmpegAudioDecoder::Failed to allocate frame");
        m_isDecoding = false;
        return;
    }
    
    // 创建一个足够大的缓冲区来存放重采样后的数据
    uint8_t* resampled_buffer = (uint8_t*)av_malloc(48000 * 2 * 2); // 1秒的数据
    if (!resampled_buffer) {
        WRITE_LOG("ffmpegAudioDecoder::Failed to allocate resampled buffer");
        m_isDecoding = false;
        return;
    }

    while (m_isDecoding) {
        AVPacketPtr packet;
        if (!m_packetQueue->dequeue(packet)) {
            // WRITE_LOG("ffmpegAudioDecoder::Deque Packet TimeOut");
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

        int receiveResult;
        while (m_isDecoding && (receiveResult = avcodec_receive_frame(m_codecCtx, frame.get())) == 0) {
            // 解码成功，进行重采样
            int resampled_data_size = swr_convert(m_swrCtx, &resampled_buffer, frame->nb_samples,
                                                  (const uint8_t**)frame->data, frame->nb_samples);
            
            if (resampled_data_size < 0) {
                char errbuf[1024] = {0};
                av_strerror(resampled_data_size, errbuf, sizeof(errbuf));
                WRITE_LOG("ffmpegAudioDecoder::swr_convert failed: %s", errbuf);
                continue;
            }

            if (resampled_data_size > 0 && m_audioDevice) {
                // 将重采样后的PCM数据写入音频设备进行播放
                qint64 written = m_audioDevice->write((const char*)resampled_buffer, 
                                                     resampled_data_size * 2 * sizeof(int16_t));
                if (written < 0) {
                    WRITE_LOG("ffmpegAudioDecoder::Failed to write to audio device: %s", 
                             m_audioDevice->errorString().toLocal8Bit().data());
                }
            }
        }
        
        if (receiveResult < 0 && receiveResult != AVERROR(EAGAIN) && receiveResult != AVERROR_EOF) {
            char errbuf[1024] = {0};
            av_strerror(receiveResult, errbuf, sizeof(errbuf));
            WRITE_LOG("ffmpegAudioDecoder::avcodec_receive_frame failed: %s", errbuf);
        }
    }

    m_audioSink->stop();
    av_free(resampled_buffer);
    resampled_buffer = nullptr;
    WRITE_LOG("Audio decoding loop finished.");
}

void ffmpegAudioDecoder::stopDecoding() {
    m_isDecoding = false;
}
