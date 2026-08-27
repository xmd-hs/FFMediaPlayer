#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct AVFilterGraph;
struct AVFilterContext;
struct AVFrame;

namespace ffplayer {

// Pitch-preserving tempo via libavfilter atempo (chains filters for rates outside 0.5–2.0).
class AudioTempoFilter {
public:
    AudioTempoFilter() = default;
    ~AudioTempoFilter() { close(); }

    AudioTempoFilter(const AudioTempoFilter&) = delete;
    AudioTempoFilter& operator=(const AudioTempoFilter&) = delete;

    bool open(int sampleRate, int channels, double tempo);
    void close();
    void flush();

    // in/out are interleaved S16 PCM. Returns false on failure.
    bool process(const std::int16_t* input, int inputFrames,
                 std::vector<std::int16_t>& outputInterleaved);
    // Signals EOF to atempo and returns its delayed tail samples.
    bool drain(std::vector<std::int16_t>& outputInterleaved);

    double tempo() const { return tempo_; }
    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    bool valid() const { return graph_ != nullptr; }

private:
    bool rebuild(double tempo);
    bool collectOutput(std::vector<std::int16_t>& outputInterleaved);

    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* src_ = nullptr;
    AVFilterContext* sink_ = nullptr;
    AVFrame* inputFrame_ = nullptr;
    AVFrame* outputFrame_ = nullptr;
    int sampleRate_ = 0;
    int channels_ = 0;
    double tempo_ = 1.0;
    std::int64_t pts_ = 0;
};

} // namespace ffplayer
