#ifndef DEVICEENUMERATOR_H
#define DEVICEENUMERATOR_H
/**
 *madebyYahei
 *使用ffmpeg自动解析本机的媒体数据源
 */
#include <QStringList>
#include "VideoCapture.h"

class DeviceEnumerator
{
public:
    static QStringList getDevices(MediaType mediaType);
};

#endif // DEVICEENUMERATOR_H
