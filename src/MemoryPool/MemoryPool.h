/**
 * @file MemoryPool.h
 * @brief 内存池统一入口
 *
 * 提供内存池的统一分配/释放接口，内部委托给 ThreadCache 实现。
 * 三级架构：ThreadCache → CentralCache → PageCache
 *
 * 使用方式：
 * @code
 *   void* ptr = Kama_memoryPool::MemoryPool::allocate(128);
 *   Kama_memoryPool::MemoryPool::deallocate(ptr, 128);
 * @endcode
 *
 * @author FFMediaPlayer Team
 * @version 1.0
 */

#pragma once

#include "ThreadCache.h"

namespace Kama_memoryPool
{

/**
 * @brief 内存池统一接口类
 *
 * 提供静态的 allocate/deallocate 方法，
 * 内部自动路由到当前线程的 ThreadCache 实例。
 */
class MemoryPool
{
public:
    /**
     * @brief 分配指定大小的内存
     * @param size 请求分配的字节数
     * @return 分配的内存地址，失败返回 nullptr
     */
    static void* allocate(size_t size)
    {
        return ThreadCache::getInstance()->allocate(size);
    }

    /**
     * @brief 释放之前分配的内存
     * @param ptr 待释放的内存地址
     * @param size 内存块大小（必须与分配时一致）
     */
    static void deallocate(void* ptr, size_t size)
    {
        ThreadCache::getInstance()->deallocate(ptr, size);
    }
};

} // namespace Kama_memoryPool
