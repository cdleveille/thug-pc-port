/* apu_volume: the host's master volume (xbox_ApuSetVolume) and the gain the
 * output path applies to each period (apu_apply_gain).
 *
 *   - the setting is clamped to 0..1 and defaults to 1;
 *   - unity gain leaves samples bit-identical;
 *   - a fixed gain scales and rounds to nearest, both signs;
 *   - zero is silence;
 *   - a change ramps across the period: no step at its start, and the last
 *     sample is at (nearly) the new gain;
 *   - nothing overflows at the extremes. */
#include "apu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

int main(void)
{
    int16_t s[512];
    int i, ok;

    CHECK("default is unity", xbox_ApuGetVolume() == 1.0f);
    xbox_ApuSetVolume(-1.0f);
    CHECK("clamped at 0", xbox_ApuGetVolume() == 0.0f);
    xbox_ApuSetVolume(3.0f);
    CHECK("clamped at 1", xbox_ApuGetVolume() == 1.0f);
    xbox_ApuSetVolume(0.25f);
    CHECK("kept", xbox_ApuGetVolume() == 0.25f);

    for (i = 0; i < 512; i++)
        s[i] = (int16_t)((i * 977) - 30000);
    apu_apply_gain(s, 512, 1.0f, 1.0f);
    for (i = 0, ok = 1; i < 512; i++)
        ok &= s[i] == (int16_t)((i * 977) - 30000);
    CHECK("unity is bit-identical", ok);

    s[0] = 1001; s[1] = -1001; s[2] = 3; s[3] = -3;
    apu_apply_gain(s, 4, 0.5f, 0.5f);
    CHECK("half, positive, rounded", s[0] == 501);
    CHECK("half, negative, rounded", s[1] == -501);
    CHECK("half of 3", s[2] == 2);
    CHECK("half of -3", s[3] == -2);

    for (i = 0; i < 512; i++)
        s[i] = 20000;
    apu_apply_gain(s, 512, 0.0f, 0.0f);
    for (i = 0, ok = 1; i < 512; i++)
        ok &= s[i] == 0;
    CHECK("zero is silence", ok);

    for (i = 0; i < 512; i++)
        s[i] = 20000;
    apu_apply_gain(s, 512, 1.0f, 0.0f);
    CHECK("ramp starts at the old gain", s[0] == 20000);
    CHECK("ramp ends near the new gain", s[511] < 100);
    for (i = 1, ok = 1; i < 512; i++)
        ok &= s[i] <= s[i - 1] && s[i - 1] - s[i] < 100;
    CHECK("ramp is smooth and monotonic", ok);

    s[0] = 32767; s[1] = -32768;
    apu_apply_gain(s, 2, 1.0f, 1.0f);
    CHECK("extremes kept", s[0] == 32767 && s[1] == -32768);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("apu_volume: all checks passed\n");
    return 0;
}
