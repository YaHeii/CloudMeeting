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
    _openCamera = false;
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


    m_CaptureThread = new QThread(this);
    m_videocapture = new VideoCapture();
    m_videocapture->setPacketQueue(m_packetQueue);
    m_videocapture->moveToThread(m_CaptureThread);

    m_decoderThread = new QThread(this);
    m_ffmpegDecoder = new ffmpegDecoder(m_packetQueue, m_frameQueue);
    m_ffmpegDecoder->moveToThread(m_decoderThread);


    QStringList videoDevices = DeviceEnumerator::getDevices(MediaType::Video);
    ui->videoDeviceComboBox->addItems(videoDevices);

    QStringList audioDevices = DeviceEnumerator::getDevices(MediaType::Audio);
    ui->audioDevicecomboBox->addItems(audioDevices);

    connect(m_CaptureThread, &QThread::started, m_videocapture, &VideoCapture::startReading);

    //// 采集器打开发出信号携带解码参数
    connect(m_videocapture, &VideoCapture::deviceOpenSuccessfully, this, &MainWindow::onDeviceOpened);
    //// 解码器初始化成功，开始解码
    connect(m_decoderThread, &QThread::started, m_ffmpegDecoder, &ffmpegDecoder::startDecoding);
    //// 采集线程
    connect(m_CaptureThread, &QThread::started, m_videocapture, &VideoCapture::startReading);
    //// 每解出一帧通知主线程
    connect(m_ffmpegDecoder, &ffmpegDecoder::newFrameAvailable, this, &MainWindow::onNewFrameAvailable, Qt::QueuedConnection);

    connect(m_videocapture, &VideoCapture::errorOccurred, this, &MainWindow::handleError);


    // 线程结束后，自动清理工作对象和线程本身
    connect(m_CaptureThread, &QThread::finished, m_videocapture, &QObject::deleteLater);
    connect(m_CaptureThread, &QThread::finished, m_CaptureThread, &QObject::deleteLater);

    // 4. 启动线程
    m_CaptureThread->start();
    m_decoderThread->start();
}

void MainWindow::on_openVideoButton_clicked() {
    QString videoDevice = "..."; // 从UI获取
    QString audioDevice = "...";

    // 清空队列，以防上次运行时有残留数据
    m_packetQueue.clear();
    m_frameQueue.clear();

    QMetaObject::invokeMethod(m_videocapture, "openDevice", Qt::QueuedConnection,
                              Q_ARG(QString, videoDevice), Q_ARG(QString, audioDevice));
    // QString videoDevice = ui->videoDeviceComboBox->currentText();
    // QString audioDevice = ui->audioDevicecomboBox->currentText();
    //
    // QMetaObject::invokeMethod(m_videocapture, "openDevice", Qt::QueuedConnection,
    //                           Q_ARG(QString, videoDevice), Q_ARG(QString, audioDevice));
}

void MainWindow::on_exitmeetBtn_clicked(){

    QMetaObject::invokeMethod(m_CaptureThread, "closeDevice", Qt::QueuedConnection);
}
void MainWindow::onNewFrameAvailable()
{
    // 这是在UI主线程中执行的
    std::unique_ptr<QImage> image;
    if (m_frameQueue.dequeue(image)) {
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
void MainWindow::onDeviceOpened(AVCodecParameters* params)
{
    WRITE_LOG("Main thread: Device opened. Initializing decoder...");
    // 在解码线程中初始化解码器
    QMetaObject::invokeMethod(m_ffmpegDecoder, "init", Qt::QueuedConnection, Q_ARG(AVCodecParameters*, params));
}
MainWindow::~MainWindow()
{
    if (m_CaptureThread && m_CaptureThread->isRunning()) {
        m_videocapture->stopReading();
        m_CaptureThread->quit();
        m_CaptureThread->wait(); // 等待线程完全退出
    }
    if(m_decoderThread->isRunning()) {
        m_ffmpegDecoder->stopDecoding();
        m_decoderThread->quit();
        m_decoderThread->wait();
    }
    delete ui;
}
