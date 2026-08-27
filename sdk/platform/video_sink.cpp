#include "video_sink.h"
#include <cstring>

namespace ffplayer {

void PlatformVideoSink::onVideoFrame(const VideoFrame &frame)
{
    // Copy plane data so lastFrame() does not dangle after the source AVFrame is freed.
    frame_ = {};
    frame_.width = frame.width;
    frame_.height = frame.height;
    frame_.format = frame.format;
    frame_.ptsMs = frame.ptsMs;
    for (int i = 0; i < 3; ++i) {
        planes_[i].clear();
        frame_.data[i] = nullptr;
        frame_.linesize[i] = 0;
        if (!frame.data[i] || frame.linesize[i] <= 0 || frame.height <= 0) continue;
        const int rows = (i == 0) ? frame.height : (frame.height + 1) / 2;
        const int stride = frame.linesize[i];
        planes_[i].resize(static_cast<std::size_t>(rows) * static_cast<std::size_t>(stride));
        for (int y = 0; y < rows; ++y) {
            std::memcpy(
                planes_[i].data() + static_cast<std::size_t>(y) * stride,
                frame.data[i] + y * frame.linesize[i],
                static_cast<std::size_t>(stride));
        }
        frame_.data[i] = planes_[i].data();
        frame_.linesize[i] = stride;
    }
}

} // namespace ffplayer
