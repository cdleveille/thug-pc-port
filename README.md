# THUG PC Port

A native Windows and Linux port of **Tony Hawk's Underground** (2003), built by
statically recompiling the original Xbox executable into C with the
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit. It does not
emulate anything: the game's own code runs natively.

**Status: early bring-up.** On Linux, the recompiled game boots with sound
and controller support, navigates its menus and plays free skate. An OpenGL
renderer draws it at your display's resolution, in widescreen, with the frame
rate unlocked. See [docs/BRINGUP.md](docs/BRINGUP.md) for what works and
what doesn't.

To build and play:

```
scripts/build.sh     # rebuild whatever is out of date
scripts/play.sh      # play; the log goes to stderr.txt
```

In game, **F1** (or Back+Start held together on a pad) opens the settings
overlay:
- **Display:** resolution, antialiasing (2x, 4x or 8x MSAA), brightness,
  fullscreen, vsync, a frame rate limit, anisotropic filtering, widescreen.
- **Audio:** volume, and muting while the window is in the background.
- **Controller:** vibration strength.
- **Game:** the original 60 Hz timing, an FPS counter.
- Live performance figures, and **Restore defaults**.

Changes apply at once (widescreen at the next start) and are saved to
`~/.config/thug-pc-port/settings.ini` when the overlay closes. While it is
open the game gets no input and the pad does not vibrate. In a window, the
mouse cursor hides after two seconds without moving.

`scripts/play.sh --help` lists its options (`--fullscreen`, `--4x3`,
`--scale <x>`, `--no-vsync`). They, and the variables below, override the
saved settings for one run.

| Key or switch | Effect |
|---|---|
| F11 or Alt+Enter | Toggle borderless fullscreen |
| `THUG_FULLSCREEN=1` | Start fullscreen |
| `THUG_WIDESCREEN=0` | 4:3 instead of 16:9 |
| `THUG_RES_SCALE=<x>` | Internal resolution, as a multiple of 480 lines (default: your desktop's height) |
| `THUG_VSYNC=0` | Don't wait for the display |
| `THUG_FPS_LIMIT=<n>` | Cap the frame rate (0 = off) |
| `THUG_SHOW_FPS=1` | FPS counter in the corner |
| `THUG_ANISO=<n>` | Anisotropic filtering (default 16) |
| `THUG_MSAA=<n>` | Antialiasing: 1 (off, default), 2, 4 or 8 samples |
| `THUG_BRIGHTNESS=<b>` | Gamma on the final image, 0.5 to 2 (default 1) |
| `THUG_VOLUME=<n>` | Master volume, 0 to 100 (default 100) |
| `THUG_MUTE_UNFOCUSED=1` | Silent while the window is in the background |
| `THUG_RUMBLE=<n>` | Vibration strength, 0 (off) to 100 (default 100) |
| `THUG_VBLANK_TIMING=1` | The original 60 Hz game timing (runs fast above 60 fps) |
| `RECOMP_PB_GL=0` | The old CPU renderer (slow) |

Windows builds are planned.

## You need

- Your own **Tony Hawk's Underground (USA) Xbox disc image**. This repository
  contains no game code or data, and no generated code derived from it.
  - Supported `default.xbe`: SHA-1 `2951de954c96a8b962ba1cde34548fa82ae668d7`
- Linux x86-64 with:
  - clang or GCC
  - CMake 3.20 or newer
  - Ninja
  - SDL2 and libepoxy
  - Python 3.10 or newer with `capstone`

  On Arch:

  ```
  sudo pacman -S --needed git python python-capstone cmake ninja clang sdl2 libepoxy
  ```

## Build

```
git clone --recursive <this repo>
cd thug-pc-port
cp "/path/to/Tony Hawk's Underground (USA).iso" rom/
scripts/build.sh
scripts/play.sh
```

`scripts/build.sh` runs the steps below as needed: it applies the toolkit
patches, extracts the disc the first time, lifts `default.xbe` to C (a few
minutes; again when `src/recomp_manual.c` or the seed functions change, or
with `--regen`), configures CMake and compiles. By hand:

```
scripts/apply-patches.sh    # local fixes to the toolkit, pending upstream
scripts/extract.sh          # unpack the disc into game/ and verify the XBE
scripts/regen.sh            # lift default.xbe to C in src/recomp/gen/
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/bin/thug 2> stderr.txt
```

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz and its
  contributors (MIT; parts LGPL-2.1+ from xemu)
- [thug-decomp](https://github.com/andrewnakas/thug-decomp), used as a
  reference for the shared Neversoft codebase

Tony Hawk's Underground is © Activision and Neversoft. This is an unofficial
fan project, not affiliated with or endorsed by either.
