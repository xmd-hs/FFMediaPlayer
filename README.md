# FFMediaPlayer

基于 C++17 和 FFmpeg 的跨平台媒体播放器。

- **`sdk/`**：可移植播放引擎（UI 无关；核心 = C++17 + FFmpeg）
- **`src/MediaPlayerApp/`**：Qt5 演示应用（窗口、Metal / D3D11 送显）

硬解与 GPU 零拷贝是可选平台桥接；`-DFFPLAYER_ENABLE_HWACCEL=OFF` 得到纯 CPU 引擎。

## 功能

- 音视频播放、暂停、Seek、倍速、音量
- 多音轨 / 多字幕轨切换
- 可选硬解（VideoToolbox / D3D11VA / VAAPI）与零拷贝送显
- Windows D3D11VA 解码纹理直接采样送显，支持播放中软/硬解切换
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

## Windows 硬解直出验证

Qt 示例在 Windows 下会将 D3D11 诊断写入应用目录的 `ffplayer_hw.log`。
使用支持 D3D11VA 的 H.264 或 HEVC 媒体播放后，出现以下日志表示解码纹理的
指定数组切片已直接作为 Shader Resource View 采样，路径中没有
`CopySubresourceRegion` 中间 GPU 纹理复制：

```text
D3D11 decode texture: 320x176 format=103 array=17 bind=0x208 misc=0x0
D3D11 zero-copy verified: decoder texture sampled directly; slice=13 srv=1 gpu-copy=0
D3D11 stats: received=180 presented=180 failed=0 success=100.00%
```

其中 `format=103` 是 `DXGI_FORMAT_NV12`，`bind=0x208` 包含
`D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE`。`gpu-copy=0` 只会在纹理
可被直接创建 SRV 且实际送显成功后输出；硬件解码不可用、驱动不支持或编码格式
不匹配时，播放器会回退到软解，不会输出该成功标记。

播放中切换应用的“硬件解码”复选框，可在标准错误流查看切换过程：

```text
[ffplayer] hw-switch requested: target=hardware position_ms=...
[ffplayer] hw-switch decoder rebuilt: requested=hardware active=hardware resumed=1
[ffplayer] hw-switch completed: active=hardware position_ms=...
```

切换会在当前播放位置暂停视频管线、重建目标解码器、从关键帧 seek 并恢复原有播放状态；
这不是无间断的单帧级切换。若目标硬解不可用，`active=software` 明确表示已回退软解。

## 发布说明

格式支持取决于 FFmpeg 构建选项；请遵守 FFmpeg 与第三方许可证。
