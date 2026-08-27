#include "player_session.h"

namespace ffplayer {

void PlayerSession::subtitleLoop()
{
    while (!stopRequested_) {
        waitWhilePaused();
        if (stopRequested_) break;

        AVPacket* packet = nullptr;
        if (!subtitlePackets_.pop(packet)) break;
        if (!packet) {
            // The last cue remains visible until its advertised end time. It is
            // cleared by close(), seek(), or an explicit subtitle-track change.
            continue;
        }
        const std::uint64_t epoch = seekEpoch_.load();
        DecodedSubtitle decoded;
        bool ok = false;
        if (epochIsCurrent(epoch)) {
            std::lock_guard<std::mutex> lock(subtitleCodecMutex_);
            ok = subtitleDecoder_.decode(packet, decoded);
        }
        if (ok && epochIsCurrent(epoch) && subtitleSink_) {
            const MediaTimeMs startMs = decoded.startMs;
            const MediaTimeMs endMs = decoded.endMs;
            MediaTimeMs now = 0;
            while (!stopRequested_ && epochIsCurrent(epoch) && startMs > 0) {
                waitWhilePaused();
                if (stopRequested_ || !epochIsCurrent(epoch)) break;
                now = masterClockMs();
                if (now >= startMs) break;
                waitForMasterClock(startMs, epoch);
            }
            if (!stopRequested_ && epochIsCurrent(epoch)) {
                now = masterClockMs();
                if (!(endMs > 0 && now > endMs)) {
                    if (decoded.hasText) {
                        subtitleSink_->onSubtitle(decoded.text, startMs, endMs);
                    }
                    if (!decoded.images.empty()) {
                        subtitleSink_->onSubtitleImages(decoded.images);
                    }
                }
            }
        }
        freePacket(packet);
    }
}

} // namespace ffplayer
