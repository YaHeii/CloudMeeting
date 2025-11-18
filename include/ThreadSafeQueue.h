#ifndef THREADSAFEQUEUE_H
#define THREADSAFEQUEUE_H
#include <QMutex>
#include <QQueue>
#include <QWaitCondition>
#include <memory>
#include <queue>

const int QUEUE_MAXSIZE = 1500;
const int WAIT_MILLISECONDS = 2000;

template<typename T>
class QUEUE_DATA {
public:
    QUEUE_DATA() = default;

    QUEUE_DATA(const QUEUE_DATA &) = delete;

    QUEUE_DATA &operator=(const QUEUE_DATA &) = delete;

    /**
     * @brief 向队列尾部添加一个元素（生产者）
     * @param item 元素的智能指针，所有权将被转移到队列中
     */
    void enqueue(T item) //入队无引用
    {
        QMutexLocker locker(&m_mutex);

        while (m_queue.size() >= QUEUE_MAXSIZE) {
            m_notFullCond.wait(&m_mutex);
        }

        m_queue.push(std::move(item));     
        m_notEmptyCond.wakeOne();
    }

    /**
     * @brief 从队列头部取出一个元素（消费者）
     * @param result 用于接收元素的智能指针引用
     * @return 如果成功取出元素则返回 true，超时则返回 false
     */
    bool dequeue(T &result) //出队有引用
    {
        QMutexLocker locker(&m_mutex);

        while (m_queue.empty()) {
            if (!m_notEmptyCond.wait(&m_mutex, WAIT_MILLISECONDS)) {
                return false;
            }
        }

        result = std::move(m_queue.front());
        m_queue.pop();

        //唤醒一个可能正在等待的生产者
        m_notFullCond.wakeOne();
        return true;
    }


    void clear() {
        QMutexLocker locker(&m_mutex);
        std::queue<T> empty_queue;
        m_queue.swap(empty_queue);

        // 唤醒所有可能在等待队列变满的生产者线程
        m_notFullCond.wakeAll();
        m_notEmptyCond.wakeAll();
    }

private:
    mutable QMutex m_mutex;
    QWaitCondition m_notEmptyCond; // 条件：队列不为空
    QWaitCondition m_notFullCond; // 条件：队列不满
    std::queue<T> m_queue;
};


#endif // THREADSAFEQUEUE_H
