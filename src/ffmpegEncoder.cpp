/**
 *完成音视频的帧数据编码
 *madebyYahei
 */

#include "ffmpegEncoder.h"

#include "logqueue.h"
#include "log_global.h"
#include "libavutil/opt.h"

ffmpegEncoder::ffmpegEncoder(QUEUE_DATA<AVFramePtr> *frameQueue, QUEUE_DATA<AVPacketPtr> *packetQueue, QObject *parent)
    : m_frameQueue(frameQueue), m_packetQueue(packetQueue) {
}

ffmpegEncoder::~ffmpegEncoder() {
    clear();
}
bool ffmpegEncoder::initAudioEncoderAAC(AVCodecParameters* aparams) {
	WRITE_LOG("Initializing Audio AAC Encoder...");
    m_mediaType = AVMEDIA_TYPE_AUDIO;
    const AVCodec* codec = nullptr;
    codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        codec = avcodec_find_encoder_by_name("libfdk_aac");
    }
    if (!codec) {
        emit errorOccurred("Codec AAC not found.");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        emit errorOccurred("Failed to allocate codec context.");
        return false;
    }
    int sample_rate =48000;
    int nb_channels =1;
    if (aparams) {
        if (aparams->sample_rate >0) sample_rate = aparams->sample_rate;
        if (aparams->ch_layout.nb_channels >0) nb_channels = aparams->ch_layout.nb_channels;
    }

    m_codecCtx->sample_rate = sample_rate;
    av_channel_layout_default(&m_codecCtx->ch_layout, nb_channels);
    m_codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_codecCtx->bit_rate =128000; //128 kbps
    m_codecCtx->time_base = {1, m_codecCtx->sample_rate };

    if (codec->id == AV_CODEC_ID_AAC) {
        // nothing additional required for native aac
    }
    else {
        // try to set some libfdk_aac options if used
        if (av_opt_set(m_codecCtx->priv_data, "bitrate", "128000",0) <0) {
        }
    }

    m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(m_codecCtx, codec, nullptr) <0) {
        emit errorOccurred("Failed to open audio codec.");
        avcodec_free_context(&m_codecCtx);
        return false;
    }

    // reset audio samples counter
    m_audioSamplesCount =0;

    AudioResampleConfig config;
    config.frame_size = m_codecCtx->frame_size;
    config.sample_rate = m_codecCtx->sample_rate;
    config.sample_fmt = m_codecCtx->sample_fmt;
    config.ch_layout = m_codecCtx->ch_layout;
    emit audioEncoderReady(config);
    emit initializationSuccess();
    WRITE_LOG("Audio AAC encoder initialized successfully. Frame size: %d", m_codecCtx->frame_size);
    return true;
}

bool ffmpegEncoder::initAudioEncoderOpus(AVCodecParameters *aparams) {
	WRITE_LOG("Initializing Audio Opus Encoder...");
    m_mediaType = AVMEDIA_TYPE_AUDIO;
    // Prefer native AAC encoder if available
    const AVCodec *codec = avcodec_find_encoder_by_name("libopus");
    if (!codec) {
        emit errorOccurred("Codec opus not found.");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        emit errorOccurred("Failed to allocate codec context.");
        return false;
    }

    m_codecCtx->sample_rate = 48000;
    av_channel_layout_default(&m_codecCtx->ch_layout, 1); //单声道
    m_codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP; //浮点平面采样
    m_codecCtx->bit_rate = 48000;
    if (av_opt_set(m_codecCtx->priv_data, "application", "voip", 0) < 0) {
        //优化语音延迟
        emit errorOccurred("Failed to set Opus application to voip.");
    }
    // 设置 VBR (Variable Bit-Rate) 开启，可以在保证质量的同时节省带宽
    if (av_opt_set(m_codecCtx->priv_data, "vbr", "on", 0) < 0) {
        emit errorOccurred("Failed to enable VBR for Opus.");
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        emit errorOccurred("Failed to open audio codec.");
        return false;
    }
    AudioResampleConfig config;
    config.frame_size = m_codecCtx->frame_size;
    config.sample_rate = m_codecCtx->sample_rate;
    config.sample_fmt = m_codecCtx->sample_fmt;
    config.ch_layout = m_codecCtx->ch_layout;
    emit audioEncoderReady(config);
    emit initializationSuccess();

    WRITE_LOG("Opus encoder initialized successfully. Frame size: %d", m_codecCtx->frame_size);
    return true;
}

bool ffmpegEncoder::initVideoEncoderH264(AVCodecParameters *vparams) {
	WRITE_LOG("Initializing Video H.264 Encoder...");
    m_mediaType = AVMEDIA_TYPE_VIDEO;
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264"); // 使用H.264 编码器
    if (!codec) {
        emit errorOccurred("Codec libx264 not found.");
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        emit errorOccurred("Failed to allocate codec context.");
        return false;
    }

    // 设置视频编码参数
    m_codecCtx->width = vparams->width;
    m_codecCtx->height = vparams->height;
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P; // H.264常用格式
    m_codecCtx->time_base = {1,25}; //25 fps
    m_codecCtx->framerate = {25,1};
    m_codecCtx->bit_rate =2000000; //2 Mbps
    m_codecCtx->gop_size =25;
    m_codecCtx->max_b_frames =1;
    av_opt_set(m_codecCtx->priv_data, "preset", "ultrafast",0);
    av_opt_set(m_codecCtx->priv_data, "tune", "zerolatency",0);
    m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; // 全局头
    if (avcodec_open2(m_codecCtx, codec, nullptr) <0) {
        emit errorOccurred("Failed to open video codec.");
        return false;
    }

    // reset video frame counter
    m_videoFrameCounter =0;

    emit encoderInitialized(m_codecCtx);
    emit initializationSuccess();
    WRITE_LOG("Video encoder initialized successfully.");
    return true;
}

void ffmpegEncoder::startEncoding() {
    if (m_isEncoding) return;
    m_isEncoding = true;
    QMetaObject::invokeMethod(this, "doEncodingWork", Qt::QueuedConnection);
    WRITE_LOG("Starting encoding process for %s", (m_mediaType == AVMEDIA_TYPE_VIDEO ? "video" : "audio"));
}

void ffmpegEncoder::stopEncoding() {
    m_isEncoding = false;
    WRITE_LOG("Stopping encoding process for %s", (m_mediaType == AVMEDIA_TYPE_VIDEO ? "video" : "audio"));
}

void ffmpegEncoder::doEncodingWork() { {
        QMutexLocker locker(&m_workMutex);
        m_isDoingWork = true;
    }
    auto work_guard = [this]() {
        QMutexLocker locker(&m_workMutex);
        m_isDoingWork = false;
        m_workCond.wakeAll();
    };
    AVFramePtr frame;
    if (m_frameQueue->dequeue(frame)) {
        // Assign PTS if missing and update internal counters
        if (m_mediaType == AVMEDIA_TYPE_VIDEO) {          
            frame->pts = m_videoFrameCounter++;
        } else if (m_mediaType == AVMEDIA_TYPE_AUDIO) {
            frame->pts = m_audioSamplesCount;
            m_audioSamplesCount += frame->nb_samples;
        }

        int ret = avcodec_send_frame(m_codecCtx, frame.get());
        if (ret <0) {
            emit errorOccurred("Error sending frame to encoder.");
        } else {
            while (ret >=0) {
                AVPacketPtr packet(av_packet_alloc());
                ret = avcodec_receive_packet(m_codecCtx, packet.get());
                //WRITE_LOG("Encoding"); ////判断编码循环是否正常时开启
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {////数据空或编码结束
                    break;
                } else if (ret <0) {
                    emit errorOccurred("Error receiving packet from encoder.");
                    break;
                }
                packet->stream_index = (m_mediaType == AVMEDIA_TYPE_VIDEO) ?0 :1;
                m_packetQueue->enqueue(std::move(packet));
            }
        }
    }

    work_guard();
    if (m_isEncoding) {
        QMetaObject::invokeMethod(this, "doEncodingWork", Qt::QueuedConnection);
    } else {
        // 如果在处理过程中收到了停止信号，则冲洗编码器
        flushEncoder();
        WRITE_LOG("Encoding loop finished for %s", (m_mediaType == AVMEDIA_TYPE_VIDEO ? "video" : "audio"));
    }
}

void ffmpegEncoder::flushEncoder() {
    WRITE_LOG("Flushing encoder for %s...", (m_mediaType == AVMEDIA_TYPE_VIDEO ? "video" : "audio"));
    if (!m_codecCtx) return;

    // 发送一个 NULL frame 来触发冲洗
    int ret = avcodec_send_frame(m_codecCtx, nullptr);
    if (ret < 0) {
        emit errorOccurred("Error sending flush frame to encoder.");
        return;
    }
    while (ret >= 0) {
        AVPacketPtr packet(av_packet_alloc());
        ret = avcodec_receive_packet(m_codecCtx, packet.get());
        if (ret == AVERROR_EOF) {
            WRITE_LOG("Encoder flushed successfully.");
            break;
        } else if (ret < 0) {
            emit errorOccurred("Error receiving packet from encoder during flush.");
            break;
        }
        packet->stream_index = (m_mediaType == AVMEDIA_TYPE_VIDEO) ? 0 : 1;
        m_packetQueue->enqueue(std::move(packet));
    }
}

void ffmpegEncoder::clear() {
    if (m_isEncoding) {
    } else {
        stopEncoding();
    } {
        QMutexLocker locker(&m_workMutex);
        while (m_isDoingWork) {
            m_workCond.wait(&m_workMutex);
        }
    }

    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    WRITE_LOG("ffmpegEncoder cleared.");
}
