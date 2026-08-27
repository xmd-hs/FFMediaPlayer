#include "player_session.h"
#include "player_session_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace ffplayer {

using namespace session_detail;

void PlayerSession::audioLoop()
{
    while (!stopRequested_) {
        waitWhilePaused();
        if (stopRequested_) break;

        AVPacket* packet = nullptr;
        if (!audioPackets_.pop(packet)) break;

        const std::uint64_t epoch = seekEpoch_.load();
        if (!packet) {
            if (!epochIsCurrent(epoch) || !demuxAtEof_.load(std::memory_order_acquire)) continue;
            {
                std::lock_guard<std::mutex> lock(audioCodecMutex_);
                audioDecoder_.sendEndOfStream();
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
            // atempo buffers samples internally. Drain them after the decoder
            // reaches EOF so non-1x playback does not lose its final audio.
            std::vector<std::int16_t> tail;
            {
                std::lock_guard<std::mutex> lock(resamplerMutex_);
                tempoFilter_.drain(tail);
            }
            if (!tail.empty() && audioSink_ && epochIsCurrent(epoch)) {
                const float volume = volume_.load();
                if (volume != 1.0f) {
                    for (auto& sample : tail) {
                        sample = static_cast<std::int16_t>(std::max(
                            -32768.f, std::min(32767.f, static_cast<float>(sample) * volume)));
                    }
                }
                AudioChunk chunk;
                chunk.data = reinterpret_cast<const std::uint8_t*>(tail.data());
                chunk.size = static_cast<int>(tail.size() * sizeof(std::int16_t));
                chunk.sampleRate = kOutputSampleRate;
                chunk.channels = kOutputChannels;
                chunk.ptsMs = audioClockMs_.load();
                int writeRetries = 0;
                while (!stopRequested_ && epochIsCurrent(epoch)) {
                    if (audioSink_->onAudioChunk(chunk)) {
                        const int frames = static_cast<int>(tail.size() / kOutputChannels);
                        audioClockMs_ += static_cast<MediaTimeMs>(
                            frames * 1000.0 * std::max(0.25, speed_.load()) / kOutputSampleRate);
                        notifyPlayback();
                        break;
                    }
                    if (++writeRetries > 400) break;
                    std::unique_lock<std::mutex> lock(playbackMutex_);
                    playbackCv_.wait_for(lock, std::chrono::milliseconds(5));
                }
            }
            notifyStreamFinished(epoch);
            continue;
        }

        if (!epochIsCurrent(epoch)) {
            freePacket(packet);
            continue;
        }

        while (packet && !stopRequested_) {
            waitWhilePaused();
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
                std::unique_lock<std::mutex> lock(playbackMutex_);
                playbackCv_.wait_for(lock, std::chrono::milliseconds(2));
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
    if (paused_.load(std::memory_order_acquire)) waitWhilePaused();
    if (stopRequested_ || !epochIsCurrent(epoch)) return;

    const std::uint64_t layoutId = frame->ch_layout.u.mask != 0
        ? frame->ch_layout.u.mask
        : static_cast<std::uint64_t>(frame->ch_layout.nb_channels);
    const double speed = std::max(0.25, std::min(4.0, speed_.load()));
    const MediaTimeMs frameStartMs = frame->pts;
    MediaTimeMs outputPtsMs = frameStartMs;
    MediaTimeMs seekTarget = audioSeekTargetMs_.load(std::memory_order_acquire);
    if (seekTarget >= 0 && frameStartMs >= 0 && frame->sample_rate > 0) {
        const MediaTimeMs frameDurationMs = static_cast<MediaTimeMs>(
            frame->nb_samples * 1000LL / frame->sample_rate);
        if (frameStartMs + frameDurationMs <= seekTarget) return;
    }

    int samples = 0;
    std::int16_t* pcmSamples = nullptr;
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

        // Member scratch: resize only grows capacity after the first frames.
        audioPcmScratch_.resize(static_cast<std::size_t>(maxSamples) * kOutputChannels);
        auto* pcmBytes = reinterpret_cast<std::uint8_t*>(audioPcmScratch_.data());
        std::uint8_t* outputData[] = {pcmBytes};
        samples = swr_convert(
            resampler_, outputData, maxSamples,
            const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
        if (samples <= 0) return;

        pcmSamples = audioPcmScratch_.data();

        if (seekTarget >= 0 && frameStartMs >= 0 && frameStartMs < seekTarget) {
            const auto skipSamples = static_cast<int>(std::min<MediaTimeMs>(
                samples,
                ((seekTarget - frameStartMs) * kOutputSampleRate + 999) / 1000));
            pcmSamples += skipSamples * kOutputChannels;
            samples -= skipSamples;
            outputPtsMs = seekTarget;
            if (samples <= 0) return;
        }
        if (std::fabs(speed - 1.0) > 1e-3) {
            if (!tempoFilter_.valid() ||
                std::fabs(tempoFilter_.tempo() - speed) > 1e-3 ||
                tempoFilter_.sampleRate() != kOutputSampleRate ||
                tempoFilter_.channels() != kOutputChannels) {
                if (!tempoFilter_.open(kOutputSampleRate, kOutputChannels, speed)) {
                    tempoFilter_.close();
                }
            }
            if (tempoFilter_.valid()) {
                if (tempoFilter_.process(pcmSamples, samples, audioTempoScratch_) &&
                    !audioTempoScratch_.empty()) {
                    // Point at tempo output — no second heap buffer / memcpy.
                    pcmSamples = audioTempoScratch_.data();
                    samples = static_cast<int>(audioTempoScratch_.size() / kOutputChannels);
                } else if (std::fabs(speed - 1.0) > 1e-3) {
                    return;
                }
            }
        }
    }
    if (samples <= 0 || !pcmSamples) return;
    if (seekTarget >= 0) {
        audioSeekTargetMs_.compare_exchange_strong(
            seekTarget, -1, std::memory_order_acq_rel);
    }

    const float volume = volume_.load();
    if (volume != 1.0f) {
        const int count = samples * kOutputChannels;
        for (int i = 0; i < count; ++i) {
            const float scaled = static_cast<float>(pcmSamples[i]) * volume;
            pcmSamples[i] = static_cast<std::int16_t>(
                std::max(-32768.f, std::min(32767.f, scaled)));
        }
    }

    if (!epochIsCurrent(epoch)) return;

    // Sink must copy before return (QtAudioSink does). Scratch is audio-thread-only.
    AudioChunk chunk;
    chunk.data = reinterpret_cast<const std::uint8_t*>(pcmSamples);
    chunk.size = samples * kOutputChannels * static_cast<int>(sizeof(std::int16_t));
    chunk.sampleRate = kOutputSampleRate;
    chunk.channels = kOutputChannels;
    chunk.ptsMs = outputPtsMs;

    int writeRetries = 0;
    while (!stopRequested_) {
        waitWhilePaused();
        if (!epochIsCurrent(epoch)) {
            return;
        }
        if (audioSink_->onAudioChunk(chunk)) {
            if (!epochIsCurrent(epoch)) {
                return;
            }
            const MediaTimeMs base = outputPtsMs >= 0 ? outputPtsMs : audioClockMs_.load();
            const MediaTimeMs mediaDuration =
                static_cast<MediaTimeMs>(samples * 1000.0 * speed / kOutputSampleRate);
            audioClockMs_ = base + std::max<MediaTimeMs>(0, mediaDuration);
            notifyPlayback();
            return;
        }
        if (++writeRetries > 400) {
            return;
        }
        {
            std::unique_lock<std::mutex> lock(playbackMutex_);
            playbackCv_.wait_for(lock, std::chrono::milliseconds(5));
        }
    }
}

} // namespace ffplayer
