#include "ffmpegDecoder.h"

#include "logqueue.h"
#include "log_global.h"
ffmpegDecoder::ffmpegDecoder(QUEUE_DATA<AVPacketPtr>* packetQueue, QUEUE_DATA<std::unique_ptr<QImage>>* frameQueue,QObject *parent)
    : QObject{parent}, m_packetQueue(packetQueue), m_frameQueue(frameQueue) {}

ffmpegDecoder::~ffmpegDecoder()
{
    clear();
}

bool ffmpegDecoder::init(AVCodecParameters *params) {
    if (!params) {
        return false;
    }
    m_codec = avcodec_find_decoder(params->codec_id);
    if (!m_codec) {
        WRITE_LOG("codec未解析");
        return false;
    }
    m_codecCtx = avcodec_alloc_context3(m_codec);
    if (avcodec_parameters_to_context(m_codecCtx, params) < 0) { WRITE_LOG("avcodec_parameters_to_context失败"); return false; }

    if (avcodec_open2(m_codecCtx, m_codec, nullptr) < 0) { WRITE_LOG("avcodec_open2失败"); return false; }

    WRITE_LOG("Decoder initialized successfully.");
    return true;
}


void ffmpegDecoder::startDecoding() {
    m_isDecoding = true;
    WRITE_LOG("ffmpegDecoder::startDecoding");

    AVFramePtr frame(av_frame_alloc());
    AVFramePtr rgbFrame(av_frame_alloc());
    ////TODO:内存优化
    uint8_t* buffer = nullptr;
    while (m_isDecoding) {
        AVPacketPtr packet;
        if (!m_packetQueue->dequeue(packet)) {
            WRITE_LOG("ffmpegDecoder::Deque Packet TimeOut");
            continue;
        }
        if (avcodec_send_packet(m_codecCtx, packet.get()) != 0) {
            continue;
        }
        while (avcodec_receive_frame(m_codecCtx,frame.get())==0) {
            if (!m_swsCtx) {
                m_swsCtx = sws_getContext(m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt,
                                                         m_codecCtx->width, m_codecCtx->height, AV_PIX_FMT_RGB24,
                                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
                int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, m_codecCtx->width, m_codecCtx->height, 1);
                buffer = (uint8_t*)av_malloc(bufferSize * sizeof(uint8_t));
                av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, buffer, AV_PIX_FMT_RGB24,
                                     m_codecCtx->width, m_codecCtx->height, 1);
            }
            if (m_swsCtx) {
                sws_scale(m_swsCtx, (const uint8_t* const*)frame->data, frame->linesize,
                          0, m_codecCtx->height, rgbFrame->data, rgbFrame->linesize);

                QImage tempImage(rgbFrame->data[0], m_codecCtx->width, m_codecCtx->height, QImage::Format_RGB888);
                auto image = std::make_unique<QImage>(tempImage.copy());//copy做深拷贝
                // 将QImage的所有权转移到帧队列
                m_frameQueue->enqueue(std::move(image));

                // 通知UI线程有新帧可用
                emit newFrameAvailable();
            }
        }
    }
    av_free(buffer);
    buffer = nullptr;
    WRITE_LOG("Decoding loop finished.");
}
void ffmpegDecoder::stopDecoding()
{
    m_isDecoding = false;
}

void ffmpegDecoder::clear()
{
    stopDecoding();
    if(m_codecCtx) avcodec_free_context(&m_codecCtx);
    if(m_swsCtx) sws_freeContext(m_swsCtx);
}