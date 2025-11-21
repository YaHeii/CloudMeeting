# ifndef WEBRTCPULLER
# define WEBRTCPULLER

#include <QObject>
#include <QString>
#include <QTimer>
#include <QWaitCondition>
#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"
#include "RtmpAudioPlayer.h"
#include "ffmpegVideoDecoder.h"
#include "netheader.h"
#include <QMessageBox>
#include "AudioResampleConfig.h"
#include <QNetworkReply>

#include <rtc/peerconnection.hpp>
#include <rtc/track.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
}
class WebRTCPuller : public QObject {
    Q_OBJECT

public:
    explicit WebRTCPuller(QUEUE_DATA<std::unique_ptr<QImage> >* MainQimageQueue, QObject* parent = nullptr);

    ~WebRTCPuller();

    void stopPulling();
private:
    void initializePeerConnection();
    void sendOfferToSignalingServer(const std::string& sdp);
    void onSignalingReply(QNetworkReply* response);
    void initCodecParams();
    RTPDepacketizer* m_videoDepacketizer;
    RTPDepacketizer* m_audioDepacketizer;
    // --- WebRTC members ---
    std::unique_ptr<rtc::PeerConnection> m_peerConnection;
    std::shared_ptr<rtc::Track> m_videoTrack;
    std::shared_ptr<rtc::Track> m_audioTrack;
    rtc::Configuration m_rtcConfig;

    // --- Signaling members ---
    QNetworkAccessManager* m_networkManager;
    QString m_signalingUrl;
    QString m_streamUrl;

    AVCodecParameters* m_vParams = nullptr;
    AVCodecParameters* m_aParams = nullptr;
    AVRational m_vTimeBase = { 0, 1 };
    AVRational m_aTimeBase = { 0, 1 };

    QUEUE_DATA<std::unique_ptr<QImage> >* m_MainQimageQueue;
    QUEUE_DATA<AVPacketPtr>* m_videoPacketQueue;
    QUEUE_DATA<AVPacketPtr>* m_audioPacketQueue;
    QUEUE_DATA<AVFramePtr>* m_dummyVideoFrameQueue;

    AVFormatContext* m_fmtCtx = nullptr;
    int m_videoStreamIndex = -1;
    int m_audioStreamIndex = -1;

    QString m_webrtcPuller;
    std::atomic<bool> m_isPulling = false;
    AVCodecContext* m_codecCtx = nullptr;

    QThread* m_videoDecodeThread = nullptr;
    ffmpegVideoDecoder* m_videoDecoder = nullptr;
    QThread* m_audioPlayThread = nullptr;
    RtmpAudioPlayer* m_audioPlayer = nullptr;

    // --- 线程同步 ---
    QMutex m_workMutex;
    QWaitCondition m_workCond;
    bool m_isDoingWork = false;

signals:
    void errorOccurred(const QString& errorText);
    void VideostreamOpened(AVCodecParameters* vParams, AVRational vTimeBase);
    void AudiostreamOpened(AVCodecParameters* aParams, AVRational aTimeBase);
    void streamClosed();
    void newFrameAvailable();
    void initSuccess();
public slots:
    bool init(QString WebRTCUrl);
    void clear();
    void startPulling(); 
    void doPullingWork();

    void ChangePullingState(bool isDecoding);

    void onStreamOpened_initVideo(AVCodecParameters* vParams, AVRational vTimeBase);

    void onStreamOpened_initAudio(AVCodecParameters* aParams, AVRational aTimeBase);
};


#endif