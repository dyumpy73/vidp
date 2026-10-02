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
- **Hardware Acceleration**: Built-in VAAPI hardware decoding support with graceful fallback to software decoding.
- **Advanced Subtitle Rendering**: Native ASS/SSA subtitle support with soft-shadows and styling powered by `libass`.
- **A/V Synchronization**: Audio/Video clock synchronization algorithm with frame-dropping and latency compensation.
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
make install

# Clean build artifacts
make clean
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

---

## Keyboard Shortcuts

| Key | Action |
| :--- | :--- |
| **Space** | Pause / Resume playback |
| **Left Arrow** | Seek backward 10 seconds |
| **Right Arrow** | Seek forward 10 seconds |
| **Up Arrow** | Increase volume (+10%) |
| **Down Arrow** | Decrease volume (-10%) |
| **N** | Next track in playlist |
| **P** | Previous track in playlist |
| **V** | Toggle subtitle |
| **S** | Switch subtitle stream |
| **Esc** | Quit player |

---

## Technical Highlights

- **Single-Threaded Event Loop**: The decoder, audio-push mechanism, subtitle pipeline, and window event handling all reside in a unified loop to keep state transitions perfectly predictable.
- **Hardware-Accelerated Decoding**: Leverages VAAPI contexts (`AV_HWDEVICE_TYPE_VAAPI`) for low-CPU decoding on supported GPUs.
- **Memory Recycling**: Explicit resource teardown routines (`player_close_file`) free audio buffers, hardware textures, decoders, and force glibc arena compaction via `malloc_trim(0)` between track switches.

---

## License

This project is open-source and available under the **MIT License**.
