#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <utility>

template<typename T>
class LockFreeStack
{
    struct Node
    {
        T data;
        std::atomic<Node*> next{nullptr};
        Node() : data() {}
        explicit Node(const T& d) : data(d) {}
        explicit Node(T&& d) : data(std::move(d)) {}
    };

    std::atomic<Node*> top_{nullptr};

public:
    LockFreeStack() = default;

    ~LockFreeStack()
    {
        Node* node = top_.load(std::memory_order_acquire);
        while (node)
        {
            Node* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }

    LockFreeStack(const LockFreeStack&) = delete;
    LockFreeStack& operator=(const LockFreeStack&) = delete;

    void push(const T& val)
    {
        Node* newNode = new Node(val);
        Node* oldTop = top_.load(std::memory_order_relaxed);
        do
        {
            newNode->next.store(oldTop, std::memory_order_relaxed);
        } while (!top_.compare_exchange_weak(oldTop, newNode,
            std::memory_order_release, std::memory_order_relaxed));
    }

    void push(T&& val)
    {
        Node* newNode = new Node(std::move(val));
        Node* oldTop = top_.load(std::memory_order_relaxed);
        do
        {
            newNode->next.store(oldTop, std::memory_order_relaxed);
        } while (!top_.compare_exchange_weak(oldTop, newNode,
            std::memory_order_release, std::memory_order_relaxed));
    }

    bool pop(T& val)
    {
        Node* oldTop = top_.load(std::memory_order_acquire);
        Node* newTop;
        do
        {
            if (!oldTop) return false;
            newTop = oldTop->next.load(std::memory_order_relaxed);
        } while (!top_.compare_exchange_weak(oldTop, newTop,
            std::memory_order_acquire, std::memory_order_relaxed));

        val = std::move(oldTop->data);
        delete oldTop;
        return true;
    }

    bool try_pop(T& val) { return pop(val); }

    bool empty() const
    {
        return top_.load(std::memory_order_acquire) == nullptr;
    }
};
