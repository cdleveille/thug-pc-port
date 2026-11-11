/**
 * restart.c -- start the game again in the same process. See restart.h.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE                    /* environ, syscall */
#endif
#include "restart.h"
#include "launch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

extern char **environ;

static char **s_argv;
static char **s_env;
static char   s_exe[4096];      /* this executable's path, resolved at start */

void thug_restart_init(int argc, char **argv)
{
    int i, n = 0;
    ssize_t len;

    /* Exec'ing the path keeps the process's name; exec'ing /proc/self/exe
     * names it "exe" (a task manager, pgrep). */
    len = readlink("/proc/self/exe", s_exe, sizeof s_exe - 1);
    s_exe[len > 0 ? len : 0] = 0;
#if defined(__linux__)
    /* A run started through /proc/self/exe (the fallback below, when the
     * executable was deleted meanwhile) takes its name back. */
    if (argc > 0 && argv[0]) {
        const char *base = strrchr(argv[0], '/');
        prctl(PR_SET_NAME, base ? base + 1 : argv[0], 0, 0, 0);
    }
#endif

    s_argv = (char **)calloc((size_t)argc + 1, sizeof *s_argv);
    if (s_argv)
        for (i = 0; i < argc; i++)
            s_argv[i] = strdup(argv[i]);
    while (environ && environ[n])
        n++;
    s_env = (char **)calloc((size_t)n + 1, sizeof *s_env);
    if (s_env)
        for (i = 0; i < n; i++)
            s_env[i] = strdup(environ[i]);
}

int thug_restart_supported(void)
{
    return s_argv && s_env && s_argv[0];
}

/* Every descriptor above stderr closes at the exec: close_range where the
 * kernel has it (5.11), else each one in /proc/self/fd. */
static void cloexec_all(void)
{
    DIR *d;
    struct dirent *de;

#if defined(SYS_close_range)
    if (syscall(SYS_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */) == 0)
        return;
#endif
    d = opendir("/proc/self/fd");
    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        int fd = atoi(de->d_name);
        if (fd > 2 && fd != dirfd(d))
            fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    closedir(d);
}

void thug_restart_exec(void)
{
    int log_out, log_err, err;

    if (!thug_restart_supported())
        return;
    fprintf(stderr, "[thug] restarting to apply the settings\n");
    fflush(NULL);
    /* The log, for this run to carry on in should the exec fail. */
    log_out = fcntl(1, F_DUPFD_CLOEXEC, 3);
    log_err = fcntl(2, F_DUPFD_CLOEXEC, 3);
    thug_launch_restore_stdio();
    cloexec_all();
    /* This executable's path (a rebuilt one there runs); this executable
     * even if that file is gone; then the name it was started by. */
    if (s_exe[0])
        execve(s_exe, s_argv, s_env);
    execve("/proc/self/exe", s_argv, s_env);
    if (strchr(s_argv[0], '/'))
        execve(s_argv[0], s_argv, s_env);
    else
        execvpe(s_argv[0], s_argv, s_env);
    err = errno;
    if (log_out >= 0) dup2(log_out, 1);
    if (log_err >= 0) dup2(log_err, 2);
    fprintf(stderr, "[thug] could not restart: %s\n", strerror(err));
}

#else /* _WIN32: not yet */

void thug_restart_init(int argc, char **argv) { (void)argc; (void)argv; }
int  thug_restart_supported(void) { return 0; }
void thug_restart_exec(void) { }

#endif
