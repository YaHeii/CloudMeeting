#include "WebRTCPublisher.h"
#include "logqueue.h"
#include "log_global.h"
#include <rtc/common.hpp>
#include <rtc/rtc.hpp>
#include <QTimer>
#include <chrono>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
}

// 构造函数实现
WebRTCPublisher::WebRTCPublisher(QUEUE_DATA<AVPacketPtr> *encodedPacketQueue, QObject *parent)
    : QObject(parent), 
      m_encodedPacketQueue(encodedPacketQueue)
{
    m_networkManager = nullptr;
}

void WebRTCPublisher::initThread() {
    m_networkManager = new QNetworkAccessManager(this);
    rtcPreload();
    connect(m_networkManager, &QNetworkAccessManager::finished, this, &WebRTCPublisher::onSignalingReply);
}
// 析构函数实现
WebRTCPublisher::~WebRTCPublisher() {
    clear();
}

bool WebRTCPublisher::init(const QString &signalingUrl, const QString &streamUrl) {
    WRITE_LOG("Initializing WebRTC Publisher");
    // Use C++ API to initialize libdatachannel logger and forward messages to LogQueue
    rtc::InitLogger(rtc::LogLevel::Verbose, [](rtc::LogLevel level, rtc::string message) {
        const char* file = "libdatachannel";
        const char* function = "rtc_callback";
        int line =0;
        // forward formatted message to LogQueue
        LogQueue::GetInstance().print(file, function, line, "%s", message.c_str());
    });

    m_signalingUrl = signalingUrl;
    m_streamUrl = streamUrl;
    m_rtcConfig.iceServers.clear();
    m_rtcConfig.iceServers.emplace_back("stun:stun.l.google.com:19302"); // 添加STUN服务器
    m_rtcConfig.mtu =1500;
    // m_rtcConfig.forceMediaTransport = true;
    initializePeerConnection();
    return true;
}

void WebRTCPublisher::initializePeerConnection() {
    try {
        m_peerConnection = std::make_unique<rtc::PeerConnection>(m_rtcConfig);

        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(96);
        video.addSSRC(1234, "video-send");
        m_videoTrack = m_peerConnection->addTrack(video);
        WRITE_LOG("Video track (H.264) added.");

        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            1234,           // SSRC  
            "video-send",   // CNAME  
            96,             // Payload Type  
            90000           // Clock Rate (H.264 固定为 90000)  
        );

        // 创建 H.264 打包器  
        auto h264Packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::Length,  // NAL 单元分隔符  
            rtpConfig,
            rtc::H264RtpPacketizer::DefaultMaxFragmentSize  // 最大分片大小  
        );

        // 设置打包器到轨道  
        m_videoTrack->setMediaHandler(h264Packetizer);

        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
        audio.addOpusCodec(111);
        audio.addSSRC(5678, "audio-send");
        m_audioTrack = m_peerConnection->addTrack(audio);
        WRITE_LOG("Audio track (Opus) added.");
        auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            5678,           // SSRC  
            "audio-send",   // CNAME  
            111,            // Payload Type  
            48000           // Clock Rate (Opus 固定为 48000)  
        );

        // 创建 Opus 打包器  
        auto opusPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);

        // 设置打包器到轨道  
        m_audioTrack->setMediaHandler(opusPacketizer);

        // 在添加 tracks 后立即设置本地描述
        m_peerConnection->setLocalDescription();

        /// description回调
        m_peerConnection->onLocalDescription([this](const rtc::Description &description) {
            auto descriptionToString = [](rtc::Description::Type type) {
                switch (type) {
                    case rtc::Description::Type::Unspec: return "Unspec";
                    case rtc::Description::Type::Offer: return "Offer";
                    case rtc::Description::Type::Answer: return "Answer";
                    case rtc::Description::Type::Pranswer: return "Pranswer";
                    case rtc::Description::Type::Rollback: return "Rollback";
                    default: return "Unknown";
                }
            };
            WRITE_LOG("WebRTC PeerConnection description: %s", descriptionToString(description.type()));
        });
        //// PC State回调
        m_peerConnection->onStateChange([this](rtc::PeerConnection::State state) {
            auto stateToString = [](rtc::PeerConnection::State s) {
                switch (s) {
                case rtc::PeerConnection::State::New: return "New";
                case rtc::PeerConnection::State::Connecting: return "Connecting";
                case rtc::PeerConnection::State::Connected: return "Connected";
                case rtc::PeerConnection::State::Disconnected: return "Disconnected";
                case rtc::PeerConnection::State::Failed: return "Failed";
                case rtc::PeerConnection::State::Closed: return "Closed";
                default: return "Unknown";
                }
                };
                WRITE_LOG("WebRTC PeerConnection state changed: %s", stateToString(state));

                if (state == rtc::PeerConnection::State::Connected) {
                    emit publisherStarted();
                } else if (state == rtc::PeerConnection::State::Failed) {
                    emit errorOccurred("WebRTC connection failed.");
                    stopPublishing();
                }
        });
        /// ICE State回调
        m_peerConnection->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            auto stateToString = [](rtc::PeerConnection::GatheringState s) {
                switch (s) {
                    case rtc::PeerConnection::GatheringState::New: return "New";
                    case rtc::PeerConnection::GatheringState::InProgress: return "InProgress";
                    case rtc::PeerConnection::GatheringState::Complete: return "Complete";
                    default: return "Unknown";
                }
            };
            WRITE_LOG("WebRTC ICE Gathering state changed: %s", stateToString(state));

            if (state == rtc::PeerConnection::GatheringState::Complete) {
                auto description = m_peerConnection->localDescription();
                if (description.has_value()) {
                    WRITE_LOG("ICE Gathering complete. Sending offer to server.");
                    std::string sdp_offer = description.value();
					WRITE_LOG("Local SDP Offer:\n%s", sdp_offer.c_str());
                    QMetaObject::invokeMethod(this, [this, sdp_offer]() {
                        sendOfferToSignalingServer(sdp_offer);
						}, Qt::QueuedConnection);
                } else {
                    WRITE_LOG("CRITICAL: ICE Gathering is complete, but local description is NOT available.");
                    emit errorOccurred("Failed to get local SDP description after ICE gathering.");
                }
            }
        });
        /// Signaling State回调
        m_peerConnection->onSignalingStateChange([](rtc::PeerConnection::SignalingState state) {
            auto stateToString = [](rtc::PeerConnection::SignalingState s) {
                switch (s) {
                case rtc::PeerConnection::SignalingState::Stable: return "Stable";
                case rtc::PeerConnection::SignalingState::HaveLocalOffer: return "HaveLocalOffer";
                case rtc::PeerConnection::SignalingState::HaveRemoteOffer: return "HaveRemoteOffer";
                case rtc::PeerConnection::SignalingState::HaveLocalPranswer: return "HaveLocalPranswer";
                default: return "Unknown";
                }
            };
            WRITE_LOG("Signaling state changed: %s", stateToString(state));
        });
    } catch (const std::exception &e) {
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
    if (!m_peerConnection) {
        emit errorOccurred("WebRTC publisher not initialized.");
        return;
    }

    m_isPublishing = true;
    WRITE_LOG("Starting WebRTC Publishing");

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


void WebRTCPublisher::sendOfferToSignalingServer(const std::string &sdp) {
    QJsonObject jsonPayload;

    jsonPayload["sdp"] = QString::fromStdString(sdp);
    jsonPayload["streamurl"] = m_streamUrl;

    QJsonDocument doc(jsonPayload);
    QByteArray body = doc.toJson();

    QUrl url(m_signalingUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    WRITE_LOG("Sending Offer SDP to %s", m_signalingUrl.toStdString().c_str());
    QNetworkReply *response = nullptr;
    if (m_networkManager) {
        WRITE_LOG("NetworkManager exists, attempting POST request...");
        response = m_networkManager->post(request, body);
        if (response) {
            WRITE_LOG("POST request initiated successfully.");
        }
        else {
            WRITE_LOG("ERROR: POST request initiation failed (reply is null).");
            emit errorOccurred("Failed to create network reply object.");
            return;
        }
    } else {
        WRITE_LOG("ERROR: m_networkManager is null when sending offer.");
        emit errorOccurred("Network manager not initialized.");
        return;
    }

    if (!response) {
        WRITE_LOG("ERROR: Failed to send POST request to signaling server (reply is null).");
        emit errorOccurred("Failed to send offer to signaling server.");
        return;
    }

    // Log that the request object was created
    WRITE_LOG("POST request sent to signaling server.");

    // Per-reply error logging
#if QT_VERSION >= QT_VERSION_CHECK(5,15,0)
    connect(response, &QNetworkReply::errorOccurred, this, [response, this](QNetworkReply::NetworkError code) {
        QString error = QString("Signaling reply network error occurred: %1 (Code: %2)").arg(response->errorString()).arg(code);
        WRITE_LOG(error.toStdString().c_str());
    });
#else
    connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(onSignalingReply(nullptr)));
#endif

    // Safety: timeout the request if no response within10s
    QTimer::singleShot(10000, response, [response]() {
        if (!response->isFinished()) {
            WRITE_LOG("Signaling request timeout, aborting reply.");
            response->abort();
        }
    });

    connect(response, &QNetworkReply::finished, this, [this, response]() {
        WRITE_LOG("QNetworkReply finished signal received.");
        onSignalingReply(response);
     });
    WRITE_LOG("Connections for reply signals established.");
}

void WebRTCPublisher::onSignalingReply(QNetworkReply *response) {
    WRITE_LOG("onSignalingReply called!");
    if (response->error() == QNetworkReply::NoError) {
        QByteArray response_data = response->readAll();
        WRITE_LOG("Received response from signaling server: %s", response_data.constData());

        QJsonDocument jsonDoc = QJsonDocument::fromJson(response_data);
        QJsonObject jsonObj = jsonDoc.object();

        if (jsonObj.contains("code") && jsonObj["code"].toInt() ==0 && jsonObj.contains("sdp")) {
            std::string sdpAnswer = jsonObj["sdp"].toString().toStdString();
            WRITE_LOG("Received Answer SDP from server.");

            // ---设置远端 SDP 描述 ---
            try {
                if (m_peerConnection) {
                    m_peerConnection->setRemoteDescription(rtc::Description(sdpAnswer, "answer"));
                    WRITE_LOG("Remote description:\n%s", sdpAnswer.c_str());
                } else {
                    WRITE_LOG("Error: PeerConnection is null when trying to set remote description.");
                }
            } catch (const std::exception &e) {
                QString error = QString("Failed to set remote description: %1").arg(e.what());
                WRITE_LOG(error.toStdString().c_str());
                emit errorOccurred(error);
            }
        } else {
            QString error = QString("Signaling server returned an error or invalid data: %1").arg(
            QString(response_data));
            WRITE_LOG(error.toStdString().c_str());
            emit errorOccurred(error);
        }
    } else {
        QString error = QString("Signaling request failed: %1 (HTTP Status: %2)")
                .arg(response->errorString())
                .arg(response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        WRITE_LOG(error.toStdString().c_str());
        emit errorOccurred(error);
    }
    response->deleteLater();
}

void WebRTCPublisher::doPublishingWork() {
    if (!m_isPublishing.load()) {
        WRITE_LOG("WebRTC publishing loop finished.");
        clear();
        return;
    }
    if (!m_encodedPacketQueue) {
        WRITE_LOG("Encoded packet queue is null in doPublishingWork. Stopping publisher.");
        clear();
        return;
    }
    AVPacketPtr packet;
    if (m_encodedPacketQueue->dequeue(packet)) {
        // 使用 try_dequeue 避免阻塞
        try {
            if (packet->stream_index ==0 && m_videoTrack && m_videoTrack->isOpen()) {
                WRITE_LOG("sending video packet, size: %d", packet->size);
                m_videoTrack->send(reinterpret_cast<const std::byte *>(packet->data), packet->size);
            } else if (packet->stream_index ==1 && m_audioTrack && m_audioTrack->isOpen()) {
                WRITE_LOG("sending audio packet, size: %d", packet->size);
                m_audioTrack->send(reinterpret_cast<const std::byte *>(packet->data), packet->size);
            }
        } catch (const std::exception &e) {
            WRITE_LOG("Exception while sending packet: %s", e.what());
        }
    }

    // 使用 QTimer 来避免堆栈溢出和实现非阻塞循环
    QTimer::singleShot(1, this, &WebRTCPublisher::doPublishingWork);
}

void WebRTCPublisher::clear() {
    stopPublishing(); // Ensure the flag is set

    if (m_peerConnection) {
        m_peerConnection->close();
    }
    // Let the unique_ptr handle deletion
    m_peerConnection.reset();
    m_videoTrack.reset();
    m_audioTrack.reset();

    WRITE_LOG("WebRTCPublisher cleared.");
}
