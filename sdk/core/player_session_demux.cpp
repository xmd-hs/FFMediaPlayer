#include "player_session.h"

#include <mutex>
#include <string>

namespace ffplayer {

void PlayerSession::demuxLoop()
{
    while (!stopRequested_) {
        if (paused_ || demuxAtEof_) {
            waitForDemux();
            continue;
        }

        const std::uint64_t epoch = seekEpoch_.load(std::memory_order_acquire);
        AVPacket* packet = nullptr;
        int videoStream = -1;
        int audioStream = -1;
        int subtitleStream = -1;
        bool hasVideo = false;
        bool hasAudio = false;
        {
            std::lock_guard<std::mutex> lock(demuxMutex_);
            packet = demuxer_.read();
            videoStream = demuxer_.videoStream();
            audioStream = demuxer_.audioStream();
            subtitleStream = demuxer_.subtitleStream();
            hasVideo = videoStream >= 0;
            hasAudio = audioStream >= 0;
        }

        if (packet && !epochIsCurrent(epoch)) {
            freePacket(packet);
            continue;
        }

        if (!packet) {
            if (stopRequested_) break;
            if (!epochIsCurrent(epoch)) continue;

            if (!demuxer_.lastError().empty()) {
                const std::string err = demuxer_.lastError();
                if (!demuxAtEof_.exchange(true)) {
                    if (!epochIsCurrent(epoch)) {
                        demuxAtEof_ = false;
                        continue;
                    }
                    beginEofDrain();
                    pushPipelineEofSentinels(epoch);
                }
                if (!epochIsCurrent(epoch)) continue;
                if (errorCallback_) errorCallback_(err);
                setState(PlaybackState::Error);
                continue;
            }

            if (!demuxAtEof_.exchange(true)) {
                if (!epochIsCurrent(epoch)) {
                    demuxAtEof_ = false;
                    continue;
                }
                beginEofDrain();
                pushPipelineEofSentinels(epoch);
                if (!hasVideo && !hasAudio && epochIsCurrent(epoch)) {
                    if (!finishedNotified_.exchange(true)) {
                        endEofDrain();
                        paused_ = true;
                        clock_.pause();
                        setState(PlaybackState::Ended);
                        if (finishedCallback_) finishedCallback_();
                    }
                }
            }
            continue;
        }

        if (!epochIsCurrent(epoch)) {
            freePacket(packet);
            continue;
        }

        const int stream = packet->stream_index;
        if (stream == videoStream) {
            pushPacket(videoPackets_, packet, epoch);
        } else if (stream == audioStream) {
            pushPacket(audioPackets_, packet, epoch);
        } else if (stream == subtitleStream) {
            if (subtitlePump_.load()) pushPacket(subtitlePackets_, packet, epoch);
            else freePacket(packet);
        } else {
            freePacket(packet);
        }
    }
}

} // namespace ffplayer
