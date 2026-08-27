# FFMediaPlayer SDK

SDK 是不依赖 Qt 的 C++17 播放核心。应用程序通过 `IVideoSink`、`IAudioSink` 和 `ISubtitleSink` 接收解码后的媒体数据。

## 架构

```text
Player
  -> PlayerSession
     -> FfmpegDemuxer       FFmpeg 解复用
     -> 数据包队列
     -> FfmpegDecoder       FFmpeg 解码（视频可硬解，失败回退软解）
     -> 音频重采样          IAudioSink
     -> 像素格式转换        IVideoSink
     -> 字幕解码            ISubtitleSink
```

## 视频硬解

- **一期**：硬解 + `av_hwframe_transfer_data` 回 CPU，走原有 `onVideoFrame`。
- **二期（macOS / Windows）**：VideoToolbox 或 D3D11 表面经 `HwVideoFrame` / `onHwVideoFrame` 零拷贝呈图；失败自动回退一期/软解。

`Player::setHwAccelEnabled(false)` 可强制软解；`videoHwAccelActive()` 查询是否硬解成功。

`Player` 是对外唯一入口，`PlayerSession` 负责会话状态、线程、队列、时钟、解复用和解码。SDK 内部不包含 Qt、窗口或平台音频视频 API。

## 编译和安装

```bash
cmake -S sdk -B build-sdk -DFFMPEG_ROOT=/path/to/ffmpeg
cmake --build build-sdk --config Release
cmake --install build-sdk --prefix /path/to/install
```

如果使用项目内 FFmpeg，CMake 会根据平台查找 `lib/win64`、`lib/linux64` 或 `lib/macOS`。不设置 `FFMPEG_ROOT` 时，可以通过 pkg-config 查找系统 FFmpeg。

## 基本用法

```cpp
#include <ffplayer/player.h>

ffplayer::Player player;
player.setVideoSink(&videoSink);
player.setAudioSink(&audioSink);
player.open("movie.mp4");
player.play();
```

Sink 回调中的数据必须在回调返回前消费或复制。销毁 Sink 前应先调用 `player.close()`。

## CMake 集成

安装 SDK 后：

```cmake
find_package(ffplayer_sdk CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ffplayer::ffplayer_sdk)
```

公共头文件安装在 `include/ffplayer`。平台相关的音频设备和视频渲染应由应用层实现，SDK 只提供统一数据接口。
