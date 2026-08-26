#pragma once
#include "../include/player_sink.h"

namespace ffplayer {
class PlatformVideoSink final : public IVideoSink {
public:
    void onVideoFrame(const VideoFrame &frame) override;
    const VideoFrame &lastFrame() const { return frame_; }

private:
    VideoFrame frame_{};
};
}
