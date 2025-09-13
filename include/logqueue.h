#ifndef LOGQUEUE_H
#define LOGQUEUE_H
#include <QMutex>
#include <QThread>
#include <QDateTime>
#include <QString>
#include <QDebug>
// #include <memory>
// #include <queue>
// #include "netheader.h"
#include "ThreadSafeQueue.h"
#include "log_global.h"

//TODO:LogQueue不直接继承QTread，只负责管理和创建一个QTread实例和LogWorker
class LogQueue : public QThread
{
public:
    static LogQueue& GetInstance() {
        static LogQueue instance; //懒汉式
        return instance;
    }
    LogQueue(const LogQueue&) = delete;
    LogQueue& operator=(const LogQueue&) = delete;

    void stopImmediately();
    void print(const char* file, const char* func, int line, const char* fmt, ...);

private:
    explicit LogQueue(QObject *parent = nullptr);
    void run();
    QMutex m_lock;
    volatile bool m_isCanRun;
    QUEUE_DATA<Log> m_logQueue;
    FILE *logfile;
};
#endif // LOGQUEUE_H
