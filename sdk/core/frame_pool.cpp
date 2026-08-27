#include "frame_pool.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace ffplayer {

bool AvFramePool::matches(const Slot& slot, const int width, const int height, const int format) const
{
    if (width <= 0 || height <= 0 || format < 0) {
        // Decoder shells: only reuse fully cleared frames.
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

    const int width = frame->width;
    const int height = frame->height;
    const int format = frame->format;
    const AVPixFmtDescriptor* desc =
        format >= 0 ? av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format)) : nullptr;
    const bool hw = desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
    // Keep CPU plane buffers for sized reuse (scaler). Always drop HW / blank shells.
    const bool keepCpuBuffers =
        !hw && width > 0 && height > 0 && format >= 0 && frame->buf[0] != nullptr;

    if (!keepCpuBuffers) {
        av_frame_unref(frame);
    } else {
        frame->pts = AV_NOPTS_VALUE;
        frame->pkt_dts = AV_NOPTS_VALUE;
        frame->best_effort_timestamp = AV_NOPTS_VALUE;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (free_.size() >= maxFrames_) {
        av_frame_free(&frame);
        return;
    }
    if (keepCpuBuffers) {
        free_.push_back(Slot{frame, width, height, format});
    } else {
        free_.push_back(Slot{frame, 0, 0, -1});
    }
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
