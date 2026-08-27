#pragma once

#include "../include/player_types.h"

struct AVFrame;

namespace ffplayer {

// Portable entry points. Platform zero-copy is injected via detail::hwBridgeOps()
// (see platform/hw_bridge_*.cpp). With FFPLAYER_ENABLE_HWACCEL=OFF, these never
// produce GPU surfaces.

bool makeHwVideoFrame(AVFrame* frame, MediaTimeMs ptsMs, HwVideoFrame& out);
bool isHardwarePixelFormat(int avPixelFormat);
bool canPresentHwVideoFrame(AVFrame* frame);

} // namespace ffplayer
