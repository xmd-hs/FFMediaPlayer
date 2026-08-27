#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

struct AVFrame;

namespace ffplayer {

// Reuses AVFrame shells and CPU buffers of matching width/height/format.
class AvFramePool {
public:
    explicit AvFramePool(std::size_t maxFrames = 12) : maxFrames_(maxFrames) {}
    AvFramePool(const AvFramePool&) = delete;
    AvFramePool& operator=(const AvFramePool&) = delete;

    AVFrame* acquire(int width = 0, int height = 0, int format = -1);
    void release(AVFrame* frame);
    void clear();

private:
    struct Slot {
        AVFrame* frame = nullptr;
        int width = 0;
        int height = 0;
        int format = -1;
    };

    bool matches(const Slot& slot, int width, int height, int format) const;
    std::size_t maxFrames_;
    std::mutex mutex_;
    std::vector<Slot> free_;
};

} // namespace ffplayer
