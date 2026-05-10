#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <future>

enum class PoolMode
{
    MODE_FIXED,   // 固定线程数
    MODE_CACHED   // 动态线程数
};

class ThreadPool
{
public:
    ThreadPool();
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void setMode(PoolMode mode);
    void setTaskQueMaxThreshHold(int threshhold);
    void setThreadSizeThreshHold(int threshhold);

    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>;

    void start(int initThreadSize = std::thread::hardware_concurrency());
    void stop();
    bool checkRunningState() const;

private:
    using Task = std::function<void()>;
    using ThreadFunc = std::function<void(int)>;

    void threadFunc(int threadid);
    bool submitTaskHelper(Task task);

    struct Thread
    {
        ThreadFunc func_;
        static std::atomic_int generateId_;
        int threadId_;

        Thread(ThreadFunc func)
            : func_(func)
            , threadId_(generateId_++)
        {}

        void start()
        {
            std::thread t(func_, threadId_);
            t.detach();
        }

        int getId() const { return threadId_; }
    };

    std::unordered_map<int, std::unique_ptr<Thread>> threads_;

    int initThreadSize_;
    int threadSizeThreshHold_;
    std::atomic_int curThreadSize_;
    std::atomic_int idleThreadSize_;

    std::queue<Task> taskQue_;
    std::atomic_int taskSize_;
    int taskQueMaxThreshHold_;

    std::mutex taskQueMtx_;
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
    std::condition_variable exitCond_;

    PoolMode poolMode_;
    std::atomic_bool isPoolRunning_;

    static const int TASK_MAX_THRESHHOLD = 1024;
    static const int THREAD_MAX_THRESHHOLD = 1024;
    static const int THREAD_MAX_IDLE_TIME = 60; // Cached模式空闲线程超时秒数
};

template<typename Func, typename... Args>
auto ThreadPool::submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
{
    using RType = decltype(func(args...));
    auto task = std::make_shared<std::packaged_task<RType()>>(
        std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
    std::future<RType> result = task->get_future();

    bool success = submitTaskHelper([task]() { (*task)(); });
    if (!success)
    {
        auto fallback = std::make_shared<std::packaged_task<RType()>>(
            []()->RType { return RType(); });
        (*fallback)();
        return fallback->get_future();
    }

    return result;
}

#endif // THREADPOOL_H
