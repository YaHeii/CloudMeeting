#include "ffmpegVideoDecoder.h"
#include "logqueue.h"
#include "log_global.h"
ffmpegVideoDecoder::ffmpegVideoDecoder(QUEUE_DATA<AVPacketPtr>* packetQueue, QUEUE_DATA<std::unique_ptr<QImage>>* imageQueue,QUEUE_DATA<AVFramePtr>* frameQueue ,QObject *parent)
    : QObject{parent}, m_packetQueue(packetQueue),m_frameQueue(frameQueue), m_QimageQueue(imageQueue) {
    m_rgbFrame.reset(av_frame_alloc());
    if (!m_rgbFrame) {
        WRITE_LOG("FATAL: Failed to allocate m_rgbFrame in constructor.");
        emit errorOccurred("FATAL: Failed to allocate m_rgbFrame in constructor.");
    }
}

ffmpegVideoDecoder::~ffmpegVideoDecoder()
{
    clear();
}

bool ffmpegVideoDecoder::init(AVCodecParameters *params) {
    if (!params) {
        return false;
    }
    m_codec = avcodec_find_decoder(params->codec_id);
    if (!m_codec) {
        WRITE_LOG("codec未解析");
        return false;
    }
    m_codecCtx = avcodec_alloc_context3(m_codec);
    if (!m_codecCtx) {
        WRITE_LOG("Failed to allocate codec context.");
        return false;
    }
    if (avcodec_parameters_to_context(m_codecCtx, params) < 0) {
        emit errorOccurred("avcodec_parameters_to_context failed");
        avcodec_free_context(&m_codecCtx);
        return false;
    }
    if (avcodec_open2(m_codecCtx, m_codec, nullptr) < 0) {
        emit errorOccurred("avcodec_open2 failed");
        avcodec_free_context(&m_codecCtx);
        return false;
    }
    WRITE_LOG("Video decoder initialized successfully.");
    return true;
}

void ffmpegVideoDecoder::startDecoding() {
    if (!m_codecCtx) {
        WRITE_LOG("Decoder not initialized, cannot start decoding.");
        return;
    }
    if (m_isDecoding) {
        return; // 防止重复启动
    }
    WRITE_LOG("Starting video decoding loop...");
    m_isDecoding = true;

    QMetaObject::invokeMethod(this, "doDecodingPacket", Qt::QueuedConnection);
}

void ffmpegVideoDecoder::doDecodingPacket() {
    AVFramePtr decodedFrame(av_frame_alloc());
    if (!decodedFrame) {
        WRITE_LOG("Failed to allocate decoded_frame.");
        m_isDecoding = false;
        return;
    }
    if (!m_isDecoding) {
        WRITE_LOG("Video decoding loop finished.");
        return;
    }
    WRITE_LOG("ffmpegDecoder::startDecoding");

    AVPacketPtr packet;
    if (!m_packetQueue->dequeue(packet)) {
        if (m_isDecoding) {
            QMetaObject::invokeMethod(this, "doDecodingPacket", Qt::QueuedConnection);
        }
        WRITE_LOG("ffmpegDecoder::Deque Packet TimeOut");
        return;
    }
    if (avcodec_send_packet(m_codecCtx, packet.get()) != 0) {
        WRITE_LOG("Failed to send packet to decoder.");
        if (m_isDecoding) {
            QMetaObject::invokeMethod(this, "doDecodingPacket", Qt::QueuedConnection);
        }
        return;
    }
    while (m_isDecoding) {
        WRITE_LOG("decoding.");
        int ret = avcodec_receive_frame(m_codecCtx, decodedFrame.get());
        if (ret < 0) {
            // EAGAIN 或 EOF 表示这个包已经处理完了，可以跳出内层循环
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            WRITE_LOG("avcodec_receive_frame failed.");
            break;
        }
        AVFramePtr frame_for_encoder(av_frame_clone(decodedFrame.get()));
        if (frame_for_encoder) {
            m_frameQueue->enqueue(std::move(frame_for_encoder));
        }
        if (!m_swsCtx) {
            m_swsCtx = sws_getContext(m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt,
                                                     m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_RGB24,
                                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
            int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, m_codecCtx->width, m_codecCtx->height, 1);
            rgbBuffer = (uint8_t*)av_malloc(bufferSize * sizeof(uint8_t));
            av_image_fill_arrays(m_rgbFrame->data, m_rgbFrame->linesize, rgbBuffer, AV_PIX_FMT_RGB24,
                                 m_codecCtx->width, m_codecCtx->height, 1);
        }
        if (m_swsCtx) {
            if (!decodedFrame || !decodedFrame->data[0]) {
                WRITE_LOG("Error: Attempting to scale a NULL or invalid frame!");
                break; // 跳过这一帧的处理
            }
            sws_scale(m_swsCtx, (const uint8_t* const*)decodedFrame->data, decodedFrame->linesize,
                      0, m_codecCtx->height, m_rgbFrame->data, m_rgbFrame->linesize);

            QImage tempImage(m_rgbFrame->data[0], m_codecCtx->width, m_codecCtx->height, QImage::Format_RGB888);
            auto image = std::make_unique<QImage>(tempImage.copy());//copy做深拷贝
            m_QimageQueue->enqueue(std::move(image));//添加到图片队列，用于QT渲染

            // 通知UI线程有新帧可用
            emit newFrameAvailable();
            // WRITE_LOG("NewFrameAvailable");
        }
        av_frame_unref(decodedFrame.get());
    }
    if (m_isDecoding.load()) {
        QMetaObject::invokeMethod(this, "doDecodingPacket", Qt::QueuedConnection);
    } else {
        WRITE_LOG("Video decoding loop finished.");
    }
}


void ffmpegVideoDecoder::stopDecoding()
{
    m_isDecoding = false;
}

void ffmpegVideoDecoder::clear()
{
    stopDecoding();
    if(m_codecCtx) avcodec_free_context(&m_codecCtx);
    if(m_swsCtx) sws_freeContext(m_swsCtx);
    m_swsCtx = nullptr;

    if(rgbBuffer) {
        av_free(rgbBuffer);
        rgbBuffer = nullptr;
    }
}