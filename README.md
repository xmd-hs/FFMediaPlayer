# FFMediaPlayer

FFMediaPlayer 是一个基于 C++17 和 FFmpeg 的媒体播放器项目。工程分为不依赖 Qt 的播放 SDK，以及负责窗口和交互的 Qt 应用层。

## 目录结构

```text
sdk/
  include/       SDK 公共头文件
  core/          解复用、解码、时钟、队列和会话管理
  platform/      音频和视频输出适配接口
src/MediaPlayerApp/
  main.cpp       Qt 程序入口
  player_window.*播放器窗口和控制逻辑
third_party/ffmpeg/ 项目内 FFmpeg 头文件和库
```

旧版 MediaPlayer、在线目录、重复的解码线程和旧线程池已经从构建链路中移除。

## 构建 SDK

Windows 使用项目内 FFmpeg：

```powershell
cmake -S sdk -B sdk/build -DFFMPEG_ROOT="$PWD/third_party/ffmpeg"
cmake --build sdk/build --config Release
```

Linux 或 macOS 使用系统 FFmpeg：

```bash
cmake -S sdk -B build-sdk
cmake --build build-sdk --config Release
```

需要的 FFmpeg 库包括 `avformat`、`avcodec`、`avutil`、`swresample` 和 `swscale`。

## 构建 Qt 应用

安装 Qt5 Widgets 开发环境后执行：

```bash
cmake -S src/MediaPlayerApp -B build-app \
  -DCMAKE_PREFIX_PATH=/Qt/安装路径
cmake --build build-app --config Release
```

应用层提供深色播放器界面、侧边栏、视频区域、进度条、打开文件和播放控制。音频输出与视频显示通过 Sink 接口接入。

## 播放流程

```text
Player::open
  -> PlayerSession
  -> FFmpeg 解复用
  -> 音频/视频数据包队列
  -> FFmpeg 解码
  -> 音频、视频和字幕 Sink 回调
```

SDK 不依赖 Qt，可以替换为其他桌面或嵌入式 UI。

## 发布说明

实际支持的格式和编码器取决于部署时使用的 FFmpeg 构建选项。发布程序前请检查 FFmpeg 和其他第三方组件的许可证要求。
