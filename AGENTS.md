# AGENTS.md: THUG PC Port

Read this first, every session. Then read `docs/NOTES.md` (title facts and
decision log) and `docs/BRINGUP.md` (where bring-up stands). On Windows, also
read `docs/WINDOWS.md`: the handoff for the Windows build, with the plan and
the owner's context.

## What this project is

A native Windows and Linux x86-64 build of *Tony Hawk's Underground* (Xbox,
NTSC-U, 2003), made by **static recompilation**: the game's own x86 is
translated to C by the [xboxrecomp](vendor/xboxrecomp) toolkit and
compiled natively. There is no emulation and no interpreter.

Goals, in order:
1. Boot to menus, then gameplay, on Linux (OpenGL) and Windows (D3D11).
2. Native resolution and widescreen (Hor+).
3. Unlocked frame rate, with game logic that does not speed up.

Not goals: a decompilation, rewriting game logic, or fake screens. If the game
shows something, the game's own code put it there.

## Hard rules

1. **Never commit game content.** That covers `game/` (the user's disc
   image and the files unpacked from it), any `.iso`, `.xbe` or `.bik`, and
   extracted assets. The user supplies their own disc.
2. **Never commit or hand-edit `src/recomp/gen/`.** It is lifted from the
   game's executable (a derivative of the game) and `scripts/regen.sh`
   overwrites it.
3. **Fixes go in exactly these places**, in this order of preference:
   - `config/seed_functions.json`: function entry points the detector missed.
   - `src/recomp_manual.c`: hand-written replacements for single lifted
     functions. Replace, don't wrap. Log each one in `docs/OVERRIDES.md`
     with the guest address, the symptom and the reason.
   - `vendor/xboxrecomp`: general bugs in the lifter, kernel shim or
     graphics translation. The toolkit is vendored: committed here as
     ordinary files, based on upstream v0.17.0 (`vendor/README.md`).
     After changing it:
     1. List the change in `docs/NOTES.md` ("Toolkit changes").
     2. Add or extend a test under the toolkit's `tests/`.
     3. Offer general fixes upstream. `vendor/README.md` shows how to
        diff against the upstream base.
     Never commit the pipeline's output under `vendor/xboxrecomp/tools/*/output/`:
     it is derived from the game (the toolkit's `.gitignore` excludes it).
4. **Addresses are for one binary.** `default.xbe` SHA-1
   `2951de954c96a8b962ba1cde34548fa82ae668d7` (USA retail). Anything keyed
   by address is wrong for any other build.
5. **Never skip `abi_analysis`.** Without it every lifted signature silently
   becomes cdecl with 0 parameters, and the build still succeeds.
6. **Keep `--split 250`.** Larger chunks OOM the compiler.

## Layout

| Path | What |
|---|---|
| `src/main.c` | Host entry: loads the XBE, maps memory, boots the kernel shim, enters the guest. Crash reporter for both platforms. |
| `src/host/host_sleep.c` | The frame cap's sleep to a deadline: `clock_nanosleep` on Linux, a high-resolution waitable timer on Windows |
| `src/host/launch.c` | Start-up: the log file, and the game files (found next to the executable, unpacked on first launch with a progress window from a disc image in `game/` or beside the executable (creating `game/`), refused with a message box when absent or the wrong version) |
| `src/host/game_data.c` | Finding the game directory, the XBE check, the XDVDFS reader and unpacking; no SDL, shared with `thug-extract` |
| `src/host/restart.c` | The overlay's "Restart to apply changes": the process execs itself (same PID) with the arguments and environment it was started with, its descriptors closed and stdio handed back so the new run sets up its own log; on Windows `CreateProcessW` with the same, then this process ends |
| `src/host/controls.c` | The keyboard's menu bindings (Enter/Space A, Backspace/Tab B, WASD/arrows D-pad; Esc START while skating and B in the menus, from the game's current level and `GameIsPaused`), through the toolkit's keyboard layer and its binding layers |
| `src/host/window_icon.c` | The window icon (`thug.png`, embedded at build time by `scripts/embed_icon.py`) and the app ID `thug-pc-port` |
| `src/tools/thug_extract.c` | `build/bin/thug-extract`: the same check and unpack as a command, for `scripts/build.sh` (lifting needs `default.xbe` before the game can be built) |
| `src/gpu/xbr.c` | xBR, the edge-directed magnifier behind the overlay's "Smooth text and icons" (the renderer's `up_request`); no GL, tested by `tests/xbr_test.c` |
| `tests/` | The port's own tests: `ctest --test-dir build` |
| `src/recomp_manual.c` | Manual overrides, ICALL failure logging |
| `src/recomp/gen/` | Generated C (gitignored) |
| `config/` | Title-specific pipeline inputs (seed functions, etc.) |
| `scripts/regen.sh` | Run the lifting pipeline: parse, disasm, func_id, abi, recomp |
| `scripts/build.bat` | Windows: `scripts/build.sh` from any shell, run in MSYS2's CLANG64 environment; `--setup` installs the toolchain |
| `scripts/copy_runtime_dlls.cmake` | Windows: copies the toolchain DLLs `thug.exe` needs next to it after each link (SDL3, which sdl2-compat loads at run time, included) |
| `docs/` | Title notes, bring-up log, override log, Windows handoff (`WINDOWS.md`) |
| `vendor/xboxrecomp` | The toolkit (vendored, with this port's changes) |
| `vendor/imgui` | Dear ImGui for the F1 overlay (vendored, the files the build uses) |
| `game/` | The user's disc image and the files unpacked from it (gitignored) |

## Workflow

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thug-extract
build/bin/thug-extract game        # once: game/*.iso -> game/, XBE verified
scripts/regen.sh                   # lift game/default.xbe -> src/recomp/gen/
cmake --build build
build/bin/thug 2> stderr.txt       # finds game/ from the executable's location
```

`scripts/build.sh` does all of the build steps that are out of date (it
regenerates from the `recomp` stage when `src/recomp_manual.c` is newer than
`src/recomp/gen/.regen-stamp`). The player just runs `build/bin/thug`.

The log: with stderr redirected to a file or a pipe (every recipe here), the
game leaves it alone. Otherwise (a terminal, a desktop launcher) it writes
`~/.local/state/thug-pc-port/thug.log` and prints only the player's messages;
`THUG_LOG=-` keeps stderr as it is. Before booting, the game checks
`default.xbe`'s SHA-1 every run, so a test against another folder sets
`THUG_GAME_DIR`; a folder holding only an image is unpacked into, which is how
to test first launch (link the image in rather than copying it).

`scripts/regen.sh --from <stage>` resumes from `parse`, `disasm`, `func_id`,
`abi` or `recomp`. Re-running `disasm` discards any Ghidra names applied to
`functions.json`, so re-apply them afterwards.

Then read stderr, fix, and repeat.

**At a hang, use gdb rather than the watchdog's stack scan.** The scan also
picks up stale return addresses. Every lifted function is a host symbol, so
a host backtrace is the exact guest call chain:

```
THUG_DEBUG_ATTACH=1 build/bin/thug 2> stderr.txt &
gdb -p "$(pgrep -n -x thug)" -batch -ex "thread apply all bt 25"
```

Guest registers are thread-locals (`p/x g_edi`). Guest memory is at
`g_xbox_mem_offset + va`. Before you `continue` in gdb, pass the toolkit's
trap signals: `handle SIGSEGV nostop noprint pass` (and the same for
SIGBUS).

Headless repro of a boot that needs input: `RECOMP_HEADLESS=1
RECOMP_PAD_PRESS=0x10 RECOMP_FB_DUMP=build/runs/x/fb`. This pulses Start
every 2 s and dumps frames.

To find who corrupts a guest word, use a gdb hardware watchpoint on
`g_xbox_mem_offset + va` with a value condition. It catches host threads as
well, such as the GPU executor or the APU, which `RECOMP_WATCH` can't
attribute. `RECOMP_PB_SURFACES=1` lists every render surface the game draws
into.

The GPU renderer (`src/gpu/nv2a_pb_gl.c`) has its own switches.
`THUG_GL_DUMP=<dir>` writes the presented frame as PPM every
`THUG_GL_DUMP_EVERY` flips; this works headless, where `RECOMP_FB_DUMP` only
sees guest memory and needs `THUG_GL_READBACK=1`. `THUG_GL_STATS=1` prints fps,
draws, triangles, GPU time per frame (a timer query: the one fair measure of
GPU cost, since fps is usually the executor's), texture traffic, MSAA
resolves and alpha-to-coverage draws, and dropped triangles every 120 flips.
`THUG_GL_DEBUG=1` prints the driver's errors and warnings (KHR_debug,
synchronous; slow).
`RECOMP_GL_PROBE=x,y[,n,minverts,minflip]` lists every draw that covers one
surface pixel, with its state, its depth at that pixel, and the pixel before
and after. `RECOMP_GL_NO_DEPTH`, `_NO_ALPHA`, `_NO_BLEND`,
`_NO_CULL` and `_NO_COMB` each open one gate, which is a quick way to find
the gate that eats a draw; `THUG_GL_NO_POLYOFF` and `THUG_GL_NO_MIPS` do the
same for depth bias and mipmaps, and `THUG_GL_POLYOFF_LOG=1` lists the
distinct bias values and `SET_CONTROL0` words the title uses.
`RECOMP_GL_PROBE_TEX=<hex>[,n]` prints the same
record for draws that bind a given texture, `THUG_GL_TEXDUMP=<hex>` writes
that texture's decode as PPM (in the working directory), and the toolkit's
`RECOMP_TRACE_TEX0=<hex>` shows how those batches were transformed and lit.
`RECOMP_EXEC_PROFILE=1` prints where the executor's time goes per frame
(transform, assembly, back end, decode), the measure to optimise against: the
title waits on the executor every frame, so its time is the frame time.
**A glitch the player saw:** F10 in game captures the next two frames: every
draw as a `[CAPTURE]` line (target; each stage's texture, swizzled, linear or
DXT, and whether it is a render target; the stage modes) followed by the
probe's full record, in the log, and every render target as PPM under
`~/.local/state/thug-pc-port/captures/<time>/`. `THUG_GL_CAPTURE_AT=<flip>`
does the same in a scripted run, and `THUG_GL_CAPTURE_ON_X8=1` starts one at
the first alpha-blended draw that samples an alpha-less render target (THUG's
reset transition). Captures include each target's alpha as `*_alpha.ppm`.
`THUG_GL_RT_RAW_ALPHA=1` samples such targets' stored alpha instead of 1 (an
A/B switch). The reset transition's trail is skipped on the owner's request
(`is_trail_composite`, a `[PBGL] ... trail skipped` line each time);
`THUG_GL_KEEP_TRAILS=1` draws it as the game asks. Each render target owns
its depth buffer; a clear of a zeta surface reaches every target that last
drew with it (`THUG_GL_ZETA_LOG=1` reports those), but depth contents are not
shared between targets. Captures log clears and each draw's zeta. The renderer also logs the first few bump-env stage set-ups
unasked (`[PBGL] bump-env:`).
The toolkit's fixed-function path generates texture coordinates and applies
texture matrices; `RECOMP_FFP_RAW_TEXCOORDS=1` turns that off for an A/B.
"Smooth text and icons" (on by default; `THUG_UI_UPSCALE=0` is the A/B)
replaces the stage-0 texture of 2D overlay draws (1/w exactly 1, no depth
writes) that has alpha cut-outs with an xBR-magnified copy (4x above 2x
internal scale, else 2x), made on a worker thread and swapped in at a later
bind; `[PBGL] smooth text and icons:` lines log the first few. A swizzled
texture can sit in two cache entries (the key includes a pitch register it
only inherits), so each is magnified twice.
The renderer takes the XDK's screen-space offset back out of every position
(0.53125 on a surface without anti-aliasing, 1/32 pixel per AA factor on an
anti-aliased one), so the title's 0 is the surface's edge at any internal
scale; `THUG_GL_RAW_SCREEN_OFFSET=1` keeps the raw positions (A/B).
`THUG_GL_FLICKER=<dir>` finds one-frame glitches: every presented frame is
read back (slow; diagnosis only), and a frame unlike both its neighbours is
saved with its neighbours as PPM, with a `[FLICKER]` line comparing draws,
uploads, failed binds and dropped triangles.
`THUG_BENCH_SCALES=4.5,2.25,1` cycles the internal scale every
`THUG_BENCH_PERIOD` flips (default 600) in the running scene and prints
`[BENCH]` lines: a like-for-like resolution comparison; `THUG_BENCH_MSAA=1,4,8`
does the same for antialiasing (`THUG_GL_NO_A2C=1` leaves alpha-tested edges
to the plain test). A frame dump is of the internal surface, before the
brightness pass; the overlay and brightness only exist in the window, so
check them with a visible run and a screenshot. `THUG_OVERLAY_OPEN=1`
starts with the F1 overlay open (for screenshots). The player's settings live
in `~/.config/thug-pc-port/settings.ini` and are loaded by every run, so a
test that depends on one should set its `THUG_*` variable explicitly. The
defaults (`src/ui/settings.c`) are 1080 lines (`THUG_RES_SCALE=2.25`), 4x
MSAA, smooth text and icons and a 60 fps cap, headless too: a benchmark sets `THUG_FPS_LIMIT=0`, and
a diagnosis run that wants small dumps sets `THUG_RES_SCALE=1`. A test that
must not touch the player's files points `XDG_CONFIG_HOME` and
`XDG_STATE_HOME` at scratch folders. A headless run that needs menu input:
`RECOMP_PAD_LIVE=<file>`, with one button per appended line. The toolkit's own guides are authoritative:
`vendor/xboxrecomp/docs/INDEX.md`, especially its "By Symptom" table,
`docs/pipeline/06-debugging.md` and
`docs/runtime/environment-variables.md` (every `RECOMP_*` switch).

## Environment

- Linux x86-64: clang or GCC, CMake 3.20+, Ninja, SDL2, libepoxy, Python
  3.10+ with `capstone`.
- Windows 10/11 x64 (since 2026-11-11): the same SDL2 + OpenGL renderer,
  built in the MSYS2 **CLANG64** shell (packages in the README), not the
  toolkit's D3D11 backend: THUG renders through the pushbuffer executor, not
  the D3D8 calls that backend intercepts. The workflow above is the same
  there (`scripts/build.sh`, `scripts/regen.sh`) in a CLANG64 shell;
  `scripts\build.bat` runs `build.sh` from any other shell (the owner's way),
  and from a non-MSYS2 shell a single command runs as
  `MSYSTEM=CLANG64 C:\msys64\usr\bin\bash.exe -lc "cd <repo> && ..."`.
  SDL2 there is sdl2-compat over SDL3 (MSYS2 replaced SDL2 with it). `thug.exe` is a windowed
  program: its log is `%LOCALAPPDATA%\thug-pc-port\thug.log` unless stderr
  is redirected (an MSYS2 terminal's stderr is a pipe, so it stays there),
  settings `%APPDATA%\thug-pc-port\settings.ini`. A test that must not touch
  the player's files points `APPDATA` and `LOCALAPPDATA` at scratch folders.
  `THUG_DEBUG_ATTACH` is Linux-only; MSYS2's `gdb -p <pid>` attaches to the
  game as it is. State and open items: `docs/WINDOWS.md`.
- The toolkit's D3D8 translation (`vendor/xboxrecomp/src/d3d`, D3D11 on
  Windows, OpenGL elsewhere) is not THUG's path. A rendering bug is in the
  executor (`nv2a_pb_exec.c`) or the port's renderer (`src/gpu/nv2a_pb_gl.c`).

## Title-specific knowledge

Keep `docs/NOTES.md` current with anything learned about the binary:
identified functions, globals, engine subsystems, and what the PS2 decomp
([andrewnakas/thug-decomp](https://github.com/andrewnakas/thug-decomp),
SLUS-20731) says about the same code. The PS2 and Xbox builds share the
Neversoft codebase, so names and struct layouts from the decomp often carry
over. Match them through shared strings and call structure, never by address.
