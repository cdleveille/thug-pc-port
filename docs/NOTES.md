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
  submodule at `third_party/xboxrecomp`, with the local patches below.
- **2026-11-10: Bring-up uses the pushbuffer executor** (`RECOMP_PB_EXEC`).
  The game's own XDK D3D runs unmodified, and the executor consumes its NV2A
  command stream. See "Open questions" for the long-term renderer.

## Toolkit patches

These are general fixes, not THUG-specific ones. They are kept in
`patches/xboxrecomp/` and applied by `scripts/apply-patches.sh`. They should
go upstream.

| Patch | Fix | Found by |
|---|---|---|
| 0001 | `mmio_decode_x86_64.h`: the POSIX trapped-MMIO decoder now handles the ALU, CMP and TEST forms the Win32 decoder already had. clang at `-O2` folds a lifted `MEM32(r) & imm` into `test dword [mem], imm32`. New test: `tests/mmio_decode_x86_64`. | DirectSound init segfaulted on `F7 84 03 …` against APU register `0xFE802000` |
| 0001 | `xbox_memory_layout.c`: the AC'97 bus-master write trap is now armed on x86-64 Linux, not only arm64. Before, a channel reset spun forever. | DirectSound hung in `sub_001C7138` waiting for the RR bit of `0xFEC0011B` to clear |
| 0001 | `nv2a_pb_exec.c`: a vertex program output of `w = +inf` is clamped to 1, like `+0`. The hardware and xemu both do this. | Every Bink movie quad (RHW 0, so `rcp` gives `+inf`) drew 0 pixels |
| 0001 | `xbox_memory_layout.c`: POSIX now always reserves the whole 4 GB guest window, as Windows does. It used to do this only under `RECOMP_EXT_VMA`. | With `RECOMP_FB_WINDOW=1`, SDL and the GL driver mapped memory where the contiguous window (guest `0x80000000`) belongs. The title segfaulted at guest `0x81000000`. |
| 0001 | `fb_present_sdl.c`: the SDL window now presents 2×-wide anti-aliased 32-bit surfaces (pitch 640 × 8), as the GDI presenter already did. It takes every other pixel and steps rows by the vertical AA factor. New test: `tests/fb_present_sdl`. | The window stayed black while frame dumps were correct, because THUG presents a 1280×480 surface and the presenter dropped it |
| 0001 | `nv2a_pb_exec.c`, `fb_present*.c`, `video_player.h`: the title bar shows the presented frame's rendering resolution, `<title> \| 640x480 \| FPS: n \| draws: n`. Added `xbox_FramebufferWindowSetResolution`, set at each flip from the drawn surface's unscaled clip size. Test stubs updated. | Owner request |
| 0001 | `apu_vp.c`: the voice-list walk ends at a handle of 256 or more, which only `0xFFFF` may exceed. Before, an `assert` compiled out in release builds let it index `g_dbg`, `voice_spin` and the resampler state out of bounds. New test: `tests/apu_voice_list` (ASan). | The VP thread segfaulted in `mcpx_apu_vp_frame` during the menu load (owner's windowed run) |

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
| Load-wait loop | `sub_00106EE0` (loop `0x00106F50`–`0x00106F6B`) | Spins, waiting a vblank each pass (`sub_00174090`), until `sub_00031C40` says the streams are buffered. Fixed by `RECOMP_ASYNC_IO` (see the stream start below). |
| Stream objects | `[0x24E7DC]` (checked by `sub_000358C0`), `[0x24E7E0]` (checked by `sub_00033A70`) | Ready means state `+0xA4 == 1` and either done `+0x0D` or buffered `+0x9C >= +0xA0 + +0x4C`. File handle at `+0x00`, OVERLAPPED at `+0x08`, read buffer at `+0x98` (`0x0024E7F0`, a static in .data). |
| Stream start | `sub_00033940` | `ReadFile(h, buf, 0x4000, NULL, ov)`. An immediate TRUE sets done without counting the bytes. It relies on async handles always returning pending, as the hardware does. |
| Stream continue | `sub_00033830` | `GetOverlappedResult` (`sub_00174E0C`); error 996 means not ready yet; adds the bytes to `+0x9C`, then the next `ReadFile`. |
| XAPI ReadFile / GetLastError | `sub_001748D8` / `sub_00174D9F` | Without an OVERLAPPED, a pending result waits on the file handle. |
| vblank wait | `sub_00174090` | Spins until the 64-bit counter at `0x35F0C8` changes |
| DirectSound heap allocator (current blocker) | `sub_001C2D6A` (heap object in `edi`, list head at `+0x10`) | Walks a circular free list of `{next, prev, size, tag}` nodes. Tag `'slop'` means used, 0 means free. The main thread spins here when the list is corrupt. Seen with a free node at `0x8003C03C` overwritten with `0x08000800`, then `0x08200820` (PCM-like). The write wasn't attributed to guest code at a trapped page, so the writer may be guest code on a host thread (an ISR or DPC), which `RECOMP_WATCH` can't attribute. Stalls at 256 or about 560 flips depending on timing. |
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
  Input's virtual controller. On Windows it reads them through XInput.
  `RECOMP_KEYBOARD` gives keyboard-as-pad for menus.

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
