#include "player_session.h"

#include <algorithm>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace ffplayer {

PlayerSession::PlayerSession() = default;
PlayerSession::~PlayerSession() { close(); }

void PlayerSession::setVolume(float volume)
{
    volume_ = volume < 0.f ? 0.f : volume > 1.f ? 1.f : volume;
}

void PlayerSession::setSpeed(double speed)
{
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
    if (url.empty()) return false;
    close();
    setState(PlaybackState::Opening);
    if (!demuxer_.open(url)) {
        if (errorCallback_) errorCallback_(demuxer_.lastError().empty()
            ? "failed to open media" : demuxer_.lastError());
        setState(PlaybackState::Error);
        return false;
    }
    stopRequested_ = false;
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
    stopRequested_ = true;
    paused_ = false;
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
    const PlaybackState current = state();
    if (current == PlaybackState::Idle || current == PlaybackState::Error) return;

    if (current == PlaybackState::Ended || demuxAtEof_.load()) {
        if (!seek(0)) return;
    }

    paused_ = false;
    wakeQueues();
    clock_.start();
    setState(PlaybackState::Playing);
}

void PlayerSession::pause()
{
    paused_ = true;
    clock_.pause();
    wakeQueues();
    setState(PlaybackState::Paused);
}

bool PlayerSession::seek(MediaTimeMs positionMs)
{
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
    paused_ = !wasPlaying;
    if (wasPlaying) {
        clock_.start();
        setState(PlaybackState::Playing);
    } else {
        setState(PlaybackState::Paused);
    }
    wakeQueues();
    return true;
}

bool PlayerSession::selectAudioTrack(int streamIndex)
{
    const PlaybackState current = state();
    if (current == PlaybackState::Playing || current == PlaybackState::Buffering ||
        current == PlaybackState::Seeking) {
        return false;
    }
    paused_ = true;
    wakeQueues();

    std::lock_guard<std::mutex> demuxLock(demuxMutex_);
    const int previousStream = demuxer_.audioStream();
    if (!demuxer_.selectAudioTrack(streamIndex)) return false;

    auto* parameters = demuxer_.audioParameters();
    FfmpegDecoder next;
    const bool opened = parameters && next.open(parameters);
    avcodec_parameters_free(&parameters);
    if (!opened) {
        demuxer_.selectAudioTrack(previousStream);
        return false;
    }

    seekEpoch_.fetch_add(1, std::memory_order_acq_rel);
    audioPackets_.clear(freePacket);
    resetResampler();
    if (audioSink_) audioSink_->flush();
    {
        std::lock_guard<std::mutex> codecLock(audioCodecMutex_);
        audioDecoder_ = std::move(next);
        audioDecoder_.setFramePool(nullptr);
    }
    audioClockMs_ = clock_.position();
    return true;
}

bool PlayerSession::selectSubtitleTrack(int streamIndex)
{
    const PlaybackState current = state();
    if (current == PlaybackState::Playing || current == PlaybackState::Buffering ||
        current == PlaybackState::Seeking) {
        return false;
    }
    paused_ = true;
    wakeQueues();

    std::lock_guard<std::mutex> demuxLock(demuxMutex_);
    const int previousStream = demuxer_.subtitleStream();
    if (!demuxer_.selectSubtitleTrack(streamIndex)) return false;
    if (streamIndex == -1) {
        seekEpoch_.fetch_add(1, std::memory_order_acq_rel);
        subtitlePump_ = false;
        std::lock_guard<std::mutex> codecLock(subtitleCodecMutex_);
        subtitleDecoder_.close();
        subtitlePackets_.clear(freePacket);
        subtitlePackets_.tryPush(nullptr);
        if (subtitleSink_) subtitleSink_->onSubtitleClear();
        return true;
    }

    auto* parameters = demuxer_.subtitleParameters();
    FfmpegSubtitleDecoder next;
    const bool opened = parameters && next.open(parameters);
    avcodec_parameters_free(&parameters);
    if (!opened) {
        demuxer_.selectSubtitleTrack(previousStream);
        return false;
    }

    seekEpoch_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> codecLock(subtitleCodecMutex_);
        subtitleDecoder_ = std::move(next);
    }
    subtitlePackets_.clear(freePacket);
    if (subtitleSink_) subtitleSink_->onSubtitleClear();
    if (ensureSubtitleThread()) subtitlePump_ = true;
    return true;
}

} // namespace ffplayer
