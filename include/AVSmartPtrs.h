//
// Created by lenovo on 25-9-10.
//

#ifndef AVSMARTPTRS_H
#define AVSMARTPTRS_H

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
}

struct AVPacketDeleter {
    void operator()(AVPacket *p) const {
        if (p) {
            av_packet_free(&p);
        }
    }
};

struct AVFrameDeleter {
    void operator()(AVFrame *p) const {
        if (p) {
            av_frame_free(&p);
        }
    }
};


using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

#endif //AVSMARTPTRS_H
