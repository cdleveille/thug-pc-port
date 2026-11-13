THUG PC Port @VERSION@ for Linux (x86-64)
==========================================

An unofficial native port of Tony Hawk's Underground (Xbox, USA, 2003),
made by statically recompiling the original game's code: nothing is
emulated. The port is at an early stage. You need your own copy of the
game; this package contains none of it.

Source code, issues and updates: https://github.com/cdleveille/thug-pc-port


What you need
-------------
- 64-bit x86 Linux with glibc 2.35 or newer: Ubuntu 22.04, Debian 12,
  Fedora 36 or later, or a current rolling distribution.
- A graphics driver with OpenGL 3.3: Mesa (AMD, Intel) or NVIDIA's own.
- An X11 or Wayland desktop.
- Your disc image of Tony Hawk's Underground for the Xbox, USA release,
  and about 3.5 GB of free space for the unpacked game.


Setting up
----------
1. Put your disc image in the game/ folder. A full Redump dump or a
   trimmed XISO both work; the file name must end in .iso or .xiso.
2. Open a terminal in this folder and run:  ./thug

The first launch unpacks the image into game/ (with a progress window),
checks that it is the supported USA release, and starts the game. After
that the image is not needed and can be deleted. Other releases of the
game are refused with a message saying so.


Controls
--------
A controller is player one; plug it in before or during play. The
keyboard drives the menus too:

  Enter or Space      A (accept)
  Backspace or Tab    B (back)
  Escape              Start (pause) while skating; back in menus
  W A S D             left stick
  Arrow keys          D-pad
  F1                  settings (on a pad: click both sticks, L3 + R3)
  F11 or Alt+Enter    fullscreen
  F10                 capture a graphics glitch, for a bug report

The settings cover the display (aspect ratio, resolution, antialiasing,
frame rate limit, vsync, brightness, Wayland or X11), sound and vibration.


Where things are
----------------
Settings  ~/.config/thug-pc-port/settings.ini
Log       ~/.local/state/thug-pc-port/thug.log (the previous run's is
          thug.prev.log). Attach it to a bug report.
Captures  ~/.local/state/thug-pc-port/captures/ (F10)

($XDG_CONFIG_HOME and $XDG_STATE_HOME are used instead of ~/.config and
~/.local/state when they are set.)


If something goes wrong
-----------------------
- Nothing happens, or the game exits: read the log above, or run ./thug
  from a terminal.
- "Permission denied": run  chmod +x thug
- "version `GLIBC_2.xx' not found": your distribution is older than this
  release supports (glibc 2.35).
- Low frame rate with an NVIDIA graphics card: in the settings (F1),
  set Display server to X11 and restart. NVIDIA's driver is faster there.
- Window or graphics trouble on Wayland: the same setting, or
  SDL_VIDEODRIVER=x11 ./thug
- Game files kept somewhere else:  THUG_GAME_DIR=/path/to/them ./thug


Licences
--------
The third-party components built into the game, with their licences, are
listed in licenses/: SDL 2 (zlib; its hidapi and yuv2rgb parts BSD-style),
Dear ImGui (MIT), the glad OpenGL loader, and the xboxrecomp toolkit (MIT,
with parts from xemu under the LGPL 2.1). Their source code, and the port's,
is at the address above.

Tony Hawk's Underground is property of Activision and Neversoft. This is an
unofficial fan project, not affiliated with or endorsed by either.
