# FFMediaPlayer

基于 C++17 和 FFmpeg 的跨平台媒体播放器。

- **`sdk/`**：可移植播放引擎（UI 无关；核心 = C++17 + FFmpeg）
- **`src/MediaPlayerApp/`**：Qt5 演示应用（窗口、Metal / D3D11 送显）

硬解与 GPU 零拷贝是可选平台桥接；`-DFFPLAYER_ENABLE_HWACCEL=OFF` 得到纯 CPU 引擎。

## 功能

- 音视频播放、暂停、Seek、倍速、音量
- 多音轨 / 多字幕轨切换
- 可选硬解（VideoToolbox / D3D11VA / VAAPI）与零拷贝送显
- 状态 / 错误 / 结束回调；EOF / Seek / 解码错误边界处理

## 目录结构

```text
sdk/
  include/     公共 API
  core/        可移植播放管线（无 OS 图形 API）
  platform/    stub Sink + 可选 hw_bridge_* 平台桥接
src/MediaPlayerApp/
  adapters/    Qt / Metal / D3D11 UI 适配
third_party/ffmpeg/
```

详见 [sdk/README.md](sdk/README.md)。

## 构建 SDK

```powershell
# Windows
cmake -S sdk -B sdk/build -DFFMPEG_ROOT="$PWD/third_party/ffmpeg"
cmake --build sdk/build --config Release
```

```bash
# Linux / macOS（系统 FFmpeg）
cmake -S sdk -B build-sdk
cmake --build build-sdk --config Release

# 纯 CPU、无平台图形库
cmake -S sdk -B build-sdk-cpu -DFFPLAYER_ENABLE_HWACCEL=OFF
cmake --build build-sdk-cpu --config Release
```

## 构建 Qt 应用

```bash
cmake -S src/MediaPlayerApp -B build-app -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build-app --config Release
```

## 播放流程

```text
Player → PlayerSession → Demux → Queues → Decode → Sinks
```

## API

```cpp
ffplayer::Player player;
player.setVideoSink(&videoSink);
player.setAudioSink(&audioSink);
player.setHwAccelEnabled(true); // 需编译开启 FFPLAYER_ENABLE_HWACCEL
player.open("movie.mp4");
player.play();
```

## 发布说明

格式支持取决于 FFmpeg 构建选项；请遵守 FFmpeg 与第三方许可证。
