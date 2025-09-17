#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "logqueue.h"
#include "log_global.h"
#include "netheader.h"
#include "screen.h"
#include "DeviceEnumerator.h"
#include <QMessageBox>
#include "AudioResampleConfig.h"

QRect MainWindow::pos = QRect(-1,-1,-1,-1);

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    qRegisterMetaType<MSG_TYPE>();
    qRegisterMetaType<AudioResampleConfig>();

    LogQueue::GetInstance().start();

    WRITE_LOG("-------------------------Application Start---------------------------");
    WRITE_LOG("main UI thread id: 0x%p", QThread::currentThreadId());
    ui->setupUi(this);
    _createmeet = false;
    // _openCamera = false;
    _joinmeet = false;
    Screen::init();
    MainWindow::pos = QRect(0.1 * Screen::width, 0.1 * Screen::height, 0.8 * Screen::width, 0.8 * Screen::height);

    m_videoWidget = ui->videoDisplayWidget;

    ui->openAudio->setText(QString(OPENAUDIO).toUtf8());
    ui->openVideo->setText(QString(OPENVIDEO).toUtf8());

    this->setGeometry(MainWindow::pos);
    this->setMinimumSize(QSize(MainWindow::pos.width() * 0.7, MainWindow::pos.height() * 0.7));
    this->setMaximumSize(QSize(MainWindow::pos.width(), MainWindow::pos.height()));

    ui->exitmeetBtn->setDisabled(false);
    ui->joinmeetBtn->setDisabled(false);
    ui->createmeetBtn->setDisabled(false);
    ui->openAudio->setDisabled(false);
    ui->openVideo->setDisabled(false);
    ui->sendmsg->setDisabled(false);
    mainip = 0; //主屏幕显示的用户IP图像

    // 初始化队列
    m_videoPacketQueue = new QUEUE_DATA<AVPacketPtr>();
    m_QimageQueue = new QUEUE_DATA<std::unique_ptr<QImage>>();
    m_videoFrameQueue = new QUEUE_DATA<AVFramePtr>();
    m_audioPacketQueue = new QUEUE_DATA<AVPacketPtr>();
    m_audioFrameQueue = new QUEUE_DATA<AVFramePtr>();
    m_publishPacketQueue = new QUEUE_DATA<AVPacketPtr>();


    // 采集线程
    m_CaptureThread = new QThread(this);
    m_Capture = new Capture();
    m_Capture->setPacketQueue(m_videoPacketQueue, m_audioPacketQueue);
    m_Capture->moveToThread(m_CaptureThread);
    m_CaptureThread->start();

    //音频解码线程
    m_audioDecoderThread = new QThread(this);
    m_audioDecoder = new ffmpegAudioDecoder(m_audioPacketQueue, m_audioFrameQueue);
    m_audioDecoder->moveToThread(m_audioDecoderThread);
    m_audioDecoderThread->start();
    //视频解码线程
    m_videoDecoderThread = new QThread(this);
    m_videoDecoder = new ffmpegVideoDecoder(m_videoPacketQueue, m_QimageQueue, m_videoFrameQueue);
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

    //获取可用设备
    QStringList videoDevices = DeviceEnumerator::getDevices(MediaType::Video);
    ui->videoDeviceComboBox->addItems(videoDevices);
    QStringList audioDevices = DeviceEnumerator::getDevices(MediaType::Audio);
    ui->audioDevicecomboBox->addItems(audioDevices);

    //// 采集器打开，发出信号携带解码参数
    connect(m_Capture, &Capture::deviceOpenSuccessfully, this, &MainWindow::onDeviceOpened);

    //// 视频
    connect(m_videoDecoder, &ffmpegVideoDecoder::newFrameAvailable, this, &MainWindow::onNewFrameAvailable, Qt::QueuedConnection);

    //// 音频
    connect(m_audioEncoder, &ffmpegEncoder::audioEncoderReady,m_audioDecoder, &ffmpegAudioDecoder::setResampleConfig,Qt::QueuedConnection);

    //errorOccurred处理
    connect(m_videoDecoder, &ffmpegVideoDecoder::errorOccurred, this, &MainWindow::handleError);
    connect(m_audioDecoder, &ffmpegAudioDecoder::errorOccurred, this, &MainWindow::handleError);
    connect(m_audioEncoder, &ffmpegEncoder::errorOccurred, this, &MainWindow::handleError);
    connect(m_videoEncoder, &ffmpegEncoder::errorOccurred, this, &MainWindow::handleError);
    connect(m_Capture, &Capture::errorOccurred, this, &MainWindow::handleError);

    // 线程结束后，自动清理工作对象和线程本身
    connect(m_CaptureThread, &QThread::finished, m_Capture, &QObject::deleteLater);
    connect(m_videoDecoderThread, &QThread::finished, m_videoDecoder, &QObject::deleteLater);
    connect(m_audioDecoderThread, &QThread::finished, m_audioDecoder, &QObject::deleteLater);
    connect(m_audioEncoderThread, &QThread::finished, m_audioEncoder, &QObject::deleteLater);
    connect(m_videoEncoderThread, &QThread::finished, m_videoEncoder, &QObject::deleteLater);
}

MainWindow::~MainWindow()
{
    // 停止采集
    if (m_CaptureThread && m_CaptureThread->isRunning()) {

        QMetaObject::invokeMethod(m_Capture, "closeDevice", Qt::BlockingQueuedConnection);
        m_CaptureThread->quit();
        m_CaptureThread->wait();
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
        // --- 停止采集 ---
        QMetaObject::invokeMethod(m_Capture, "closeVideo", Qt::QueuedConnection);
        // --- 停止解码 ---
        QMetaObject::invokeMethod(m_videoDecoder, "stopDecoding", Qt::QueuedConnection);
        // --- 停止编码 ---
        QMetaObject::invokeMethod(m_videoEncoder, "stopEncoding", Qt::QueuedConnection);
        // --- 清空队列 ---
        m_videoPacketQueue->clear();
        m_videoFrameQueue->clear();
        m_QimageQueue->clear();


        // --- 更新UI和状态 ---
        ui->openVideo->setText("开启视频");
        m_isVideoRunning = false;
        // m_videoWidget->updateFrame(nullptr);
        qDebug() << "Video stopped.";
    } else {
        QString videoDevice = ui->videoDeviceComboBox->currentText();
        if (videoDevice.isEmpty()) {
            qWarning() << "No video device selected!";
            return;
        }
        QMetaObject::invokeMethod(m_Capture, "openVideo", Qt::QueuedConnection,Q_ARG(QString, videoDevice));

        // --- 更新UI和状态 ---
        ui->openVideo->setText("关闭视频");
        m_isVideoRunning = true;
        qDebug() << "Video started.";
    }
}
//// 打开麦克风按钮（开关复用）
void MainWindow::on_openAudio_clicked()
{
    if (m_isAudioRunning) {
        // --- 停止采集 ---
        QMetaObject::invokeMethod(m_Capture, "closeAudio", Qt::QueuedConnection);
        // --- 停止解码 ---
        QMetaObject::invokeMethod(m_audioDecoder, "stopDecoding", Qt::QueuedConnection);
        // --- 停止编码 ---
        QMetaObject::invokeMethod(m_audioEncoder, "stopEncoding", Qt::QueuedConnection);
        // --- 清空队列 ---
        m_audioPacketQueue->clear();
        m_audioFrameQueue->clear();

        // --- 更新UI和状态 ---
        ui->openAudio->setText("打开麦克风");
        m_isAudioRunning = false;
        qDebug() << "Audio stopped.";

    } else {
        QString videoDevice = ui->videoDeviceComboBox->currentText();
        if (videoDevice.isEmpty()) {
            qWarning() << "No video device selected!";
            return;
        }
        QString audioDevice = ui->audioDevicecomboBox->currentText();
        QMetaObject::invokeMethod(m_Capture, "openAudio", Qt::QueuedConnection,Q_ARG(QString, audioDevice));

        // --- 更新UI和状态 ---
        ui->openAudio->setText("关闭麦克风");
        m_isAudioRunning = true;
        qDebug() << "Starting Audio...";
    }
}


//// 创建会议按钮
void MainWindow::on_createmeetBtn_clicked(){
    connect(m_audioEncoder, &ffmpegEncoder::initializationSuccess, this, &MainWindow::audioEncoderReady);
    connect(m_videoEncoder, &ffmpegEncoder::initializationSuccess, this, &MainWindow::videoEncoderReady);
    if (m_isAudioRunning || m_isVideoRunning) {
        ui->createmeetBtn->setEnabled(true);
        QString rtmpUrl = "rtmp://127.0.0.1:1935/live/teststream";
        qDebug() << "Joining meeting...";

        AVCodecContext *videoCtx = m_videoEncoder->getCodecContext();
        AVCodecContext *audioCtx = m_audioEncoder->getCodecContext();

        if (!videoCtx || !audioCtx) {
            QMessageBox::warning(this, "Error", "Encoders are not ready yet.");
            return;
        }

        // 使用 invokeMethod 异步调用 init 和 startPublishing
        QMetaObject::invokeMethod(m_rtmpPublisher, "init", Qt::QueuedConnection,Q_ARG(QString, rtmpUrl),
                                  Q_ARG(AVCodecContext*, videoCtx),Q_ARG(AVCodecContext*, audioCtx));

        QMetaObject::invokeMethod(m_rtmpPublisher, "startPublishing", Qt::QueuedConnection);
    }else {
        ui->createmeetBtn->setEnabled(false);
    }
}

//// 退出会议按钮
void MainWindow::on_exitmeetBtn_clicked(){
    QMetaObject::invokeMethod(m_Capture, "closeDevice", Qt::QueuedConnection);
}


void MainWindow::onDeviceOpened(AVCodecParameters* vParams, AVCodecParameters* aParams)
{
    WRITE_LOG("Main thread: Device opened. Initializing...");
    if (vParams) {
        qDebug("Initializing video pipeline");
        QMetaObject::invokeMethod(m_videoDecoder, "init", Qt::QueuedConnection, Q_ARG(AVCodecParameters*, vParams));
        QMetaObject::invokeMethod(m_videoEncoder, "initVideoEncoder", Qt::QueuedConnection, Q_ARG(AVCodecParameters*, vParams));
        QMetaObject::invokeMethod(m_videoDecoder, "startDecoding", Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_videoEncoder, "startEncoding", Qt::QueuedConnection);
    }
    if (aParams) {
        // 只需要初始化，startDecoding/Encoding 会在配置完成后自动处理
        qDebug("Initializing audio pipeline");
        QMetaObject::invokeMethod(m_audioDecoder, "init", Qt::QueuedConnection, Q_ARG(AVCodecParameters*, aParams));
        QMetaObject::invokeMethod(m_audioEncoder, "initAudioEncoder", Qt::QueuedConnection, Q_ARG(AVCodecParameters*, aParams));
        QMetaObject::invokeMethod(m_audioDecoder, "startDecoding", Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_audioEncoder, "startEncoding", Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(m_Capture, "startReading", Qt::QueuedConnection);
}
void MainWindow::audioEncoderReady()
{
   m_audioEncoderReady = true;
}
void MainWindow::videoEncoderReady()
{
   m_videoEncoderReady = true;
}
void MainWindow::onNewFrameAvailable()
{
    // 这是在UI主线程中执行的
    std::unique_ptr<QImage> image;
    if (m_QimageQueue->dequeue(image)) {
        if (image&&!image->isNull()) {
            m_videoWidget->updateFrame(image.get());
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
void MainWindow::handleDeviceOpened()
{
    qDebug() << "Main thread: Received device opened signal.";
    // 可以在这里更新UI，比如点亮一个状态灯
}
