#include "AudioPlayer.h"
#include "logqueue.h"
#include "log_global.h"
#include <QAudioFormat>
#include <QMediaDevices>
#include <libavutil/error.h>
#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QString>
#include <QRegularExpression>

static QString normalizeDeviceString(const QString& s) {
    QString out = s;
    // remove parentheses content like "麦克风 (Realtek(R) Audio)" -> "麦克风  "
    out.remove(QRegularExpression("\\(.*\\)"));
    out = out.trimmed().toLower();
    return out;
}

AudioPlayer::AudioPlayer(QUEUE_DATA<AVPacketPtr>* packetQueue,
    QObject* parent)
    : QObject{ parent }, m_packetQueue(packetQueue) {
    m_ResampleConfig.frame_size = 960;//BUG:AAC和opus对帧大小的要求不一致，考虑设置全局变量  AAC(RTMP):1024 opus(WebRTC):960
    m_ResampleConfig.sample_rate = 48000;
    //m_ResampleConfig.ch_layout = AV_CHANNEL_LAYOUT_MONO;
    // Qt 声卡通常要立体声
    m_ResampleConfig.ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    //m_ResampleConfig.sample_fmt = AV_SAMPLE_FMT_FLTP;
    // [关键] 改为 S16 (Packed)，因为 Qt 不支持 Planar 格式的直接写入
    m_ResampleConfig.sample_fmt = AV_SAMPLE_FMT_S16;
    //m_audioDeviceName = ui->audioDevicecomboBox->currentText();
}

AudioPlayer::~AudioPlayer() {
    clear();
}


bool AudioPlayer::init(AVCodecParameters* params, AVRational inputTimeBase) {
    if (!params) return false;

    const AVCodec* codec = avcodec_find_decoder(params->codec_id);
    if (!codec) {
        emit errorOccurred("AudioPlayer: Codec not found");
        return false;
    }
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        WRITE_LOG("Failed to allocate codec context.");
        return false;
    }
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
    m_inputTimeBase = inputTimeBase;
    WRITE_LOG("AudioPlayer initialized successfully.");

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

    QAudioFormat format;
    format.setSampleRate(m_ResampleConfig.sample_rate);
    format.setChannelCount(m_ResampleConfig.ch_layout.nb_channels);
    format.setSampleFormat(QAudioFormat::Int16); // 对应 AV_SAMPLE_FMT_S16
    if(m_audioDeviceName.isEmpty()) return false;
    const QAudioDevice& targetDevice = findDeviceByName(m_audioDeviceName);
    m_audioSink = new QAudioSink(targetDevice, format, this);
    // 缓冲 200ms
    m_audioSink->setBufferSize(m_ResampleConfig.sample_rate * 2 * 2 * 0.2); 

    m_audioIO = m_audioSink->start();
    if (!m_audioIO) {
        emit errorOccurred("AudioPlayer: Failed to start QAudioSink.");
        return false;
    }
    WRITE_LOG("AudioPlayer: QAudioSink initialized.");

    WRITE_LOG("Audio Decoder initialized successfully and is ready to resample and to play.");
    return true;
}

QAudioDevice AudioPlayer::findDeviceByName(const QString& name) {
    // Log available devices to help debugging
    const auto devices = QMediaDevices::audioOutputs();
    WRITE_LOG("Available QAudioDevices:");
    for (const auto &d : devices) {
        WRITE_LOG("  id='%s' desc='%s'", d.id().constData(), d.description().toUtf8().constData());
    }

    if (name.isEmpty()) return QMediaDevices::defaultAudioOutput();

    // 1) exact id match
    for (const auto& device : devices) {
        if (device.id() == name) return device;
    }

    // 2) exact description match
    for (const auto& device : devices) {
        if (device.description() == name) return device;
    }

    // 3) fuzzy contains (both directions, case-insensitive)
    QString target = name.toLower();
    for (const auto& device : devices) {
        QString desc = device.description().toLower();
        if (desc.contains(target) || target.contains(desc)) return device;
    }

    // 4) normalized comparison (remove parentheses content, trim)
    QString normTarget = normalizeDeviceString(name);
    for (const auto& device : devices) {
        QString normDesc = normalizeDeviceString(device.description());
        if (!normTarget.isEmpty() && normTarget == normDesc) return device;
    }

    WRITE_LOG("Warning: Audio device '%s' not found, using default.", name.toUtf8().constData());
    return QMediaDevices::defaultAudioOutput();
}

void AudioPlayer::ChangeDecodingState(bool isDecoding) {
    m_isDecoding = isDecoding;
    if (isDecoding) {
        startPlaying();
    }
    else {
        stopPlaying();
    }
}

void AudioPlayer::startPlaying() {
    QMetaObject::invokeMethod(this, "doDecodingWork", Qt::QueuedConnection);
    WRITE_LOG("AudioPlayer: Decoding process started.");
}
//TODO: 如何用线程变量巧妙处理暂停呢？
void AudioPlayer::stopPlaying() {
    {
        QMutexLocker locker(&m_workMutex);
        while (m_isDoingWork) {
            m_workCond.wait(&m_workMutex);
        }
    }
    WRITE_LOG("AudioPlayer: Decoding process stopped.");
}

void AudioPlayer::doDecodingWork() {
    if (!m_isDecoding) {
        WRITE_LOG("AudioPlayer: Decoding process stopped.");
        return;
    }

    AVPacketPtr packet;
    if (!m_packetQueue->dequeue(packet)) {
        if (m_isDecoding) {
            QMetaObject::invokeMethod(this, "doDecodingWork", Qt::QueuedConnection);
        }
        WRITE_LOG("AudioPlayer: Dequeue Packet TimeOut");
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

    AVFramePtr decodedFrame(av_frame_alloc());
    AVFramePtr resampledFrame(av_frame_alloc());
    if (!decodedFrame || !resampledFrame) {
        emit errorOccurred("AudioPlayer: Failed to allocate frame");
        m_isDecoding = false;
        work_guard();
        return;
    }
    if (packet->size <= 0) {
        WRITE_LOG("Warning: Received empty audio packet.");
    }
    if (avcodec_send_packet(m_codecCtx, packet.get()) != 0) {
        WRITE_LOG("Fail to send packet to decoder");
        work_guard();
        if (m_isDecoding) {
            QMetaObject::invokeMethod(this, "doDecodingWork", Qt::QueuedConnection);
        }
        return;
    }
    
    while (true) {
        int ret = 0;
        ret = avcodec_receive_frame(m_codecCtx, decodedFrame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        else if (ret < 0) {
            WRITE_LOG("Error: avcodec_receive_frame failed: %s", ret);
            break;
        }

        if (decodedFrame->ch_layout.nb_channels > 0 &&
            (decodedFrame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || decodedFrame->ch_layout.u.mask == 0)) {

            av_channel_layout_default(&decodedFrame->ch_layout, decodedFrame->ch_layout.nb_channels);
            // WRITE_LOG("Fixed audio channel layout for DirectShow input.");
        }

        if (m_fifoBasePts == AV_NOPTS_VALUE && decodedFrame->pts != AV_NOPTS_VALUE) {
            if (av_audio_fifo_size(m_fifo) == 0) {
                m_fifoBasePts = decodedFrame->pts;
                WRITE_LOG("Audio Decoder Base PTS set to: %lld", m_fifoBasePts);
            }
        }

        if (!m_swrCtx) {
            swr_alloc_set_opts2(&m_swrCtx,
                &m_ResampleConfig.ch_layout,
                m_ResampleConfig.sample_fmt,
                m_ResampleConfig.sample_rate,
                &decodedFrame->ch_layout, // [关键] 使用 decoded_frame 的真实布局
                (enum AVSampleFormat)decodedFrame->format, // [关键] 使用 decoded_frame 的真实格式
                decodedFrame->sample_rate, // [关键] 使用 decoded_frame 的真实采样率
                0, nullptr);

            if (swr_init(m_swrCtx) < 0) {
                WRITE_LOG("FATAL: Failed to init SwrContext with frame params.");
                av_frame_unref(decodedFrame.get());
                continue;
            }
            WRITE_LOG("SwrContext initialized with Frame: %d Hz, Fmt: %d", decodedFrame->sample_rate, decodedFrame->format);
        }

        resampledFrame->ch_layout = m_ResampleConfig.ch_layout;
        resampledFrame->sample_rate = m_ResampleConfig.sample_rate;
        resampledFrame->format = m_ResampleConfig.sample_fmt;

        ret = swr_convert_frame(m_swrCtx, resampledFrame.get(), decodedFrame.get());
        if (ret < 0) {
            // [!! 自动恢复 !!] 如果转换失败，很可能是格式变了。
            // 我们释放旧的 context，下次循环会通过上面的 if(!m_swrCtx) 重新创建
            WRITE_LOG("Error: swr_convert_frame failed (ret=%d). Re-initializing SwrContext...", ret);
            swr_free(&m_swrCtx);
            m_swrCtx = nullptr;

            av_frame_unref(decodedFrame.get());
            continue; // 跳过这一帧，下一次会重新初始化
        }

        if (resampledFrame->nb_samples > 0) {
            int written = av_audio_fifo_write(m_fifo, (void**)resampledFrame->data, resampledFrame->nb_samples);
            if (written < resampledFrame->nb_samples) {
                WRITE_LOG("Warning: FIFO write truncated (Queue full?).");
            }
        }
        //// m_frameQueue逻辑
        //av_frame_unref(resampledFrame.get());
        //while (av_audio_fifo_size(m_fifo) >= m_ResampleConfig.frame_size) {
        //    AVFramePtr sendFrame(av_frame_alloc());
        //    sendFrame->pts = m_fifoBasePts;
        //    // 设置帧参数
        //    sendFrame->nb_samples = m_ResampleConfig.frame_size;
        //    sendFrame->ch_layout = m_ResampleConfig.ch_layout;
        //    sendFrame->format = m_ResampleConfig.sample_fmt;
        //    sendFrame->sample_rate = m_ResampleConfig.sample_rate;
        //    sendFrame->pts = m_fifoBasePts;

        //    if (av_frame_get_buffer(sendFrame.get(), 0) < 0) {
        //        WRITE_LOG("Error: Failed to allocate buffer for sendFrame.");
        //        break;
        //    }

        //    if (av_audio_fifo_read(m_fifo, (void**)sendFrame->data, m_ResampleConfig.frame_size) < m_ResampleConfig.frame_size) {
        //        WRITE_LOG("Error: FIFO read failed.");
        //        break;
        //    };

        //    // --- 计算PTS ---
        //    int64_t duration = av_rescale_q(sendFrame->nb_samples,
        //        { 1, m_ResampleConfig.sample_rate },
        //        m_inputTimeBase);
        //    m_fifoBasePts += duration;
        //    //WRITE_LOG("Audio Decoder: Enqueuing frame (Size: %d)", sendFrame->nb_samples);
        //    m_frameQueue->enqueue(std::move(sendFrame));
        //}
        av_frame_unref(decodedFrame.get());
        av_frame_unref(resampledFrame.get());

        // 临时缓冲区用于从 FIFO 读数据送给声卡
     // S16 双声道 = 4 bytes per sample
        const int bytes_per_sample = 4;
        while (av_audio_fifo_size(m_fifo) > 0) {
            int bytesFree = m_audioSink->bytesFree();
            if (bytesFree < m_ResampleConfig.frame_size * bytes_per_sample) {
                // 空间不够，跳出循环，等下一次 slice 再来，或者稍作 sleep
                // 这里选择直接跳出，让 QTimer/EventLoop 调度下一次任务，避免阻塞解码
                break;
            }
            int samples_to_read = std::min(av_audio_fifo_size(m_fifo), m_ResampleConfig.frame_size);

            // 3. 准备缓冲区 (av_audio_fifo_read 需要 void**)
            // 因为是 S16 Packed 格式，data[0] 就是全部数据
            void* data_ptr = nullptr;
            // 简单的栈上数组或者临时 vector，这里用 av_samples_alloc 比较稳妥但慢
            // 建议类成员变量复用 buffer，这里为了演示逻辑用 malloc
            int buffer_size = samples_to_read * bytes_per_sample;
            uint8_t* pBuffer = (uint8_t*)av_malloc(buffer_size);

            if (av_audio_fifo_read(m_fifo, (void**)&pBuffer, samples_to_read) < samples_to_read) {
                WRITE_LOG("FIFO Read Error");
            }
            else {
                // 4. 写入声卡
                m_audioIO->write((const char*)pBuffer, buffer_size);
            }
            av_free(pBuffer);
        }

    }

    work_guard();

    if (m_isDecoding) {
        QMetaObject::invokeMethod(this, "doDecodingWork", Qt::QueuedConnection);
    }
}




void AudioPlayer::clear() {
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
    WRITE_LOG("AudioPlayer cleared.");
}