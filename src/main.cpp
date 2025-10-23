#include "mainwindow.h"
#include <QDebug>
#include <QApplication>
#include "logqueue.h"

// C语言的头文件，需要用 extern "C" 包裹
extern "C" {
#include <libavcodec/avcodec.h>
}

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    MainWindow w;
    w.show();

    // 程序运行
    int result = a.exec();

    // 程序退出前，确保日志线程正确停止
    LogQueue::GetInstance().stopImmediately();
    LogQueue::GetInstance().wait(); // 等待日志线程结束

    return result;
}
