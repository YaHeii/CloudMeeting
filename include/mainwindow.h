#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "VideoCapture.h"
#include "ffmpegDecoder.h"
#include "VideoWidget.h"
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    static QRect pos;
    bool  _createmeet; //是否创建会议
    bool _joinmeet; // 加入会议
    bool _openCamera; //是否开启摄像头
    Ui::MainWindow *ui;
    quint32 mainip; //主屏幕显示的IP图像

    VideoCapture *m_videocapture;
    QThread *m_CaptureThread;

    QThread *m_decoderThread;
    ffmpegDecoder *m_ffmpegDecoder;

    QUEUE_DATA<AVPacketPtr>* m_packetQueue;
    QUEUE_DATA<std::unique_ptr<QImage>>* m_frameQueue;

    VideoWidget* m_videoWidget;

private slots:
    ////TODO:开启视频和开启音频按钮触发后转变为关闭视频和关闭音频
    void on_openVideoButton_clicked();
    void on_exitmeetBtn_clicked();//暂时使用退出会议来关闭视频音频

    //// 处理采集到的数据包
    // void handleVideoPacket(AVPacket *packet);
    // void handleAudioPacket(AVPacket *packet);
    void handleDeviceOpened();
    void handleError(const QString &errorText);

    void onNewFrameAvailable();
    void onDeviceOpened(AVCodecParameters* params);

};
#endif // MAINWINDOW_H
