#include "hw_frame_bridge.h"

#include <memory>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#endif

#if defined(_WIN32)
#include <d3d11.h>
#endif

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

#if defined(__APPLE__)
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
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
#endif

#if defined(_WIN32)
    if (frame->format == AV_PIX_FMT_D3D11) {
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
#endif

    (void)ptsMs;
    return false;
}

bool canPresentHwVideoFrame(AVFrame* frame)
{
    if (!frame || !isHardwarePixelFormat(frame->format)) return false;

#if defined(__APPLE__)
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        auto* pixbuf = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
        if (!pixbuf) return false;
        const OSType fmt = CVPixelBufferGetPixelFormatType(pixbuf);
        return fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
               fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
               fmt == kCVPixelFormatType_32BGRA;
    }
#endif

#if defined(_WIN32)
    if (frame->format == AV_PIX_FMT_D3D11) {
        auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        if (!texture) return false;
        D3D11_TEXTURE2D_DESC desc = {};
        texture->GetDesc(&desc);
        return desc.Format == DXGI_FORMAT_NV12 ||
               desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
               desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
    }
#endif

    return false;
}

} // namespace ffplayer
