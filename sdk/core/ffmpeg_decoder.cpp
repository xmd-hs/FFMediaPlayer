#include "ffmpeg_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace ffplayer {

namespace {

std::string errorText(int code)
{
    char buffer[256]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// Prefer the platform's primary decoder; others are attempted as fallbacks.
AVHWDeviceType preferredHwDeviceType()
{
#if defined(__APPLE__)
    return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(_WIN32)
    return AV_HWDEVICE_TYPE_D3D11VA;
#else
    // Broad desktop Linux default; CUDA/Vulkan still tried if advertised by the codec.
    return AV_HWDEVICE_TYPE_VAAPI;
#endif
}

bool isHwPixelFormat(int format)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
    return desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
}

enum AVPixelFormat getHwFormat(AVCodecContext* ctx, const enum AVPixelFormat* pixFmts)
{
    const auto* self = static_cast<const FfmpegDecoder*>(ctx->opaque);
    const auto target = self ? static_cast<AVPixelFormat>(self->hwPixelFormat()) : AV_PIX_FMT_NONE;
    for (const enum AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == target) return *p;
    }
    return AV_PIX_FMT_NONE;
}

} // namespace

FfmpegDecoder::~FfmpegDecoder() { close(); }

FfmpegDecoder::FfmpegDecoder(FfmpegDecoder&& other) noexcept
    : context_(other.context_)
    , hwDeviceCtx_(other.hwDeviceCtx_)
    , hwPixFmt_(other.hwPixFmt_)
    , hwActive_(other.hwActive_)
    , lastError_(std::move(other.lastError_))
{
    other.context_ = nullptr;
    other.hwDeviceCtx_ = nullptr;
    other.hwPixFmt_ = AV_PIX_FMT_NONE;
    other.hwActive_ = false;
}

FfmpegDecoder& FfmpegDecoder::operator=(FfmpegDecoder&& other) noexcept
{
    if (this != &other) {
        close();
        context_ = other.context_;
        hwDeviceCtx_ = other.hwDeviceCtx_;
        hwPixFmt_ = other.hwPixFmt_;
        hwActive_ = other.hwActive_;
        lastError_ = std::move(other.lastError_);
        other.context_ = nullptr;
        other.hwDeviceCtx_ = nullptr;
        other.hwPixFmt_ = AV_PIX_FMT_NONE;
        other.hwActive_ = false;
    }
    return *this;
}

bool FfmpegDecoder::initContext(const AVCodecParameters* parameters, const AVCodec* codec)
{
    context_ = avcodec_alloc_context3(codec);
    if (!context_) {
        lastError_ = "decoder context allocation failed";
        return false;
    }
    if (avcodec_parameters_to_context(context_, parameters) < 0) {
        lastError_ = "copy codec parameters failed";
        return false;
    }
    // Demuxer normalizes packet timestamps to milliseconds.
    context_->pkt_timebase = AVRational{1, 1000};
    return true;
}

bool FfmpegDecoder::tryOpenHardware(const AVCodecParameters* parameters, const AVCodec* codec)
{
    if (parameters->codec_type != AVMEDIA_TYPE_VIDEO) return false;

    const AVHWDeviceType preferred = preferredHwDeviceType();

    // Collect candidate (device_type, pix_fmt) pairs: preferred first, then others.
    struct Candidate {
        AVHWDeviceType type;
        AVPixelFormat pixFmt;
    };
    Candidate candidates[16];
    int candidateCount = 0;
    Candidate preferredCandidates[8];
    int preferredCount = 0;

    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) break;
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
        Candidate c{config->device_type, config->pix_fmt};
        if (config->device_type == preferred) {
            if (preferredCount < 8) preferredCandidates[preferredCount++] = c;
        } else if (candidateCount < 16) {
            candidates[candidateCount++] = c;
        }
    }

    auto tryOne = [&](AVHWDeviceType type, AVPixelFormat pixFmt) -> bool {
        if (type == AV_HWDEVICE_TYPE_NONE) return false;
        close();
        if (!initContext(parameters, codec)) {
            close();
            return false;
        }

        AVBufferRef* device = nullptr;
        const int createResult = av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0);
        if (createResult < 0) {
            lastError_ = "hw device create failed: " + errorText(createResult);
            close();
            return false;
        }

        hwDeviceCtx_ = device;
        hwPixFmt_ = pixFmt;
        context_->opaque = this;
        context_->get_format = getHwFormat;
        context_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
        if (!context_->hw_device_ctx) {
            lastError_ = "hw_device_ctx ref failed";
            close();
            return false;
        }

        const int openResult = avcodec_open2(context_, codec, nullptr);
        if (openResult < 0) {
            lastError_ = "hw decoder open failed: " + errorText(openResult);
            close();
            return false;
        }

        hwActive_ = true;
        lastError_.clear();
        return true;
    };

    for (int i = 0; i < preferredCount; ++i) {
        if (tryOne(preferredCandidates[i].type, preferredCandidates[i].pixFmt)) return true;
    }
    for (int i = 0; i < candidateCount; ++i) {
        if (tryOne(candidates[i].type, candidates[i].pixFmt)) return true;
    }
    return false;
}

bool FfmpegDecoder::openSoftware(const AVCodecParameters* parameters, const AVCodec* codec)
{
    close();
    if (!initContext(parameters, codec)) {
        close();
        return false;
    }
    const int result = avcodec_open2(context_, codec, nullptr);
    if (result < 0) {
        lastError_ = "decoder open failed: " + errorText(result);
        close();
        return false;
    }
    hwActive_ = false;
    lastError_.clear();
    return true;
}

bool FfmpegDecoder::open(const AVCodecParameters* parameters, bool enableHwAccel)
{
    close();
    if (!parameters) {
        lastError_ = "null codec parameters";
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) {
        lastError_ = "decoder not found";
        return false;
    }

    if (enableHwAccel && tryOpenHardware(parameters, codec)) return true;

    // Soft decode (also the fallback when hardware is unavailable).
    return openSoftware(parameters, codec);
}

void FfmpegDecoder::close()
{
    if (context_) {
        context_->opaque = nullptr;
        context_->get_format = nullptr;
        avcodec_free_context(&context_);
    }
    av_buffer_unref(&hwDeviceCtx_);
    hwPixFmt_ = AV_PIX_FMT_NONE;
    hwActive_ = false;
}

int FfmpegDecoder::send(AVPacket* packet)
{
    if (!context_ || !packet) return AVERROR(EINVAL);
    return avcodec_send_packet(context_, packet);
}

AVFrame* FfmpegDecoder::transferToSystemMemory(AVFrame* hwFrame)
{
    AVFrame* swFrame = av_frame_alloc();
    if (!swFrame) {
        av_frame_free(&hwFrame);
        return nullptr;
    }
    if (av_hwframe_transfer_data(swFrame, hwFrame, 0) < 0) {
        av_frame_free(&swFrame);
        av_frame_free(&hwFrame);
        return nullptr;
    }
    if (av_frame_copy_props(swFrame, hwFrame) < 0) {
        av_frame_free(&swFrame);
        av_frame_free(&hwFrame);
        return nullptr;
    }
    av_frame_free(&hwFrame);
    if (swFrame->pts == AV_NOPTS_VALUE) swFrame->pts = swFrame->best_effort_timestamp;
    return swFrame;
}

AVFrame* FfmpegDecoder::receive(bool preferHwSurface)
{
    if (!context_) return nullptr;
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;
    if (avcodec_receive_frame(context_, frame) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    if (frame->pts == AV_NOPTS_VALUE) frame->pts = frame->best_effort_timestamp;

    if (isHwPixelFormat(frame->format) && !preferHwSurface) {
        return transferToSystemMemory(frame);
    }
    return frame;
}

void FfmpegDecoder::flush()
{
    if (context_) avcodec_flush_buffers(context_);
}

bool FfmpegDecoder::sendEndOfStream()
{
    return context_ && avcodec_send_packet(context_, nullptr) >= 0;
}

} // namespace ffplayer
