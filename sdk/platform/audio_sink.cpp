#include "audio_sink.h"

namespace ffplayer {

bool PlatformAudioSink::onAudioChunk(const AudioChunk &chunk)
{
    if (!chunk.data || chunk.size <= 0 || chunk.sampleRate <= 0 || chunk.channels <= 0)
        return false;

    const auto bytesPerSecond = static_cast<MediaTimeMs>(chunk.sampleRate) * chunk.channels * 2;
    bufferedMs_ += static_cast<MediaTimeMs>(chunk.size) * 1000 / bytesPerSecond;
    return true;
}

MediaTimeMs PlatformAudioSink::bufferedDurationMs() const
{
    return bufferedMs_;
}

} // namespace ffplayer
