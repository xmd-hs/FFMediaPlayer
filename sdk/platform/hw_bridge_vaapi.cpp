#include "hw_bridge_ops.h"

#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}

namespace ffplayer {
namespace detail {
namespace {

constexpr std::uint32_t fourcc(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(a) |
           (static_cast<std::uint32_t>(b) << 8) |
           (static_cast<std::uint32_t>(c) << 16) |
           (static_cast<std::uint32_t>(d) << 24);
}

bool isSupportedDrmFormat(std::uint32_t format)
{
    return format == fourcc('N', 'V', '1', '2') ||
           format == fourcc('N', 'V', '2', '1') ||
           format == fourcc('P', '0', '1', '0') ||
           format == fourcc('A', 'R', '2', '4') ||
           format == fourcc('X', 'R', '2', '4') ||
           format == fourcc('A', 'B', '2', '4') ||
           format == fourcc('X', 'B', '2', '4');
}

bool drmDescriptorPresentable(const AVDRMFrameDescriptor* desc)
{
    if (!desc || desc->nb_layers <= 0 || desc->nb_objects <= 0) return false;
    if (desc->objects[0].fd < 0) return false;
    return isSupportedDrmFormat(desc->layers[0].format);
}

AVFrame* mapToDrmPrime(AVFrame* src)
{
    if (!src) return nullptr;
    if (src->format == AV_PIX_FMT_DRM_PRIME) {
        AVFrame* held = av_frame_alloc();
        if (!held || av_frame_ref(held, src) < 0) {
            av_frame_free(&held);
            return nullptr;
        }
        return held;
    }

    AVFrame* mapped = av_frame_alloc();
    if (!mapped) return nullptr;
    mapped->format = AV_PIX_FMT_DRM_PRIME;
    if (av_hwframe_map(mapped, src, 0) < 0) {
        av_frame_free(&mapped);
        return nullptr;
    }
    return mapped;
}

struct DrmFrameKeepAlive {
    AVFrame* mapped = nullptr;
    AVFrame* source = nullptr;

    ~DrmFrameKeepAlive()
    {
        av_frame_free(&mapped);
        av_frame_free(&source);
    }
};

bool makeVaapi(AVFrame* frame, MediaTimeMs ptsMs, HwVideoFrame& out)
{
    if (!frame) return false;
    if (frame->format != AV_PIX_FMT_VAAPI && frame->format != AV_PIX_FMT_DRM_PRIME) return false;

    AVFrame* mapped = mapToDrmPrime(frame);
    if (!mapped || !mapped->data[0]) {
        av_frame_free(&mapped);
        return false;
    }
    auto* desc = reinterpret_cast<AVDRMFrameDescriptor*>(mapped->data[0]);
    if (!drmDescriptorPresentable(desc)) {
        av_frame_free(&mapped);
        return false;
    }

    auto* holder = new (std::nothrow) DrmFrameKeepAlive();
    if (!holder) {
        av_frame_free(&mapped);
        return false;
    }
    holder->mapped = mapped;
    holder->source = av_frame_alloc();
    if (!holder->source || av_frame_ref(holder->source, frame) < 0) {
        // Destructor frees mapped (+ source if allocated).
        delete holder;
        return false;
    }

    out.backend = HwVideoBackend::VAAPI;
    out.nativeHandle = holder->mapped->data[0]; // AVDRMFrameDescriptor*
    out.keepAlive = std::shared_ptr<void>(holder, [](void* p) {
        delete static_cast<DrmFrameKeepAlive*>(p);
    });
    out.width = frame->width;
    out.height = frame->height;
    out.ptsMs = ptsMs;
    return true;
}

bool canPresentVaapi(AVFrame* frame)
{
    if (!frame) return false;
    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        auto* desc = reinterpret_cast<AVDRMFrameDescriptor*>(frame->data[0]);
        return drmDescriptorPresentable(desc);
    }
    if (frame->format == AV_PIX_FMT_VAAPI) {
        return frame->data[3] != nullptr;
    }
    return false;
}

int preferredVaapi()
{
    return static_cast<int>(AV_HWDEVICE_TYPE_VAAPI);
}

const HwBridgeOps kOps{makeVaapi, canPresentVaapi, preferredVaapi, nullptr};

} // namespace

const HwBridgeOps* hwBridgeOps()
{
    return &kOps;
}

} // namespace detail
} // namespace ffplayer
