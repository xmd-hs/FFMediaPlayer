#include "hw_bridge_ops.h"

#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <CoreVideo/CoreVideo.h>

namespace ffplayer {
namespace detail {
namespace {

bool makeVt(AVFrame* frame, MediaTimeMs ptsMs, HwVideoFrame& out)
{
    if (!frame || frame->format != AV_PIX_FMT_VIDEOTOOLBOX) return false;
    auto* pixbuf = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
    if (!pixbuf) return false;
    CVBufferRetain(pixbuf);
    out.backend = HwVideoBackend::VideoToolbox;
    out.nativeHandle = pixbuf;
    out.keepAlive = std::shared_ptr<void>(pixbuf, [](void* p) {
        if (p) CVBufferRelease(static_cast<CVPixelBufferRef>(p));
    });
    out.width = frame->width;
    out.height = frame->height;
    out.ptsMs = ptsMs;
    return true;
}

bool canPresentVt(AVFrame* frame)
{
    if (!frame || frame->format != AV_PIX_FMT_VIDEOTOOLBOX) return false;
    auto* pixbuf = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
    if (!pixbuf) return false;
    const OSType fmt = CVPixelBufferGetPixelFormatType(pixbuf);
    return fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
           fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
           fmt == kCVPixelFormatType_32BGRA;
}

int preferredVt()
{
    return static_cast<int>(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
}

const HwBridgeOps kOps{makeVt, canPresentVt, preferredVt};

} // namespace

const HwBridgeOps* hwBridgeOps()
{
    return &kOps;
}

} // namespace detail
} // namespace ffplayer
