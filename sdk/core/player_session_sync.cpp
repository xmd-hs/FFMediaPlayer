#include "player_session.h"
#include "player_session_internal.h"

#include <chrono>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace ffplayer {

using namespace session_detail;

void PlayerSession::freePacket(AVPacket* packet)
{
    if (packet) av_packet_free(&packet);
}

void PlayerSession::wakeQueues()
{
    videoPackets_.wakeWaiters();
    audioPackets_.wakeWaiters();
    subtitlePackets_.wakeWaiters();
    notifyPlayback();
}

void PlayerSession::notifyPlayback()
{
    playbackCv_.notify_all();
}

void PlayerSession::waitWhilePaused()
{
    if (stopRequested_ || eofDrainActive_ || !paused_) return;
    std::unique_lock<std::mutex> lock(playbackMutex_);
    playbackCv_.wait(lock, [&] {
        return stopRequested_ || !paused_ || eofDrainActive_;
    });
}

void PlayerSession::waitForDemux()
{
    std::unique_lock<std::mutex> lock(playbackMutex_);
    playbackCv_.wait(lock, [&] {
        return stopRequested_ || (!paused_ && !demuxAtEof_);
    });
}

void PlayerSession::waitForMasterClock(const MediaTimeMs targetPts, const std::uint64_t epoch)
{
    if (targetPts <= 0) return;
    std::unique_lock<std::mutex> lock(playbackMutex_);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kSyncMaxWaitMs);
    playbackCv_.wait_until(lock, deadline, [&] {
        if (stopRequested_ || !epochIsCurrent(epoch) || paused_) return true;
        return masterClockMs() >= targetPts;
    });
}

void PlayerSession::releaseVideoFrame(AVFrame* frame)
{
    if (!frame) return;
    videoDecoder_.releaseFrame(frame);
}

bool PlayerSession::shouldDropVideoFrame(const MediaTimeMs pts, const MediaTimeMs master,
                                         bool& catchUpMode) const
{
    if (master <= 0) return false;
    const MediaTimeMs delta = pts - master;

    if (delta < -kSyncCatchUpEnterMs) catchUpMode = true;
    else if (catchUpMode && delta > -kSyncCatchUpExitMs) catchUpMode = false;

    const MediaTimeMs dropThreshold = catchUpMode ? kSyncLatePresentMs : kSyncLateDropMs;
    return delta < -dropThreshold;
}

void PlayerSession::beginEofDrain()
{
    eofDrainActive_ = true;
    wakeQueues();
}

void PlayerSession::endEofDrain()
{
    eofDrainActive_ = false;
    notifyPlayback();
}

void PlayerSession::pushPipelineEofSentinels(const std::uint64_t epoch)
{
    if (demuxer_.videoStream() >= 0) pushEofSentinel(videoPackets_, epoch);
    if (demuxer_.audioStream() >= 0) pushEofSentinel(audioPackets_, epoch);
    if (subtitlePump_.load()) pushEofSentinel(subtitlePackets_, epoch);
}

void PlayerSession::signalDecodeError(const std::string& message, const std::uint64_t epoch)
{
    if (!epochIsCurrent(epoch)) return;

    bool expected = false;
    if (decodeErrorNotified_.compare_exchange_strong(expected, true)) {
        if (errorCallback_) errorCallback_(message);
        setState(PlaybackState::Error);
    }

    if (!demuxAtEof_.exchange(true)) {
        beginEofDrain();
        pushPipelineEofSentinels(epoch);
    }
}

bool PlayerSession::epochIsCurrent(std::uint64_t epoch) const
{
    return epoch == seekEpoch_.load(std::memory_order_acquire);
}

bool PlayerSession::pushPacket(PacketQueue<AVPacket*>& queue, AVPacket* packet, std::uint64_t epoch)
{
    while (!stopRequested_) {
        if (!epochIsCurrent(epoch)) {
            freePacket(packet);
            return false;
        }
        if (queue.push(packet, &paused_)) return true;
        waitWhilePaused();
        if (stopRequested_ || !epochIsCurrent(epoch)) break;
    }
    freePacket(packet);
    return false;
}

bool PlayerSession::pushEofSentinel(PacketQueue<AVPacket*>& queue, std::uint64_t epoch)
{
    while (!stopRequested_) {
        if (!epochIsCurrent(epoch) || !demuxAtEof_.load(std::memory_order_acquire)) return false;
        if (queue.tryPush(nullptr)) {
            if (!epochIsCurrent(epoch) || !demuxAtEof_.load(std::memory_order_acquire)) return false;
            return true;
        }
        wakeQueues();
        {
            std::unique_lock<std::mutex> lock(playbackMutex_);
            playbackCv_.wait_for(lock, std::chrono::milliseconds(10), [&] {
                return stopRequested_ || !epochIsCurrent(epoch) ||
                    !demuxAtEof_.load(std::memory_order_acquire);
            });
        }
    }
    return false;
}

bool PlayerSession::sendPacket(FfmpegDecoder& decoder, AVPacket*& packet, std::mutex& codecMutex,
                               std::uint64_t epoch)
{
    while (!stopRequested_ && packet) {
        waitWhilePaused();
        if (stopRequested_) break;
        if (!epochIsCurrent(epoch)) {
            freePacket(packet);
            return false;
        }
        int result = 0;
        {
            std::lock_guard<std::mutex> lock(codecMutex);
            result = decoder.send(packet);
        }
        if (result == 0) {
            av_packet_free(&packet);
            return true;
        }
        if (result == AVERROR(EAGAIN)) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(codecMutex);
            while (AVFrame* frame = decoder.receive()) {
                decoder.releaseFrame(frame);
            }
            result = decoder.send(packet);
        }
        if (result == 0) {
            av_packet_free(&packet);
            return true;
        }
        if (result == AVERROR(EAGAIN)) {
            return false;
        }

        const std::string err = decoder.lastError().empty()
            ? ("decode send failed: " + std::to_string(result))
            : decoder.lastError();
        signalDecodeError(err, epoch);
        av_packet_free(&packet);
        return false;
    }
    if (packet) av_packet_free(&packet);
    return false;
}

int PlayerSession::expectedEofWorkers() const
{
    return (demuxer_.videoStream() >= 0 ? 1 : 0) + (demuxer_.audioStream() >= 0 ? 1 : 0);
}

void PlayerSession::notifyStreamFinished(std::uint64_t epoch)
{
    if (!epochIsCurrent(epoch) || !demuxAtEof_.load()) return;
    const int expected = expectedEofWorkers();
    if (expected <= 0) return;
    const int done = eofWorkers_.fetch_add(1) + 1;
    if (done < expected) return;
    if (!epochIsCurrent(epoch) || !demuxAtEof_.load()) {
        eofWorkers_.fetch_sub(1);
        return;
    }
    if (!finishedNotified_.exchange(true)) {
        endEofDrain();
        paused_ = true;
        clock_.pause();
        notifyPlayback();
        if (state() != PlaybackState::Error) {
            setState(PlaybackState::Ended);
            if (finishedCallback_) finishedCallback_();
        }
    }
}

MediaTimeMs PlayerSession::masterClockMs() const
{
    MediaTimeMs audio = audioClockMs_.load();
    if (audio > 0) {
        if (audioSink_) {
            const MediaTimeMs bufferedWall = audioSink_->bufferedDurationMs();
            if (bufferedWall > 0) {
                const double speed = std::max(0.25, speed_.load());
                const auto bufferedMedia = static_cast<MediaTimeMs>(bufferedWall * speed);
                if (bufferedMedia > 0 && bufferedMedia < audio) audio -= bufferedMedia;
            }
        }
        return audio;
    }
    return clock_.position();
}

MediaTimeMs PlayerSession::position() const
{
    const MediaTimeMs media = masterClockMs();
    if (media > 0) return media;
    return clock_.position();
}

PlaybackState PlayerSession::state() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void PlayerSession::setState(PlaybackState state)
{
    Player::StateCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
        cb = stateCallback_;
    }
    if (cb) cb(state);
}

} // namespace ffplayer
