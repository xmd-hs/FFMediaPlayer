#pragma once

#include "../include/player_types.h"

struct AVFrame;

namespace ffplayer {

// Build a zero-copy HwVideoFrame from a hardware AVFrame.
// Returns false if the format is unsupported (caller should transfer to system memory).
bool makeHwVideoFrame(AVFrame* frame, MediaTimeMs ptsMs, HwVideoFrame& out);

bool isHardwarePixelFormat(int avPixelFormat);

// True when the platform sink can present this hw frame without CPU fallback.
bool canPresentHwVideoFrame(AVFrame* frame);

} // namespace ffplayer
