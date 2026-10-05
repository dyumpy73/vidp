# VidP (Video Player)

A lightweight, single-threaded media player built in C using **FFmpeg**, **SDL2**, and **libass**. Designed with a focus on simplicity, low memory footprint, and deterministic state management without threading complexity.

![C](https://img.shields.io/badge/Language-C-blue.svg)
![FFmpeg](https://img.shields.io/badge/Library-FFmpeg-green.svg)
![SDL2](https://img.shields.io/badge/Library-SDL2-blue.svg)
![libass](https://img.shields.io/badge/Library-libass-red.svg)
![License](https://img.shields.io/badge/License-MIT-brightgreen.svg)

---

## Features

- **Single-Threaded Architecture**: Eliminates race conditions and concurrency overhead while maintaining smooth playback.
- **Multi-Backend Hardware Acceleration**: Hardware decoding support with fallback priority (VAAPI, CUDA, VDPAU) to software decoding.
- **Auto-Switching Display Backend**: Seamlessly initializes the best available GPU renderer (OpenGL -> OpenGLES2 -> Vulkan) with automatic software fallback.
- **Advanced Subtitle Rendering**: Native ASS/SSA subtitle support with soft-shadows and styling powered by `libass`.
- **A/V Synchronization**: Audio/Video clock synchronization algorithm with frame-dropping and latency compensation.
- **Robust Audio Pipeline**: 256 KB ring buffer (~1.3 s @ 48 kHz stereo) with "drop-oldest" overflow strategy to survive video-sync blocking without stutter.
- **Dynamic Resolution Support**: Handles mid-playback resolution changes (mixed-resolution anime, OVA, compilation movies) by re-initializing scaler, texture, and subtitle surface on the fly.
- **Playlist & Directory Scanning**: Automatically builds and sorts playlists from directory inputs or multiple file arguments.
- **Low Memory Overhead**: Conservative working set memory footprint optimized with deterministic allocation cleanup (malloc_trim).
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

# Install to system (optional, requires root)
sudo make install

# Clean build artifacts
make clean

# Uninstall from system
sudo make uninstall
```

Alternatively, to compile manually without Make:

```bash
gcc -O2 -Wall src/vidp.c -o vidp \
    $(pkg-config --cflags --libs libavcodec libavformat libswscale libswresample sdl2 libass) -lm
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

- **Single-Threaded Event Loop**: The decoder, audio-push mechanism, subtitle pipeline, and window event handling all reside in a unified loop to keep state transitions perfectly predictable.
- **Dynamic Renderer Fallback**: Automatically negotiates GPU rendering backends at runtime via SDL2 hints, ensuring Vulkan/OpenGL acceleration on modern drivers while maintaining compatibility with legacy system.
- **Zero-Copy VRAM Discarding**: Fast seeking optimization on long-GOP streams (e.g., HEVC/x265). Discards pre-target frames directly in GPU memory before invoking PCIe transfers via `av_hwframe_transfer_data`, eliminating seek latency.
- **Dynamic NV12/IYUV Texture Allocation**: Dynamically switches SDL texture pixel formats on-the-fly to match hardware acceleration outputs (NV12) or software decoding fallbacks (IYUV).
- **Dynamic Resolution Handling**: Detects mid-stream resolution changes (common in mixed-resolution anime) and safely re-allocates the scaler context, video texture, and subtitle surface without leaking or crashing.
- **Full-Screen Subtitle Surface**: Subtitle rendering uses a fixed-size RGBA surface matching the video display rect. This trades ~8 MB constant memory (1080p) for elimination of per-frame `realloc()` churn — a net win on long playback sessions.
- **Memory Recycling**: Explicit resource teardown routines (`player_close_file`) free audio buffers, hardware textures, decoders, and force glibc arena compaction via `malloc_trim(0)` between track switches.

---

## Known Limitations

- Single audio track only — no runtime track switching.
- No subtitle delay adjustment (subtitle sync offset).
- No hardware-accelerated subtitle blending (rendering is done on CPU via libass).
- Audio device is not re-initialized on mid-stream channel layout change (stereo → 5.1).

---

## License

This project is open-source and available under the **MIT License**.
