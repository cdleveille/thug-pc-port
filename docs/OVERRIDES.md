# Manual overrides

Every function replaced in `src/recomp_manual.c`, and why. Remove the row when
the override is removed.

| Guest VA | Function (best name) | Symptom without it | What the override does | Added |
|---|---|---|---|---|
| `0x00178B85` | XAPI media check (XDK 5659), caller `sub_00178D5F` | The disc security sector can't be authenticated (there is no drive). The title reboots to the dashboard via `HalReturnToFirmware`. | Returns `STATUS_SUCCESS`. | 2026-11-10 |
| `0x00173F80` | Engine frame-length update (Neversoft `Tmr`), called once a frame from the main loop | Game time counts whole vblanks per swap, at least one, so above 60 fps the game runs faster than real time (at 144 fps, 2.4×). | Sets `[0x35F030]` (float) and `[0x35F038]` (double) to the real host time between frames in vblanks of the title's refresh rate, averaged over four frames and capped at four vblanks as before. `THUG_VBLANK_TIMING=1` restores the original arithmetic. | 2026-11-10 |
