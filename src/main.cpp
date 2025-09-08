#include "mainwindow.h"
#include <QDebug>
#include <QApplication>
// C语言的头文件，需要用 extern "C" 包裹
extern "C" {
#include <libavcodec/avcodec.h>
}
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    MainWindow w;
    w.show();

    //...
    qDebug() << "FFmpeg libavcodec version: " << avcodec_configuration();
    return a.exec();
}
