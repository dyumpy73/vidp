# VidP (Video Player)

A lightweight media player built in C using **FFmpeg**, **SDL2**, and **libass**. 
Application logic runs in a single thread for predictable state transitions, 
with FFmpeg and SDL handling their own isolated internal threads.

![C](https://img.shields.io/badge/Language-C-blue.svg)
![FFmpeg](https://img.shields.io/badge/Library-FFmpeg-green.svg)
![SDL2](https://img.shields.io/badge/Library-SDL2-blue.svg)
![libass](https://img.shields.io/badge/Library-libass-red.svg)
![License](https://img.shields.io/badge/License-MIT-brightgreen.svg)

---

## Features

- **Single-Threaded Application Loop**: Application logic (demux, sync, render, event handling) runs in a single thread, eliminating application-level race conditions. FFmpeg internal decoder threads (2) and SDL audio callback thread remain as isolated subsystems.
- **Multi-Backend Hardware Acceleration (Linux)**: Hardware decoding with priority fallback VAAPI → CUDA → VDPAU → software. Non-Linux platforms currently use software decoding only.
- **Auto-Switching Display Backend**: Seamlessly initializes the best available GPU renderer (OpenGL -> OpenGLES2 -> Vulkan) with automatic software fallback.
- **Advanced Subtitle Rendering**: Native ASS/SSA subtitle support with soft-shadows and styling powered by `libass`.
- **Wall-Clock A/V Synchronization**: Video frame PTS is compared against monotonic system time, with automatic clock re-anchoring when drift exceeds 100 ms. Includes frame dropping and delay compensation for transient timing spikes.
- **Robust Audio Pipeline**: 256 KB ring buffer (~1.3 s @ 48 kHz stereo) with "drop-oldest" overflow strategy to survive fast demuxing. Prolonged video-sync stalls beyond the buffer length can still cause brief underrun.
- **Dynamic Resolution Support**: Handles mid-playback resolution changes (mixed-resolution anime, OVA, compilation movies) by re-initializing scaler, texture, and subtitle surface on the fly.
- **Seek with Subtitle Pre-Roll**: Rewinds a configurable pre-roll duration (default 5 s) before the target PTS so subtitle chunks that overlap the seek point are correctly loaded.
- **Playlist & Directory Scanning**: Automatically builds and sorts playlists from directory inputs or multiple file arguments.
- **Deterministic Memory Management**: Explicit resource teardown between tracks with `malloc_trim(0)` to release glibc arena back to the OS. Working set stays bounded across playlist transitions.

---

## Architecture

- `src/main.c`      — entry point, playlist loop
- `src/player.c`    — orchestration: open/close/run loop, seek, events
- `src/video.c`     — video decode + render + HW accel
- `src/audio.c`     — audio decode + ring buffer + resample
- `src/subtitle.c`  — libass integration, parallel track rendering
- `src/playlist.c`  — directory scanning, playlist building
- `src/utils.c`     — common utilities
- `src/player.h`    — shared types and declarations

---

## Dependencies

Ensure you have the required libraries and header files installed on your system.

### Arch Linux
```bash
sudo pacman -S gcc pkg-config ffmpeg sdl2 libass
```

### Debian / Ubuntu
```bash
sudo apt update
sudo apt install gcc pkg-config libavcodec-dev libavformat-dev libswscale-dev libswresample-dev libsdl2-dev libass-dev
```

---

## Building

You can build the project using `make`:

```bash
# Build the executable
make

# Build with debug symbols
make debug

# Build optimized release binary (stripped)
make release

# Install to system (optional, requires root)
sudo make install

# Clean build artifacts
make clean

# Uninstall from system
sudo make uninstall
```

Alternatively, to compile manually without Make:

```bash
gcc -O2 -Wall -o vidp \
    src/main.c src/player.c src/video.c src/audio.c \
    src/subtitle.c src/playlist.c src/utils.c \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil \
      libswscale libswresample sdl2 libass) -lm
```

---

## Usage

Pass one or more video files, or a folder containing media files:

```bash
# Play a single file
vidp path/to/video.mkv

# Play an entire directory
vidp path/to/anime_folder/

# Play multiple specific files
vidp episode1.mkv episode2.mkv episode3.mkv
```

> **Note:** VidP handles mid-stream resolution changes automatically (e.g. 720p opening → 1080p main content). You'll see a log line like `[VidP] Dynamic resolution change: 1280x720 -> 1920x1080` when this happens.

---

## Keyboard Shortcuts

| Key | Action |
| :--- | :--- |
| **Space** | Pause / Resume playback |
| **Left Arrow** | Seek backward 10 seconds |
| **Right Arrow** | Seek forward 10 seconds |
| **Up Arrow** | Increase volume (+10%) |
| **Down Arrow** | Decrease volume (-10%) |
| **F** | Toggle fullscreen mode |
| **N** | Next track in playlist |
| **P** | Previous track in playlist |
| **V** | Toggle subtitle |
| **S** | Switch subtitle stream |
| **Esc** | Quit player |

---

## Technical Highlights

- **Single-Threaded Event Loop**: Decoder orchestration, audio-push mechanism, subtitle pipeline, and window event handling all reside in a unified loop to keep state transitions predictable.
- **Dynamic Renderer Fallback**: Automatically negotiates GPU rendering backends at runtime via SDL2 hints, ensuring Vulkan/OpenGL acceleration on modern drivers while maintaining compatibility with legacy systems.
- **Pre-Transfer Frame Discarding**: During seek, frames before the target PTS are dropped while still in GPU memory (before `av_hwframe_transfer_data`), avoiding unnecessary PCIe round-trips on long-GOP HEVC/x265 streams.
- **Dynamic NV12/IYUV Texture Allocation**: Dynamically switches SDL texture pixel formats on-the-fly to match hardware acceleration outputs (NV12) or software decoding fallbacks (IYUV).
- **Dynamic Resolution Handling**: Detects mid-stream resolution changes (common in mixed-resolution anime) and safely re-allocates the scaler context, video texture, and subtitle surface without leaking or crashing.
- **Reusable Subtitle Surface**: Subtitle rendering reuses a single RGBA surface sized to match the current video display rect. The surface is only re-allocated when dimensions change, avoiding per-frame `realloc()` churn. Memory cost scales with display size (~8 MB at 1920×1080).
- **Memory Recycling**: Explicit resource teardown routines (`player_close_file`) free audio buffers, hardware textures, decoders, and force glibc arena compaction via `malloc_trim(0)` between track switches.

---

## Known Limitations

- Single audio track only — no runtime track switching.
- No subtitle delay adjustment (subtitle sync offset).
- No hardware-accelerated subtitle blending (rendering is done on CPU via libass).
- Audio device is not re-initialized on mid-stream channel layout change (stereo → 5.1).
- Hardware acceleration backends are currently Linux-only (VAAPI, CUDA, VDPAU).
- Subtitle pre-roll during seek may visually stall on very high-bitrate 4K content; the duration is configurable via `SUBTITLE_PREROLL_SEC` in `src/player.h`.

---

## License

This project is open-source and available under the **MIT License**.
