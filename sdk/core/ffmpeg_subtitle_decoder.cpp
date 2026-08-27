#include "ffmpeg_subtitle_decoder.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace ffplayer {

namespace {

std::string stripAssOverrides(std::string input)
{
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '{') {
            const auto end = input.find('}', i);
            if (end == std::string::npos) break;
            i = end;
            continue;
        }
        if (input[i] == '\\' && i + 1 < input.size()) {
            const char next = input[i + 1];
            if (next == 'N' || next == 'n') {
                out.push_back('\n');
                ++i;
                continue;
            }
            if (next == 'h') {
                out.push_back(' ');
                ++i;
                continue;
            }
        }
        out.push_back(input[i]);
    }
    return out;
}

std::string assEventToPlain(const char* ass)
{
    if (!ass) return {};
    std::string line = ass;
    // Dialogue: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text
    const auto dialogue = line.find("Dialogue:");
    if (dialogue != std::string::npos) {
        std::size_t pos = dialogue;
        int commas = 0;
        while (pos < line.size() && commas < 9) {
            if (line[pos] == ',') ++commas;
            ++pos;
        }
        if (commas >= 9) line = line.substr(pos);
    }
    line = stripAssOverrides(line);
    // Trim
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    return line.substr(start);
}

bool convertBitmapRect(const AVSubtitleRect* rect, SubtitleImage& image)
{
    if (!rect || rect->type != SUBTITLE_BITMAP || !rect->data[0] || rect->w <= 0 || rect->h <= 0) {
        return false;
    }
    const int width = rect->w;
    const int height = rect->h;
    image.width = width;
    image.height = height;
    image.x = rect->x;
    image.y = rect->y;
    image.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);

    const std::uint8_t* bitmap = rect->data[0];
    const int linesize = rect->linesize[0];
    const std::uint32_t* palette = reinterpret_cast<const std::uint32_t*>(rect->data[1]);
    const int paletteSize = rect->nb_colors > 0 ? rect->nb_colors : 256;

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = bitmap + y * linesize;
        std::uint8_t* dst = image.rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            const int index = src[x];
            std::uint32_t color = 0;
            if (palette && index >= 0 && index < paletteSize) color = palette[index];
            // FFmpeg palette is often stored as RGBA in host endianness via uint32.
            dst[x * 4 + 0] = static_cast<std::uint8_t>(color & 0xff);
            dst[x * 4 + 1] = static_cast<std::uint8_t>((color >> 8) & 0xff);
            dst[x * 4 + 2] = static_cast<std::uint8_t>((color >> 16) & 0xff);
            dst[x * 4 + 3] = static_cast<std::uint8_t>((color >> 24) & 0xff);
        }
    }
    return true;
}

} // namespace

FfmpegSubtitleDecoder::~FfmpegSubtitleDecoder()
{
    close();
}

FfmpegSubtitleDecoder::FfmpegSubtitleDecoder(FfmpegSubtitleDecoder&& other) noexcept
    : context_(other.context_)
{
    other.context_ = nullptr;
}

FfmpegSubtitleDecoder& FfmpegSubtitleDecoder::operator=(FfmpegSubtitleDecoder&& other) noexcept
{
    if (this != &other) {
        close();
        context_ = other.context_;
        other.context_ = nullptr;
    }
    return *this;
}

bool FfmpegSubtitleDecoder::open(const AVCodecParameters *parameters)
{
    close();
    if (!parameters) return false;

    const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
    if (!codec) return false;

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
    if (context_) avcodec_free_context(&context_);
}

void FfmpegSubtitleDecoder::flush()
{
    if (context_) avcodec_flush_buffers(context_);
}

bool FfmpegSubtitleDecoder::decode(AVPacket *packet, DecodedSubtitle &out)
{
    out = {};
    if (!context_ || !packet) return false;

    AVSubtitle subtitle{};
    int gotSubtitle = 0;
    const int result = avcodec_decode_subtitle2(context_, &subtitle, &gotSubtitle, packet);
    if (result < 0 || !gotSubtitle) {
        avsubtitle_free(&subtitle);
        return false;
    }

    const MediaTimeMs packetPts = packet->pts >= 0 ? packet->pts : 0;
    // start/end_display_time are milliseconds relative to the packet PTS.
    MediaTimeMs start = packetPts + static_cast<MediaTimeMs>(subtitle.start_display_time);
    MediaTimeMs end = packetPts + static_cast<MediaTimeMs>(subtitle.end_display_time);
    if (end <= start) {
        MediaTimeMs duration = packet->duration > 0 ? packet->duration : 0;
        if (duration <= 0) duration = 3000;
        end = start + duration;
    }

    out.image.startMs = start;
    out.image.endMs = end;

    for (unsigned int index = 0; index < subtitle.num_rects; ++index) {
        const AVSubtitleRect *rect = subtitle.rects[index];
        if (!rect) continue;

        if (rect->type == SUBTITLE_BITMAP) {
            SubtitleImage image;
            if (convertBitmapRect(rect, image)) {
                image.startMs = start;
                image.endMs = end;
                // Keep the largest bitmap if multiple rects exist.
                if (!out.hasImage || image.width * image.height > out.image.width * out.image.height) {
                    out.image = std::move(image);
                    out.hasImage = true;
                }
            }
            continue;
        }

        const char *line = rect->text ? rect->text : rect->ass;
        if (!line) continue;
        std::string plain = rect->text ? stripAssOverrides(line) : assEventToPlain(line);
        if (plain.empty()) continue;
        if (!out.text.empty()) out.text.push_back('\n');
        out.text += plain;
        out.hasText = true;
    }

    avsubtitle_free(&subtitle);
    return out.hasText || out.hasImage;
}

} // namespace ffplayer
