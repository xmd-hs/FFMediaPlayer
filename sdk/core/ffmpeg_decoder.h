#pragma once

#include <string>

struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;
struct AVFrame;
struct AVBufferRef;

namespace ffplayer {

class AvFramePool;

class FfmpegDecoder {
public:
    FfmpegDecoder() = default;
    ~FfmpegDecoder();

    FfmpegDecoder(const FfmpegDecoder&) = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;
    FfmpegDecoder(FfmpegDecoder&& other) noexcept;
    FfmpegDecoder& operator=(FfmpegDecoder&& other) noexcept;

    // enableHwAccel: try platform hardware decode for video; falls back to software.
    bool open(const AVCodecParameters* parameters, bool enableHwAccel = true);
    void close();

    // Does not take ownership of packet. Returns FFmpeg error code (0 = accepted).
    int send(AVPacket* packet);
    // preferHwSurface: when true, leave hardware frames on the GPU/device (phase-2).
    // when false, transfer to system memory (phase-1 behavior).
    AVFrame* receive(bool preferHwSurface = false);
    void flush();
    bool sendEndOfStream();
    void setFramePool(AvFramePool* pool) { framePool_ = pool; }
    void releaseFrame(AVFrame* frame);

    bool hwAccelActive() const { return hwActive_; }
    int hwPixelFormat() const { return hwPixFmt_; }
    const std::string& lastError() const { return lastError_; }

private:
    bool initContext(const AVCodecParameters* parameters, const AVCodec* codec);
    bool tryOpenHardware(const AVCodecParameters* parameters, const AVCodec* codec);
    bool openSoftware(const AVCodecParameters* parameters, const AVCodec* codec);
    AVFrame* transferToSystemMemory(AVFrame* hwFrame);

    AVCodecContext* context_ = nullptr;
    AVBufferRef* hwDeviceCtx_ = nullptr;
    int hwPixFmt_ = -1; // AV_PIX_FMT_NONE
    bool hwActive_ = false;
    AvFramePool* framePool_ = nullptr;
    std::string lastError_;
};

} // namespace ffplayer
