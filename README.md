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
- `assets/vidp.desktop` — launcher entry (installed by `make install`)
- `assets/icon.svg`     — application icon (installed by `make install`)
- `Makefile`        — build & install rules

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

## Install

### Debian/Ubuntu:
```bash
wget https://github.com/dyumpy73/vidp/releases/latest/download/vidp_1.0.0_amd64.deb
sudo dpkg -i vidp_1.0.0_amd64.deb && sudo apt install -f
```

### Any Linux (portable):
```bash
wget https://github.com/dyumpy73/vidp/releases/latest/download/VidP-x86_64.AppImage
chmod +x VidP-x86_64.AppImage && ./VidP-x86_64.AppImage
```

---

## Building

```bash
# Build the executable (debug-friendly, no strip)
make

# Build with debug symbols (-g -O0)
make debug

# Build optimized release binary (-O2, stripped)
make release

# Install to /usr/local (default, requires root)
sudo make install

# Install to a custom prefix
sudo make install PREFIX=/usr          # system-wide, like distro packages
sudo make install PREFIX=$HOME/.local  # user-only, no root needed

# Uninstall (must use the same PREFIX as install)
sudo make uninstall
sudo make uninstall PREFIX=$HOME/.local

# Clean build artifacts
make clean
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
- Only ASS/SSA subtitle streams are supported (SRT/WebVTT/PGS/VobSub are ignored).
- Audio device is not re-initialized on mid-stream channel layout change.
- Hardware acceleration backends are currently Linux-only (VAAPI, CUDA, VDPAU).
- Drag & drop is only handled on the idle screen, and does not work on Wayland native. Workaround: use `Ctrl+O`, CLI args, or "Open With VidP".

---

## License

This project is open-source and available under the [GNU General Public License v2.0 or later](LICENSE).

### Third-Party Licenses
- **FFmpeg** (GPL-2.0-or-later) - [ffmpeg.org](https://ffmpeg.org/)
- **SDL3** (zlib) - [libsdl.org](https://www.libsdl.org/)
- **libass** (ISC) - [github.com/libass/libass](https://github.com/libass/libass)
