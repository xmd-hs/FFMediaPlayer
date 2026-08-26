#include "video_sink.h"

namespace ffplayer {

void PlatformVideoSink::onVideoFrame(const VideoFrame &frame)
{
    frame_ = frame;
}

} // namespace ffplayer
