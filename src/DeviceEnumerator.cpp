#include "DeviceEnumerator.h"
#include <QDebug>

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavutil/error.h> // for av_strerror
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

        // 检查 media_types 指针是否有效
        if (deviceInfo->media_types) {
            for (int j = 0; deviceInfo->media_types[j] != AVMEDIA_TYPE_NB; ++j) {
                bool typeMatch = false;
                if (mediaType == MediaType::Video && deviceInfo->media_types[j] == AVMEDIA_TYPE_VIDEO) {
                    typeMatch = true;
                } else if (mediaType == MediaType::Audio && deviceInfo->media_types[j] == AVMEDIA_TYPE_AUDIO) {
                    typeMatch = true;
                }

                if (typeMatch) {
                    // 使用 device_description，因为它通常更友好
                    QString friendlyName = QString::fromStdString(deviceInfo->device_description);
                    qDebug() << "Found matching device:" << friendlyName;
                    deviceList.append(friendlyName);
                    break; // 已找到匹配类型，继续检查下一个设备
                }
            }
        }
    }

    // 释放列表，防止内存泄漏
    avdevice_free_list_devices(&deviceInfoList);

    // 移除可能出现的重复项
    deviceList.removeDuplicates();

    return deviceList;
}