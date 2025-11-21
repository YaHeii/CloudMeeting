#include "WebRTCPuller.h"
#include "logqueue.h"
#include "log_global.h"
#include "RtmpAudioPlayer.h"
#include <QApplication>
#include <QScreen>
#include <QDebug>
#include "RTPDepacketizer.h"

WebRTCPuller::WebRTCPuller(QUEUE_DATA<std::unique_ptr<QImage> >* MainQimageQueue,
	QObject *parent)
	: QObject(parent), m_MainQimageQueue(MainQimageQueue)
{ 

	m_videoPacketQueue = new QUEUE_DATA<AVPacketPtr>();
    m_audioPacketQueue = new QUEUE_DATA<AVPacketPtr>();
	m_dummyVideoFrameQueue = new QUEUE_DATA<AVFramePtr>();

	m_videoDecoder = new ffmpegVideoDecoder(m_videoPacketQueue,
											m_MainQimageQueue, // 使用外部 QImage 队列
											m_dummyVideoFrameQueue);
	m_audioPlayer = new RtmpAudioPlayer(m_audioPacketQueue);

	m_videoDecodeThread = new QThread();
	m_audioPlayThread = new QThread();
	m_videoDecoder->moveToThread(m_videoDecodeThread);
	m_audioPlayer->moveToThread(m_audioPlayThread);

	connect(this, &WebRTCPuller::VideostreamOpened,
		this, &WebRTCPuller::onStreamOpened_initVideo, Qt::QueuedConnection);
	connect(this, &WebRTCPuller::AudiostreamOpened,
		this, &WebRTCPuller::onStreamOpened_initAudio, Qt::QueuedConnection);

	connect(m_videoDecoder, &ffmpegVideoDecoder::errorOccurred, this, &WebRTCPuller::errorOccurred);
	connect(m_audioPlayer, &RtmpAudioPlayer::errorOccurred, this, &WebRTCPuller::errorOccurred);
	connect(m_videoDecoder, &ffmpegVideoDecoder::newFrameAvailable, this, &WebRTCPuller::newFrameAvailable);
	WRITE_LOG("WebRTCPuller (Player Module) created.");
}

WebRTCPuller::~WebRTCPuller()
{
	clear();
	WRITE_LOG("WebRTCPuller (Player Module) destroyed.");
}


bool WebRTCPuller::init(QString WebRTCUrl) {

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


        rtc::Description::Audio audio("audio");
        //rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
        audio.addOpusCodec(111);
        audio.addSSRC(43, "audio-send", "audio-stream", "audio-track");
        audio.setDirection(rtc::Description::Direction::RecvOnly);
        m_audioTrack = m_peerConnection->addTrack(audio);
        WRITE_LOG("Audio track (Opus) added.");


        m_peerConnection->onTrack([this](std::shared_ptr<rtc::Track> track) {
            WRITE_LOG("Stream Track Received! MID: %s", track->mid().c_str());
            std::string trackType = track->description();
            bool isVideo = (trackType.find("video") != std::string::npos);

            // 绑定 RTP 数据回调
            track->onMessage([this, isVideo](rtc::message_variant message) {
                if (!std::holds_alternative<rtc::binary>(message)) return;

                auto& data_bin = std::get<rtc::binary>(message);
                // data_bin 是 std::vector<byte> 或类似结构

                // 将 std::byte 转换为 uint8_t 指针
                const uint8_t* data_ptr = reinterpret_cast<const uint8_t*>(data_bin.data());
                size_t data_size = data_bin.size();

                // 判断是视频还是音频
                // 注意：实际判断通常基于 Description 或 SDP 协商结果，这里简单通过 mid 或 description 判断
                if (isVideo) {
                    // === 视频处理 ===
                    // 直接推入解包器。
                    // 解包器内部负责：JitterBuffer排序 -> 定时取包 -> FU-A重组 -> 加上00000001 -> 入队 m_videoPacketQueue
                    if (m_videoDepacketizer) {
                        m_videoDepacketizer->pushPacket(data_ptr, data_size);
                    } else {
                        WRITE_LOG("Video Depacketizer Failed.");
                    }
                } else {
                    if (m_audioDepacketizer) {
                        m_audioDepacketizer->pushPacket(data_ptr, data_size);
                    } else {
                        WRITE_LOG("Audio Depacketizer Failed.");
                    }

                }
             });
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
                initCodecParams();
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

void WebRTCPuller::initCodecParams() {
    // 1. 构造 Video Parameters (H.264)
    AVCodecParameters* vParams = avcodec_parameters_alloc();
    vParams->codec_type = AVMEDIA_TYPE_VIDEO;
    vParams->codec_id = AV_CODEC_ID_H264;
    // 宽高先给0或者预设值，FFmpeg收到第一个SPS包后会自动更新Context
    vParams->width = 1920;
    vParams->height = 1080;

    // RTP H.264 标准时基
    AVRational vTimeBase = { 1, 90000 };

    // 2. 构造 Audio Parameters (Opus)
    AVCodecParameters* aParams = avcodec_parameters_alloc();
    aParams->codec_type = AVMEDIA_TYPE_AUDIO;
    aParams->codec_id = AV_CODEC_ID_OPUS;
    aParams->sample_rate = 48000;
    aParams->ch_layout.nb_channels = 2; 

    // RTP Opus 标准时基
    AVRational aTimeBase = { 1, 48000 };

    emit VideostreamOpened(vParams, vTimeBase);
    emit AudiostreamOpened(aParams, aTimeBase);

}


void WebRTCPuller::onStreamOpened_initVideo(AVCodecParameters* vParams, AVRational vTimeBase) {


	if (vParams) {
		WRITE_LOG("RtmpPuller: Initializing video decoder...");
		// 跨线程调用 ffmpegVideoDecoder::init
		if (QMetaObject::invokeMethod(m_videoDecoder, "init",
			Q_ARG(AVCodecParameters*, vParams),
			Q_ARG(AVRational, vTimeBase)))
		{
			// 成功后，启动视频解码循环
			QMetaObject::invokeMethod(m_videoDecoder, "ChangeDecodingState", Qt::QueuedConnection, Q_ARG(bool, true));
		}
		else {
			emit errorOccurred("RtmpPuller: Failed to invoke video init.");
		}
	}
}

void WebRTCPuller::onStreamOpened_initAudio(AVCodecParameters* aParams, AVRational aTimeBase) {

	if (aParams) {
		WRITE_LOG("RtmpPuller: Initializing audio player...");
		// 跨线程调用 RtmpAudioPlayer::init
		if (QMetaObject::invokeMethod(m_audioPlayer, "init",
			Q_ARG(AVCodecParameters*, aParams),
			Q_ARG(AVRational, aTimeBase)))
		{
			// 成功后，启动音频解码播放循环
			QMetaObject::invokeMethod(m_audioPlayer, "startPlaying", Qt::QueuedConnection);
		}
		else {
			emit errorOccurred("RtmpPuller: Failed to invoke audio init.");
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

	if (m_isPulling) {
		WRITE_LOG("WebRTCPuller already pulling.");
		return;
	}
	m_isPulling = true;
	WRITE_LOG("WebRTCPuller: Starting all threads...");
	m_videoDecodeThread->start();
	m_audioPlayThread->start();

	QMetaObject::invokeMethod(this, "doPullingWork", Qt::QueuedConnection);
}

void WebRTCPuller::stopPulling() {
	if (!m_isPulling) {
		WRITE_LOG("WebRTCPuller already stopped.");
		return;
	}
	m_isPulling = false;
	WRITE_LOG("WebRTCPuller: Stopping all threads...");
}


void WebRTCPuller::doPullingWork() {
    //WRITE_LOG("doPullingWork clicked");

    if (!m_isPulling) {
        WRITE_LOG("RtmpPuller: Stopping all threads...");
        return;
    }

    AVPacketPtr packet(av_packet_alloc());
    if (!packet) {
        WRITE_LOG("RtmpPuller: Failed to allocate AVPacket.");
        if (m_isPulling) {
            QMetaObject::invokeMethod(this, "doPullingWork", Qt::QueuedConnection);
        }
        return;
    }
    {
        QMutexLocker locker(&m_workMutex);
        m_isDoingWork = true;
    }
    auto work_guard = [this]() {
        QMutexLocker locker(&m_workMutex);
        m_isDoingWork = false;
        m_workCond.wakeAll();
        };
    int ret = 0;
    if (ret = av_read_frame(m_fmtCtx, packet.get()) < 0) {
        if (ret == AVERROR_EOF) {
            WRITE_LOG("RtmpPuller: End of stream.");
        }
        else if (m_isPulling == false) {
            WRITE_LOG("RtmpPuller: Pulling interrupted by user.");
        }
        else {
            WRITE_LOG("RtmpPuller: av_read_frame error:");
            emit errorOccurred("RtmpPuller: av_read_frame error: %1");
        }

    }

    // 将包放入正确的内部队列
    if (packet->stream_index == m_videoStreamIndex) {
        m_videoPacketQueue->enqueue(std::move(packet));
    }
    else if (packet->stream_index == m_audioStreamIndex) {
        m_audioPacketQueue->enqueue(std::move(packet));
    }
    else {
        WRITE_LOG("RtmpPuller: Unknown stream index.");
    }

    work_guard();
    if (m_isPulling) {
        QMetaObject::invokeMethod(this, "doPullingWork", Qt::QueuedConnection);
    }

}