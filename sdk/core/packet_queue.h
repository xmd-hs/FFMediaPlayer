#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace ffplayer {

template <typename T>
class PacketQueue {
public:
    using Freer = void (*)(T);

    explicit PacketQueue(std::size_t capacity = 128) : capacity_(capacity) {}

    // abortFlag: when set, a blocked push wakes and returns false (caller keeps/frees value).
    bool push(T value, const std::atomic_bool* abortFlag = nullptr)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [&] {
            return stopped_ || queue_.size() < capacity_ ||
                (abortFlag && abortFlag->load(std::memory_order_acquire));
        });
        if (stopped_) return false;
        if (abortFlag && abortFlag->load(std::memory_order_acquire)) return false;
        queue_.push_back(std::move(value));
        notEmpty_.notify_one();
        return true;
    }

    // Non-blocking push. Returns false if stopped or full (value not taken).
    bool tryPush(T value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || queue_.size() >= capacity_) return false;
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

    void clear(Freer freer = nullptr)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        freeAll(freer);
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void stop(Freer freer = nullptr)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        freeAll(freer);
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    void restart()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = false;
    }

    // Wake blocked push/pop waiters (e.g. pause/seek) so they can re-check flags.
    void wakeWaiters()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    bool empty() const { return size() == 0; }

private:
    void freeAll(Freer freer)
    {
        if (freer) {
            for (auto& item : queue_) freer(item);
        }
        queue_.clear();
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<T> queue_;
    bool stopped_ = false;
};

} // namespace ffplayer
