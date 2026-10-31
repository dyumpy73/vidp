# VidP (Video Player)

A lightweight C media player built with FFmpeg, SDL3, and libass. The application's playback pipeline (demux → decode → convert → render) runs on a single thread. FFmpeg's video decoder and SDL's audio device use their own internal worker threads.

## Features

- **Hardware decoding** with automatic fallback: VAAPI → CUDA → VDPAU → software (Linux). Falls back after a failed open or 10 consecutive frame transfer failures.
- **Render backend** auto-select: OpenGL → OpenGLES2 → Vulkan → software.
- **Subtitles:** ASS/SSA via libass, multiple tracks per file, embedded fonts (MKV attachments) loaded automatically.
- **A/V sync:** audio is the master clock (last pushed PTS minus queued data and device latency). Without audio (or if it starves for > 250 ms) it falls back to the wall clock, which re-anchors when drift exceeds 100 ms. Late frames are dropped (> 50 ms behind audio, > 20 ms behind the wall-clock target when no audio is available), max 15 in a row.
- **Audio:** resampled to 16-bit PCM via libswresample and played through an SDL3 audio stream. Source sample rate is preserved; surround (> 2 channels) is downmixed to stereo.
- **Dynamic resolution** handled on the fly (e.g. 720p intro → 1080p main).
- Respects **SAR** and **rotation** metadata.
- Video texture is **downscaled to the window size** when the source is much larger than the display area (see `VIDP_SCALE` below).
- Initial window sized to video, capped at 90% of usable display area.
- Subtitle seek uses a **1.5 s pre-roll** so subtitles that started shortly before the seek target still render.
- **Playlist** from folders or explicit files. Natural sort (ep2 before ep10), case-insensitive, applied per folder. Explicit CLI order preserved.
- **Idle screen + file picker** when launched without any media argument.
- **Drag & drop** of files or folders onto the idle screen (X11/XWayland).
- Cursor hides after 2 s idle. Volume persists across playlist entries.
- Memory is trimmed with `malloc_trim(0)` on track switch, on seek, and every 300 rendered frames (glibc only).

## Layout

```
src/main.c      entry point, file-to-file loop
src/player.c    open/close, main loop, seek, events, frame presentation
src/video.c     video decode + render + HW accel
src/audio.c     audio decode + resample + SDL audio stream
src/subtitle.c  libass integration
src/playlist.c  folder scan + playlist building
src/idle.c      idle screen + file picker
src/utils.c     clock, natural sort, media check, UI text cache
src/player.h    shared types, tunables, declarations
assets/         .desktop entry + .svg icon
Makefile
```

## Dependencies

Minimum versions (approximate): **FFmpeg 6.1+** (uses `AVChannelLayout` and
codec-parameter side data), **SDL3** and **SDL3_ttf** 3.x, **libass**.

Text rendering needs a TrueType font: DejaVu Sans, Liberation Sans, or Noto
Sans in the usual system paths, or any font that `fc-match sans` can resolve
(requires fontconfig). Without a font, the idle screen and volume overlay
still work but show no text.

**Arch:**
```bash
sudo pacman -S gcc pkg-config ffmpeg sdl3 sdl3_ttf libass ttf-dejavu
```

**Debian / Ubuntu:**
```bash
sudo apt install gcc pkg-config libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev libsdl3-dev libsdl3-ttf-dev libass-dev \
    fonts-dejavu-core
```

## Install

> **`.deb` note:** built on **Debian Forky** (Testing/Sid). Requires
> `libavcodec63` and `libsdl3-0`. On Debian 13 (Trixie) or older, use the
> **AppImage** instead.

**Any Linux (recommended):**
```bash
wget https://github.com/dyumpy73/vidp/releases/latest/download/VidP-x86_64.AppImage
chmod +x VidP-x86_64.AppImage
./VidP-x86_64.AppImage
```

**Debian Forky / Testing / Sid:**
```bash
wget https://github.com/dyumpy73/vidp/releases/latest/download/vidp_0.0.4_amd64.deb
sudo dpkg -i vidp_0.0.4_amd64.deb
sudo apt install -f
```

## Building

```bash
make            # debug-friendly, unstripped
make debug      # -g -O0
make release    # -O2, stripped
make clean

sudo make install                     # → /usr/local
sudo make install PREFIX=/usr         # system-wide
sudo make install PREFIX=$HOME/.local # user-only, no root
sudo make uninstall [PREFIX=...]      # must match install PREFIX
```

Manual build without Make:
```bash
gcc -O2 -Wall -Wextra src/*.c -o vidp \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil \
      libswscale libswresample sdl3 sdl3-ttf libass) -lm -lpthread
```

## Usage

```bash
vidp path/to/video.mkv         # single file
vidp path/to/anime_folder/     # whole directory
vidp ep1.mkv ep2.mkv ep3.mkv   # explicit files
vidp                           # idle screen
```

- Folders are scanned non-recursively; hidden files are skipped.
- Extensions: `mkv mp4 avi webm mov flv m4v ts wmv mpg mpeg ogv`. Note that `.ts` also matches TypeScript source files in a scanned folder.
- Arguments that do not exist or lack a supported extension are ignored. If none remain, the idle screen is shown. A file with a supported extension that fails to open is skipped with an error message.
- The window title shows the playlist position and file name.

### Environment variables

| Variable | Effect |
| :--- | :--- |
| `VIDP_SCALE=off` (or `0`) | Disable downscaling the video texture to window size. |
| `VIDP_AUDIO_MARGIN_MS=<ms>` | Safety margin used when video waits on the audio buffer (default `60`). |

## Keyboard Shortcuts

### Playback

| Key | Action |
| :--- | :--- |
| **Space** | Pause / resume |
| **Left / Right** | Seek −10 s / +10 s (Right near the end → next track) |
| **Up / Down** | Volume ±10 % (cap 100 %) |
| **F** | Toggle fullscreen |
| **N** | Next track (no effect on the last one) |
| **P** | Previous track (restarts the file if on the first one) |
| **V** | Toggle subtitles |
| **S** | Cycle subtitle streams (last step = off) |
| **Esc** | Quit |

### Idle screen

| Key | Action |
| :--- | :--- |
| **Ctrl+O** | Open file picker |
| **Esc** | Quit |

### File picker

Selections persist while navigating between folders, so files from several
folders can be queued in one go.

| Key | Action |
| :--- | :--- |
| **Up / Down / PageUp / PageDown / Home / End** | Move cursor |
| **Enter** | On a folder: open it. On a file: play all selected files (or the cursor file if nothing is selected) |
| **Backspace** | Parent folder |
| **Space** (hold) + arrows | Range select / unselect (depends on the state of the first item), release to commit |
| **Ctrl+A** | Select / unselect all files in the current folder |
| **Ctrl+Enter** | Play all media in the folder under the cursor (or the current folder if the cursor is on a file); ignores selection, non-recursive |
| **Esc** | Cancel |

## Known Limitations

- Single audio track (the first one) — no runtime switching.
- Only ASS/SSA subtitles (SRT/WebVTT/PGS/VobSub ignored); no external subtitle files.
- Subtitle events that started before the 1.5 s pre-roll window are not restored after a seek.
- Files without a video stream (audio-only) are not supported.
- No loop/repeat; the player exits after the last playlist entry.
- No keyboard-free controls: no mouse seeking or on-screen seek bar.
- HDR content is converted to 8-bit without tone mapping.
- Hardware-decoded frames are copied back to system memory (no zero-copy).
- Audio device is not re-initialized on a mid-stream channel layout change.
- Hardware acceleration is Linux-only (VAAPI, CUDA, VDPAU).

## License

[GNU General Public License v2.0 or later](LICENSE).

Third-party:
- **FFmpeg** — GPL-2.0-or-later — [ffmpeg.org](https://ffmpeg.org/)
- **SDL3** — zlib — [libsdl.org](https://www.libsdl.org/)
- **libass** — ISC — [github.com/libass/libass](https://github.com/libass/libass)
