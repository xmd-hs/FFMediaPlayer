#include "media_clock.h"
#include <algorithm>

namespace ffplayer {

void MediaClock::reset(MediaTimeMs positionMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    baseMs_ = std::max<MediaTimeMs>(0, positionMs);
    startedAt_ = std::chrono::steady_clock::now();
    running_ = false;
}

void MediaClock::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return;
    startedAt_ = std::chrono::steady_clock::now();
    running_ = true;
}

void MediaClock::pause()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt_).count();
    baseMs_ += static_cast<MediaTimeMs>(elapsed * speed_);
    running_ = false;
}

void MediaClock::setSpeed(double speed)
{
    if (speed <= 0.0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt_).count();
        baseMs_ += static_cast<MediaTimeMs>(elapsed * speed_);
        startedAt_ = std::chrono::steady_clock::now();
    }
    speed_ = speed;
}

MediaTimeMs MediaClock::position() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return baseMs_;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt_).count();
    return baseMs_ + static_cast<MediaTimeMs>(elapsed * speed_);
}

bool MediaClock::running() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

} // namespace ffplayer
