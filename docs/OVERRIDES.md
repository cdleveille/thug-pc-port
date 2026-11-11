# Manual overrides

Every function replaced in `src/recomp_manual.c`, and why. Remove the row when
the override is removed.

| Guest VA | Function (best name) | Symptom without it | What the override does | Added |
|---|---|---|---|---|
| `0x00178B85` | XAPI media check (XDK 5659), caller `sub_00178D5F` | The disc security sector can't be authenticated (there is no drive). The title reboots to the dashboard via `HalReturnToFirmware`. | Returns `STATUS_SUCCESS`. | 2026-11-10 |
| `0x0008F830` | The screen aspect getter (`fld [0x2B33DC]`; set by the script command `SetScreen`), read by the viewport's aspect (`sub_0008FDA0`) and the screen-to-3D helper (`sub_000C5960`) | The game knows two shapes, 4:3 (1.33333) and 16:9 (1.77778); at 16:10 the picture would be a 16:9 view squeezed horizontally. | For an aspect wider than 4:3 (`aspect_ratio`, `THUG_ASPECT`) returns the chosen aspect; at 4:3 the stored value, so letterboxed cutscenes stay as they were. | 2026-11-11 |
| `0x0008F860` | The field-of-view ratio getter (`fld [0x229C2C]`, tan(angle/2) / tan(72°/2), set by `SetScreen`'s angle), read by the cameras when their FOV is set (`sub_0009ADA0` and siblings) | As above: the 16:9 field of view (88.18°) at any wide aspect. | Scales the stored ratio by chosen / stored aspect, which keeps the game's vertical view (Hor+): exactly the game's values at 16:9, 82.2° at 16:10; the stored value at 4:3. | 2026-11-11 |
| `0x00173F80` | Engine frame-length update (Neversoft `Tmr`), called once a frame from the main loop | Game time counts whole vblanks per swap, at least one, so above 60 fps the game runs faster than real time (at 144 fps, 2.4×). | Sets `[0x35F030]` (float) and `[0x35F038]` (double) to the real host time between frames in vblanks of the title's refresh rate, averaged over four frames and capped at four vblanks as before. `THUG_VBLANK_TIMING=1` restores the original arithmetic. | 2026-11-10 |
