/**
 * prof_sampler.h -- a sampling profiler built into the game, for Windows
 * (Linux has perf: `perf record -g -p <pid>` gives more).
 *
 *   THUG_PROF=<start s>[,<seconds>[,<interval us>]]
 *
 * From <start> seconds after launch, for <seconds> (default 10), every
 * <interval> microseconds (default 1000) each thread of the process is
 * suspended for a moment, its stack walked with the unwind tables, and
 * resumed. The report goes to THUG_PROF_OUT (default prof.txt in the state
 * folder): per thread, its CPU time over the window and its samples; then,
 * per thread, the hottest leaf addresses (self) and the hottest addresses
 * anywhere on the stack (inclusive), as module+offset. Addresses in thug.exe
 * are relative to its image base, which the report states;
 * scripts/prof_report.py names them from the executable's symbols.
 */
#ifndef THUG_PROF_SAMPLER_H
#define THUG_PROF_SAMPLER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start the sampler thread if THUG_PROF is set. Any time after main starts. */
void thug_prof_start(void);

#ifdef __cplusplus
}
#endif

#endif /* THUG_PROF_SAMPLER_H */
