#include "hw_bridge_ops.h"

#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <d3d11.h>

namespace ffplayer {
namespace detail {
namespace {

bool makeD3d11(AVFrame* frame, MediaTimeMs ptsMs, HwVideoFrame& out)
{
    if (!frame || frame->format != AV_PIX_FMT_D3D11) return false;
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    if (!texture) return false;

    AVFrame* held = av_frame_alloc();
    if (!held || av_frame_ref(held, frame) < 0) {
        av_frame_free(&held);
        return false;
    }

    out.backend = HwVideoBackend::D3D11;
    out.nativeHandle = texture;
    out.subresourceIndex = static_cast<int>(reinterpret_cast<intptr_t>(frame->data[1]));
    out.keepAlive = std::shared_ptr<void>(held, [](void* p) {
        auto* f = static_cast<AVFrame*>(p);
        av_frame_free(&f);
    });
    out.width = frame->width;
    out.height = frame->height;
    out.ptsMs = ptsMs;
    return true;
}

bool canPresentD3d11(AVFrame* frame)
{
    if (!frame || frame->format != AV_PIX_FMT_D3D11) return false;
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    if (!texture) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    return desc.Format == DXGI_FORMAT_NV12 ||
           desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
}

int preferredD3d11()
{
    return static_cast<int>(AV_HWDEVICE_TYPE_D3D11VA);
}

const HwBridgeOps kOps{makeD3d11, canPresentD3d11, preferredD3d11};

} // namespace

const HwBridgeOps* hwBridgeOps()
{
    return &kOps;
}

} // namespace detail
} // namespace ffplayer
