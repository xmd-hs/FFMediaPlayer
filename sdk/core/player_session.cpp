#include "player_session.h"
#include <chrono>
#include <vector>
#include <thread>
#include <algorithm>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace ffplayer {

PlayerSession::PlayerSession() = default;
PlayerSession::~PlayerSession() { close(); }

bool PlayerSession::open(const std::string& url)
{
    if (url.empty()) return false;
    close();
    setState(PlaybackState::Opening);
    if (!demuxer_.open(url)) {
        if (errorCallback_) errorCallback_(demuxer_.lastError());
        setState(PlaybackState::Error);
        return false;
    }
    stopRequested_ = false;
    eofWorkers_ = 0;
    finishedNotified_ = false;
    paused_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = url;
        durationMs_ = demuxer_.duration();
    }
    auto vp = demuxer_.videoParameters();
    auto ap = demuxer_.audioParameters();
    auto sp = demuxer_.subtitleParameters();
    if (vp) {
        const bool opened = videoDecoder_.open(vp);
        avcodec_parameters_free(&vp);
        if (!opened) {
            close();
            setState(PlaybackState::Error);
            return false;
        }
    }
    if (ap) {
        const bool opened = audioDecoder_.open(ap);
        avcodec_parameters_free(&ap);
        if (!opened) {
            close();
            setState(PlaybackState::Error);
            return false;
        }
    }
    if (sp) {
        const bool opened = subtitleDecoder_.open(sp);
        avcodec_parameters_free(&sp);
        if (!opened) {
            // Subtitle decoding is optional; keep audio/video playback alive.
            subtitleDecoder_.close();
        }
    }
    videoPackets_.restart();
    audioPackets_.restart();
    subtitlePackets_.restart();
    demuxThread_ = std::thread(&PlayerSession::demuxLoop, this);
    if (demuxer_.videoStream() >= 0) videoThread_ = std::thread(&PlayerSession::videoLoop, this);
    if (demuxer_.audioStream() >= 0) audioThread_ = std::thread(&PlayerSession::audioLoop, this);
    if (demuxer_.subtitleStream() >= 0 && subtitleSink_)
        subtitleThread_ = std::thread(&PlayerSession::subtitleLoop, this);
    setState(PlaybackState::Paused);
    return true;
}

void PlayerSession::close()
{
    stopRequested_ = true;
    videoPackets_.stop();
    audioPackets_.stop();
    subtitlePackets_.stop();
    if (demuxThread_.joinable()) demuxThread_.join();
    if (videoThread_.joinable()) videoThread_.join();
    if (audioThread_.joinable()) audioThread_.join();
    if (subtitleThread_.joinable()) subtitleThread_.join();
    AVPacket* packet = nullptr;
    while (videoPackets_.pop(packet)) {
        av_packet_free(&packet);
    }
    while (audioPackets_.pop(packet)) {
        av_packet_free(&packet);
    }
    while (subtitlePackets_.pop(packet)) av_packet_free(&packet);
    subtitleDecoder_.close();
    videoDecoder_.close();
    audioDecoder_.close();
    if (resampler_) swr_free(&resampler_);
    if (scaler_) sws_freeContext(scaler_);
    clock_.pause();
    if (subtitleSink_) subtitleSink_->onSubtitleClear();
    demuxer_.close();
    std::lock_guard<std::mutex> lock(mutex_);
    url_.clear();
    durationMs_ = 0;
    state_ = PlaybackState::Idle;
}

void PlayerSession::play()
{
    if (state() == PlaybackState::Idle || state() == PlaybackState::Error) return;
    paused_ = false;
    clock_.start();
    setState(PlaybackState::Playing);
}

void PlayerSession::pause()
{
    paused_ = true;
    clock_.pause();
    setState(PlaybackState::Paused);
}

bool PlayerSession::seek(MediaTimeMs positionMs)
{
    if (positionMs < 0 || (durationMs_ > 0 && positionMs > durationMs_)) return false;
    const bool wasPlaying = !paused_;
    paused_ = true;
    setState(PlaybackState::Seeking);
    videoPackets_.clear();
    audioPackets_.clear();
    subtitlePackets_.clear();
    videoDecoder_.flush();
    audioDecoder_.flush();
    bool seekOk = false;
    {
        std::lock_guard<std::mutex> lock(demuxMutex_);
        seekOk = demuxer_.seek(positionMs);
    }
    if (!seekOk) {
        setState(wasPlaying ? PlaybackState::Playing : PlaybackState::Paused);
        paused_ = !wasPlaying;
        return false;
    }
    clock_.reset(positionMs);
    audioClockMs_ = positionMs;
    videoClockMs_ = positionMs;
    paused_ = !wasPlaying;
    if (wasPlaying) { clock_.start(); setState(PlaybackState::Playing); }
    else setState(PlaybackState::Paused);
    return true;
}

bool PlayerSession::selectAudioTrack(int streamIndex)
{
    if (state() == PlaybackState::Playing || state() == PlaybackState::Buffering) return false;
    std::lock_guard<std::mutex> lock(demuxMutex_);
    if (!demuxer_.selectAudioTrack(streamIndex)) return false;
    auto* parameters = demuxer_.audioParameters();
    if (!parameters || !audioDecoder_.open(parameters)) { avcodec_parameters_free(&parameters); return false; }
    avcodec_parameters_free(&parameters);
    audioPackets_.clear();
    audioClockMs_ = clock_.position();
    return true;
}

bool PlayerSession::selectSubtitleTrack(int streamIndex)
{
    if (state() == PlaybackState::Playing || state() == PlaybackState::Buffering) return false;
    std::lock_guard<std::mutex> lock(demuxMutex_);
    if (!demuxer_.selectSubtitleTrack(streamIndex)) return false;
    if (streamIndex == -1) {
        subtitleDecoder_.close();
        subtitlePackets_.clear();
        if (subtitleSink_) subtitleSink_->onSubtitleClear();
        return true;
    }
    auto* parameters = demuxer_.subtitleParameters();
    if (!parameters || !subtitleDecoder_.open(parameters)) { avcodec_parameters_free(&parameters); return false; }
    avcodec_parameters_free(&parameters);
    subtitlePackets_.clear();
    if (subtitleSink_) subtitleSink_->onSubtitleClear();
    return true;
}

void PlayerSession::demuxLoop()
{
    while (!stopRequested_) {
        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if ((demuxer_.videoStream() >= 0 && videoPackets_.empty()) &&
            (demuxer_.audioStream() >= 0 && audioPackets_.empty()))
            setState(PlaybackState::Buffering);
        AVPacket* packet = nullptr;
        {
            std::lock_guard<std::mutex> lock(demuxMutex_);
            packet = demuxer_.read();
        }
        if (!packet) {
            if (stopRequested_) break;
            if (!demuxer_.lastError().empty()) {
                if (errorCallback_) errorCallback_(demuxer_.lastError());
                setState(PlaybackState::Error);
            } else {
                videoPackets_.push(nullptr);
                audioPackets_.push(nullptr);
                subtitlePackets_.push(nullptr);
                if (demuxer_.videoStream() < 0 && demuxer_.audioStream() < 0) {
                    if (!finishedNotified_.exchange(true) && finishedCallback_) finishedCallback_();
                    setState(PlaybackState::Ended);
                }
            }
            paused_ = true;
            clock_.pause();
            break;
        }
        const int stream = packet->stream_index;
        if (stream == demuxer_.videoStream()) {
            if (!videoPackets_.push(packet)) av_packet_free(&packet);
        } else if (stream == demuxer_.audioStream()) {
            if (!audioPackets_.push(packet)) av_packet_free(&packet);
        } else if (stream == demuxer_.subtitleStream()) {
            if (!subtitlePackets_.push(packet)) av_packet_free(&packet);
        } else av_packet_free(&packet);
        if (state() == PlaybackState::Buffering) setState(PlaybackState::Playing);
    }
}

void PlayerSession::subtitleLoop()
{
    while (!stopRequested_) {
        AVPacket* packet = nullptr;
        if (!subtitlePackets_.pop(packet)) break;
        if (!packet) { if (subtitleSink_) subtitleSink_->onSubtitleClear(); break; }
        std::string text; MediaTimeMs start = 0, end = 0;
        if (subtitleDecoder_.decode(packet, text, start, end) && subtitleSink_)
            subtitleSink_->onSubtitle(text, start, end);
        av_packet_free(&packet);
    }
}

void PlayerSession::videoLoop()
{
    while (!stopRequested_) {
        AVPacket* packet = nullptr;
        if (!videoPackets_.pop(packet)) break;
        if (!packet) {
            videoDecoder_.sendEndOfStream();
            while (AVFrame* frame = videoDecoder_.receive()) {
                // Drain decoder after EOF so buffered final frames are delivered.
                if (videoSink_) {
                    VideoFrame out;
                    out.width = frame->width; out.height = frame->height;
                    out.ptsMs = frame->pts == AV_NOPTS_VALUE ? videoClockMs_.load() : frame->pts;
                    for (int i = 0; i < 3; ++i) { out.data[i] = frame->data[i]; out.linesize[i] = frame->linesize[i]; }
                    videoSink_->onVideoFrame(out);
                }
                av_frame_free(&frame);
            }
            if (++eofWorkers_ >= ((demuxer_.videoStream() >= 0) ? 1 : 0) + ((demuxer_.audioStream() >= 0) ? 1 : 0)) {
                if (!finishedNotified_.exchange(true) && finishedCallback_) finishedCallback_();
            }
            break;
        }
        if (!videoDecoder_.send(packet)) continue;
        while (AVFrame* frame = videoDecoder_.receive()) {
            const MediaTimeMs pts = frame->pts < 0 ? videoClockMs_.load() : frame->pts;
            videoClockMs_ = pts;
            // Use audio as the master clock for A/V synchronisation.
            const MediaTimeMs master = audioClockMs_.load();
            if (master > 0 && pts > master + 40) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::min<MediaTimeMs>(pts - master, 100)));
            } else if (master > 0 && pts + 250 < master) {
                av_frame_free(&frame);
                continue;
            }
            if (videoSink_) {
                AVFrame* yuv = frame;
                if (frame->format != AV_PIX_FMT_YUV420P) {
                    if (!scaler_) scaler_ = sws_getContext(frame->width, frame->height,
                        static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
                        AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    yuv = av_frame_alloc();
                    yuv->format = AV_PIX_FMT_YUV420P; yuv->width = frame->width; yuv->height = frame->height;
                    if (!yuv || av_frame_get_buffer(yuv, 32) < 0 ||
                        !scaler_ || sws_scale(scaler_, frame->data, frame->linesize, 0, frame->height,
                            yuv->data, yuv->linesize) <= 0) { av_frame_free(&yuv); yuv = nullptr; }
                }
                if (!yuv) { av_frame_free(&frame); continue; }
                VideoFrame out;
                for (int i = 0; i < 3; ++i) { out.data[i] = yuv->data[i]; out.linesize[i] = yuv->linesize[i]; }
                out.width = yuv->width; out.height = yuv->height; out.ptsMs = pts;
                videoSink_->onVideoFrame(out);
                if (yuv != frame) av_frame_free(&yuv);
            }
            av_frame_free(&frame);
        }
    }
}

void PlayerSession::audioLoop()
{
    while (!stopRequested_) {
        AVPacket* packet = nullptr;
        if (!audioPackets_.pop(packet)) break;
        if (!packet) {
            audioDecoder_.sendEndOfStream();
            while (AVFrame* frame = audioDecoder_.receive()) {
                av_frame_free(&frame);
            }
            if (++eofWorkers_ >= ((demuxer_.videoStream() >= 0) ? 1 : 0) + ((demuxer_.audioStream() >= 0) ? 1 : 0)) {
                if (!finishedNotified_.exchange(true) && finishedCallback_) finishedCallback_();
            }
            break;
        }
        if (!audioDecoder_.send(packet)) continue;
        while (AVFrame* frame = audioDecoder_.receive()) {
            processAudioFrame(frame);
            av_frame_free(&frame);
        }
    }
}

void PlayerSession::processAudioFrame(AVFrame* frame)
{
    if (!frame || !audioSink_) {
        return;
    }

    if (!resampler_) {
        AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
        AVChannelLayout inputLayout = frame->ch_layout;
        swr_alloc_set_opts2(
            &resampler_, &outputLayout, AV_SAMPLE_FMT_S16, 48000,
            &inputLayout, static_cast<AVSampleFormat>(frame->format),
            frame->sample_rate, 0, nullptr);
        if (!resampler_ || swr_init(resampler_) < 0) {
            swr_free(&resampler_);
            return;
        }
    }

    const int maxSamples = av_rescale_rnd(
        swr_get_delay(resampler_, frame->sample_rate) + frame->nb_samples,
        48000, frame->sample_rate, AV_ROUND_UP);
    if (maxSamples <= 0) {
        return;
    }

    std::vector<std::uint8_t> pcm(
        static_cast<std::size_t>(maxSamples) * 2 * sizeof(std::int16_t));
    std::uint8_t* outputData[] = {pcm.data()};
    const int samples = swr_convert(
        resampler_, outputData, maxSamples,
        const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
    if (samples <= 0) {
        return;
    }

    AudioChunk chunk;
    chunk.data = pcm.data();
    chunk.size = samples * 2 * static_cast<int>(sizeof(std::int16_t));
    chunk.sampleRate = 48000;
    chunk.channels = 2;
    chunk.ptsMs = frame->pts;
    audioSink_->onAudioChunk(chunk);

    const MediaTimeMs base = frame->pts >= 0 ? frame->pts : audioClockMs_.load();
    audioClockMs_ = base + static_cast<MediaTimeMs>(samples * 1000LL / 48000);
}

PlaybackState PlayerSession::state() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

MediaTimeMs PlayerSession::audioClock() const
{
    const MediaTimeMs audio = audioClockMs_.load();
    return audio > 0 ? audio : videoClockMs_.load();
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
