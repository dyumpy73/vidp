# VidP (Video Player)

A small video player written in C, built on **FFmpeg**, **SDL3**, and **libass**. Application logic runs in a single thread, while FFmpeg and SDL handle their own internal threads.

---

## Features

- Hardware decoding with fallback: VAAPI → CUDA → VDPAU → software (Linux only). If the hardware decoder fails to open, or 10 consecutive frame transfers fail, VidP falls back to software decoding automatically.
- Automatic render backend: OpenGL → OpenGLES2 → Vulkan → software.
- Native ASS/SSA subtitles via libass, with multiple subtitle tracks per file and fonts embedded in the container (e.g. MKV attachments) loaded automatically.
- A/V sync: when a file has audio, the audio stream is the master clock (decoded PTS minus the data still queued in the SDL audio stream). Without audio, VidP falls back to the wall clock and re-anchors automatically if drift exceeds 100ms. Late frames are dropped (more than 50ms behind audio, or 20ms behind the wall clock), never more than 15 in a row.
- Audio is resampled to 16-bit PCM with libswresample and played through an SDL3 audio stream, keeping the source sample rate and channel count. No custom ring buffer.
- Dynamic resolution (720p opening → 1080p main content) handled on the fly.
- Respects pixel aspect ratio (SAR) and rotation metadata (`rotate` tag / display matrix).
- Initial window is sized to the video, capped at 90% of the primary display's usable area.
- Subtitle seek uses a 1.5-second pre-roll (`SUBTITLE_PREROLL_SEC`) so overlapping subtitles still get read.
- Playlist from a folder or multiple files. Files found inside a folder are sorted naturally and case-insensitively (ep2 before ep10); files passed explicitly on the command line keep the order you gave.
- Idle screen + in-app file picker when launched with no (valid) arguments.
- Drag & drop onto the idle screen (works on X11/XWayland, see Known Limitations).
- Mouse cursor auto-hides after 2 seconds of inactivity.
- Volume is kept when moving between playlist entries.
- `malloc_trim(0)` on every track switch to hand memory back to the OS (glibc only).

---

## Layout

- `src/main.c`      — entry point, file-to-file loop
- `src/player.c`    — open/close file, main loop, seek, events, frame presentation
- `src/video.c`     — video decode + render + hw accel
- `src/audio.c`     — audio decode + resample + SDL audio stream
- `src/subtitle.c`  — libass integration
- `src/playlist.c`  — folder scanning, playlist building
- `src/idle.c`      — idle screen + file picker
- `src/utils.c`     — small utilities (clock, natural sort, media file check, blank cursor)
- `src/player.h`    — shared types, tunables and declarations

---

## Dependencies

Ensure you have the required libraries and header files installed on your system.

The idle screen and file picker render text with SDL3_ttf and look for one of these fonts: DejaVu Sans, Liberation Sans, or Noto Sans (in the usual `/usr/share/fonts/...` locations). If none is found, the idle screen still works but shows no text.

### Arch Linux
```bash
sudo pacman -S gcc pkg-config ffmpeg sdl3 sdl3_ttf libass ttf-dejavu
```

### Debian / Ubuntu
```bash
sudo apt update
sudo apt install gcc pkg-config libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev libsdl3-dev libsdl3-ttf-dev libass-dev \
    fonts-dejavu-core
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
gcc -O2 -Wall -Wextra src/*.c -o vidp \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil \
      libswscale libswresample sdl3 sdl3-ttf libass) -lm -lpthread
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

# No arguments: open the idle screen
vidp
```

- Folders are scanned non-recursively; hidden files (starting with `.`) are skipped.
- Recognized extensions: `mkv`, `mp4`, `avi`, `webm`, `mov`, `flv`, `m4v`, `ts`, `wmv`, `mpg`, `mpeg`, `ogv`. Anything else passed as an argument is ignored.
- If no playable file results from the arguments, the idle screen is shown.

> **Note:** VidP handles mid-stream resolution changes automatically (e.g. 720p opening → 1080p main content). You'll see a log line like `[VidP] Dynamic resolution change: 1280x720 -> 1920x1080` when this happens.

---

## Keyboard Shortcuts

### Playback

| Key | Action |
| :--- | :--- |
| **Space** | Pause / Resume playback |
| **Left Arrow** | Seek backward 10 seconds (while paused, shows the new frame) |
| **Right Arrow** | Seek forward 10 seconds; seeking past the end moves to the next track |
| **Up Arrow** | Increase volume (+10%, max 100%) |
| **Down Arrow** | Decrease volume (-10%) |
| **F** | Toggle fullscreen mode |
| **N** | Next track in playlist |
| **P** | Previous track in playlist (on the first track: restart from the beginning) |
| **V** | Show / hide subtitles |
| **S** | Cycle through subtitle streams (the last step turns subtitles off) |
| **Esc** | Quit player |

### Idle screen

| Key | Action |
| :--- | :--- |
| **Ctrl+O** | Open the file picker |
| **Esc** | Quit |

### File picker

| Key | Action |
| :--- | :--- |
| **Up / Down / PageUp / PageDown / Home / End** | Move the cursor |
| **Enter** | Open folder, or play the selected files (or the file under the cursor if none are selected) |
| **Backspace** | Go to the parent folder |
| **Space** (hold) + arrows | Select / unselect a range of files; release to commit |
| **Ctrl+A** | Select / unselect all files in the folder |
| **Ctrl+Enter** | Play every media file in the folder under the cursor (or the current folder) |
| **Esc** | Cancel |

---

## Known Limitations

- Single audio track only — no runtime track switching.
- Only ASS/SSA subtitle streams are supported. Text formats such as SRT / WebVTT / `mov_text` and image-based formats such as PGS / VobSub are ignored.
- No subtitle delay adjustment (subtitle sync offset).
- No hardware-accelerated subtitle blending (rendering is done on CPU via libass).
- Audio output uses the file's own channel count and sample rate; the audio device is not re-initialized on a mid-stream channel layout change (stereo → 5.1).
- Volume cannot go above 100%.
- Hardware acceleration backends are currently Linux-only (VAAPI, CUDA, VDPAU).
- Subtitle pre-roll during seek may visually stall on very high-bitrate 4K content; the duration is configurable via `SUBTITLE_PREROLL_SEC` in `src/player.h`.
- Drag & drop is only handled on the idle screen (not during playback), and does not work on Wayland native. Workarounds: use `Ctrl+O` on the idle screen, pass files as arguments, or "Open With VidP" from your file manager. The DnD code is kept intact and will work once SDL fixes it.

---

## License

This project is open-source and available under the **MIT License**.
