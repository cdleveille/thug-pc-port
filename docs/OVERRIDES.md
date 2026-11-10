# Manual overrides

Every function replaced in `src/recomp_manual.c`, and why. Remove the row when
the override is removed.

| Guest VA | Function (best name) | Symptom without it | What the override does | Added |
|---|---|---|---|---|
| `0x00178B85` | XAPI media check (XDK 5659), caller `sub_00178D5F` | The disc security sector can't be authenticated (there is no drive). The title reboots to the dashboard via `HalReturnToFirmware`. | Returns `STATUS_SUCCESS`. | 2026-11-10 |
