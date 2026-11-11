/**
 * launch.h -- what the game does before it boots, so that running the
 * executable is all a player has to do.
 *
 *   1. The log: when nobody redirected stderr (a terminal, a desktop
 *      launcher, Explorer), it goes to a file instead -- thug.log in the state
 *      directory ($XDG_STATE_HOME/thug-pc-port, ~/.local/state/thug-pc-port;
 *      %LOCALAPPDATA%\thug-pc-port on Windows), the previous run's kept as
 *      thug.prev.log -- and the terminal, if any, gets only what a player
 *      needs to read. THUG_LOG=<file> picks the file, THUG_LOG=- keeps stderr
 *      as it is. A redirected stderr (a file or a pipe, as scripts and the
 *      debugging recipes use) is left alone.
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

/* The folder for the log and glitch captures, created if need be: as above,
 * with forward slashes on POSIX and backslashes on Windows. 1 with it in out,
 * 0 when there is none (no HOME, no LOCALAPPDATA, or it cannot be made). */
int thug_launch_state_dir(char *out, size_t n);

/* Put stdout and stderr back as the game was started with (the log, if
 * thug_launch_log_setup moved them to it, stays where it is on disk): for a
 * restart, so the new run sets up its own log. */
void thug_launch_restore_stdio(void);

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
