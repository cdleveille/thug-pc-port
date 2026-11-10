# THUG PC Port

A native Windows and Linux port of **Tony Hawk's Underground** (2003), built by
statically recompiling the original Xbox executable into C with the
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) toolkit. It does not
emulate anything: the game's own code runs natively.

**Status: early bring-up.** On Linux, the recompiled game opens a window,
plays its intro movies with sound, reads a controller, and reaches the main
menu and story mode's 3D skater-select screen. It then stalls (a memory
corruption in its sound library), so it isn't playable yet. See
[docs/BRINGUP.md](docs/BRINGUP.md).

To watch it run, from the repo root after building:

```
RECOMP_FB_WINDOW=1 build/bin/thug 2> stderr.txt
```

Planned features: native resolution, widescreen, unlocked frame rate, and
native Windows and Linux builds.

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
scripts/apply-patches.sh    # local fixes to the toolkit, pending upstream
scripts/extract.sh          # unpack the disc into game/ and verify the XBE
scripts/regen.sh            # lift default.xbe to C in src/recomp/gen/
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/bin/thug
```

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz and its
  contributors (MIT; parts LGPL-2.1+ from xemu)
- [thug-decomp](https://github.com/andrewnakas/thug-decomp), used as a
  reference for the shared Neversoft codebase

Tony Hawk's Underground is © Activision and Neversoft. This is an unofficial
fan project, not affiliated with or endorsed by either.
