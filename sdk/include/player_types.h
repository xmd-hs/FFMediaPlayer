#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ffplayer {

using MediaTimeMs = std::int64_t;

enum class PlaybackState {
    Idle, 
    Opening, 
    Playing, 
    Paused, 
    Buffering, 
    Seeking, 
    Ended, 
    Error
};

struct VideoFrame {
    const std::uint8_t* data[3]{};
    int linesize[3]{};
    int width = 0;
    int height = 0;
    MediaTimeMs ptsMs = 0;
};

// Phase-2 zero-copy: platform hardware surface (do not free nativeHandle manually —
// keepAlive owns the retain/release).
enum class HwVideoBackend {
    None = 0,
    VideoToolbox,
    D3D11,
    VAAPI,
    CUDA
};

struct HwVideoFrame {
    HwVideoBackend backend = HwVideoBackend::None;
    void* nativeHandle = nullptr;
    std::shared_ptr<void> keepAlive;
    int subresourceIndex = 0; // D3D11 texture array slice (data[1] from FFmpeg)
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

struct SubtitleImage {
    std::vector<std::uint8_t> rgba; // tightly packed RGBA8888
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    MediaTimeMs startMs = 0;
    MediaTimeMs endMs = 0;
};

} // namespace ffplayer
