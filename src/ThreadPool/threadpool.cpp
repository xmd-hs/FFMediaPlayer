#include <iostream>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <thread>
#include <chrono>
#include "threadpool.h"

std::atomic_int ThreadPool::Thread::generateId_{0};

ThreadPool::ThreadPool()
    : initThreadSize_(0)
    , taskSize_(0)
    , idleThreadSize_(0)
    , curThreadSize_(0)
    , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
    , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
    , poolMode_(PoolMode::MODE_FIXED)
    , isPoolRunning_(false)
{}

ThreadPool::~ThreadPool()
{
    stop();
}

void ThreadPool::stop()
{
    if (!isPoolRunning_)
        return;

    isPoolRunning_ = false;

    std::unique_lock<std::mutex> lock(taskQueMtx_);
    notEmpty_.notify_all();
    exitCond_.wait(lock, [&]()->bool { return threads_.size() == 0; });
}

void ThreadPool::setMode(PoolMode mode)
{
    if (checkRunningState()) return;
    poolMode_ = mode;
}

void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
    if (checkRunningState()) return;
    taskQueMaxThreshHold_ = threshhold;
}

void ThreadPool::setThreadSizeThreshHold(int threshhold)
{
    if (checkRunningState()) return;
    if (poolMode_ == PoolMode::MODE_CACHED)
        threadSizeThreshHold_ = threshhold;
}

bool ThreadPool::submitTaskHelper(Task task)
{
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    if (!notFull_.wait_for(lock, std::chrono::seconds(1),
        [&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
    {
        std::cerr << "task queue is full, submit task fail." << std::endl;
        return false;
    }

    taskQue_.emplace(std::move(task));
    taskSize_++;

    notEmpty_.notify_one();

    if (poolMode_ == PoolMode::MODE_CACHED
        && taskSize_ > idleThreadSize_
        && curThreadSize_ < threadSizeThreshHold_)
    {
        auto ptr = std::make_unique<Thread>(
            std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        threads_[threadId]->start();
        curThreadSize_++;
        idleThreadSize_++;
    }

    return true;
}

void ThreadPool::start(int initThreadSize)
{
    isPoolRunning_ = true;

    initThreadSize_ = initThreadSize;
    curThreadSize_ = initThreadSize;

    for (int i = 0; i < initThreadSize_; i++)
    {
        auto ptr = std::make_unique<Thread>(
            std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        threads_[threadId]->start();
        idleThreadSize_++;
    }
}

void ThreadPool::threadFunc(int threadid)
{
    auto lastTime = std::chrono::high_resolution_clock().now();

    for (;;)
    {
        Task task;
        bool needExit = false;
        {
            std::unique_lock<std::mutex> lock(taskQueMtx_);

            while (taskQue_.size() == 0)
            {
                if (!isPoolRunning_)
                {
                    needExit = true;
                    break;
                }

                if (poolMode_ == PoolMode::MODE_CACHED)
                {
                    if (std::cv_status::timeout ==
                        notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto now = std::chrono::high_resolution_clock::now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                        if (dur.count() >= THREAD_MAX_IDLE_TIME
                            && curThreadSize_ > initThreadSize_)
                        {
                            needExit = true;
                            break;
                        }
                    }
                }
                else
                {
                    notEmpty_.wait(lock);
                }
            }

            if (needExit)
            {
                threads_.erase(threadid);
                curThreadSize_--;
                idleThreadSize_--;
                exitCond_.notify_all();
                return;
            }

            idleThreadSize_--;

            task = taskQue_.front();
            taskQue_.pop();
            taskSize_--;

            if (taskQue_.size() > 0)
                notEmpty_.notify_all();

            notFull_.notify_all();
        }

        if (task)
            task();

        idleThreadSize_++;
        lastTime = std::chrono::high_resolution_clock::now();
    }
}

bool ThreadPool::checkRunningState() const
{
    return isPoolRunning_;
}
