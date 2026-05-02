# AGENTS.md

## Build

```bash
cd build && cmake .. && make -j$(nproc)
# Release (default, -O3 -DNDEBUG):
cmake .. -DCMAKE_BUILD_TYPE=Release
# Debug (-Og -g):
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

If you change build type, delete `build/*` first — cmake caches the previous type.

Zero warnings required. `-Wall -Wextra` is enforced.

## Run

```bash
cd build && ./multi_video_player <video1> [video2] ...
```

Must run under **Wayland** (check: `echo $XDG_SESSION_TYPE`). X11 / headless will fail. Videos should be placed in `videos/` (gitignored).

## Architecture

Single window, single EGL/GLES2 context, single-threaded render loop. N decoder threads produce frames asynchronously.

```
WaylandWindow (libdecor decorated)
  └── EGLRenderer (single EGLContext + EGLSurface)
        ├── m_playerTexY[i] + m_playerTexUV[i]   (DMA-BUF NV12 per player)
        └── m_cpuTexY[i] + m_cpuTexU[i] + m_cpuTexV[i] (CPU YUV420P per player)
```

Each PlayerWindow wraps a VaapiDecoder. `startAll(dmaCb, cpuCb)` registers both callbacks — priority is DMA-BUF, falls back to CPU for unsupported codecs (MPEG4, VP8).

## Critical gotchas

### Frame callback ordering (Wayland)

**Must call `scheduleFrame()` BEFORE `eglSwapBuffers()`**, not after. On Wayland, `eglSwapBuffers` performs the surface commit. The frame callback (`wl_surface_frame`) must be registered before the commit or it waits for a commit that never comes — render loop blocks forever.

Correct sequence per frame:
```
scheduleFrame() → swapBuffers() → awaitFrame()
```

### Render loop must always draw + swap at vsync rate

The render loop runs at display refresh rate (vsync). Each iteration renders ALL grid cells regardless of whether a new frame arrived. If no new frame for a player, re-render from its existing GL textures — never leave a cell black. Skipping swaps causes flicker.

### playerLastType tracking

Each player tracks `{NONE, DMA, CPU}` to know which texture set to reuse when no new frame arrives:

- New DMA-BUF frame → `renderDmaBufToRect` (updates textures + draws)
- New CPU frame → `renderCpuFrameToRect` (uploads to per-player CPU textures + draws)
- No frame, last was DMA → `renderDmaBufToRectPrev` (draws from existing DMA-BUF textures)
- No frame, last was CPU → `renderCpuFrameToRectPrev` (draws from existing CPU textures)
- No frame, NONE → skip (cell stays black from clear)

Using the wrong `*Prev` method causes green flicker (rendering from uninitialized textures).

### DMA-BUF zero-copy path

Hardware decode → `vaExportSurfaceHandle` → DRM PRIME fds → `eglCreateImage(EGL_LINUX_DMA_BUF_EXT)` → `glEGLImageTargetTexture2DOES` → NV12 shader. The `DmaBufFrame` destructor closes the original fds; Mesa's EGL dups them internally so textures remain valid.

VAAPI hardware decode only works for **H.264 and HEVC** on Intel iHD driver. Other codecs silently fall back to software decode → YUV420P CPU textures.

### `getLatestFrame` consumes the frame (move semantics)

Both `getLatestFrame(DmaBufFrame&)` and `getLatestCpuFrame(VideoFrame&)` **move** the frame out. Calling them to "check" whether a frame exists will consume it. Never check-then-get in two calls — fetch once and branch on the result.

## File map

| File | Role |
|------|------|
| `main.cpp` | Grid layout, render loop, cell borders, aspect-fit |
| `wayland_window.cpp/h` | libdecor window, `scheduleFrame/awaitFrame` vsync sync |
| `egl_renderer.cpp/h` | EGL/GLES2, DMA-BUF import, NV12/YUV420P shaders, per-player textures |
| `vaapi_decoder.cpp/h` | FFmpeg+VAAPI decode, DMA-BUF export, CPU fallback, audio decode |
| `player_window.cpp/h` | Decoder wrapper, dual frame buffer (DMA + CPU) |
| `audio_output.cpp/h` | PulseAudio simple API |
