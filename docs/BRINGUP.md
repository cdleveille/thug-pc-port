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
- [x] Front-end menus: main menu, story mode, free skate, skater select and
  level select
- [x] In-game (skating): free skate New Jersey loads and runs
- [x] GPU renderer (OpenGL 3.3, `src/gpu/nv2a_pb_gl.c`): direct present,
  internal resolution scaling
- [x] Audio: plays through PipeWire (confirmed by the owner, 2026-11-10)
- [x] Input: a real XInput pad works through SDL2 (owner-verified: A or
  Start skips the intro)
- [x] Movies (Bink): Activision, Neversoft and intro movies render
- [x] Windows build (MSYS2 CLANG64; scripted runs into free skate; the
  owner's playtest is still to come)
- [x] Widescreen (native 16:9, Hor+) and native resolution
- [x] Unlocked frame rate with real-time game logic
- [ ] Shadows (depth textures)
- [ ] 60 fps or better in the heaviest views (vertex programs still run on
  the CPU)

## Log

### 2026-11-11: WASD is the left stick

- Owner request: W A S D drive the left stick, the arrow keys stay the
  d-pad. `src/host/controls.c` binds them to the toolkit's
  `INPUT_LSTICK_*` controls in both layers (skating and menus), with
  `INPUT_DIAGONAL_ROUND`: W+A puts the stick on its circle (-23169, 23169),
  as a real stick goes, not full on both axes.
- Checked: a scratch probe through the toolkit's keyboard layer, both
  layers: W/S/A/D full deflection on one axis with no d-pad bit, W+A on the
  circle, Up and Left the d-pad with the stick at rest. A scripted run showed
  the main menu follows the left stick (down, down, up: Story Mode to High
  Score / Free Skate), so WASD still moves through the menus. Skating with
  the keys needs the owner's hands.

### 2026-11-11: the Windows build

- **Set up** on the owner's Windows 11 machine (RTX 4080, a scaled 4K
  desktop): MSYS2 from winget, then the CLANG64 packages (`docs/WINDOWS.md`; the
  capstone package exists, `mingw-w64-clang-x86_64-python-capstone`).
  `thug-extract` unpacked the image in 7 s and verified `default.xbe`;
  `scripts/regen.sh` lifted it in 3 m 42 s, 10,026 functions, 0 failed, 41
  files, as on Linux. The toolkit's runtime libraries compiled under MinGW
  clang as they were.
- **Ported** the Linux path, file by file from the handoff's table (the
  "What was done" table in `docs/WINDOWS.md` has each): SDL2, epoxy and ImGui
  on Windows; the renderer's stub removed; the window loop, the Linux boot
  path in `main.c`; the log, captures and progress window; restart through
  `CreateProcessW`; the keyboard from SDL events with pads on XInput
  (toolkit); a precise frame-cap sleep; DPI awareness; `-mwindows`; the
  toolchain DLLs copied next to `thug.exe`. Defaults: `RECOMP_WIN_PLACEHOLDER`
  (the 4 GB guest window held before the GL driver loads). The first full
  build linked at once (26 s at 20 threads).
- **Found on the way:** `nv2a_pb_exec.c` declared its DXT cache thread-local
  with `__declspec(thread)` on `_WIN32`, which MinGW ignores (one warning
  among many), so the CPU rasteriser's workers would have shared it; and
  running the toolkit's suites showed 41 of them failing to link
  (`clock_gettime64`) and one Windows-only diagnostic slip in the raster pool.
  All three fixed in the toolkit (NOTES "Toolkit changes").
- **Checked** (scratch `APPDATA`/`LOCALAPPDATA`, muted visible runs):
  - Headless with Start pulsed: intro movies, main menu, story options, the
    pre-made skater select room, all drawn as on Linux (frame dumps).
  - Visible: a 2560x1440 window on the 4K desktop (real pixels), 1080p
    internal at 60.0 fps with the cap, the icon in the title bar, the story
    intro cutscene (the Muska flyer textured); the owner's XInput pad found on
    port 1; Escape's layers switching; smooth text and icons magnifying on its
    thread; the F1 overlay (window capture); WM_CLOSE logging "window closed"
    and writing `%APPDATA%\thug-pc-port\settings.ini` with the window's size
    and position.
  - First launch from a folder holding only the image (a hard link,
    `THUG_GAME_DIR`): the progress window, the unpack, the check, then the
    intro. The progress window was 520x90 real pixels, tiny on the 4K
    desktop with its title cut off; it now scales by the desktop's DPI
    (checked: the whole title and percentage show).
  - Restart: a scratch probe of `restart.c` restarted with the same arguments
    (one with a space) and the starting environment, not a variable added
    after start-up.
  - Free skate: `RECOMP_PAD_LIVE` through the menus into New Jersey, then
    skating (a skitch for 750 points). Headless at 1080p, uncapped: 63-66 fps
    at about 1,000 draws, 40 at 2,100 (Linux: 83-84 at 1,000). The
    executor's walk is 184 ns per index (Linux 155-170); GL submission is
    faster than on Linux. Recorded in `docs/WINDOWS.md`, not investigated.
  - The toolkit's suites, each in a fresh folder: 70 pass (all 11
    Windows-only ones, and `kernel_open_directory`, which fails on Linux), 14
    POSIX-only, `d3d8_smoke` does not build (D3D11 layer, not THUG's path).
    New: `tests/input_xinput_empty_ports`; `tests/input_keyboard_sdl` now runs
    on Windows. The port's own test passes.
- **Not checked:** anything by hand (pad feel, rumble, hotplug, keys,
  fullscreen, F10), sound by ear (XAudio2 starts; every run was muted), a
  crash's message box, gdb on Windows. The owner checklist is in
  `docs/WINDOWS.md`.
- **`scripts\build.bat`** (owner request: build without switching shells):
  runs `build.sh` in CLANG64 from cmd or PowerShell; `--setup` installs
  MSYS2 and the packages. Checked from PowerShell and cmd: help, an
  up-to-date build, a rebuild after a change, `--release` passed on, the
  error for a missing MSYS2, `--setup` on the installed toolchain. Two bugs
  found by those runs: `shift` moves `%0`, so with any argument the script
  looked for `build.sh` in the current folder (the folder is now taken
  first); and see the next point.
- **MSYS2 replaced SDL2 with sdl2-compat.** `--setup`'s `pacman -Syu`
  removed SDL2 2.32.10 for sdl2-compat 2.32.74 over SDL3 (marked as
  replacing it), and the package list's `SDL2` swapped it back. The list now
  names `sdl2-compat`, and the game was checked on it, which showed:
  - The window was DPI-unaware (the window's awareness 0, 1707x960 logical,
    stretched by Windows): sdl2-compat does not pass SDL2's DPI hint on.
    Fixed by declaring per-monitor awareness through the Windows API before
    SDL starts; now awareness 2 at 168 dpi (the desktop is at 175%), a
    2560x1440 window, the frame filling it (a capture of the screen, not
    only `PrintWindow`, which shows a virtualised window unscaled).
  - The DLL copy kept the old `SDL2.dll` (the resolver looks beside the
    executable first) and never copied `SDL3.dll`, which sdl2-compat loads
    with `LoadLibrary`. Fixed in `scripts/copy_runtime_dlls.cmake`.
  - Unchanged: the overlay, the progress window, the pad, keyboard layers,
    60 fps; `input_keyboard_sdl`, `input_xinput_empty_ports`,
    `input_xinput_imports` and the port's test pass.

### 2026-11-11: handoff to Windows

- The owner moves to Windows 11 to bring up a Windows build; the next
  session starts there, without this machine's context.
- `docs/WINDOWS.md` has the plan: the same SDL2 + OpenGL renderer under
  MinGW-w64 (MSYS2 CLANG64), a file-by-file list of what is Linux-only today,
  the order of work, and an owner test checklist. It also has the owner's
  context.
- The state at handoff:
  - The game builds, and the port's test (`ctest --test-dir build`) passes.
  - Every toolkit suite under `vendor/xboxrecomp/tests` was configured,
    built and run on Linux, each in a fresh folder. 71 pass.
  - 13 suites are Windows-only (they stop at configure or need
    `windows.h`), or register no tests on Linux (`cmake_d3d8_consumer`,
    `d3d8_smoke`). `audio_mixer` has no `CMakeLists.txt`.
  - One fails: `kernel_open_directory` ("non-directory open of a directory
    succeeded"). It fails at the last commit too, so it predates the day's
    work. THUG plays regardless; not investigated.
- The day's work (glass shards, rumble, smooth text) is in commit `afb2cec`;
  this entry, `docs/WINDOWS.md` and the pointers to it in AGENTS.md and the
  README came after it.

### 2026-11-11: smooth text and icons (xBR)

- Owner report: on-screen text is readable but looks low-res, with noisy,
  stepped edges.
- Cause: the game's text, icons and logos are bitmaps drawn 1:1 for
  640x480 (about 11 pixels a glyph), and at 1080p the renderer magnifies them
  3x across and 2.25x down with bilinear filtering (what the game asks for).
  The trick font's grey speckle is its own design.
- Compared offline on the dumped fonts (`THUG_GL_TEXDUMP`): bilinear,
  Catmull-Rom, an alpha re-cut, and xBR, hq2x, Scale2x and Eagle. xBR and
  hq2x draw the outlines as smooth curves; Catmull-Rom only sharpens a little.
- Built (NOTES "Decisions"): `src/gpu/xbr.c`, xBR 2x on RGBA (twice for 4x;
  about 18 ms for a 256x256 atlas at 4x, so on a worker thread), and the
  renderer swaps it in for the stage-0 texture of 2D overlay draws whose alpha
  has cut-outs. Overlay: Display, "Smooth text and icons", on by default;
  `THUG_UI_UPSCALE`, settings key `smooth_text_and_icons`.
- Checked:
  - `tests/xbr_test.c` (`ctest --test-dir build`): flat areas and straight
    edges come out as plain pixel doubling, an alpha-only staircase is found
    and blended, a shallow edge ramps along its rows. It fails with alpha left
    out of the distance, and with the wrong neighbour subpixel blended.
  - Headless at 1080p, menus and free skate: 24 textures magnified (menu and
    trick fonts, button icons, HUD art), about 30 MB, no repeat requests; menu
    and trick text visibly smoother, frame rate as before.
  - A temporary switch flipped the setting off and on mid-run: the game's own
    textures came back, then the smooth ones.
  - The overlay shows the checkbox (screenshot).
- Seen on the way: a swizzled texture can occupy two cache entries (the key
  includes a pitch register it only inherits), so it is uploaded, and
  magnified, twice. Harmless; noted in AGENTS.md.

### 2026-11-11: grind rumble stops after a second

- Owner report: grinding a rail starts the vibration, which stops after one
  or two seconds although the grind goes on.
- `RECOMP_USB_TRACE=1` in a headless run (skating, jumping, grinding) shows
  how THUG drives the motors: one rumble report when an effect starts (left
  motor 32639, 52223 or 65279) and one with 0 when it ends, nothing in
  between. An Xbox pad keeps the last speeds it was sent, but the toolkit gave
  each request to SDL with a 1000 ms duration, assuming the title re-sends
  every frame.
- Fix (toolkit, NOTES "Toolkit changes"): the speeds are held until the
  title sends others (SDL's longest duration, re-armed from the input poll
  every 30 s), and the overlay's focus or a strength change re-sends them.
  `tests/input_sdl_hotplug` covers it.
- Owner to confirm with a pad: the rumble should last as long as the grind.
- Glass shards (previous entries): the owner finds them better but still a
  little floaty; their fall is the game's own (128 units/s², a third of real
  gravity). Left as is for now; the owner may revisit.

### 2026-11-11: glass shards stuck to the screen (inline batches never transformed)

- Owner report: in Manhattan, glass broken by the skater leaves shards that
  look "floaty" and stay on screen too long.
- Timing first: the shatter is the game's script `ShatterAndDie` and the
  engine's `Shatter` (NOTES "Title-side facts"). Shard life (4 s), gravity
  (128 units a second squared, about a third of real gravity) and launch
  speeds are the game's own, and the update runs on `FrameLength()`
  (measured with gdb: a 1/60 s step at the 60 fps cap, as on the console).
  The motion was never the problem.
- The owner's F10 captures (shards in flight, then an unbroken pane): the
  pane draws with texture `0x833EE000` (64×64 DXT, amber, vertex alpha
  `0x66`) through the fixed-function transform. The shards draw with the
  same texture and alpha, 210 vertices (70 shards), but their first vertex
  was at (976, 198) with depth −5782 and 1/w = 1, and texture coordinates
  (s, t, 1, 0). Those are world coordinates drawn as pixels.
- Cause: the shards are a `DrawVerticesUP` (`sub_0018B970`), so the vertices
  arrive in the pushbuffer as an `INLINE_ARRAY`, and the executor sent every
  inline batch to its screen-space path regardless of the transform mode. The
  shards were pinned to the screen, ignored the camera, rose as they fell
  (screen y points down) and, with q = 0, sampled the texture as white.
- Checked before changing it: a survey of a boot into free skate counted
  about 380,000 inline batches and one immediate (`SET_VERTEX4F`) batch a
  frame, all in vertex-program mode (HUD, text). Fixed-function inline
  batches are only the shards.
- Fix (toolkit, NOTES "Toolkit changes"): in fixed-function mode an inline
  batch is transformed like any other. New case in `tests/nv2a_ffp_backend`
  (fails with the old rule: the triangle lands at its raw coordinates with
  q ≠ 1). The executor tests (`nv2a_ffp_backend`, `nv2a_pb_constants`,
  `nv2a_raster_batch`, `nv2a_tex_filter`) pass, and a headless run into free
  skate draws the HUD, text and level as before.
- Not checked in the game: breaking glass headless needs a route to a pane.
  Owner to confirm the shards now fly from the pane in 3D, amber and
  see-through.

### 2026-11-11: the overlay's pad chord is L3 + R3

- Owner request: both sticks clicked opens and closes the overlay, in place
  of Back + Start. B still closes it.
- The overlay now tracks every controller button held (any pad). Closing
  leaves the chord (or B) held, and the game's pad used to be handed back at
  once, so the game read those buttons as a fresh press (with Back + Start,
  likely the pause menu); now it stays blocked until every button is let go.
  A pad pulled out meanwhile clears the record.
- Checked with synthetic controller events through the overlay's handler
  (a temporary hook): L3 then R3 opens and blocks; R3 then L3 closes, the
  pad comes back only when the last of the two is released; B with the
  overlay closed reaches the game. The first button of the chord still
  reaches the game before the second is pressed.

### 2026-11-11: the disc image can sit beside the executable

- Owner request: a player can drop the .iso next to `thug`; the first launch
  creates `game/` and unpacks into it.
- `thug_game_dir_locate` gives `<exe dir>/game` the image beside the
  executable when that folder holds none or does not exist yet;
  `thug_game_prepare` creates the folder before checking free space. Ready
  files in `game/` still win, and an interrupted unpack resumes from the image
  beside the executable. `thug-extract` shares the code.
- Checked in a scratch install (`build/` copy of `thug` and a link to the
  image, launched from elsewhere): first launch created `game/`, unpacked
  3.4 GB, verified `default.xbe` and booted; the second launch used `game/`
  directly; with `default.xbe` deleted the unpack ran again from the image
  beside the executable; with nothing there (a copy outside the repository)
  the message names both places.

### 2026-11-11: "Restart to apply changes"

- Owner request: a button in the overlay, shown only while a change waits
  for a restart (today: switching between 4:3 and a wide aspect, whose menu
  and HUD layout the game picks at boot). It asks first: progress since the
  last save is lost.
- `src/host/restart.c`: the main loop saves the window and settings, then
  the process execs itself. Same PID, so a launcher that watches it (Steam)
  sees one game. Run with the arguments and environment it was started with
  (start-up exports the settings as THUG_* variables, which would override
  the file just saved); every descriptor above stderr closed at the exec
  (`close_range` with CLOEXEC), so the old window's display connection
  closes with it; stdout and stderr handed back from the log
  (`thug_launch_restore_stdio`), so the new run rotates the log as a fresh
  start does. The executable's path is resolved at start-up and exec'd:
  exec'ing `/proc/self/exe` renamed the process "exe".
- Checked with a temporary trigger (since removed) in scratch config and
  state folders: same PID and name after, 80 descriptors before and after,
  `thug.prev.log` ending in "restarting", the new run at the new aspect, the
  settings file written. The button and its confirmation by screenshot. Not
  on Windows yet (`thug_restart_supported` is 0, the button stays hidden).

### 2026-11-11: aspect ratios 4:3, 16:10 and 16:9

- Owner request: resolutions in 4:3 and 16:10 as well as 16:9, with the
  field of view following the aspect.
- How the game does it (NOTES "Title-side facts"): the script command
  `SetScreen` sets the screen aspect and an angle, from which the engine
  keeps a field-of-view ratio; the scripts know standard (4:3, 72°) and
  widescreen (16:9, 88.18°, which is Hor+). Found through the script C
  function table and the scripts themselves (`camera.qb`, unpacked from
  `qb.prx` into a scratch folder).
- The setting `aspect_ratio` (`THUG_ASPECT`) replaces `widescreen`; old files
  and `THUG_WIDESCREEN` are read as before. Wider than 4:3 turns on the
  dashboard flag (the game's widescreen mode: HUD and menu layout); the
  renderer renders and presents the chosen aspect; the aspect and ratio
  getters are overridden to give the game the chosen aspect with the ratio
  scaled to match. At 4:3 they pass the game's values through.
- Checked: three scripted runs into Manhattan (16:9, 16:10, 4:3). The 16:10
  frame's centre 4:3 matches the 4:3 frame (static scenery lines up
  exactly in a blend; squeezing the whole frame ghosts), so 16:10 is Hor+.
  A live switch 16:9 → 16:10 mid-level: the cameras read the ratio
  200-480 times a second, so the view follows within a frame. Old and new
  settings lines and variables resolve as expected.
- Not changed: in widescreen mode the game stretches its 2D menus and HUD
  by the anamorphic factor (as on the console at 16:9); at 16:10 the stretch
  is 1.2 instead of 1.33. Switching between 4:3 and a wide aspect changes
  that layout only at the next start (the overlay says so).

### 2026-11-11: a line and a jump at the left edge in menu transitions

- Owner report: after Story Mode and back, a thin bright line at the
  image's left edge for a second or two, and the picture shifts a little.
  (The black bars either side are the pillarbox: a maximized window on the
  owner's 4K desktop is 3840x2041, wider than 16:9.)
- The main menu's screen changes use the reset transition: the scene is
  drawn into a 640x480 surface and composited onto the 2x-wide buffer. A
  capture (`THUG_GL_CAPTURE_ON_X8=1`) showed the scene surface's first
  column never drawn and the second half drawn, at internal scale 2.25: the
  title's sprites start at x = 0.53125, the XDK's screen-space offset,
  while the 2x-wide buffer's start at 0.0625. The undrawn column keeps
  whatever the surface held (dark in my run, bright in the owner's), and
  the whole image sits half a 640-pixel (3 output pixels) to the right of
  the normal path's.
- Fix: the renderer subtracts the offset for the surface's anti-aliasing
  mode (`screen_offset`, NOTES "Title-side facts"). Checked with window
  screenshots: the transition's left edge now matches a normal frame column
  for column (before: a 4-5 pixel ramp and the shift); the first column of
  every frame no longer carries one MSAA sample of clear colour; play and
  the pause menu look the same as before. `THUG_GL_RAW_SCREEN_OFFSET=1`
  restores the old mapping.

### 2026-11-11: plugging in a controller crashed the game

- Owner report, and two crashes in scripted runs: `malloc(): unaligned
  tcache chunk detected` (abort) and a SIGSEGV in `malloc`, both on the
  emulated USB thread: `ohci_run_periodic_list` → `usb_gamepad_report` →
  `xbox_InputGetState` → `refresh_controllers_locked` →
  `SDL_GameControllerOpen` (`xinput_device.c:268`) → sdl2-compat → SDL3.
- Cause: every poll of a port without a pad (ports 2–4 always) re-enumerated
  SDL's devices by index from the polling thread, while the window thread
  pumps SDL events, which is where SDL handles a hotplug. The joystick lock
  did not make the index-based calls safe on sdl2-compat over SDL3.
- Fix (toolkit, tested): with the main pump on, the pumping thread alone
  opens and closes pads, at start and on SDL's controller device events; the
  title's threads only read the pads already open. Each connect and
  disconnect is logged as `[INPUT] port N: ...`. A start-up run opened the
  owner's Xbox 360 pad on port 1. Plugging in and out mid-game needs the
  owner's hands.

### 2026-11-11: Escape pauses, and goes back in the menus

- Owner request: Escape is START while skating (it pauses), and B in the
  menus (it goes back; in the pause menu B is "get back in the game").
- `src/host/controls.c` installs two toolkit binding layers (Escape on START,
  and Escape on B with Backspace and Tab) and, on each pass of the window
  loop, puts in force the one the game's state calls for, only while Escape
  is up: otherwise the press that pauses would reach the pause menu as a B
  and resume at once. The toolkit gained the layers
  (`xbox_InputKeyboardSetLayer`, `xbox_InputKeyboardUseLayer`; tested),
  since swapping bindings with `SetBindings` was not safe while the title
  polls.
- First version: the byte that a memory diff showed equal in the pause and
  front-end menus and different while skating (`0x0024C3BC`). The owner
  found Escape still START in the main menu: Options' first item, MTX
  TEASER, played when Escape was pressed there. The byte turned out to be
  the music's pause flag (`PauseMusic`), which the front end clears when its
  music starts.
- Second version, from the game's own script functions (named through the
  C-function table, NOTES "Title-side facts"): B when the current level is
  `load_skateshop` (the main menu and every screen under it) or
  `GameIsPaused`; START otherwise, including the intro movies. `InFrontEnd`
  was no use: THUG's main menu runs in the current game mode, whose
  `is_frontend` is 0.
- Checked in visible scripted runs against frame dumps: B from the first
  main menu, through the load prompt, Options (B returns to the main menu),
  Create-A-Skater and System Link; START from the level load and while
  skating; B for each pause, START on resume; B through Quit, its prompt and
  the reload of the main menu. Pressing Escape itself still needs the owner
  (no key injector on this Wayland host).
- Keyed to the USA retail XBE like every address here.

### 2026-11-11: keyboard controls for the menus

- Owner request: Enter/Space A, Backspace/Tab B, Escape START, WASD and the
  arrows the d-pad. The toolkit's keyboard layer already fed port 0 (merged
  over a pad, or alone with none), with a fixed map; it gained
  `xbox_InputKeyboardSetBindings` (toolkit, tested) and
  `src/host/controls.c` installs THUG's bindings. `RECOMP_KEYBOARD=1` is
  now a default.
- Gotchas handled: Alt+Enter (fullscreen) and Alt+Tab (window switching) do
  not reach the game as A and B; opening the F1 overlay releases every held
  key, since the overlay takes the key-ups; Escape closing the overlay never
  reaches the game (the overlay consumes it). Keys are read only with the
  window focused, and losing the focus releases them (toolkit).
- Not tested end to end: no key injector on this Wayland host. The binding
  and SDL-event layers are covered by the toolkit's tests; the owner checks
  the menus.

### 2026-11-11: the window reopens as it was left

- Owner request. The settings file gains `window_width`, `window_height`,
  `window_x`, `window_y` and `window_maximized`: `host_window.c` records the
  plain window's size (not while maximised or fullscreen, so un-maximising
  goes back to it), the maximised state and, except on Wayland (no client
  may read or set its position), the position, on every size, move,
  maximise and restore event, and saves them when the window is closed. The
  renderer opens the window from them, clamped to today's desktop, the
  position only if it still lands on a display; `[PBGL] window ...` says
  what it did. Checked on KDE Wayland with a scratch config: 1000x600
  reopens at 1000x600, and maximised reopens maximised. The save on close
  is for the owner to confirm (a script cannot press the close button).

### 2026-11-11: window icon

- The owner's `thug.png` (512x512 RGBA) is the window icon: CMake runs
  `scripts/embed_icon.py` (standard-library PNG decode, 2x2 box shrink with
  premultiplied alpha, identical to ImageMagick's box resize) into a C array,
  and `src/host/window_icon.c` sets it on the game window and the first-launch
  progress window, with the app ID `thug-pc-port`. On KDE Plasma 6.8 Wayland
  (SDL 3.4 under sdl2-compat) it reaches the title bar through
  xdg-toplevel-icon; checked with a window screenshot. The taskbar is for the
  owner to confirm.

### 2026-11-11: still water, texture matrices and texgen; glitch capture

- **Owner reports:** in Manhattan (story), bailing into the river makes parts
  of the screen repeat in strips for about a second (a sign copied along a
  row, an object smeared); the same in New Jersey's rainy night mission. And
  the river is static: it never moves.
- **First guess, wrong:** the bump-env texture modes (6, 7) added their
  offset after normalising a linear texture's coordinate, which would make
  an offset of a few texels several whole textures. Fixed anyway (the offset
  is now added first, as in xemu's psh.c), and the first distinct bump-env
  set-ups of a run are logged (`[PBGL] bump-env:`), but the owner's next
  session logged none: THUG's water does not use them.
- **The gap:** the executor's fixed-function path ignored texture-coordinate
  generation and texture matrices entirely; every coordinate came straight
  from the vertex streams. THUG uses both (a register census): texture
  matrices on stages 0-2 from the menus on, REFLECTION_MAP texgen on S, T
  and R in levels. In Manhattan two textures (`0x83484000`, `0x83487000`)
  have a matrix whose translation moves every frame, a scrolling texture;
  three reflection maps fold the reflection vector onto 2D with a 0.5
  scale/offset matrix. Implemented after xemu's `vsh-ff.c` (toolkit, see
  NOTES); `RECOMP_FFP_RAW_TEXCOORDS=1` is the A/B switch.
- **Checked:** the toolkit test computes a scale-and-translate matrix, an
  object-linear plane and a reflection vector by hand; menus and levels
  render as before apart from those surfaces. **Not seen yet:** the river
  moving, and whether this was also the splash glitch. The scripted route
  into Manhattan does not reliably reach the water.
- **The splash itself, from the owner's F10 capture:** not a water effect but
  the game's reset transition (also out of bounds). The scene goes into a
  640x480 X8R8G8B8 surface and is laid over the un-cleared screen buffer at
  alpha 0.15 x the surface's alpha: an accumulation trail, each frame keeping
  85% of the last, so a moving camera leaves fading copies. One real fault:
  the renderer sampled the X8 surface's stored alpha instead of 1 (now a GL
  alpha swizzle for alpha-less formats). A self-triggering capture of a
  scripted out-of-bounds reset showed that alpha is 1 almost everywhere
  (mean 0.995) and about 0.6 only under the HUD, so the fix affects the HUD
  and the trail of the scenery is the game's own blend. Whether the original
  looks the same is not confirmed; the owner does not remember.
- **Owner's call: skip it.** The composite is recognised (one full-surface
  quad sampling another render target in an alpha-less format, alpha below
  1, alpha blending, depth ALWAYS without writes) and drawn unblended. In a
  scripted out-of-bounds run it matched once, at the reset, and the screen
  buffer then equals the scene surface (0.3% of pixels differ, from the 2x
  downscale of the comparison). `THUG_GL_KEEP_TRAILS=1` keeps the trail.
- **Still there, says the owner, with two more F10 captures.** The second
  (out of bounds by the freeway) shows the copies inside the scene surface
  `0x80ABC000` itself: a stack of skateboards, doubled limbs, star copies,
  torn letters. Stale depth. Captures now log clears and each draw's zeta:
  the game clears zeta `0x80864000` with the screen buffer bound, then draws
  the scene into `0x80ABC000` with that zeta. On the NV2A one clear covers
  both; here each target has its own depth buffer, and the scene surface's
  was never cleared. A zeta clear now reaches every target that last drew
  with that zeta (`THUG_GL_ZETA_LOG=1` reports it). In the scripted run it
  fired on every frame from the first transition on. Owner to confirm in
  game.
- **Glitch capture:** F10 in game (or `THUG_GL_CAPTURE_AT=<flip>`) records
  the next two frames: every draw as a `[CAPTURE]` line plus the probe's full
  record in the log, and every render target as PPM under
  `~/.local/state/thug-pc-port/captures/<time>/`. For effects on screen for
  a second, which no scripted run can find.

### 2026-11-11: one repository, one executable

- **Owner request:** commit the third-party code instead of using
  submodules; make the `thug` executable all a player runs, unpacking and
  checking the disc image itself (the image now lives in `game/`).
- **Vendored** `vendor/xboxrecomp` (with this port's changes in place;
  the patch workflow is gone) and the parts of Dear ImGui the build uses.
  The toolkit's 679 tracked files match its own list exactly; its
  per-game pipeline output stays ignored. See `vendor/README.md`.
- **Start-up** (`src/host/launch.c`, `src/host/game_data.c`):
  - `game/` is found next to the executable or in the tree above it, so
    `build/bin/thug` works from any directory.
  - Disc image only: unpacked on first launch (5,797 files, 3.4 GB; the
    Redump image's game partition at 0x18300000), with a progress window and
    a free-space check, then `default.xbe` verified. Byte-for-byte the same
    as the old Python extraction; about 2 s from a warm cache.
  - `default.xbe` written last, so an interrupted unpack is redone; a
    corrupted or other-version `default.xbe` is replaced from the image, or
    refused without one.
  - Nothing there: a message box (and the terminal) says what to put where.
  - The log goes to `~/.local/state/thug-pc-port/thug.log` unless stderr is
    redirected; a crash points the terminal at it.
- `scripts/build.sh` builds and runs `thug-extract` (the same code as a
  command) before lifting. `extract.sh`, `play.sh` and `apply-patches.sh`
  are gone.
- **Not done:** the Windows side compiles in principle (message box, no
  progress window) but is untested; Windows builds are not set up.

### 2026-11-10: antialiasing, more overlay settings

- **Owner request:** antialiasing (2x, 4x, 8x) in the overlay, plus any other
  quality-of-life settings worth having.
- **MSAA.** Every render target can carry a multisampled colour and
  depth-stencil pair (`THUG_MSAA`, live from the overlay). Draws and clears go
  there; the single-sample texture becomes the resolve, done lazily when
  something reads it: the game's own 2×-wide AA pass (once a frame), the
  present, a dump. Changing the sample count or the scale reallocates every
  target and carries its contents across. Checked with frame dumps: slanted
  edges, the skater's hair and the chain-link fences in New Jersey are smooth
  at 4x and 8x.
- **Alpha-tested edges.** Plain MSAA does nothing for cutouts (fences,
  foliage, hair). With GL_ARB_sample_shading, an alpha-tested draw writes
  `gl_SampleMask` from how far its alpha is from the reference, measured in
  pixels (`fwidth`), so the cut is antialiased like a polygon edge and keeps
  its shape. THUG never disables blending (opaque surfaces blend ONE, ZERO;
  cutouts blend SRC_ALPHA over an alpha test with references from 1 to
  0x80), so both of those qualify; additive and modulating blends do not, and
  neither does `GEQUAL 0`, which always passes.
  - First attempt drew nothing: a shader that writes `gl_SampleMask` on one
    path must write it on all of them, or coverage is undefined (NVIDIA:
    none).
- **Cost** (GPU time per frame from a timer query, skater-select room):
  - 720p: 7.0 ms at every setting; the executor is the limit (about 143 fps).
  - 4K: 24-25 ms off, within a few percent at 4x and 8x, though runs vary.
    At 4K the GPU is already the limit without MSAA (100% busy, about 40 fps
    in that room): the generic combiner shader is expensive over THUG's
    2×-wide buffer. Specialising it is the next GPU optimisation.
- **Also added to the overlay:**
  - Brightness: a gamma pass on the presented frame (only when not 100%).
  - Volume and "mute in background" (toolkit: a master gain at the APU's
    output, ramped so it never clicks).
  - Vibration strength (toolkit: a rumble scale; the pad also stops rumbling
    while the overlay is open).
  - Restore defaults (keeps the window mode).
  - The mouse cursor hides after 2 s still in a window, and always in
    fullscreen unless the overlay is open.
- **Diagnostics:** `THUG_GL_STATS` adds GPU ms, resolves and alpha-to-coverage
  draws; `THUG_BENCH_MSAA`; `THUG_GL_DEBUG` (KHR_debug).

### 2026-11-10: face artifacts, sky seam

- **Owner report:** artifacts around the skater's face, and the sky looks
  wrong, with a sharp contrast between two areas.
- **Face.** Frame dumps of the skater-select screen with
  `THUG_GL_NO_POLYOFF=1` and `THUG_GL_NO_MIPS=1` pointed at depth bias:
  without it, the face was clean. `RECOMP_GL_PROBE` on an eye showed bias
  on every draw. The executor read the fill enable from `0x340`, which is
  `SET_ALPHA_REF` (THUG keeps it at 1), instead of
  `SET_POLY_OFFSET_FILL_ENABLE` (`0x338`). Every draw took the last factor
  and units set, and the eyes and mouth (drawn without bias) lost to the
  face under them. Fixed in the toolkit; its test now checks that `0x340`
  does not enable bias.
- **Depth range.** The renderer put the title's z in half the depth buffer
  (window z 0.5 to 1), which halved its precision and doubled each bias
  unit. Window depth is now the title's z.
- **Sky.** About 115 triangles a frame never reached GL: the top of the sky
  dome projects tens of millions of pixels off screen, past the renderer's
  1e6 sanity bound, so the clear colour showed across the top of the sky.
  The bound is now 1e12, and GL clips the rest. New Jersey now drops 0
  triangles a frame (`THUG_GL_STATS` counts them), and the dome is whole.
- **Monitor logo (skater-select room).** With the bias fixed, a blue-grey
  shape appears on the left monitor behind the skater. It looks native:
  - A 32×32 logo decal (`0x831E1000`: additive, depth writes on, no bias)
    is drawn before the monitor's screen and sits about 180 depth units in
    front of it. Both use the same fixed-function transform.
  - Wherever the logo's alpha passes the alpha test, it writes depth. The
    screen then fails the test there, and the game's own clear colour
    (`0x506070`) shows through.
  - The method trace shows no bias enabled for it, so hardware given the
    same commands draws the same. This hasn't been checked against a
    console capture. The misread bias had been hiding it.
- **Diagnostics added:**
  - `THUG_GL_NO_POLYOFF` and `THUG_GL_NO_MIPS`.
  - `THUG_GL_POLYOFF_LOG`: the distinct offsets and `SET_CONTROL0` values.
    THUG uses `0x00100001`, so no w-buffer.
  - The probe prints each triangle's depth at the pixel.
  - `RECOMP_TRACE_TEX0` lists the poly-offset methods too.

### 2026-11-10: performance in levels

- **Owner report:** skating New Jersey at 1080p rarely holds 60 fps; is the
  flicker fix to blame?
- **Not the fix:** the same route with the old and new pushbuffer walk gives
  the same frame rate for the same draw count.
- **Where the time goes** (`RECOMP_EXEC_PROFILE=1`): the title's thread
  waits in D3D's kickoff (`sub_00190730`, the PFB flush handshake) while the
  executor works, so the executor's time is the frame time. Heaviest view
  before: 22 ms (vertex programs 310 ns a vertex, GL submission 5.5 us a
  draw).
- **Tried and reverted:** acknowledging the kickoff mid-walk (and publishing
  GET) to overlap the two. With the mirrored fence it either let D3D overwrite
  unread commands (the mirror ticked mid-walk) or sent D3D into the
  software-method wake-up it cannot leave (GET behind a fence). It needs the
  semaphore fence plus the GPU's software-method interrupt.
- **Done:**
  - GL submission: sampler objects, bindings tracked per unit, constant
    uniforms set once, no stage-0 unbind per draw, vertex data written
    straight into a persistently mapped ring (ARB_buffer_storage, fenced
    8 MB segments). About 2.5 us a draw, from 5.5.
  - Vertex programs: run 8 vertices at a time (toolkit), inputs resolved per
    batch, unique vertices transformed once and assembled through a map,
    no copies for unclipped triangles. Executor time per index 155-170 ns,
    from 235-260.
- **Result** (headless, 1080p, same scripted route, 2 runs): mean 83-84 fps
  at about 1,000 draws a frame (min 49-60), against 60-77 fps at 680-930
  draws (min 38-41) before. The heaviest views are still around 50.
- **Next, if more is needed:** indexed draws to the back end (convert each
  unique vertex once), a GL thread so submission overlaps transform, and the
  software-method interrupt so the title and the executor can overlap.

### 2026-11-10: the flicker, found and fixed

- **Owner report:** a one-frame flash, most visible on the main menu (the
  background panel goes flat grey-blue), also in levels; the menu skater is a
  silhouette.
- **Detector.** `THUG_GL_FLICKER=<dir>` reads back every presented frame and
  saves any frame unlike both neighbours, with per-frame counters. 83 hits in
  80 s on the main menu: exploded geometry (dropped non-finite vertices), the
  background panel gone, once two frames' draws before one flip.
- **Cause.** `RECOMP_TRACE_TEX0` on the panel's texture: in the bad frame the
  panel was drawn with the skater's vertex arrays, and the methods before it
  had no array setup; D3D had sent it, in commands the executor never ran.
  The default pushbuffer scan handled ring wraps by guessing the ring's
  bounds from the lowest and highest PUT seen, so each wrap could drop this
  lap's first commands or replay stale ones. A walk that follows the JUMP and
  ends at PUT (toolkit) gives 0 hits in 90 s of menu and in a level.
- **Side trip.** A real, semaphore-driven fence (instead of the mirror) cut
  the hits first, but it was the wrap all along, and it hangs at the save
  check (D3D's software-method wake-up). Kept as `THUG_FENCE_SEMAPHORE=1`.
  Game timing was ruled out too.
- **Silhouette:** intended. The program outputs RGB 0 with animated alpha.

### 2026-11-10: settings overlay (F1), resolution benchmark

- **Owner request:** try 1080p instead of 4K, and an in-game overlay to
  change settings on the fly.
- **Benchmark.** `THUG_BENCH_SCALES` cycles the internal scale in one running
  scene (headless, RTX 4080, 185 draws a frame), five rounds each:
  3840×2160 about 71 fps, 1920×1080 about 109, 853×480 about 134. So at 4K
  the GPU work is a real cost, most likely the generic register-combiner
  fragment shader running over THUG's 2×-wide buffer (16.6 million pixels a
  pass at 4K). Specialising shaders per combiner setup would cut it.
- **Overlay.** Dear ImGui (v1.91.9b, `vendor/imgui`), drawn by
  the renderer over the finished frame (`nv2a_pb_gl_set_overlay`) through
  epoxy. Events are queued from the main thread. While open the game's pad
  reads at rest (toolkit `xbox_InputSetBlocked`) and keyboard input stays
  with the overlay; F1, Esc, or Back+Start and B on a pad. Settings:
  resolution (native, 2160p to 480p, applied live: every render target is
  reallocated and its contents scaled across), fullscreen, vsync, frame rate
  limit, anisotropic filtering, widescreen (next start), original 60 Hz
  timing (live), FPS counter; live fps, frame time, draws, triangles and a
  frame-time graph. `src/ui/settings.c` keeps them in
  `~/.config/thug-pc-port/settings.ini`; a `THUG_*` variable overrides a line
  for one run.

### 2026-11-10: black cutscene props, decal flicker

- **Owner report:** runs well; textures missing in cutscenes (Chad Muska's car,
  the dresser in the player's room, the flyer Eric holds in the story intro);
  some flicker here and there.
- **Black props.** `RECOMP_GL_PROBE_TEX` found the flyer's draw and
  `THUG_GL_TEXDUMP` showed its texture decodes correctly; the vertex colour
  was `0x80000000`. `RECOMP_TRACE_TEX0` showed fixed-function lighting with
  colour material `0x14` and the global ambient in `MATERIAL_EMISSION`, a
  term the toolkit's lighting never applied (toolkit fix, see NOTES). The
  flyer and the dresser now render; the car uses the same path.
- **Flicker.** Neither the executor nor the renderer handled polygon offset,
  and THUG uses it on about 100 draws a frame (decals: graffiti, markings).
  Now passed through and applied (bias in the zeta format's units, slope term
  scaled by the internal resolution). Not confirmed by the owner yet. The
  presented surface never changes between frames (`THUG_GL_STATS` counts it),
  so a wrong-surface flash is ruled out.
- **Culling** looked at and left off: see NOTES ("Culling").

### 2026-11-10: GPU renderer, widescreen, unlocked frame rate

- **Renderer.** `src/gpu/nv2a_pb_gl.c` started as the otogi1 fork's OpenGL back
  end and is reworked for THUG. Each change is marked `[thug]`.
  - One target per colour surface, sized to the furthest clip drawn into it,
    with the clip as a scissor (the fork made a target per clip, splitting
    THUG's letterboxed and full-frame passes on the same buffer).
  - A texture whose address is a render target binds that target's GL
    texture, so render-to-texture needs no readback.
  - Linear textures are addressed in texels, swizzled and DXT ones
    normalised; the shader scales per stage.
  - The flipped surface is blitted straight to the renderer's own window,
    aspect-fitted, and nothing is read back unless asked
    (`THUG_GL_READBACK=1`, or `RECOMP_FB_DUMP*`).
  - Internal resolution: every surface renders at `THUG_RES_SCALE` (default
    desktop height / 480); widescreen scales x by a further 4/3, so 1080p
    renders the 640×480 front buffer at 1920×1080.
  - Texture cache: 4096 entries, hashed lookup, LRU. The content hash covers
    the bytes the format occupies (the stale pitch register made it hash 80×
    too much and re-upload on unrelated writes), sampled above 32 KB. Decode
    failures are cached. Mipmaps are generated for swizzled/DXT textures, with
    trilinear plus 16× anisotropic filtering where the title asks for mip
    filtering.
  - P8 textures decode through their palette (toolkit fix): fonts and menu
    art were solid blocks without it.
  - Clears write what `CLEAR_SURFACE` names, whatever the draw's write masks.
    THUG clears with depth writes off, so before this every 3D draw failed
    the depth test.
  - Vertex data streams through a 64 MB ring (unsynchronised maps), and the
    combiner uniforms, render state and target binding are only re-sent when
    they change.
- **Window.** `src/host/host_window.c` runs the guest on a pthread and pumps
  SDL events on the main thread. The executor's thread swaps the window
  (vsync, adaptive if available). F11 or Alt+Enter toggles fullscreen. The
  title shows the output resolution and FPS. `RECOMP_HEADLESS=1` keeps it
  hidden; `THUG_GL_DUMP=<dir>` writes frames as PPM.
- **Frame timing.** THUG's game time is vblanks per swap, at least one
  (`sub_00173F80`, see `docs/NOTES.md`), so above 60 fps the game ran fast.
  The override uses host time instead. Checked in a level: `[0x35F030]` read
  0.91, 1.25 and 1.01 vblanks at the measured 48–66 fps.
- **Widescreen.** On by default. The AV pack query now carries the
  dashboard's video flags (toolkit fix), and THUG then creates a widescreen
  device and renders Hor+ with a 16:9 HUD.
- **Executor.** A post-transform vertex cache (toolkit) roughly halved the
  vertex-program work in levels.
- **Measured** (RTX 4080, headless, 2× scale): menus 60–160 fps; free skate,
  New Jersey, 75–100 fps in ordinary views and 47–60 in the heaviest (about
  1,900 draws, 54K triangles a frame), up from about 9. The executor thread
  is the limit: the vertex-program interpreter and per-draw GL submission.
- **Not done:** shadows (depth textures, format `0x2E`); a black quad in the
  intro cutscene; face culling stays off (`RECOMP_GL_NO_CULL=1`).
- **Next:** owner playtest (windowed path, input feel, any artifacts left);
  move vertex programs to the GPU or run them across threads; shadows.

### 2026-11-10: in a level (free skate, New Jersey)

- **Owner report:** the menus navigate, at about 8 to 10 fps and with laggy
  input. Free skate froze on the level loading screen.
- **Reproduced headless:** I drove the menus through `RECOMP_PAD_LIVE` and
  watched the frame dumps. The freeze was `sub_000B3D20` (texture level copy)
  with a corrupt loop bound.
- **gdb tracer** (breakpoints on the copy, `GetLevelDesc` and `LockRect`):
  copy #11 had a null source texture. The log showed `[CONTIG] arena
  exhausted` 100+ times with `allocs=2027 frees=0`. Contiguous memory is
  never freed without `RECOMP_HEAP_RECLAIM`, so the create failed and the
  title called `exit()`. `KeBugCheck` returned, and the stack was 4 bytes
  off.
- **Fixes:**
  - `RECOMP_HEAP_RECLAIM=1` is now a default (408 frees by level load, no
    exhaustion).
  - `KeBugCheck` now ends the process (toolkit patch), so a future fatal
    error exits with a message instead of freezing.
- **Result:** free skate, New Jersey loads and runs: HUD, skater, level.
- **Performance:** about 9 fps in the menus and in-game. A profile (`top -H`
  plus gdb samples) shows the game thread waiting in D3D for push-buffer
  space, the NV2A executor thread at about 88%, and about 15 software
  rasteriser workers at 50 to 65%. The game is **GPU-bound on the software
  rasteriser**. Input latency follows from the low frame rate (input is read
  once a frame).
- **Next:** a GPU render back end. Otogi's fork (`sigods/xboxrecomp`, branch
  `otogi1-linux-v0.1`, MIT) has `src/nv2a/nv2a_pb_gl.c`, an OpenGL 3.3 back
  end for this executor that runs the register combiners in GLSL. It plugs
  in through `nv2a_backend.h`.

### 2026-11-10: main-menu freeze and dead pad: colour draws at physical 0

- **Owner run:** skipped the intro, then the main menu froze and the pad
  stopped responding. It wasn't a crash: the game kept drawing at about
  8 flips/s, and it never loaded past `skaterparts.prx`.
- **Cause:** the DirectSound free list ended in a node that pointed into
  zeroed memory, so the allocator looped forever.
  - `RECOMP_WATCH` showed the node was valid when written, then later
    overwritten by an unattributed bulk write.
  - A **gdb hardware watchpoint** caught the writer: `put_pixel` on the NV2A
    executor thread.
  - `RECOMP_PB_SURFACES` showed THUG drawing with colour enabled into a
    surface at colour offset 0, which resolves to `0x80000000` and up.
  - That range covers DirectSound's first contiguous blocks and page 0, XPP's
    private USB arena.
- **Fix:** toolkit patch. A colour surface at physical 0 means no colour
  surface; depth and stencil are still written. RAMHT and DMA-object base
  resolution were also added, but THUG's objects are based at 0.
- **Result:** 6 of 6 runs since the fix (headless and windowed, synthetic
  Start presses) reach the pre-made skater-select screen. Before it, about 3
  in 10 froze at the main menu at about 256 flips.
- **New blocker at skater select (about 560 flips):** the texture
  level-copy loop `sub_000B3D20` runs away. See NOTES, "Texture level copy".

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
