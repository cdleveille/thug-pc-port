/*
 * mmio_decode_x86_64 - does the x86-64 trapped-MMIO decoder service the
 * instructions clang emits against a device register, through a real fault:
 * a PROT_NONE page, SIGSEGV, the ucontext, and a resume past the instruction.
 *
 * The ALU and TEST forms are the point. A lifted `MEM32(reg) & imm` compiles
 * to `test dword [mem], imm32`, and the branch after it reads the flags the
 * decoder wrote into the saved RFLAGS -- so a decoder that services the read
 * but leaves ZF stale is silently wrong, and one that does not know the form
 * crashes. XDK DirectSound polls the APU exactly that way (Tony Hawk's
 * Underground: F7 84 03 F0 1F FE FF 18 00 00 00).
 *
 * Skips (exit 77) on hosts that are not x86-64 Linux.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#if defined(__x86_64__) && defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include "mmio_decode_x86_64.h"

static int failures;
#define CHECK_U64(name, got, want) \
    do { uint64_t g_=(uint64_t)(got), w_=(uint64_t)(want); \
         if (g_ != w_) { printf("FAIL: %s (got 0x%llX, want 0x%llX)\n", name, \
                   (unsigned long long)g_, (unsigned long long)w_); failures++; } } while (0)

#define WIN 0x8000u
static uint8_t *win;                 /* PROT_NONE */
static uint8_t model[WIN];           /* the device behind it */
static int traps, unknown, reads, writes;

static uint64_t rd(void *d, uint32_t off, int size)
{ uint64_t v = 0; reads++; memcpy(&v, (uint8_t *)d + off, size); return v; }
static void wr(void *d, uint32_t off, uint64_t v, int size)
{ writes++; memcpy((uint8_t *)d + off, &v, size); }

static void handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t f = (uintptr_t)si->si_addr;

    if (f >= (uintptr_t)win && f < (uintptr_t)win + WIN) {
        traps++;
        if (mmio_emulate_x86_64((ucontext_t *)ucv,
                                (uint32_t)(f - (uintptr_t)win), model, rd, wr))
            return;
        unknown++;
    }
    /* An undecoded form cannot be stepped safely without its length: die
     * with a message naming the case rather than looping on the fault. */
    printf("FAIL: undecoded fault at %p -- aborting\n", (void *)f);
    fflush(stdout);
    _exit(1);
}

static void set32(uint32_t off, uint32_t v) { memcpy(model + off, &v, 4); }
static uint32_t get32(uint32_t off) { uint32_t v; memcpy(&v, model + off, 4); return v; }

static void reset_counts(void) { reads = writes = 0; }

int main(void)
{
    struct sigaction sa;
    unsigned char z, c, s, o;
    uint32_t r32;

    win = mmap(NULL, WIN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (win == MAP_FAILED) { perror("mmap"); return 1; }

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    /* 1. THUG's exact instruction: test dword [rbx+rax*1-0x1E010], 0x18.
     *    The encoding needs base=rbx, index=rax, disp32, imm32. */
    {
        uintptr_t base = (uintptr_t)win + 0x2000 + 0x1E010;
        uintptr_t index = 0;

        set32(0x2000, 0x10);                         /* bit 4 set: ZF=0 */
        reset_counts();
        __asm__ volatile(".byte 0xF7, 0x84, 0x03, 0xF0, 0x1F, 0xFE, 0xFF, 0x18, 0x00, 0x00, 0x00\n\t"
                         "setz %0"
                         : "=r"(z) : "b"(base), "a"(index) : "cc", "memory");
        CHECK_U64("THUG test [rbx+rax-0x1E010],0x18: ZF clear", z, 0);
        CHECK_U64("THUG test: one read, no write", (uint64_t)reads << 8 | writes, 1u << 8);

        set32(0x2000, 0x01);                         /* no bit 3/4: ZF=1 */
        __asm__ volatile(".byte 0xF7, 0x84, 0x03, 0xF0, 0x1F, 0xFE, 0xFF, 0x18, 0x00, 0x00, 0x00\n\t"
                         "setz %0"
                         : "=r"(z) : "b"(base), "a"(index) : "cc", "memory");
        CHECK_U64("THUG test: ZF set", z, 1);
    }

    /* 2. test byte [mem], imm8 (F6 /0) */
    {
        uint8_t *p = win + 0x100;

        model[0x100] = 0x80;
        __asm__ volatile("testb $0x80, (%2)\n\tsetz %0\n\tsets %1"
                         : "=r"(z), "=r"(s) : "r"(p) : "cc", "memory");
        CHECK_U64("test byte imm8: ZF", z, 0);
        CHECK_U64("test byte imm8: SF", s, 1);
    }

    /* 3. test [mem], reg (85) */
    {
        uint32_t mask = 0x00F0;
        uint8_t *p = win + 0x200;

        set32(0x200, 0x0F0F);
        __asm__ volatile("testl %2, (%1)\n\tsetz %0"
                         : "=r"(z) : "r"(p), "r"(mask) : "cc", "memory");
        CHECK_U64("test [mem],reg: ZF", z, 1);
    }

    /* 4. cmp dword [mem], imm8 (83 /7): 5 - 7 borrows (CF) and is negative. */
    {
        uint8_t *p = win + 0x300;

        set32(0x300, 5);
        reset_counts();
        __asm__ volatile("cmpl $7, (%2)\n\tsetb %0\n\tsetz %1"
                         : "=r"(c), "=r"(z) : "r"(p) : "cc", "memory");
        CHECK_U64("cmp [mem],imm8: CF", c, 1);
        CHECK_U64("cmp [mem],imm8: ZF", z, 0);
        CHECK_U64("cmp: one read, no write", (uint64_t)reads << 8 | writes, 1u << 8);
        CHECK_U64("cmp leaves the register", get32(0x300), 5);
    }

    /* 5. cmp signed overflow: 0x80000000 - 1 sets OF, clears SF. */
    {
        uint8_t *p = win + 0x310;

        set32(0x310, 0x80000000u);
        __asm__ volatile("cmpl $1, (%2)\n\tseto %0\n\tsets %1"
                         : "=r"(o), "=r"(s) : "r"(p) : "cc", "memory");
        CHECK_U64("cmp INT_MIN,1: OF", o, 1);
        CHECK_U64("cmp INT_MIN,1: SF", s, 0);
    }

    /* 6. or dword [mem], imm32 (81 /1): read-modify-write. */
    {
        uint8_t *p = win + 0x400;

        set32(0x400, 0x0000F000);
        reset_counts();
        __asm__ volatile("orl $0x12340000, (%0)" :: "r"(p) : "cc", "memory");
        CHECK_U64("or [mem],imm32", get32(0x400), 0x1234F000);
        CHECK_U64("or: one read, one write", (uint64_t)reads << 8 | writes, 1u << 8 | 1);
    }

    /* 7. and eax, [mem] (23): register destination. */
    {
        uint8_t *p = win + 0x500;

        r32 = 0xFFFF00FF;
        set32(0x500, 0x0F0F0F0F);
        __asm__ volatile("andl (%1), %0" : "+r"(r32) : "r"(p) : "cc", "memory");
        CHECK_U64("and reg,[mem]", r32, 0x0F0F000F);
    }

    /* 8. add [mem], reg (01) with carry out. */
    {
        uint8_t *p = win + 0x600;
        uint32_t one = 1;

        set32(0x600, 0xFFFFFFFFu);
        __asm__ volatile("addl %2, (%1)\n\tsetc %0"
                         : "=r"(c) : "r"(p), "r"(one) : "cc", "memory");
        CHECK_U64("add [mem],reg: result", get32(0x600), 0);
        CHECK_U64("add [mem],reg: CF", c, 1);
    }

    /* 9. sub reg, [mem] (2B) keeps the upper half zeroed (32-bit write). */
    {
        uint8_t *p = win + 0x700;
        uint64_t r64 = 0xDEADBEEF00000010ull;

        set32(0x700, 0x4);
        __asm__ volatile("subl (%1), %k0" : "+r"(r64) : "r"(p) : "cc", "memory");
        CHECK_U64("sub reg32,[mem] zero-extends", r64, 0xC);
    }

    /* 10. xor byte [mem], imm8 (80 /6). */
    model[0x800] = 0xAA;
    {
        uint8_t *p = win + 0x800;
        __asm__ volatile("xorb $0xFF, (%0)" :: "r"(p) : "cc", "memory");
    }
    CHECK_U64("xor byte [mem],imm8", model[0x800], 0x55);

    /* 11. The existing MOV path still works: load and store. */
    {
        uint8_t *p = win + 0x900;

        set32(0x900, 0xCAFEF00D);
        __asm__ volatile("movl (%1), %0" : "=r"(r32) : "r"(p) : "memory");
        CHECK_U64("mov load", r32, 0xCAFEF00D);
        r32 = 0x12345678;
        __asm__ volatile("movl %0, 4(%1)" :: "r"(r32), "r"(p) : "memory");
        CHECK_U64("mov store", get32(0x904), 0x12345678);
    }

    CHECK_U64("no undecoded traps", unknown, 0);

    if (failures) {
        printf("%d failure(s), %d trap(s)\n", failures, traps);
        return 1;
    }
    printf("mmio_decode_x86_64: all checks passed (%d traps)\n", traps);
    return 0;
}

#else
int main(void)
{
    printf("mmio_decode_x86_64: skipped (not x86-64 Linux)\n");
    return 77;
}
#endif
