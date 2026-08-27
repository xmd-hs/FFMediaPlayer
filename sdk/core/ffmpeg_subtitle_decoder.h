#pragma once
#include "../include/player_types.h"
#include <string>
#include <vector>

struct AVCodecContext;
struct AVCodecParameters;
struct AVPacket;

namespace ffplayer {

struct DecodedSubtitle {
    std::string text;
    std::vector<SubtitleImage> images;
    MediaTimeMs startMs = 0;
    MediaTimeMs endMs = 0;
    bool hasText = false;
};

class FfmpegSubtitleDecoder {
public:
    FfmpegSubtitleDecoder() = default;
    ~FfmpegSubtitleDecoder();
    FfmpegSubtitleDecoder(const FfmpegSubtitleDecoder&) = delete;
    FfmpegSubtitleDecoder& operator=(const FfmpegSubtitleDecoder&) = delete;
    FfmpegSubtitleDecoder(FfmpegSubtitleDecoder&& other) noexcept;
    FfmpegSubtitleDecoder& operator=(FfmpegSubtitleDecoder&& other) noexcept;
    bool open(const AVCodecParameters* parameters);
    void close();
    void flush();
    bool decode(AVPacket* packet, DecodedSubtitle& out);

private:
    AVCodecContext* context_ = nullptr;
};

} // namespace ffplayer
