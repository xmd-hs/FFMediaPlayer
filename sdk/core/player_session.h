#pragma once

#include "../include/player.h"
#include "media_clock.h"
#include "ffmpeg_demuxer.h"
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>
#include "packet_queue.h"
#include "ffmpeg_decoder.h"
#include "ffmpeg_subtitle_decoder.h"

struct SwrContext;
struct SwsContext;

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
    void setVolume(float volume) { volume_ = volume < 0 ? 0 : volume > 1 ? 1 : volume; }
    void setSpeed(double speed) { clock_.setSpeed(speed); }

    PlaybackState state() const;
    MediaTimeMs position() const { return clock_.position(); }
    MediaTimeMs duration() const { return durationMs_; }
    std::vector<TrackInfo> audioTracks() const { return demuxer_.audioTracks(); }
    std::vector<TrackInfo> subtitleTracks() const { return demuxer_.subtitleTracks(); }
    bool selectAudioTrack(int streamIndex);
    bool selectSubtitleTrack(int streamIndex);

private:
    void setState(PlaybackState state);
    void demuxLoop();
    void videoLoop();
    void audioLoop();
    void subtitleLoop();
    MediaTimeMs audioClock() const;

    mutable std::mutex mutex_;
    mutable std::mutex demuxMutex_;
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
    PacketQueue<AVPacket*> videoPackets_{128};
    PacketQueue<AVPacket*> audioPackets_{128};
    PacketQueue<AVPacket*> subtitlePackets_{64};
    std::thread demuxThread_;
    std::thread videoThread_;
    std::thread audioThread_;
    std::thread subtitleThread_;
    std::atomic_bool stopRequested_{false};
    std::atomic_bool paused_{true};
    IVideoSink* videoSink_ = nullptr;
    IAudioSink* audioSink_ = nullptr;
    ISubtitleSink* subtitleSink_ = nullptr;
    Player::StateCallback stateCallback_;
    Player::ErrorCallback errorCallback_;
    Player::FinishedCallback finishedCallback_;
    float volume_ = 1.0f;
    std::atomic<MediaTimeMs> audioClockMs_{0};
    std::atomic<MediaTimeMs> videoClockMs_{0};
    std::atomic<int> eofWorkers_{0};
    std::atomic_bool finishedNotified_{false};
};

} // namespace ffplayer
