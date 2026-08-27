#include "audio_sink.h"

namespace ffplayer {

bool PlatformAudioSink::onAudioChunk(const AudioChunk &chunk)
{
    if (!chunk.data || chunk.size <= 0 || chunk.sampleRate <= 0 || chunk.channels <= 0)
        return false;
    // Stub sink: accept and discard. Never report a fake growing buffer clock.
    bufferedMs_ = 0;
    return true;
}

MediaTimeMs PlatformAudioSink::bufferedDurationMs() const
{
    return 0;
}

void PlatformAudioSink::flush()
{
    bufferedMs_ = 0;
}

} // namespace ffplayer
