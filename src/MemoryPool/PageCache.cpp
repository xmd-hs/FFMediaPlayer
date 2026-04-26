#include "PageCache.h"
#include <cstring>
#include <cstdlib>
#include <cstdint>

#ifdef _WIN32
#else
#include <sys/mman.h>
#endif

namespace Kama_memoryPool
{

void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;

        if (span->next)
        {
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }

        if (span->numPages > numPages) 
        {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + 
                                numPages * PAGE_SIZE;
            newSpan->numPages = span->numPages - numPages;
            newSpan->next = nullptr;

            auto& list = freeSpans_[newSpan->numPages];
            newSpan->next = list;
            list = newSpan;

            span->numPages = numPages;
        }

        spanMap_[span->pageAddr] = span;
        return span->pageAddr;
    }

    void* memory = systemAlloc(numPages);
    if (!memory) return nullptr;

    Span* span = new Span;
    span->pageAddr = memory;
    span->numPages = numPages;
    span->next = nullptr;

    spanMap_[memory] = span;
    return memory;
}

void PageCache::deallocateSpan(void* ptr, size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = spanMap_.find(ptr);
    if (it == spanMap_.end()) return;

    Span* span = it->second;

    void* nextAddr = static_cast<char*>(ptr) + numPages * PAGE_SIZE;
    auto nextIt = spanMap_.find(nextAddr);
    
    if (nextIt != spanMap_.end())
    {
        Span* nextSpan = nextIt->second;
        
        bool found = false;
        auto& nextList = freeSpans_[nextSpan->numPages];
        
        if (nextList == nextSpan)
        {
            nextList = nextSpan->next;
            found = true;
        }
        else if (nextList)
        {
            Span* prev = nextList;
            while (prev->next)
            {
                if (prev->next == nextSpan)
                {   
                    prev->next = nextSpan->next;
                    found = true;
                    break;
                }
                prev = prev->next;
            }
        }

        if (found)
        {
            span->numPages += nextSpan->numPages;
            spanMap_.erase(nextAddr);
            delete nextSpan;
        }
    }

    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}

void* PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;

#ifdef _WIN32
    size_t allocSize = size + PAGE_SIZE + sizeof(void*);
    void* rawPtr = std::malloc(allocSize);
    if (!rawPtr) return nullptr;

    void* alignedPtr = reinterpret_cast<void*>(
        (reinterpret_cast<uintptr_t>(rawPtr) + sizeof(void*) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)
    );

    void** storedPtr = reinterpret_cast<void**>(alignedPtr) - 1;
    *storedPtr = rawPtr;

    memset(alignedPtr, 0, size);
    return alignedPtr;
#else
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;
    return ptr;
#endif
}

void PageCache::systemFree(void* ptr, size_t numPages)
{
    if (!ptr) return;

#ifdef _WIN32
    void** storedPtr = reinterpret_cast<void**>(ptr) - 1;
    void* rawPtr = *storedPtr;
    std::free(rawPtr);
#else
    size_t size = numPages * PAGE_SIZE;
    munmap(ptr, size);
#endif
}

PageCache::~PageCache()
{
    for (auto& pair : spanMap_)
    {
        Span* span = pair.second;
        if (span->pageAddr)
        {
            systemFree(span->pageAddr, span->numPages);
        }
        delete span;
    }
    spanMap_.clear();

    for (auto& pair : freeSpans_)
    {
        Span* span = pair.second;
        while (span)
        {
            Span* next = span->next;
            delete span;
            span = next;
        }
    }
    freeSpans_.clear();
}

} // namespace memoryPool
