#pragma once

#include "player_types.h"

namespace ffplayer {

class IVideoSink {
public:
    virtual ~IVideoSink() = default;
    virtual void onVideoFrame(const VideoFrame& frame) = 0;
    // Phase-2: return true if the host can present HwVideoFrame without CPU copy.
    virtual bool supportsHwVideo() const { return false; }
    virtual void onHwVideoFrame(const HwVideoFrame& frame) { (void)frame; }
};

class IAudioSink {
public:
    virtual ~IAudioSink() = default;
    virtual bool onAudioChunk(const AudioChunk& chunk) = 0;
    virtual MediaTimeMs bufferedDurationMs() const { return 0; }
    virtual void flush() {}
};

class ISubtitleSink {
public:
    virtual ~ISubtitleSink() = default;
    virtual void onSubtitle(const std::string& text,
                            MediaTimeMs startMs,
                            MediaTimeMs endMs) = 0;
    // Bitmap / PGS style overlays (RGBA8888). Default no-op for text-only hosts.
    virtual void onSubtitleImage(const SubtitleImage& image)
    {
        (void)image;
    }
    virtual void onSubtitleClear() = 0;
};

} // namespace ffplayer
