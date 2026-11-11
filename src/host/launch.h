/**
 * launch.h -- what the game does before it boots, so that running the
 * executable is all a player has to do.
 *
 *   1. The log: when nobody redirected stderr (a terminal, a desktop
 *      launcher), it goes to a file instead -- thug.log in the state directory
 *      ($XDG_STATE_HOME/thug-pc-port, ~/.local/state/thug-pc-port), the
 *      previous run's kept as thug.prev.log -- and the terminal, if any, gets
 *      only what a player needs to read. THUG_LOG=<file> picks the file,
 *      THUG_LOG=- keeps stderr as it is. A redirected stderr (a file or a
 *      pipe, as scripts and the debugging recipes use) is left alone.
 *   2. The game's files (src/host/game_data.h): found next to the
 *      executable; unpacked from the disc image on first launch, with a
 *      progress window; refused with a message box when absent or not the
 *      supported version.
 */
#ifndef THUG_LAUNCH_H
#define THUG_LAUNCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void thug_launch_log_setup(void);

/* 1 with game_dir ready to boot from, or 0 after telling the player why the
 * game cannot start (the caller exits). */
int thug_launch_prepare(char *game_dir, size_t n);

/* From a fault handler (async-signal-safe): point the terminal, if the log
 * went to a file, at that file. */
void thug_launch_crash_notice(void);

#ifdef __cplusplus
}
#endif

#endif /* THUG_LAUNCH_H */
