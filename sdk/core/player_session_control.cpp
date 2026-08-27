#include "player_session.h"

#include <algorithm>
#include <iostream>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace ffplayer {

PlayerSession::PlayerSession() = default;
PlayerSession::~PlayerSession() { close(); }

void PlayerSession::setVideoSink(IVideoSink* sink)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlaybackState::Idle) videoSink_ = sink;
}

void PlayerSession::setAudioSink(IAudioSink* sink)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlaybackState::Idle) audioSink_ = sink;
}

void PlayerSession::setSubtitleSink(ISubtitleSink* sink)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlaybackState::Idle) subtitleSink_ = sink;
}

void PlayerSession::setStateCallback(Player::StateCallback cb)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlaybackState::Idle) stateCallback_ = std::move(cb);
}

void PlayerSession::setErrorCallback(Player::ErrorCallback cb)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlaybackState::Idle) errorCallback_ = std::move(cb);
}

void PlayerSession::setFinishedCallback(Player::FinishedCallback cb)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlaybackState::Idle) finishedCallback_ = std::move(cb);
}

void PlayerSession::setHwAccelEnabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
#if FFPLAYER_ENABLE_HWACCEL
    hwAccelEnabled_ = enabled;
    const PlaybackState current = state();
    if (current == PlaybackState::Idle || current == PlaybackState::Opening ||
        current == PlaybackState::Error || demuxer_.videoStream() < 0) {
        std::clog << "[ffplayer] hw-switch deferred: preference="
                  << (enabled ? "hardware" : "software") << '\n';
        return;
    }
    const MediaTimeMs switchPosition = position();
    std::clog << "[ffplayer] hw-switch requested: target="
              << (enabled ? "hardware" : "software")
              << " position_ms=" << switchPosition << '\n';
    const bool switched = switchVideoDecoder(enabled, switchPosition);
    std::clog << "[ffplayer] hw-switch " << (switched ? "completed" : "failed")
              << ": active=" << (videoDecoder_.hwAccelActive() ? "hardware" : "software")
              << " position_ms=" << position() << '\n';
#else
    (void)enabled;
#endif
}

bool PlayerSession::switchVideoDecoder(bool enableHwAccel, MediaTimeMs positionMs)
{
    const PlaybackState current = state();
    const bool resumePlayback = current == PlaybackState::Playing ||
        current == PlaybackState::Buffering;
    if (resumePlayback) pause();

    AVCodecParameters* parameters = nullptr;
    {
        std::lock_guard<std::mutex> lock(demuxMutex_);
        parameters = demuxer_.videoParameters();
    }
    FfmpegDecoder next;
    const bool opened = parameters && next.open(parameters, enableHwAccel);
    avcodec_parameters_free(&parameters);
    if (!opened) {
        std::clog << "[ffplayer] hw-switch decoder rebuild failed\n";
        if (resumePlayback) play();
        return false;
    }
    const bool activeHardware = next.hwAccelActive();
    next.setFramePool(&videoFramePool_);

    // Invalidate in-flight video work before replacing the decoder context.
    seekEpoch_.fetch_add(1, std::memory_order_acq_rel);
    videoPackets_.clear(freePacket);
    {
        std::lock_guard<std::mutex> lock(videoCodecMutex_);
        videoDecoder_ = std::move(next);
    }

    // Re-read from a keyframe at the current media position. seek() restores
    // the audio/video clocks and resumes playback when it was previously active.
    if (!seek(positionMs)) {
        std::clog << "[ffplayer] hw-switch seek recovery failed: position_ms="
                  << positionMs << '\n';
        if (resumePlayback) play();
        return false;
    }
    if (resumePlayback) play();
    std::clog << "[ffplayer] hw-switch decoder rebuilt: requested="
              << (enableHwAccel ? "hardware" : "software")
              << " active=" << (activeHardware ? "hardware" : "software")
              << " resumed=" << (resumePlayback ? 1 : 0) << '\n';
    return true;
}

void PlayerSession::setVolume(float volume)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    volume_ = volume < 0.f ? 0.f : volume > 1.f ? 1.f : volume;
}

void PlayerSession::setSpeed(double speed)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    if (speed <= 0.0) return;
    speed = std::max(0.25, std::min(4.0, speed));
    speed_ = speed;
    clock_.setSpeed(speed);
    std::lock_guard<std::mutex> lock(resamplerMutex_);
    tempoFilter_.close();
}

void PlayerSession::resetResampler()
{
    std::lock_guard<std::mutex> lock(resamplerMutex_);
    if (resampler_) swr_free(&resampler_);
    resamplerSrcRate_ = 0;
    resamplerSrcFormat_ = -1;
    resamplerSrcLayout_ = 0;
    tempoFilter_.close();
}

void PlayerSession::resetScaler()
{
    if (scaler_) sws_freeContext(scaler_);
    scaler_ = nullptr;
    scalerSrcW_ = 0;
    scalerSrcH_ = 0;
    scalerSrcFormat_ = -1;
    scalerDstFormat_ = -1;
}

bool PlayerSession::ensureSubtitleThread()
{
    if (!subtitleSink_) return false;
    if (subtitleThread_.joinable()) return true;
    subtitlePackets_.restart();
    subtitleThread_ = std::thread(&PlayerSession::subtitleLoop, this);
    return true;
}

bool PlayerSession::open(const std::string& url)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    if (url.empty()) return false;
    close();
    // close() sets this flag; clear it before FFmpeg invokes the I/O interrupt
    // callback while opening a new network source.
    stopRequested_ = false;
    setState(PlaybackState::Opening);
    demuxer_.setInterruptCallback([this] { return stopRequested_.load(std::memory_order_acquire); });
    if (!demuxer_.open(url)) {
        if (errorCallback_) errorCallback_(demuxer_.lastError().empty()
            ? "failed to open media" : demuxer_.lastError());
        setState(PlaybackState::Error);
        return false;
    }
    demuxAtEof_ = false;
    eofDrainActive_ = false;
    decodeErrorNotified_ = false;
    subtitlePump_ = false;
    eofWorkers_ = 0;
    finishedNotified_ = false;
    seekEpoch_ = 0;
    paused_ = true;
    audioClockMs_ = 0;
    videoClockMs_ = 0;
    audioSeekTargetMs_ = -1;
    videoSeekTargetMs_ = -1;
    videoCatchUp_ = false;
    videoFramePool_.clear();
    scalerFramePool_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = url;
        durationMs_ = demuxer_.duration();
    }
    auto vp = demuxer_.videoParameters();
    auto ap = demuxer_.audioParameters();
    auto sp = demuxer_.subtitleParameters();
    if (vp) {
        const bool opened = videoDecoder_.open(vp, hwAccelEnabled_.load());
        avcodec_parameters_free(&vp);
        if (!opened) {
            if (errorCallback_) errorCallback_(videoDecoder_.lastError());
            close();
            setState(PlaybackState::Error);
            return false;
        }
        videoDecoder_.setFramePool(&videoFramePool_);
    }
    if (ap) {
        const bool opened = audioDecoder_.open(ap, false);
        avcodec_parameters_free(&ap);
        if (!opened) {
            if (errorCallback_) errorCallback_(audioDecoder_.lastError());
            close();
            setState(PlaybackState::Error);
            return false;
        }
        audioDecoder_.setFramePool(nullptr);
    }
    if (sp) {
        const bool opened = subtitleDecoder_.open(sp);
        avcodec_parameters_free(&sp);
        if (!opened) {
            subtitleDecoder_.close();
        }
    }
    videoPackets_.restart();
    audioPackets_.restart();
    subtitlePackets_.restart();
    demuxThread_ = std::thread(&PlayerSession::demuxLoop, this);
    if (demuxer_.videoStream() >= 0) videoThread_ = std::thread(&PlayerSession::videoLoop, this);
    if (demuxer_.audioStream() >= 0) audioThread_ = std::thread(&PlayerSession::audioLoop, this);
    if (demuxer_.subtitleStream() >= 0 && subtitleSink_) {
        subtitlePump_ = true;
        subtitleThread_ = std::thread(&PlayerSession::subtitleLoop, this);
    }
    setState(PlaybackState::Paused);
    return true;
}

void PlayerSession::close()
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    stopRequested_ = true;
    paused_ = false;
    if (audioSink_) audioSink_->setPaused(false);
    wakeQueues();
    videoPackets_.stop(freePacket);
    audioPackets_.stop(freePacket);
    subtitlePackets_.stop(freePacket);
    if (demuxThread_.joinable()) demuxThread_.join();
    if (videoThread_.joinable()) videoThread_.join();
    if (audioThread_.joinable()) audioThread_.join();
    if (subtitleThread_.joinable()) subtitleThread_.join();
    {
        std::scoped_lock lock(videoCodecMutex_, audioCodecMutex_, subtitleCodecMutex_);
        subtitleDecoder_.close();
        videoDecoder_.close();
        audioDecoder_.close();
    }
    resetResampler();
    resetScaler();
    clock_.pause();
    if (audioSink_) audioSink_->flush();
    if (subtitleSink_) subtitleSink_->onSubtitleClear();
    demuxer_.close();
    demuxAtEof_ = false;
    eofDrainActive_ = false;
    decodeErrorNotified_ = false;
    subtitlePump_ = false;
    eofWorkers_ = 0;
    finishedNotified_ = false;
    videoCatchUp_ = false;
    videoFramePool_.clear();
    scalerFramePool_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    url_.clear();
    durationMs_ = 0;
    state_ = PlaybackState::Idle;
}

void PlayerSession::play()
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    const PlaybackState current = state();
    if (current == PlaybackState::Idle || current == PlaybackState::Error) return;

    if (current == PlaybackState::Ended) {
        if (!seek(0)) return;
    }

    paused_ = false;
    if (audioSink_) audioSink_->setPaused(false);
    wakeQueues();
    clock_.start();
    setState(PlaybackState::Playing);
}

void PlayerSession::pause()
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    const PlaybackState current = state();
    if (current != PlaybackState::Playing && current != PlaybackState::Buffering) return;
    const MediaTimeMs pausedAt = position();
    paused_ = true;
    if (audioSink_) audioSink_->setPaused(true);
    clock_.reset(pausedAt);
    wakeQueues();
    setState(PlaybackState::Paused);
}

bool PlayerSession::seek(MediaTimeMs positionMs)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    if (positionMs < 0 || (durationMs_ > 0 && positionMs > durationMs_)) return false;
    const PlaybackState current = state();
    if (current == PlaybackState::Idle || current == PlaybackState::Opening ||
        current == PlaybackState::Error) {
        return false;
    }

    const bool wasPlaying = current == PlaybackState::Playing ||
        current == PlaybackState::Buffering ||
        (current == PlaybackState::Seeking && !paused_);

    paused_ = true;
    clock_.pause();
    wakeQueues();
    setState(PlaybackState::Seeking);

    bool seekOk = false;
    {
        std::lock_guard<std::mutex> lock(demuxMutex_);
        seekOk = demuxer_.seek(positionMs);
    }
    if (!seekOk) {
        paused_ = !wasPlaying;
        if (wasPlaying) {
            clock_.start();
            setState(PlaybackState::Playing);
        } else {
            setState(PlaybackState::Paused);
        }
        wakeQueues();
        return false;
    }

    demuxAtEof_ = false;
    eofDrainActive_ = false;
    decodeErrorNotified_ = false;
    finishedNotified_ = false;
    eofWorkers_ = 0;
    seekEpoch_.fetch_add(1, std::memory_order_acq_rel);
    audioSeekTargetMs_ = demuxer_.audioStream() >= 0 ? positionMs : -1;
    videoSeekTargetMs_ = demuxer_.videoStream() >= 0 ? positionMs : -1;

    videoPackets_.clear(freePacket);
    audioPackets_.clear(freePacket);
    subtitlePackets_.clear(freePacket);
    {
        std::scoped_lock lock(videoCodecMutex_, audioCodecMutex_, subtitleCodecMutex_);
        videoDecoder_.flush();
        audioDecoder_.flush();
        subtitleDecoder_.flush();
    }
    {
        std::lock_guard<std::mutex> lock(resamplerMutex_);
        tempoFilter_.flush();
    }
    if (audioSink_) audioSink_->flush();
    if (subtitleSink_) subtitleSink_->onSubtitleClear();

    clock_.reset(positionMs);
    audioClockMs_ = positionMs;
    videoClockMs_ = positionMs;
    videoCatchUp_ = false;
    videoFramePool_.clear();
    scalerFramePool_.clear();
    const std::uint64_t previewEpoch = seekEpoch_.load(std::memory_order_acquire);
    if (!wasPlaying) decodePausedVideoPreview(positionMs, previewEpoch);
    paused_ = !wasPlaying;
    if (wasPlaying) {
        if (audioSink_) audioSink_->setPaused(false);
        clock_.start();
        setState(PlaybackState::Playing);
    } else {
        if (audioSink_) audioSink_->setPaused(true);
        setState(PlaybackState::Paused);
    }
    wakeQueues();
    return true;
}

bool PlayerSession::selectAudioTrack(int streamIndex)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    const PlaybackState current = state();
    if (current == PlaybackState::Idle || current == PlaybackState::Opening ||
        current == PlaybackState::Seeking || current == PlaybackState::Error) return false;

    const bool resumePlayback = current == PlaybackState::Playing ||
        current == PlaybackState::Buffering;
    const MediaTimeMs switchPosition = position();
    if (resumePlayback) pause();

    bool switched = false;
    {
        std::lock_guard<std::mutex> demuxLock(demuxMutex_);
        const int previousStream = demuxer_.audioStream();
        if (previousStream == streamIndex) {
            switched = true;
        } else if (demuxer_.selectAudioTrack(streamIndex)) {
            auto* parameters = demuxer_.audioParameters();
            FfmpegDecoder next;
            const bool opened = parameters && next.open(parameters, false);
            avcodec_parameters_free(&parameters);
            if (!opened) {
                demuxer_.selectAudioTrack(previousStream);
            } else {
                seekEpoch_.fetch_add(1, std::memory_order_acq_rel);
                audioPackets_.clear(freePacket);
                resetResampler();
                if (audioSink_) audioSink_->flush();
                {
                    std::lock_guard<std::mutex> codecLock(audioCodecMutex_);
                    audioDecoder_ = std::move(next);
                    audioDecoder_.setFramePool(nullptr);
                }
                switched = true;
            }
        }
    }

    if (switched) switched = seek(switchPosition);
    if (resumePlayback) play();
    return switched;
}

bool PlayerSession::selectSubtitleTrack(int streamIndex)
{
    std::lock_guard<std::recursive_mutex> controlLock(controlMutex_);
    const PlaybackState current = state();
    if (current == PlaybackState::Idle || current == PlaybackState::Opening ||
        current == PlaybackState::Seeking || current == PlaybackState::Error) return false;

    const bool resumePlayback = current == PlaybackState::Playing ||
        current == PlaybackState::Buffering;
    const MediaTimeMs switchPosition = position();
    if (resumePlayback) pause();

    bool switched = false;
    {
        std::lock_guard<std::mutex> demuxLock(demuxMutex_);
        const int previousStream = demuxer_.subtitleStream();
        if (previousStream == streamIndex) {
            switched = true;
        } else if (demuxer_.selectSubtitleTrack(streamIndex)) {
            if (streamIndex == -1) {
                seekEpoch_.fetch_add(1, std::memory_order_acq_rel);
                subtitlePump_ = false;
                {
                    std::lock_guard<std::mutex> codecLock(subtitleCodecMutex_);
                    subtitleDecoder_.close();
                }
                subtitlePackets_.clear(freePacket);
                if (subtitleSink_) subtitleSink_->onSubtitleClear();
                switched = true;
            } else {
                auto* parameters = demuxer_.subtitleParameters();
                FfmpegSubtitleDecoder next;
                const bool opened = parameters && next.open(parameters);
                avcodec_parameters_free(&parameters);
                if (!opened) {
                    demuxer_.selectSubtitleTrack(previousStream);
                } else {
                    seekEpoch_.fetch_add(1, std::memory_order_acq_rel);
                    {
                        std::lock_guard<std::mutex> codecLock(subtitleCodecMutex_);
                        subtitleDecoder_ = std::move(next);
                    }
                    subtitlePackets_.clear(freePacket);
                    if (subtitleSink_) subtitleSink_->onSubtitleClear();
                    if (ensureSubtitleThread()) subtitlePump_ = true;
                    switched = true;
                }
            }
        }
    }

    if (switched) switched = seek(switchPosition);
    if (resumePlayback) play();
    return switched;
}

} // namespace ffplayer
