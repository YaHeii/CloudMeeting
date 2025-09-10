//
// Created by lenovo on 25-9-9.
//

#ifndef FFMPEGINPUT_H
#define FFMPEGINPUT_H
#include <QObject>
#include "AVSmartPtrs.h"
#include "ThreadSafeQueue.h"

extern "C"{
#include <libavutil/avutil.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

enum class MediaType {
    Video,
    Audio
};

class VideoCapture : public QObject{
    Q_OBJECT
public:
    explicit VideoCapture(QObject* parent = nullptr);
    ~VideoCapture();

    void setPacketQueue(QUEUE_DATA<AVPacketPtr>* packetQueue);
    AVCodecParameters* getCodecParameters();

    void closeDevice();
signals:
    void deviceOpenSuccessfully(AVCodecParameters* params);

    void errorOccurred(const QString &errorText);

    void videoPacketReady(AVPacket *packet);
    void audioPacketReady(AVPacket *packet);
public slots:
    void openDevice(const QString &videoDeviceName, const QString &audioDeviceName);

    void startReading();
    void stopReading();
private:
    AVFormatContext* m_FormatCtx = nullptr;
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;

    volatile bool m_isReading = false;
    static void initializeFFmpeg();

    QUEUE_DATA<AVPacketPtr>* m_packetQueue = nullptr;
};



#endif //FFMPEGINPUT_H
