#include "RtmpPublisher.h"

#include "logqueue.h"
#include "log_global.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavutil/time.h"
}

RtmpPublisher::RtmpPublisher(QUEUE_DATA<AVPacketPtr> *encodedPacketQueue, QObject *parent)
    : QObject{parent}, m_encodedPacketQueue(encodedPacketQueue) {
}

RtmpPublisher::~RtmpPublisher() {
    clear();
}

bool RtmpPublisher::init(const QString &rtmpUrl, AVCodecContext *vCodecCtx, AVCodecContext *aCodecCtx) {
    avformat_network_init();
    if (m_outputFmtCtx) {
		WRITE_LOG("Publisher already initialized. Clearing previous state.");
        clear();
		return false;
    }
    if (!vCodecCtx && !aCodecCtx) {
        WRITE_LOG("No codec parameters provided.");
        clear();
        return false;
    }

    int ret = avformat_alloc_output_context2(&m_outputFmtCtx, nullptr, "flv", rtmpUrl.toStdString().c_str());
    if (ret < 0 || !m_outputFmtCtx) {
        WRITE_LOG("Failed to allocate output context.");
		clear();
        return false;
    }

    if (vCodecCtx) {
        m_videoStream = avformat_new_stream(m_outputFmtCtx, nullptr);
        if (!m_videoStream) {
            WRITE_LOG("Fail to create videoStream");
            clear();
            return false;
        }
        avcodec_parameters_from_context(m_videoStream->codecpar, vCodecCtx);
        m_videoStream->codecpar->codec_tag = 0;
        // Ensure stream time_base is set from encoder context so muxer/interleaving work correctly
        //m_videoStream->time_base = vCodecCtx->time_base;
        m_videoEncoderTimeBase = vCodecCtx->time_base; //设置编码器时间基
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
        // Set audio stream time_base from encoder
        //m_audioStream->time_base = aCodecCtx->time_base;
        m_audioEncoderTimeBase = aCodecCtx->time_base;
    }
    // --- 打开IO ---
    if (!(m_outputFmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_outputFmtCtx->pb, rtmpUrl.toStdString().c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            char errbuf[1024] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            emit errorOccurred(QString("Publisher Error: Failed to open RTMP URL: %1").arg(errbuf));
			WRITE_LOG("Failed to open RTMP URL: %s", errbuf);
            clear();
            return false;
        }
    }


    // --- 写入文件头 ---
    ret = avformat_write_header(m_outputFmtCtx, nullptr);
    if (ret < 0) {
        char errbuf[1024] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit errorOccurred(QString("Publisher Error: Fail to write RTMP header: %1 (ret=%2)").arg(errbuf).arg(ret));
        WRITE_LOG("Failed to write RTMP header: %s (ret=%d)", errbuf, ret);
        clear();
        return false;
    }
    if (m_videoStream) {
        WRITE_LOG("Video stream time_base: %d/%d (encoder: %d/%d)",
            m_videoStream->time_base.num, m_videoStream->time_base.den,
            m_videoEncoderTimeBase.num, m_videoEncoderTimeBase.den);
    }
    if (m_audioStream) {
        WRITE_LOG("Audio stream time_base: %d/%d (encoder: %d/%d)",
            m_audioStream->time_base.num, m_audioStream->time_base.den,
            m_audioEncoderTimeBase.num, m_audioEncoderTimeBase.den);
    }
    WRITE_LOG("RTMP publisher initialized. Connected to %s", rtmpUrl.toStdString().c_str());
    return true;
}

void RtmpPublisher::ChangeRtmpPublishingState(bool isPublishing) {
    m_isPublishing = isPublishing;
    if (m_isPublishing) {
        if (!m_outputFmtCtx) {
            WRITE_LOG("Publisher already started or not initialized.");
            return;
        }
        startPublishing();
    } else {
        stopPublishing();
    }
}

void RtmpPublisher::startPublishing() {

    emit publisherStarted();
    // 使用事件调用启动推流
    QMetaObject::invokeMethod(this, "doPublishingWork", Qt::QueuedConnection);
    WRITE_LOG("RTMP publishing process started...");
}

void RtmpPublisher::stopPublishing() {
    WRITE_LOG("Stopping RTMP publishing process...");
}

void RtmpPublisher::doPublishingWork() {
    if (!m_isPublishing) {
        if (m_outputFmtCtx) {
            av_write_trailer(m_outputFmtCtx);
        }
        WRITE_LOG("RTMP publishing loop finished.");
        emit publisherStopped();
        return;
    } {
        QMutexLocker locker(&m_workMutex);
        m_isDoingWork = true;
    }
    auto work_guard = [this]() {
        QMutexLocker locker(&m_workMutex);
        m_isDoingWork = false;
        m_workCond.wakeAll();
     };
    AVPacketPtr packet;
    if (!m_encodedPacketQueue->dequeue(packet)) {
        if (m_isPublishing) {
            // 只是暂时为空，继续调度下一次尝试
            work_guard();
            QMetaObject::invokeMethod(this, "doPublishingWork", Qt::QueuedConnection);
        }
        else {
            // 确认停止，写入文件尾并结束
            if (m_outputFmtCtx) {
                av_write_trailer(m_outputFmtCtx);
            }
            work_guard();
            WRITE_LOG("RTMP publishing loop finished.");
            emit publisherStopped();
        }
        return;
    }

    AVStream* dest_stream = nullptr;
    AVRational source_time_base;

    // 判断包的类型，并设置好目标流和源时间基
    if (packet->stream_index == 0 && m_videoStream) {
        // 视频流
        dest_stream = m_videoStream;
        source_time_base = m_videoEncoderTimeBase;
    }
    else if (packet->stream_index == 1 && m_audioStream) {
        // 音频流
        dest_stream = m_audioStream;
        source_time_base = m_audioEncoderTimeBase;
    }
    else {
        WRITE_LOG("Unknown packet stream_index: %d", packet->stream_index);
        work_guard();
        // 调度下一次，不能因为一个未知包就停止整个推流
        if (m_isPublishing) QMetaObject::invokeMethod(this, "doPublishingWork", Qt::QueuedConnection);
        return;
    }

    // --- 时间基转换 ---
    // 将packet的PTS/DTS从编码器的时间基(source_time_base)转换到输出流的时间基(dest_stream->time_base)
    av_packet_rescale_ts(packet.get(), source_time_base, dest_stream->time_base);
    packet->pos = -1;
    if (packet->data == nullptr) {
        WRITE_LOG("Packet data is null, skipping frame.");
        work_guard(); // 释放锁
        // 调度下一次，不能因为一个空包就停止整个推流
        if (m_isPublishing) QMetaObject::invokeMethod(this, "doPublishingWork", Qt::QueuedConnection);
        return; // 立即返回，不要处理这个坏包
    }
    bool is_video = (dest_stream == m_videoStream);
    const char* media_type = is_video ? "VIDEO" : "AUDIO";
    //qDebug() << "Attempting to write frame:"
    //    << media_type
    //    << "PTS:" << packet->pts
    //    << "DTS:" << packet->dts
    //    << "Size:" << packet->size
    //    << "Stream:" << packet->stream_index
    //    << "Keyframe:" << ((packet->flags & AV_PKT_FLAG_KEY) ? "Yes" : "No");

    int ret = av_interleaved_write_frame(m_outputFmtCtx, packet.get());
    if (ret < 0) {
        char errbuf[1024] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        WRITE_LOG("Error writing frame to RTMP stream: %s", errbuf);

        WRITE_LOG("======= ERROR WRITING FRAME (ret=%d: %s) =======", ret, errbuf);
        WRITE_LOG("Packet Media Type: %s", media_type);
        WRITE_LOG("Packet Stream Index: %d (Target Stream Index: %d)", packet->stream_index, dest_stream->index);

        // --- 时间戳诊断 ---
        WRITE_LOG("Packet PTS: %lld", packet->pts);
        WRITE_LOG("Packet DTS: %lld", packet->dts);
        WRITE_LOG("Packet Duration: %lld", packet->duration);

        // --- 时间基诊断 ---
        WRITE_LOG("Source Encoder Time Base: %d/%d", source_time_base.num, source_time_base.den);
        WRITE_LOG("Target Stream Time Base: %d/%d", dest_stream->time_base.num, dest_stream->time_base.den);

        // --- 标志和大小诊断 ---
        WRITE_LOG("Packet Size: %d bytes", packet->size);
        WRITE_LOG("Packet Flags: %s", (packet->flags & AV_PKT_FLAG_KEY) ? "KEYFRAME" : "Not a keyframe");

        // --- 关键性检查 ---
        if (packet->dts < 0) {
            WRITE_LOG("CRITICAL: Packet DTS is negative (%lld). This is almost always invalid after rescale!", packet->dts);
        }
        if (is_video && !(packet->flags & AV_PKT_FLAG_KEY)) {
            WRITE_LOG("INFO: Writing a NON-KEYFRAME video packet. Did we send a keyframe first?");
        }
        if (is_video && packet->dts > packet->pts) {
            WRITE_LOG("WARNING: Video Packet DTS (%lld) > PTS (%lld). This is impossible if you disabled B-frames (max_b_frames = 0).", packet->dts, packet->pts);
        }

        WRITE_LOG("====================================================");


        emit errorOccurred("Failed to write frame, publisher may be disconnected.");
        if (m_outputFmtCtx) {
            av_write_trailer(m_outputFmtCtx);
        }
        work_guard();
        emit publisherStopped();
        return; // 错误发生，终止事件链
    }
    work_guard();
    if (m_isPublishing) {
        QMetaObject::invokeMethod(this, "doPublishingWork", Qt::QueuedConnection);
    } else {
        // 推完最后一个包后被告知停止
        if (m_outputFmtCtx) {
            av_write_trailer(m_outputFmtCtx);
        }
        WRITE_LOG("RTMP publishing loop finished.");
        emit publisherStopped();
    }
}

/// <summary>
/// 彻底释放对象时调用
/// </summary>
void RtmpPublisher::clear() {
    stopPublishing();
    //增加同步等待
    {
        QMutexLocker locker(&m_workMutex);
        while (m_isDoingWork) {
            m_workCond.wait(&m_workMutex);
        }
    }

    if (m_outputFmtCtx) {
        if (!(m_outputFmtCtx->oformat->flags & AVFMT_NOFILE) && m_outputFmtCtx->pb) {
            avio_closep(&m_outputFmtCtx->pb);
        }
        avformat_free_context(m_outputFmtCtx);
        m_outputFmtCtx = nullptr;
    }
    m_videoStream = nullptr;
    m_audioStream = nullptr;
    WRITE_LOG("RtmpPublisher cleared.");
}
