#pragma once
#include "../include/player_sink.h"
#include <cstdint>
#include <vector>

namespace ffplayer {
class PlatformVideoSink final : public IVideoSink {
public:
    void onVideoFrame(const VideoFrame &frame) override;
    const VideoFrame &lastFrame() const { return frame_; }

private:
    VideoFrame frame_{};
    std::vector<std::uint8_t> planes_[3];
};
}
