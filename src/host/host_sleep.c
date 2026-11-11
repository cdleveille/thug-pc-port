/**
 * host_sleep.c -- see host_sleep.h.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L        /* clock_nanosleep */
#endif
#include "host_sleep.h"

#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void thug_sleep_until(const struct timespec *deadline)
{
    /* One timer per thread: a waitable timer belongs to whoever sets it. */
    static __thread HANDLE timer;
    static __thread int tried;
    struct timespec now;
    int64_t ns;

    clock_gettime(CLOCK_MONOTONIC, &now);
    ns = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000LL
       + (deadline->tv_nsec - now.tv_nsec);
    if (ns <= 0)
        return;
    if (!tried) {
        tried = 1;
        /* Windows 10 1803 and later; else an ordinary timer, which still
         * honours the 1 ms resolution SDL asks for at start-up. */
        timer = CreateWaitableTimerExW(NULL, NULL,
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        if (!timer)
            timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100);            /* relative, 100 ns units */
        if (due.QuadPart < 0
                && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD)((ns + 999999) / 1000000));
}

#else /* POSIX */

void thug_sleep_until(const struct timespec *deadline)
{
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
}

#endif
