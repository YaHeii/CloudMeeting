#include "ffmpegAudioDecoder.h"
#include "ffmpegAudioDecoder.h"
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QMediaDevices>
#include <QDebug>

#include "logqueue.h"
#include "log_global.h"

ffmpegAudioDecoder::ffmpegAudioDecoder(QUEUE_DATA<AVPacketPtr> *packetQueue, QUEUE_DATA<AVFramePtr> *frameQueue,
                                       QObject *parent)
    : QObject{parent}, m_packetQueue(packetQueue), m_frameQueue(frameQueue) {
    m_ResampleConfig.frame_size = 960;//BUG:AAC和opus对帧大小的要求不一致，考虑设置全局变量  AAC(RTMP):1024 opus(WebRTC):960
    m_ResampleConfig.sample_rate = 48000;
    m_ResampleConfig.ch_layout = AV_CHANNEL_LAYOUT_MONO;
    m_ResampleConfig.sample_fmt = AV_SAMPLE_FMT_FLTP;
}

ffmpegAudioDecoder::~ffmpegAudioDecoder() {
    stopDecoding();
    clear();
}

bool ffmpegAudioDecoder::init(AVCodecParameters *params, AVRational inputTimeBase) {
    if (!params) {
        WRITE_LOG("Audio codec not found");
		return false;
    }
    m_codec = avcodec_find_decoder(params->codec_id);
    if (!m_codec) {
		WRITE_LOG("decodeCapture failed for codec id: %d", params->codec_id);
        return false;
    }
    m_codecCtx = avcodec_alloc_context3(m_codec);
    if (avcodec_parameters_to_context(m_codecCtx, params) < 0) {
        WRITE_LOG("avcodec_parameters_to_context failed");  
        return false;
    }
    if (avcodec_open2(m_codecCtx, m_codec, nullptr) < 0) {
        WRITE_LOG("avcodec_open2 failed");
        return false;
    }
    
    WRITE_LOG("Audio Decoder initialized successfully.");
	m_inputTimeBase = inputTimeBase;

    if (m_swrCtx) swr_free(&m_swrCtx);

    swr_alloc_set_opts2(&m_swrCtx,
        &m_ResampleConfig.ch_layout,    // 目标通道布局
        m_ResampleConfig.sample_fmt,  // 目标采样格式
        m_ResampleConfig.sample_rate, // 目标采样率
        &m_codecCtx->ch_layout, // 源通道布局
        m_codecCtx->sample_fmt, // 源采样格式
        m_codecCtx->sample_rate, // 源采样率
        0, nullptr);
    if (!m_swrCtx || swr_init(m_swrCtx) < 0) {
        emit errorOccurred("Failed to initialize audio resampler in init.");
        return false;
    }

    if (m_fifo) av_audio_fifo_free(m_fifo);
    m_fifo = av_audio_fifo_alloc(m_ResampleConfig.sample_fmt, m_ResampleConfig.ch_layout.nb_channels, 1);
    if (!m_fifo) {
        emit errorOccurred("Failed to allocate audio FIFO in init.");
        return false;
    }
    m_fifoBasePts = AV_NOPTS_VALUE;

    WRITE_LOG("Audio Decoder initialized successfully and is ready to resample.");
    return true;
}

void ffmpegAudioDecoder::ChangeDecodingState(bool isEncoding) {
    m_isDecoding = isEncoding;
    if (m_isDecoding) {
        startDecoding();
    }
    else {
        stopDecoding();
    }
}

void ffmpegAudioDecoder::startDecoding() {
    QMetaObject::invokeMethod(this, "doDecodingPacket", Qt::QueuedConnection);
    WRITE_LOG("Audio decoding process started.");
}

void ffmpegAudioDecoder::stopDecoding() {
    WRITE_LOG("Audio decoding process stopped.");
}

void ffmpegAudioDecoder::doDecodingPacket() {
    if (!m_isDecoding) {
        WRITE_LOG("Audio decoding loop finished.");
        return;
    }

    AVPacketPtr packet;
    if (!m_packetQueue->dequeue(packet)) {
        WRITE_LOG("ffmpegAudioDecoder::Deque Packet TimeOut");
        if (m_isDecoding) {
            QMetaObject::invokeMethod(this, "doDecodingPacket", Qt::QueuedConnection);
        }
        return;
    }
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
    AVFramePtr resampledFrame(av_frame_alloc());
    if (!decoded_frame || !resampledFrame) {
        emit errorOccurred("ffmpegAudioDecoder::Failed to allocate frame");
        m_isDecoding = false;
        return;
    }
    if (packet->size <= 0) {
        WRITE_LOG("Warning: Received empty audio packet.");
    }
    int ret = avcodec_send_packet(m_codecCtx, packet.get());
    if (ret < 0) {
        char errbuf[1024] = { 0 };
        av_strerror(ret, errbuf, sizeof(errbuf));
        WRITE_LOG("Error: avcodec_send_packet failed: %s", errbuf);
    }
    while (true) {
        ret = avcodec_receive_frame(m_codecCtx, decoded_frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        else if (ret < 0) {
            WRITE_LOG("Error: avcodec_receive_frame failed: %s", ret);
            break;
        }

        ////检测音频通道布局,后期删除
        if (decoded_frame->ch_layout.nb_channels > 0 &&
            (decoded_frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || decoded_frame->ch_layout.u.mask == 0)) {

            av_channel_layout_default(&decoded_frame->ch_layout, decoded_frame->ch_layout.nb_channels);
            // WRITE_LOG("Fixed audio channel layout for DirectShow input.");
        }


        // 设置BasePts
        //如果FIFO是空的，那么这个解码帧是音频流的起始帧，那么设置PTS为后续基准PTS
        if (m_fifoBasePts == AV_NOPTS_VALUE && decoded_frame->pts != AV_NOPTS_VALUE) {
            if (av_audio_fifo_size(m_fifo) == 0) {
                m_fifoBasePts = decoded_frame->pts;
                WRITE_LOG("Audio Decoder Base PTS set to: %lld", m_fifoBasePts);
            }
        }
        if (!m_swrCtx) {
            swr_alloc_set_opts2(&m_swrCtx,
                &m_ResampleConfig.ch_layout,
                m_ResampleConfig.sample_fmt,
                m_ResampleConfig.sample_rate,
                &decoded_frame->ch_layout, // [关键] 使用 decoded_frame 的真实布局
                (enum AVSampleFormat)decoded_frame->format, // [关键] 使用 decoded_frame 的真实格式
                decoded_frame->sample_rate, // [关键] 使用 decoded_frame 的真实采样率
                0, nullptr);

            if (swr_init(m_swrCtx) < 0) {
                WRITE_LOG("FATAL: Failed to init SwrContext with frame params.");
                av_frame_unref(decoded_frame.get());
                continue;
            }
            WRITE_LOG("SwrContext initialized with Frame: %d Hz, Fmt: %d", decoded_frame->sample_rate, decoded_frame->format);
        }

        resampledFrame->ch_layout = m_ResampleConfig.ch_layout;
        resampledFrame->sample_rate = m_ResampleConfig.sample_rate;
        resampledFrame->format = m_ResampleConfig.sample_fmt;

        ret = swr_convert_frame(m_swrCtx, resampledFrame.get(), decoded_frame.get());

        if (ret < 0) {
            // [!! 自动恢复 !!] 如果转换失败，很可能是格式变了。
            // 我们释放旧的 context，下次循环会通过上面的 if(!m_swrCtx) 重新创建
            WRITE_LOG("Error: swr_convert_frame failed (ret=%d). Re-initializing SwrContext...", ret);
            swr_free(&m_swrCtx);
            m_swrCtx = nullptr;

            av_frame_unref(decoded_frame.get());
            continue; // 跳过这一帧，下一次会重新初始化
        }


        // --- 写入FIFO ---
        if (resampledFrame->nb_samples > 0) {
            int written = av_audio_fifo_write(m_fifo, (void**)resampledFrame->data, resampledFrame->nb_samples);
            if (written < resampledFrame->nb_samples) {
                WRITE_LOG("Warning: FIFO write truncated (Queue full?).");
            }
        }
        av_frame_unref(resampledFrame.get());
        // --- 读取固定帧 ---
        while (av_audio_fifo_size(m_fifo) >= m_ResampleConfig.frame_size) {
            AVFramePtr sendFrame(av_frame_alloc());
            sendFrame->pts = m_fifoBasePts;
            // 设置帧参数
            sendFrame->nb_samples = m_ResampleConfig.frame_size;
            sendFrame->ch_layout = m_ResampleConfig.ch_layout;
            sendFrame->format = m_ResampleConfig.sample_fmt;
            sendFrame->sample_rate = m_ResampleConfig.sample_rate;
            sendFrame->pts = m_fifoBasePts;

            if (av_frame_get_buffer(sendFrame.get(), 0) < 0) {
                WRITE_LOG("Error: Failed to allocate buffer for sendFrame.");
                break;
            }

            if (av_audio_fifo_read(m_fifo, (void**)sendFrame->data, m_ResampleConfig.frame_size) < m_ResampleConfig.frame_size) {
                WRITE_LOG("Error: FIFO read failed.");
                break;
            };

            // --- 计算PTS ---
            int64_t duration = av_rescale_q(sendFrame->nb_samples,
                { 1, m_ResampleConfig.sample_rate },
                m_inputTimeBase);
            m_fifoBasePts += duration;
            //WRITE_LOG("Audio Decoder: Enqueuing frame (Size: %d)", sendFrame->nb_samples);
            m_frameQueue->enqueue(std::move(sendFrame));
        }
        av_frame_unref(decoded_frame.get());
        av_frame_unref(resampledFrame.get());
    }
    work_guard();

    if (m_isDecoding) {
        QMetaObject::invokeMethod(this, "doDecodingPacket", Qt::QueuedConnection);

    }
}


void ffmpegAudioDecoder::clear() {
    stopDecoding();
    {
        QMutexLocker locker(&m_workMutex);
        while (m_isDoingWork) {
            m_workCond.wait(&m_workMutex);
        }
    }

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
    WRITE_LOG("Audio decoder cleared successfully.");
}
