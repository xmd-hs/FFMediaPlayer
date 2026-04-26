/**
 * @file Common.h
 * @brief 内存池公共定义与工具类
 *
 * 定义内存池的核心常量、对齐策略和大小类管理工具：
 * - ALIGNMENT: 内存对齐粒度（8字节）
 * - MAX_BYTES: 内存池管理的最大对象大小（256KB）
 * - SizeClass: 大小类计算工具，负责对齐和索引映射
 *
 * @author FFMediaPlayer Team
 * @version 1.0
 */

#pragma once

#include <cstddef>
#include <atomic>
#include <array>

namespace Kama_memoryPool
{

constexpr size_t ALIGNMENT = 8;                  ///< 内存对齐粒度，8字节对齐
constexpr size_t MAX_BYTES = 256 * 1024;         ///< 内存池管理的最大对象大小，256KB
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT;  ///< 自由链表数组大小

/**
 * @brief 内存块头部信息结构
 *
 * 每个从系统分配的内存块头部携带此信息，用于跟踪内存块状态和链表管理。
 */
struct BlockHeader
{
    size_t size;           ///< 内存块大小（字节）
    bool   inUse;          ///< 使用标志，true 表示正在使用
    BlockHeader* next;     ///< 指向下一个内存块的指针
};

/**
 * @brief 大小类管理工具类
 *
 * 提供内存大小的对齐计算和自由链表索引映射功能。
 * 所有内存分配请求都会被对齐到 ALIGNMENT 的整数倍，
 * 并映射到对应的自由链表索引。
 */
class SizeClass
{
public:
    /**
     * @brief 将请求字节数向上对齐到 ALIGNMENT 的整数倍
     * @param bytes 请求的字节数
     * @return 对齐后的字节数
     */
    static size_t roundUp(size_t bytes)
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    /**
     * @brief 计算请求字节数对应的自由链表索引
     * @param bytes 请求的字节数
     * @return 自由链表数组索引
     */
    static size_t getIndex(size_t bytes)
    {
        bytes = std::max(bytes, ALIGNMENT);
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};

} // namespace Kama_memoryPool
