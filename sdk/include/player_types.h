#pragma once

#include <cstdint>
#include <string>

namespace ffplayer {

using MediaTimeMs = std::int64_t;

enum class PlaybackState {
    Idle, Opening, Playing, Paused, Buffering, Seeking, Ended, Error
};

struct VideoFrame {
    const std::uint8_t* data[3]{};
    int linesize[3]{};
    int width = 0;
    int height = 0;
    MediaTimeMs ptsMs = 0;
};

struct AudioChunk {
    const std::uint8_t* data = nullptr;
    int size = 0;
    int sampleRate = 0;
    int channels = 0;
    MediaTimeMs ptsMs = 0;
};

struct TrackInfo {
    int streamIndex = -1;
    std::string language;
    std::string title;
    std::string codec;
};

} // namespace ffplayer
