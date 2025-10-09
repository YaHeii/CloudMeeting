#include "WebRTCPublisher.h"
#include "logqueue.h"
#include "log_global.h"

#include <QTimer>

extern "C" {
#include <libavcodec/avcodec.h>
}

// 构造函数实现
WebRTCPublisher::WebRTCPublisher(QUEUE_DATA<AVPacketPtr>* encodedPacketQueue, QObject *parent) : QObject(parent) {
    m_networkManager = new QNetworkAccessManager(this);
    connect(m_networkManager, &QNetworkAccessManager::finished, this, &WebRTCPublisher::onSignalingReply);
}

// 析构函数实现
WebRTCPublisher::~WebRTCPublisher() {
    clear();
}

bool WebRTCPublisher::init(const QString& signalingUrl, const QString& streamUrl) {
    WRITE_LOG("Initializing WebRTC Publisher");
    m_signalingUrl = signalingUrl;
    m_streamUrl = streamUrl;

    m_rtcConfig.iceServers.emplace_back("stun:stun.l.google.com:19302");// 添加STUN服务器

    initializePeerConnection();
    return true;
}

void WebRTCPublisher::initializePeerConnection() {
    try {
        m_peerConnection = std::make_unique<rtc::PeerConnection>(m_rtcConfig);

        m_peerConnection->onStateChange([this](rtc::PeerConnection::State state) {
            WRITE_LOG("WebRTC PeerConnection state changed: %d", static_cast<int>(state));
            if (state == rtc::PeerConnection::State::Connected) {
                // When connected, start the publishing loop
                if (m_isPublishing) {
                    emit publisherStarted();
                    QMetaObject::invokeMethod(this, "doPublishingWork", Qt::QueuedConnection);
                }
            } else if (state == rtc::PeerConnection::State::Failed) {
                emit errorOccurred("WebRTC connection failed.");
                stopPublishing();
            }
        });

        m_peerConnection->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            WRITE_LOG("WebRTC ICE Gathering state changed: %d", static_cast<int>(state));
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                auto description = m_peerConnection->localDescription();
                if (description.has_value()) {
                    WRITE_LOG("ICE Gathering complete. Sending offer to server.");
                    sendOfferToSignalingServer(description.value());
                } else {
                    emit errorOccurred("Failed to get local SDP description after ICE gathering.");
                }
            }
        });

        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(96, std::nullopt);
        m_videoTrack = m_peerConnection->addTrack(video);
        WRITE_LOG("Video track (H.264) added.");
        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);

        // 添加 Opus 编解码器，使用 payload type 111
        // 同样，第二个参数 profile 是可选的
        audio.addOpusCodec(111, std::nullopt);
        m_audioTrack = m_peerConnection->addTrack(audio);
        WRITE_LOG("Audio track (Opus) added.");
    } catch (const std::exception& e) {
        QString error = QString("Failed to create PeerConnection: %1").arg(e.what());
        WRITE_LOG(error.toStdString().c_str());
        emit errorOccurred(error);
    }
}

void WebRTCPublisher::startPublishing() {
    if (m_isPublishing) {
        WRITE_LOG("WebRTC publisher is already running.");
        return;
    }
    m_isPublishing = true;
    if (!m_peerConnection) {
        emit errorOccurred("WebRTC publisher not initialized.");
        m_isPublishing = false;
        return;
    }

    WRITE_LOG("Starting WebRTC Publishing");
    m_peerConnection->setLocalDescription();
    QMetaObject::invokeMethod(this, "doPublishingWork", Qt::QueuedConnection);
}
void WebRTCPublisher::stopPublishing() {
    if (!m_isPublishing.load()) {
        return;
    }
    m_isPublishing = false;
    WRITE_LOG("Stopping WebRTC publishing process...");
    emit publisherStopped();
}
//// 考虑使用datachannel库的SDP标准格式
void WebRTCPublisher::sendOfferToSignalingServer(const std::string& sdp) {
    QJsonObject jsonPayload;
    // This is the typical payload for SRS HTTP-FLV API
    jsonPayload["sdp"] = QString::fromStdString(sdp);
    jsonPayload["streamurl"] = m_streamUrl;

    QJsonDocument doc(jsonPayload);
    QByteArray body = doc.toJson();

    QNetworkRequest request(m_signalingUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    WRITE_LOG("Sending Offer SDP to %s", m_signalingUrl.toStdString().c_str());
    m_networkManager->post(request, body);
}

void WebRTCPublisher::onSignalingReply(QNetworkReply* reply) {
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray response_data = reply->readAll();
        QJsonDocument jsonDoc = QJsonDocument::fromJson(response_data);
        QJsonObject jsonObj = jsonDoc.object();

        if (jsonObj.contains("sdp") && jsonObj.contains("code") && jsonObj["code"].toInt() == 0) {
            std::string sdpAnswer = jsonObj["sdp"].toString().toStdString();
            WRITE_LOG("Received Answer SDP from server.");
            m_peerConnection->setRemoteDescription(rtc::Description(sdpAnswer, "answer"));
        } else {
            QString error = QString("Signaling server returned an error: %1").arg(QString(response_data));
            WRITE_LOG(error.toStdString().c_str());
            emit errorOccurred(error);
        }
    } else {
        QString error = QString("Signaling request failed: %1").arg(reply->errorString());
        WRITE_LOG(error.toStdString().c_str());
        emit errorOccurred(error);
    }
    reply->deleteLater();
}

void WebRTCPublisher::doPublishingWork() {
    if (!m_isPublishing.load()) {
        WRITE_LOG("WebRTC publishing loop finished.");
        clear();
        return;
    }

    AVPacketPtr packet;
    // Use a timed dequeue to avoid a busy-wait loop if the queue is empty
    if (!m_encodedPacketQueue->dequeue(packet)) {
        // Queue is empty, schedule the next check
        QMetaObject::invokeMethod(this, "doPublishingWork", Qt::QueuedConnection);
        return;
    }

    try {
        if (packet->stream_index == 0 && m_videoTrack && m_videoTrack->isOpen()) { // Video stream
            // libdatachannel handles RTP packetization. Just send the raw NAL units.
            m_videoTrack->send(reinterpret_cast<const std::byte*>(packet->data), packet->size);
        } else if (packet->stream_index == 1 && m_audioTrack && m_audioTrack->isOpen()) { // Audio stream
            m_audioTrack->send(reinterpret_cast<const std::byte*>(packet->data), packet->size);
        }
    } catch (const std::exception& e) {
        WRITE_LOG("Exception while sending packet: %s", e.what());
    }

    // Schedule the next packet processing
    if (m_isPublishing.load()) {
        QMetaObject::invokeMethod(this, "doPublishingWork", Qt::QueuedConnection);
    } else {
        WRITE_LOG("WebRTC publishing loop finished after last packet.");
        clear();
    }
}

void WebRTCPublisher::clear() {
    stopPublishing(); // Ensure the flag is set

    if(m_peerConnection) {
        m_peerConnection->close();
    }
    // Let the unique_ptr handle deletion
    m_peerConnection.reset();
    m_videoTrack.reset();
    m_audioTrack.reset();

    WRITE_LOG("WebRTCPublisher cleared.");
}
