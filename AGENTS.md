# AGENTS.md: THUG PC Port

Read this first, every session. Then read `docs/NOTES.md` (title facts and
decision log) and `docs/BRINGUP.md` (where bring-up stands).

## What this project is

A native Windows and Linux x86-64 build of *Tony Hawk's Underground* (Xbox,
NTSC-U, 2003), made by **static recompilation**: the game's own x86 is
translated to C by the [xboxrecomp](third_party/xboxrecomp) toolkit and
compiled natively. There is no emulation and no interpreter.

Goals, in order:
1. Boot to menus, then gameplay, on Linux (OpenGL) and Windows (D3D11).
2. Native resolution and widescreen (Hor+).
3. Unlocked frame rate, with game logic that does not speed up.

Not goals: a decompilation, rewriting game logic, or fake screens. If the game
shows something, the game's own code put it there.

## Hard rules

1. **Never commit game content.** That covers `rom/`, `game/`, any `.iso`,
   `.xbe` or `.bik`, and extracted assets. The user supplies their own disc.
2. **Never commit or hand-edit `src/recomp/gen/`.** It is lifted from the
   game's executable (a derivative of the game) and `scripts/regen.sh`
   overwrites it.
3. **Fixes go in exactly these places**, in this order of preference:
   - `config/seed_functions.json`: function entry points the detector missed.
   - `src/recomp_manual.c`: hand-written replacements for single lifted
     functions. Replace, don't wrap. Log each one in `docs/OVERRIDES.md`
     with the guest address, the symptom and the reason.
   - `third_party/xboxrecomp`: general bugs in the lifter, kernel shim or
     graphics translation. It is a git submodule, so a change there is
     invisible to anyone else until it's exported. After changing it:
     1. Regenerate `patches/xboxrecomp/*.patch`. Check it with
        `git -C third_party/xboxrecomp apply --check -R <patch>`.
     2. List the change in `docs/NOTES.md` ("Toolkit patches").
     3. Add or extend a test under the toolkit's `tests/`.
     4. Offer general fixes upstream.
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
| `src/recomp_manual.c` | Manual overrides, ICALL failure logging |
| `src/recomp/gen/` | Generated C (gitignored) |
| `config/` | Title-specific pipeline inputs (seed functions, etc.) |
| `scripts/extract.sh` | Unpack `rom/*.iso` into `game/` and verify the XBE hash |
| `scripts/regen.sh` | Run the lifting pipeline: parse, disasm, func_id, abi, recomp |
| `scripts/apply-patches.sh` | Apply `patches/xboxrecomp/*.patch` to the submodule (idempotent) |
| `patches/xboxrecomp/` | Local toolkit fixes not yet upstream |
| `docs/` | Title notes, bring-up log, override log |
| `third_party/xboxrecomp` | The toolkit (submodule) |
| `rom/`, `game/` | The user's disc image and extracted files (gitignored) |

## Workflow

```
scripts/apply-patches.sh           # once per checkout: local toolkit fixes
scripts/extract.sh                 # once: rom/*.iso -> game/
scripts/regen.sh                   # lift game/default.xbe -> src/recomp/gen/
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
build/bin/thug 2> stderr.txt       # run from the repo root; reads game/
```

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
every 2 s and dumps frames. The toolkit's own guides are authoritative:
`third_party/xboxrecomp/docs/INDEX.md`, especially its "By Symptom" table,
`docs/pipeline/06-debugging.md` and
`docs/runtime/environment-variables.md` (every `RECOMP_*` switch).

## Environment

- Linux x86-64 is the primary development host: clang or GCC, CMake 3.20+,
  Ninja, SDL2, libepoxy, Python 3.10+ with `capstone`.
- Windows builds use the toolkit's D3D11 backend (MSVC 2022 or MinGW-w64).
  They are not set up yet.
- The toolkit's OpenGL path is less complete than its D3D11 path. A rendering
  bug on Linux may be a toolkit gap rather than a title bug.

## Title-specific knowledge

Keep `docs/NOTES.md` current with anything learned about the binary:
identified functions, globals, engine subsystems, and what the PS2 decomp
([andrewnakas/thug-decomp](https://github.com/andrewnakas/thug-decomp),
SLUS-20731) says about the same code. The PS2 and Xbox builds share the
Neversoft codebase, so names and struct layouts from the decomp often carry
over. Match them through shared strings and call structure, never by address.
