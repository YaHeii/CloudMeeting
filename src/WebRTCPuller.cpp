#include "WebRTCPuller.h"


WebRTCPuller::WebRTCPuller(QUEUE_DATA<std::unique_ptr<QImage> >* MainQimageQueue,
	QObject *parent)
	: QObject(parent), m_MainQimageQueue(MainQimageQueue)
{ 

	m_videoPacketQueue = new QUEUE_DATA<AVPacketPtr>();
    m_audioPacketQueue = new QUEUE_DATA<AVPacketPtr>();
	m_dummyVideoFrameQueue = new QUEUE_DATA<AVFramePtr>();
    m_networkManager = nullptr;
    m_networkManager = new QNetworkAccessManager(this);
    rtcPreload();


	m_videoDecoder = new ffmpegVideoDecoder(m_videoPacketQueue,
											m_MainQimageQueue, // 使用外部 QImage 队列
											m_dummyVideoFrameQueue);
	m_audioPlayer = new AudioPlayer(m_audioPacketQueue);

	m_videoDecodeThread = new QThread();
	m_audioPlayThread = new QThread();
	m_videoDecoder->moveToThread(m_videoDecodeThread);
	m_audioPlayer->moveToThread(m_audioPlayThread);
    m_videoDecodeThread->start();
    m_audioPlayThread->start();


	connect(this, &WebRTCPuller::VideostreamOpened,
		this, &WebRTCPuller::onStreamOpened_initVideo, Qt::QueuedConnection);
	connect(this, &WebRTCPuller::AudiostreamOpened,
		this, &WebRTCPuller::onStreamOpened_initAudio, Qt::QueuedConnection);

	connect(m_videoDecoder, &ffmpegVideoDecoder::errorOccurred, this, &WebRTCPuller::errorOccurred);
	connect(m_audioPlayer, &AudioPlayer::errorOccurred, this, &WebRTCPuller::errorOccurred);
	connect(m_videoDecoder, &ffmpegVideoDecoder::newFrameAvailable, this, &WebRTCPuller::newFrameAvailable);
	WRITE_LOG("WebRTCPuller (Player Module) created.");
}

WebRTCPuller::~WebRTCPuller()
{
	clear();
	WRITE_LOG("WebRTCPuller (Player Module) destroyed.");
}


bool WebRTCPuller::init(QString WebRTCUrl, QString audioDeviceName) {

	WRITE_LOG("Initializing WebRTC Puller");

	//// 初始化dc日志系统，使用内置日志系统输出
	//rtc::InitLogger(rtc::LogLevel::Verbose, [](rtc::LogLevel level, rtc::string message) {
	//    const char* file = "libdatachannel";
	//    const char* function = "rtc_callback";
	//    int line =0;
	//    LogQueue::GetInstance().print(file, function, line, "%s", message.c_str());
	//});
    m_videoDepacketizer = new RTPDepacketizer(90000,m_videoPacketQueue,true,this);
    m_audioDepacketizer = new RTPDepacketizer(48000,m_audioPacketQueue,false,this);
	m_signalingUrl = WebRTCUrl;
	m_streamUrl = WebRTCUrl;
	m_rtcConfig.iceServers.clear();
	m_rtcConfig.iceServers.emplace_back(rtc::IceServer("stun.iptel.org"));
	m_rtcConfig.iceServers.emplace_back(rtc::IceServer("stun.rixtelecom.se"));
	m_rtcConfig.iceServers.emplace_back(rtc::IceServer("stun:stun.l.google.com:19302"));
	m_rtcConfig.iceServers.emplace_back(rtc::IceServer("stun:stun1.l.google.com:19302"));
	m_rtcConfig.mtu = 1500;
	m_rtcConfig.portRangeBegin = 10000;
	m_rtcConfig.portRangeEnd = 20000;
    if (m_audioPlayer) {
        m_audioPlayer->setTargetDeviceName(audioDeviceName);
    }
    initializePeerConnection();
	return true;
}

void WebRTCPuller::initializePeerConnection()
{ 
    try {
        m_peerConnection = std::make_unique<rtc::PeerConnection>(m_rtcConfig);

        //// 创建SDP
        // video part
        rtc::Description::Video video("video");
        //rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addH264Codec(96);
        video.addRtxCodec(97, 96, 90000);
        video.addSSRC(42, "video-send", "video-stream", "video-track");
        video.setDirection(rtc::Description::Direction::RecvOnly);
        m_videoTrack = m_peerConnection->addTrack(video);
        WRITE_LOG("Video RecvOnly track (H.264) added.");
        m_videoTrack->onMessage([this](rtc::message_variant message) {
            std::string trackType = m_videoTrack->description();
            //WRITE_LOG("Stream Track Received! MID: %s", m_videoTrack->mid().c_str());

            if (!m_isPulling) {
                return;
            }
            if (!std::holds_alternative<rtc::binary>(message)) return;

            auto& data_bin = std::get<rtc::binary>(message);
            // data_bin 是 std::vector<byte> 或类似结构
            if (m_videoDepacketizer) {
                 WRITE_LOG("Video Packet Received size=%d", data_bin.size());
                m_videoDepacketizer->pushPacket(
                    reinterpret_cast<const uint8_t*>(data_bin.data()),
                    data_bin.size()
                );
            }
         });

        rtc::Description::Audio audio("audio");
        //rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
        audio.addOpusCodec(111);
        audio.addSSRC(43, "audio-send", "audio-stream", "audio-track");
        audio.setDirection(rtc::Description::Direction::RecvOnly);
        m_audioTrack = m_peerConnection->addTrack(audio);
        WRITE_LOG("Audio track (Opus) added.");

        m_audioTrack->onMessage([this](rtc::message_variant message) {
            std::string trackType = m_audioTrack->description();
            //WRITE_LOG("Stream Track Received! MID: %s", m_audioTrack->mid().c_str());

            if (!m_isPulling) {
                return;
            }
            if (!std::holds_alternative<rtc::binary>(message)) return;

            auto& data_bin = std::get<rtc::binary>(message);
            // data_bin 是 std::vector<byte> 或类似结构
            if (m_audioDepacketizer) {
                 WRITE_LOG("Audio Packet Received size=%d", data_bin.size());
                m_audioDepacketizer->pushPacket(
                    reinterpret_cast<const uint8_t*>(data_bin.data()),
                    data_bin.size()
                );
            }
         });
        //// description回调
        m_peerConnection->onLocalDescription([this](const rtc::Description& description) {
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
            if (state == rtc::PeerConnection::State::Connected) {
                initAudioCodecParams();
                initVideoCodecParams();
            }
            else if (state == rtc::PeerConnection::State::Failed) {
                emit errorOccurred("WebRTC connection failed.");
                stopPulling();
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
        emit initSuccess();
        WRITE_LOG("Local description set, waiting for ICE gathering...");
    }
    catch (const std::exception& e) {
        QString error = QString("Failed to create PeerConnection: %1").arg(e.what());
        WRITE_LOG(error.toStdString().c_str());
        emit errorOccurred(error);
    }
}


void WebRTCPuller::sendOfferToSignalingServer(const std::string& sdp) {

    QUrl url(m_signalingUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    WRITE_LOG("Sending Offer SDP to %s", m_signalingUrl.toStdString().c_str());

    QByteArray body = QByteArray::fromStdString(sdp);
    WRITE_LOG("Request JSON:\n%s", body.constData());

    QNetworkReply* response = nullptr;
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


    QTimer* timeoutTimer = new QTimer(this);
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

void WebRTCPuller::onSignalingReply(QNetworkReply* response) {
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
        WRITE_LOG("WHEP publish successful,SDP_Answer:", sdpAnswer);
    }
    else {
        WRITE_LOG("WHEP failed with status:", httpStatus);
        emit errorOccurred(QString("WHEP failed: HTTP %1").arg(httpStatus));
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

void WebRTCPuller::initAudioCodecParams() {

    m_aParams = avcodec_parameters_alloc();
    m_aParams->codec_type = AVMEDIA_TYPE_AUDIO;
    m_aParams->codec_id = AV_CODEC_ID_OPUS;
    m_aParams->sample_rate = 48000;
    m_aParams->ch_layout.nb_channels = 2; 

    // RTP Opus 标准时基
    m_aTimeBase = { 1, 48000 };

    emit AudiostreamOpened(m_aParams, m_aTimeBase);

}
void WebRTCPuller::initVideoCodecParams() {
    // 1. 构造 Video Parameters (H.264)
    m_vParams = avcodec_parameters_alloc();
    m_vParams->codec_type = AVMEDIA_TYPE_VIDEO;
    m_vParams->codec_id = AV_CODEC_ID_H264;
    // 宽高先给0或者预设值，FFmpeg收到第一个SPS包后会自动更新Context
    m_vParams->width = 1920;
    m_vParams->height = 1080;

    // RTP H.264 标准时基
    m_vTimeBase = { 1, 90000 };
    emit VideostreamOpened(m_vParams, m_vTimeBase);
}

void WebRTCPuller::onStreamOpened_initVideo(AVCodecParameters* vParams, AVRational vTimeBase) {


	if (vParams) {
		WRITE_LOG("WebRTCPuller: Initializing video decoder...");
		// 跨线程调用 ffmpegVideoDecoder::init
		if (QMetaObject::invokeMethod(m_videoDecoder, "init",
			Q_ARG(AVCodecParameters*, vParams),
			Q_ARG(AVRational, vTimeBase)))
		{
			// 成功后，启动视频解码循环
			QMetaObject::invokeMethod(m_videoDecoder, "ChangeDecodingState", Qt::QueuedConnection, Q_ARG(bool, true));
		}
		else {
			emit errorOccurred("WebRTCPuller: Failed to invoke video init.");
		}
	}
}

void WebRTCPuller::onStreamOpened_initAudio(AVCodecParameters* aParams, AVRational aTimeBase) {

	if (aParams) {
		WRITE_LOG("WebRTCPuller: Initializing audio player...");
		// 跨线程调用 RtmpAudioPlayer::init
		if (QMetaObject::invokeMethod(m_audioPlayer, "init",
			Q_ARG(AVCodecParameters*, aParams),
			Q_ARG(AVRational, aTimeBase)))
		{
			// 成功后，启动音频解码播放循环
            // TODO:修改接口
			QMetaObject::invokeMethod(m_audioPlayer, "startPlaying", Qt::QueuedConnection);
		}
		else {
			emit errorOccurred("WebRTCPuller: Failed to invoke audio init.");
		}
	}
}

void WebRTCPuller::ChangePullingState(bool isPulling) {
	m_isPulling = isPulling;
	if (m_isPulling) {
		WRITE_LOG("WebRTCPuller: Start pulling.");
		startPulling();
	}
	else {
		stopPulling();
	}
}

void WebRTCPuller::startPulling() {
    m_isPulling = true;

	WRITE_LOG("WebRTCPuller: Starting all threads...");

}

void WebRTCPuller::stopPulling() {
	m_isPulling = false;
	WRITE_LOG("WebRTCPuller: Stopping all threads...");
}



void WebRTCPuller::clear() { 
    WRITE_LOG("TO CLEAR THREAD");
}