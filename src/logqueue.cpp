#include <QDebug>
#include "logqueue.h"


LogQueue::LogQueue(QObject *parent) : QThread(parent) {}

void LogQueue::run()
{
    m_isCanRun = true;
    FILE* logfile = nullptr;
    errno_t r = fopen_s(&logfile, "./log.txt", "a");
    if(r != 0 || logfile == nullptr)
    {
        qDebug() << "打开文件失败:" << r;
        m_isCanRun = false;
        return;
    }
    for(;;)
    {
        {
            QMutexLocker lock(&m_lock);
            if(m_isCanRun == false)
            {
                break;
            }
        }
        Log log;
        if(!m_logQueue.dequeue(log)) continue;


        if (!log.data.isEmpty()) {
            size_t to_write = log.data.size();
            size_t written = fwrite(log.data.constData(), 1, to_write, logfile);

            if (written != to_write) {
                qDebug() << "Log thread: Failed to write full log message to file.";
            }
        }
    }
    fflush(logfile);
    fclose(logfile);
    logfile = nullptr;
    qDebug() << "Log thread finished.";
}

void LogQueue::stopImmediately()
{
    QMutexLocker lock(&m_lock);
    if (!m_isCanRun) {
        return;
    }
    m_isCanRun = false;

    //唤醒可能正在等待的 run() 线程
    m_logQueue.clear(); // clear() 内部会调用 wakeAll()
}

void LogQueue::print(const char* file, const char* func, int line, const char* fmt, ...)
{

    QString prefix = QString("[%1] <%2:%3:%4> ")
                         .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz"))
                         .arg(file)
                         .arg(func)
                         .arg(line);


    va_list ap;
    va_start(ap, fmt);
    QString log_content = QString::vasprintf(fmt, ap);
    va_end(ap);
    QByteArray log_data = (prefix + log_content + "\n").toUtf8();

    Log log;
    log.data = std::move(log_data);

    m_logQueue.enqueue(log);
}
