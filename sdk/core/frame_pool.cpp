#include "frame_pool.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace ffplayer {

bool AvFramePool::matches(const Slot& slot, const int width, const int height, const int format) const
{
    if (width <= 0 || height <= 0 || format < 0) {
        // Decoder shells: only reuse cleared frames (dimensions zeroed by av_frame_unref).
        return slot.width <= 0 && slot.height <= 0;
    }
    return slot.width == width && slot.height == height && slot.format == format;
}

AVFrame* AvFramePool::acquire(const int width, const int height, const int format)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if (!matches(*it, width, height, format)) continue;
            AVFrame* frame = it->frame;
            free_.erase(it);
            return frame;
        }
    }

    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;

    if (width > 0 && height > 0 && format >= 0) {
        frame->format = format;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 32) < 0) {
            av_frame_free(&frame);
            return nullptr;
        }
    }
    return frame;
}

void AvFramePool::release(AVFrame* frame)
{
    if (!frame) return;
    av_frame_unref(frame);

    std::lock_guard<std::mutex> lock(mutex_);
    if (free_.size() >= maxFrames_) {
        av_frame_free(&frame);
        return;
    }
    free_.push_back(Slot{frame, frame->width, frame->height, frame->format});
}

void AvFramePool::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : free_) {
        av_frame_free(&slot.frame);
    }
    free_.clear();
}

} // namespace ffplayer
