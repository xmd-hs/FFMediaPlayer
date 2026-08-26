#include "ffmpeg_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
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
}

FfmpegDecoder::~FfmpegDecoder() { close(); }

bool FfmpegDecoder::open(const AVCodecParameters* parameters)
{
    close();
    if (!parameters) { lastError_ = "null codec parameters"; return false; }
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) { lastError_ = "decoder not found"; return false; }
    context_ = avcodec_alloc_context3(codec);
    if (!context_) { lastError_ = "decoder context allocation failed"; return false; }
    int result = avcodec_parameters_to_context(context_, parameters);
    if (result < 0) { lastError_ = "copy codec parameters failed"; close(); return false; }
    // Demuxer normalizes packet timestamps to milliseconds.
    context_->pkt_timebase = AVRational{1, 1000};
    result = avcodec_open2(context_, codec, nullptr);
    if (result < 0) { lastError_ = "decoder open failed: " + errorText(result); close(); return false; }
    lastError_.clear();
    return true;
}

void FfmpegDecoder::close()
{
    if (context_) avcodec_free_context(&context_);
}

bool FfmpegDecoder::send(AVPacket* packet)
{
    if (!context_ || !packet) return false;
    const int result = avcodec_send_packet(context_, packet);
    av_packet_free(&packet); // Decoder takes a copy; ownership ends here.
    return result == 0 || result == AVERROR(EAGAIN);
}

AVFrame* FfmpegDecoder::receive()
{
    if (!context_) return nullptr;
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;
    if (avcodec_receive_frame(context_, frame) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }
    if (frame->pts == AV_NOPTS_VALUE) frame->pts = frame->best_effort_timestamp;
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
