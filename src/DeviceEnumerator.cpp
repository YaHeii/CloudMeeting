//
// Created by lenovo on 25-9-10.
//

#include "DeviceEnumerator.h"
#include <QDebug>

#include "logqueue.h"
#include "log_global.h"

extern "C" {
#include <libavdevice/avdevice.h>
}

QStringList DeviceEnumerator::getDevices(MediaType mediaType)
{
    QStringList deviceList;
    const AVInputFormat *inputFormat = nullptr;
    const char* formatName = nullptr;

#ifdef Q_OS_WIN
    formatName = "dshow";
#elif defined(Q_OS_LINUX)
    formatName = (mediaType == MediaType::Video) ? "v4l2" : "alsa";
#else
    qWarning() << "Unsupported OS for device enumeration.";
    return deviceList;
#endif

    inputFormat = av_find_input_format(formatName);
    if (!inputFormat) {
        qWarning() << "Could not find input format:" << formatName;
        return deviceList;
    }

    AVDeviceInfoList *deviceInfoList = nullptr;
    // 调用FFmpeg的API来列出输入源
    int ret = avdevice_list_input_sources(inputFormat, nullptr, nullptr, &deviceInfoList);

    if (ret < 0) {
        char errbuf[1024] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        qWarning() << "Failed to list devices:" << errbuf;
        return deviceList;
    }

    qDebug() << "Found" << deviceInfoList->nb_devices << "devices for" << formatName;

    for (int i = 0; i < deviceInfoList->nb_devices; ++i) {
        AVDeviceInfo *deviceInfo = deviceInfoList->devices[i];
        // dshow下，需要根据媒体类型过滤

#ifdef Q_OS_WIN
        QString deviceName = QString::fromStdString(deviceInfo->device_name);
       //  qDebug() << "Found Device" << i << "->"
       // << "Name:" << deviceName;
        WRITE_LOG("Found Device",i,"Name:",deviceName);
        if (mediaType == MediaType::Video && deviceName.startsWith("video=")) {
            // deviceInfo->device_description 更友好，比如 "Integrated Camera"
            deviceList.append(QString::fromStdString(deviceInfo->device_description));
        } else if (mediaType == MediaType::Audio && deviceName.startsWith("audio=")) {
            deviceList.append(QString::fromStdString(deviceInfo->device_description));
        }
#else
        // Linux下，v4l2/alsa列出的直接就是需要的
        deviceList.append(QString::fromStdString(deviceInfo->device_name));
#endif
    }

    // 释放列表，否则会内存泄漏
    avdevice_free_list_devices(&deviceInfoList);

    return deviceList;
}