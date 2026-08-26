#include "ffmpeg_demuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace ffplayer {

namespace {
std::string errorText(int code)
{
    char buffer[256]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

AVCodecParameters *copyParameters(AVFormatContext *format, int stream)
{
    if (!format || stream < 0)
        return nullptr;
    auto *parameters = avcodec_parameters_alloc();
    if (!parameters || avcodec_parameters_copy(parameters, format->streams[stream]->codecpar) < 0) {
        avcodec_parameters_free(&parameters);
        return nullptr;
    }
    return parameters;
}
}

FfmpegDemuxer::~FfmpegDemuxer() { close(); }

bool FfmpegDemuxer::open(const std::string &url)
{
    close();
    if (url.empty() || avformat_open_input(&format_, url.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(format_, nullptr) < 0) { close(); return false; }
    videoStream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audioStream_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    subtitleStream_ = av_find_best_stream(format_, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
    durationMs_ = format_->duration > 0 ? av_rescale_q(format_->duration, AV_TIME_BASE_Q, {1, 1000}) : 0;
    return videoStream_ >= 0 || audioStream_ >= 0;
}

void FfmpegDemuxer::close()
{
    if (format_) avformat_close_input(&format_);
    videoStream_ = audioStream_ = subtitleStream_ = -1;
    durationMs_ = 0;
}

AVPacket *FfmpegDemuxer::read()
{
    if (!format_) return nullptr;
    auto *packet = av_packet_alloc();
    if (!packet) return nullptr;
    const int result = av_read_frame(format_, packet);
    if (result < 0) { av_packet_free(&packet); if (result != AVERROR_EOF) lastError_ = errorText(result); return nullptr; }
    const auto timeBase = format_->streams[packet->stream_index]->time_base;
    if (packet->pts != AV_NOPTS_VALUE) packet->pts = av_rescale_q(packet->pts, timeBase, {1, 1000});
    if (packet->dts != AV_NOPTS_VALUE) packet->dts = av_rescale_q(packet->dts, timeBase, {1, 1000});
    return packet;
}

bool FfmpegDemuxer::seek(MediaTimeMs position)
{
    if (!format_) return false;
    const int stream = videoStream_ >= 0 ? videoStream_ : audioStream_;
    if (stream < 0 || av_seek_frame(format_, stream, av_rescale_q(position, {1, 1000}, format_->streams[stream]->time_base), AVSEEK_FLAG_BACKWARD) < 0)
        return false;
    avformat_flush(format_);
    return true;
}

bool FfmpegDemuxer::selectAudioTrack(int stream) { if (!format_ || stream < 0 || stream >= static_cast<int>(format_->nb_streams) || format_->streams[stream]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) return false; audioStream_ = stream; return true; }
bool FfmpegDemuxer::selectSubtitleTrack(int stream) { if (stream == -1) { subtitleStream_ = -1; return true; } if (!format_ || stream < 0 || stream >= static_cast<int>(format_->nb_streams) || format_->streams[stream]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) return false; subtitleStream_ = stream; return true; }
AVCodecParameters *FfmpegDemuxer::videoParameters() const { return copyParameters(format_, videoStream_); }
AVCodecParameters *FfmpegDemuxer::audioParameters() const { return copyParameters(format_, audioStream_); }
AVCodecParameters *FfmpegDemuxer::subtitleParameters() const { return copyParameters(format_, subtitleStream_); }

std::vector<TrackInfo> FfmpegDemuxer::audioTracks() const
{
    std::vector<TrackInfo> tracks;
    if (!format_) return tracks;
    for (unsigned int i = 0; i < format_->nb_streams; ++i) {
        if (format_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            TrackInfo track;
            track.streamIndex = static_cast<int>(i);
            track.codec = avcodec_get_name(format_->streams[i]->codecpar->codec_id);
            tracks.push_back(track);
        }
    }
    return tracks;
}

std::vector<TrackInfo> FfmpegDemuxer::subtitleTracks() const
{
    std::vector<TrackInfo> tracks;
    if (!format_) return tracks;
    for (unsigned int i = 0; i < format_->nb_streams; ++i) {
        if (format_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            TrackInfo track;
            track.streamIndex = static_cast<int>(i);
            track.codec = avcodec_get_name(format_->streams[i]->codecpar->codec_id);
            tracks.push_back(track);
        }
    }
    return tracks;
}

} // namespace ffplayer
