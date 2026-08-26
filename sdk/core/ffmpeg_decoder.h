#pragma once
#include <string>
struct AVCodecContext; struct AVCodecParameters; struct AVPacket; struct AVFrame;
namespace ffplayer {
class FfmpegDecoder {
public:
    FfmpegDecoder() = default; ~FfmpegDecoder();
    FfmpegDecoder(const FfmpegDecoder&) = delete; FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;
    bool open(const AVCodecParameters*); void close(); bool send(AVPacket*); AVFrame* receive(); void flush(); bool sendEndOfStream();
    const std::string& lastError() const { return lastError_; }
private: AVCodecContext* context_ = nullptr; std::string lastError_;
}; }
