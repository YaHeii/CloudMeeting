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
    m_ResampleConfig.frame_size = 1024;
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
    if (avcodec_send_packet(m_codecCtx, packet.get()) == 0) {
        while (avcodec_receive_frame(m_codecCtx, decoded_frame.get()) == 0) {
            if (!m_isDecoding) break;
            // 解码成功，进行重采样
            resampledFrame->ch_layout = m_ResampleConfig.ch_layout;
            resampledFrame->sample_rate = m_ResampleConfig.sample_rate;
            resampledFrame->format = m_ResampleConfig.sample_fmt;

            // 设置BasePts
            //如果FIFO是空的，那么这个解码帧是音频流的起始帧，那么设置PTS为后续基准PTS
            if (m_fifoBasePts == AV_NOPTS_VALUE && decoded_frame->pts != AV_NOPTS_VALUE) {
                if (av_audio_fifo_size(m_fifo) == 0) {
                    m_fifoBasePts = decoded_frame->pts;
                }
            }

            if (swr_convert_frame(m_swrCtx, resampledFrame.get(), decoded_frame.get()) < 0) continue;

            // --- 写入FIFO ---
            av_audio_fifo_write(m_fifo, (void **) resampledFrame->data, resampledFrame->nb_samples);

            // --- 读取固定帧 ---
            while (av_audio_fifo_size(m_fifo) >= m_ResampleConfig.frame_size) {
                AVFramePtr sendFrame(av_frame_alloc());
                sendFrame->pts = m_fifoBasePts;
                // 设置帧参数
                sendFrame->nb_samples = m_ResampleConfig.frame_size;
                sendFrame->ch_layout = m_ResampleConfig.ch_layout;
                sendFrame->format = m_ResampleConfig.sample_fmt;
                sendFrame->sample_rate = m_ResampleConfig.sample_rate;

                if (av_frame_get_buffer(sendFrame.get(), 0) < 0) break;

                av_audio_fifo_read(m_fifo, (void **) sendFrame->data, m_ResampleConfig.frame_size);

                // --- 计算PTS ---
                int64_t duration = av_rescale_q(sendFrame->nb_samples,
                    { 1, m_ResampleConfig.sample_rate },
                    m_inputTimeBase);
                m_fifoBasePts += duration;
                // --- 入队给编码器 ---
                if(m_isDecoding){
                    m_frameQueue->enqueue(std::move(sendFrame));
				}
            }
        }
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
