/**
 * host_window.h -- the game's window loop when the GPU renderer owns the
 * window (POSIX).
 */
#ifndef THUG_HOST_WINDOW_H
#define THUG_HOST_WINDOW_H

/* Run guest_main on a pthread and pump the GPU renderer's window on this
 * (the process main) thread until the guest returns or the window is closed.
 * Without a renderer window it calls guest_main directly. */
int thug_host_window_main(int (*guest_main)(void));

#endif /* THUG_HOST_WINDOW_H */
