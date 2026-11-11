# Windows build: handoff

Written 2026-11-11 on the Linux machine, for the first session on the owner's
Windows 11 machine. Read `AGENTS.md` first as always, then `docs/NOTES.md`
and `docs/BRINGUP.md`, then this.

**Goal:** a working Windows build the owner can play, with the same features
as Linux: the GPU renderer, F1 overlay, settings, widescreen, internal
resolution, MSAA, smooth text and icons, keyboard and pad controls, rumble.

**Status:** the game has never been built or run on Windows. Linux is
playable, with menus, free skate, sound, pads, the overlay and every fix in
`docs/BRINGUP.md`.

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
  lifted C (`src/recomp/gen/`). On the new machine:
  - Copy the owner's `.iso` into `game/`, or beside the built `thug.exe`. The
    first launch unpacks it.
  - Lift the code again with `scripts/regen.sh` (a few minutes). Or copy
    `src/recomp/gen/` from the Linux machine: the lift is the same on any
    OS. Never commit it either way.
- **Ways of working that held up on Linux:**
  - Reproduce headless (`RECOMP_HEADLESS=1`, `RECOMP_PAD_LIVE`, frame dumps;
    see AGENTS.md) before asking the owner to test.
  - Test runs point `XDG_CONFIG_HOME` / `XDG_STATE_HOME` at scratch folders
    so the owner's settings stay untouched. On Windows the equivalents are
    `APPDATA` and the state folder described below.
  - A fix has a test where one fits, and an entry in `docs/BRINGUP.md`.
  - Toolkit changes are also logged in NOTES "Toolkit changes".

## How THUG renders, and why Windows should reuse the Linux path

THUG runs its own lifted XDK Direct3D. The toolkit's pushbuffer executor
(`vendor/xboxrecomp/src/kernel/nv2a_pb_exec.c`, `RECOMP_PB_EXEC=1`) reads the
NV2A command stream that D3D writes and hands each batch to a render back
end (`nv2a_backend.h`). The port's back end is `src/gpu/nv2a_pb_gl.c`
(OpenGL 3.3 through SDL2 and libepoxy). Everything the player sees is built
on it: the window loop (`src/host/host_window.c`), the overlay, MSAA,
internal resolution, widescreen, smooth text, captures.

The toolkit's own "Windows D3D11 backend" (`vendor/xboxrecomp/src/d3d/`,
recommended in its GETTING_STARTED) translates *intercepted* D3D8 calls. THUG
doesn't go through it, so it is not a route to a working build. Without
`nv2a_pb_gl.c`, Windows would fall back to the executor's CPU rasteriser:
about 9 fps and no overlay.

**So the plan is: build the same SDL2 + OpenGL path on Windows**, with
MinGW-w64. OpenGL 3.3 core is well supported by NVIDIA's Windows driver.
The port's code uses pthreads, `clock_gettime`, `clock_nanosleep` and
`setenv`, which MinGW-w64 provides (winpthreads), or mostly so; MSVC would
need all of it rewritten. The lifted code builds with either.

## Toolchain (recommended: MSYS2, CLANG64 environment)

Install MSYS2, open the **CLANG64** shell, then:

```
pacman -S --needed git mingw-w64-clang-x86_64-{clang,cmake,ninja,pkgconf,SDL2,libepoxy,python,python-pip,gdb}
python -m pip install capstone     # or the mingw-w64-clang-x86_64-python-capstone package, if it exists
```

Package names are from memory: check them with `pacman -Ss`. clang matches
the compiler used on Linux. `scripts/build.sh` and `scripts/regen.sh` are
bash and call `python3`, so run them from that shell. The toolkit already
handles MinGW in places: its XInput link picks `xinput1_4` (stock Windows
lacks `XINPUT1_3.dll`), and `vendor/xboxrecomp/cmake/mingw-w64-x86_64.cmake`
is a cross-compile toolchain for building from Linux.

## What is Linux-only today: the work list

| Where | Today on Windows | What to do |
|---|---|---|
| `CMakeLists.txt` | The renderer and ImGui are inside `if(NOT WIN32)`; Windows links `d3d11` etc. | Build the SDL2/epoxy/ImGui block for MinGW too (`pkg_check_modules` works in MSYS2). |
| `src/gpu/nv2a_pb_gl.c` | `#if defined(_WIN32)`: a stub whose `nv2a_pb_gl_init()` returns 0 | Remove the stub. Check `mkdir(path, mode)` (MinGW's takes one argument), `setenv` (see the `setenv_once` macro in `settings.c`), and the capture folder, which uses `XDG_STATE_HOME`/`HOME`. |
| `src/host/host_window.c` | The whole file is `#if !defined(_WIN32)` | Enable it. Drop `getrlimit` (keep the 64 MB guest stack through the pthread attribute). |
| `src/main.c` | On `_WIN32` it calls `thug_guest_main()` straight away: no renderer, no window loop | Take the Linux path: `nv2a_pb_gl_init()`, then `thug_host_window_main()`. Define `SDL_MAIN_HANDLED` (or call `SDL_SetMainReady`) so SDL doesn't replace `main`. The `__GL_THREADED_OPTIMIZATIONS` setting is Linux-only; on Windows the NVIDIA control panel owns it. The Windows crash handler (`veh_handler`, MMIO traps) is already there. |
| `src/host/launch.c` | The log-file setup is POSIX-only (`XDG_STATE_HOME`/`HOME`); the first-launch progress window is a stub | Put the log in `%LOCALAPPDATA%\thug-pc-port\thug.log` (a GUI program's stderr goes nowhere), and the captures beside it. The progress window can use the same SDL code as Linux. |
| Toolkit input (`vendor/xboxrecomp/src/input`) | Windows builds the XInput backend and the raw-input mouse/keyboard; `keyboard_sdl.c` (SDL key events) is POSIX-only. `host_window.c` calls `xbox_InputSetMainPump`, `xbox_InputSdlInit`, `xbox_InputSdlEvent`, which exist only in the SDL backend. | Recommended: pads stay on XInput (native; Steam Input presents XInput), and the keyboard comes from SDL events. Build `keyboard_sdl.c` on Windows too, and give the XInput backend no-op `SetMainPump` and refresh entry points. The overlay's L3+R3 chord reads SDL controller events, so keep `SDL_INIT_GAMECONTROLLER`: SDL and XInput can both read one pad. Rumble: XInput holds the motors itself (`xinput_device.c`). This is a toolkit change: log it in NOTES and extend a test (`tests/input_*`). |
| `src/host/restart.c` | Stubs: `thug_restart_supported()` is 0, so the overlay hides "Restart to apply changes" | Later: `CreateProcessW` with the same command line and environment, then exit. |
| Frame cap (`nv2a_pb_gl.c`, `clock_nanosleep` with `TIMER_ABSTIME`) | winpthreads sleeps at the system timer's ~15.6 ms granularity | Use a high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), or `timeBeginPeriod(1)`, or a 60 fps cap won't hold. |
| High DPI | Not handled | Set `SDL_HINT_WINDOWS_DPI_AWARENESS` to `permonitorv2` before creating the window: the 4K display is scaled. |
| `src/host/game_data.c`, `src/ui/settings.c` | Windows branches exist: exe folder via `GetModuleFileNameA`, settings in `%APPDATA%\thug-pc-port\settings.ini` | Check that they behave. |
| Toolkit runtime (memory, kernel, MMIO, audio) | Windows is the toolkit's original platform (4 GB guest window by `VirtualAlloc`, the vectored-exception MMIO decoder, XAudio2 audio) | THUG has only run on POSIX. Go through NOTES "Toolkit changes" for fixes made on the POSIX side that need a Windows counterpart (the `0001` rows are mostly POSIX). |

## Suggested order

1. **Set up.** MSYS2 and the packages; clone; copy the `.iso` into `game/`.
2. **Check the toolkit on Windows.** Its test suites build alone:
   `cmake -S vendor/xboxrecomp/tests/<suite> -B <dir> -G Ninja`, then
   `cmake --build <dir>` and `ctest --test-dir <dir>`.
   - These suites run only on Windows, so they have never run for this port:
     `apu_vp`, `fb_present`, `idex_channel`, `input_xinput_imports`,
     `kernel_dispatch`, `kernel_irql_abi`, `kernel_physical_address`,
     `kernel_regressions`, `nv2a_pvideo`, `mmio_decode` and `xaudio2`.
     They are a quick check of the runtime under MinGW.
   - On Linux, 71 suites pass. `kernel_open_directory` fails there, and
     already did before today's changes (see BRINGUP 2026-11-11).
3. **Unpack and lift.** Build `thug-extract`, run `build/bin/thug-extract
   game`, then `scripts/regen.sh` (or copy `src/recomp/gen/` across).
4. **Build as it stands first.** This uses the CPU rasteriser and the
   toolkit's own window. It shakes out compile and link errors in the toolkit
   and the lifted code under MinGW clang, and shows whether the runtime boots
   (intro movies, menus, sound) before the renderer is involved. Expect it to
   be slow.
5. **Bring up the GL path:** `CMakeLists.txt`, `nv2a_pb_gl.c`,
   `host_window.c`, `main.c` and input, from the table above. The renderer's
   switches (AGENTS.md) work the same: `THUG_GL_STATS=1`, `THUG_GL_DUMP`,
   F10 captures.
6. **Polish:** the log and capture paths, frame-cap timing, DPI, the progress
   window, restart.
7. **Owner checklist:**
   - Intro movies (A or Start skips), main menu, free skate, story mode.
   - Sound and volume.
   - Pad: buttons, rumble during a grind (should last the whole grind), and
     unplugging and replugging.
   - Keyboard: Esc is B in menus and START while skating.
   - F1 and L3+R3 overlay; F11 and Alt+Enter fullscreen; F10 capture.
   - Each aspect ratio and resolution; smooth text on and off.
   - Settings saved and restored, and the window reopening as it was left.

## Debugging on Windows

- **Backtraces:** the lifted functions are host symbols (`sub_XXXXXXXX`), so
  a backtrace still reads as the guest call chain. `gdb` from MSYS2 attaches
  to MinGW programs (`gdb -p <pid>`), and the AGENTS.md recipes carry over.
  The Linux-only parts: `THUG_DEBUG_ATTACH` (a `prctl`), and the
  SIGSEGV/SIGBUS handling (Windows uses the vectored handler).
- **Crashes:** the crash reporter in `src/main.c` has a Windows branch.
- **Console window:** a MinGW build is a console program by default, which
  keeps stderr visible while bringing it up. Switch to `-mwindows` once the
  log file works.

## Known open items carried over from Linux

- Face culling stays off (`RECOMP_GL_NO_CULL=1`).
- Shadows are not drawn.
- Vertex programs run on the CPU, so the heaviest views fall below 60 fps.
- The owner thinks the glass shards still fall a little slowly, though that
  is the game's own gravity (NOTES "Glass shatter"). Left as is.
