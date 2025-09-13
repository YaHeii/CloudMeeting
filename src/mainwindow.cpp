#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "logqueue.h"
#include "log_global.h"
#include "netheader.h"
#include "screen.h"
#include "DeviceEnumerator.h"

QRect MainWindow::pos = QRect(-1,-1,-1,-1);
// extern LogQueue *logqueue;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    LogQueue::GetInstance().start();

    WRITE_LOG("main:",QThread::currentThread());
    qRegisterMetaType<MSG_TYPE>();

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
    m_videoSendPacketQueue = new QUEUE_DATA<AVPacketPtr>();
    m_audioPacketQueue = new QUEUE_DATA<AVPacketPtr>();
    m_audioFrameQueue = new QUEUE_DATA<AVFramePtr>();
    m_audioSendPacketQueue = new QUEUE_DATA<AVPacketPtr>();

    //采集线程
    m_CaptureThread = new QThread(this);
    m_capture = new VideoCapture();
    m_capture->setPacketQueue(m_videoPacketQueue, m_audioPacketQueue);
    m_capture->moveToThread(m_CaptureThread);
    m_CaptureThread->start();
    //音频解码线程
    m_audioDecoderThread = new QThread(this);
    m_audioDecoder = new ffmpegAudioDecoder(m_audioPacketQueue, m_audioFrameQueue);
    m_audioDecoder->moveToThread(m_audioDecoderThread);
    m_audioDecoderThread->start();
    //视频解码线程
    m_videoDecoderThread = new QThread(this);
    m_videoDecoder = new ffmpegDecoder(m_videoPacketQueue, m_QimageQueue, m_videoFrameQueue);
    m_videoDecoder->moveToThread(m_videoDecoderThread);
    m_videoDecoderThread->start();

    // 音频编码线程
    m_audioEncoderThread = new QThread(this);
    m_audioEncoder = new ffmpegEncoder(m_audioFrameQueue, m_audioSendPacketQueue);
    m_audioEncoder->moveToThread(m_audioEncoderThread);
    m_audioEncoderThread->start();
    // 视频编码线程
    m_videoEncoderThread = new QThread(this);
    m_videoEncoder = new ffmpegEncoder(m_videoFrameQueue, m_videoSendPacketQueue);
    m_videoEncoder->moveToThread(m_videoEncoderThread);
    m_videoEncoderThread->start();

    //获取可用设备
    QStringList videoDevices = DeviceEnumerator::getDevices(MediaType::Video);
    ui->videoDeviceComboBox->addItems(videoDevices);
    QStringList audioDevices = DeviceEnumerator::getDevices(MediaType::Audio);
    ui->audioDevicecomboBox->addItems(audioDevices);

    //// 采集器打开，发出信号携带解码参数
    connect(m_capture, &VideoCapture::deviceOpenSuccessfully, this, &MainWindow::onDeviceOpened);

    //// 视频
    //// 解码器初始化成功，开始解码
    connect(m_videoDecoderThread, &QThread::started, m_videoDecoder, &ffmpegDecoder::startDecoding);
    //// 每解出一帧通知主线程
    connect(m_videoDecoder, &ffmpegDecoder::newFrameAvailable, this, &MainWindow::onNewFrameAvailable, Qt::QueuedConnection);

    connect(m_capture, &VideoCapture::errorOccurred, this, &MainWindow::handleError);

    // 音频
    // 连接音频链的信号槽
    connect(m_audioDecoderThread, &QThread::started, m_audioDecoder, &ffmpegAudioDecoder::startDecoding);
    // connect(m_audioDecoder, &ffmpegAudioDecoder::errorOccurred, this, &MainWindow::onAudioDecodeError);
    // connect(m_capture, &VideoCapture::errorOccurred, this, &MainWindow::onCaptureError);

    // 线程结束后，自动清理工作对象和线程本身
    connect(m_CaptureThread, &QThread::finished, m_capture, &QObject::deleteLater);
    connect(m_videoDecoderThread, &QThread::finished, m_videoDecoder, &QObject::deleteLater);
    connect(m_audioDecoderThread, &QThread::finished, m_audioDecoder, &QObject::deleteLater);
}

MainWindow::~MainWindow()
{
    // 停止视频捕获
    if (m_CaptureThread && m_CaptureThread->isRunning()) {
        // 先停止读取
        QMetaObject::invokeMethod(m_capture, "stopReading", Qt::BlockingQueuedConnection);
        // 再关闭设备
        QMetaObject::invokeMethod(m_capture, "closeDevice", Qt::BlockingQueuedConnection);
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
////TODO:音视频通道开关解耦
void MainWindow::on_openVideo_clicked() {

    if (m_isVideoRunning) {

        QMetaObject::invokeMethod(m_capture, "closeDevice", Qt::BlockingQueuedConnection);

        // 2. 清空队列
        m_videoPacketQueue->clear();
        m_videoFrameQueue->clear();

        // 3. 更新UI和状态
        ui->openVideo->setText("开启视频");
        m_isVideoRunning = false;
        qDebug() << "Video stopped.";

    } else {

        QString videoDevice = ui->videoDeviceComboBox->currentText();
        if (videoDevice.isEmpty()) {
            qWarning() << "No video device selected!";
            return;
        }
        QString audioDevice = ui->audioDevicecomboBox->currentText();
        QMetaObject::invokeMethod(m_capture, "openDevice", Qt::QueuedConnection,
                                  Q_ARG(QString, videoDevice),
                                  Q_ARG(QString, audioDevice));

        // 3. 更新UI和状态
        ui->openVideo->setText("关闭视频");
        m_isVideoRunning = true;
        qDebug() << "Starting video...";
    }
}

// void on_openAudioButton_clicked() {
//
// }
void MainWindow::onDeviceOpened(AVCodecParameters* vParams, AVCodecParameters* aParams)
{
    WRITE_LOG("Main thread: Device opened. Initializing decoder...");
    if (vParams) {
        WRITE_LOG("Initializing video decoder");
        QMetaObject::invokeMethod(m_videoDecoder, "init", Qt::QueuedConnection, Q_ARG(AVCodecParameters*, vParams));
        QMetaObject::invokeMethod(m_videoDecoder, "startDecoding", Qt::QueuedConnection);
    }
    if (aParams) {
        WRITE_LOG("Initializing audio decoder");
        QMetaObject::invokeMethod(m_audioDecoder, "init", Qt::QueuedConnection, Q_ARG(AVCodecParameters*, aParams));
        QMetaObject::invokeMethod(m_audioDecoder, "startDecoding", Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(m_capture, "startReading", Qt::QueuedConnection);
}

void MainWindow::on_exitmeetBtn_clicked(){

    QMetaObject::invokeMethod(m_CaptureThread, "closeDevice", Qt::QueuedConnection);
}
void MainWindow::onNewFrameAvailable()
{
    // 这是在UI主线程中执行的
    std::unique_ptr<QImage> image;
    if (m_videoFrameQueue->dequeue(image)) {
        if (image&&!image->isNull()) {
            m_videoWidget->updateFrame(image.get());
        }
    }
}
void MainWindow::handleError(const QString &errorText) {
    WRITE_LOG(errorText.toLocal8Bit());
}
void MainWindow::handleDeviceOpened()
{
    qDebug() << "Main thread: Received device opened signal.";
    // 可以在这里更新UI，比如点亮一个状态灯
}
