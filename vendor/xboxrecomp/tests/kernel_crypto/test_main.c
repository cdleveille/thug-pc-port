/* The Xc* crypto services against published vectors: SHA-1 (FIPS 180-1),
 * HMAC-SHA1 (RFC 2202) and RC4. They are the toolkit's own code on every
 * host; nothing links a crypto library. */
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This crypto-only test must never dispatch guest code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static int failures;

static void check(const char *name, const UCHAR *got, const char *want_hex, size_t n)
{
    char hex[2 * 64 + 1];
    size_t i;

    for (i = 0; i < n; i++)
        snprintf(hex + 2 * i, 3, "%02x", got[i]);
    if (strcmp(hex, want_hex) != 0) {
        fprintf(stderr, "FAIL: %s: %s, expected %s\n", name, hex, want_hex);
        failures++;
    }
}

static void sha1(const char *name, const char *msg, ULONG repeat, const char *want)
{
    XBOX_SHA_CONTEXT ctx;
    UCHAR d[20];
    ULONG i;

    xbox_XcSHAInit(&ctx);
    for (i = 0; i < repeat; i++)
        xbox_XcSHAUpdate(&ctx, (const UCHAR *)msg, (ULONG)strlen(msg));
    xbox_XcSHAFinal(&ctx, d);
    check(name, d, want, sizeof d);
}

static void hmac(const char *name, const UCHAR *key, ULONG key_len,
                 const char *data, ULONG split, const char *want)
{
    UCHAR d[20];
    ULONG n = (ULONG)strlen(data);

    xbox_XcHMAC(key, key_len, (const UCHAR *)data, split,
                (const UCHAR *)data + split, n - split, d);
    check(name, d, want, sizeof d);
}

static void rc4(const char *name, const char *key, const char *plain, const char *want)
{
    XBOX_RC4_CONTEXT ctx;
    UCHAR buf[64];
    ULONG n = (ULONG)strlen(plain);

    memcpy(buf, plain, n);
    xbox_XcRC4Key(&ctx, (ULONG)strlen(key), (const UCHAR *)key);
    xbox_XcRC4Crypt(&ctx, n, buf);
    check(name, buf, want, n);
}

int main(void)
{
    UCHAR k20[20], k80[80];

    sha1("SHA-1 abc", "abc", 1, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1("SHA-1 448 bits", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
         "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    /* A million 'a's, fed 1000 at a time: the streaming path across blocks. */
    {
        char a[1001];
        memset(a, 'a', 1000);
        a[1000] = 0;
        sha1("SHA-1 million a", a, 1000, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    }

    memset(k20, 0x0b, sizeof k20);
    hmac("HMAC case 1", k20, sizeof k20, "Hi There", 3,
         "b617318655057264e28bc0b6fb378c8ef146be00");
    hmac("HMAC case 2", (const UCHAR *)"Jefe", 4, "what do ya want for nothing?", 0,
         "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    memset(k80, 0xaa, sizeof k80);
    hmac("HMAC case 6 (key longer than a block)", k80, sizeof k80,
         "Test Using Larger Than Block-Size Key - Hash Key First", 20,
         "aa4ae5e15272d00e95705637ce8a3b55ed402112");

    rc4("RC4 Key", "Key", "Plaintext", "bbf316e8d940af0ad3");
    rc4("RC4 Wiki", "Wiki", "pedia", "1021bf0420");
    rc4("RC4 Secret", "Secret", "Attack at dawn", "45a01f645fc35b383552544b9bf5");

    if (failures)
        return 1;
    printf("kernel_crypto: SHA-1, HMAC-SHA1 and RC4 match their published vectors\n");
    return 0;
}
