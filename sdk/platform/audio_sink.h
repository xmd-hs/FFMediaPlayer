#pragma once
#include "../include/player_sink.h"

namespace ffplayer {
class PlatformAudioSink final : public IAudioSink {
public:
    bool onAudioChunk(const AudioChunk &chunk) override;
    MediaTimeMs bufferedDurationMs() const override;

private:
    MediaTimeMs bufferedMs_ = 0;
};
}
