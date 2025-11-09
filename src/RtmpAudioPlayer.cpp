#include "RtmpAudioPlayer.h"
#include "logqueue.h"
#include "log_global.h"
#include <QAudioFormat>
#include <QMediaDevices>
#include <libavutil/error.h>

RtmpAudioPlayer::RtmpAudioPlayer(std::shared_ptr<QUEUE_DATA<AVPacketPtr>> packetQueue,
    QObject* parent)
    : QObject{ parent }, m_packetQueue(packetQueue) {
}

RtmpAudioPlayer::~RtmpAudioPlayer() {
    cleanup();
}

bool RtmpAudioPlayer::init(AVCodecParameters* params, AVRational inputTimeBase) {
    if (!params) return false;
    m_inputTimeBase = inputTimeBase;

    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
        emit errorOccurred("AudioPlayer: Codec not found");
        return false;
    }
    m_codecCtx = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(m_codecCtx, params) < 0) {
        avcodec_free_context(&m_codecCtx);
        emit errorOccurred("AudioPlayer: avcodec_parameters_to_context failed");
        return false;
    }
    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&m_codecCtx);
        emit errorOccurred("AudioPlayer: avcodec_open2 failed");
        return false;
    }

    WRITE_LOG("RtmpAudioPlayer initialized successfully.");
    return true;
}

bool RtmpAudioPlayer::initAudioOutput(AVFrame* frame) {
    // 设置重采样器 (SwrContext)，目标格式为 QAudioSink 支持的格式

    // QAudioFormat 期望的格式 (例如 S16LE)
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    QAudioFormat::SampleFormat q_sample_format = QAudioFormat::Int16;

    // 目标通道布局
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2); // 强制立体声

    // 目标采样率
    int out_sample_rate = 48000;

    // 释放旧的
    if (m_swrCtx) swr_free(&m_swrCtx);

    swr_alloc_set_opts2(&m_swrCtx,
        &out_ch_layout,      // 目标
        out_sample_fmt,      // 目标
        out_sample_rate,     // 目标
        &frame->ch_layout,   // 源
        (AVSampleFormat)frame->format, // 源
        frame->sample_rate,  // 源
        0, nullptr);

    if (!m_swrCtx || swr_init(m_swrCtx) < 0) {
        emit errorOccurred("AudioPlayer: Failed to initialize audio resampler.");
        return false;
    }

    // 初始化 QAudioSink
    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
    }

    QAudioFormat format;
    format.setSampleRate(out_sample_rate);
    format.setChannelCount(out_ch_layout.nb_channels);
    format.setSampleFormat(q_sample_format);

    const QAudioDevice& defaultDevice = QMediaDevices::defaultAudioOutput();
    m_audioSink = new QAudioSink(defaultDevice, format, this);

    // (可选) 设置缓冲区大小
    m_audioSink->setBufferSize(out_sample_rate * out_ch_layout.nb_channels * 2 * 0.5); // 0.5秒

    m_audioDevice = m_audioSink->start(); // 开始播放，获取 QIODevice
    if (!m_audioDevice) {
        emit errorOccurred("AudioPlayer: Failed to start QAudioSink.");
        return false;
    }

    WRITE_LOG("AudioPlayer: QAudioSink started.");
    return true;
}

void RtmpAudioPlayer::startPlaying() {
    if (m_isDecoding) return;
    m_isDecoding = true;
    QMetaObject::invokeMethod(this, "doDecodingWork", Qt::QueuedConnection);
    WRITE_LOG("AudioPlayer: Decoding process started.");
}

void RtmpAudioPlayer::stopPlaying() {
    m_isDecoding = false;
    // 使用与 ffmpegVideoDecoder::clear() 相同的同步机制
    {
        QMutexLocker locker(&m_workMutex);
        while (m_isDoingWork) {
            m_workCond.wait(&m_workMutex);
        }
    }
    WRITE_LOG("AudioPlayer: Decoding process stopped.");
}

void RtmpAudioPlayer::cleanup() {
    stopPlaying();

    if (m_audioSink) {
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
        m_audioDevice = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
    if (m_resampledData) {
        av_freep(&m_resampledData[0]);
        av_freep(&m_resampledData);
    }
    WRITE_LOG("RtmpAudioPlayer cleared.");
}

void RtmpAudioPlayer::doDecodingWork() {
    if (!m_isDecoding) {
        return;
    }

    AVPacketPtr packet;
    if (!m_packetQueue->dequeue(packet)) {
        WRITE_LOG("AudioPlayer: Dequeue Packet TimeOut");
        if (m_isDecoding) {
            QMetaObject::invokeMethod(this, "doDecodingWork", Qt::QueuedConnection);
        }
        return;
    }

    // 锁 (复制自 ffmpegVideoDecoder::doDecodingPacket)
    {
        QMutexLocker locker(&m_workMutex);
        m_isDoingWork = true;
    }
    auto work_guard = [this]() {
        QMutexLocker locker(&m_workMutex);
        m_isDoingWork = false;
        m_workCond.wakeAll();
        };

    AVFramePtr decoded_frame(av_frame_alloc());
    if (!decoded_frame) {
        emit errorOccurred("AudioPlayer: Failed to allocate frame");
        m_isDecoding = false;
        return;
    }

    if (avcodec_send_packet(m_codecCtx, packet.get()) == 0) {
        while (avcodec_receive_frame(m_codecCtx, decoded_frame.get()) == 0) {
            if (!m_isDecoding) break;

            // 1. 首次接收到帧时，初始化 QAudioSink 和 SwrContext
            if (!m_audioSink) {
                if (!initAudioOutput(decoded_frame.get())) {
                    m_isDecoding = false;
                    break;
                }
            }

            // 2. 分配重采样缓冲区
            // (基于 ffmpegAudioDecoder::setResampleConfig 的逻辑)
            int dst_nb_samples = av_rescale_rnd(decoded_frame->nb_samples,
                m_audioSink->format().sampleRate(),
                decoded_frame->sample_rate, AV_ROUND_UP);

            if (dst_nb_samples > m_resampledDataSize) {
                av_freep(&m_resampledData[0]);
                av_freep(&m_resampledData);
                av_samples_alloc_array_and_samples(&m_resampledData, &m_resampledLinesize,
                    m_audioSink->format().channelCount(),
                    dst_nb_samples, AV_SAMPLE_FMT_S16, 0);
                m_resampledDataSize = dst_nb_samples;
            }

            // 3. 重采样
            int samples_converted = swr_convert(m_swrCtx,
                m_resampledData, dst_nb_samples,
                (const uint8_t**)decoded_frame->data,
                decoded_frame->nb_samples);

            if (samples_converted <= 0) continue;

            // 4. 计算缓冲区大小并写入 QAudioSink
            int buffer_size = av_samples_get_buffer_size(&m_resampledLinesize,
                m_audioSink->format().channelCount(),
                samples_converted,
                AV_SAMPLE_FMT_S16, 1);

            if (m_audioDevice) {
                // (可选) 检查缓冲区空闲空间，实现音视频同步
                // int free_bytes = m_audioSink->bytesFree();
                // while (free_bytes < buffer_size && m_isDecoding) {
                //     QThread::msleep(10);
                //     free_bytes = m_audioSink->bytesFree();
                // }
                // if (!m_isDecoding) break;

                m_audioDevice->write((const char*)m_resampledData[0], buffer_size);
            }
        }
    }

    work_guard();

    if (m_isDecoding) {
        QMetaObject::invokeMethod(this, "doDecodingWork", Qt::QueuedConnection);
    }
}