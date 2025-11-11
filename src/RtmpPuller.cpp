#include "RtmpPuller.h"
#include "logqueue.h"
#include "log_global.h"
#include "RtmpAudioPlayer.h"
#include <QApplication>
#include <QScreen>
#include <QDebug>

RtmpPuller::RtmpPuller(QUEUE_DATA<std::unique_ptr<QImage> >* MainQimageQueue, 
	QObject* parent)
	: QObject{parent}, m_MainQimageQueue(MainQimageQueue)
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

	connect(this, &RtmpPuller::VideostreamOpened,
		this, &RtmpPuller::onStreamOpened_initVideo, Qt::QueuedConnection);
	connect(this, &RtmpPuller::AudiostreamOpened,
		this, &RtmpPuller::onStreamOpened_initAudio, Qt::QueuedConnection);

	// 转发错误信号
	connect(m_videoDecoder, &ffmpegVideoDecoder::errorOccurred, this, &RtmpPuller::errorOccurred);
	connect(m_audioPlayer, &RtmpAudioPlayer::errorOccurred, this, &RtmpPuller::errorOccurred);

	WRITE_LOG("RtmpPuller (Player Module) created.");
}


RtmpPuller::~RtmpPuller() {
	clear();
	WRITE_LOG("RtmpPuller (Player Module) destroyed.");
}



void RtmpPuller::ChangePullingState(bool isPulling) {
	m_isPulling = isPulling;

}

bool RtmpPuller::init(QString RtmpUrl) {
	m_fmtCtx = avformat_alloc_context();
	if (!m_fmtCtx) {
		emit errorOccurred("RtmpPuller: Failed to allocate AVFormatContext");
		return false;
	}

	AVDictionary* opts = nullptr;
	av_dict_set(&opts, "stimeout", "5000000", 0);
	av_dict_set(&opts, "probesize", "4096", 0);

	int ret = avformat_open_input(&m_fmtCtx, m_rtmpPullerLink.toStdString().c_str(), nullptr, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		//emit errorOccurred(QString("RtmpPuller: Failed to open stream: %1").arg(avErrorToString(ret)));
		return false;
	}

	if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
		emit errorOccurred("RtmpPuller: Failed to find stream info");
		return false;
	}

	m_videoStreamIndex = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	m_audioStreamIndex = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

	if (m_videoStreamIndex < 0 && m_audioStreamIndex < 0) {
		emit errorOccurred("RtmpPuller: No video or audio streams found.");
		return false;
	}
	if (m_videoStreamIndex >= 0) {
		m_vParams = m_fmtCtx->streams[m_videoStreamIndex]->codecpar;
		m_vTimeBase = m_fmtCtx->streams[m_videoStreamIndex]->time_base;
		emit VideostreamOpened(m_vParams, m_vTimeBase);
	}
	else {
        WRITE_LOG("RtmpPuller: No video stream found.");
	}
	if (m_audioStreamIndex >= 0) {
		m_aParams = m_fmtCtx->streams[m_audioStreamIndex]->codecpar;
		m_aTimeBase = m_fmtCtx->streams[m_audioStreamIndex]->time_base;
		emit AudiostreamOpened(m_aParams, m_aTimeBase);
	}
	else {
        WRITE_LOG("RtmpPuller: No audio stream found.");
	}

	return true;
}

// 在 RtmpPuller (Demuxer) 线程中初始化解码器
void RtmpPuller::onStreamOpened_initVideo(AVCodecParameters* vParams, AVRational vTimeBase) {


	if (vParams) {
		WRITE_LOG("RtmpPuller: Initializing video decoder...");
		// 跨线程调用 ffmpegVideoDecoder::init
		if (QMetaObject::invokeMethod(m_videoDecoder, "init",
			Q_ARG(AVCodecParameters*, vParams),
			Q_ARG(AVRational, vTimeBase)))
		{
			// 成功后，启动视频解码循环
			QMetaObject::invokeMethod(m_videoDecoder, "startDecoding", Qt::QueuedConnection);
		}
		else {
			emit errorOccurred("RtmpPuller: Failed to invoke video init.");
		}
	}
}

void RtmpPuller::onStreamOpened_initAudio(AVCodecParameters* aParams, AVRational aTimeBase) {

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


void RtmpPuller::startPulling() {

	if (m_isPulling) {
		WRITE_LOG("RtmpPuller already pulling.");

		return;
	}
	WRITE_LOG("RtmpPuller: Starting all threads...");

	m_videoDecodeThread->start();
	m_audioPlayThread->start();
	
	QMetaObject::invokeMethod(this, "doPullingWork", Qt::QueuedConnection);
}

void RtmpPuller::doPullingWork() {
	if (!m_isPulling) {
		WRITE_LOG("RtmpPuller: Stopping all threads...");
		clear();
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

	int ret = av_read_frame(m_fmtCtx, packet.get());

	if (ret < 0) {
		if (ret == AVERROR_EOF) {
			WRITE_LOG("RtmpPuller: End of stream.");
		}
		else if (m_isPulling == false) {
			WRITE_LOG("RtmpPuller: Pulling interrupted by user.");
		}
		else {
			WRITE_LOG("RtmpPuller: av_read_frame error:");
			//emit errorOccurred(QString("RtmpPuller: av_read_frame error: %1").arg(avErrorToString(ret)));
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
		QMetaObject::invokeMethod(this, "doDecodingPacket", Qt::QueuedConnection);
	}

}

void RtmpPuller::stopPulling() {
	if (!m_isPulling) return;
	WRITE_LOG("RtmpPuller: Stopping all threads...");

	// 1. 设置标志，这将
	m_isPulling = false;

	// 2. 停止子线程的工作循环
	//    (使用 QMetaObject::invokeMethod 确保在正确的线程中调用)
	QMetaObject::invokeMethod(m_videoDecoder, "stopDecoding", Qt::BlockingQueuedConnection);
	QMetaObject::invokeMethod(m_audioPlayer, "stopPlaying", Qt::BlockingQueuedConnection);

	// 3. 清空队列，唤醒所有在 dequeue 上等待的线程
	m_videoPacketQueue->clear();
	m_audioPacketQueue->clear();

	// 4. 退出并等待子线程 QThread 结束
	if (m_videoDecodeThread && m_videoDecodeThread->isRunning()) {
		m_videoDecodeThread->quit();
		m_videoDecodeThread->wait();
	}
	if (m_audioPlayThread && m_audioPlayThread->isRunning()) {
		m_audioPlayThread->quit();
		m_audioPlayThread->wait();
	}

	WRITE_LOG("RtmpPuller: All threads stopped.");
}

void RtmpPuller::clear() {
	// 确保在退出前清理 Demuxer 资源
	if (m_fmtCtx) {
		avformat_close_input(&m_fmtCtx);
		m_fmtCtx = nullptr;
	}

	emit streamClosed();

	stopPulling();

	// 释放子线程和工作对象
	delete m_videoDecodeThread;
	delete m_videoDecoder;
	delete m_audioPlayThread;
	delete m_audioPlayer;

	m_videoDecodeThread = nullptr;
	m_videoDecoder = nullptr;
	m_audioPlayThread = nullptr;
	m_audioPlayer = nullptr;

	WRITE_LOG("RtmpPuller resources cleared.");
}




