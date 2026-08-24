# FFMediaPlayer

基于 **FFmpeg + Qt** 的本地 / 网络媒体播放器（C++17）。  
解复用 → 解码 → 音视频同步 → OpenGL 渲染，并带播放列表、字幕、在线目录等功能。适合音视频学习与 Demo。

仓库：https://github.com/xmd-hs/FFMediaPlayer

---

## 功能概览

| 类别 | 能力 |
|------|------|
| 播放 | 本地文件、HTTP(S) / RTSP 等网络地址、HLS（取决于 FFmpeg 编译选项） |
| 控制 | 播放 / 暂停、Seek、音量、0.5x～4.0x 倍速、全屏 |
| 列表 | 多选加入、上一首 / 下一首、列表循环 / 单曲循环 / 随机、退出后持久化 |
| 字幕 | 内嵌轨切换、外挂 SRT、位图字幕基础支持 |
| 音轨 | 多音轨切换 |
| 网络 | 直链播放、在线 JSON 目录、User-Agent / 自定义 Header（鉴权） |
| 体验 | 缓冲提示、可读打开错误、直播禁拖进度条、截图、可选硬件解码 |
| 快捷键 | 空格播放暂停 · `←` `→` Seek · `↑` `↓` 音量 · `F` 全屏 · `S` 截图 |

---

## 目录结构

```text
FFMediaPlayer/
├── include/                 # FFmpeg 头文件（随仓库提供）
├── lib/linux64/release/     # Linux x64 预编译 FFmpeg 库
├── sample_catalog.json      # 在线列表 JSON 示例
├── third_party/
│   ├── README.md
│   └── my-MemoryPool/       # git submodule → my-MemoryPool v3
└── src/
    ├── LockFree/            # 无锁队列 / 栈
    ├── ThreadPool/          # 全局线程池
    └── MediaPlayerApp/
        ├── Config/          # qmake 工程（.pro / .pri）
        ├── Core/            # 解复用、解码、重采样、音频输出
        ├── Thread/          # 解复用 / 音视频 / 字幕线程
        └── UI/              # 主窗口、OpenGL 画面、列表、字幕叠加
```

---

## 依赖

| 依赖 | 说明 |
|------|------|
| **Qt 5**（推荐 5.12+） | `core` `gui` `widgets` `opengl` `multimedia` `network` |
| **FFmpeg** | 仓库已带 Linux 头文件与 `lib/linux64`；其它平台需自备 |
| **C++17** 编译器 | GCC / Clang / MSVC |
| **[my-MemoryPool](https://github.com/xmd-hs/my-MemoryPool) v3** | 通过 submodule 引入，静态编进播放器 |

### 内存池

高频小对象（如 YUV 平面缓冲、PCM 缓冲、重采样临时区）使用 **my-MemoryPool v3**：

- 路径：`third_party/my-MemoryPool`
- 公共头：`#include <kama/MemoryPool.h>`
- 命名空间：`Kama_memoryPool::MemoryPool`
- 注意：池分配的指针必须用本池 `deallocate`，不可与 `free` / `delete` 混用  
- AVPacket / AVFrame 等仍由 **FFmpeg** 分配与释放

初始化 submodule：

```bash
git submodule update --init --recursive
```

详见 [`third_party/README.md`](third_party/README.md)。

---

## 获取代码

```bash
git clone --recurse-submodules https://github.com/xmd-hs/FFMediaPlayer.git
cd FFMediaPlayer

# 若克隆时未带 submodule：
git submodule update --init --recursive
```

---

## 编译（Linux）

仓库默认面向 **Linux x64**，并链接 `lib/linux64/release` 下的 FFmpeg。

```bash
# 安装 Qt5 开发包（发行版包名可能不同）
# Ubuntu 示例：
# sudo apt install qtbase5-dev libqt5opengl5-dev qtmultimedia5-dev

cd src/MediaPlayerApp/Config
qmake FFMediaPlayer.pro
make -j$(nproc)
```

产物默认输出到：

- Debug：`bin/linux64/debug/`
- Release：`bin/linux64/release/`

链接库：`avcodec` `avformat` `avutil` `swresample` `swscale`（仓库 `lib/linux64` 里其余 `.so` 为 FFmpeg 配套，运行时可能仍需在库搜索路径中）。

运行前确保能找到 FFmpeg 动态库（可将 `lib/linux64/release` 加入 `LD_LIBRARY_PATH`，或把 `.so` 与可执行文件放在同目录；工程已设置 `$ORIGIN` rpath）。

### Windows / macOS

- `.pro` 中预留了 `win32` 配置，需自行准备 `lib/win64` 与对应 FFmpeg。
- macOS 需自备 FFmpeg（Homebrew 等）并修改 `LIBS` / `INCLUDEPATH`；当前仓库未附带 macOS 预编译库。

---

## 使用说明

### 本地文件

1. 点击 **打开文件**（可多选）→ 加入播放列表并开始播放。  
2. 双击列表项切换；**上一首 / 下一首** 或循环 / 随机模式。

### 网络直链

右侧 **打开网络地址** 输入例如：

```text
https://example.com/video.mp4
https://example.com/live/index.m3u8
rtsp://...
```

点击 **播放网络地址** 或按回车。

### 鉴权 Header

在 **网络请求头 / 鉴权** 中填写：

- User-Agent（可选）  
- 自定义 Header，每行一条，例如：

```text
Authorization: Bearer <token>
Cookie: session=xxx
```

点击 **应用网络选项** 后，**下一次打开** 网络地址时生效（会一并传给 FFmpeg `avformat_open_input`）。

### 在线列表 JSON

格式为数组，字段：

```json
[
  {
    "title": "示例标题",
    "url": "https://cdn.example.com/a.mp4",
    "durationMs": 0
  }
]
```

可用仓库根目录 [`sample_catalog.json`](sample_catalog.json) 试跑：右侧填路径或 URL → **拉取** / **本地JSON**。

### 字幕

- 下拉框切换内嵌字幕轨（默认关）。  
- **外挂SRT** 加载 `.srt` 文件。  
- 时间轴与播放时钟对齐，显示在画面底部。

### 硬件解码

勾选 **尝试硬件解码** 后重新打开片源。  
会按平台尝试 VideoToolbox / D3D11VA / VAAPI / CUDA 等；失败自动回退软解。是否可用取决于本机 FFmpeg 与驱动。

---

## 架构示意

```text
UI (MediaPlayer / VideoWidget / Playlist)
        │  Open(url) / Seek / 轨切换
        ▼
DemuxThread ──► MediaDemuxer (libavformat, 网络/本地)
        │
        ├─► VideoThread  ──► MediaDecoder ──► OpenGL YUV
        ├─► AudioThread  ──► Decode + Resample ──► QAudioOutput
        └─► SubtitleThread ──► 文本 / 位图 ──► SubtitleOverlay

缓冲分配：my-MemoryPool v3（YUV / PCM / 重采样等）
```

---

## 支持的媒体类型（UI 过滤器）

常见容器与音频：`mp4` `mkv` `avi` `mov` `flv` `webm` `ts` `mp3` `wav` `aac` `flac` 等。  
实际能否解码取决于链接的 FFmpeg 编解码器集合。

---

## 已知限制

- 工程化程度偏 Demo：无完整自动化测试与 CI。  
- 位图字幕、复杂 ASS 为简化实现。  
- 直播 / 无时长流不支持拖动 Seek。  
- 全局播放核心仍偏单实例；多窗口需再改造。  
- 当前预编译库以 **Linux x64** 为主。

---

## 相关项目

- 播放器：https://github.com/xmd-hs/FFMediaPlayer  
- 内存池：https://github.com/xmd-hs/my-MemoryPool（本仓库使用 **v3**）

---

## 许可证

本项目与依赖库请分别查看各自仓库声明。用于学习与研究时，商用前请自行做合规审查（含 FFmpeg 许可组合）。
