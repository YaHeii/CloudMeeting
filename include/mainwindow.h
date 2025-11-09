#ifndef MAINWINDOW_H
#define MAINWINDOW_H
/**
 *madebyYahei
 */
#include <QMainWindow>
#include "Capture.h"
#include "ffmpegVideoDecoder.h"
#include "VideoWidget.h"
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"
#include "ffmpegAudioDecoder.h"
#include "ffmpegEncoder.h"
#include "RtmpPublisher.h"
#include "WebRTCPublisher.h"
#include "RtmpPuller.h"

QT_BEGIN_NAMESPACE

namespace Ui {
    class MainWindow;
}

QT_END_NAMESPACE

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);

    ~MainWindow();

private:
    static QRect pos;
    bool _createmeet; //是否创建会议
    bool _joinmeet; // 加入会议
    bool m_isVideoRunning = false;
    bool m_isAudioRunning = false;
    bool m_isCaptureRunning = false;

    /// 线程之间传递参数
	AVCodecParameters* m_videoParams = nullptr;
	AVCodecParameters* m_audioParams = nullptr;
	AVRational m_videoTimeBase;
	AVRational m_audioTimeBase;


    Ui::MainWindow *ui;
    quint32 mainip; //主屏幕显示的IP图像

    // --- 采集 ---
    QThread *m_CaptureThread;
    Capture *m_Capture;
    QUEUE_DATA<AVPacketPtr> *m_packetQueue; //采集队列

    // --- 视频处理链 ---
    ffmpegVideoDecoder *m_videoDecoder; 
    ffmpegEncoder *m_videoEncoder;
    QUEUE_DATA<AVPacketPtr> *m_videoPacketQueue; //采集队列
    QUEUE_DATA<std::unique_ptr<QImage> > *m_SmallQimageQueue; //晓萍显示队列
    QUEUE_DATA<AVFramePtr> *m_videoFrameQueue; //网络传输帧队列
	QUEUE_DATA<std::unique_ptr<QImage> >* m_MainQimageQueue; //主显示队列


    // --- 音频处理链 ---
    ffmpegAudioDecoder *m_audioDecoder;
    ffmpegEncoder *m_audioEncoder;
    QUEUE_DATA<AVPacketPtr> *m_audioPacketQueue;
    QUEUE_DATA<AVFramePtr> *m_audioFrameQueue; //网络传输帧队列
    QUEUE_DATA<AVPacketPtr>* m_publishPacketQueue; //发送队列

    // --- 推流 ---
    WebRTCPublisher *m_webRTCPublisher; //WebRTC
    RtmpPublisher *m_rtmpPublisher; //RTMP
    RtmpPuller* m_rtmpPuller; //RTMP拉流



    // --- 线程 ---
    QThread *m_videoDecoderThread;
    QThread *m_audioDecoderThread;
    QThread *m_videoEncoderThread;
    QThread *m_audioEncoderThread;
    QThread *m_rtmpPublisherThread;
    QThread *m_webRTCPublisherThread;
	QThread* m_rtmpPullerThread;

    VideoWidget *m_videoWidget;

    bool m_videoEncoderReady = false;
    bool m_audioEncoderReady = false;

private slots:
    void on_openVideo_clicked();
    void on_openAudio_clicked();
    void on_exitmeetBtn_clicked(); //暂时使用退出会议来关闭视频音频
    void on_createmeetBtn_clicked();
	void on_LiveStreamBtn_clicked();
	void on_joinmeetBtn_clicked();

    void onNewFrameAvailable();
    void videoEncoderReady();
    void audioEncoderReady();

    //// 处理采集到的数据包
    void handleDeviceOpened();

    void handleError(const QString &errorText);

    void onDeviceOpened(AVCodecParameters* vParams, AVCodecParameters* aParams, AVRational vTimeBase, AVRational aTimeBase);
};
#endif // MAINWINDOW_H
