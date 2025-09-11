#ifndef MAINWINDOW_H
#define MAINWINDOW_H
/**
 *madebyYahei
 */
#include <QMainWindow>
#include "VideoCapture.h"
#include "ffmpegDecoder.h"
#include "VideoWidget.h"
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"
#include "ffmpegAudioDecoder.h"

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
    bool m_isVideoRunning = false;
    bool m_isAudioRunning = false;
    bool m_isCaptureRunning = false;

    Ui::MainWindow *ui;
    quint32 mainip; //主屏幕显示的IP图像

    QThread *m_CaptureThread;
    VideoCapture *m_capture;


    // --- 视频处理链 ---
    QThread* m_videoDecoderThread;
    ffmpegDecoder* m_videoDecoder; // 之前的 ffmpegDecoder
    QUEUE_DATA<AVPacketPtr>* m_videoPacketQueue;
    QUEUE_DATA<std::unique_ptr<QImage>>* m_videoFrameQueue;

    // --- 音频处理链 ---
    QThread* m_audioDecoderThread;
    ffmpegAudioDecoder* m_audioDecoder;
    QUEUE_DATA<AVPacketPtr>* m_audioPacketQueue;
    //QUEUE_DATA<AudioFrame> m_audioFrameQueue; // 解码后的音频帧队列

    VideoWidget* m_videoWidget;

private slots:
    ////TODO:开启视频和开启音频按钮触发后转变为关闭视频和关闭音频
    void on_openVideo_clicked();
    // void on_openAudioButton_clicked();
    void on_exitmeetBtn_clicked();//暂时使用退出会议来关闭视频音频

    void onNewFrameAvailable();

    //// 处理采集到的数据包
    // void handleVideoPacket(AVPacket *packet);
    // void handleAudioPacket(AVPacket *packet);
    void handleDeviceOpened();
    void handleError(const QString &errorText);
    //
    // void onVideoDeviceOpened(AVCodecParameters* params);
    // void onAudioDeviceOpened(AVCodecParameters* params);
    void onDeviceOpened(AVCodecParameters* vParams, AVCodecParameters* aParams);

};
#endif // MAINWINDOW_H
