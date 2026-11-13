# THUG PC Port

A native Windows and Linux port of **Tony Hawk's Underground** (2003), built by
statically recompiling the original Xbox executable into C with the
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit. It does not
emulate anything: the game's own code runs natively.

**Status: early bring-up.** On Linux, the recompiled game boots with sound
and controller support, navigates its menus and plays free skate. An OpenGL
renderer draws it at your display's resolution, in widescreen, with the frame
rate unlocked. The Windows build runs the same renderer and reaches free
skate in scripted runs; it still needs a full playtest. See
[docs/BRINGUP.md](docs/BRINGUP.md) for what works and what doesn't.

To build and play, put your disc image in `game/`, then on Linux:

```
scripts/build.sh     # rebuild whatever is out of date
build/bin/thug       # play
```

On Windows, from any shell (cmd, PowerShell):

```
scripts\build.bat     # installs the toolchain if missing, then rebuilds whatever is out of date
build\bin\thug.exe    # play
```

## Running the game

Run `thug`. It looks for the extracted game files in a `game/` folder next to it (or in the
tree above it, so `build/bin/thug` finds the repository's `game/`):

- **Only the disc image** (a Redump dump or a trimmed XISO, `.iso`), either
  beside `thug` or in `game/`: on first launch the game creates `game/` if
  need be, unpacks the image into it (about 3.4 GB, with a progress window),
  checks that `default.xbe` is the supported USA build, and starts. The image
  can stay or go afterwards.
- **The unpacked files** (`game/default.xbe` and `game/data/`, with or
  without the image): `default.xbe` is checked and the game starts.
- **Neither**, or another version of the game: it says so and exits.

An interrupted unpack is noticed and done again (`default.xbe` is written
last). `THUG_GAME_DIR=<dir>` points the game somewhere else.

The game's log goes to `~/.local/state/thug-pc-port/thug.log` on Linux and
`%LOCALAPPDATA%\thug-pc-port\thug.log` on Windows (the previous run's is
`thug.prev.log`) unless you redirect stderr yourself; `THUG_LOG=<file>` picks
another file, `THUG_LOG=-` keeps it on stderr.

In game, **F1** (or **L3 + R3**, both sticks clicked, on a pad) opens the settings
overlay:
- **Display:** aspect ratio (4:3, 16:10 or 16:9; the field of view widens
  to fit), resolution (common sizes for the chosen aspect, or native),
  antialiasing (2x, 4x or 8x MSAA), smooth text and icons (the game's
  640x480-era text, icons and logos enlarged with xBR, so their outlines are
  smooth curves rather than soft staircases), brightness, fullscreen, vsync,
  a frame rate limit, anisotropic filtering.
- **Audio:** volume, and muting while the window is in the background.
- **Controller:** vibration strength.
- **Game:** the original 60 Hz timing, an FPS counter.
- Live performance figures, and **Restore defaults**.

Changes apply at once, except a switch between 4:3 and a widescreen aspect
(the game lays out its menus and HUD at start-up): then a **Restart to apply
changes** button appears, which asks first, since progress since your last
save is lost. Settings are saved to
`~/.config/thug-pc-port/settings.ini` (Windows:
`%APPDATA%\thug-pc-port\settings.ini`) when the overlay closes. While it is
open the game gets no input and the pad does not vibrate. In a window, the
mouse cursor hides after two seconds without moving. The window reopens as
you left it: its size, whether it was maximised or fullscreen, and its
position where the desktop allows a program to choose it (X11, Windows; on
Wayland the desktop places windows itself).

The keyboard drives the menus as player one's pad, with or without a
controller plugged in (a pad and the keys work together); it reads keys only
while the game's window has the focus. `RECOMP_KEYBOARD=0` turns it off.

The variables below override the saved settings for one run.

| Key or switch | Effect |
|---|---|
| Enter or Space | A (accept) |
| Backspace or Tab | B (back) |
| Escape | START (pause) while skating; B (back) in the main menu's screens and the pause menu, so it also leaves the pause menu |
| W A S D | Left stick (steering) |
| Arrow keys | D-pad |
| F11 or Alt+Enter | Toggle borderless fullscreen |
| F10 | Capture a graphics glitch: the next two frames go to `~/.local/state/thug-pc-port/captures/` (Windows: `%LOCALAPPDATA%\thug-pc-port\captures\`) and the log, for a bug report |
| `THUG_FULLSCREEN=1` | Start fullscreen |
| `THUG_ASPECT=<ratio>` | Aspect ratio: `16:9` (default), `16:10` or `4:3`. The game's field of view follows (Hor+); switching between 4:3 and a wide ratio changes the menu and HUD layout at the next start. `THUG_WIDESCREEN=0` still means 4:3 |
| `THUG_RES_SCALE=<x>` | Internal resolution, as a multiple of 480 lines (default 2.25: 1920x1080 at 16:9; 0 = your desktop's height) |
| `THUG_VSYNC=0` | Don't wait for the display |
| `THUG_FPS_LIMIT=<n>` | Cap the frame rate (default 60; 0 = off) |
| `THUG_SHOW_FPS=1` | FPS counter in the corner |
| `THUG_ANISO=<n>` | Anisotropic filtering (default 16) |
| `THUG_MSAA=<n>` | Antialiasing: 1 (off), 2, 4 (default) or 8 samples |
| `THUG_UI_UPSCALE=0` | Text, icons and logos with plain filtering, as before "Smooth text and icons" (default 1, on) |
| `THUG_BRIGHTNESS=<b>` | Gamma on the final image, 0.5 to 2 (default 1) |
| `THUG_VOLUME=<n>` | Master volume, 0 to 100 (default 100) |
| `THUG_MUTE_UNFOCUSED=1` | Silent while the window is in the background |
| `THUG_RUMBLE=<n>` | Vibration strength, 0 (off) to 100 (default 100) |
| `THUG_VBLANK_TIMING=1` | The original 60 Hz game timing (runs fast above 60 fps) |
| `RECOMP_PB_GL=0` | The old CPU renderer (slow) |

The Windows build's state and its open items are in
[docs/WINDOWS.md](docs/WINDOWS.md).

## You need

- Your own **Tony Hawk's Underground (USA) Xbox disc image**. This repository
  contains no game code or data, and no generated code derived from it. The
  game's files go in `game/`, which git ignores.
  - Supported `default.xbe`: SHA-1 `2951de954c96a8b962ba1cde34548fa82ae668d7`
- **Windows 10 or 11** (x64) with [MSYS2](https://www.msys2.org/) and its
  CLANG64 packages. `scripts\build.bat` installs whichever is missing when it
  runs (MSYS2 through winget, if it is not in `C:\msys64` or
  `%MSYS2_ROOT%`), or by hand in an MSYS2 shell:

  ```
  pacman -S --needed git mingw-w64-clang-x86_64-{clang,cmake,ninja,pkgconf,sdl2-compat,python,python-capstone,gdb}
  ```

  The build copies the DLLs it needs (SDL2 and SDL3, libc++,
  winpthread) next to `thug.exe`, so the game then starts from Explorer too.
- **Linux** x86-64 with:
  - clang or GCC
  - CMake 3.20 or newer
  - Ninja
  - Python 3.10 or newer with `capstone`

  On Arch:

  ```
  sudo pacman -S --needed git python python-capstone cmake ninja clang
  ```

  SDL is built from `vendor/SDL` into the game, and OpenGL is loaded through
  `vendor/glad`, so `build/bin/thug` is one file that needs only glibc and
  the graphics driver: copy it, with the disc image beside it, to another
  PC. SDL's X11, Wayland and sound back ends are built when their headers
  are installed (they are on any Arch desktop) and loaded at run time.
  `-DTHUG_BUNDLED_SDL=OFF` links the system's SDL2 instead. The executable
  runs on systems with the same or a newer glibc than the one it was built
  on.

## Build

```
git clone <this repo>
cd thug-pc-port
cp "/path/to/Tony Hawk's Underground (USA).iso" game/
scripts/build.sh
build/bin/thug
```

On Windows, `scripts\build.bat` does the same from any shell: it runs
`scripts/build.sh` in MSYS2's CLANG64 environment, so no MSYS2 shell needs
opening. It takes the same `--regen` and `--release`, and installs whatever
part of the toolchain is missing before it builds. The result is
`build\bin\thug.exe`, which runs from anywhere.

Building needs the unpacked `default.xbe` (it is lifted to C), so
`scripts/build.sh` unpacks the image itself the first time, with
`build/bin/thug-extract`: the game's own start-up check as a command. Then it
lifts `default.xbe` to C (a few minutes; again when `src/recomp_manual.c` or
the seed functions change, or with `--regen`), configures CMake and compiles.
By hand:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thug-extract
build/bin/thug-extract game     # unpack game/*.iso if needed, verify default.xbe
scripts/regen.sh                # lift default.xbe to C in src/recomp/gen/
cmake --build build
build/bin/thug
```

`build/bin/thug-extract --list <image>` lists a disc image's files.

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz and its
  contributors (MIT; parts LGPL-2.1+ from xemu)
- [thug-decomp](https://github.com/andrewnakas/thug-decomp), used as a
  reference for the shared Neversoft codebase

Tony Hawk's Underground is property of © Activision and Neversoft. This is an 
unofficial fan project, not affiliated with or endorsed by either. No game assets
are included in this repository.
