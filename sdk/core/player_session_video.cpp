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
}

namespace ffplayer {

using namespace session_detail;

void PlayerSession::presentVideoFrame(AVFrame* frame, MediaTimeMs pts, std::uint64_t epoch)
{
    if (!frame || !videoSink_ || !epochIsCurrent(epoch)) return;

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
        converted = scalerFramePool_.acquire(yuv->width, yuv->height, AV_PIX_FMT_YUV420P);
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
    out.ptsMs = pts;
    videoSink_->onVideoFrame(out);
    if (converted) scalerFramePool_.release(converted);
    if (ownedTransfer) videoFramePool_.release(ownedTransfer);
}

bool PlayerSession::handleVideoFrame(AVFrame* frame, std::uint64_t epoch)
{
    if (!frame) return false;
    if (!epochIsCurrent(epoch)) {
        releaseVideoFrame(frame);
        return false;
    }
    if (paused_.load(std::memory_order_acquire) &&
        eofDrainActive_.load(std::memory_order_acquire)) {
        releaseVideoFrame(frame);
        return true;
    }
    const MediaTimeMs pts = frame->pts < 0 ? videoClockMs_.load() : frame->pts;
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
            std::vector<AVFrame*> drained;
            {
                std::lock_guard<std::mutex> lock(videoCodecMutex_);
                videoDecoder_.sendEndOfStream();
                while (AVFrame* frame = videoDecoder_.receive(preferHwSurface)) drained.push_back(frame);
            }
            for (AVFrame* frame : drained) {
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
