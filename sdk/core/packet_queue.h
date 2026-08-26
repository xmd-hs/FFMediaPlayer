#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace ffplayer {

template <typename T>
class PacketQueue {
public:
    explicit PacketQueue(std::size_t capacity = 128) : capacity_(capacity) {}

    bool push(T value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [&] { return stopped_ || queue_.size() < capacity_; });
        if (stopped_) return false;
        queue_.push_back(std::move(value));
        notEmpty_.notify_one();
        return true;
    }

    bool pop(T& value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        value = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();
        return true;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        notFull_.notify_all();
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        queue_.clear();
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void restart()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = false;
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const { return size() == 0; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T> queue_;
    bool stopped_ = false;
};

} // namespace ffplayer
