#pragma once

#include "../include/player_types.h"
#include <string>
#include <cstdint>
#include <vector>

struct AVFormatContext;
struct AVPacket;
struct AVCodecParameters;

namespace ffplayer {

class FfmpegDemuxer {
public:
    FfmpegDemuxer() = default;
    ~FfmpegDemuxer();

    FfmpegDemuxer(const FfmpegDemuxer&) = delete;
    FfmpegDemuxer& operator=(const FfmpegDemuxer&) = delete;

    bool open(const std::string& url);
    void close();
    AVPacket* read();
    bool seek(MediaTimeMs positionMs);
    bool selectAudioTrack(int streamIndex);
    bool selectSubtitleTrack(int streamIndex);
    AVCodecParameters* videoParameters() const;
    AVCodecParameters* audioParameters() const;
    AVCodecParameters* subtitleParameters() const;
    std::vector<TrackInfo> audioTracks() const;
    std::vector<TrackInfo> subtitleTracks() const;

    MediaTimeMs duration() const { return durationMs_; }
    int videoStream() const { return videoStream_; }
    int audioStream() const { return audioStream_; }
    int subtitleStream() const { return subtitleStream_; }
    const std::string& lastError() const { return lastError_; }

private:
    AVFormatContext* format_ = nullptr;
    MediaTimeMs durationMs_ = 0;
    int videoStream_ = -1;
    int audioStream_ = -1;
    int subtitleStream_ = -1;
    std::string lastError_;
};

} // namespace ffplayer
