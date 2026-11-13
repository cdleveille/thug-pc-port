# Title notes: Tony Hawk's Underground (Xbox, NTSC-U)

Facts about the binary and the decisions made around it. Keep this current.

## The binary

| | |
|---|---|
| Disc | Tony Hawk's Underground (USA), full Redump-style image, game partition at `0x18300000` |
| `default.xbe` SHA-1 | `2951de954c96a8b962ba1cde34548fa82ae668d7` |
| `default.xbe` SHA-256 | `02f456feb59dd8d77276027bc87bb1f127eae804ca17c001a4a34a35b01e5bd9` |
| Title ID | `0x4156002D` |
| Build date | 2003-09-19 22:36:02 UTC (cert 2003-09-26) |
| Debug path | `c:\skate5\Build\XBox\Final\Skate5.exe` (Skate5 is the internal name) |
| XDK | 1.0.5659 (XAPILIB, D3D8, D3DX8, XGRAPHC, DSOUND, XBOXKRNL, LIBC, XNETS) |
| Base / image size | `0x00010000` / `0x00372480` |
| Entry point | `0x00177858` |
| Kernel thunks | `0x00207D60`, 135 imports |
| RTTI | none (`tools.rtti` finds no MSVC type descriptors) |

### Sections

| Section | VA | Size | Contents |
|---|---|---|---|
| `.text` | `0x00011000` | `0x17A8E0` | game code and CRT |
| `D3D` | `0x0018B8E0` | `0x13268` | XDK Direct3D 8 (statically linked) |
| `D3DX` | `0x0019EB60` | `0x270` | D3DX helpers |
| `XGRPH` | `0x0019EDE0` | `0x1F498` | XGraphics (swizzle etc.) |
| `DSOUND` | `0x001BE280` | `0xA3BC` | DirectSound |
| `WMADEC` | `0x001C8640` | `0x19274` | WMA decoder (music) |
| `BINK*` | `0x001E18C0` to `0x001F6D4C` | about 0x15000 | Bink video decoder and blitters. Four sections share the name `BINKYUY2` |
| `XNET` | `0x001F6D60` | `0x98A4` | Xbox networking (system link) |
| `XPP` | `0x00200620` | `0x7738` | XAPI peripherals (pads, memory units) |
| `.rdata` | `0x00207D60` | `0x1E080` | read-only data, kernel thunk table at its start |
| `.data` | `0x00225DE0` | `0x1430F4` | data (raw 0x1D334; rest is BSS) |
| `DOLBY` | `0x00368EE0` | `0x7180` | Dolby DSP microcode for the APU. Not x86. |
| `BINKDATA` | `0x00370060` | `0xF600` | Bink tables. Not code. |

## Disc contents

| Path | Notes |
|---|---|
| `data/movies/*.bik` | 41 Bink movies (1.6 GB). Decoded by the statically linked Bink code, not by the toolkit's video player. |
| `data/streams/*.wad` | 4 audio stream archives (1.2 GB) |
| `data/pre/*.prx` | 183 level/asset packages ("pre" files) |
| `data/scripts/*.qb`, `*.qb` | QScript bytecode: much of the game logic lives here, not in C++ |
| `*.xbx` | Xbox-specific asset formats (models, textures, cutscenes) |

## Decisions

- **2026-11-10: Target the Xbox USA build with xboxrecomp.** It's x86 and
  little-endian like the PC, and it has a mature D3D8 translation runtime with
  Linux (OpenGL 3.3) and Windows (D3D11) back ends. Similar-era titles are
  already playable with this toolkit.
  - PS2 was rejected because it renders through VU1 microcode and the GS, and
    PS2Recomp is early.
  - GameCube was rejected because it's big-endian PowerPC and has no
    comparable runtime.
- **2026-11-10: Toolkit pinned at xboxrecomp v0.17.0** (`343b6d8`), as a
  git submodule with the local patches below (now vendored, see next).
- **2026-11-11: Toolkit and Dear ImGui vendored.** Both submodules became
  ordinary files in this repository, under `vendor/` (owner's call: one
  repository, a plain clone builds; the folder was `third_party/` until the
  owner renamed it the same day). The toolkit carries this port's changes in place, so
  `patches/xboxrecomp/` and `scripts/apply-patches.sh` are gone; ImGui keeps
  only the files the build compiles. Upstream bases: `vendor/README.md`.
- **2026-11-11: The executable prepares its own game files** (owner's call:
  running `thug` should be all a player does). The disc image lives in
  `game/` (`rom/` is gone); on first launch the game unpacks it with its own
  XDVDFS reader (`src/host/game_data.c`, after the toolkit's
  `tools/xiso/xdvdfs.py`), checks `default.xbe`'s SHA-1 every launch, and
  refuses missing files or another version with a message box.
  `scripts/extract.sh` and `scripts/play.sh` are gone; the build uses
  `build/bin/thug-extract`, the same code as a command. The log goes to
  `~/.local/state/thug-pc-port/thug.log` unless stderr is redirected.
- **2026-11-11: The reset transition's trail is skipped** (owner's call: it
  looks bad; kept after the real fault below was found). Bailing into water or leaving the play area makes the game
  blend each frame over the last at 15% for about a second (see "Reset
  transition" below), which leaves trails of copies. The renderer draws that
  one composite unblended (`is_trail_composite`), so each frame replaces the
  last. A deliberate departure from the game's own look (the first; see also
  "Smooth text and icons"); `THUG_GL_KEEP_TRAILS=1` restores it. The sharp repeated copies the
  owner saw were mostly a renderer fault, though: the scene surface's depth
  was never cleared (see "Shared depth surface").
- **2026-11-11: Smooth text and icons** (owner's request: on-screen text
  looked low-res, with noisy, stepped edges). The game's text, icons and
  logos are 640x480-era bitmaps (an 11-pixel font in a 256x256 P8 atlas,
  `0x80CCD000` for menus, `0x80CE1000` for trick text, whose grey speckle is
  the font's own look). Compared offline: Catmull-Rom sharpens only a little,
  re-cutting the alpha edge brings the texel staircase back, and xBR redraws
  the outlines as curves (clear at 4K, modest at 1080p). The renderer now
  magnifies such textures with xBR (`src/gpu/xbr.c`, written for the port
  from Hyllian's description, alpha counted in the edge test), on a worker
  thread. A departure from the original look like the trail above, so it is
  an overlay setting, on by default (owner's request); `THUG_UI_UPSCALE=0`
  turns it off. HD replacement textures, the only route to real detail,
  remain an option for later.
- **2026-11-11: The Windows build is the Linux build** (the plan in
  `docs/WINDOWS.md`): the same SDL2 + OpenGL 3.3 renderer, window loop,
  overlay and input, compiled with MSYS2 CLANG64 (clang, winpthreads), not
  MSVC and not the toolkit's D3D11 path, which THUG never goes through.
  Windows-specific choices:
  - `thug.exe` is a windowed (GUI-subsystem) program, so no console window
    opens beside it; `-DTHUG_WINDOWS_CONSOLE=ON` builds a console one. The log
    is `%LOCALAPPDATA%\thug-pc-port\thug.log`, settings
    `%APPDATA%\thug-pc-port\settings.ini`, captures under
    `%LOCALAPPDATA%\thug-pc-port\captures`. A terminal it is started from
    still gets the player's messages (`AttachConsole`), and a redirected
    stderr is left alone, as on Linux.
  - `RECOMP_WIN_PLACEHOLDER=1` is a default: the whole 4 GB guest window is
    reserved in one placeholder before anything is mapped, because the GL
    driver loads before the guest maps its memory (the POSIX side reserves
    the whole window for the same reason).
  - The build copies the toolchain DLLs (SDL2 and SDL3, libepoxy, libc++,
    winpthread) next to `thug.exe` (`scripts/copy_runtime_dlls.cmake`).
    Static linking was not used: the toolkit's input library links the SDL2
    DLL, and a static SDL in the game beside it would make two SDLs.
  - SDL2 is MSYS2's `sdl2-compat` over SDL3: MSYS2 marks it as replacing the
    SDL2 package, so `pacman -Syu` installs it whatever one asks for. The
    game was first brought up on real SDL2 2.32.10; on sdl2-compat two
    things differed, both fixed: SDL2's DPI hint is not passed on (the
    process now declares itself per-monitor aware through the Windows API),
    and SDL3.dll is loaded at run time rather than imported (the DLL copy
    knows that pair).
  - `scripts\build.bat` builds from any Windows shell by running
    `scripts/build.sh` in CLANG64, and installs MSYS2 and the build
    packages itself when they are missing (no option to remember). The
    owner does not want to switch shells to build.
  - The frame cap sleeps on a high-resolution waitable timer
    (`src/host/host_sleep.c`): winpthreads' `clock_nanosleep` rounds to the
    system tick.
  - The window is per-monitor DPI aware (`SetProcessDpiAwarenessContext`,
    and SDL2's `SDL_WINDOWS_DPI_AWARENESS`), so a scaled 4K desktop gets
    real pixels, not a stretched window.
  - Closing the window and restarting end the process with
    `TerminateProcess`, not `ExitProcess`, which would run the GL driver's
    DLL detach after killing threads that may hold its locks. Restarting is
    `CreateProcessW` with the starting command line and environment (no
    exec on Windows; the PID changes).
- **2026-11-12: Performance: what was kept, what was not, and where the
  time goes.** Profiled on Windows with the built-in sampler (`THUG_PROF`,
  `scripts/prof_report.py`). In a level the game's thread does about 18% real
  work and spins the rest in D3D's kickoff (`sub_00190730`, the PFB flush
  wait) on the NV2A executor, which is the critical path; a game worker
  (`sub_00175520`) spins a core in `KeWaitForMultipleObjects` but is not on
  it. Kept (toolkit table above; the renderer's own: the builtin `isfinite`,
  indexed draws through `glDrawElementsBaseVertex` from the same persistent
  ring, `THUG_GL_NO_INDEXED=1` the A/B): the executor's cost per index from
  205 to about 130 ns and the frame time per 1,000 draws from about 18 to
  12 ms, measured against the previous commit in alternating runs of the
  same route (headless, 1080p, uncapped; about 1.5x in levels). Tried and
  dropped, measured on the same route:
  - `-march=x86-64-v3` (AVX2): no gain (noise).
  - `-O3`: about 3%, inside run-to-run noise (which is up to 10% on this
    machine); not worth the compile time.
  - A render thread for GL (Black's design): GL submission is only about 1 ms
    a frame of the executor's time, and copying each batch into the queue cost
    about as much as it saved; the end-of-walk drain needed for safety (the
    title may reuse memory once a segment is acknowledged) took the rest.
  - Vertex-program operands as pointers instead of copies: slower (58-65
    against 53-55 ns per vertex in a micro-benchmark of THUG-like programs);
    the copies are cheap in L1, and transposing every input read costs more.
    In the game the same interpreter costs about 130 ns a vertex: cache
    pressure, not arithmetic.
  What is left is mostly vertex work on the CPU: about half of every walk is
  transform (vertex programs and fixed function), then assembly and clipping.
  The next large step is the one Black's SDK takes: vertex programs (and the
  fixed-function transform) as GPU shaders (`docs/WINDOWS.md`, "Still open").
- **2026-11-12: What Black's recompiler does differently** (TGP482's
  XboxXBE-Recomp SDK, MIT, which Black, the Splinter Cell, 007 and Red Dead
  Revolver ports use). It implements the XDK's Direct3D 8 at the API level
  (found by signatures) and draws with D3D12 on a render thread, translating
  vertex programs, combiners and the fixed-function pipeline to HLSL; there
  is no pushbuffer and no CPU vertex work. Its generated code passes the
  registers as `X86Context& __restrict` with the guest memory base as a
  parameter, flags in locals, and plain (non-volatile, `may_alias`) memory
  accesses, where xboxrecomp uses thread-local globals and `volatile`
  accesses through a global offset. For THUG the D3D route would replace the
  whole renderer; the vertex-program translation (`runtime/gpu/shaders.cpp`)
  is the part worth borrowing. The code-generation model would speed the
  game's own thread, which is not the bottleneck today.
- **2026-11-13: NVIDIA's threaded optimisations on X11 only.** A build
  copied to a second CachyOS PC (GTX 1080 Ti, driver 580.196, KDE Plasma
  Wayland) aborted at the first shader compile: "No provider of
  eglGetCurrentContext found. Requires one of: EGL 14". On Wayland SDL's
  context is EGL, and with `__GL_THREADED_OPTIMIZATIONS=1` NVIDIA's EGL
  rejects its own current display (`eglQueryString(eglGetCurrentDisplay(),
  EGL_VERSION)` fails with `EGL_BAD_DISPLAY`); libepoxy, asking whether the
  context is desktop GL, aborts on that. Reproduced with a small SDL2 +
  libepoxy program; GLX (X11) is unaffected. `nv2a_pb_gl_driver_thread` now
  sets the variable once SDL has picked its video driver, for `x11` only, and
  before the first window (the progress window's too), which is still in time
  for the driver. Mesa's `mesa_glthread` is unchanged;
  `THUG_GL_NO_DRIVER_THREAD=1` still turns both off. With libepoxy gone
  (next entry) the variable still cannot go on under Wayland with that
  driver: forced on, the executor's thread fails `eglMakeCurrent` ("error
  EGL_SUCCESS") and nothing draws. The owner's RTX 4080 (also KDE Wayland,
  a newer driver) ran with it, unmeasured; there `SDL_VIDEODRIVER=x11` or
  setting it explicitly (an explicit setting wins) brings it back.
- **2026-11-13: One self-contained Linux executable** (owner's request: copy
  `thug` to another PC and run it). `ldd build/bin/thug` now lists only libc
  and libm (74 MB, RelWithDebInfo); checked on the GTX 1080 Ti PC: native
  Wayland (libwayland-client, libdecor, NVIDIA EGL loaded at run time), X11
  with `SDL_VIDEODRIVER=x11` (GLX), headless frame dumps.
  - SDL 2.32.10 is vendored (`vendor/SDL`, owner's call over a download at
    configure time) and linked static on Linux (`THUG_BUNDLED_SDL`, on by
    default; off links the system's SDL2). Its X11, Wayland, PipeWire,
    PulseAudio and ALSA back ends load their libraries at run time. The
    toolkit's `find_package(SDL2)` calls are pointed at the same static
    library (an empty `SDL2Config.cmake` in the build folder), so there is one
    SDL. Windows keeps the DLLs (the decision above stands there).
  - Real SDL2, unlike sdl2-compat over SDL3, tries X11 before Wayland, so
    `thug_window_hints` asks for `wayland,x11` (owner's call: native Wayland
    as before); `SDL_VIDEODRIVER` still overrides.
  - libepoxy is replaced by glad 2 (`vendor/glad`: GL 3.3 core plus
    `ARB_buffer_storage`, `KHR_debug`, `ARB_sample_shading` and the two
    anisotropic-filtering extensions), loaded once from
    `SDL_GL_GetProcAddress` after the context is made, on every platform.
    Without epoxy's GLX/EGL guessing there is nothing left to abort on the
    driver quirk above. The toolkit's D3D8 layer, whose POSIX back end needs
    libepoxy, is replaced on Linux by an interface target with its headers:
    THUG never draws through it.
  - OpenSSL is no longer linked (toolkit change below), and
    libstdc++/libgcc are linked static.
  - What remains from the system: glibc and the GL driver, which must match
    the installed kernel driver. The glibc floor is the build machine's
    (2.43 on CachyOS in 2026-11: `atan2f@GLIBC_2.43`), so a build for older
    distributions has to be made on one: `scripts/release.sh` (next entry).
- **2026-11-13: Linux releases are compiled in an Ubuntu 22.04 container**
  (`scripts/release.sh`), so they run on glibc 2.35 and newer: Ubuntu 22.04,
  Debian 12, Fedora 36 and later.
  - Not GitHub Actions: lifting needs `default.xbe`, which must not reach
    GitHub (hard rule 1). Docker on the owner's machine mounts the repository
    instead, so nothing is copied into the image.
  - Lifting stays on the host (`scripts/build.sh --lift-only`): a container's
    capstone from pip could differ from the tested one and lift differently.
    Only the compile moves.
  - clang 23 from apt.llvm.org, the version the port is developed with
    (`scripts/release/Dockerfile`, the key's fingerprint checked).
  - The container installs every SDL back end's headers, and
    `THUG_REQUIRE_SDL_BACKENDS` makes a missing one a configure error
    (otherwise a warning): SDL drops a back end silently and the build still
    succeeds.
  - `scripts/release/package.sh` refuses an executable that needs a glibc
    newer than 2.35, links the C++ runtime as a shared library, or names a
    library outside glibc. It ships the executable with its debug info
    removed (`--strip-debug`: the symbol tables, and so the crash
    reporter's names, stay) and keeps the debug info beside the release.
  - Jobs are capped by the container's memory, about 700 MB each: the
    largest lifted chunk peaks at about 320 MB in clang, and Docker
    Desktop's VM has 8 GB by default.
- **2026-11-10: Bring-up uses the pushbuffer executor** (`RECOMP_PB_EXEC`).
  The game's own XDK D3D runs unmodified, and the executor consumes its NV2A
  command stream. See "Open questions" for the long-term renderer.

## Toolkit changes

These are general fixes, not THUG-specific ones, made in place in the
vendored `vendor/xboxrecomp` (upstream base: `vendor/README.md`).
They should go upstream. The first column is where a change was first
carried: `0001` is the former `patches/xboxrecomp/0001-posix-fixes.patch`,
`vendor` a change made after vendoring.

| Patch | Fix | Found by |
|---|---|---|
| 0001 | `mmio_decode_x86_64.h`: the POSIX trapped-MMIO decoder now handles the ALU, CMP and TEST forms the Win32 decoder already had. clang at `-O2` folds a lifted `MEM32(r) & imm` into `test dword [mem], imm32`. New test: `tests/mmio_decode_x86_64`. | DirectSound init segfaulted on `F7 84 03 …` against APU register `0xFE802000` |
| 0001 | `xbox_memory_layout.c`: the AC'97 bus-master write trap is now armed on x86-64 Linux, not only arm64. Before, a channel reset spun forever. | DirectSound hung in `sub_001C7138` waiting for the RR bit of `0xFEC0011B` to clear |
| 0001 | `nv2a_pb_exec.c`: a vertex program output of `w = +inf` is clamped to 1, like `+0`. The hardware and xemu both do this. | Every Bink movie quad (RHW 0, so `rcp` gives `+inf`) drew 0 pixels |
| 0001 | `xbox_memory_layout.c`: POSIX now always reserves the whole 4 GB guest window, as Windows does. It used to do this only under `RECOMP_EXT_VMA`. | With `RECOMP_FB_WINDOW=1`, SDL and the GL driver mapped memory where the contiguous window (guest `0x80000000`) belongs. The title segfaulted at guest `0x81000000`. |
| 0001 | `fb_present_sdl.c`: the SDL window now presents 2×-wide anti-aliased 32-bit surfaces (pitch 640 × 8), as the GDI presenter already did. It takes every other pixel and steps rows by the vertical AA factor. New test: `tests/fb_present_sdl`. | The window stayed black while frame dumps were correct, because THUG presents a 1280×480 surface and the presenter dropped it |
| 0001 | `nv2a_pb_exec.c`, `fb_present*.c`, `video_player.h`: the title bar shows the presented frame's rendering resolution, `<title> \| 640x480 \| FPS: n \| draws: n`. Added `xbox_FramebufferWindowSetResolution`, set at each flip from the drawn surface's unscaled clip size. Test stubs updated. | Owner request |
| 0001 | `nv2a_pb_exec.c`: a draw whose colour surface is at physical 0 writes no colour (depth and stencil still go through), as the clear path already did. On the console physical 0 is kernel memory; here page 0 is XPP's private USB arena and the pages above it are the first contiguous blocks. | THUG left the colour offset at 0 after a depth-only shadow pass, then issued colour-enabled draws there. They overwrote DirectSound's heap (its allocator then spun in `sub_001C2D6A`) and, it seems, the USB structures (the pad stopped responding at the main menu). Caught with a gdb hardware watchpoint: the writer was `put_pixel` on the GPU executor thread. |
| 0001 | `nv2a_pb_exec.c`: `NV097_SET_CONTEXT_DMA_COLOR`/`_ZETA` are resolved through RAMHT to the DMA object's base, and surface offsets are made relative to it. Logged once per handle as `[GPU] context DMA`. | Investigating the above. THUG's objects (handles 9 and 10) are based at 0, so it changes nothing for THUG yet, but it's correct for any title that uses based objects. |
| 0001 | `nv2a_pb_exec.c`: `RECOMP_PB_SURFACES=1` logs each distinct colour surface a draw uses (offset, resolved range, pitch, clip, format, zeta, colour mask). | Diagnostic |
| 0001 | `kernel_bridge.c`: `KeBugCheck` and `KeBugCheckEx` end the process, as on the console. `RECOMP_BUGCHECK_CONTINUE=1` restores returning. | THUG's `exit()` (`sub_00182EEB` → `_doexit` `sub_00182E53` → `KeBugCheck`) returned, leaving the caller 4 bytes off its frame. The texture-copy loop then read a pointer as its bound and spun forever on the level's loading screen. |
| 0001 | `apu_vp.c`: the voice-list walk ends at a handle of 256 or more, which only `0xFFFF` may exceed. Before, an `assert` compiled out in release builds let it index `g_dbg`, `voice_spin` and the resampler state out of bounds. New test: `tests/apu_voice_list` (ASan). | The VP thread segfaulted in `mcpx_apu_vp_frame` during the menu load (owner's windowed run) |
| 0001 | `nv2a_backend.h`, `nv2a_combiner.h`, `nv2a_pb_exec.c`: the back-end interface carries what a GPU renderer needs and the CPU rasteriser already had: surface clip origin, zeta format, every vertex's specular, fog factor and four stages of texture coordinates, the four stage textures, the register combiners, fog colour and clip-plane mode. | Bringing up the GPU renderer (`src/gpu/nv2a_pb_gl.c`) |
| 0001 | `nv2a_backend.h`, `nv2a_pb_exec.c`: `Nv2aTexture` carries the stage's palette address, and `nv2a_backend_decode_texture` uses it. Test: `tests/nv2a_tex_filter` (P8 through its palette, and refused without one). | Every 8-bit indexed (P8) texture failed to decode on the GPU renderer: THUG's fonts and much of its menu art drew as solid blocks |
| 0001 | `nv2a_pb_exec.c`: a screen-space batch hands its stage-0 coordinates on as the title gave them (texels for a linear texture, normalised for a swizzled one), the same units as a program batch. | Linear textures sampled at the wrong scale on the GPU renderer |
| 0001 | `nv2a_pb_exec.c`: a post-transform vertex cache. Within a batch each vertex index is transformed once, not once per occurrence (direct-mapped, 4096 entries). `RECOMP_NO_VCACHE=1` turns it off. | Profiling in a level: the vertex-program interpreter was most of the executor thread's time, and indexed meshes name each vertex several times |
| 0001 | `kernel_hal.c`, `kernel_xbox.c`, `kernel.h`: the AV pack query (`AvSendTVEncoderOption` option 6) carries the dashboard's video flags in bits 16–21, as the console's kernel does. `xbox_HostVideoFlags()` is the one source for them. Test: `tests/kernel_eeprom_settings`. | With the widescreen setting, D3D's display-mode search (`sub_00197364`) refused every widescreen mode because the AV word lacked bit `0x10000`, and `CreateDevice` failed |
| 0001 | `kernel_xbox.c`: `RECOMP_VIDEO_FLAGS` (hex) and `RECOMP_WIDESCREEN=1` set the reported video settings. | Widescreen |
| 0001 | `nv2a_pb_exec.c`: fixed-function lighting follows `SET_COLOR_MATERIAL` as xemu and nouveau (NV20) do: base colour = ambient source × `MATERIAL_EMISSION` (`0x03A8`, where D3D puts the global ambient when the ambient comes from the vertex) + emission source, and the vertex specular colour can be a source. Test: `tests/nv2a_ffp_backend`. | THUG's pre-lit cutscene props (colour material `0x14`) were black where they faced away from both lights: the Chad Muska flyer in the story intro, Muska's car, the dresser in the player's room |
| 0001 | `nv2a_backend.h`, `nv2a_pb_exec.c`: polygon offset (`SET_POLY_OFFSET_FILL_ENABLE` `0x338`, `_SCALE_FACTOR` `0x384`, `_BIAS` `0x388`) and `SET_CONTROL0` reach the back end. Test: `tests/nv2a_ffp_backend` (`0x338` enables bias; `0x340`, `SET_ALPHA_REF`, must not). | THUG draws about 100 decal batches a frame with depth bias; without it they z-fought the surfaces under them (flicker). The first version read the enable from `0x340`, which THUG keeps at 1, so every draw took the last bias set: the skater's eyes and mouth lost to the face under them |
| 0001 | `nv2a_pb_scan.c`, `xbox_memory_layout.c`: the default pushbuffer scan walks from where it last stopped to PUT, following JUMPs (the ring's wrap) and ending exactly at PUT (`nv2a_pb_scan_to_put`); a method whose parameters are not all written yet waits for the next kick; a GET the title moved restarts the walk there. `RECOMP_PB_SEGMENT_LEGACY=1` restores the old scan. Test: `tests/nv2a_pb_walk`. | One-frame glitches about once a second (and a frame of exploded geometry now and then): at each ring wrap the old scan guessed the ring's bounds from the lowest and highest PUT seen, and dropped this lap's first commands or replayed an old lap's. In THUG the main menu's background panel was drawn with the previous mesh's vertex arrays (the dropped commands had set its own), and once a whole stale frame was drawn again |
| 0001 | `xbox_memory_layout.c`, `nv2a_pb_exec.c`: `xbox_Nv2aTrackFence` + `nv2a_pb_semaphore_from_fence_word(1)`: an experimental fence that the executor writes from D3D's semaphore releases instead of mirroring "all submitted is consumed". Off in THUG (`THUG_FENCE_SEMAPHORE=1`): it needs the software-method interrupt (see the header). | Investigating the flicker |
| 0001 | `nv2a_vsh_interp.c/.h`: `nv2a_vsh_run_batch`, the decoded vertex program run for up to 8 vertices at once over structure-of-arrays registers (one decode per instruction, vectorisable lane loops), bit-identical to the scalar runner. `RECOMP_VSH_BATCH_CHECK=1` compares them live; `RECOMP_VSH_NO_BATCH=1` turns it off. Test: `tests/nv2a_vsh` (fuzzed against random programs). | Vertex programs were about 310 ns a vertex, the largest single cost of the executor in a level |
| 0001 | `nv2a_pb_exec.c`: a batch's indices are deduplicated into a list of unique vertices before transforming (assembly reads them through a map instead of copying 128 bytes per repeat); program inputs are resolved once per batch, bounds-checked against the batch's highest index, and read in a tight loop (`xf_gather_fast`, identical to `fetch_attr`); an unclipped triangle is no longer copied, and the back-end path skips the CPU rasteriser's triangle set-up. | Same profile |
| 0001 | `nv2a_pb_exec.c`, `nv2a_pb_scan.c`: `RECOMP_EXEC_PROFILE=1` prints, every 120 flips, the executor's time per frame split into the walk, vertex transform (program and fixed-function, gather and interpreter), triangle assembly and the back end. | Diagnostic |
| 0001 | `nv2a_pb_exec.c`: with `RECOMP_TRACE_TEX0`, the trace also prints the program constants (c0-c15), the first vertex's input, and the array, draw and polygon-offset methods back to both the previous draw and the vertex-array setup in force. | Diagnostic |
| 0001 | `xinput_device.c`, `xinput_xbox.h`: `xbox_InputSetBlocked(on)`. While set, every port reports a connected pad at rest with a new packet number, so a host overlay can take the input focus without the title seeing a disconnect. Test: `tests/input_sdl_hotplug`. | The F1 settings overlay |
| 0001 | `xinput_device.c`, `xinput_xbox.h`: `xbox_InputSetRumbleScale(scale)` multiplies every motor speed the title asks for (0 = off, which also stops the motors); `xbox_InputSetBlocked(1)` now stops the motors too and passes no rumble until cleared. Test: `tests/input_sdl_hotplug`. | The overlay's vibration setting; a pad that kept rumbling while the overlay was open |
| vendor | `keyboard.c`, `keyboard.h`: `xbox_InputKeyboardSetBindings(b)` installs a port's own keyboard bindings in place of the built-in map (NULL restores it); `xbox_InputKeyboardBindings` reports what is in force. Test: `tests/input_keyboard_map`. | THUG's menu keys (Enter/Space for A, Escape for START, WASD as the d-pad), which the fixed map could not express |
| vendor | `keyboard.c`, `keyboard.h`, `src/input/README.md`: binding layers. `xbox_InputKeyboardSetLayer(n, b)` installs up to four sets of bindings and `xbox_InputKeyboardUseLayer(n)` chooses the one in force from any thread at any time (one `int`; a layer is only rewritten while out of force, so the poll needs no lock); `SetBindings` is layer 0. A poll now reads the bindings before the keys. Test: `tests/input_keyboard_map` (`test_layers`). | Escape as START while skating and B in the menus (owner request): `SetBindings` could not be called safely while the title polls |
| vendor | `xinput_device.c`, `keyboard_sdl.c/.h`: with the main pump on, only the thread that pumps SDL events opens and closes controllers (`xbox_InputSdlRefreshControllers`, run by `xbox_InputSdlInit` and on `SDL_CONTROLLERDEVICEADDED`/`REMOVED` through `xbox_InputSdlEvent`); the title's threads only read open pads, and a pad that is pulled out reads as disconnected until that thread closes it. Each open and close is logged (`[INPUT] port N: ... connected`). Tests: `tests/input_sdl_hotplug` (no enumeration off the pumping thread, under a concurrent hotplug), `tests/input_keyboard_sdl` (device events refresh). | Plugging a controller in mid-game crashed (glibc `malloc(): unaligned tcache chunk`, or SIGSEGV in `malloc`): every poll of a port with no pad, from the emulated USB thread, re-enumerated with `SDL_NumJoysticks`/`SDL_GameControllerOpen(index)`, and on sdl2-compat over SDL3 that raced the main thread handling the same hotplug |
| vendor | `nv2a_pb_exec.c`: the fixed-function path generates texture coordinates (`SET_TEXGEN_*` 0x03C0: eye- and object-linear planes at 0x0840, sphere, normal and reflection maps) and applies texture matrices (`SET_TEXTURE_MATRIX_ENABLE` 0x0420, matrices at 0x06C0, rows of four, a 2D coordinate read as (s, t, 0, 1)), as xemu's `vsh-ff.c`. `RECOMP_FFP_RAW_TEXCOORDS=1` restores the raw streams. Test: `tests/nv2a_ffp_backend`. | Manhattan's water stood still, and reflection-mapped surfaces showed their raw coordinate stream: THUG scrolls textures and lays reflections through these |
| 0001 | `apu.h`, `apu_core.c`: `xbox_ApuSetVolume(gain)`, a host master volume on every period sent to the audio device (after the `RECOMP_APU_WAV` dump, which keeps the title's own level), ramped across one period so a change never clicks; `apu_apply_gain` does the scaling. New test: `tests/apu_volume`. | The overlay's volume and "mute in background" |
| 0001 | `nv2a_pb_exec.c`: `RECOMP_TRACE_TEX0=<hex>[,n]` prints how the first n batches sampling that texture in stage 0 were transformed and lit (program or fixed function, colour material, lights, attributes). | Diagnostic |
| vendor | `xinput_device.c`: vibration holds as the Xbox pad's does, until the title sends other speeds. Each port keeps the title's last request; SDL gets it with its longest duration (0xFFFF ms) and the input poll re-arms it every 30 s while it should run. The player's strength and the overlay's focus re-send it when they change, so an overlay closed mid-rumble resumes it (XInput on Windows holds the motors itself; it gets the same re-send). Test: `tests/input_sdl_hotplug` (held, re-armed, a stop stays stopped, resumed after the focus returns; fails with a one-second hold or without the re-arm). | A grind's rumble stopped after a second (owner report). THUG sends a rumble report only when a speed changes (`RECOMP_USB_TRACE=1`: one report as an effect starts, one with 0 as it ends), and every SDL request was given 1000 ms |
| vendor | `keyboard_sdl.c/.h`, `xinput_device.c`, `xinput_xbox.h`, `src/input/CMakeLists.txt`: on Windows, a host that runs its own SDL window feeds the keys from SDL events, as on POSIX: `keyboard_sdl.c` is built when SDL2 is found (`XBOXRECOMP_INPUT_SDL_KEYBOARD`), and the XInput backend gains `xbox_InputSetMainPump` and `xbox_InputSdlRefreshControllers` as no-ops (XInput needs no pump and no hotplug handling). Pads stay on XInput. Test: `tests/input_keyboard_sdl` now also runs on Windows; `tests/input_xinput_empty_ports` (below). | The Windows build: `host_window.c` runs one SDL main loop on both systems |
| vendor | `xinput_device.c`: a port found empty is asked about again only after a second (`XINPUT_EMPTY_RETRY_MS`). A connected pad is still read on every call, and one pulled out reads as disconnected at once; connects and disconnects are logged (`[INPUT] port N: ...`). The `#pragma comment(lib)` is MSVC-only. New test: `tests/input_xinput_empty_ports` (a fake XInput counts the calls; fails with the old rule). | `XInputGetState` on an empty slot re-enumerates the devices (about a millisecond a call), and the title's USB poll asks for all four ports every frame or more |
| vendor | `nv2a_pb_exec.c`: `NV_TLS` (the DXT block cache) is `__declspec(thread)` only under MSVC, `__thread` otherwise, as every other thread-local macro in the toolkit already was. Under MinGW clang the declspec was ignored with a warning, so the CPU rasteriser's workers shared one cache. Checked by the build: the `unknown attribute 'thread'` warning is gone. | The first MinGW build of THUG |
| vendor | `src/platform/CMakeLists.txt`: under MinGW the `platform` interface links `Threads::Threads` (winpthreads), and the `nv2a_ffp_backend`, `nv2a_pb_constants`, `nv2a_pb_walk`, `nv2a_raster_batch` and `nv2a_tex_filter` tests link `pthread` there too. `nv2a_pb_scan.c` and `nv2a_pb_exec.c` call `clock_gettime` (profiling) on every host. Checked by those suites and 36 others, which all failed to link (`undefined symbol: clock_gettime64`) and now pass. | Running the toolkit's suites under MSYS2 CLANG64 |
| vendor | `nv2a_pb_exec.c`: the Windows raster pool's small-batch path sets `s_raster_last_workers = 1`, as the pthreads branch does; it kept the previous batch's count. Diagnostic only (the caller drew the batch either way). Test: `tests/nv2a_raster_batch`, which failed on Windows ("a batch of 2944 pixels woke 4 workers") and passes. | Same |
| vendor | `nv2a_pb_exec.c`, `xbox_memory_layout.c`, `apu_mmio_hook.c`: three `getenv` calls on hot paths are read once: `RECOMP_PB_EXEC_VERBOSE` in `draw_primitive` (every draw), `RECOMP_PB_SEGMENT_LEGACY` in the ack thread (every kick), `RECOMP_APU_TRACE` in the APU trap handler (every trapped access). The `[GPU] draw #N` line every 200 draws only prints under `RECOMP_PB_EXEC_VERBOSE`. Checked by the profiler (no CRT in the executor's samples) and the executor suites. | Profiling on Windows (`THUG_PROF`): Windows' UCRT `getenv` scans the whole environment under a lock, and the per-draw one was over 40% of the executor's time in a level; the log line was another 9% (unbuffered stderr, a system call each) |
| vendor | `nv2a_pb_exec.c`: `CLEAR_SURFACE` clears the CPU rasteriser's depth buffer only when no back end draws; a back end clears its own (`clear_surface` hands it the clear) and the CPU one is never read then. | 1280x1024 floats per clear, 6% of the executor in a level |
| vendor | `xbox_memory_layout.c` (`nv2a_ack_busy_bits`): `xbox_VblankAckHeld()`, which reads the clock, is asked once per pass of the ack loop for the two vblank registers, not once per acknowledged register. | About 9% of the executor thread (six clock reads every pass) |
| vendor | `nv2a_pb_exec.c`: `isfinite`, `isinf` and `signbit` are the compiler builtins under GCC and clang (MinGW's are calls into `__fpclassify` with branches; the answers are the same), and `pack_color` clamps with compares that become min/max and converts through `int` (bit-identical for finite input; a NaN packs to 0, as the unsigned conversion of a NaN gave on x86-64). | Per-vertex costs in the profile: `pack_color` was 40% of `backend_from_xf` |
| vendor | `nv2a_backend.h`, `nv2a_pb_exec.c`: indexed back-end batches. A back end that sets `NV2A_BACKEND_INDEXED` in the new `Nv2aBackend.flags` gets each vertex a batch's triangles share once (`Nv2aBatch.vertices`, `count`) and the triangle list as `indices`/`index_count`; corners made by clipping get vertices of their own. Every other back end gets plain triangle lists as before (a three-member initialiser leaves `flags` 0). Test: `tests/nv2a_ffp_backend` (a four-vertex strip: six vertices for a plain back end; four vertices and six indices for an indexed one, naming the same triangles). | `backend_from_xf` and the renderer's vertex fill converted and copied every shared vertex once per triangle (about 2.7 times per unique vertex in a level) |
| vendor | `nv2a_pb_exec.c`: `RECOMP_EXEC_PROFILE` also prints walks (kicked segments) per frame. | Diagnostic: THUG kicks 15-36 segments a frame of 25-40 draws each |
| vendor | `nv2a_pb_exec.c` (`batch_is_ffp`): in fixed-function mode an inline batch (`INLINE_ARRAY`, `SET_VERTEX4F`) is transformed like an array batch. Before, every inline batch went to the screen-space path, which reads positions as pixels. XDK D3D draws pre-transformed (XYZRHW) vertices through its passthrough program, so fixed mode always means object space. Test: `tests/nv2a_ffp_backend` (an inline triangle in world units, coloured and textured). | THUG's breaking glass (`DrawVerticesUP`, fixed function) stuck to the screen as white triangles that drifted up as the shards fell: world positions drawn as pixels, texture q 0. In menus and free skate every other inline batch (about 380,000, and the one immediate batch a frame) runs a vertex program, so nothing else changes |
| vendor | `src/kernel/CMakeLists.txt`: POSIX no longer links OpenSSL. Nothing called it: `kernel_crypto.c` has its own SHA-1, RC4 and HMAC-SHA1 on every host. New test: `tests/kernel_crypto` (FIPS 180-1, RFC 2202 and RC4 vectors, HMAC data split across both buffers). | A self-contained THUG executable: `libcrypto` (with zlib, brotli and zstd) was its largest dependency, for no calls |

Not fixed, noted: `win32_compat.c` `VirtualAlloc` maps a `PAGE_NOACCESS`
reserve as read-write, because `PROT_NONE` is 0 and
`prot ? prot : PROT_READ|PROT_WRITE` treats it as "unspecified". Reserved
memory is therefore accessible on POSIX. It's harmless so far, but it could
hide stray accesses.

## Title-side facts found in bring-up

| What | Where | Notes |
|---|---|---|
| XAPI media check | `sub_00178B85` | Overridden (see `docs/OVERRIDES.md`) |
| Security-sector validator | `sub_00178ACE` | Checks the title ID, SHA-1 and RSA signature |
| D3D device pointer | `0x0019C358` | `+0x2C` is the submitted fence; `+0x30` points at the GPU-consumed word. Registered with `xbox_Nv2aMirrorFence`. |
| D3D GPU wait | `sub_00190900` | Spins until consumed passes target |
| D3D mode change at Present | `sub_0018C830` → `sub_0018BEF0` | Calls `AvSetDisplayMode` |
| DirectSound AC'97 channel reset | `sub_001C7138` | Channel table at `0x001C7D88`: PCM-out `0x10`, S/PDIF `0x70` |
| Sound init (game) | `sub_0003EB10` | Calls into DSOUND |
| Render surfaces | color `0x00170000` (1280×480, 2× horizontal AA), front `0x003C8000` (640×480) | Each frame renders wide and then resolves down to 640×480 with one textured triangle |
| XPP USB host-controller init | `sub_002007DC` (to `0x002008E8`) | Reached only through a pointer. The linear sweep misaligned on the data table before it and split off its tail as `sub_0020081B`. Seeded in `config/seed_functions.json`, which fixed it: the pad enumerates and is polled. |
| USB host-controller model | `xbox_OhciInit()` in `src/main.c` | Nothing in the runtime calls it. Without the call, `RECOMP_USB=1` does nothing. |
| XPP USB ISR `cli`/`sti` | `0x002069BF` / `0x002069D0` | Lifted as no-ops; `RECOMP_DISPATCH_GATE=1` stands in for them |
| Frame timing (Neversoft `Tmr`) | vblank ISR `sub_00173E90` (counter `0x35F0C8`), swap callback `sub_00173ED0` (ring of 4 swap deltas at `0x35F040`), frame-length update `sub_00173F80`, `FrameLength()` `sub_001741F0` | Game time is vblanks per swap: `[0x35F030]` = mean of the last four deltas, each clamped to 1..4. `FrameLength()` = slomo `[0x2408AC]` / refresh `[0x2408B0]` × `[0x35F030]`. A frame shorter than a vblank counts as a whole one, so above 60 fps the game runs fast. `sub_00173F80` is overridden to use host time (`docs/OVERRIDES.md`). |
| Device setup | `sub_000BE470` | Builds the present parameters from the video flags (`sub_001758FE`): 640×480, `X8R8G8B8`, 2× horizontal supersampling (`0x1021`), `D24S8`; widescreen adds `D3DPRESENTFLAG_WIDESCREEN`, 720p switches to 1280×720 |
| D3D `CreateDevice` | `sub_00191A10` → `sub_00196C50`; display-mode search `sub_00197364` | Mode table at `0x0019A7F0` (rows of flags, width/height, encoder word) |
| Widescreen | dashboard flag `XC_VIDEO_FLAGS_WIDESCREEN` | THUG supports it natively: Hor+ field of view and a 16:9 HUD, rendered anamorphic into the same 640×480 surfaces. On for any `aspect_ratio` wider than 4:3. The 2D menus and HUD are not compensated for the anamorphic stretch (a dialog box is 1.33× wider at 16:9 than at 4:3, as on the console). |
| Screen modes | script `camera.qb`: `screen_setup_standard` (`SetScreen Aspect = 1.33333 Angle = camera_fov` (72)), `screen_setup_widescreen` (1.77778, `widescreen_camera_fov` 88.18), `screen_setup_letterbox` (the same with `Letterbox = 1`); each sets the global flags `SCREEN_MODE_STANDARD`/`SCREEN_MODE_WIDE` and `current_screen_mode` | Boot (`sub_000A6FA0`) runs `screen_setup_widescreen` when the video flags have widescreen or 720p (`& 3`). Cutscenes in standard mode switch to letterbox and back (`cutscene.qb`, `CutsceneStartVideo`). The in-game Standard/Widescreen/Letterbox menu exists only on PS2 and GameCube (`IsPS2`, `IsNgc` in `gamemenu.qb`). `SCREEN_MODE_WIDE` widens a few layouts in script (the main menu's background plane ×1.333). 88.18° is 72° made Hor+ for 16:9. |
| `SetScreen` | script C function `sub_00117740` (`Aspect`, `Angle`, `Letterbox`) → aspect setter `sub_0008F820` (`[0x2B33DC]`), angle setter `sub_0008FB50` (current angle `[0x2B33D8]`, ratio tan(angle/2)/tan(base/2) at `[0x229C2C]`, base `[0x2B33D4]` = `camera_fov`, then each viewport's camera via `sub_0009ADA0`), letterbox `sub_0009BAE0`; getters `sub_0008F830` (aspect), `sub_0008F850` (base angle), `sub_0008F860` (ratio) | Engine init (`sub_0009C290`) sets 4/3 and the base angle directly. The getters are the only readers; `sub_0008F830` and `sub_0008F860` are overridden for 16:10 (`docs/OVERRIDES.md`). In play the cameras read the ratio a few hundred times a second. |
| Glass shatter | script C function `Shatter` (`sub_001211E0`; `area`, `variance`, `vel_x/y/z`, `spread`, `life`, `bounce`, `bounce_amp`), called by `ShatterAndDie` (`SK4_Scripts.qb`: area 1000, variance 4.6, velocities `Random(...)` of 0/±30 sideways and 10/30/50 up, no `life`). Object built by `sub_00088300` (+0x14 life, default 4.0 from `0x20BE7C`; +0x18 gravity 128.0 from `0x20BE88`; +0x1C bounce height, default -10000, i.e. none; +0x20 bounce_amp 0.8); list at `[0x2B2F54]`; per-frame update `sub_00088A30` (from `sub_0009B840`); drawn by `sub_00088B00` (from `sub_000A7060`), `sub_000A9220` per object: the source mesh's material (`[obj+0x28]+0x14`, `sub_000BA9A0`), its vertex shader (`+0x40`, `sub_000BFC60`), then `DrawVerticesUP` (`sub_0018B970`: triangle list, `[obj+0x24]`×3 vertices at `[obj+0x2C]`, stride byte `+0x18`), so the shards arrive as an `INLINE_ARRAY` in fixed-function mode with the pane's texture and its vertex alpha (`0x66` in Manhattan) | All in real time: the update takes `FrameLength()` once, counts `life` down by it and hands it to `sub_000A93E0` → `sub_000883E0` (pos += vel·dt, vel.y -= 128·dt). Measured `[0x35F030]` ≈ 1.0 at 60 fps (dt 1/60 s, as on the console) and 0.33 uncapped headless. So shards drift for four seconds under a third of real gravity (units are inches) by the game's own numbers. |
| Script archive | `data/pre/qb.prx`: Neversoft PRE (version `0xABCD0003`; entries {size, compressed size, name length, name CRC, name, data}, 4-aligned; LZSS with a 4096-byte ring, F = 18, threshold 2) | The compiled scripts (`.qb`) carry a table of the names they use (token `0x2B`: checksum, name), so a token dump reads almost like source. Unpack to a scratch folder only: it is game content. |
| Shadow / depth textures | depth-only passes into zeta `0x033A1000` (256×256) and `0x03100000`, sampled as format `0x2E` (`LIN_D24S8`) | Not drawn yet: neither renderer samples depth textures |
| Cutscene props | fixed-function T&L, lighting on, colour material `0x14`, two directional lights, global ambient in `MATERIAL_EMISSION` | e.g. the flyer, texture `0x832DB000` (128×256 DXT1) |
| Main menu skater | vertex program, skinned (`0x83390000` texture) | Drawn black on purpose: the program's colour output is RGB 0 with the alpha animating (a fading silhouette). Not a missing texture. |
| Depth state | `SET_CONTROL0` = `0x00100001`; colour clear `0x00506070` | Z-buffer, not w-buffer (bit 16 clear), fixed-point D24S8. Depth bias comes in two forms: D3D's (`0x384`/`0x388` then all three enables `0x330`–`0x338`, negative amounts, toward the camera) and a raw form at the start of the frame that writes only `0x338`, with +2/+4. |
| Skater-select room, left monitor | logo decal texture `0x831E1000` (32×32 DXT5), additive, depth writes on, alpha test ≥ 1, no bias; drawn before the monitor's screen (`0x831E0000`) | The logo sits about 180 depth units in front of the screen (same fixed-function transform), so wherever its alpha passes it blocks the screen, and the clear colour shows through as a blue-grey shape. The game's own commands produce it; not checked against a console. |
| Reset transition (water bail, out of bounds) | scene rendered into `0x80ABC000` (640x480, LIN_X8R8G8B8) instead of the 2x-wide buffer, then one quad over the wide buffer (`0x804E0000`), not cleared: colour v0 x t0, alpha v0.a (`0x26`, 0.15) x t0.a, SRC_ALPHA / ONE_MINUS_SRC_ALPHA, depth ALWAYS | An accumulation trail for about a second: each frame keeps 85% of the last, so anything moving leaves fading copies. Captured from the owner's game (F10) and a scripted out-of-bounds run. The X8 surface must read alpha 1 (it did not: the HUD region, alpha about 0.6 in the surface, faded at the wrong rate). Whether the original shows the same trail is not confirmed. Skipped by default (decision log). |
| Shared depth surface | zeta `0x80864000`, used by the screen buffer `0x804E0000` and the transition's scene surface `0x80ABC000` | Each frame clears it (depth + stencil, flags 3) with `0x804E0000` bound, then draws the scene into `0x80ABC000` with the same zeta. The renderer's targets each own a depth buffer, so a zeta clear now reaches every target that last drew with that zeta; contents are still not shared. |
| Scrolling textures | fixed-function texture matrices; Manhattan: `0x83484000`, `0x83487000` (stage 0) | The translation column moves every frame (about -0.0002 a frame in s, twice that in t): the game animates water and similar surfaces this way, not by rewriting UVs. |
| Reflection maps | `SET_TEXGEN` REFLECTION_MAP on S, T, R with a matrix of scale 0.5, offset 0.5 (e.g. `0x832DA000`, `0x8325D000`, `0x83272000`) | The eye-space reflection vector folded onto a 2D environment map; stage mode stays PROJECT2D/3D, no cube maps seen. |
| Script C-function table | `{name, function}` pairs, about `0x0023DCE8`–`0x0023F480` (1,183 named entries in guest memory) | The quickest way to name a function: find its address as a dword in a memory dump and read the string beside it. Checksums are Neversoft's: CRC-32 of the lowercased name, initial `0xFFFFFFFF`, no final xor. |
| Skate module | pointer at `0x002FCD68`; current level checksum at `+0x398` (`LevelIs`, `sub_0011E870`), `+0x394` read by `GetCurrentLevel` (`sub_0011DFA0`), game mode at `+0x5C` (`sub_000F1880`) | Objects live in the contiguous window (`0x8160AF80` in every run so far). Level checksums seen: `load_skateshop` `0x9F2BAFB7` (the main menu and all its screens: Options, Create-A-Skater, System Link), `load_ny` `0x7276630A` (Manhattan). |
| Game paused | `GameIsPaused` (`sub_0011D430`): byte `+0x40` of the module at `0x002FCD7C` (the one `PauseGame` `sub_0011D400` and `UnPauseGame` `sub_0011D420` act on) | 1 exactly while the pause menu is open, 0 while skating and in the main menu. With the level, it drives Escape (`src/host/controls.c`): B in `load_skateshop` or while paused, START otherwise. |
| `InFrontEnd` | `sub_0011DE80`: the game mode's script struct (`+4`) value `is_frontend` (`0xA6479767`) | Not usable: THUG's main menu runs in the current game mode (`freeskate` at boot, `career` once the save loads, `net` in System Link), all with `is_frontend = 0`. Script structs are lists of components {type byte `+0`, name `+4`, value `+8`, next `+0xC`}; type 1 integer, 0xA structure, 0xD name (an unnamed one includes a global struct); `sub_000202B0` searches them, last match wins. |
| Music paused | byte `0x0024C3BC`, set by `sub_00032010`, which `PauseMusic` (`sub_00115880`) calls; the previous value is kept at `0x0024D3C8` | Not a "menu open" flag, although it looks like one at first: 1 in the pause menu and the first main menu, but the front end clears it when its music starts (after the load prompt), so it reads 0 on most front-end screens. The first Escape switch used it, and Escape stayed START in Options. |
| Fog switch | byte `0x002B2F58`, set by `sub_00088280` from `EnableFog` / `DisableFog` | 0 in the front end and 1 in Manhattan in the memory diff, so it looked like an "in a level" flag; it only tracks fog. |
| Screen-space offset | in every position Xbox D3D hands the GPU: 2D sprites in a 640x480 surface span x 0.53125..640.531; the reset transition's composite into the 2x-wide buffer starts at (0.0625, 0.03125); D3D's own resolve into the 640x480 front buffer uses raw positions (0, 0) | The XDK adds 0.5 + 1/32 on a surface without anti-aliasing and only the 1/32 bias (in real pixels) on an anti-aliased one. Harmless at 640x480 (the NV2A snaps to 1/16 pixel and samples centres), but at a higher internal scale the first half-column and half-row were never drawn and an image drawn through a plain surface sat half a pixel from the same image drawn into the 2x-wide one: a stale line and a small jump at the left edge during menu transitions (owner report), and one MSAA sample of clear colour in every frame's first column. `screen_offset()` in `src/gpu/nv2a_pb_gl.c` removes it. |
| Sky dome | level skies (seen in New Jersey) | Its top vertices project tens of millions of pixels off screen. A renderer that drops "absurd" positions loses the top of the sky (seen as a hard edge between the dome and the clear colour). |
| Culling | every draw sets cull face `GL_FRONT` (`0x404`), front face `GL_CW` (`0x900`) | With culling enabled in the GPU renderer, world triangles and the sky (both negative signed area in surface pixels, y down) are culled together while the skater draws correctly, so fixed-function and vertex-program output disagree on winding somewhere. Culling stays off (`RECOMP_GL_NO_CULL=1`) until that is understood. Strips also need `RECOMP_STRIP_ALT_WINDING=1` once culling is on. |
| Load-wait loop | `sub_00106EE0` (loop `0x00106F50`–`0x00106F6B`) | Spins, waiting a vblank each pass (`sub_00174090`), until `sub_00031C40` says the streams are buffered. Fixed by `RECOMP_ASYNC_IO` (see the stream start below). |
| Stream objects | `[0x24E7DC]` (checked by `sub_000358C0`), `[0x24E7E0]` (checked by `sub_00033A70`) | Ready means state `+0xA4 == 1` and either done `+0x0D` or buffered `+0x9C >= +0xA0 + +0x4C`. File handle at `+0x00`, OVERLAPPED at `+0x08`, read buffer at `+0x98` (`0x0024E7F0`, a static in .data). |
| Stream start | `sub_00033940` | `ReadFile(h, buf, 0x4000, NULL, ov)`. An immediate TRUE sets done without counting the bytes. It relies on async handles always returning pending, as the hardware does. |
| Stream continue | `sub_00033830` | `GetOverlappedResult` (`sub_00174E0C`); error 996 means not ready yet; adds the bytes to `+0x9C`, then the next `ReadFile`. |
| XAPI ReadFile / GetLastError | `sub_001748D8` / `sub_00174D9F` | Without an OVERLAPPED, a pending result waits on the file handle. |
| vblank wait | `sub_00174090` | Spins until the 64-bit counter at `0x35F0C8` changes |
| DirectSound heap allocator | `sub_001C2D6A` (heap object in `edi`, list head at `+0x10`) | Walks a circular free list of `{next, prev, size, tag}` nodes. Tag `'slop'` means used, 0 means free. The main thread spun here when the list was overwritten. The heap pages are the first contiguous blocks (`0x8000B000`, `0x8000C000`...), so the colour-at-physical-0 draws hit them. Fixed by the colour-offset-0 guard (toolkit patch). |
| Colour offset 0 surface | `[SURF] colour 0x00000000 pitch 512 clip 256x256 fmt 0x123 zeta 0x039AF000` | First a depth-only shadow pass (mask 0), then colour-enabled draws with the offset still 0 (mask `0x01010101`, clip 256x256, later 640x480). |
| Texture level copy | `sub_000B3D20` (`this` in `ecx`; source and destination wrappers at `+0x10`, their D3D textures at `+0x10`) | Copies a texture level by level: `GetLevelCount` (`sub_0018E5C0`), `GetLevelDesc` (`sub_0018E5D0`), `CreateTexture` (`sub_0018E510`), `LockRect` (`sub_0018E630`). On a failed create it calls `exit()` (`sub_00182EEB`). This was the "freeze" at skater select and at level load. The cause was not the copy: the contiguous arena was exhausted (nothing was ever freed), so the create failed and `exit()` → `KeBugCheck` returned. Fixed by `RECOMP_HEAP_RECLAIM=1` (now a default) and the bug-check patch. |
| CRT `exit()` / `_doexit` | `sub_00182EEB` / `sub_00182E53` | Ends in `KeBugCheck(0)` |
| D3D push-buffer space wait | `sub_00190730` (via `sub_00190850`/`sub_00190A70`) | Where the main thread spends its time while the software rasteriser is the bottleneck |
| Busy worker thread | `sub_00175520` (thread `sub_00176393`) | Spins about 93% of a core in `KeWaitForMultipleObjects`. Wasteful, but not the bottleneck. Not investigated yet. |
| DirectSound voice array | `VPVADDR = 0x10000` (contiguous, `0x80010000`); SGE table `VPSGEADDR = 0xC000` | 256 voices × `0x80` |
| Disassembler phantoms | `BINKDATA` `0x0037071A`, `0x003707C9` | Data decoded as code; never called |

## Input requirements (from the project owner, 2026-11-10)

- Any XInput-style controller must work, including Steam Input when the game
  is added to Steam as a non-Steam game. The game's own Xbox button prompts
  are fine as they are.
- Mouse and keyboard support is not a goal. Keyboard navigation of menus
  would be nice to have.
- The toolkit already fits this. On Linux it reads pads through SDL2's
  GameController API (`src/input/xinput_device.c`), which handles Steam
  Input's virtual controller. On Windows it reads them through XInput (Steam
  Input presents XInput), and the keyboard comes from the SDL window's
  events, as on Linux. `RECOMP_KEYBOARD` gives keyboard-as-pad for menus.

## Open questions

- **Graphics path.** THUG statically links XDK D3D8 build 5659. There are two
  options:
  - Use the toolkit's pushbuffer executor (`RECOMP_PB_EXEC`). It lets the
    game's own D3D run and replays its NV2A command stream.
  - Redirect the game's D3D entry points to `xbox_d3d8`, the high-level
    D3D8-to-D3D11/GL layer.

  The high-level route is what widescreen and native resolution need, but it
  means first identifying the D3D functions in the `D3D` section.
  XDK D3D also writes pushbuffer commands inline from game code, so the
  high-level route can't see everything. The likely end state is the
  executor with a GPU render back end (`nv2a_backend.h`) at a scaled
  resolution, plus game-side patches for the projection (widescreen) and the
  frame limiter.
- **Frame timing.** Find where the game measures frame time. This decides
  how unlocked FPS has to be done: decoupling the timestep, or interpolating
  extra frames. The PS2 decomp names only about 600 of its 11,000 functions
  so far, so expect to find this from strings and call structure rather than
  from names.
