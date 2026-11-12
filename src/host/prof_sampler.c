/**
 * prof_sampler.c -- see prof_sampler.h.
 *
 * In process, so it shares the process's locks: a thread is only suspended
 * while its stack is walked, nothing is allocated or printed meanwhile, and
 * the tables are made before sampling starts. RtlLookupFunctionEntry takes
 * the loader's function-table lock shared; a thread suspended while loading
 * a module could hold it, so the sampler starts once start-up is over.
 * Diagnosis only.
 */
#include "prof_sampler.h"

#if defined(_WIN32)

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launch.h"

#define MAX_THREADS 256
#define MAX_FRAMES  48
#define TABLE_BITS  20                     /* entries per table, a power of 2 */
#define TABLE_SIZE  (1u << TABLE_BITS)

typedef struct {
    DWORD    tid;
    HANDLE   h;
    uint64_t samples;
    uint64_t cpu0, cpu1;                   /* kernel + user, 100 ns */
    int      cpu0_set;
    wchar_t  name[64];
} Thr;

typedef struct {
    uint64_t addr;                         /* 0 = empty */
    uint32_t thr;
    uint32_t count;
} Slot;

static Thr     s_thr[MAX_THREADS];
static int     s_nthr;
static Slot   *s_self, *s_incl;
static double  s_start_s = 10.0, s_dur_s = 10.0;
static int     s_interval_us = 1000;
static uint64_t s_rounds, s_lost;

static uint64_t cpu_time(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u))
        return 0;
    return ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime)
         + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

static void count(Slot *t, uint64_t addr, uint32_t thr)
{
    uint64_t h = (addr * 0x9E3779B97F4A7C15ull) ^ thr;
    uint32_t i = (uint32_t)(h >> (64 - TABLE_BITS)), n;

    for (n = 0; n < TABLE_SIZE; n++, i = (i + 1) & (TABLE_SIZE - 1)) {
        if (t[i].addr == addr && t[i].thr == thr) {
            t[i].count++;
            return;
        }
        if (!t[i].addr) {
            t[i].addr = addr;
            t[i].thr = thr;
            t[i].count = 1;
            return;
        }
    }
    s_lost++;
}

/* The threads of this process, other than the sampler. */
static void refresh_threads(void)
{
    typedef HRESULT (WINAPI *get_desc_fn)(HANDLE, PWSTR *);
    static get_desc_fn get_desc;
    static int looked;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();

    if (!looked) {
        looked = 1;
        get_desc = (get_desc_fn)(void (*)(void))GetProcAddress(
            GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    }
    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            int i, known = 0;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me)
                continue;
            for (i = 0; i < s_nthr; i++)
                if (s_thr[i].tid == te.th32ThreadID)
                    known = 1;
            if (known || s_nthr >= MAX_THREADS)
                continue;
            s_thr[s_nthr].tid = te.th32ThreadID;
            s_thr[s_nthr].h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                                         | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!s_thr[s_nthr].h)
                continue;
            if (get_desc) {
                PWSTR d = NULL;
                if (SUCCEEDED(get_desc(s_thr[s_nthr].h, &d)) && d) {
                    wcsncpy(s_thr[s_nthr].name, d, 63);
                    LocalFree(d);
                }
            }
            s_thr[s_nthr].cpu0 = cpu_time(s_thr[s_nthr].h);
            s_thr[s_nthr].cpu0_set = 1;
            s_nthr++;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

/* Walk one suspended thread's stack into pcs[]; the number of frames. */
static int walk(HANDLE h, uint64_t *pcs)
{
    CONTEXT c;
    int n = 0;

    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(h, &c))
        return 0;
    while (n < MAX_FRAMES && c.Rip) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fn;
        pcs[n++] = c.Rip;
        fn = RtlLookupFunctionEntry(c.Rip, &image, NULL);
        if (fn) {
            void *handler_data = NULL;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fn, &c,
                             &handler_data, &frame, NULL);
        } else {
            /* a leaf: the return address is on top of the stack */
            if (IsBadReadPtr((void *)c.Rsp, 8))
                break;
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        }
        if (c.Rsp & 7)
            break;
    }
    return n;
}

static void sample_round(void)
{
    uint64_t pcs[MAX_FRAMES];
    int i, n, j, k;

    for (i = 0; i < s_nthr; i++) {
        if (SuspendThread(s_thr[i].h) == (DWORD)-1)
            continue;
        n = walk(s_thr[i].h, pcs);
        ResumeThread(s_thr[i].h);
        if (!n)
            continue;
        s_thr[i].samples++;
        count(s_self, pcs[0], (uint32_t)i);
        for (j = 0; j < n; j++) {
            int dup = 0;
            for (k = 0; k < j; k++)
                if (pcs[k] == pcs[j])
                    dup = 1;
            if (!dup)
                count(s_incl, pcs[j], (uint32_t)i);
        }
    }
    s_rounds++;
}

static int cmp_slot(const void *a, const void *b)
{
    const Slot *x = (const Slot *)a, *y = (const Slot *)b;
    if (x->thr != y->thr)
        return x->thr < y->thr ? -1 : 1;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

/* module+offset for an address, written into buf; for a module other than
 * the executable (whose names prof_report.py reads from its symbol table),
 * also the nearest export DbgHelp knows, as " module!name". */
static void where(uint64_t addr, char *buf, size_t n)
{
    HMODULE m = NULL, exe = GetModuleHandleA(NULL);
    char path[MAX_PATH] = "?", *base;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &m) && m) {
        GetModuleFileNameA(m, path, sizeof path);
        base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        if (m != exe) {
            char sbuf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *sym = (SYMBOL_INFO *)sbuf;
            DWORD64 disp = 0;
            memset(sbuf, 0, sizeof sbuf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) {
                snprintf(buf, n, "%s+0x%llx %s!%s", base,
                         (unsigned long long)(addr - (uint64_t)(uintptr_t)m), base, sym->Name);
                return;
            }
        }
        snprintf(buf, n, "%s+0x%llx", base,
                 (unsigned long long)(addr - (uint64_t)(uintptr_t)m));
    } else {
        snprintf(buf, n, "?+0x%llx", (unsigned long long)addr);
    }
}

static void write_table(FILE *f, const char *kind, Slot *t, int per_thread)
{
    uint32_t used = 0, i;
    Slot *v;
    int last = -1, shown = 0;

    for (i = 0; i < TABLE_SIZE; i++)
        if (t[i].addr)
            used++;
    v = (Slot *)malloc((size_t)used * sizeof *v);
    if (!v)
        return;
    used = 0;
    for (i = 0; i < TABLE_SIZE; i++)
        if (t[i].addr)
            v[used++] = t[i];
    qsort(v, used, sizeof *v, cmp_slot);
    for (i = 0; i < used; i++) {
        char w[MAX_PATH + 32];
        if ((int)v[i].thr != last) {
            last = (int)v[i].thr;
            shown = 0;
        }
        if (shown++ >= per_thread)
            continue;
        where(v[i].addr, w, sizeof w);
        fprintf(f, "%s %lu %u %s\n", kind, (unsigned long)s_thr[v[i].thr].tid, v[i].count, w);
    }
    free(v);
}

static void report(double secs)
{
    char path[1100];
    const char *out = getenv("THUG_PROF_OUT");
    FILE *f;
    int i;

    if (out && *out) {
        snprintf(path, sizeof path, "%s", out);
    } else {
        char dir[1000];
        if (!thug_launch_state_dir(dir, sizeof dir))
            snprintf(dir, sizeof dir, ".");
        snprintf(path, sizeof path, "%s\\prof.txt", dir);
    }
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[PROF] cannot write %s\n", path);
        return;
    }
    /* Addresses are module+offset; the executable's own image base is in its
     * file (the loader rewrites the copy in memory). */
    fprintf(f, "# THUG_PROF\n");
    fprintf(f, "window %.2f s, interval %d us, rounds %llu, lost %llu\n", secs,
            s_interval_us, (unsigned long long)s_rounds, (unsigned long long)s_lost);
    for (i = 0; i < s_nthr; i++) {
        char name[64];
        uint64_t cpu = s_thr[i].cpu1 > s_thr[i].cpu0 ? s_thr[i].cpu1 - s_thr[i].cpu0 : 0;
        WideCharToMultiByte(CP_UTF8, 0, s_thr[i].name, -1, name, sizeof name, NULL, NULL);
        fprintf(f, "thread %lu cpu_ms %.0f cpu_pct %.1f samples %llu name \"%s\"\n",
                (unsigned long)s_thr[i].tid, (double)cpu / 1e4,
                secs > 0 ? (double)cpu / 1e5 / secs : 0.0,
                (unsigned long long)s_thr[i].samples, name);
    }
    /* Per thread, every address up to a limit: prof_report.py adds a
     * function's addresses up, so the top few would leave most of the hot
     * functions' samples out. */
    write_table(f, "self", s_self, 4000);
    write_table(f, "incl", s_incl, 6000);
    fclose(f);
    fprintf(stderr, "[PROF] %llu rounds over %.1f s written to %s\n",
            (unsigned long long)s_rounds, secs, path);
}

static DWORD WINAPI sampler_main(LPVOID arg)
{
    LARGE_INTEGER freq, t0, t;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* HIGH_RESOLUTION */,
                                          TIMER_ALL_ACCESS);
    int i;

    (void)arg;
    if (!timer)
        timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    Sleep((DWORD)(s_start_s * 1000.0));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    refresh_threads();
    fprintf(stderr, "[PROF] sampling %d threads for %.1f s\n", s_nthr, s_dur_s);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    for (;;) {
        LARGE_INTEGER due;
        double el;
        QueryPerformanceCounter(&t);
        el = (double)(t.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (el >= s_dur_s)
            break;
        if ((s_rounds & 255) == 255)
            refresh_threads();
        sample_round();
        due.QuadPart = -(LONGLONG)s_interval_us * 10;
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            Sleep(1);
    }
    QueryPerformanceCounter(&t);
    for (i = 0; i < s_nthr; i++)
        s_thr[i].cpu1 = cpu_time(s_thr[i].h);
    /* Names for the system DLLs' addresses: their exports, from the modules
     * loaded now (main.c's crash handler set DbgHelp up at start). */
    SymRefreshModuleList(GetCurrentProcess());
    report((double)(t.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
    return 0;
}

void thug_prof_start(void)
{
    const char *e = getenv("THUG_PROF");
    HANDLE th;

    if (!e || !*e)
        return;
    sscanf(e, "%lf,%lf,%d", &s_start_s, &s_dur_s, &s_interval_us);
    if (s_interval_us < 100)
        s_interval_us = 100;
    s_self = (Slot *)calloc(TABLE_SIZE, sizeof *s_self);
    s_incl = (Slot *)calloc(TABLE_SIZE, sizeof *s_incl);
    if (!s_self || !s_incl)
        return;
    th = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
}

#else /* POSIX: perf does this better */

#include <stdio.h>
#include <stdlib.h>

void thug_prof_start(void)
{
    const char *e = getenv("THUG_PROF");
    if (e && *e)
        fprintf(stderr, "[PROF] THUG_PROF is Windows-only; on Linux use"
                        " perf record -g -p <pid>\n");
}

#endif
