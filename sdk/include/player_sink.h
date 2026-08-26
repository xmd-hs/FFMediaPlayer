#pragma once

#include "player_types.h"

namespace ffplayer {

class IVideoSink {
public:
    virtual ~IVideoSink() = default;
    virtual void onVideoFrame(const VideoFrame& frame) = 0;
};

class IAudioSink {
public:
    virtual ~IAudioSink() = default;
    virtual bool onAudioChunk(const AudioChunk& chunk) = 0;
    // Returns the amount of PCM buffered by the output device.
    virtual MediaTimeMs bufferedDurationMs() const { return 0; }
};

class ISubtitleSink {
public:
    virtual ~ISubtitleSink() = default;
    virtual void onSubtitle(const std::string& text,
                            MediaTimeMs startMs,
                            MediaTimeMs endMs) = 0;
    virtual void onSubtitleClear() = 0;
};

} // namespace ffplayer
