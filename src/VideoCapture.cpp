#include "VideoCapture.h"

#include "logqueue.h"
#include "log_global.h"


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
}

VideoCapture::~VideoCapture() {
    closeDevice();
    WRITE_LOG("Closing device.");
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

    m_videoStreamIndex = av_find_best_stream(m_FormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    m_audioStreamIndex = av_find_best_stream(m_FormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (m_videoStreamIndex < 0 && m_audioStreamIndex < 0) {
        emit errorOccurred("Failed to find any video or audio stream.");
        closeDevice();
        return;
    }

    WRITE_LOG("Device opened successfully.");
    WRITE_LOG("Video stream index:",m_videoStreamIndex);
    WRITE_LOG("Video stream index:",m_audioStreamIndex);
    emit deviceOpenSuccessfully(m_FormatCtx->streams[m_videoStreamIndex]->codecpar);
    //这里提交语音解码？
}

void VideoCapture::startReading() {
    if (!m_packetQueue) {
        WRITE_LOG("Packet queue NOT SET.");
    }

    if (!m_FormatCtx) {
        WRITE_LOG("Failed to start reading.");
        return;
    }
    m_isReading = true;
    WRITE_LOG("Starting to read frames...");

    while (m_isReading) {
        AVPacketPtr packet(av_packet_alloc());
        if (!packet) {
            emit errorOccurred("Failed to allocate packet.");
            break;
        }

        int ret = av_read_frame(m_FormatCtx, packet.get());
        if (ret < 0) {
            break;
        }

        if (packet->stream_index == m_videoStreamIndex) {
            m_packetQueue->enqueue(std::move(packet));
        }else if (packet->stream_index == m_audioStreamIndex) {
            m_packetQueue->enqueue(std::move(packet));
        }else {
            //emit errorOccurred("Failed to read frame.");
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
    stopReading(); // 确保读取循环已停止

    if (m_FormatCtx) {
        avformat_close_input(&m_FormatCtx);
        m_FormatCtx = nullptr; // 置空指针，防止野指针
        m_videoStreamIndex = -1;
        m_audioStreamIndex = -1;
        WRITE_LOG("Device closed.");
    }
}