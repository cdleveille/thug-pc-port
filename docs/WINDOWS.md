# Windows build

Started as the handoff written on the Linux machine (2026-11-11); now the
state of the Windows build. Read `AGENTS.md` first as always, then
`docs/NOTES.md` and `docs/BRINGUP.md`, then this.

**Goal:** a working Windows build the owner can play, with the same features
as Linux: the GPU renderer, F1 overlay, settings, widescreen, internal
resolution, MSAA, smooth text and icons, keyboard and pad controls, rumble.

**Status (2026-11-11):** `build\bin\thug.exe` builds with MSYS2 CLANG64 and
runs the Linux renderer and window loop. In scripted runs it boots through the
intro movies to the main menu, story mode's intro cutscene, skater select and
free skate in New Jersey (skating, a skitch for 750 points); menus hold a
steady 60 fps at 1080p internal. The overlay, the saved settings and the
first-launch unpack work. 70 of the toolkit's test suites pass on Windows.
It has not been played by hand yet: see the owner checklist below.

## Context a new session won't otherwise have

- **The owner** plays with an XInput controller (Steam Input should work
  too), on an RTX 4080 with a 4K display. On Linux they ran 1080p internal in
  a maximised window. They playtest, report what they see (often with
  screenshots or F10 captures), and decide on defaults. They **commit and
  push themselves** (SSH-signed commits): don't commit; when a change is
  ready, say which files to add.
- **Defaults the owner chose** (`src/ui/settings.c`): 16:9, 1920x1080,
  windowed, vsync on, a 60 fps cap, 16x anisotropic filtering, 4x MSAA,
  smooth text and icons on, volume and vibration at 100%. Escape is START
  while skating and B in menus; L3+R3 or F1 opens the overlay.
- **Not in git:** the disc image and its unpacked files (`game/`), and the
  lifted C (`src/recomp/gen/`). `scripts/build.sh` unpacks and lifts on a
  fresh clone (the lift takes about 4 minutes here).
- **Ways of working that held up:**
  - Reproduce headless (`RECOMP_HEADLESS=1`, `RECOMP_PAD_LIVE`, frame dumps;
    see AGENTS.md) before asking the owner to test.
  - Test runs point `APPDATA` (settings) and `LOCALAPPDATA` (log, captures)
    at scratch folders, so the owner's files stay untouched, and set
    `THUG_VOLUME=0` for visible runs on the owner's machine.
  - A visible window can be captured without bringing it to the front:
    PowerShell, `PrintWindow(hwnd, hdc, 2)` (PW_RENDERFULLCONTENT) on the
    process's main window.
  - A fix has a test where one fits, and an entry in `docs/BRINGUP.md`.
  - Toolkit changes are also logged in NOTES "Toolkit changes".

## How THUG renders, and why Windows reuses the Linux path

THUG runs its own lifted XDK Direct3D. The toolkit's pushbuffer executor
(`vendor/xboxrecomp/src/kernel/nv2a_pb_exec.c`, `RECOMP_PB_EXEC=1`) reads the
NV2A command stream that D3D writes and hands each batch to a render back
end (`nv2a_backend.h`). The port's back end is `src/gpu/nv2a_pb_gl.c`
(OpenGL 3.3 through SDL2 and libepoxy). Everything the player sees is built
on it: the window loop (`src/host/host_window.c`), the overlay, MSAA,
internal resolution, widescreen, smooth text, captures.

The toolkit's own "Windows D3D11 backend" (`vendor/xboxrecomp/src/d3d/`)
translates *intercepted* D3D8 calls. THUG doesn't go through it. So Windows
builds the same SDL2 + OpenGL path, with MinGW-w64 clang (MSYS2 CLANG64):
the port's code uses pthreads and `clock_gettime`, which winpthreads
provides.

## Toolchain (MSYS2, CLANG64 environment)

`scripts\build.bat` is the Windows entry point, from cmd, PowerShell or the
VS Code terminal: it finds MSYS2 (`%MSYS2_ROOT%`, else `C:\msys64`), sets
`MSYSTEM=CLANG64` and runs `scripts/build.sh` in MSYS2's bash, passing
`--regen` and `--release` on. CLANG64's login profile puts `/clang64/bin`
first and drops the rest of the Windows PATH, so no other cmake or Python on
the machine is picked up. `--setup` first installs MSYS2 through winget if it
is missing (only when `MSYS2_ROOT` is unset), then `pacman -Syu` (twice: the
first run may stop after updating pacman itself) and
`pacman -S --needed` of the packages below. Without `--setup` it checks
that the main tools are there and says to run it if not.

The packages (what `--setup` installs; by hand, in any MSYS2 shell):

```
pacman -S --needed git mingw-w64-clang-x86_64-{clang,cmake,ninja,pkgconf,sdl2-compat,libepoxy,python,python-capstone,gdb}
```

SDL2 is `sdl2-compat` (SDL2's API over SDL3): since 2026 MSYS2 marks it as
replacing the SDL2 package, so `pacman -Syu` swaps it in anyway (a package
list naming `SDL2` made them swap back and forth). Versions in use
(2026-11-11): clang 23.1.5, CMake 4.4.5, sdl2-compat 2.32.74 over SDL3
3.4.20, libepoxy 1.5.10, Python 3.14 with capstone 5.0.x.

The build copies the toolchain DLLs `thug.exe` needs next to it
(`scripts/copy_runtime_dlls.cmake`): SDL2, SDL3 (which sdl2-compat loads at
run time, so no import table names it) and its libiconv, libepoxy, libc++
and winpthread. It first removes the old copies of those, since the
dependency resolver looks beside the executable first and would otherwise
keep a stale DLL. `build\bin` then runs from Explorer or any shell.
`thug.exe` is a windowed program (`-DTHUG_WINDOWS_CONSOLE=ON` builds a
console one).

## What was done (2026-11-11)

| Where | Now |
|---|---|
| `CMakeLists.txt` | SDL2, epoxy and ImGui on every platform; `SDL_MAIN_HANDLED`; 8 MB stack reserve for every thread (as Linux's default; the toolkit starts title threads with CreateThread's default); `-mwindows`; the DLL copy |
| `scripts\build.bat` | The build from any Windows shell, and `--setup` for the toolchain (above) |
| `src/gpu/nv2a_pb_gl.c` | The stub is gone. `_mkdir`, `localtime_s`; captures in the state folder from `thug_launch_state_dir`; the NVIDIA/Mesa threading variables stay Linux-only |
| Frame cap | `src/host/host_sleep.c`: a high-resolution waitable timer on Windows (measured: 60.0 fps at a 60 cap) |
| `src/host/host_window.c` | Built on Windows; `getrlimit` is POSIX-only (the guest thread asks for 64 MB itself); closing the window ends with `TerminateProcess` |
| `src/main.c` | The Linux path: `nv2a_pb_gl_init()`, then `thug_host_window_main()`. `RECOMP_WIN_PLACEHOLDER=1` is a default (the 4 GB guest window reserved before the GL driver can take any of it). An unhandled-exception filter points the player at the log |
| `src/host/launch.c` | The log in `%LOCALAPPDATA%\thug-pc-port\thug.log` (rotated to `thug.prev.log`) unless stderr is a file or pipe; a terminal that started the game gets the player's messages (`AttachConsole`); the crash notice is a message box when there is no terminal. The first-launch progress window is the SDL one, scaled by the desktop's DPI |
| Toolkit input | `keyboard_sdl.c` built on Windows with SDL2; the XInput backend has no-op `SetMainPump` and `SdlRefreshControllers`, and asks about an empty port once a second (NOTES "Toolkit changes") |
| `src/host/restart.c` | `CreateProcessW` with the starting command line and environment, the original stdio handles, then `TerminateProcess` (checked with a probe: arguments with spaces, the starting environment, not the variables added at start-up) |
| High DPI | `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` before SDL starts, plus SDL2's `SDL_WINDOWS_DPI_AWARENESS` hint (`window_icon.c`). sdl2-compat ignores the hint: with it alone the window was DPI-unaware (1707x960, stretched by Windows to 175%). Now on the 4K desktop at 175% the window opens at 2560x1440 real pixels on either SDL |
| `game_data.c`, `settings.c` | Worked as they were: `GetModuleFileNameA`, `_fseeki64` for the 7.8 GB image, `%APPDATA%\thug-pc-port\settings.ini` |
| Toolkit runtime | Runs: the placeholder reservation, the vectored-exception MMIO decoder (APU, AC'97, USB), XAudio2. General MinGW fixes: the DXT cache's thread-local, winpthread linked for `clock_gettime` (runtime and the `nv2a_*` tests), the Windows raster pool's worker count (NOTES "Toolkit changes") |
| Toolkit tests | Every suite configured, built and run in a fresh folder under CLANG64: 70 pass, including all 11 that only run on Windows (`apu_vp`, `fb_present`, `idex_channel`, `input_xinput_imports`, `kernel_dispatch`, `kernel_irql_abi`, `kernel_physical_address`, `kernel_regressions`, `nv2a_pvideo`, `mmio_decode`, `xaudio2`) and `kernel_open_directory`, which fails on Linux; 14 are POSIX-only by design; `d3d8_smoke` does not build (below) |

## Still open

- **The owner's playtest** (checklist below). Nothing has been played by
  hand: the scripted runs pressed Start and nothing else.
- **Sound** initialises (XAudio2, 48 kHz) and the APU runs, but every test
  run was muted; whether it sounds right needs ears.
- **Performance in levels** is lower than on Linux. Headless, 1080p, free
  skate New Jersey, uncapped: about 63-66 fps at 1,000 draws a frame (Linux:
  83-84), 40 fps in the heaviest views (2,100 draws). `RECOMP_EXEC_PROFILE`
  puts the executor's walk at about 14 ms a frame for 1,100 batches, 184 ns
  per index (Linux 155-170 ns). GL submission is 1.8 ms of it, about 1.7 us a
  draw (Linux 2.5 us), so the difference is in the executor's own work
  (transform 4.3 ms, decode and state 5.2 ms), not the driver. Not investigated:
  Windows TLS access (every guest register is a thread-local), the
  scheduler, or codegen. With the default 60 fps cap most views hold 60.
- **Pad hotplug and rumble** through XInput: a connected pad was found on
  port 1 at start-up; plugging, unplugging and rumble need hands.
- **gdb attach:** `THUG_DEBUG_ATTACH` is Linux-only; MSYS2's `gdb -p` should
  attach as is, not tried yet. The crash reporter's `SymFromAddr` reads PDB
  symbols, which a MinGW build has not got (DWARF), so it names only
  exported functions; `gdb` reads the DWARF.
- `vendor/xboxrecomp/tests/d3d8_smoke` does not build standalone on Windows
  (`platform/xbox_winnt.h` not on its include path): the D3D11 layer, not
  THUG's path; an upstream matter.

## Owner checklist

- Intro movies (A or Start skips), main menu, free skate, story mode.
- Sound and volume.
- Pad: buttons, rumble during a grind (should last the whole grind), and
  unplugging and replugging.
- Keyboard: Esc is B in menus and START while skating.
- F1 and L3+R3 overlay; F11 and Alt+Enter fullscreen; F10 capture (files in
  `%LOCALAPPDATA%\thug-pc-port\captures`).
- Each aspect ratio and resolution; smooth text on and off; "Restart to
  apply changes" after switching between 4:3 and a wide aspect.
- Settings saved and restored, and the window reopening as it was left.
- Starting from Explorer (no console window should appear) and from a
  terminal (it should print where the log is).

## Debugging on Windows

- **Backtraces:** the lifted functions are host symbols (`sub_XXXXXXXX`), so
  a backtrace still reads as the guest call chain. `gdb` from MSYS2 attaches
  to MinGW programs (`gdb -p <pid>`), and the AGENTS.md recipes carry over
  apart from `THUG_DEBUG_ATTACH` and the SIGSEGV/SIGBUS handling (Windows
  uses the vectored handler; tell gdb to pass first-chance access
  violations, which the MMIO traps use).
- **Logs:** run from an MSYS2 terminal and stderr stays there (it is a pipe);
  otherwise `%LOCALAPPDATA%\thug-pc-port\thug.log`. A console build
  (`-DTHUG_WINDOWS_CONSOLE=ON`) shows a console window beside the game.

## Known open items carried over from Linux

- Face culling stays off (`RECOMP_GL_NO_CULL=1`).
- Shadows are not drawn.
- Vertex programs run on the CPU, so the heaviest views fall below 60 fps.
- The owner thinks the glass shards still fall a little slowly, though that
  is the game's own gravity (NOTES "Glass shatter"). Left as is.
