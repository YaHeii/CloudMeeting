#include "RtmpPuller.h"
#include "logqueue.h"
#include "log_global.h"
#include "RtmpAudioPlayer.h"
#include <QApplication>
#include <QScreen>
#include <QDebug>

RtmpPuller::RtmpPuller(QString rtmpPullerLink, 
	QUEUE_DATA<std::unique_ptr<QImage> >* MainQimageQueue, 
	QObject* parent)
	: QObject{parent}, m_rtmpPullerLink(rtmpPullerLink), m_MainQimageQueue(MainQimageQueue)
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

	connect(this, &RtmpPuller::streamOpened,
		this, &RtmpPuller::onStreamOpened_initVideo, Qt::QueuedConnection);
	connect(this, &RtmpPuller::streamOpened,
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

void RtmpPuller::startPulling() {

	if (m_isPulling) {
		WRITE_LOG("RtmpPuller already pulling.");
		return;
	}
	WRITE_LOG("RtmpPuller: Starting all threads...");

	m_videoDecodeThread->start();
	m_audioPlayThread->start();
	
	AVDictionary* opts = nullptr;
	av_dict_set(&opts, "stimeout", "5000000", 0);  // 5秒超时
	av_dict_set(&opts, "probesize", "4096", 0);
	int ret = avformat_open_input(&m_fmtCtx, m_rtmpPullerLink.toStdString().c_str(), nullptr, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		WRITE_LOG("fail to open RTM stream:", m_rtmpPullerLink);
		return;
	}

	if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
		WRITE_LOG("fail to find stream info");
		return;
	}

	m_videoStreamIndex = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	m_audioStreamIndex = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	QMetaObject::invokeMethod(this, "doPullingWork", Qt::QueuedConnection);
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
	WRITE_LOG("RtmpPuller cleared.");

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

// 在 RtmpPuller (Demuxer) 线程中初始化解码器
void RtmpPuller::onStreamOpened_initVideo(AVCodecParameters* vParams, AVCodecParameters* aParams, AVRational vTimeBase, AVRational aTimeBase) {
	Q_UNUSED(aParams);
	Q_UNUSED(aTimeBase);

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

void RtmpPuller::onStreamOpened_initAudio(AVCodecParameters* vParams, AVCodecParameters* aParams, AVRational vTimeBase, AVRational aTimeBase) {
	Q_UNUSED(vParams);
	Q_UNUSED(vTimeBase);

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

bool RtmpPuller::initialize() { 
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

	return true;
}

void RtmpPuller::doPullingWork() {
	m_isPulling = true;

	if (!initialize()) {
		clear(); // 清理已分配的资源
		m_isPulling = false;
		return;
	}

	WRITE_LOG("RtmpPuller (Demuxer) thread started. VideoIdx: %d, AudioIdx: %d", m_videoStreamIndex, m_audioStreamIndex);

	// --- 提取参数并发送信号 ---
	// (这必须在 initialize() 之后，在循环开始之前)
	AVCodecParameters* vParams = nullptr;
	AVCodecParameters* aParams = nullptr;
	AVRational vTimeBase = { 0, 1 };
	AVRational aTimeBase = { 0, 1 };

	if (m_videoStreamIndex >= 0) {
		vParams = m_fmtCtx->streams[m_videoStreamIndex]->codecpar;
		vTimeBase = m_fmtCtx->streams[m_videoStreamIndex]->time_base;
	}
	if (m_audioStreamIndex >= 0) {
		aParams = m_fmtCtx->streams[m_audioStreamIndex]->codecpar;
		aTimeBase = m_fmtCtx->streams[m_audioStreamIndex]->time_base;
	}

	// 发送信号，触发 onStreamOpened_initVideo/Audio 槽
	emit streamOpened(vParams, aParams, vTimeBase, aTimeBase);


	// --- 解包循环 ---
	while (m_isPulling) {
		AVPacketPtr packet(av_packet_alloc());
		if (!packet) {
			WRITE_LOG("RtmpPuller: Failed to allocate AVPacket.");
			break;
		}

		// av_read_frame 会被 interruptCallback 中断
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
			break; // 退出循环
		}

		// 将包放入正确的内部队列
		if (packet->stream_index == m_videoStreamIndex) {
			m_videoPacketQueue->enqueue(std::move(packet));
		}
		else if (packet->stream_index == m_audioStreamIndex) {
			m_audioPacketQueue->enqueue(std::move(packet));
		}
		else {
			// 丢弃其他包
		}
	}

	// --- 循环结束 ---
	m_isPulling = false;

	// 确保在退出前清理 Demuxer 资源
	if (m_fmtCtx) {
		avformat_close_input(&m_fmtCtx);
		m_fmtCtx = nullptr;
	}

	// 触发子线程停止 (通过清空队列)
	m_videoPacketQueue->clear();
	m_audioPacketQueue->clear();

	emit streamClosed();
	WRITE_LOG("RtmpPuller (Demuxer) thread finished.");
}