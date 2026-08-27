#include "audio_tempo.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

namespace ffplayer {

namespace {

void appendAtempo(std::string& filters, double factor)
{
    if (!filters.empty()) filters += ',';
    char buf[64];
    std::snprintf(buf, sizeof(buf), "atempo=%.6f", factor);
    filters += buf;
}

// Split tempo into atempo stages, each within [0.5, 2.0].
std::string buildAtempoChain(double tempo)
{
    std::string filters;
    double remaining = tempo;
    while (remaining > 2.0 + 1e-6) {
        appendAtempo(filters, 2.0);
        remaining /= 2.0;
    }
    while (remaining < 0.5 - 1e-6) {
        appendAtempo(filters, 0.5);
        remaining /= 0.5;
    }
    if (std::fabs(remaining - 1.0) > 1e-3) {
        appendAtempo(filters, remaining);
    }
    return filters;
}

} // namespace

bool AudioTempoFilter::open(int sampleRate, int channels, double tempo)
{
    if (sampleRate <= 0 || channels <= 0 || tempo <= 0.0) return false;
    sampleRate_ = sampleRate;
    channels_ = channels;
    return rebuild(tempo);
}

void AudioTempoFilter::close()
{
    if (graph_) {
        avfilter_graph_free(&graph_);
        graph_ = nullptr;
    }
    src_ = nullptr;
    sink_ = nullptr;
    av_frame_free(&inputFrame_);
    av_frame_free(&outputFrame_);
    tempo_ = 1.0;
    pts_ = 0;
}

void AudioTempoFilter::flush()
{
    if (!graph_) return;
    const double tempo = tempo_;
    const int rate = sampleRate_;
    const int channels = channels_;
    close();
    if (rate > 0 && channels > 0) open(rate, channels, tempo);
}

bool AudioTempoFilter::rebuild(double tempo)
{
    close();
    tempo = std::max(0.25, std::min(4.0, tempo));
    tempo_ = tempo;
    pts_ = 0;

    if (std::fabs(tempo - 1.0) <= 1e-3) {
        // Identity — caller should bypass the filter.
        return true;
    }

    const std::string chain = buildAtempoChain(tempo);
    if (chain.empty()) return true;

    graph_ = avfilter_graph_alloc();
    if (!graph_) return false;

    const AVFilter* abuffer = avfilter_get_by_name("abuffer");
    const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffer || !abuffersink) {
        close();
        return false;
    }

    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels_);
    char layoutDesc[64];
    av_channel_layout_describe(&layout, layoutDesc, sizeof(layoutDesc));

    char args[256];
    std::snprintf(args, sizeof(args),
                  "time_base=1/%d:sample_rate=%d:sample_fmt=s16:channel_layout=%s",
                  sampleRate_, sampleRate_, layoutDesc);

    if (avfilter_graph_create_filter(&src_, abuffer, "in", args, nullptr, graph_) < 0) {
        close();
        return false;
    }
    if (avfilter_graph_create_filter(&sink_, abuffersink, "out", nullptr, nullptr, graph_) < 0) {
        close();
        return false;
    }

    enum AVSampleFormat sampleFmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int sampleRates[] = { sampleRate_, -1 };
    if (av_opt_set_int_list(sink_, "sample_fmts", sampleFmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN) < 0 ||
        av_opt_set_int_list(sink_, "sample_rates", sampleRates, -1, AV_OPT_SEARCH_CHILDREN) < 0) {
        close();
        return false;
    }

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        close();
        return false;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = src_;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = sink_;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    const int linkResult = avfilter_graph_parse_ptr(graph_, chain.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (linkResult < 0 || avfilter_graph_config(graph_, nullptr) < 0) {
        close();
        return false;
    }
    return true;
}

bool AudioTempoFilter::process(const std::int16_t* input, int inputFrames,
                               std::vector<std::int16_t>& outputInterleaved)
{
    outputInterleaved.clear();
    if (!input || inputFrames <= 0) return false;
    const auto estimatedFrames = static_cast<std::size_t>(
        std::ceil(inputFrames / std::max(tempo_, 0.25))) + 256;
    const auto estimatedSamples = estimatedFrames * static_cast<std::size_t>(channels_);
    if (outputInterleaved.capacity() < estimatedSamples) {
        outputInterleaved.reserve(estimatedSamples);
    }

    // Bypass when tempo ~ 1 or filter unavailable.
    if (!graph_ || std::fabs(tempo_ - 1.0) <= 1e-3) {
        outputInterleaved.assign(input, input + static_cast<std::size_t>(inputFrames) * channels_);
        return true;
    }

    if (!inputFrame_) inputFrame_ = av_frame_alloc();
    if (!outputFrame_) outputFrame_ = av_frame_alloc();
    if (!inputFrame_ || !outputFrame_) return false;

    av_frame_unref(inputFrame_);
    inputFrame_->format = AV_SAMPLE_FMT_S16;
    inputFrame_->sample_rate = sampleRate_;
    inputFrame_->nb_samples = inputFrames;
    av_channel_layout_default(&inputFrame_->ch_layout, channels_);
    inputFrame_->pts = pts_;
    pts_ += inputFrames;

    if (av_frame_get_buffer(inputFrame_, 0) < 0) return false;
    const int bytes = inputFrames * channels_ * static_cast<int>(sizeof(std::int16_t));
    std::memcpy(inputFrame_->data[0], input, static_cast<std::size_t>(bytes));

    if (av_buffersrc_add_frame_flags(src_, inputFrame_, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
        return false;

    return collectOutput(outputInterleaved);
}

bool AudioTempoFilter::drain(std::vector<std::int16_t>& outputInterleaved)
{
    outputInterleaved.clear();
    if (!graph_) return true;
    if (av_buffersrc_add_frame_flags(src_, nullptr, 0) < 0) return false;
    return collectOutput(outputInterleaved);
}

bool AudioTempoFilter::collectOutput(std::vector<std::int16_t>& outputInterleaved)
{
    if (!outputFrame_) return false;
    for (;;) {
        av_frame_unref(outputFrame_);
        const int ret = av_buffersink_get_frame(sink_, outputFrame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) return false;
        const int frames = outputFrame_->nb_samples;
        const auto* samples = reinterpret_cast<const std::int16_t*>(outputFrame_->data[0]);
        outputInterleaved.insert(outputInterleaved.end(), samples,
                                 samples + static_cast<std::size_t>(frames) * channels_);
    }
    return true;
}

} // namespace ffplayer
