#ifndef WEBRTCMANAGER_H
#define WEBRTCMANAGER_H

#include <QObject>
#include <QObject>
#include <QThread>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonDocument>

#include "ThreadSafeQueue.h"
#include "AVSmartPtrs.h"

#include <../third_party/libdatachannel/include/rtc/peerconnection.hpp>
#include <../third_party/libdatachannel/include/rtc/track.hpp>

extern "C" {
    struct AVCodecContext;
}


class WebRTCPublisher : public QObject {
    Q_OBJECT

public:
    explicit WebRTCPublisher(QUEUE_DATA<AVPacketPtr>* encodedPacketQueue, QObject *parent = nullptr);
    ~WebRTCPublisher();

private:
    void initializePeerConnection();
    void sendOfferToSignalingServer(const std::string& sdp);

    QUEUE_DATA<AVPacketPtr>* m_encodedPacketQueue;
    std::atomic<bool> m_isPublishing = false;

    // --- WebRTC members ---
    std::unique_ptr<rtc::PeerConnection> m_peerConnection;
    std::shared_ptr<rtc::Track> m_videoTrack;
    std::shared_ptr<rtc::Track> m_audioTrack;
    rtc::Configuration m_rtcConfig;

    // --- Signaling members ---
    QNetworkAccessManager* m_networkManager;
    QString m_signalingUrl;
    QString m_streamUrl;
signals:
    void errorOccurred(const QString& errorText);
    void publisherStarted();
    void publisherStopped();

public slots:
    bool init(const QString& signalingUrl, const QString& streamUrl);
    void startPublishing();
    void stopPublishing();
    void clear();
    void initThread();

private slots:
    void doPublishingWork();
    void onSignalingReply(QNetworkReply* reply);
};



#endif //WEBRTCMANAGER_H
