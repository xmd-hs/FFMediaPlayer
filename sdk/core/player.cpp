#include "../include/player.h"
#include "player_session.h"

#include <utility>

namespace ffplayer {

class Player::PlayerImpl {
public:
    PlayerSession session;
};

Player::Player() : impl_(std::make_unique<PlayerImpl>())
{
}

Player::~Player() = default;

void Player::setVideoSink(IVideoSink *sink)
{
    impl_->session.setVideoSink(sink);
}
void Player::setAudioSink(IAudioSink *sink) { impl_->session.setAudioSink(sink); }
void Player::setSubtitleSink(ISubtitleSink *sink) { impl_->session.setSubtitleSink(sink); }
void Player::setStateCallback(StateCallback callback) { impl_->session.setStateCallback(std::move(callback)); }
void Player::setErrorCallback(ErrorCallback callback) { impl_->session.setErrorCallback(std::move(callback)); }
void Player::setFinishedCallback(FinishedCallback callback) { impl_->session.setFinishedCallback(std::move(callback)); }
bool Player::open(const std::string &url) { return impl_->session.open(url); }
void Player::close() { impl_->session.close(); }
void Player::play() { impl_->session.play(); }
void Player::pause() { impl_->session.pause(); }
bool Player::seek(MediaTimeMs position) { return impl_->session.seek(position); }
std::vector<TrackInfo> Player::audioTracks() const { return impl_->session.audioTracks(); }
std::vector<TrackInfo> Player::subtitleTracks() const { return impl_->session.subtitleTracks(); }
bool Player::selectAudioTrack(int streamIndex) { return impl_->session.selectAudioTrack(streamIndex); }
bool Player::selectSubtitleTrack(int streamIndex) { return impl_->session.selectSubtitleTrack(streamIndex); }
PlaybackState Player::state() const { return impl_->session.state(); }
MediaTimeMs Player::position() const { return impl_->session.position(); }
MediaTimeMs Player::duration() const { return impl_->session.duration(); }
void Player::setVolume(float volume) { impl_->session.setVolume(volume); }
void Player::setSpeed(double speed) { impl_->session.setSpeed(speed); }
void Player::setHwAccelEnabled(bool enabled) { impl_->session.setHwAccelEnabled(enabled); }
bool Player::hwAccelEnabled() const { return impl_->session.hwAccelEnabled(); }
bool Player::videoHwAccelActive() const { return impl_->session.videoHwAccelActive(); }

} // namespace ffplayer
