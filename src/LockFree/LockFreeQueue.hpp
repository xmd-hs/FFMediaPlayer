#pragma once

#include <atomic>
#include <cstddef>
#include <utility>

template<typename T, size_t Capacity>
class LockFreeQueue
{
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

    struct Cell
    {
        std::atomic<size_t> sequence{0};
        T data{};
        Cell() = default;
    };

    Cell* buffer_;
    size_t mask_;
    alignas(64) std::atomic<size_t> enqueuePos_{0};
    alignas(64) std::atomic<size_t> dequeuePos_{0};

public:
    LockFreeQueue()
        : buffer_(new Cell[Capacity])
        , mask_(Capacity - 1)
    {
        for (size_t i = 0; i < Capacity; i++)
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }

    ~LockFreeQueue() { delete[] buffer_; }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    void reset()
    {
        enqueuePos_.store(0, std::memory_order_relaxed);
        dequeuePos_.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < Capacity; i++)
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }

    bool push(const T& val)
    {
        Cell* cell;
        size_t pos = enqueuePos_.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0)
            {
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
                return false;
            else
                pos = enqueuePos_.load(std::memory_order_relaxed);
        }
        cell->data = val;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& val)
    {
        Cell* cell;
        size_t pos = dequeuePos_.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0)
            {
                if (dequeuePos_.compare_exchange_weak(pos, pos + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
                return false;
            else
                pos = dequeuePos_.load(std::memory_order_relaxed);
        }
        val = std::move(cell->data);
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    bool try_push(const T& val) { return push(val); }
    bool try_pop(T& val) { return pop(val); }

    bool push(T&& val)
    {
        Cell* cell;
        size_t pos = enqueuePos_.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0)
            {
                if (enqueuePos_.compare_exchange_weak(pos, pos + 1,
                    std::memory_order_relaxed, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
                return false;
            else
                pos = enqueuePos_.load(std::memory_order_relaxed);
        }
        cell->data = std::move(val);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    size_t size() const
    {
        size_t enq = enqueuePos_.load(std::memory_order_acquire);
        size_t deq = dequeuePos_.load(std::memory_order_acquire);
        return enq >= deq ? enq - deq : 0;
    }

    bool empty() const { return size() == 0; }
    bool full() const { return size() >= Capacity; }
    size_t capacity() const { return Capacity; }
};
