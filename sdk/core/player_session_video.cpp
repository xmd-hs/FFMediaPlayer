#include "player_session.h"
#include "player_session_internal.h"
#include "hw_frame_bridge.h"

#include <chrono>
#include <mutex>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <libavcodec/packet.h>
}

namespace ffplayer {

using namespace session_detail;

void PlayerSession::presentVideoFrame(AVFrame* frame, MediaTimeMs pts, std::uint64_t epoch)
{
    if (!frame || !videoSink_ || !epochIsCurrent(epoch)) return;
    std::lock_guard<std::mutex> presentationLock(videoPresentationMutex_);
    if (!epochIsCurrent(epoch)) return;

    if (isHardwarePixelFormat(frame->format) && videoSink_->supportsHwVideo() &&
        canPresentHwVideoFrame(frame)) {
        HwVideoFrame hw;
        if (makeHwVideoFrame(frame, pts, hw) && epochIsCurrent(epoch)) {
            videoSink_->onHwVideoFrame(hw);
            return;
        }
    }

    AVFrame* ownedTransfer = nullptr;
    AVFrame* yuv = frame;
    if (isHardwarePixelFormat(frame->format)) {
        AVFrame* sw = videoFramePool_.acquire();
        if (!sw || av_hwframe_transfer_data(sw, frame, 0) < 0) {
            if (sw) videoFramePool_.release(sw);
            return;
        }
        av_frame_copy_props(sw, frame);
        ownedTransfer = sw;
        yuv = sw;
    }

    const VideoPixelFormat outputFormat = videoSink_->preferredSoftwarePixelFormat();
    const AVPixelFormat outputAvFormat = outputFormat == VideoPixelFormat::Bgra32
        ? AV_PIX_FMT_BGRA : AV_PIX_FMT_YUV420P;

    AVFrame* converted = nullptr;
    if (yuv->format != outputAvFormat) {
        if (!scaler_ || scalerSrcW_ != yuv->width || scalerSrcH_ != yuv->height ||
            scalerSrcFormat_ != yuv->format || scalerDstFormat_ != outputAvFormat) {
            resetScaler();
            scaler_ = sws_getContext(
                yuv->width, yuv->height, static_cast<AVPixelFormat>(yuv->format),
                yuv->width, yuv->height, outputAvFormat,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            scalerSrcW_ = yuv->width;
            scalerSrcH_ = yuv->height;
            scalerSrcFormat_ = yuv->format;
            scalerDstFormat_ = outputAvFormat;
        }
        converted = scalerFramePool_.acquire(yuv->width, yuv->height, outputAvFormat);
        if (!converted || !scaler_ ||
            sws_scale(scaler_, yuv->data, yuv->linesize, 0, yuv->height,
                      converted->data, converted->linesize) <= 0) {
            if (converted) scalerFramePool_.release(converted);
            if (ownedTransfer) videoFramePool_.release(ownedTransfer);
            return;
        }
        yuv = converted;
    }

    if (!epochIsCurrent(epoch)) {
        if (converted) scalerFramePool_.release(converted);
        if (ownedTransfer) videoFramePool_.release(ownedTransfer);
        return;
    }

    VideoFrame out;
    for (int i = 0; i < 3; ++i) {
        out.data[i] = yuv->data[i];
        out.linesize[i] = yuv->linesize[i];
    }
    out.width = yuv->width;
    out.height = yuv->height;
    out.format = outputFormat;
    out.ptsMs = pts;
    videoSink_->onVideoFrame(out);
    if (converted) scalerFramePool_.release(converted);
    if (ownedTransfer) videoFramePool_.release(ownedTransfer);
}

bool PlayerSession::decodePausedVideoPreview(MediaTimeMs targetMs, std::uint64_t epoch)
{
    if (!videoSink_ || demuxer_.videoStream() < 0 || !epochIsCurrent(epoch)) return false;

    const bool preferHwSurface = videoSink_->supportsHwVideo() && videoDecoder_.hwAccelActive();
    constexpr int kMaxPreviewPackets = 512;

    auto consumeFrames = [&]() -> bool {
        for (;;) {
            AVFrame* frame = nullptr;
            {
                std::lock_guard<std::mutex> lock(videoCodecMutex_);
                frame = videoDecoder_.receive(preferHwSurface);
            }
            if (!frame) return false;
            if (!epochIsCurrent(epoch)) {
                releaseVideoFrame(frame);
                return false;
            }

            const MediaTimeMs pts = frame->pts < 0 ? targetMs : frame->pts;
            if (pts < targetMs) {
                releaseVideoFrame(frame);
                continue;
            }

            videoClockMs_ = pts;
            MediaTimeMs expected = targetMs;
            videoSeekTargetMs_.compare_exchange_strong(
                expected, -1, std::memory_order_acq_rel);
            presentVideoFrame(frame, pts, epoch);
            releaseVideoFrame(frame);
            return true;
        }
    };

    for (int count = 0; count < kMaxPreviewPackets && epochIsCurrent(epoch); ++count) {
        AVPacket* packet = nullptr;
        int videoStream = -1;
        int audioStream = -1;
        int subtitleStream = -1;
        {
            std::lock_guard<std::mutex> lock(demuxMutex_);
            packet = demuxer_.read();
            videoStream = demuxer_.videoStream();
            audioStream = demuxer_.audioStream();
            subtitleStream = demuxer_.subtitleStream();
        }
        if (!packet) return consumeFrames();

        const std::size_t packetBytes = packet->size > 0
            ? static_cast<std::size_t>(packet->size) : 1;
        if (packet->stream_index == audioStream) {
            if (!audioPackets_.tryPush(packet, packetBytes)) freePacket(packet);
            continue;
        }
        if (packet->stream_index == subtitleStream) {
            if (!subtitlePump_.load() || !subtitlePackets_.tryPush(packet, packetBytes)) {
                freePacket(packet);
            }
            continue;
        }
        if (packet->stream_index != videoStream) {
            freePacket(packet);
            continue;
        }

        int sendResult = 0;
        {
            std::lock_guard<std::mutex> lock(videoCodecMutex_);
            sendResult = videoDecoder_.send(packet);
        }
        if (sendResult == AVERROR(EAGAIN)) {
            if (consumeFrames()) {
                freePacket(packet);
                return true;
            }
            {
                std::lock_guard<std::mutex> lock(videoCodecMutex_);
                sendResult = videoDecoder_.send(packet);
            }
        }
        freePacket(packet);
        if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) continue;
        if (consumeFrames()) return true;
    }
    return false;
}

bool PlayerSession::handleVideoFrame(AVFrame* frame, std::uint64_t epoch)
{
    if (!frame) return false;
    if (!epochIsCurrent(epoch)) {
        releaseVideoFrame(frame);
        return false;
    }
    if (paused_.load(std::memory_order_acquire)) {
        waitWhilePaused();
    }
    if (stopRequested_ || !epochIsCurrent(epoch)) {
        releaseVideoFrame(frame);
        return false;
    }
    const MediaTimeMs pts = frame->pts < 0 ? videoClockMs_.load() : frame->pts;
    MediaTimeMs seekTarget = videoSeekTargetMs_.load(std::memory_order_acquire);
    if (seekTarget >= 0 && pts < seekTarget) {
        releaseVideoFrame(frame);
        return true;
    }
    if (seekTarget >= 0) {
        videoSeekTargetMs_.compare_exchange_strong(
            seekTarget, -1, std::memory_order_acq_rel);
    }
    videoClockMs_ = pts;
    const MediaTimeMs master = masterClockMs();

    bool catchUp = videoCatchUp_.load(std::memory_order_acquire);
    if (shouldDropVideoFrame(pts, master, catchUp)) {
        videoCatchUp_.store(catchUp, std::memory_order_release);
        releaseVideoFrame(frame);
        return true;
    }
    videoCatchUp_.store(catchUp, std::memory_order_release);

    if (master >= 0 && pts > master + kSyncEarlyMs) {
        const MediaTimeMs target = pts - kSyncEarlyMs / 2;
        waitForMasterClock(target, epoch);
        if (!epochIsCurrent(epoch) || stopRequested_ || paused_) {
            releaseVideoFrame(frame);
            return false;
        }
        const MediaTimeMs refreshed = masterClockMs();
        catchUp = videoCatchUp_.load(std::memory_order_acquire);
        if (shouldDropVideoFrame(pts, refreshed, catchUp)) {
            videoCatchUp_.store(catchUp, std::memory_order_release);
            releaseVideoFrame(frame);
            return true;
        }
        videoCatchUp_.store(catchUp, std::memory_order_release);
    }

    presentVideoFrame(frame, pts, epoch);
    releaseVideoFrame(frame);
    return true;
}

void PlayerSession::videoLoop()
{
    while (!stopRequested_) {
        waitWhilePaused();
        if (stopRequested_) break;

        const bool preferHwSurface =
            videoSink_ && videoSink_->supportsHwVideo() && videoDecoder_.hwAccelActive();

        AVPacket* packet = nullptr;
        if (!videoPackets_.pop(packet)) break;

        const std::uint64_t epoch = seekEpoch_.load();
        if (!packet) {
            if (!epochIsCurrent(epoch) || !demuxAtEof_.load(std::memory_order_acquire)) continue;
            {
                std::lock_guard<std::mutex> lock(videoCodecMutex_);
                videoDecoder_.sendEndOfStream();
            }
            for (;;) {
                AVFrame* frame = nullptr;
                {
                    std::lock_guard<std::mutex> lock(videoCodecMutex_);
                    frame = videoDecoder_.receive(preferHwSurface);
                }
                if (!frame) break;
                if (epochIsCurrent(epoch)) handleVideoFrame(frame, epoch);
                else releaseVideoFrame(frame);
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
                std::unique_lock<std::mutex> lock(playbackMutex_);
                playbackCv_.wait_for(lock, std::chrono::milliseconds(2));
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

} // namespace ffplayer
