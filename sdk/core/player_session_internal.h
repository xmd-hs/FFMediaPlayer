#pragma once

#include "../include/player_types.h"

namespace ffplayer {
namespace session_detail {

constexpr int kOutputSampleRate = 48000;
constexpr int kOutputChannels = 2;

// Adaptive A/V sync thresholds (milliseconds).
constexpr MediaTimeMs kSyncEarlyMs = 20;
constexpr MediaTimeMs kSyncLatePresentMs = 60;
constexpr MediaTimeMs kSyncLateDropMs = 120;
constexpr MediaTimeMs kSyncCatchUpEnterMs = 280;
constexpr MediaTimeMs kSyncCatchUpExitMs = 100;
constexpr MediaTimeMs kSyncMaxWaitMs = 250;

} // namespace session_detail
} // namespace ffplayer
