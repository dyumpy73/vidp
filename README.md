# VidP

A small video player written in C, built on **FFmpeg**, **SDL2**, and **libass**. Application logic runs in a single thread, while FFmpeg and SDL handle their own internal threads.

---

## Features

- Hardware decoding with fallback: VAAPI → CUDA → VDPAU → software (Linux only).
- Automatic render backend: OpenGL → OpenGLES2 → software.
- Native ASS/SSA subtitles via libass.
- A/V sync uses the wall clock, auto re-anchors if drift exceeds 100ms. Late frames get dropped.
- 256 KB audio ring buffer (~1.3s @ 48kHz stereo), drop-oldest when full.
- Dynamic resolution (720p opening → 1080p main content) handled on the fly.
- Subtitle seek uses a 5-second pre-roll so overlapping subtitles still get read.
- Playlist from a folder or multiple files, natural sort (ep2 before ep10).
- Idle screen + in-app file picker when launched with no arguments.
- Drag & drop (works on X11/XWayland, see Known limitations).
- `malloc_trim(0)` on every track switch to hand memory back to the OS.

---

## Layout

- `src/main.c`      — entry point, file-to-file loop
- `src/player.c`    — open/close file, main loop, seek, events
- `src/video.c`     — video decode + render + hw accel
- `src/audio.c`     — audio decode + ring buffer + resample
- `src/subtitle.c`  — libass integration
- `src/playlist.c`  — folder scanning, playlist building
- `src/idle.c`     — idle screen + file picker
- `src/utils.c`     — small utilities
- `src/player.h`    — shared types and declarations
---

## Dependencies

Ensure you have the required libraries and header files installed on your system.

### Arch Linux
```bash
sudo pacman -S gcc pkg-config ffmpeg sdl2 sdl2_ttf libass
```

### Debian / Ubuntu
```bash
sudo apt update
sudo apt install gcc pkg-config libavcodec-dev libavformat-dev libswscale-dev libswresample-dev libsdl2-dev libsdl2-ttf libass-dev
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

## Known Limitations

- Single audio track only — no runtime track switching.
- No subtitle delay adjustment (subtitle sync offset).
- No hardware-accelerated subtitle blending (rendering is done on CPU via libass).
- Audio device is not re-initialized on mid-stream channel layout change (stereo → 5.1).
- Hardware acceleration backends are currently Linux-only (VAAPI, CUDA, VDPAU).
- Subtitle pre-roll during seek may visually stall on very high-bitrate 4K content; the duration is configurable via `SUBTITLE_PREROLL_SEC` in `src/player.h`.
- Drag & drop does not work on Wayland native, Workarounds: use `Ctrl+O`, pass files as arguments, or "Open With VidP" from your file manager. The DnD code is kept intact and will work once SDL fixes it.

---

## License

This project is open-source and available under the **MIT License**.
