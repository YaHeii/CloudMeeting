#include "Capture.h"
#include "logqueue.h"
#include "log_global.h"
#include "libavutil/time.h"

bool ffmpegInputInitialized = false;

Capture::Capture(QUEUE_DATA<AVPacketPtr>* videoQueue, QUEUE_DATA<AVPacketPtr>* audioQueue, QObject* parent)
    : QObject(parent), m_videoPacketQueue(videoQueue), m_audioPacketQueue(audioQueue)
{
    initializeFFmpeg();
}
void Capture::initializeFFmpeg() {
    if (!ffmpegInputInitialized) {
        avdevice_register_all();
        ffmpegInputInitialized = true;
        WRITE_LOG("FFmpeg devices registered.");
    }
}

Capture::~Capture() {
    closeDevice(); // closeDevice 现在会干净地关闭两者
    WRITE_LOG("VideoCapture destroyed.");
}


void Capture::openAudio(const QString& audioDeviceName) {
    WRITE_LOG("Opening audio device:", audioDeviceName);
    if (m_isAudioOpen) {
        WRITE_LOG("Audio device already open.");
        return;
    }
    const AVInputFormat* inputFormat = av_find_input_format("dshow");
    AVDictionary* options = nullptr;
    QString deviceUrl = QString("audio=%1").arg(audioDeviceName);
    av_dict_set(&options, "rtbufsize", "10000000", 0);

    if (!inputFormat) {
        emit errorOccurred("No inputFormat provided.");
        return;
    }

    m_AudioFormatCtx = avformat_alloc_context();
    int ret = avformat_open_input(&m_AudioFormatCtx, deviceUrl.toStdString().c_str(), inputFormat, &options);
    av_dict_free(&options);

    if (ret < 0) {
        char errbuf[1024] = { 0 };
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit errorOccurred(QString("Failed to open audio device: %1").arg(errbuf));
        avformat_close_input(&m_AudioFormatCtx);
        m_AudioFormatCtx = nullptr;
        return;
    }

    if (avformat_find_stream_info(m_AudioFormatCtx, nullptr) < 0) {
        emit errorOccurred("Failed to find audio stream information.");
        avformat_close_input(&m_AudioFormatCtx);
        m_AudioFormatCtx = nullptr;
        return;
    }

    m_audioStreamIndex = av_find_best_stream(m_AudioFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_audioStreamIndex < 0) {
        emit errorOccurred("Failed to find audio stream in device.");
        avformat_close_input(&m_AudioFormatCtx);
        m_AudioFormatCtx = nullptr;
        return;
    }

    m_aParams = avcodec_parameters_alloc();
    avcodec_parameters_copy(m_aParams, m_AudioFormatCtx->streams[m_audioStreamIndex]->codecpar);
    m_aTimeBase = m_AudioFormatCtx->streams[m_audioStreamIndex]->time_base;

    m_audioDeviceName = audioDeviceName;
    m_isAudioOpen = true;
    m_audioStartTime = av_gettime();
    WRITE_LOG("Audio device opened successfully.Audio stream Index:", m_audioStreamIndex);

    startAudioReading();
    emit audioDeviceOpenSuccessfully(m_aParams, m_aTimeBase);
}

void Capture::openVideo(const QString &VideoDeviceName) {
    WRITE_LOG("Opening video device:", VideoDeviceName);
    if (m_isVideoOpen) {
        WRITE_LOG("Video device already open.");
        return;
    }

    const AVInputFormat* inputFormat = av_find_input_format("dshow");
    AVDictionary* options = nullptr;
    QString deviceUrl = QString("video=%1").arg(VideoDeviceName);
    av_dict_set(&options, "rtbufsize", "10000000", 0);

    if (!inputFormat) {
        emit errorOccurred("No inputFormat provided.");
        return;
    }

    m_VideoFormatCtx = avformat_alloc_context();
    int ret = avformat_open_input(&m_VideoFormatCtx, deviceUrl.toStdString().c_str(), inputFormat, &options);
    av_dict_free(&options);

    if (ret < 0) {
        char errbuf[1024] = { 0 };
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit errorOccurred(QString("Failed to open video device: %1").arg(errbuf));
        avformat_close_input(&m_VideoFormatCtx);
        m_VideoFormatCtx = nullptr;
        return;
    }

    if (avformat_find_stream_info(m_VideoFormatCtx, nullptr) < 0) {
        emit errorOccurred("Failed to find video stream information.");
        avformat_close_input(&m_VideoFormatCtx);
        m_VideoFormatCtx = nullptr;
        return;
    }

    m_videoStreamIndex = av_find_best_stream(m_VideoFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStreamIndex < 0) {
        emit errorOccurred("Failed to find video stream in device.");
        avformat_close_input(&m_VideoFormatCtx);
        m_VideoFormatCtx = nullptr;
        return;
    }

    m_vParams = avcodec_parameters_alloc();
    avcodec_parameters_copy(m_vParams, m_VideoFormatCtx->streams[m_videoStreamIndex]->codecpar);
    m_vTimeBase = m_VideoFormatCtx->streams[m_videoStreamIndex]->time_base;

    m_videoDeviceName = VideoDeviceName;
    m_isVideoOpen = true;
    m_videoStartTime = av_gettime();
    WRITE_LOG("Video device opened successfully.");
    startVideoReading();
    emit videoDeviceOpenSuccessfully(m_vParams, m_vTimeBase);
}

void Capture::closeAudio() {
    WRITE_LOG("Closing audio device.");
    if (!m_isAudioOpen) return;

    stopAudioReading(); // 停止读取循环

    if (m_AudioFormatCtx) {
        avformat_close_input(&m_AudioFormatCtx);
        m_AudioFormatCtx = nullptr;
    }
    if (m_aParams) {
        avcodec_parameters_free(&m_aParams);
        m_aParams = nullptr;
    }

    m_isAudioOpen = false;
    m_audioStreamIndex = -1;
    m_aTimeBase = { 0, 1 };
    WRITE_LOG("Audio device closed.");

}

void Capture::closeVideo() {
    WRITE_LOG("Closing video device.");
    if (!m_isVideoOpen) return;

    stopVideoReading(); // 停止读取循环

    if (m_VideoFormatCtx) {
        avformat_close_input(&m_VideoFormatCtx);
        m_VideoFormatCtx = nullptr;
    }
    if (m_vParams) {
        avcodec_parameters_free(&m_vParams);
        m_vParams = nullptr;
    }

    m_isVideoOpen = false;
    m_videoStreamIndex = -1;
    m_vTimeBase = { 0, 1 };
    WRITE_LOG("Video device closed.");
}
void Capture::closeDevice() {
    closeAudio();
    closeVideo();
}

// 提供统一接口
// TODO: How to use it
void Capture::configReadingStatus(bool openVideo, bool openAudio) {
    m_isVideoOpen = openVideo;
    m_isAudioOpen = openAudio;
    if (m_isVideoOpen && !m_isReadingVideo) {
        startVideoReading();
    }
    if (m_isAudioOpen && !m_isReadingAudio) {
        startAudioReading();
    }
    if (m_isAudioOpen && m_isVideoOpen) {
        startVideoReading();
        startAudioReading();
    }
    // false情况，doRead中的if分支会自动退出
}
void Capture::stopReading() { 
    stopAudioReading();
    stopVideoReading();
}
void Capture::startVideoReading() {
    if (!m_VideoFormatCtx) {
        WRITE_LOG("Failed to start video reading - format context is null.");
        return;
    }
    if (m_isReadingVideo) {
        WRITE_LOG("Already reading video frames.");
        return;
    }
    m_isReadingVideo = true;
    WRITE_LOG("Starting to read video frames...");
    QMetaObject::invokeMethod(this, "doReadVideoFrame", Qt::QueuedConnection);
}

void Capture::stopVideoReading() {
    m_isReadingVideo = false;
    // 等待 doReadVideoFrame 循环结束
    {
        QMutexLocker locker(&m_videoWorkMutex);
        while (m_isDoingVideoWork) {
            m_videoWorkCond.wait(&m_videoWorkMutex);
        }
    }
}

void Capture::doReadVideoFrame() {
    if (!m_isReadingVideo.load()) {
        WRITE_LOG("Video reading loop gracefully stopped.");
        return;
    }
    {
        QMutexLocker locker(&m_videoWorkMutex);
        m_isDoingVideoWork = true;
    }
    auto work_guard = [this]() {
        QMutexLocker locker(&m_videoWorkMutex);
        m_isDoingVideoWork = false;
        m_videoWorkCond.wakeAll();
        };
    if (!m_videoPacketQueue) {
        WRITE_LOG("Video packet queue NOT SET.");
        work_guard();
        return;
    }

    AVPacketPtr packet(av_packet_alloc());
    if (!packet) {
        emit errorOccurred("Failed to allocate video packet.");
        m_isReadingVideo = false;
        work_guard();
        return;
    }

    int ret = av_read_frame(m_VideoFormatCtx, packet.get());
    if (ret < 0) {
        if (ret != AVERROR_EOF) { // EOF 可能是正常的
            char errbuf[1024] = { 0 };
            av_strerror(ret, errbuf, sizeof(errbuf));
            WRITE_LOG("Failed to read video frame: %s", errbuf);
        }
        m_isReadingVideo = false;
        work_guard();
        return;
    }

    if (packet->pts == AV_NOPTS_VALUE) {
        int64_t now_time = av_gettime();
        int64_t pts_in_us = now_time - m_videoStartTime;
        packet->pts = av_rescale_q(pts_in_us, { 1, 1000000 }, m_vTimeBase);
        packet->dts = packet->pts;
    }

    if (packet->stream_index == m_videoStreamIndex) {
        m_videoPacketQueue->enqueue(std::move(packet));
        WRITE_LOG("Video frame read.");
    }

    work_guard();
    if (m_isReadingVideo) {
        QMetaObject::invokeMethod(this, "doReadVideoFrame", Qt::QueuedConnection);
    }
}
void Capture::startAudioReading() {
    if (!m_AudioFormatCtx) {
        WRITE_LOG("Failed to start audio reading - format context is null.");
        return;
    }
    if (m_isReadingAudio) {
        WRITE_LOG("Already reading audio frames.");
        return;
    }
    WRITE_LOG("Starting to read audio frames...");
    m_isReadingAudio = true;
    QMetaObject::invokeMethod(this, "doReadAudioFrame", Qt::QueuedConnection);
}

void Capture::stopAudioReading() {
    m_isReadingAudio = false;
    // 等待 doReadAudioFrame 循环结束
    {
        QMutexLocker locker(&m_audioWorkMutex);
        while (m_isDoingAudioWork) {
            m_audioWorkCond.wait(&m_audioWorkMutex);
        }
    }
}

void Capture::doReadAudioFrame() {
    if (!m_isReadingAudio.load()) {
        WRITE_LOG("Audio reading loop gracefully stopped.");
        return;
    }
    {
        QMutexLocker locker(&m_audioWorkMutex);
        m_isDoingAudioWork = true;
    }
    auto work_guard = [this]() {
        QMutexLocker locker(&m_audioWorkMutex);
        m_isDoingAudioWork = false;
        m_audioWorkCond.wakeAll();
        };

    if (!m_audioPacketQueue) {
        WRITE_LOG("Audio packet queue NOT SET.");
        work_guard();
        return;
    }

    AVPacketPtr packet(av_packet_alloc());
    if (!packet) {
        emit errorOccurred("Failed to allocate audio packet.");
        m_isReadingAudio = false;
        work_guard();
        return;
    }

    int ret = av_read_frame(m_AudioFormatCtx, packet.get());
    if (ret < 0) {
        if (ret != AVERROR_EOF) {
            char errbuf[1024] = { 0 };
            av_strerror(ret, errbuf, sizeof(errbuf));
            WRITE_LOG("Failed to read audio frame: %s", errbuf);
        }
        m_isReadingAudio = false;
        work_guard();
        return;
    }

    if (packet->pts == AV_NOPTS_VALUE) {
        int64_t now_time = av_gettime();
        int64_t pts_in_us = now_time - m_audioStartTime;
        packet->pts = av_rescale_q(pts_in_us, { 1, 1000000 }, m_aTimeBase);
        packet->dts = packet->pts;
    }

    if (packet->stream_index == m_audioStreamIndex) {
        m_audioPacketQueue->enqueue(std::move(packet));
    }

    work_guard();
    if (m_isReadingAudio) {
        QMetaObject::invokeMethod(this, "doReadAudioFrame", Qt::QueuedConnection);
    }
}
