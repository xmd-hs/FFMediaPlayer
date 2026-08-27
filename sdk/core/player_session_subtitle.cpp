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
            if (subtitleSink_ && epochIsCurrent(seekEpoch_.load()) &&
                demuxAtEof_.load(std::memory_order_acquire)) {
                subtitleSink_->onSubtitleClear();
            }
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
            const MediaTimeMs startMs = decoded.image.startMs;
            const MediaTimeMs endMs = decoded.image.endMs;
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
                        subtitleSink_->onSubtitle(decoded.text, decoded.image.startMs, decoded.image.endMs);
                    }
                    if (decoded.hasImage) {
                        subtitleSink_->onSubtitleImage(decoded.image);
                    }
                }
            }
        }
        freePacket(packet);
    }
}

} // namespace ffplayer
