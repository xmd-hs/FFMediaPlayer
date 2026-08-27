# FFMediaPlayer

基于 C++17 和 FFmpeg 的跨平台媒体播放器。工程分为不依赖 Qt 的播放 SDK（`sdk/`），以及负责窗口和交互的 Qt5 应用（`src/MediaPlayerApp/`）。

## 功能

- 音视频播放、暂停、Seek、倍速、音量
- 多音轨 / 多字幕轨切换
- 视频硬解（VideoToolbox / D3D11VA / VAAPI），失败自动回退软解
- GPU 零拷贝送显（macOS Metal、Windows D3D11），不支持时回退 CPU 帧
- 播放状态、错误与结束回调
- EOF / Seek / 解码错误等边界场景的稳定性处理

## 目录结构

```text
sdk/
  include/           SDK 公共头文件（ffplayer 命名空间）
  core/              解复用、解码、时钟、队列、会话管理
  platform/          音频/视频 Sink 默认实现
src/MediaPlayerApp/
  main.cpp           Qt 程序入口
  player_window.*    播放器窗口与控制逻辑
  adapters/          Qt Sink、Metal / D3D11 视频视图
third_party/ffmpeg/  项目内 FFmpeg 头文件与库（Windows / Linux）
```

SDK 详细说明见 [sdk/README.md](sdk/README.md)。

## 构建 SDK

**Windows**（使用项目内 FFmpeg）：

```powershell
cmake -S sdk -B sdk/build -DFFMPEG_ROOT="$PWD/third_party/ffmpeg"
cmake --build sdk/build --config Release
```

**Linux / macOS**（使用系统 FFmpeg，需安装开发包）：

```bash
cmake -S sdk -B build-sdk
cmake --build build-sdk --config Release
```

依赖 FFmpeg 组件：`avformat`、`avcodec`、`avutil`、`swresample`、`swscale`。

## 构建 Qt 应用

安装 Qt5 Widgets 与 Qt5 Multimedia 后：

```bash
cmake -S src/MediaPlayerApp -B build-app \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/5.x/gcc_64
cmake --build build-app --config Release
```

- **macOS**：链接 Metal / CoreVideo，硬解帧经 `MetalVideoView` 零拷贝呈现
- **Windows**：链接 `d3d11` / `dxgi`，硬解帧经 `D3d11VideoView` 零拷贝呈现
- 其他平台或未启用硬解时，走 CPU `QImage` 路径

## 播放流程

```text
Player::open
  -> PlayerSession
  -> FFmpeg 解复用
  -> 音频 / 视频 / 字幕 PacketQueue
  -> FFmpeg 解码（可选硬解）
  -> IAudioSink / IVideoSink / ISubtitleSink 回调
```

SDK 不依赖 Qt，可替换为其他桌面或嵌入式 UI；应用层通过 Sink 接口接入音频输出与视频显示。

## API 概览

```cpp
#include <ffplayer/player.h>

ffplayer::Player player;
player.setVideoSink(&videoSink);
player.setAudioSink(&audioSink);
player.setErrorCallback([](const std::string& msg) { /* ... */ });
player.setHwAccelEnabled(true);
player.open("movie.mp4");
player.play();
```

硬解开关：`setHwAccelEnabled()`；运行时查询：`videoHwAccelActive()`。支持零拷贝的 Sink 需实现 `supportsHwVideo()` 与 `onHwVideoFrame()`。

## 发布说明

实际支持的容器格式与编解码器取决于部署时使用的 FFmpeg 构建选项。发布前请检查 FFmpeg 及其他第三方组件的许可证要求。
