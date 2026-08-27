#include "hw_bridge_ops.h"

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <d3d11.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

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
    return (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
        (desc.Format == DXGI_FORMAT_NV12 ||
        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM);
}

bool configureD3d11Frames(AVCodecContext* codec, int pixelFormat)
{
    if (!codec || pixelFormat != AV_PIX_FMT_D3D11 || !codec->hw_device_ctx) return false;

    AVBufferRef* framesRef = nullptr;
    if (avcodec_get_hw_frames_parameters(codec, codec->hw_device_ctx,
                                         AV_PIX_FMT_D3D11, &framesRef) < 0 || !framesRef) {
        av_buffer_unref(&framesRef);
        return false;
    }
    auto* frames = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
    if (!frames || !frames->hwctx) {
        av_buffer_unref(&framesRef);
        return false;
    }
    auto* d3dFrames = reinterpret_cast<AVD3D11VAFramesContext*>(frames->hwctx);
    d3dFrames->BindFlags |= D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    if (av_hwframe_ctx_init(framesRef) < 0) {
        av_buffer_unref(&framesRef);
        return false;
    }
    codec->hw_frames_ctx = framesRef;
    return true;
}

int preferredD3d11()
{
    return static_cast<int>(AV_HWDEVICE_TYPE_D3D11VA);
}

const HwBridgeOps kOps{makeD3d11, canPresentD3d11, preferredD3d11, configureD3d11Frames};

} // namespace

const HwBridgeOps* hwBridgeOps()
{
    return &kOps;
}

} // namespace detail
} // namespace ffplayer
