/**
 * host_sleep.h -- sleeping until a point on the monotonic clock, precisely.
 *
 * The frame cap (src/gpu/nv2a_pb_gl.c) holds each flip to a schedule on
 * CLOCK_MONOTONIC. POSIX sleeps to the deadline with clock_nanosleep. On
 * Windows, winpthreads' clock_nanosleep is a Sleep() that rounds to the
 * system tick (15.6 ms unless something raised the timer resolution), which
 * turns a 60 fps cap into 32; there a high-resolution waitable timer takes the
 * sleep instead.
 */
#ifndef THUG_HOST_SLEEP_H
#define THUG_HOST_SLEEP_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sleep until *deadline on CLOCK_MONOTONIC (as clock_gettime reads it).
 * Returns at once if it has passed. Per thread: call it from any thread. */
void thug_sleep_until(const struct timespec *deadline);

#ifdef __cplusplus
}
#endif

#endif /* THUG_HOST_SLEEP_H */
