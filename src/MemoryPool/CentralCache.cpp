#include "CentralCache.h"
#include "PageCache.h"
#include <cassert>
#include <thread>

namespace Kama_memoryPool
{

static const size_t SPAN_PAGES = 8;

void* CentralCache::fetchRange(size_t index, size_t batchNum)
{
    if (index >= FREE_LIST_SIZE || batchNum == 0)
        return nullptr;

    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    void* result = nullptr;
    try
    {
        result = centralFreeList_[index].load(std::memory_order_relaxed);

        if (!result)
        {
            size_t size = (index + 1) * ALIGNMENT;
            size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;
            size_t actualSpanPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
                ? SPAN_PAGES : numPages;
            result = fetchFromPageCache(size);

            if (!result)
            {
                locks_[index].clear(std::memory_order_release);
                return nullptr;
            }

            char* start = static_cast<char*>(result);
            size_t totalBlocks = (actualSpanPages * PageCache::PAGE_SIZE) / size;
            if (totalBlocks == 0) totalBlocks = 1;
            size_t allocBlocks = std::min(batchNum, totalBlocks);

            for (size_t i = 1; i < allocBlocks; ++i)
            {
                char* current = start + (i - 1) * size;
                char* next = start + i * size;
                *reinterpret_cast<void**>(current) = next;
            }
            *reinterpret_cast<void**>(start + (allocBlocks - 1) * size) = nullptr;

            if (totalBlocks > allocBlocks)
            {
                void* remainStart = start + allocBlocks * size;
                for (size_t i = allocBlocks + 1; i < totalBlocks; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (totalBlocks - 1) * size) = nullptr;

                centralFreeList_[index].store(remainStart, std::memory_order_release);
            }
        }
        else
        {
            void* current = result;
            void* prev = nullptr;
            size_t count = 0;

            while (current && count < batchNum)
            {
                prev = current;
                current = *reinterpret_cast<void**>(current);
                count++;
            }

            if (prev)
            {
                *reinterpret_cast<void**>(prev) = nullptr;
            }

            centralFreeList_[index].store(current, std::memory_order_release);
        }
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
    return result;
}

void CentralCache::returnRange(void* start, size_t numBlocks, size_t index)
{
    if (!start || index >= FREE_LIST_SIZE || numBlocks == 0)
        return;

    while (locks_[index].test_and_set(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    try
    {
        void* end = start;
        size_t count = 1;
        while (count < numBlocks && *reinterpret_cast<void**>(end) != nullptr) {
            end = *reinterpret_cast<void**>(end);
            count++;
        }

        void* current = centralFreeList_[index].load(std::memory_order_relaxed);
        *reinterpret_cast<void**>(end) = current;
        centralFreeList_[index].store(start, std::memory_order_release);
    }
    catch (...)
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
}

void* CentralCache::fetchFromPageCache(size_t size)
{
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE)
    {
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    }
    else
    {
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

} // namespace Kama_memoryPool
