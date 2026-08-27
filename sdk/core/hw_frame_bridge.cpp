#include "hw_frame_bridge.h"
#include "hw_bridge_ops.h"

extern "C" {
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace ffplayer {

bool isHardwarePixelFormat(int avPixelFormat)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(avPixelFormat));
    return desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
}

bool makeHwVideoFrame(AVFrame* frame, MediaTimeMs ptsMs, HwVideoFrame& out)
{
    out = {};
    if (!frame || !isHardwarePixelFormat(frame->format)) return false;
    const detail::HwBridgeOps* ops = detail::hwBridgeOps();
    if (!ops || !ops->make) return false;
    return ops->make(frame, ptsMs, out);
}

bool canPresentHwVideoFrame(AVFrame* frame)
{
    if (!frame || !isHardwarePixelFormat(frame->format)) return false;
    const detail::HwBridgeOps* ops = detail::hwBridgeOps();
    if (!ops || !ops->canPresent) return false;
    return ops->canPresent(frame);
}

} // namespace ffplayer
