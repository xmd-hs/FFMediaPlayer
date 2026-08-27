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

    explicit PacketQueue(std::size_t capacity = 128, std::size_t maxBytes = 0)
        : capacity_(capacity), maxBytes_(maxBytes)
    {
    }

    // abortFlag: when set, a blocked push wakes and returns false (caller keeps/frees value).
    bool push(T value, const std::atomic_bool* abortFlag = nullptr,
              std::size_t valueBytes = 1)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [&] {
            const bool hasByteCapacity = maxBytes_ == 0 || queue_.empty() ||
                bufferedBytes_ + valueBytes <= maxBytes_;
            return stopped_ || (queue_.size() < capacity_ && hasByteCapacity) ||
                (abortFlag && abortFlag->load(std::memory_order_acquire));
        });
        if (stopped_) return false;
        if (abortFlag && abortFlag->load(std::memory_order_acquire)) return false;
        queue_.push_back(Node{std::move(value), valueBytes});
        bufferedBytes_ += valueBytes;
        notEmpty_.notify_one();
        return true;
    }

    // Non-blocking push. Returns false if stopped or full (value not taken).
    bool tryPush(T value, std::size_t valueBytes = 1)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool hasByteCapacity = maxBytes_ == 0 || queue_.empty() ||
            bufferedBytes_ + valueBytes <= maxBytes_;
        if (stopped_ || queue_.size() >= capacity_ || !hasByteCapacity) return false;
        queue_.push_back(Node{std::move(value), valueBytes});
        bufferedBytes_ += valueBytes;
        notEmpty_.notify_one();
        return true;
    }

    bool pop(T& value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [&] { return stopped_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        Node& front = queue_.front();
        value = std::move(front.value);
        bufferedBytes_ -= front.bytes;
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
    struct Node {
        T value;
        std::size_t bytes = 0;
    };

    void freeAll(Freer freer)
    {
        if (freer) {
            for (auto& item : queue_) freer(item.value);
        }
        queue_.clear();
        bufferedBytes_ = 0;
    }

    const std::size_t capacity_;
    const std::size_t maxBytes_;
    std::size_t bufferedBytes_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<Node> queue_;
    bool stopped_ = false;
};

} // namespace ffplayer
