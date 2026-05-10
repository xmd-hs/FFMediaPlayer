#pragma once

#include <cstddef>
#include <atomic>
#include <array>

namespace Kama_memoryPool
{

constexpr size_t ALIGNMENT = 8;
constexpr size_t MAX_BYTES = 256 * 1024;
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT;

class SizeClass
{
public:
    static size_t roundUp(size_t bytes)
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    static size_t getIndex(size_t bytes)
    {
        bytes = std::max(bytes, ALIGNMENT);
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};

} // namespace Kama_memoryPool
