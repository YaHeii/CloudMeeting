#include "mainwindow.h"
#include "ui_mainwindow.h"

QRect MainWindow::pos = QRect(-1, -1, -1, -1);

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
      , ui(new Ui::MainWindow) {
    qRegisterMetaType<MSG_TYPE>();
    qRegisterMetaType<AudioResampleConfig>();
    // 日志单例初始化
    LogQueue::GetInstance().start();

    // UI初始化
    ui->setupUi(this);
    _createmeet = false;
    // _openCamera = false;
    _joinmeet = false;
    Screen::init();
    MainWindow::pos = QRect(0.1 * Screen::width, 0.1 * Screen::height, 0.8 * Screen::width, 0.8 * Screen::height);

    m_videoLocalWidget = ui->smallVideoDisplayWidget;
    m_videoRemoteWidget = ui->mainVideoDisplayWidget;

    ui->openAudio->setText(QString(OPENAUDIO).toUtf8());
    ui->openVideo->setText(QString(OPENVIDEO).toUtf8());

    this->setGeometry(this->pos);
    this->setMinimumSize(QSize(this->pos.width() * 0.7, this->pos.height() * 0.7));
    this->setMaximumSize(QSize(this->pos.width(), this->pos.height()));

    ui->exitmeetBtn->setDisabled(false);
    ui->joinmeetBtn->setDisabled(false);
    ui->createmeetBtn->setDisabled(false);
    ui->openAudio->setDisabled(false);
    ui->openVideo->setDisabled(false);
    ui->sendmsg->setDisabled(false);
    ui->meetnum->setPlaceholderText("rtmp://127.0.0.1:1935/live/teststream");
    mainip = 0; //主屏幕显示的用户IP图像

    // 初始化队列
	m_videoPacketQueue = new QUEUE_DATA<AVPacketPtr>();//采集到的视频包队列
    m_SmallQimageQueue = new QUEUE_DATA<std::unique_ptr<QImage> >();
    m_videoFrameQueue = new QUEUE_DATA<AVFramePtr>();
    m_audioPacketQueue = new QUEUE_DATA<AVPacketPtr>();
    m_audioFrameQueue = new QUEUE_DATA<AVFramePtr>();
    m_publishPacketQueue = new QUEUE_DATA<AVPacketPtr>();
	m_MainQimageQueue = new QUEUE_DATA<std::unique_ptr<QImage> >(); //拉流得到的显示队列，暂时不开启线程

    //// TODO: 创建工厂管理线程，加快启动速度
    // TODO: 创建视频与音频参数单例结构体，不传递vParams 和 aParams
    // 音频采集线程
    // 视频采集线程
    m_VideoCaptureThread = new QThread(this);
    m_VideoCapture = new Capture(m_videoPacketQueue, nullptr);
    m_VideoCapture->moveToThread(m_VideoCaptureThread);
    m_VideoCaptureThread->start();
    connect(m_VideoCapture, &Capture::videoDeviceOpenSuccessfully, this, &MainWindow::onVideoDeviceOpened);
    // 音频采集线程
    m_AudioCaptureThread = new QThread(this);
    m_AudioCapture = new Capture(nullptr, m_audioPacketQueue);
    m_AudioCapture->moveToThread(m_AudioCaptureThread);
    m_AudioCaptureThread->start();
    connect(m_AudioCapture, &Capture::audioDeviceOpenSuccessfully, this, &MainWindow::onAudioDeviceOpened);

    //音频解码线程
    m_audioDecoderThread = new QThread(this);
    m_audioDecoder = new ffmpegAudioDecoder(m_audioPacketQueue, m_audioFrameQueue);
    m_audioDecoder->moveToThread(m_audioDecoderThread);
    m_audioDecoderThread->start();
    //视频解码线程
    m_videoDecoderThread = new QThread(this);
    m_videoDecoder = new ffmpegVideoDecoder(m_videoPacketQueue, m_SmallQimageQueue, m_videoFrameQueue);
    m_videoDecoder->moveToThread(m_videoDecoderThread);
    m_videoDecoderThread->start();

    // 音频编码线程
    m_audioEncoderThread = new QThread(this);
    m_audioEncoder = new ffmpegEncoder(m_audioFrameQueue, m_publishPacketQueue);
    m_audioEncoder->moveToThread(m_audioEncoderThread);
    m_audioEncoderThread->start();
    // 视频编码线程
    m_videoEncoderThread = new QThread(this);
    m_videoEncoder = new ffmpegEncoder(m_videoFrameQueue, m_publishPacketQueue);
    m_videoEncoder->moveToThread(m_videoEncoderThread);
    m_videoEncoderThread->start();

    //RTMP推流线程
    m_rtmpPublisherThread = new QThread(this);
    m_rtmpPublisher = new RtmpPublisher(m_publishPacketQueue);
    m_rtmpPublisher->moveToThread(m_rtmpPublisherThread);
    m_rtmpPublisherThread->start();

    //WebRTC推流线程
    m_webRTCPublisherThread = new QThread(this);
    m_webRTCPublisher = new WebRTCPublisher(m_publishPacketQueue);
    m_webRTCPublisher->moveToThread(m_webRTCPublisherThread);
    m_webRTCPublisherThread->start();
    QMetaObject::invokeMethod(m_webRTCPublisher, "initThread", Qt::QueuedConnection);// 为了初始化libdatachannel
    connect(m_webRTCPublisher, &WebRTCPublisher::PLIReceived, this, &MainWindow::on_PLIReceived_webrtcPublisher, Qt::QueuedConnection);//处理RTC->RTMP转码时的PLI请求

    // RTMP拉流
    m_rtmpPullerThread = new QThread(this);
    m_rtmpPuller = new RtmpPuller(m_MainQimageQueue);
    m_rtmpPuller->moveToThread(m_rtmpPullerThread);
    m_rtmpPullerThread->start();
    connect(m_rtmpPuller, &RtmpPuller::newFrameAvailable, this, &MainWindow::onNewRemoteFrameAvailable, Qt::QueuedConnection);

    //获取可用设备
    QStringList videoDevices = DeviceEnumerator::getDevices(MediaType::Video);
    ui->videoDeviceComboBox->addItems(videoDevices);
    QStringList audioDevices = DeviceEnumerator::getDevices(MediaType::Audio);
    ui->audioDevicecomboBox->addItems(audioDevices);


    
    //// TODO：创建全局单例->参数管理器，解耦编码器参数传递
    //// 视频
    connect(m_videoDecoder, &ffmpegVideoDecoder::newFrameAvailable, this, &MainWindow::onNewLocalFrameAvailable,
            Qt::QueuedConnection);
    connect(m_videoEncoder, &ffmpegEncoder::initializationSuccess, this, &MainWindow::videoEncoderReady);

    //// 音频
    connect(m_audioEncoder, &ffmpegEncoder::initializationSuccess, this, &MainWindow::audioEncoderReady);


    //errorOccurred处理
    connect(m_videoDecoder, &ffmpegVideoDecoder::errorOccurred, this, &MainWindow::handleError);
    connect(m_AudioCapture, &Capture::errorOccurred, this, &MainWindow::handleError);
    connect(m_audioDecoder, &ffmpegAudioDecoder::errorOccurred, this, &MainWindow::handleError);
    connect(m_audioEncoder, &ffmpegEncoder::errorOccurred, this, &MainWindow::handleError);
    connect(m_videoEncoder, &ffmpegEncoder::errorOccurred, this, &MainWindow::handleError);
    connect(m_VideoCapture, &Capture::errorOccurred, this, &MainWindow::handleError);
    connect(m_webRTCPublisher, &WebRTCPublisher::errorOccurred, this, &MainWindow::handleError);
    connect(m_rtmpPuller, &RtmpPuller::errorOccurred, this, &MainWindow::handleError);
    // 线程结束后，自动清理工作对象和线程本身
    connect(m_VideoCaptureThread, &QThread::finished, m_VideoCapture, &QObject::deleteLater);    
    connect(m_AudioCaptureThread, &QThread::finished, m_AudioCapture, &QObject::deleteLater);
    connect(m_videoDecoderThread, &QThread::finished, m_videoDecoder, &QObject::deleteLater);
    connect(m_audioDecoderThread, &QThread::finished, m_audioDecoder, &QObject::deleteLater);
    connect(m_audioEncoderThread, &QThread::finished, m_audioEncoder, &QObject::deleteLater);
    connect(m_videoEncoderThread, &QThread::finished, m_videoEncoder, &QObject::deleteLater);
    connect(m_webRTCPublisherThread, &QThread::finished, m_webRTCPublisher, &QObject::deleteLater);
}

MainWindow::~MainWindow() {
    // 停止视频采集
    if (m_VideoCaptureThread && m_VideoCaptureThread->isRunning()) {
        QMetaObject::invokeMethod(m_VideoCapture, "closeDevice", Qt::BlockingQueuedConnection);
        m_VideoCaptureThread->quit();
        m_VideoCaptureThread->wait();
    }
    // 停止音频采集
    if (m_AudioCaptureThread && m_AudioCaptureThread->isRunning()) {
        QMetaObject::invokeMethod(m_AudioCapture, "closeDevice", Qt::BlockingQueuedConnection);
        m_AudioCaptureThread->quit();
        m_AudioCaptureThread->wait();
    }

    // 停止音频解码
    if (m_audioDecoderThread && m_audioDecoderThread->isRunning()) {
        QMetaObject::invokeMethod(m_audioDecoder, "stopDecoding", Qt::BlockingQueuedConnection);
        m_audioDecoderThread->quit();
        m_audioDecoderThread->wait();
    }
    // 停止视频解码
    if (m_videoDecoderThread && m_videoDecoderThread->isRunning()) {
        QMetaObject::invokeMethod(m_videoDecoder, "stopDecoding", Qt::BlockingQueuedConnection);
        m_videoDecoderThread->quit();
        m_videoDecoderThread->wait();
    }
    // 停止RTMP
    if (m_rtmpPullerThread && m_rtmpPullerThread->isRunning()) {
        // 使用 BlockingQueuedConnection 确保 clear() 执行完毕
        QMetaObject::invokeMethod(m_rtmpPuller, "clear", Qt::BlockingQueuedConnection);
        m_rtmpPullerThread->quit();
        m_rtmpPullerThread->wait();
    }

    // 清理队列，确保在其他对象销毁前清理队列以避免wakeAll崩溃
    if (m_videoPacketQueue) {
        m_videoPacketQueue->clear();
        delete m_videoPacketQueue;
        m_videoPacketQueue = nullptr;
    }

    if (m_videoFrameQueue) {
        m_videoFrameQueue->clear();
        delete m_videoFrameQueue;
        m_videoFrameQueue = nullptr;
    }

    if (m_audioPacketQueue) {
        m_audioPacketQueue->clear();
        delete m_audioPacketQueue;
        m_audioPacketQueue = nullptr;
    }

    delete ui;
}

//// 打开视频按钮（开关复用）
void MainWindow::on_openVideo_clicked() {
    if (m_isVideoRunning) {
        QMetaObject::invokeMethod(m_VideoCapture, "closeVideo", Qt::QueuedConnection);
        ui->openVideo->setText("开启视频");
        m_isVideoRunning = false;
        qDebug() << "Video stopped.";
        WRITE_LOG("Video stopped.");
    } else {
        QString videoDevice = ui->videoDeviceComboBox->currentText();
        if (videoDevice.isEmpty()) {
            qWarning() << "No video device selected!";
            return;
        }
        QMetaObject::invokeMethod(m_VideoCapture, "openVideo", Qt::QueuedConnection,Q_ARG(QString, videoDevice));
        ui->openVideo->setText("关闭视频");
        m_isVideoRunning = true;
        qDebug() << "Video started.";
        WRITE_LOG("Video started.");
    }
}

//// 打开麦克风按钮（开关复用）
void MainWindow::on_openAudio_clicked() {
    if (m_isAudioRunning) {
        m_isAudioRunning = false;

        QMetaObject::invokeMethod(m_AudioCapture, "closeAudio", Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_audioDecoder, "ChangeDecodingState", Qt::QueuedConnection,
            Q_ARG(bool, m_isAudioRunning));
        ui->openAudio->setText("打开麦克风");

        
        qDebug() << "Audio stopped.";
        WRITE_LOG("Audio stopped.");
    } else {
        QString audioDevice = ui->audioDevicecomboBox->currentText();
        if (audioDevice.isEmpty()) {
            qWarning() << "No video device selected!";
            return;
        }
        m_isAudioRunning = true;
        QMetaObject::invokeMethod(m_AudioCapture, "openAudio", Qt::QueuedConnection,Q_ARG(QString, audioDevice));
        QMetaObject::invokeMethod(m_audioDecoder, "ChangeDecodingState",
            Q_ARG(bool, m_isAudioRunning), Qt::QueuedConnection);
        ui->openAudio->setText("关闭麦克风");
        qDebug() << "Starting Audio.";
        WRITE_LOG("Starting Audio.");
    }
}

//// 开启直播按钮
void MainWindow::on_LiveStreamingBtn_clicked() {
    qDebug() << "on_LiveStreamBtn_clicked";

    ui->LiveStreamingBtn->setEnabled(false);
    ui->createmeetBtn->setEnabled(false);

    m_isRtmpPublishRequested = true;
    m_isWebRtcPublishRequested = false;


    if (m_videoParams) {
        qDebug("Initializing video pipeline(H264)");
        QMetaObject::invokeMethod(m_videoEncoder, "initVideoEncoderH264", Qt::QueuedConnection,
                                  Q_ARG(AVCodecParameters*, m_videoParams));
    }
    if (m_audioParams) {
        qDebug("Initializing audio pipeline(AAC)");
        QMetaObject::invokeMethod(m_audioEncoder, "initAudioEncoderAAC", Qt::QueuedConnection,
                                 Q_ARG(AVCodecParameters*, m_audioParams));
    }
}
 // 创建会议按钮
void MainWindow::on_createmeetBtn_clicked() {
    qDebug() << "on_createmeetBtn_clicked";

    ui->LiveStreamingBtn->setEnabled(false);
    ui->createmeetBtn->setEnabled(false);

    m_isRtmpPublishRequested = false;
    m_isWebRtcPublishRequested = true;

    if (m_videoParams) {
        qDebug("Initializing video pipeline(H264)");
        QMetaObject::invokeMethod(m_videoEncoder, "initVideoEncoderH264", Qt::QueuedConnection,
                                  Q_ARG(AVCodecParameters*, m_videoParams));
    }
    if (m_audioParams) {
        qDebug("Initializing audio pipeline(AAC)");
        QMetaObject::invokeMethod(m_audioEncoder, "initAudioEncoderOpus", Qt::QueuedConnection,
            Q_ARG(AVCodecParameters*, m_audioParams));
    }
}

void MainWindow::audioEncoderReady() {
    m_audioEncoderReady = true;
    QMetaObject::invokeMethod(m_audioEncoder, "ChangeEncodingState",
        Q_ARG(bool, m_audioEncoderReady));
    checkAndStartPublishing();//设置工厂，启动音频
}

void MainWindow::videoEncoderReady() {
    m_videoEncoderReady = true;
    QMetaObject::invokeMethod(m_videoEncoder, "ChangeEncodingState",
        Q_ARG(bool, m_videoEncoderReady));
    checkAndStartPublishing();
}

void MainWindow::checkAndStartPublishing() { 
    if (!m_audioEncoderReady || !m_videoEncoderReady) {
        //WRITE_LOG("Waiting for %s encoder to be ready.", m_audioEncoderReady? m_videoEncoderReady: m_audioEncoderReady);
        return;
    }
    if (m_isRtmpPublishRequested) {
        WRITE_LOG("Encoders ready, starting RTMP publish...");
        AVCodecContext* videoCtx = m_videoEncoder->getCodecContext();
        AVCodecContext* audioCtx = m_audioEncoder->getCodecContext();
        if (!videoCtx || !audioCtx) {
            handleError("Error：Encoders are ready, but context is null (critical error).");
            ui->LiveStreamingBtn->setEnabled(true);
            ui->createmeetBtn->setEnabled(true);
            return;
        }
        //// TODO: rtmpUrl 应该由服务器连接->text()指定
        QString rtmpUrl = "rtmp://127.0.0.1:1935/live/teststream";
        if (!QMetaObject::invokeMethod(m_rtmpPublisher, "init",
                                        Qt::QueuedConnection,
                                        Q_ARG(QString, rtmpUrl),
                                        Q_ARG(AVCodecContext*, videoCtx),
                                        Q_ARG(AVCodecContext*, audioCtx))) {
            WRITE_LOG("Failed to invoke RTMP publisher initialization.");
        }
        else {
            QMetaObject::invokeMethod(m_rtmpPublisher, "ChangeRtmpPublishingState", Qt::QueuedConnection,
                                      Q_ARG(bool, m_isRtmpPublishRequested));
        }
        m_isRtmpPublishRequested = false;
    }
    else if (m_isWebRtcPublishRequested) {
        WRITE_LOG("Encoders ready, starting WebRTC publish...");
        AVCodecContext* videoCtx = m_videoEncoder->getCodecContext();
        AVCodecContext* audioCtx = m_audioEncoder->getCodecContext();
        if (!videoCtx || !audioCtx) {
            handleError("Error：Encoders are ready, but context is null (critical error).");
            ui->LiveStreamingBtn->setEnabled(true);
            ui->createmeetBtn->setEnabled(true);
            return;
        }
   //     QString srsServerUrl = "http://172.24.73.45:1985";
   //     //QString srsServerUrl = ui->serverUrl->text();
   //     //QString srsServerPort = ui->port->text();
   //     QString m_roomId = "roomID";
   //     QString m_userId = "user";
   //     QString webRTCsignalingUrl = srsServerUrl + "/rtc/v1/whip/?app=live&stream=" + m_roomId;
   //     QString webRTCstreamUrl = srsServerUrl + "/rtc/v1/whip/?app=live&stream=" + m_roomId;*/
   //     QString webRTCsignalingUrl = "http://localhost:1985/rtc/v1/whip/?app=live&stream=livestream";
   //     QString webRTCstreamUrl = "http://localhost:1985/rtc/v1/whip/?app=live&stream=livestream";
        QString webRTCsignalingUrl = "http://172.24.73.45:1985/rtc/v1/whip/?app=live&stream=livestream";
        QString webRTCstreamUrl = "http://172.24.73.45:1985/rtc/v1/whip/?app=live&stream=livestream";

        if (!QMetaObject::invokeMethod(m_webRTCPublisher, "init", Qt::QueuedConnection,
                                        Q_ARG(QString, webRTCsignalingUrl),
                                        Q_ARG(QString, webRTCstreamUrl))) {
            WRITE_LOG("Failed to invoke WebRTC publisher initialization.");
        } else {
            QMetaObject::invokeMethod(m_webRTCPublisher, "ChangeWebRtcPublishingState", Qt::QueuedConnection,
                                         Q_ARG(bool, m_isWebRtcPublishRequested));
        }
        m_isWebRtcPublishRequested = false;
    }
}


//// 加入房间按钮
void MainWindow::on_joinmeetBtn_clicked() {
    qDebug() << "on_joinmeetBtn_clicked";
    WRITE_LOG("Joining meeting");
    QString rtmpUrl = "rtmp://127.0.0.1:1935/live/teststream";
    //QString rtmpUrl = ui->meetnum->text();
    if (!rtmpUrl.isEmpty()) {
        QMetaObject::invokeMethod(m_rtmpPuller, "init", Qt::QueuedConnection,
                                  Q_ARG(QString, rtmpUrl));
    }
    connect(m_rtmpPuller, &RtmpPuller::initSuccess, this, &MainWindow::onRtmpPullerInitSuccess);
    //QMetaObject::invokeMethod(m_rtmpPuller, "startPulling", Qt::QueuedConnection);
}
void MainWindow::onRtmpPullerInitSuccess() {
    WRITE_LOG("RTMP puller initialized.");
    QMetaObject::invokeMethod(m_rtmpPuller, "startPulling", Qt::QueuedConnection);
}

//// 退出会议按钮
void MainWindow::on_exitmeetBtn_clicked() {
    //QMetaObject::invokeMethod(m_Capture, "closeDevice", Qt::QueuedConnection);
}

void MainWindow::onAudioDeviceOpened(AVCodecParameters* aParams, AVRational aTimeBase) {
    m_audioParams = aParams;
    m_audioTimeBase = aTimeBase;
    if (m_audioParams) {
        qDebug("Initializing audio pipeline");
        QMetaObject::invokeMethod(m_audioDecoder, "init", Qt::QueuedConnection,
            Q_ARG(AVCodecParameters*, m_audioParams),
            Q_ARG(AVRational, m_audioTimeBase));
        m_isAudioDecoderReady = true;
        QMetaObject::invokeMethod(m_audioDecoder, "ChangeDecodingState", Qt::QueuedConnection,
            Q_ARG(bool, m_isAudioDecoderReady));
    }
}

void MainWindow::onVideoDeviceOpened(AVCodecParameters *vParams, AVRational vTimeBase) {
    m_videoParams = vParams;
    m_videoTimeBase = vTimeBase;
    if (m_videoParams) {
        qDebug("Initializing video pipeline");
        QMetaObject::invokeMethod(m_videoDecoder, "init", Qt::QueuedConnection,
            Q_ARG(AVCodecParameters*, m_videoParams),
            Q_ARG(AVRational, m_audioTimeBase));
        m_isVideoDecoderReady = true;
        QMetaObject::invokeMethod(m_videoDecoder, "ChangeDecodingState", Qt::QueuedConnection,
            Q_ARG(bool, m_isVideoDecoderReady));
    }
}

void MainWindow::onNewLocalFrameAvailable() {
    // 这是在UI主线程中执行的
    std::unique_ptr<QImage> image;
    if (m_SmallQimageQueue->dequeue(image)) {
        if (image && !image->isNull()) {
            m_videoLocalWidget->updateFrame(image.get());
        }
    }
}

void MainWindow::onNewRemoteFrameAvailable() {
    WRITE_LOG("New remote frame available.");
    std::unique_ptr<QImage> image;
    if (m_MainQimageQueue->dequeue(image)) {
        if (image && !image->isNull()) {
            m_videoRemoteWidget->updateFrame(image.get());
        }
    }
}

void MainWindow::handleError(const QString &errorText) {
    WRITE_LOG(errorText.toLocal8Bit());

    // 创建并显示错误信息弹窗
    QMessageBox errorBox;
    errorBox.setIcon(QMessageBox::Critical);
    errorBox.setWindowTitle("错误");
    errorBox.setText(errorText);
    errorBox.setStandardButtons(QMessageBox::Ok);
    errorBox.setDefaultButton(QMessageBox::Ok);

    // 显示模态对话框
    errorBox.exec();
}

void MainWindow::handleDeviceOpened() {
    qDebug() << "Main thread: Received device opened signal.";
}
void MainWindow::on_PLIReceived_webrtcPublisher() {
    WRITE_LOG("WebRTC requested Keyframe (PLI). Forwarding to Encoder...");

    // 告诉视频编码器：立即生成一个 IDR 帧
    if (m_videoEncoder) {
        QMetaObject::invokeMethod(m_videoEncoder, "requestKeyFrame", Qt::QueuedConnection);
    }
}