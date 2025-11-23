#include "WebRTCPublisher.h"
#include "logqueue.h"
#include "log_global.h"
#include <rtc/common.hpp>
#include <rtc/rtc.hpp>
#include <QTimer>
#include <chrono>
#include <memory>
#include <sstream> // 需要包含头文件


extern "C" {
#include <libavcodec/avcodec.h>
}

WebRTCPublisher::WebRTCPublisher(QUEUE_DATA<AVPacketPtr> *encodedPacketQueue, QObject *parent)
    : QObject(parent), 
      m_encodedPacketQueue(encodedPacketQueue)
{
    m_networkManager = nullptr;
    m_networkManager = new QNetworkAccessManager(this);
    rtcPreload();
    //connect(m_networkManager, &QNetworkAccessManager::finished, this, &WebRTCPublisher::onSignalingReply);
}


WebRTCPublisher::~WebRTCPublisher() {
    clear();
}

bool WebRTCPublisher::init(const QString &signalingUrl, const QString &streamUrl) {

    WRITE_LOG("Initializing WebRTC Publisher");

    //// 初始化dc日志系统，使用内置日志系统输出
    //rtc::InitLogger(rtc::LogLevel::Verbose, [](rtc::LogLevel level, rtc::string message) {
    //    const char* file = "libdatachannel";
    //    const char* function = "rtc_callback";
    //    int line =0;
    //    LogQueue::GetInstance().print(file, function, line, "%s", message.c_str());
    //});

    m_signalingUrl = signalingUrl;
    m_streamUrl = streamUrl;
    m_rtcConfig.iceServers.clear(); 
    m_rtcConfig.iceServers.emplace_back(rtc::IceServer("stun.iptel.org"));
    m_rtcConfig.iceServers.emplace_back(rtc::IceServer("stun.rixtelecom.se"));

    m_rtcConfig.iceServers.emplace_back(rtc::IceServer("stun:stun.l.google.com:19302"));
    m_rtcConfig.iceServers.emplace_back(rtc::IceServer("stun:stun1.l.google.com:19302"));
    m_rtcConfig.mtu =1500;
    m_rtcConfig.portRangeBegin = 10000;
    m_rtcConfig.portRangeEnd = 20000;
    initializePeerConnection();
    return true;
}

void WebRTCPublisher::initializePeerConnection() {
    try {
        m_peerConnection = std::make_unique<rtc::PeerConnection>(m_rtcConfig);

        //// 创建SDP
        // video part
        rtc::Description::Video video("video");
        //rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(96);
        video.addRtxCodec(97, 96, 90000);
        video.addSSRC(42, "video-send", "video-stream", "video-track");
        video.setDirection(rtc::Description::Direction::SendOnly);
        m_videoTrack = m_peerConnection->addTrack(video);
        WRITE_LOG("Video track (H.264) added.");


        rtc::Description::Audio audio("audio");
        //rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
        audio.addOpusCodec(111);
        audio.addSSRC(43, "audio-send", "audio-stream", "audio-track");
        audio.setDirection(rtc::Description::Direction::SendOnly);
        m_audioTrack = m_peerConnection->addTrack(audio);
        WRITE_LOG("Audio track (Opus) added.");


		//// video_rtp打包配置
        auto VideortpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            42,           // SSRC  
            "video-send",   // CNAME  
            96,             // Payload Type  
            90000           // Clock Rate (H.264 固定为 90000)  
        );
        // 创建 H.264 打包器  
        auto h264Packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence,  // NAL 单元分隔符  (00 00 00 01)
            VideortpConfig,
            rtc::H264RtpPacketizer::DefaultMaxFragmentSize  // 最大分片大小  
        );
        // 设置打包器到轨道  
        m_videoTrack->setMediaHandler(h264Packetizer);

		//// audio_rtp打包配置
        auto AudiortpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            43,           // SSRC  
            "audio-send",   // CNAME  
            111,            // Payload Type  
            48000           // Clock Rate (Opus 固定为 48000)  
        );
        // 创建 Opus 打包器  
        auto opusPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(AudiortpConfig);
        // 设置打包器到轨道  
        m_audioTrack->setMediaHandler(opusPacketizer);

        //// description回调
        m_peerConnection->onLocalDescription([this](const rtc::Description &description) {
            QMetaObject::invokeMethod(this, [this, description]() {
                //QString sdp_offer = QString::fromStdString(description);
                std::string sdp_offer = description;

                sendOfferToSignalingServer(sdp_offer);
                });
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
            QMetaObject::invokeMethod(this, [this, state]() {
                if (state == rtc::PeerConnection::State::Connected) {
                    emit publisherStarted();
                    onPLI_Received();
                    //if (m_pliTimer) {
                    //    m_pliTimer->stop();
                    //    m_pliTimer->deleteLater();
                    //    m_pliTimer = nullptr;
                    //}
                    //m_pliTimer = new QTimer(this);
                    //connect(m_pliTimer, &QTimer::timeout, this, [this]() {
                    //    static int count = 0;
                    //    if (count++ < 30) {
                    //        WRITE_LOG("WebRTC: Scheduled Keyframe Request (%d/5)", count);
                    //        onPLI_Received();
                    //    }
                    //    else {
                    //        m_pliTimer->stop();
                    //        count = 0;
                    //    }
                    //    });
                    //m_pliTimer->start(1000);
                }
                else if (state == rtc::PeerConnection::State::Failed) {
                    emit errorOccurred("WebRTC connection failed.");
                    stopPublishing();
                }
                else if (state == rtc::PeerConnection::State::Closed ||
                    state == rtc::PeerConnection::State::Disconnected) {
                    if (m_pliTimer) {
                        m_pliTimer->stop();
                        m_pliTimer->deleteLater();
                        m_pliTimer = nullptr;
                    }
                }
            });
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
        //完成回调后添加offer，防止竞态
        m_peerConnection->setLocalDescription();
        WRITE_LOG("Local description set, waiting for ICE gathering...");
    } catch (const std::exception &e) {
        QString error = QString("Failed to create PeerConnection: %1").arg(e.what());
        WRITE_LOG(error.toStdString().c_str());
        emit errorOccurred(error);
    }
}

void WebRTCPublisher::ChangeWebRtcPublishingState(bool isPublishing) {
    m_isPublishing = isPublishing;
    if (m_isPublishing) {
        if (!m_peerConnection) {
            emit errorOccurred("WebRTC publisher not initialized.");
            return;
        }
        startPublishing();
    } else {
        stopPublishing();
    }
}

void WebRTCPublisher::startPublishing() {
    QMetaObject::invokeMethod(this, "doPublishingWork", Qt::QueuedConnection);
    WRITE_LOG("Starting WebRTC Publishing");
}

void WebRTCPublisher::stopPublishing() {
    clear();
    WRITE_LOG("Stopping WebRTC publishing process...");

}


void WebRTCPublisher::sendOfferToSignalingServer(const std::string &sdp) {

    QUrl url(m_signalingUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    WRITE_LOG("Sending Offer SDP to %s", m_signalingUrl.toStdString().c_str());

    QByteArray body = QByteArray::fromStdString(sdp);
    WRITE_LOG("Request JSON:\n%s", body.constData());

    QNetworkReply *response = nullptr;
    if (!m_networkManager) {
        WRITE_LOG("ERROR: m_networkManager is null when sending offer.");
        emit errorOccurred("Network manager not initialized.");
        return;
    }

    WRITE_LOG("NetworkManager exists, attempting POST request...");
    response = m_networkManager->post(request, body);
    if (!response) {
        WRITE_LOG("ERROR: Failed to send POST request to signaling server (reply is null).");
        emit errorOccurred("Failed to send offer to signaling server.");
        return;
    }

    WRITE_LOG("POST request sent to signaling server.");

    connect(response, &QNetworkReply::errorOccurred, this, [response, this](QNetworkReply::NetworkError code) {
        QString error = QString("Signaling reply network error occurred: %1 (Code: %2)").arg(response->errorString()).arg(code);
        WRITE_LOG(error.toStdString().c_str());
    });


    QTimer * timeoutTimer = new QTimer(this);
    timeoutTimer->setSingleShot(true);
    connect(timeoutTimer, &QTimer::timeout, this, [response, timeoutTimer, this]() {
        if (response && !response->isFinished()) {
            WRITE_LOG("Signaling request timeout (10s), aborting reply.");
            response->abort();
        }
        timeoutTimer->deleteLater();
        });
    timeoutTimer->start(10000);


    connect(response, &QNetworkReply::finished, this, [this, response, timeoutTimer]() {
        if (timeoutTimer && timeoutTimer->isActive()) {
            timeoutTimer->stop();
            timeoutTimer->deleteLater();
        }
        WRITE_LOG("QNetworkReply finished signal received.");
        onSignalingReply(response);
        });

    WRITE_LOG("Connections for reply signals established.");
}

void WebRTCPublisher::onSignalingReply(QNetworkReply *response) {
    WRITE_LOG("onSignalingReply called!");
    if (!response) {
        WRITE_LOG("ERROR: Response is null in onSignalingReply");
        emit errorOccurred("Null response received");
        return;
    }
    if (response->error() != QNetworkReply::NoError) {
        QString error = QString("Network error: %1 (HTTP: %2)")
            .arg(response->errorString())
            .arg(response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        WRITE_LOG(error.toStdString().c_str());
        response->deleteLater();
        emit errorOccurred(error);
        return;
    }
    QByteArray response_data = response->readAll();
    QString sdpAnswer = NULL;
	//HTTP状态码检查
    int httpStatus = response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus == 201) {  // WHIP 成功状态码
        sdpAnswer = QString::fromUtf8(response_data);
        WRITE_LOG("WHIP publish successful,SDP_Answer:", sdpAnswer);
    }
    else {
        WRITE_LOG("WHIP failed with status:", httpStatus);
        emit errorOccurred(QString("WHIP failed: HTTP %1").arg(httpStatus));
    }
    

    // 设置远端SDP描述
    try {
        if (m_peerConnection) {
            m_peerConnection->setRemoteDescription(rtc::Description(sdpAnswer.toStdString(), "answer"));
            WRITE_LOG("Remote description set successfully.");
        }
        else {
            WRITE_LOG("ERROR: PeerConnection is null when trying to set remote description.");
            emit errorOccurred("PeerConnection is null");
        }
    }
    catch (const std::exception& e) {
        QString error = QString("Failed to set remote description: %1").arg(e.what());
        WRITE_LOG(error.toStdString().c_str());
        emit errorOccurred(error);
    }

    QTimer::singleShot(30000, [response]() { // 30秒后删除
        if (response) {
            response->deleteLater();
        }
    });

}
std::vector<std::byte> normalizeH264StartCodes(const uint8_t* data, int size) {
    std::vector<std::byte> buffer;
    buffer.reserve(size + 16);

    for (int i = 0; i < size; ) {
        // 1. 优先检查 4 字节起始码 (00 00 00 01)
        // 如果已经是 4 字节，直接拷贝，不做修改
        if (i + 3 < size &&
            data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01) {

            buffer.push_back(std::byte(0x00));
            buffer.push_back(std::byte(0x00));
            buffer.push_back(std::byte(0x00));
            buffer.push_back(std::byte(0x01));
            i += 4; // 跳过这 4 个字节
        }
        // 2. 检查 3 字节起始码 (00 00 01)
        // 只有是 3 字节时，才补一个 00
        else if (i + 2 < size &&
            data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {

            buffer.push_back(std::byte(0x00));
            buffer.push_back(std::byte(0x00));
            buffer.push_back(std::byte(0x00)); // 补入 00
            buffer.push_back(std::byte(0x01));
            i += 3; // 跳过这 3 个字节
        }
        // 3. 普通数据
        else {
            buffer.push_back(std::byte(data[i]));
            i++;
        }
    }
    return buffer;
}
//TODO:使用m_isDoingWork+cond条件保护推流线程
void WebRTCPublisher::doPublishingWork() {
    if (!m_isPublishing.load()) {
        WRITE_LOG("WebRTC publishing loop finished.");
        return;
    }
    if (!m_encodedPacketQueue) {
        WRITE_LOG("Encoded packet queue is null in doPublishingWork. Stopping publisher.");
        return;
    }
    AVPacketPtr packet;
    if (m_encodedPacketQueue->dequeue(packet)) {
        // 使用 try_dequeue 避免阻塞
        try {
            if (packet->stream_index ==0 && m_videoTrack && m_videoTrack->isOpen()) {
                auto normalizedData = normalizeH264StartCodes(packet->data, packet->size);
                //m_videoTrack->send(
                //    reinterpret_cast<const std::byte*>(packet->data),
                //    packet->size
                //);
                m_videoTrack->send(normalizedData);
 /*               if (packet->flags & AV_PKT_FLAG_KEY) {
                    WRITE_LOG("WebRTC: Sent Video Keyframe (Original Size: %d, Sent: %d)",
                        packet->size, normalizedData.size());
                }*/
                //WRITE_LOG("sending Video frame, Size: %d, frame = %s", packet->size, packet->flags);
                //WRITE_LOG("sending Video frame");
             
            } else if (packet->stream_index ==1 && m_audioTrack && m_audioTrack->isOpen()) {
                //WRITE_LOG("sending audio packet, size: %d", packet->size);
                m_audioTrack->send(
                    reinterpret_cast<const std::byte*>(packet->data),
                    packet->size
                );
            }
        } catch (const std::exception &e) {
            WRITE_LOG("Exception while sending packet: %s", e.what());
        }
    }

    // 使用 QTimer 来避免堆栈溢出和实现非阻塞循环
    QTimer::singleShot(1, this, &WebRTCPublisher::doPublishingWork);
}

void WebRTCPublisher::clear() {

    if (m_peerConnection) {
        m_peerConnection->close();
    }

    m_peerConnection.reset();
    m_videoTrack.reset();
    m_audioTrack.reset();

    WRITE_LOG("WebRTCPublisher cleared.");
}

void WebRTCPublisher::onPLI_Received() {
    //WRITE_LOG("Libdatachannel onPLI callback!");
    // 跨线程安全地调用这个槽
    emit PLIReceived();
}
