#include "player_session.h"
#include "hw_frame_bridge.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>
#include <thread>
#include <algorithm>
#include <mutex>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace ffplayer {

namespace {
constexpr int kOutputSampleRate = 48000;
constexpr int kOutputChannels = 2;
}

PlayerSession::PlayerSession() = default;
PlayerSession::~PlayerSession() { close(); }

void PlayerSession::freePacket(AVPacket* packet)
{
    if (packet) av_packet_free(&packet);
}

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

void PlayerSession::wakeQueues()
{
    videoPackets_.wakeWaiters();
    audioPackets_.wakeWaiters();
    subtitlePackets_.wakeWaiters();
}

void PlayerSession::beginEofDrain()
{
    eofDrainActive_ = true;
    wakeQueues();
}

void PlayerSession::endEofDrain()
{
    eofDrainActive_ = false;
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
        while (paused_ && !stopRequested_ && epochIsCurrent(epoch)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (stopRequested_ || !epochIsCurrent(epoch)) break;
    }
    freePacket(packet);
    return false;
}

bool PlayerSession::pushEofSentinel(PacketQueue<AVPacket*>& queue, std::uint64_t epoch)
{
    while (!stopRequested_) {
        // Seek/close can invalidate EOF mid-push — never inject null into a new generation.
        if (!epochIsCurrent(epoch) || !demuxAtEof_.load(std::memory_order_acquire)) return false;
        if (queue.tryPush(nullptr)) {
            // Recheck after push: seek may have cleared demuxAtEof_/epoch between check and push.
            // Workers also ignore null when !demuxAtEof_, so a stray null is discarded.
            if (!epochIsCurrent(epoch) || !demuxAtEof_.load(std::memory_order_acquire)) return false;
            return true;
        }
        wakeQueues();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
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
    // Only pump subtitle packets when a sink can consume them — otherwise the queue fills and demux stalls.
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
        if (!seek(0)) {
            // Stay Ended/idle at EOF — do not clear flags and pretend we can play.
            return;
        }
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
    wakeQueues(); // unblock demux push waiting on a full queue
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

    // Invalidate EOF before bumping epoch / clearing queues so a late
    // pushEofSentinel cannot inject a live null into the new generation.
    demuxAtEof_ = false;
    eofDrainActive_ = false;
    decodeErrorNotified_ = false;
    finishedNotified_ = false;
    eofWorkers_ = 0;

    // Demux moved successfully — now invalidate in-flight work and drop stale packets.
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

    // Only mutate live decoder/queues after the replacement opens successfully.
    seekEpoch_.fetch_add(1, std::memory_order_acq_rel);
    audioPackets_.clear(freePacket);
    resetResampler();
    if (audioSink_) audioSink_->flush();
    {
        std::lock_guard<std::mutex> codecLock(audioCodecMutex_);
        audioDecoder_ = std::move(next);
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

void PlayerSession::waitIfPaused()
{
    while (paused_ && !eofDrainActive_ && !stopRequested_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

bool PlayerSession::sendPacket(FfmpegDecoder& decoder, AVPacket*& packet, std::mutex& codecMutex,
                               std::uint64_t epoch)
{
    while (!stopRequested_ && packet) {
        waitIfPaused();
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

        // Drain pending frames and retry once before treating as fatal.
        {
            std::lock_guard<std::mutex> lock(codecMutex);
            while (AVFrame* frame = decoder.receive()) {
                av_frame_free(&frame);
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
    // Drop late EOF notifications from a superseded seek generation.
    if (!epochIsCurrent(epoch) || !demuxAtEof_.load()) {
        eofWorkers_.fetch_sub(1);
        return;
    }
    if (!finishedNotified_.exchange(true)) {
        endEofDrain();
        paused_ = true;
        clock_.pause();
        // Keep Error if demux already failed; only report Ended for clean EOF.
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

void PlayerSession::demuxLoop()
{
    while (!stopRequested_) {
        if (paused_ || demuxAtEof_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
            // Null from a read that raced with seek: drop and resume with the new epoch.
            if (!epochIsCurrent(epoch)) continue;

            if (!demuxer_.lastError().empty()) {
                const std::string err = demuxer_.lastError();
                if (!demuxAtEof_.exchange(true)) {
                    if (!epochIsCurrent(epoch)) {
                        demuxAtEof_ = false;
                        continue;
                    }
                    // Unblock consumers so they can observe EOF and exit the decode path.
                    beginEofDrain();
                    pushPipelineEofSentinels(epoch);
                }
                if (!epochIsCurrent(epoch)) continue;
                if (errorCallback_) errorCallback_(err);
                setState(PlaybackState::Error);
                // Do not pause here: workers must still pop EOF sentinels.
                continue;
            }

            if (!demuxAtEof_.exchange(true)) {
                if (!epochIsCurrent(epoch)) {
                    // Seek won the race — do not leave demux stuck at EOF.
                    demuxAtEof_ = false;
                    continue;
                }
                // Keep consumers runnable so they can pop EOF sentinels and drain.
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
            // Idle demux via demuxAtEof_; do NOT pause workers before they finish EOF.
            continue;
        }

        if (!epochIsCurrent(epoch)) {
            freePacket(packet);
            continue;
        }
        // If paused, still push — pushPacket waits instead of dropping media.

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

void PlayerSession::subtitleLoop()
{
    while (!stopRequested_) {
        waitIfPaused();
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
                waitIfPaused();
                if (stopRequested_ || !epochIsCurrent(epoch)) break;
                now = masterClockMs();
                // Wait until the master clock reaches start; do not treat clock==0 as "ready".
                if (now >= startMs) break;
                const auto sleepMs = std::min<MediaTimeMs>(5, startMs - now);
                std::this_thread::sleep_for(std::chrono::milliseconds(std::max<MediaTimeMs>(1, sleepMs)));
            }
            if (!stopRequested_ && epochIsCurrent(epoch)) {
                now = masterClockMs();
                // Cue already expired while waiting — skip.
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

void PlayerSession::presentVideoFrame(AVFrame* frame, MediaTimeMs pts, std::uint64_t epoch)
{
    if (!frame || !videoSink_ || !epochIsCurrent(epoch)) return;

    // Phase-2 zero-copy path (only for formats the platform sink can actually draw).
    if (isHardwarePixelFormat(frame->format) && videoSink_->supportsHwVideo() &&
        canPresentHwVideoFrame(frame)) {
        HwVideoFrame hw;
        if (makeHwVideoFrame(frame, pts, hw) && epochIsCurrent(epoch)) {
            videoSink_->onHwVideoFrame(hw);
            return;
        }
        // Fall through: transfer to system memory below.
    }

    AVFrame* ownedTransfer = nullptr;
    AVFrame* yuv = frame;
    if (isHardwarePixelFormat(frame->format)) {
        AVFrame* sw = av_frame_alloc();
        if (!sw || av_hwframe_transfer_data(sw, frame, 0) < 0) {
            if (sw) av_frame_free(&sw);
            return;
        }
        av_frame_copy_props(sw, frame);
        ownedTransfer = sw;
        yuv = sw;
    }

    AVFrame* converted = nullptr;
    if (yuv->format != AV_PIX_FMT_YUV420P) {
        if (!scaler_ || scalerSrcW_ != yuv->width || scalerSrcH_ != yuv->height ||
            scalerSrcFormat_ != yuv->format) {
            resetScaler();
            scaler_ = sws_getContext(
                yuv->width, yuv->height, static_cast<AVPixelFormat>(yuv->format),
                yuv->width, yuv->height, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            scalerSrcW_ = yuv->width;
            scalerSrcH_ = yuv->height;
            scalerSrcFormat_ = yuv->format;
        }
        converted = av_frame_alloc();
        if (!converted) {
            if (ownedTransfer) av_frame_free(&ownedTransfer);
            return;
        }
        converted->format = AV_PIX_FMT_YUV420P;
        converted->width = yuv->width;
        converted->height = yuv->height;
        if (av_frame_get_buffer(converted, 32) < 0 || !scaler_ ||
            sws_scale(scaler_, yuv->data, yuv->linesize, 0, yuv->height,
                      converted->data, converted->linesize) <= 0) {
            av_frame_free(&converted);
            if (ownedTransfer) av_frame_free(&ownedTransfer);
            return;
        }
        yuv = converted;
    }

    if (!epochIsCurrent(epoch)) {
        if (converted) av_frame_free(&converted);
        if (ownedTransfer) av_frame_free(&ownedTransfer);
        return;
    }

    VideoFrame out;
    for (int i = 0; i < 3; ++i) {
        out.data[i] = yuv->data[i];
        out.linesize[i] = yuv->linesize[i];
    }
    out.width = yuv->width;
    out.height = yuv->height;
    out.ptsMs = pts;
    videoSink_->onVideoFrame(out);
    if (converted) av_frame_free(&converted);
    if (ownedTransfer) av_frame_free(&ownedTransfer);
}

bool PlayerSession::handleVideoFrame(AVFrame* frame, std::uint64_t epoch)
{
    if (!frame) return false;
    if (!epochIsCurrent(epoch)) {
        av_frame_free(&frame);
        return false;
    }
    // EOF drain while user paused: flush decoder state without presenting.
    if (paused_.load(std::memory_order_acquire) &&
        eofDrainActive_.load(std::memory_order_acquire)) {
        av_frame_free(&frame);
        return true;
    }
    const MediaTimeMs pts = frame->pts < 0 ? videoClockMs_.load() : frame->pts;
    videoClockMs_ = pts;
    const MediaTimeMs master = masterClockMs();
    double speed = 1.0;
    {
        std::lock_guard<std::mutex> lock(resamplerMutex_);
        if (tempoFilter_.valid() && std::fabs(speed_.load() - 1.0) > 1e-3) {
            speed = std::max(0.01, speed_.load());
        }
    }
    if (master >= 0 && pts > master + 40) {
        const MediaTimeMs delay = std::min<MediaTimeMs>(pts - master, 100);
        const auto sleepMs = static_cast<MediaTimeMs>(delay / speed);
        for (MediaTimeMs slept = 0; slept < sleepMs && !stopRequested_ && epochIsCurrent(epoch);) {
            if (paused_) break;
            const MediaTimeMs step = std::min<MediaTimeMs>(sleepMs - slept, 5);
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            slept += step;
        }
        if (!epochIsCurrent(epoch) || stopRequested_ || paused_) {
            av_frame_free(&frame);
            return false;
        }
    } else if (master > 0 && pts + 250 < master) {
        av_frame_free(&frame);
        return true;
    }
    presentVideoFrame(frame, pts, epoch);
    av_frame_free(&frame);
    return true;
}

void PlayerSession::videoLoop()
{
    while (!stopRequested_) {
        waitIfPaused();
        if (stopRequested_) break;

        const bool preferHwSurface =
            videoSink_ && videoSink_->supportsHwVideo() && videoDecoder_.hwAccelActive();

        AVPacket* packet = nullptr;
        if (!videoPackets_.pop(packet)) break;

        const std::uint64_t epoch = seekEpoch_.load();
        if (!packet) {
            // Stale EOF sentinel from a generation superseded by seek.
            if (!epochIsCurrent(epoch) || !demuxAtEof_.load(std::memory_order_acquire)) continue;
            std::vector<AVFrame*> drained;
            {
                std::lock_guard<std::mutex> lock(videoCodecMutex_);
                videoDecoder_.sendEndOfStream();
                while (AVFrame* frame = videoDecoder_.receive(preferHwSurface)) drained.push_back(frame);
            }
            for (AVFrame* frame : drained) {
                if (epochIsCurrent(epoch)) handleVideoFrame(frame, epoch);
                else av_frame_free(&frame);
            }
            notifyStreamFinished(epoch);
            continue;
        }

        if (!epochIsCurrent(epoch)) {
            freePacket(packet);
            continue;
        }

        while (packet && !stopRequested_) {
            waitIfPaused();
            if (!epochIsCurrent(epoch)) {
                freePacket(packet);
                break;
            }
            if (sendPacket(videoDecoder_, packet, videoCodecMutex_, epoch)) break;

            bool gotFrame = false;
            for (;;) {
                AVFrame* frame = nullptr;
                {
                    std::lock_guard<std::mutex> lock(videoCodecMutex_);
                    frame = videoDecoder_.receive(preferHwSurface);
                }
                if (!frame) break;
                gotFrame = true;
                handleVideoFrame(frame, epoch);
            }
            if (!gotFrame && packet) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        for (;;) {
            AVFrame* frame = nullptr;
            {
                std::lock_guard<std::mutex> lock(videoCodecMutex_);
                frame = videoDecoder_.receive(preferHwSurface);
            }
            if (!frame) break;
            handleVideoFrame(frame, epoch);
        }
    }
}

void PlayerSession::audioLoop()
{
    while (!stopRequested_) {
        waitIfPaused();
        if (stopRequested_) break;

        AVPacket* packet = nullptr;
        if (!audioPackets_.pop(packet)) break;

        const std::uint64_t epoch = seekEpoch_.load();
        if (!packet) {
            if (!epochIsCurrent(epoch) || !demuxAtEof_.load(std::memory_order_acquire)) continue;
            std::vector<AVFrame*> drained;
            {
                std::lock_guard<std::mutex> lock(audioCodecMutex_);
                audioDecoder_.sendEndOfStream();
                while (AVFrame* frame = audioDecoder_.receive()) drained.push_back(frame);
            }
            for (AVFrame* frame : drained) {
                if (epochIsCurrent(epoch)) processAudioFrame(frame, epoch);
                av_frame_free(&frame);
            }
            notifyStreamFinished(epoch);
            continue;
        }

        if (!epochIsCurrent(epoch)) {
            freePacket(packet);
            continue;
        }

        while (packet && !stopRequested_) {
            waitIfPaused();
            if (!epochIsCurrent(epoch)) {
                freePacket(packet);
                break;
            }
            if (sendPacket(audioDecoder_, packet, audioCodecMutex_, epoch)) break;

            bool gotFrame = false;
            for (;;) {
                AVFrame* frame = nullptr;
                {
                    std::lock_guard<std::mutex> lock(audioCodecMutex_);
                    frame = audioDecoder_.receive();
                }
                if (!frame) break;
                gotFrame = true;
                if (epochIsCurrent(epoch)) processAudioFrame(frame, epoch);
                av_frame_free(&frame);
            }
            if (!gotFrame && packet) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        for (;;) {
            AVFrame* frame = nullptr;
            {
                std::lock_guard<std::mutex> lock(audioCodecMutex_);
                frame = audioDecoder_.receive();
            }
            if (!frame) break;
            if (epochIsCurrent(epoch)) processAudioFrame(frame, epoch);
            av_frame_free(&frame);
        }
    }
}

void PlayerSession::processAudioFrame(AVFrame* frame, std::uint64_t epoch)
{
    if (!frame || !audioSink_ || !epochIsCurrent(epoch)) return;
    if (paused_.load(std::memory_order_acquire) &&
        eofDrainActive_.load(std::memory_order_acquire)) {
        return;
    }

    const std::uint64_t layoutId = frame->ch_layout.u.mask != 0
        ? frame->ch_layout.u.mask
        : static_cast<std::uint64_t>(frame->ch_layout.nb_channels);
    const double speed = std::max(0.25, std::min(4.0, speed_.load()));

    std::vector<std::uint8_t> pcm;
    int samples = 0;
    {
        std::lock_guard<std::mutex> lock(resamplerMutex_);
        if (!resampler_ || resamplerSrcRate_ != frame->sample_rate ||
            resamplerSrcFormat_ != frame->format || resamplerSrcLayout_ != layoutId) {
            if (resampler_) swr_free(&resampler_);
            resamplerSrcRate_ = 0;
            resamplerSrcFormat_ = -1;
            resamplerSrcLayout_ = 0;
            AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
            AVChannelLayout inputLayout = frame->ch_layout;
            if (swr_alloc_set_opts2(
                    &resampler_, &outputLayout, AV_SAMPLE_FMT_S16, kOutputSampleRate,
                    &inputLayout, static_cast<AVSampleFormat>(frame->format),
                    frame->sample_rate, 0, nullptr) < 0 ||
                !resampler_ || swr_init(resampler_) < 0) {
                if (resampler_) swr_free(&resampler_);
                return;
            }
            resamplerSrcRate_ = frame->sample_rate;
            resamplerSrcFormat_ = frame->format;
            resamplerSrcLayout_ = layoutId;
        }

        const int maxSamples = av_rescale_rnd(
            swr_get_delay(resampler_, frame->sample_rate) + frame->nb_samples,
            kOutputSampleRate, frame->sample_rate, AV_ROUND_UP);
        if (maxSamples <= 0) return;

        pcm.resize(static_cast<std::size_t>(maxSamples) * kOutputChannels * sizeof(std::int16_t));
        std::uint8_t* outputData[] = {pcm.data()};
        samples = swr_convert(
            resampler_, outputData, maxSamples,
            const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
        if (samples <= 0) return;

        // Pitch-preserving tempo (atempo). Falls back to passthrough if filter unavailable.
        if (std::fabs(speed - 1.0) > 1e-3) {
            if (!tempoFilter_.valid() ||
                std::fabs(tempoFilter_.tempo() - speed) > 1e-3 ||
                tempoFilter_.sampleRate() != kOutputSampleRate ||
                tempoFilter_.channels() != kOutputChannels) {
                if (!tempoFilter_.open(kOutputSampleRate, kOutputChannels, speed)) {
                    // Keep playback working even without atempo (pitch will change).
                    tempoFilter_.close();
                }
            }
            if (tempoFilter_.valid()) {
                std::vector<std::int16_t> stretched;
                const auto* inSamples = reinterpret_cast<const std::int16_t*>(pcm.data());
                if (tempoFilter_.process(inSamples, samples, stretched) && !stretched.empty()) {
                    pcm.resize(stretched.size() * sizeof(std::int16_t));
                    std::memcpy(pcm.data(), stretched.data(), pcm.size());
                    samples = static_cast<int>(stretched.size() / kOutputChannels);
                } else if (std::fabs(speed - 1.0) > 1e-3) {
                    // atempo still buffering — skip this slice rather than play at wrong speed.
                    return;
                }
            }
            // If atempo is unavailable, keep original PCM (1x duration); video sync matches 1x.
        }
    }
    if (samples <= 0) return;

    const float volume = volume_.load();
    if (volume != 1.0f) {
        auto* samples16 = reinterpret_cast<std::int16_t*>(pcm.data());
        const int count = samples * kOutputChannels;
        for (int i = 0; i < count; ++i) {
            const float scaled = static_cast<float>(samples16[i]) * volume;
            samples16[i] = static_cast<std::int16_t>(
                std::max(-32768.f, std::min(32767.f, scaled)));
        }
    }

    if (!epochIsCurrent(epoch)) return;

    AudioChunk chunk;
    chunk.data = pcm.data();
    chunk.size = samples * kOutputChannels * static_cast<int>(sizeof(std::int16_t));
    chunk.sampleRate = kOutputSampleRate;
    chunk.channels = kOutputChannels;
    chunk.ptsMs = frame->pts;

    int writeRetries = 0;
    while (!stopRequested_) {
        waitIfPaused();
        if (!epochIsCurrent(epoch)) {
            // Seek may have flushed mid-write; drop any remainder that still landed.
            if (audioSink_) audioSink_->flush();
            return;
        }
        if (audioSink_->onAudioChunk(chunk)) {
            if (!epochIsCurrent(epoch)) {
                audioSink_->flush();
                return;
            }
            const MediaTimeMs base = frame->pts >= 0 ? frame->pts : audioClockMs_.load();
            const MediaTimeMs mediaDuration =
                static_cast<MediaTimeMs>(samples * 1000LL / kOutputSampleRate);
            audioClockMs_ = base + std::max<MediaTimeMs>(0, mediaDuration);
            return;
        }
        if (++writeRetries > 400) {
            // Sink never accepted (e.g. no audio device) — drop without advancing clock.
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
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
