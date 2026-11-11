/**
 * restart.h -- start the game again in the same process, for settings that
 * only take effect at start-up (the overlay's "Restart to apply changes").
 *
 * The process replaces itself (execve), so its ID stays the same and a
 * launcher that watches it (Steam) sees one game, not a game that quit. The
 * new run gets the environment and arguments the player started this one
 * with, not the THUG_* variables start-up exported from the settings file:
 * those would override the file just saved. Its log starts afresh where
 * this one's did, and nothing this run had open survives the exec (the old
 * window's display connection included, so no frozen window is left behind).
 *
 * Lifted code cannot be stopped mid-flight, so nothing is torn down first,
 * as when the window is closed.
 */
#ifndef THUG_RESTART_H
#define THUG_RESTART_H

#ifdef __cplusplus
extern "C" {
#endif

/* First thing in main(): remember the arguments and the environment. */
void thug_restart_init(int argc, char **argv);

/* Whether this build can restart itself (POSIX; not yet Windows). */
int  thug_restart_supported(void);

/* Restart now. Returns only if the exec failed (logged). Main thread, after
 * the settings are saved. */
void thug_restart_exec(void);

#ifdef __cplusplus
}
#endif

#endif /* THUG_RESTART_H */
