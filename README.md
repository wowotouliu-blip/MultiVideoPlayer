# Multi Video Player

基于 VAAPI 硬件解码 + EGL DMA-BUF 零拷贝的多视频播放器，所有视频合成在单个窗口中按 grid 布局显示。

> 本项目代码由 AI 生成，使用 OpenCode + DeepSeek V4 Pro 编写。

## 功能特性

- **单窗口 Grid 布局** — 多个视频在同一窗口中显示，自动排列为 2×1 / 2×2 / 3×2 等
- **VAAPI 硬件解码** — H.264 / HEVC GPU 解码（需 Intel/AMD VAAPI 驱动）
- **DMA-BUF 零拷贝** — 解码帧通过 DRM PRIME fd 直接导入 EGL，无需 CPU 拷贝
- **CPU 软解回退** — 不支持的编码（如 MPEG4）自动回退 FFmpeg 软件解码
- **双纹理策略** — 每个 Player 独立维护 DMA-BUF 和 CPU 两套纹理，按需切换
- **无闪屏** — vsync 驱动渲染循环，无新帧时从已有纹理重绘，不会出现黑帧/绿帧
- **PulseAudio 音频输出** — 每个视频独立音频流

## 系统要求

- Linux（Wayland 桌面环境）
- 支持 VAAPI 的 Intel / AMD GPU
- CMake 3.16+，C++17

## 依赖安装

```bash
sudo apt install \
    libwayland-dev libwayland-egl-dev libegl-dev libgles-dev \
    libva-dev libva-drm2 libdrm-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswresample-dev \
    libdecor-0-dev \
    libpulse-dev \
    wayland-protocols pkg-config
```

## 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## 使用

```bash
./multi_video_player <video1> [video2] [video3] ...
```

示例：

```bash
# 播放单个视频（1×1）
./multi_video_player video.mp4

# 同时播放两个视频（2×1 grid）
./multi_video_player video1.mp4 video2.mp4

# 四个视频（2×2 grid）
./multi_video_player v1.mp4 v2.mp4 v3.mp4 v4.mp4

# 混合软解/硬解
./multi_video_player hevc_hardware.mp4 old_mpeg4.avi
```

### 演示

[▶ 点击观看演示录屏 (WebM)](screenrecord.webm)

## 技术架构

```
┌─────────────────────────────────────────────────────┐
│                   Main (single thread)               │
│  ┌──────────┐                                       │
│  │ Wayland  │  wl_display → libdecor decorated      │
│  │  Window  │  window                                │
│  └────┬─────┘                                       │
│       │                                              │
│  ┌────┴─────┐                                       │
│  │   EGL     │  single EGLContext + EGLSurface       │
│  │ Renderer  │  ┌─────────┐ ┌─────────┐             │
│  │           │  │ p0 tex  │ │ p1 tex  │  ...        │
│  │           │  │ Y + UV  │ │ Y + UV  │             │
│  │           │  └─────────┘ └─────────┘             │
│  └────┬─────┘                                       │
│       │                                              │
│  ┌────┴─────┐  PlayerWindow[] (one per video)       │
│  │ Decoder  │  ┌──────────────┐                      │
│  │ Threads  │  │ VaapiDecoder │ DMA-BUF export       │
│  │          │  └──────────────┘                      │
│  └──────────┘                                       │
│                                                      │
│  Render loop:                                        │
│    beginFrame() → per-cell render → drawBorders()    │
│    → scheduleFrame → swapBuffers → awaitFrame        │
└─────────────────────────────────────────────────────┘
```

### 数据流

```
video file → av_read_frame → avcodec_send_packet
                                ↓
              ┌─ VAAPI HW decode → AV_PIX_FMT_VAAPI
              │   vaExportSurfaceHandle → DMA-BUF fd
              │   eglCreateImage → glEGLImageTargetTexture2DOES
              │   → NV12 shader render
              │
              └─ (fallback) SW decode → AV_PIX_FMT_YUV420P
                  upload to GL luminance textures
                  → YUV420P shader render
```

### 渲染管线

```
for each cell in grid:
  glScissor(cell) → glClear(black)
  glViewport(letterboxed region)
    if new DMA-BUF frame:   createEGLImages + renderQuadDmaBuf
    elif new CPU frame:     upload CPU textures + renderYuv420p
    elif last was DMA-BUF:  renderDmaBufToRectPrev (reuse textures)
    elif last was CPU:      renderCpuFrameToRectPrev (reuse textures)

drawBorders() via glScissor + glClear(dark gray)
scheduleFrame() → eglSwapBuffers() → awaitFrame()
```

## 模块说明

| 模块 | 职责 |
|------|------|
| `main.cpp` | Grid 布局、渲染循环、cell 分隔线绘制 |
| `wayland_window` | libdecor 窗口管理、frame callback vsync 同步 |
| `vaapi_decoder` | FFmpeg + VAAPI 解码、DMA-BUF 导出、CPU 回退 |
| `egl_renderer` | EGL/GLES2 渲染、EGLImage DMA-BUF 导入、NV12/YUV420P 着色器 |
| `player_window` | 解码器包装、帧缓冲（DMA-BUF + CPU 双路径） |
| `audio_output` | PulseAudio simple API 音频播放 |

## 验证环境

```bash
vainfo                          # 检查 VAAPI 驱动支持
ls /dev/dri/renderD*            # 检查 DRM 渲染节点
echo $XDG_SESSION_TYPE          # 确认 Wayland 环境
```

## License

MIT
