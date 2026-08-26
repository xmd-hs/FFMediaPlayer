#pragma once

#include "../include/player_types.h"
#include <atomic>
#include <chrono>
#include <mutex>

namespace ffplayer {

class MediaClock {
public:
    void reset(MediaTimeMs positionMs = 0);
    void start();
    void pause();
    void setSpeed(double speed);
    MediaTimeMs position() const;
    bool running() const;

private:
    mutable std::mutex mutex_;
    MediaTimeMs baseMs_ = 0;
    double speed_ = 1.0;
    bool running_ = false;
    std::chrono::steady_clock::time_point startedAt_{};
};

} // namespace ffplayer
