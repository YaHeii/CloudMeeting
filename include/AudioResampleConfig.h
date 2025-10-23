/**
 *音频编码器重采样通道配置
 *madebyYahei
 */


#ifndef AUDIORESAMPLECONFIG_H
#define AUDIORESAMPLECONFIG_H
#include "libavutil/samplefmt.h"
#include "libavutil/channel_layout.h"

// 使用 Qt 的元类型系统，以便在信号槽中传递
#include <QMetaType>

struct AudioResampleConfig {
    int frame_size = 0;
    int sample_rate = 0;
    AVSampleFormat sample_fmt = AV_SAMPLE_FMT_NONE;
    AVChannelLayout ch_layout;

    AudioResampleConfig() {
        av_channel_layout_default(&ch_layout, 0); // 初始化
    }
};

Q_DECLARE_METATYPE(AudioResampleConfig)
#endif //AUDIORESAMPLECONFIG_H
