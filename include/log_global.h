#ifndef LOG_GLOBAL_H
#define LOG_GLOBAL_H

#include <QByteArray>


class LogQueue;

struct Log
{
    QByteArray data;
};

extern LogQueue* log_instance;
//TODO:做多级错误日志处理
#define WRITE_LOG(LOGTEXT, ...) do { \
LogQueue::GetInstance().print(__FILE__, __FUNCTION__, __LINE__, LOGTEXT, ##__VA_ARGS__); \
} while(0)

#endif // LOG_GLOBAL_H
