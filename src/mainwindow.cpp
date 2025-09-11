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

    _createmeet = false;
    // _openCamera = false;
    _joinmeet = false;
    Screen::init();
    MainWindow::pos = QRect(0.1 * Screen::width, 0.1 * Screen::height, 0.8 * Screen::width, 0.8 * Screen::height);

    ui->setupUi(this);

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

    //采集线程
    m_CaptureThread = new QThread(this);
    m_capture = new VideoCapture();
    m_capture->setPacketQueue(m_videoPacketQueue, m_audioPacketQueue);
    m_capture->moveToThread(m_CaptureThread);
    m_CaptureThread->start();
    //音频线程
    m_audioDecoderThread = new QThread(this);
    m_audioDecoder = new ffmpegAudioDecoder(m_audioPacketQueue);
    m_audioDecoder->moveToThread(m_audioDecoderThread);
    m_audioDecoderThread->start();
    //视频线程
    m_videoDecoderThread = new QThread(this);
    m_videoDecoder = new ffmpegDecoder(m_videoPacketQueue, m_videoFrameQueue);
    m_videoDecoder->moveToThread(m_videoDecoderThread);
    m_videoDecoderThread->start();

    //获取可用设备
    QStringList videoDevices = DeviceEnumerator::getDevices(MediaType::Video);
    ui->videoDeviceComboBox->addItems(videoDevices);
    QStringList audioDevices = DeviceEnumerator::getDevices(MediaType::Audio);
    ui->audioDevicecomboBox->addItems(audioDevices);

    // connect(m_CaptureThread, &QThread::started, m_capture, &VideoCapture::startReading);

    //// 视频
    //// 采集器打开发出信号携带解码参数
    connect(m_capture, &VideoCapture::deviceOpenSuccessfully, this, &MainWindow::onDeviceOpened);
    //// 解码器初始化成功，开始解码
    connect(m_videoDecoderThread, &QThread::started, m_videoDecoder, &ffmpegDecoder::startDecoding);
    //// 采集线程
    // connect(m_CaptureThread, &QThread::started, m_capture, &VideoCapture::startReading);
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
    if (m_CaptureThread && m_CaptureThread->isRunning()) {
        QMetaObject::invokeMethod(m_capture, "stopReading", Qt::QueuedConnection);
        m_CaptureThread->quit();
        m_CaptureThread->wait(); // 等待线程完全退出
    }
    if(m_audioDecoderThread->isRunning()) {
        QMetaObject::invokeMethod(m_videoDecoder, "stopDecoding", Qt::QueuedConnection);
        m_audioDecoderThread->quit();
        m_audioDecoderThread->wait();
    }
    if(m_videoDecoderThread->isRunning()) {
        QMetaObject::invokeMethod(m_audioDecoder, "stopDecoding", Qt::QueuedConnection);
        m_videoDecoderThread->quit();
        m_videoDecoderThread->wait();
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
    }
    if (aParams) {
        WRITE_LOG("Initializing audio decoder");
        // 在音频解码线程中初始化解码器
        QMetaObject::invokeMethod(m_audioDecoder, "init", Qt::QueuedConnection, Q_ARG(AVCodecParameters*, aParams));
        // 音频解码器初始化后，也可以开始工作
        QMetaObject::invokeMethod(m_audioDecoder, "startDecoding", Qt::QueuedConnection);
    }
    // 在解码线程中初始化解码器
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
