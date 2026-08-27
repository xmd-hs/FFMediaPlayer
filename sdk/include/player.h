#pragma once

#include "player_sink.h"
#include <functional>
#include <string>
#include <memory>
#include <vector>

namespace ffplayer {

class Player {
public:
    using StateCallback = std::function<void(PlaybackState)>;
    using ErrorCallback = std::function<void(const std::string&)>;
    using FinishedCallback = std::function<void()>;

    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // All control methods are serialized internally and may be called from
    // different threads. Sinks and callbacks are configured while the player is Idle. Call close()
    // before replacing or destroying a configured sink.
    void setVideoSink(IVideoSink* sink);
    void setAudioSink(IAudioSink* sink);
    void setSubtitleSink(ISubtitleSink* sink);
    void setStateCallback(StateCallback callback);
    void setErrorCallback(ErrorCallback callback);
    void setFinishedCallback(FinishedCallback callback);

    bool open(const std::string& url);
    void close();
    void play();
    void pause();
    bool seek(MediaTimeMs positionMs);

    PlaybackState state() const;
    MediaTimeMs position() const;
    MediaTimeMs duration() const;
    std::vector<TrackInfo> audioTracks() const;
    std::vector<TrackInfo> subtitleTracks() const;
    int selectedAudioTrack() const;
    int selectedSubtitleTrack() const;
    bool selectAudioTrack(int streamIndex);
    bool selectSubtitleTrack(int streamIndex);
    void setVolume(float volume);
    void setSpeed(double speed);
    // Switches the active video decoder at the current position when media is open;
    // otherwise sets the preference for the next open().
    void setHwAccelEnabled(bool enabled);
    bool hwAccelEnabled() const;
    // True after open() if the video decoder is using hardware acceleration.
    bool videoHwAccelActive() const;

private:
    class PlayerImpl;
    std::unique_ptr<PlayerImpl> impl_;
};

} // namespace ffplayer
