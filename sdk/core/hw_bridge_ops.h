#pragma once

#include "../include/player_types.h"

struct AVFrame;

namespace ffplayer {
namespace detail {

// Optional platform backend. Returns nullptr when hardware bridges are disabled
// or unavailable for this build — the portable core never includes OS headers.
struct HwBridgeOps {
    bool (*make)(AVFrame* frame, MediaTimeMs ptsMs, HwVideoFrame& out);
    bool (*canPresent)(AVFrame* frame);
    // Preferred FFmpeg hwdevice type, or AV_HWDEVICE_TYPE_NONE (-1 style via int).
    int (*preferredDeviceType)();
};

const HwBridgeOps* hwBridgeOps();

} // namespace detail
} // namespace ffplayer
