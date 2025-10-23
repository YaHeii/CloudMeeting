#include <QDebug>
#include "logqueue.h"
#include <QDateTime>
#include <QFile>
#include <QTextStream>

LogQueue::LogQueue(QObject *parent) : QThread(parent), m_isCanRun(false), logfile(nullptr) {
}

void LogQueue::run() {
    m_isCanRun = true;

    // 使用Qt文件API替代标准C文件API，提高跨平台兼容性
    QFile file("./log.txt");
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "打开日志文件失败:" << file.errorString();
        m_isCanRun = false;
        return;
    }

    QTextStream out(&file);

    qDebug() << "日志线程已启动";

    while (true) {
        // 检查是否应该停止线程
        {
            QMutexLocker lock(&m_lock);
            if (!m_isCanRun) {
                break;
            }
        }

        Log log;
        if (!m_logQueue.dequeue(log)) {
            // 队列超时，继续循环检查是否需要停止线程
            continue;
        }

        if (!log.data.isEmpty()) {
            // 写入日志到文件
            out << log.data;
            out.flush(); // 立即刷新到磁盘

            if (file.error() != QFile::NoError) {
                qDebug() << "写入日志文件时出错:" << file.errorString();
            }
        }
    }

    file.close();
    qDebug() << "日志线程已结束";
}

void LogQueue::stopImmediately() {
    QMutexLocker lock(&m_lock);
    if (!m_isCanRun) {
        return;
    }
    m_isCanRun = false;

    // 唤醒可能正在等待的 run() 线程
    m_logQueue.clear(); // clear() 内部会调用 wakeAll()
}

void LogQueue::print(const char *file, const char *func, int line, const char *fmt, ...) {
    const char *fileName = strrchr(file, '/'); // 按 Unix 路径分隔符查找
    if (!fileName) fileName = strrchr(file, '\\'); // 按 Windows 路径分隔符查找
    fileName = fileName ? (fileName + 1) : file; // 无分隔符时直接用完整路径
    // 格式化时间戳和位置信息
    QString prefix = QString("[%1] <%2:%3:%4> ")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"))
            .arg(fileName)
            .arg(func)
            .arg(line);

    // 处理可变参数
    va_list ap;
    va_start(ap, fmt);
    QString log_content = QString::vasprintf(fmt, ap);
    va_end(ap);

    // 组合完整日志消息
    QByteArray log_data = (prefix + log_content + "\n").toUtf8();

    // 创建日志对象并加入队列
    Log log;
    log.data = std::move(log_data);
    m_logQueue.enqueue(log);
}
