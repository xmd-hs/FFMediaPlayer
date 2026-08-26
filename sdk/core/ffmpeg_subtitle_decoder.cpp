#include "ffmpeg_subtitle_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace ffplayer {

FfmpegSubtitleDecoder::~FfmpegSubtitleDecoder()
{
    close();
}

bool FfmpegSubtitleDecoder::open(const AVCodecParameters *parameters)
{
    close();
    if (!parameters)
        return false;

    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec)
        return false;

    context_ = avcodec_alloc_context3(codec);
    if (!context_ || avcodec_parameters_to_context(context_, parameters) < 0 ||
        avcodec_open2(context_, codec, nullptr) < 0) {
        close();
        return false;
    }
    return true;
}

void FfmpegSubtitleDecoder::close()
{
    if (context_)
        avcodec_free_context(&context_);
}

bool FfmpegSubtitleDecoder::decode(AVPacket *packet, std::string &text,
                                   MediaTimeMs &start, MediaTimeMs &end)
{
    if (!context_ || !packet)
        return false;

    AVSubtitle subtitle{};
    int gotSubtitle = 0;
    const int result = avcodec_decode_subtitle2(context_, &subtitle, &gotSubtitle, packet);
    start = packet->pts >= 0 ? packet->pts : 0;
    end = start + (packet->duration > 0 ? packet->duration : 3000);
    if (result < 0 || !gotSubtitle) {
        avsubtitle_free(&subtitle);
        return false;
    }

    for (unsigned int index = 0; index < subtitle.num_rects; ++index) {
        const AVSubtitleRect *rect = subtitle.rects[index];
        const char *line = rect && rect->text ? rect->text : rect && rect->ass ? rect->ass : nullptr;
        if (line) {
            if (!text.empty())
                text.push_back('\n');
            text += line;
        }
    }
    avsubtitle_free(&subtitle);
    return !text.empty();
}

} // namespace ffplayer
