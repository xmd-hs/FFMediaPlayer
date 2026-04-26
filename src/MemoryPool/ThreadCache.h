/**
 * @file ThreadCache.h
 * @brief 线程本地缓存（ThreadCache）
 *
 * 三级内存池架构的第一级，每个线程拥有独立的 ThreadCache 实例。
 * 采用 thread_local 单例模式，实现无锁的线程本地内存分配/释放。
 *
 * 分配流程：
 * 1. 检查线程本地自由链表是否有可用内存块
 * 2. 若无，批量从 CentralCache 获取内存块
 *
 * 释放流程：
 * 1. 将内存块归还到线程本地自由链表
 * 2. 若自由链表超过阈值，批量归还给 CentralCache
 *
 * @author FFMediaPlayer Team
 * @version 1.0
 */

#pragma once

#include "Common.h"

namespace Kama_memoryPool
{

/**
 * @brief 线程本地缓存类
 *
 * 每个线程通过 thread_local 拥有独立的 ThreadCache 实例，
 * 线程内内存分配/释放无需加锁，极大提升并发性能。
 * 当本地缓存不足时，批量从 CentralCache 获取；
 * 当本地缓存过多时，批量归还给 CentralCache。
 */
class ThreadCache
{
public:
    /**
     * @brief 获取当前线程的 ThreadCache 单例
     * @return 当前线程的 ThreadCache 实例指针
     */
    static ThreadCache* getInstance()
    {
        static thread_local ThreadCache instance;
        return &instance;
    }

    /**
     * @brief 析构函数，将所有缓存的内存归还给 CentralCache
     */
    ~ThreadCache();

    /**
     * @brief 从线程本地缓存分配内存
     * @param size 请求分配的字节数
     * @return 分配的内存地址，失败返回 nullptr
     *
     * 若请求大小超过 MAX_BYTES，直接调用系统 malloc。
     */
    void* allocate(size_t size);

    /**
     * @brief 将内存归还到线程本地缓存
     * @param ptr 待释放的内存地址
     * @param size 内存块大小
     *
     * 若请求大小超过 MAX_BYTES，直接调用系统 free。
     * 当自由链表超过阈值时，自动批量归还给 CentralCache。
     */
    void deallocate(void* ptr, size_t size);

private:
    ThreadCache()
    {
        freeList_.fill(nullptr);
        freeListSize_.fill(0);
    }

    /**
     * @brief 从中心缓存批量获取内存块
     * @param index 自由链表索引
     * @return 获取的内存块地址
     */
    void* fetchFromCentralCache(size_t index);

    /**
     * @brief 将内存块批量归还给中心缓存
     * @param start 待归还链表起始地址
     * @param size 内存块大小
     */
    void returnToCentralCache(void* start, size_t size);

    /**
     * @brief 计算批量获取内存块的数量
     * @param size 单个内存块大小
     * @return 批量获取的数量
     *
     * 根据对象大小动态调整批量数，小对象多取、大对象少取，
     * 控制每次批量获取的总内存不超过 4KB。
     */
    size_t getBatchNum(size_t size);

    /**
     * @brief 判断是否需要将内存归还给中心缓存
     * @param index 自由链表索引
     * @return true 表示需要归还
     */
    bool shouldReturnToCentralCache(size_t index);

private:
    std::array<void*, FREE_LIST_SIZE> freeList_;       ///< 每个大小类的自由链表头指针
    std::array<size_t, FREE_LIST_SIZE> freeListSize_;  ///< 每个大小类的自由链表节点计数
};

} // namespace Kama_memoryPool
