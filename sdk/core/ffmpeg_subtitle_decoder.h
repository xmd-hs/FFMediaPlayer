#pragma once
#include "../include/player_types.h"
#include <string>
struct AVCodecContext; struct AVCodecParameters; struct AVPacket;
namespace ffplayer {
class FfmpegSubtitleDecoder {
public:
    ~FfmpegSubtitleDecoder();
    bool open(const AVCodecParameters* parameters);
    void close();
    bool decode(AVPacket* packet, std::string& text, MediaTimeMs& startMs, MediaTimeMs& endMs);
private: AVCodecContext* context_ = nullptr;
};
}
