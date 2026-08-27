# FFMediaPlayer SDK

**可移植播放引擎**：核心只依赖 **C++17 + FFmpeg**，不依赖 Qt、窗口系统或音频设备 API。UI 通过 `IVideoSink` / `IAudioSink` / `ISubtitleSink` 接入。

硬解与 GPU 零拷贝是**可选平台插件**（`platform/hw_bridge_*.cpp`），可用 `-DFFPLAYER_ENABLE_HWACCEL=OFF` 编出纯 CPU 引擎。

## 分层

```text
include/     公共 API（Player、Sink、类型）—— 平台无关
core/        播放管线（解复用 / 队列 / 解码 / 同步 / 会话）—— 平台无关
platform/    可选实现
               audio_sink / video_sink   stub
               hw_bridge_none            无硬解桥接
               hw_bridge_videotoolbox    macOS
               hw_bridge_d3d11           Windows
               hw_bridge_vaapi           Linux (VAAPI→DRM_PRIME)
```

`core/` **不包含** `#include <d3d11.h>` / CoreVideo / libva。平台符号通过 `detail::hwBridgeOps()` 注入。

## 架构

```text
Player
  -> PlayerSession          (control / sync / demux / video / audio / subtitle)
     -> FfmpegDemuxer
     -> PacketQueue
     -> FfmpegDecoder       可选硬解（经 hwBridgeOps 选择设备）
     -> IAudioSink / IVideoSink / ISubtitleSink
```

## 硬解与零拷贝

| 构建 | 行为 |
|------|------|
| `FFPLAYER_ENABLE_HWACCEL=OFF` | 纯 CPU；不链接 Apple 框架 / d3d11；`hwBridgeOps()==nullptr` |
| `ON` + macOS | VideoToolbox 硬解 + `CVPixelBuffer` 零拷贝 |
| `ON` + Windows | D3D11VA + `ID3D11Texture2D` 零拷贝 |
| `ON` + Linux | VAAPI 硬解 + `AVDRMFrameDescriptor*`（DMA-BUF）零拷贝 |

运行时调用 `Player::setHwAccelEnabled()` 会在当前位置重建视频解码器并恢复播放；
未打开媒体时，它设置下一次 `open()` 的解码偏好（仅在编译开启硬解时有效）。

Windows Qt 示例会在应用目录写入 `ffplayer_hw.log`。出现
`D3D11 zero-copy verified ... gpu-copy=0` 表示 FFmpeg 的 D3D11 解码纹理已被
直接创建 Shader Resource View 并采样；该标记只在纹理支持采样且 SRV 创建成功后输出。
若硬件、驱动或编码格式不支持，解码器会回退软解，且不会输出该成功标记。

播放中调用 `setHwAccelEnabled()` 时，标准错误流会依次输出
`[ffplayer] hw-switch requested`、`decoder rebuilt`、`completed`。其中
`active=hardware` 表示目标硬解已启用，`active=software` 表示已回退软解。切换在
当前位置重建解码器、seek 到关键帧并恢复播放，属于受控重建流程，不承诺单帧无缝。

典型 Windows D3D11 成功日志如下：

```text
D3D11 decode texture: 320x176 format=103 array=17 bind=0x208 misc=0x0
D3D11 zero-copy verified: decoder texture sampled directly; slice=13 srv=1 gpu-copy=0
D3D11 stats: received=180 presented=180 failed=0 success=100.00%
```

`format=103` 为 `DXGI_FORMAT_NV12`，`bind=0x208` 包含解码与着色器资源绑定标志。

## 编译

```bash
# 默认可带平台硬解桥接
cmake -S sdk -B build-sdk -DFFMPEG_ROOT=/path/to/ffmpeg
cmake --build build-sdk --config Release

# 纯可移植 CPU 引擎
cmake -S sdk -B build-sdk-cpu -DFFPLAYER_ENABLE_HWACCEL=OFF
```

依赖：`avformat`、`avcodec`、`avutil`、`swresample`、`swscale`、`avfilter`。

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

Linux 零拷贝：`HwVideoBackend::VAAPI` 时 `nativeHandle` 为 `AVDRMFrameDescriptor*`，由 `keepAlive` 持有生命周期。

## CMake 集成

```cmake
find_package(ffplayer_sdk CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE ffplayer::ffplayer_sdk)
```

安装包不内置 FFmpeg；消费端需要安装对应平台和架构的 FFmpeg 开发文件。FFmpeg
不在系统搜索路径时，在配置消费工程时传入
`-DFFPLAYER_FFMPEG_ROOT=/path/to/ffmpeg`（也可使用 `FFMPEG_ROOT` 环境变量）。

公共头文件安装在 `include/ffplayer`。
