#include "RtmpAudioPlayer.h"
#include "logqueue.h"
#include "log_global.h"
#include <QAudioFormat>
#include <QMediaDevices>
#include <libavutil/error.h>

RtmpAudioPlayer::RtmpAudioPlayer(QUEUE_DATA<AVPacketPtr>* packetQueue,
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
    // �����ز����� (SwrContext)��Ŀ���ʽΪ QAudioSink ֧�ֵĸ�ʽ

    // QAudioFormat �����ĸ�ʽ (���� S16LE)
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    QAudioFormat::SampleFormat q_sample_format = QAudioFormat::Int16;

    // Ŀ��ͨ������
    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2); // ǿ��������

    // Ŀ�������
    int out_sample_rate = 48000;

    // �ͷžɵ�
    if (m_swrCtx) swr_free(&m_swrCtx);

    swr_alloc_set_opts2(&m_swrCtx,
        &out_ch_layout,      // Ŀ��
        out_sample_fmt,      // Ŀ��
        out_sample_rate,     // Ŀ��
        &frame->ch_layout,   // Դ
        (AVSampleFormat)frame->format, // Դ
        frame->sample_rate,  // Դ
        0, nullptr);

    if (!m_swrCtx || swr_init(m_swrCtx) < 0) {
        emit errorOccurred("AudioPlayer: Failed to initialize audio resampler.");
        return false;
    }

    // ��ʼ�� QAudioSink
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

    // (��ѡ) ���û�������С
    m_audioSink->setBufferSize(out_sample_rate * out_ch_layout.nb_channels * 2 * 0.5); // 0.5��

    m_audioDevice = m_audioSink->start(); // ��ʼ���ţ���ȡ QIODevice
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
    // ʹ���� ffmpegVideoDecoder::clear() ��ͬ��ͬ������
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

    // �� (������ ffmpegVideoDecoder::doDecodingPacket)
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

            // 1. �״ν��յ�֡ʱ����ʼ�� QAudioSink �� SwrContext
            if (!m_audioSink) {
                if (!initAudioOutput(decoded_frame.get())) {
                    m_isDecoding = false;
                    break;
                }
            }

            // 2. �����ز���������
            // (���� ffmpegAudioDecoder::setResampleConfig ���߼�)
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

            // 3. �ز���
            int samples_converted = swr_convert(m_swrCtx,
                m_resampledData, dst_nb_samples,
                (const uint8_t**)decoded_frame->data,
                decoded_frame->nb_samples);

            if (samples_converted <= 0) continue;

            // 4. ���㻺������С��д�� QAudioSink
            int buffer_size = av_samples_get_buffer_size(&m_resampledLinesize,
                m_audioSink->format().channelCount(),
                samples_converted,
                AV_SAMPLE_FMT_S16, 1);

            if (m_audioDevice) {
                // (��ѡ) ��黺�������пռ䣬ʵ������Ƶͬ��
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