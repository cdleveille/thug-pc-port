# Bring-up log

Newest first. Each entry records the date, what was run, what happened, and
the next step. Record failures here as well as successes.

## Milestones

- [x] Disc extracted, XBE verified (USA retail)
- [x] Toolkit runtime libraries build on Linux (clang 23)
- [x] Pipeline lifts the whole XBE to C (10,026 functions, 0 failed)
- [x] Generated C compiles and links (clang, about 14 s at -j20)
- [x] Guest entry point runs (CRT startup, XAPI init)
- [x] D3D device created
- [x] First frame presented (pushbuffer executor, CPU rasteriser)
- [x] Window (SDL2 + OpenGL on Linux): frames, plus resolution and FPS in the title bar
- [~] Front-end menus: main menu, story mode and the pre-made skater select
  render, then the game stalls (corrupted DirectSound heap)
- [ ] In-game (skating)
- [x] Audio: plays through PipeWire (confirmed by the owner, 2026-11-10)
- [x] Input: a real XInput pad works through SDL2 (owner-verified: A or
  Start skips the intro)
- [x] Movies (Bink): Activision, Neversoft and intro movies render
- [ ] Windows build
- [ ] Widescreen / native resolution
- [ ] Unlocked frame rate

## Log

### 2026-11-10: menu load, skater select, DirectSound heap

1. **Owner run: real pad works** (A or Start skips the intro). Then it froze
   and **segfaulted in `mcpx_apu_vp_frame`**, writing past `g_dbg.vp.v[]` for
   an out-of-range voice handle.
   Fix: toolkit patch and a new ASan test.
2. **The menu-load hang is an audio stream that never buffers.**
   `sub_00033940` treats an immediate `ReadFile` success on an async handle
   as end-of-stream.
   Fix: `RECOMP_ASYNC_IO=1`, an existing toolkit switch, now a default. The
   game then reaches the main menu, story mode and the **pre-made skater
   select screen**: 3D bedroom and skater, textured, with button prompts.
3. **Voice-list garbage handles.** These came from contiguous pages
   colliding with the image's physical range (the stream buffer is a .data
   static).
   Fix: `RECOMP_CONTIG_AVOID_IMAGE=1`, now a default. Zero bad handles in 3
   of 3 runs.
4. **Current blocker:** the main thread spins in DirectSound's heap
   allocator (`sub_001C2D6A`) because its free list is corrupted. A free
   node gets overwritten with PCM-like data. The stall lands at 256 or about
   560 flips depending on timing.
   - Not fixed by `RECOMP_GUEST_LOCK=1` (tried).
   - gdb (`THUG_DEBUG_ATTACH=1`) gives exact guest call chains.
   - A gdb hardware watchpoint on the node didn't fire under gdb, because
     the corruption moves between runs.

Next steps:
- Catch the writer: a gdb hardware watchpoint on whichever node is corrupt
  in a deterministic run.
- Or audit which host threads run guest code: the DirectSound ISR/DPC
  (vector 6, `0x001C74C6`) and the WMA decoder.

### 2026-11-10: window, controller, loading screen

1. **The windowed run segfaulted after a moment of black.** SDL and the GL
   driver had mapped memory where the contiguous window goes.
   Fix: toolkit patch (always reserve 4 GB on POSIX).
2. **The window stayed black but audio played.** The SDL presenter dropped
   the 1280×480 2×-AA surface.
   Fix: toolkit patch and a new test. The owner confirmed the picture.
3. **Title bar:** now shows the rendering resolution before the FPS.
4. **No input: `0x002007DC` was never lifted.** It is XAPI's USB
   host-controller init.
   Fix: seed it in `config/seed_functions.json`.
5. **Still no USB.** Nothing called `xbox_OhciInit()`.
   Fix: call it in `main.c`, with `RECOMP_USB=1` as a default.
6. **The pad enumerates and is polled.** `RECOMP_PAD_PRESS=0x10` skips the
   intro. The game loads `mainmenu_bg`, shows its "Loading" screen, loads
   `cas_bedroom*`, `skaterparts.prx` and `Intro_01.cut.Xbx`, then stops
   flipping at about 261 flips.
7. **Same hang with `RECOMP_DISPATCH_GATE=1`** (now a default, for the USB
   ISR's `cli`/`sti`). The main thread spins in `sub_00106EE0`, waiting on
   `[this+0xB4C]`.

Next steps:
- Find what should set `[this+0xB4C]`. Run `RECOMP_WAIT_LOG=1`, then
  `RECOMP_WATCH=<this+0xB4C>` once `this` is known.
- Verify a real pad through SDL (windowed).

### 2026-11-10: first boot to the intro movies

Each run is headless (`RECOMP_HEADLESS=1`) with `RECOMP_WATCHDOG_SECS`.
Frames are captured with `RECOMP_FB_DUMP`.

1. **Exited to the dashboard.** The XAPI media check failed, because the disc
   security sector is RSA-signed and there is no drive.
   Fix: override `sub_00178B85`.
2. **Hung in DirectSound.** The title waited on audio hardware.
   Fix: `RECOMP_AC97_READY=1` (the emulated APU).
3. **SIGSEGV in DirectSound.** The POSIX MMIO decoder doesn't know
   `test [mem], imm32`.
   Fix: toolkit patch (ALU and TEST forms) and a new decoder test.
4. **Hung in the DirectSound AC'97 channel reset.** The bus-master write trap
   was never armed on x86-64 Linux.
   Fix: toolkit patch.
5. **Loaded assets and opened `atvi.bik`, then hung in D3D** waiting for the
   GPU to consume the pushbuffer.
   Fix: `RECOMP_PB_EXEC=1` plus `xbox_Nv2aMirrorFence(0x0019C358, 0x2C, 0x30)`.
6. **About 29 flips/s, movies decode, frames all black.** Bink's movie quad
   has `w = +inf` and was dropped.
   Fix: toolkit patch (clamp `+inf` like `+0`).
7. **Activision, Neversoft and intro movies render correctly.**

`RECOMP_AC97_READY`, `RECOMP_PB_EXEC` and `RECOMP_VBLANK=paced` are now
defaults in `src/main.c`.

Next steps:
- See what follows the intro.
- Bring up input: `RECOMP_USB=1` for pads, and the keyboard-as-pad mapping.
- Open a window (`RECOMP_FB_WINDOW=1`).
- Check audio output.

### 2026-11-10: project set up

- Extracted the Redump image with `tools.xiso`: 5,797 files, 3.2 GB.
- `tools.xbe_parser`: XDK 5659 and 30 sections. See `docs/NOTES.md`.
- `tools.rtti`: no RTTI in the image.
- Toolkit v0.17.0 runtime libraries build cleanly on Linux with clang.
