#include "VideoCapture.h"

#include "logqueue.h"
#include "log_global.h"
#include "libavutil/time.h"

bool ffmpegInputInitialized = false;
void VideoCapture::initializeFFmpeg() {
    if (!ffmpegInputInitialized) {
        avdevice_register_all();
        ffmpegInputInitialized = true;
        WRITE_LOG("FFmpeg devices registered.");
    }
}

VideoCapture::VideoCapture(QObject* parent) : QObject(parent) {
    initializeFFmpeg();
    openDevice();
}

VideoCapture::~VideoCapture() {
    // 确保读取已停止
    stopReading();
    // 确保设备已关闭
    closeDevice();
    WRITE_LOG("VideoCapture destroyed.");
}

void VideoCapture::setPacketQueue(QUEUE_DATA<AVPacketPtr>* videoQueue, QUEUE_DATA<AVPacketPtr>* audioQueue)
{
    // 线程安全地设置队列指针
    QMutexLocker locker(&m_queueMutex);
    m_videoPacketQueue = videoQueue;
    m_audioPacketQueue = audioQueue;
}

void VideoCapture::openDevice(const QString &videoDeviceName, const QString &audioDeviceName) {
    if (m_FormatCtx) {
        closeDevice();
    }

    const AVInputFormat *inputFormat = nullptr;
    AVDictionary *options = nullptr;
    QString deviceUrl;

#ifdef Q_OS_WIN
    // Windows 使用 dshow
    inputFormat = av_find_input_format("dshow");
    if (!videoDeviceName.isEmpty() && !audioDeviceName.isEmpty()) {
        deviceUrl = QString("video=%1:audio=%2").arg(videoDeviceName).arg(audioDeviceName);
    } else if (!videoDeviceName.isEmpty()) {
        deviceUrl = QString("video=%1").arg(videoDeviceName);
    } else if (!audioDeviceName.isEmpty()) {
        deviceUrl = QString("audio=%1").arg(audioDeviceName);
    } else {
        emit errorOccurred("No device name provided for Windows.");
        return;
    }
    // 设置dshow参数以增加缓冲区大小，避免帧丢失
    av_dict_set(&options, "rtbufsize", "10000000", 0); // 设置为100MB缓冲区
    // 可以设置一些dshow参数，例如视频尺寸、帧率等
    // av_dict_set(&options, "video_size", "640x480", 0);
    // av_dict_set(&options, "framerate", "30", 0);
#elif defined(Q_OS_LINUX)
    // Linux 使用 v4l2 (video) 和 alsa (audio)
    // 注意：Linux下通常需要分开采集视频和音频
    if (!videoDeviceName.isEmpty()) {
        inputFormat = av_find_input_format("v4l2");
        deviceUrl = videoDeviceName; // 例如 "/dev/video0"
    } else if (!audioDeviceName.isEmpty()) {
        inputFormat = av_find_input_format("alsa");
        deviceUrl = audioDeviceName; // 例如 "hw:0"
    } else {
        emit errorOccurred("No device name provided for Linux.");
        return;
    }
#else
    emit errorOccurred("Unsupported operating system.");
    return;
#endif
    if (!inputFormat) {
        emit errorOccurred("No inputFormat provided.");
        return;
    }

    m_FormatCtx = avformat_alloc_context();
    int ret = avformat_open_input(&m_FormatCtx,deviceUrl.toStdString().c_str(),inputFormat,&options);
    av_dict_free(&options);
    if (ret < 0) {
        char errbuf[1024] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit errorOccurred(QString("Failed to open input device: %1").arg(errbuf));
        avformat_close_input(&m_FormatCtx); // 确保即使失败也清理
        m_FormatCtx = nullptr;
        return;
    }

    if (avformat_find_stream_info(m_FormatCtx, nullptr) < 0) {
        emit errorOccurred("Failed to find stream information.");
        closeDevice();
        return;
    }

    // 查找最佳视频和音频流
    m_videoStreamIndex = av_find_best_stream(m_FormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    m_audioStreamIndex = av_find_best_stream(m_FormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // 检查是否找到至少一个流
    if (m_videoStreamIndex < 0 && m_audioStreamIndex < 0) {
        emit errorOccurred("Failed to find any video or audio stream.");
        closeDevice();
        return;
    }

    // 记录找到的流信息
    WRITE_LOG("Device opened successfully.");
    if (m_videoStreamIndex >= 0) {
        WRITE_LOG("Video stream index:", m_videoStreamIndex);
        AVCodecParameters* vParams = m_FormatCtx->streams[m_videoStreamIndex]->codecpar;
        WRITE_LOG("Video codec: %1", QString("Codec ID: %1").arg(vParams->codec_id));
        WRITE_LOG("Video format: %1x%2", QString("%1x%2").arg(vParams->width).arg(vParams->height));
    } else {
        WRITE_LOG("No video stream found.");
    }
    
    if (m_audioStreamIndex >= 0) {
        WRITE_LOG("Audio stream index:", m_audioStreamIndex);
        AVCodecParameters* aParams = m_FormatCtx->streams[m_audioStreamIndex]->codecpar;
        WRITE_LOG("Audio codec: %1", QString("Codec ID: %1").arg(aParams->codec_id));
        // WRITE_LOG(QString("Audio channels: %1").arg(aParams->channels));
    } else {
        WRITE_LOG("No audio stream found.");
    }
    AVCodecParameters* vParams = (m_videoStreamIndex >= 0) ? m_FormatCtx->streams[m_videoStreamIndex]->codecpar : nullptr;
    AVCodecParameters* aParams = (m_audioStreamIndex >= 0) ? m_FormatCtx->streams[m_audioStreamIndex]->codecpar : nullptr;

    emit deviceOpenSuccessfully(vParams,aParams);
}

void VideoCapture::startReading() {
    // 线程安全地获取队列指针
    QUEUE_DATA<AVPacketPtr>* videoQueue = nullptr;
    QUEUE_DATA<AVPacketPtr>* audioQueue = nullptr;
    {
        QMutexLocker locker(&m_queueMutex);
        videoQueue = m_videoPacketQueue;
        audioQueue = m_audioPacketQueue;
    }
    
    if (!videoQueue || !audioQueue) {
        WRITE_LOG("Packet queue NOT SET.");
        return;
    }

    if (!m_FormatCtx) {
        WRITE_LOG("Failed to start reading - format context is null.");
        return;
    }
    
    WRITE_LOG("Starting to read frames...");
    int64_t startTime = av_gettime();
    m_isReading = true;
    while (m_isReading) {
        AVPacketPtr packet(av_packet_alloc());
        if (!packet) {
            WRITE_LOG("Failed to allocate packet.");
            emit errorOccurred("Failed to allocate packet.");
            break;
        }

        int ret = av_read_frame(m_FormatCtx, packet.get());
        if (ret < 0) { 
            // 检查是否是正常结束还是错误
            if (ret == AVERROR_EOF) {
                WRITE_LOG("End of file reached.");
            } else {
                char errbuf[1024] = {0};
                av_strerror(ret, errbuf, sizeof(errbuf));
                WRITE_LOG("Failed to read frame: %s", errbuf);
                emit errorOccurred(QString("Failed to read frame: %1").arg(errbuf));
            }
            break;
        }
        if (packet->pts == AV_NOPTS_VALUE) {
            // 获取当前时间
            int64_t now_time = av_gettime();
            // 计算从开始到现在的时长（微秒）
            int64_t pts_in_us = now_time - startTime;

            // 获取输入流的时间基 (例如 {1, 1000000})
            AVRational time_base = m_FormatCtx->streams[packet->stream_index]->time_base;

            // 将微秒单位的时间戳，转换为流的时间基单位
            packet->pts = av_rescale_q(pts_in_us, {1, 1000000}, time_base);
            packet->dts = packet->pts; // 视频会议，DTS=PTS
        }

        if (packet->stream_index == m_videoStreamIndex) {
            videoQueue->enqueue(std::move(packet));
        } else if (packet->stream_index == m_audioStreamIndex) {
            audioQueue->enqueue(std::move(packet));
        } else {

        }
    }
    m_isReading = false;
    WRITE_LOG("Stopping reading.");
}

void VideoCapture::stopReading() {
    m_isReading = false;
}

void VideoCapture::closeDevice()
{
    if (m_FormatCtx) {
        stopReading();
        avformat_close_input(&m_FormatCtx);
        m_FormatCtx = nullptr;
        m_videoStreamIndex = -1;
        m_audioStreamIndex = -1;
        WRITE_LOG("Device closed.");
    }
}

void VideoCapture::closeAudio() {
    if (m_FormatCtx && m_audioStreamIndex >= 0) {
        // 仅关闭音频流
        m_audioStreamIndex = -1;
        WRITE_LOG("Audio stream closed.");
    }
}
void VideoCapture::closeVideo() {
    if (m_FormatCtx && m_audioStreamIndex >= 0) {
        // 仅关闭音频流
        m_videoStreamIndex = -1;
        WRITE_LOG("Audio stream closed.");
    }
}

void VideoCapture::openAudio(const QString &audioDeviceName) {
    if (m_FormatCtx) {
        closeDevice();
    }

    const AVInputFormat *inputFormat = nullptr;
    AVDictionary *options = nullptr;
    QString deviceUrl;

#ifdef Q_OS_WIN
    // Windows 使用 dshow
    inputFormat = av_find_input_format("dshow");
    if (!audioDeviceName.isEmpty()) {
        deviceUrl = QString("audio=%1").arg(audioDeviceName);
    } else {
        emit errorOccurred("No audio device name provided for Windows.");
        return;
    }
    // 设置dshow参数以增加缓冲区大小，避免帧丢失
    av_dict_set(&options, "rtbufsize", "10000000", 0); // 设置为100MB缓冲区
#elif defined(Q_OS_LINUX)
    emit errorOccurred("Unsupported operating system.");
    return;
#endif

    if (!inputFormat) {
        emit errorOccurred("No input format provided for audio.");
        return;
    }

    m_FormatCtx = avformat_alloc_context();
    int ret = avformat_open_input(&m_FormatCtx, deviceUrl.toStdString().c_str(), inputFormat, &options);
    av_dict_free(&options);
    if (ret < 0) {
        char errbuf[1024] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit errorOccurred(QString("Failed to open audio input device: %1").arg(errbuf));
        avformat_close_input(&m_FormatCtx);
        m_FormatCtx = nullptr;
        return;
    }

    if (avformat_find_stream_info(m_FormatCtx, nullptr) < 0) {
        emit errorOccurred("Failed to find audio stream information.");
        closeDevice();
        return;
    }

    // 查找最佳音频流
    m_audioStreamIndex = av_find_best_stream(m_FormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (m_audioStreamIndex < 0) {
        emit errorOccurred("Failed to find any audio stream.");
        closeDevice();
        return;
    }

    WRITE_LOG("Audio device opened successfully.");
    WRITE_LOG("Audio stream index:", m_audioStreamIndex);
    AVCodecParameters* aParams = m_FormatCtx->streams[m_audioStreamIndex]->codecpar;
    WRITE_LOG("Audio codec: %1", QString("Codec ID: %1").arg(aParams->codec_id));

    // 视频流索引保持为-1，因为我们只打开了音频设备
    m_videoStreamIndex = -1;

    emit deviceOpenSuccessfully(nullptr, aParams);
}

void VideoCapture::openVideo(const QString &VideoDeviceName) {
    if (m_FormatCtx) {
        closeDevice();
    }

    const AVInputFormat *inputFormat = nullptr;
    AVDictionary *options = nullptr;
    QString deviceUrl;

#ifdef Q_OS_WIN
    // Windows 使用 dshow
    inputFormat = av_find_input_format("dshow");
    if (!VideoDeviceName.isEmpty()) {
        deviceUrl = QString("video=%1").arg(VideoDeviceName);
    } else {
        emit errorOccurred("No video device name provided for Windows.");
        return;
    }
    // 设置dshow参数以增加缓冲区大小，避免帧丢失
    av_dict_set(&options, "rtbufsize", "10000000", 0); // 设置为100MB缓冲区
    // 可以设置一些dshow参数，例如视频尺寸、帧率等
    // av_dict_set(&options, "video_size", "640x480", 0);
    // av_dict_set(&options, "framerate", "30", 0);
#elif defined(Q_OS_LINUX)
    // Linux 使用 v4l2
    inputFormat = av_find_input_format("v4l2");
    if (!VideoDeviceName.isEmpty()) {
        deviceUrl = VideoDeviceName; // 例如 "/dev/video0"
    } else {
        emit errorOccurred("No video device name provided for Linux.");
        return;
    }
#else
    emit errorOccurred("Unsupported operating system.");
    return;
#endif

    if (!inputFormat) {
        emit errorOccurred("No input format provided for video.");
        return;
    }

    m_FormatCtx = avformat_alloc_context();
    int ret = avformat_open_input(&m_FormatCtx, deviceUrl.toStdString().c_str(), inputFormat, &options);
    av_dict_free(&options);
    if (ret < 0) {
        char errbuf[1024] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        emit errorOccurred(QString("Failed to open video input device: %1").arg(errbuf));
        avformat_close_input(&m_FormatCtx);
        m_FormatCtx = nullptr;
        return;
    }

    if (avformat_find_stream_info(m_FormatCtx, nullptr) < 0) {
        emit errorOccurred("Failed to find video stream information.");
        closeDevice();
        return;
    }

    // 查找最佳视频流
    m_videoStreamIndex = av_find_best_stream(m_FormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (m_videoStreamIndex < 0) {
        emit errorOccurred("Failed to find any video stream.");
        closeDevice();
        return;
    }

    WRITE_LOG("Video device opened successfully.");
    WRITE_LOG("Video stream index:", m_videoStreamIndex);
    AVCodecParameters* vParams = m_FormatCtx->streams[m_videoStreamIndex]->codecpar;
    WRITE_LOG("Video codec: %1", QString("Codec ID: %1").arg(vParams->codec_id));
    WRITE_LOG("Video format: %1x%2", QString("%1x%2").arg(vParams->width).arg(vParams->height));

    // 音频流索引保持为-1，因为我们只打开了视频设备
    m_audioStreamIndex = -1;

    emit deviceOpenSuccessfully(vParams, nullptr);
}