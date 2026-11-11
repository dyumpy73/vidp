# VidP (Video Player)

Lightweight C media player built with FFmpeg, SDL3, and libass.

## Features

- Multi-threaded pipeline: demuxer + video decoder + audio decoder + main render thread
- Hardware decode: VAAPI → CUDA → VDPAU → software (auto-fallback)
- Render: OpenGL → OpenGLES2 → Vulkan → software (auto-select)
- ASS/SSA subtitles via libass, embedded fonts supported
- Audio master clock, wall-clock fallback
- Adaptive queue sizing based on RAM/CPU
- Seek with stale-packet guard (no H.264 reference warnings)
- Persistent demuxer — seek back after EOF works
- Playlist from folders or explicit files, natural sort
- Idle screen with file picker, drag & drop
- Texture downscale to window size

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

FFmpeg 6.1+, SDL3, SDL3_ttf, libass. A TrueType font (DejaVu/Liberation/Noto) for UI text.

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
wget https://github.com/dyumpy73/vidp/releases/latest/download/vidp_0.1.0_amd64.deb
sudo dpkg -i vidp_0.1.0_amd64.deb
sudo apt install -f
```

## Building

```bash
make            # debug-friendly, unstripped
make debug      # -g -O0
make release    # -O2, stripped
make clean

sudo make install                     # system-wide
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
vidp file.mkv         # single file
vidp folder/          # directory
vidp ep1.mkv ep2.mkv  # explicit files
vidp                  # idle screen
```

- Folders are scanned non-recursively; hidden files are skipped.
- Extensions: `mkv mp4 avi webm mov flv m4v wmv mpg mpeg ogv`.

### Environment variables

| Variable | Effect |
| :--- | :--- |
| `VIDP_SCALE=off` (or `0`) | Disable downscaling the video texture to window size. |
| `VIDP_THREADS=<n>` | Video decoder threads (default 2 on HW, min(4,cores) on SW). |
| `VIDP_FRAME_QUEUE=<n>` | Frame queue size, 2–8. |
| `VIDP_PACKET_QUEUE=<n>` | Packet queue size, 4–32. |
| `VIDP_LOWMEM=1` | Force minimal queues. |

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

- Single audio track only (first one).
- ASS/SSA subtitles only, no external files.
- No audio-only files.
- No loop.
- No mouse controls.
- HDR → 8-bit without tone mapping (no zero-copy HW decode).

## License

[GNU General Public License v2.0 or later](LICENSE).

Third-party:
- **FFmpeg** — GPL-2.0-or-later — [ffmpeg.org](https://ffmpeg.org/)
- **SDL3** — zlib — [libsdl.org](https://www.libsdl.org/)
- **libass** — ISC — [github.com/libass/libass](https://github.com/libass/libass)
