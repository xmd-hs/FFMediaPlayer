#pragma once

#include "../include/player.h"
#include "media_clock.h"
#include "ffmpeg_demuxer.h"
#include <atomic>
#include <mutex>
#include <thread>
#include "packet_queue.h"
#include "ffmpeg_decoder.h"
#include "ffmpeg_subtitle_decoder.h"
#include "audio_tempo.h"
#include "frame_pool.h"
#include <condition_variable>

struct SwrContext;
struct SwsContext;
struct AVPacket;
struct AVFrame;

namespace ffplayer {

class PlayerSession {
public:
    PlayerSession();
    ~PlayerSession();

    bool open(const std::string& url);
    void close();
    void play();
    void pause();
    bool seek(MediaTimeMs positionMs);

    void setVideoSink(IVideoSink* sink) { videoSink_ = sink; }
    void setAudioSink(IAudioSink* sink) { audioSink_ = sink; }
    void setSubtitleSink(ISubtitleSink* sink) { subtitleSink_ = sink; }
    void setStateCallback(Player::StateCallback cb) { stateCallback_ = std::move(cb); }
    void setErrorCallback(Player::ErrorCallback cb) { errorCallback_ = std::move(cb); }
    void setFinishedCallback(Player::FinishedCallback cb) { finishedCallback_ = std::move(cb); }
    void setVolume(float volume);
    void setSpeed(double speed);
    void setHwAccelEnabled(bool enabled) { hwAccelEnabled_ = enabled; }
    bool hwAccelEnabled() const { return hwAccelEnabled_.load(); }
    bool videoHwAccelActive() const { return videoDecoder_.hwAccelActive(); }

    PlaybackState state() const;
    MediaTimeMs position() const;
    MediaTimeMs duration() const { return durationMs_; }
    std::vector<TrackInfo> audioTracks() const { return demuxer_.audioTracks(); }
    std::vector<TrackInfo> subtitleTracks() const { return demuxer_.subtitleTracks(); }
    bool selectAudioTrack(int streamIndex);
    bool selectSubtitleTrack(int streamIndex);

private:
    static void freePacket(AVPacket* packet);

    void setState(PlaybackState state);
    void demuxLoop();
    void videoLoop();
    void audioLoop();
    void processAudioFrame(AVFrame* frame, std::uint64_t epoch);
    void subtitleLoop();
    void waitWhilePaused();
    void waitForDemux();
    void waitForMasterClock(MediaTimeMs targetPts, std::uint64_t epoch);
    void notifyPlayback();
    void releaseVideoFrame(AVFrame* frame);
    bool shouldDropVideoFrame(MediaTimeMs pts, MediaTimeMs master, bool& catchUpMode) const;
    void signalDecodeError(const std::string& message, std::uint64_t epoch);
    void pushPipelineEofSentinels(std::uint64_t epoch);
    bool pushPacket(PacketQueue<AVPacket*>& queue, AVPacket* packet, std::uint64_t epoch);
    bool pushEofSentinel(PacketQueue<AVPacket*>& queue, std::uint64_t epoch);
    bool ensureSubtitleThread();
    bool epochIsCurrent(std::uint64_t epoch) const;
    bool sendPacket(FfmpegDecoder& decoder, AVPacket*& packet, std::mutex& codecMutex,
                    std::uint64_t epoch);
    void presentVideoFrame(AVFrame* frame, MediaTimeMs pts, std::uint64_t epoch);
    bool handleVideoFrame(AVFrame* frame, std::uint64_t epoch);
    void notifyStreamFinished(std::uint64_t epoch);
    int expectedEofWorkers() const;
    MediaTimeMs masterClockMs() const;
    void resetResampler();
    void resetScaler();
    void wakeQueues();
    void beginEofDrain();
    void endEofDrain();

    mutable std::mutex mutex_;
    mutable std::mutex demuxMutex_;
    std::mutex videoCodecMutex_;
    std::mutex audioCodecMutex_;
    std::mutex subtitleCodecMutex_;
    std::mutex resamplerMutex_;
    std::string url_;
    MediaTimeMs durationMs_ = 0;
    PlaybackState state_ = PlaybackState::Idle;
    MediaClock clock_;
    FfmpegDemuxer demuxer_;
    FfmpegDecoder videoDecoder_;
    FfmpegDecoder audioDecoder_;
    FfmpegSubtitleDecoder subtitleDecoder_;
    ::SwrContext* resampler_ = nullptr;
    ::SwsContext* scaler_ = nullptr;
    int scalerSrcW_ = 0;
    int scalerSrcH_ = 0;
    int scalerSrcFormat_ = -1;
    int resamplerSrcRate_ = 0;
    int resamplerSrcFormat_ = -1;
    std::uint64_t resamplerSrcLayout_ = 0;
    AudioTempoFilter tempoFilter_;
    AvFramePool videoFramePool_{12};
    AvFramePool scalerFramePool_{6};
    std::mutex playbackMutex_;
    std::condition_variable playbackCv_;
    std::atomic_bool videoCatchUp_{false};
    PacketQueue<AVPacket*> videoPackets_{128};
    PacketQueue<AVPacket*> audioPackets_{128};
    PacketQueue<AVPacket*> subtitlePackets_{64};
    std::thread demuxThread_;
    std::thread videoThread_;
    std::thread audioThread_;
    std::thread subtitleThread_;
    std::atomic_bool stopRequested_{false};
    std::atomic_bool paused_{true};
    std::atomic_bool demuxAtEof_{false};
    std::atomic_bool eofDrainActive_{false};
    std::atomic_bool decodeErrorNotified_{false};
    std::atomic_bool subtitlePump_{false};
    std::atomic<std::uint64_t> seekEpoch_{0};
    IVideoSink* videoSink_ = nullptr;
    IAudioSink* audioSink_ = nullptr;
    ISubtitleSink* subtitleSink_ = nullptr;
    Player::StateCallback stateCallback_;
    Player::ErrorCallback errorCallback_;
    Player::FinishedCallback finishedCallback_;
    std::atomic<float> volume_{1.0f};
    std::atomic<double> speed_{1.0};
    std::atomic<MediaTimeMs> audioClockMs_{0};
    std::atomic<MediaTimeMs> videoClockMs_{0};
    std::atomic<int> eofWorkers_{0};
    std::atomic_bool finishedNotified_{false};
    std::atomic_bool hwAccelEnabled_{true};
};

} // namespace ffplayer
