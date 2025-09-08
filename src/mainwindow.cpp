#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "logqueue.h"
#include "log_global.h"
#include "netheader.h"
#include "screen.h"

QRect MainWindow::pos = QRect(-1,-1,-1,-1);
// extern LogQueue *logqueue;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    LogQueue::GetInstance().start();
\
    qDebug() << "main: " <<QThread::currentThread();
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

    ui->exitmeetBtn->setDisabled(true);
    ui->joinmeetBtn->setDisabled(true);
    ui->createmeetBtn->setDisabled(true);
    ui->openAudio->setDisabled(true);
    ui->openVideo->setDisabled(true);
    ui->sendmsg->setDisabled(true);
    mainip = 0; //主屏幕显示的用户IP图像


}

MainWindow::~MainWindow()
{
    delete ui;
}
