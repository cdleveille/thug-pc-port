/**
 * game_data.c -- finding the game's files and unpacking the disc image. See
 * game_data.h.
 *
 * The disc reader follows the toolkit's tools/xiso/xdvdfs.py (format notes in
 * vendor/xboxrecomp/docs/formats/disc-image.md): probe the XDVDFS
 * descriptor at each known game-partition base, then read directories as
 * runs of fixed-layout entries, sector by sector, rather than following the
 * binary-tree links (a bad link would silently drop a subtree).
 */
#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64           /* the full disc image is ~7.5 GB */
#if !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700              /* fseeko, realpath, readlink, S_IFMT */
#endif
#endif

#include "game_data.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#define mkdir_one(p) mkdir((p), 0755)
#endif

/* ── SHA-1 (FIPS 180-4) ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t  buf[64];
    size_t   n;
} Sha1;

static uint32_t rol32(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

static void sha1_block(Sha1 *s, const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16
             | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
    s->len = 0;
    s->n = 0;
}

static void sha1_update(Sha1 *s, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    s->len += len;
    while (len) {
        size_t take = 64 - s->n < len ? 64 - s->n : len;
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n == 64) {
            sha1_block(s, s->buf);
            s->n = 0;
        }
    }
}

static void sha1_hex(Sha1 *s, char hex[41])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    int i;

    sha1_update(s, &pad, 1);
    while (s->n != 56)
        sha1_update(s, &zero, 1);
    for (i = 0; i < 8; i++)
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(s, lenb, 8);
    for (i = 0; i < 20; i++)
        sprintf(hex + 2 * i, "%02x", (unsigned)(s->h[i / 4] >> (24 - 8 * (i % 4))) & 0xFF);
    hex[40] = 0;
}

int thug_sha1_file(const char *path, char hex[41])
{
    FILE *f = fopen(path, "rb");
    static uint8_t buf[1 << 16];
    size_t n;
    Sha1 s;

    if (!f)
        return -1;
    sha1_init(&s);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        sha1_update(&s, buf, n);
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    sha1_hex(&s, hex);
    return 0;
}

/* ── Files and directories ──────────────────────────────────────────────── */

static int is_file(const char *path, uint64_t *size)
{
    struct stat st;
    if (stat(path, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG)
        return 0;
    if (size)
        *size = (uint64_t)st.st_size;
    return 1;
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

/* mkdir -p of path's parent directories (path itself is a file). */
static int make_parents(const char *path)
{
    char tmp[1400];
    size_t i;

    snprintf(tmp, sizeof tmp, "%s", path);
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            if (!is_dir(tmp) && mkdir_one(tmp) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = c;
        }
    }
    return 0;
}

static int has_suffix_ci(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf), i;
    if (n < m)
        return 0;
    for (i = 0; i < m; i++)
        if (tolower((unsigned char)s[n - m + i]) != tolower((unsigned char)suf[i]))
            return 0;
    return 1;
}

/* The first disc image in dir, alphabetically. */
static void find_image(const char *dir, char *out, size_t n)
{
    char best[512] = "";

    out[0] = 0;
#if defined(_WIN32)
    {
        WIN32_FIND_DATAA fd;
        char pat[1100];
        HANDLE h;
        snprintf(pat, sizeof pat, "%s\\*", dir);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    && (has_suffix_ci(fd.cFileName, ".iso") || has_suffix_ci(fd.cFileName, ".xiso"))
                    && (!best[0] || strcmp(fd.cFileName, best) < 0))
                snprintf(best, sizeof best, "%s", fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        if (!d)
            return;
        while ((e = readdir(d)) != NULL) {
            char full[1100];
            snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            if ((has_suffix_ci(e->d_name, ".iso") || has_suffix_ci(e->d_name, ".xiso"))
                    && is_file(full, NULL) && (!best[0] || strcmp(e->d_name, best) < 0))
                snprintf(best, sizeof best, "%s", e->d_name);
        }
        closedir(d);
    }
#endif
    if (best[0])
        snprintf(out, n, "%s/%s", dir, best);
}

void thug_game_dir_scan(const char *dir, ThugGameDir *gd)
{
    char xbe[1100], d[sizeof gd->dir];

    snprintf(d, sizeof d, "%s", dir);  /* dir may be gd->dir itself */
    memset(gd, 0, sizeof *gd);
    snprintf(gd->dir, sizeof gd->dir, "%s", d);
    dir = gd->dir;
    find_image(dir, gd->iso, sizeof gd->iso);
    snprintf(xbe, sizeof xbe, "%s/default.xbe", dir);
    gd->has_xbe = is_file(xbe, NULL);
    if (gd->has_xbe && thug_sha1_file(xbe, gd->xbe_sha1) == 0)
        gd->xbe_ok = strcmp(gd->xbe_sha1, THUG_XBE_SHA1) == 0;
}

void thug_exe_dir(char *out, size_t n)
{
    char path[1024];
    char *slash;

    snprintf(out, n, ".");
#if defined(_WIN32)
    {
        DWORD len = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
        if (len == 0 || len >= sizeof path)
            return;
        path[len] = 0;
    }
#elif defined(__linux__)
    {
        ssize_t len = readlink("/proc/self/exe", path, sizeof path - 1);
        if (len <= 0)
            return;
        path[len] = 0;
    }
#else
    return;
#endif
    slash = strrchr(path, '/');
#if defined(_WIN32)
    if (!slash || (strrchr(path, '\\') && strrchr(path, '\\') > slash))
        slash = strrchr(path, '\\');
#endif
    if (!slash)
        return;
    *slash = 0;
    snprintf(out, n, "%s", path);
}

/* path with "..", "." and links resolved, when it exists. */
static void tidy_path(const char *path, char *out, size_t n)
{
#if defined(_WIN32)
    if (!_fullpath(out, path, n))
        snprintf(out, n, "%s", path);
#else
    char buf[4096];
    if (realpath(path, buf))
        snprintf(out, n, "%s", buf);
    else
        snprintf(out, n, "%s", path);
#endif
}

int thug_game_dir_locate(ThugGameDir *out, char *searched, size_t searched_n)
{
    const char *env = getenv("THUG_GAME_DIR");
    char exe[1024], cand[5][1100], first[1100] = "";
    size_t used = 0;
    int i, nc = 0;

    if (searched && searched_n)
        searched[0] = 0;
    if (env && *env) {
        tidy_path(env, cand[0], sizeof cand[0]);
        nc = 1;
    } else {
        thug_exe_dir(exe, sizeof exe);
        snprintf(cand[nc++], sizeof cand[0], "%s/game", exe);
        snprintf(cand[nc++], sizeof cand[0], "%s/../game", exe);
        snprintf(cand[nc++], sizeof cand[0], "%s/../../game", exe);
        snprintf(cand[nc++], sizeof cand[0], "game");
        for (i = 0; i < nc; i++) {
            char t[1100];
            tidy_path(cand[i], t, sizeof t);
            memcpy(cand[i], t, sizeof t);
        }
    }
    for (i = 0; i < nc; i++) {
        int j, dup = 0;
        for (j = 0; j < i; j++)
            dup |= strcmp(cand[i], cand[j]) == 0;
        if (dup)
            continue;
        if (!first[0])
            snprintf(first, sizeof first, "%s", cand[i]);
        if (searched && used + strlen(cand[i]) + 4 < searched_n)
            used += (size_t)snprintf(searched + used, searched_n - used, "  %s\n", cand[i]);
        if (is_dir(cand[i])) {
            thug_game_dir_scan(cand[i], out);
            if (out->has_xbe || out->iso[0])
                return 1;
        }
    }
    memset(out, 0, sizeof *out);
    snprintf(out->dir, sizeof out->dir, "%s", first);
    return 0;
}

/* ── XDVDFS ─────────────────────────────────────────────────────────────── */

#define SECTOR 2048u
static const char k_magic[] = "MICROSOFT*XBOX*MEDIA";

/* A disc image may be a bare game partition or carry the DVD-video partition
 * in front of it (a Redump dump: game partition at 0x18300000). */
static const uint64_t k_bases[] = {
    0x00000000ull, 0x0000FD90ull, 0x00030600ull, 0x0FD90000ull, 0x18300000ull,
};

typedef struct {
    FILE    *f;
    uint64_t base;
    uint32_t root_sector, root_size;
} Iso;

typedef struct {
    char     path[512];       /* relative, '/'-separated */
    uint32_t sector, size;
} IsoFile;

typedef struct {
    IsoFile *v;
    size_t   n, cap;
    uint64_t total;
} IsoList;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int iso_seek(FILE *f, uint64_t off)
{
#if defined(_WIN32)
    return _fseeki64(f, (__int64)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

static int iso_read(Iso *iso, uint64_t off, void *buf, size_t n)
{
    return iso_seek(iso->f, iso->base + off) == 0 && fread(buf, 1, n, iso->f) == n ? 0 : -1;
}

static int iso_open(Iso *iso, const char *path, char *err, size_t err_n)
{
    uint8_t desc[SECTOR];
    size_t i;

    memset(iso, 0, sizeof *iso);
    iso->f = fopen(path, "rb");
    if (!iso->f) {
        snprintf(err, err_n, "Cannot open the disc image %s: %s.", path, strerror(errno));
        return -1;
    }
    for (i = 0; i < sizeof k_bases / sizeof k_bases[0]; i++) {
        iso->base = k_bases[i];
        if (iso_read(iso, 32ull * SECTOR, desc, sizeof desc) == 0
                && memcmp(desc, k_magic, sizeof k_magic - 1) == 0) {
            iso->root_sector = le32(desc + 0x14);
            iso->root_size = le32(desc + 0x18);
            return 0;
        }
    }
    fclose(iso->f);
    iso->f = NULL;
    snprintf(err, err_n, "%s is not an Xbox disc image (no XDVDFS file system "
             "found in it).", path);
    return -1;
}

static void iso_close(Iso *iso)
{
    if (iso->f)
        fclose(iso->f);
    iso->f = NULL;
}

/* A name the image may hold: printable ASCII, no separators, not . or .. */
static int name_ok(const char *s, size_t n)
{
    size_t i;
    if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] < 0x20 || s[i] > 0x7E || s[i] == '/' || s[i] == '\\' || s[i] == ':')
            return 0;
    return 1;
}

static int list_add(IsoList *l, const char *path, uint32_t sector, uint32_t size)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 1024;
        IsoFile *v = (IsoFile *)realloc(l->v, cap * sizeof *v);
        if (!v)
            return -1;
        l->v = v;
        l->cap = cap;
    }
    snprintf(l->v[l->n].path, sizeof l->v[l->n].path, "%s", path);
    l->v[l->n].sector = sector;
    l->v[l->n].size = size;
    l->n++;
    l->total += size;
    return 0;
}

/* Every file under one directory, recursively. */
static int iso_walk(Iso *iso, uint32_t sector, uint32_t size, const char *prefix,
                    int depth, IsoList *l, char *err, size_t err_n)
{
    uint8_t *data;
    uint32_t base;

    if (depth > 32 || size > (16u << 20) || l->n > 200000) {
        snprintf(err, err_n, "The disc image's directory tree is malformed.");
        return -1;
    }
    if (size == 0)
        return 0;
    data = (uint8_t *)malloc(size);
    if (!data || iso_read(iso, (uint64_t)sector * SECTOR, data, size) != 0) {
        free(data);
        snprintf(err, err_n, "The disc image is truncated or unreadable.");
        return -1;
    }
    for (base = 0; base < size; base += SECTOR) {
        uint32_t end = size - base < SECTOR ? size - base : SECTOR, off = 0;
        const uint8_t *chunk = data + base;
        while (off + 0x0E <= end) {
            uint16_t left = (uint16_t)(chunk[off] | chunk[off + 1] << 8);
            uint16_t right = (uint16_t)(chunk[off + 2] | chunk[off + 3] << 8);
            uint32_t esec = le32(chunk + off + 4), esize = le32(chunk + off + 8);
            uint8_t attr = chunk[off + 12], nlen = chunk[off + 13];
            char path[512];
            /* 0xFFFF/0xFFFF padding or an empty name ends this sector's run. */
            if ((left == 0xFFFF && right == 0xFFFF) || nlen == 0 || off + 0x0E + nlen > end)
                break;
            if (!name_ok((const char *)chunk + off + 0x0E, nlen)) {
                free(data);
                snprintf(err, err_n, "The disc image holds an unexpected file name.");
                return -1;
            }
            snprintf(path, sizeof path, "%s%s%.*s", prefix, *prefix ? "/" : "",
                     (int)nlen, (const char *)chunk + off + 0x0E);
            if (attr & 0x10) {
                if (iso_walk(iso, esec, esize, path, depth + 1, l, err, err_n) != 0) {
                    free(data);
                    return -1;
                }
            } else if (list_add(l, path, esec, esize) != 0) {
                free(data);
                snprintf(err, err_n, "Out of memory reading the disc image.");
                return -1;
            }
            off += (0x0E + nlen + 3u) & ~3u;
        }
    }
    free(data);
    return 0;
}

static int iso_files(Iso *iso, IsoList *l, char *err, size_t err_n)
{
    memset(l, 0, sizeof *l);
    return iso_walk(iso, iso->root_sector, iso->root_size, "", 0, l, err, err_n);
}

int thug_iso_list(const char *path, FILE *out, char *err, size_t err_n)
{
    Iso iso;
    IsoList l;
    size_t i;

    if (iso_open(&iso, path, err, err_n) != 0)
        return -1;
    if (iso_files(&iso, &l, err, err_n) != 0) {
        iso_close(&iso);
        free(l.v);
        return -1;
    }
    for (i = 0; i < l.n; i++)
        fprintf(out, "%12u  %s\n", l.v[i].size, l.v[i].path);
    fprintf(out, "%zu files, %llu bytes; game partition at 0x%llX\n", l.n,
            (unsigned long long)l.total, (unsigned long long)iso.base);
    iso_close(&iso);
    free(l.v);
    return 0;
}

/* ── Unpacking ──────────────────────────────────────────────────────────── */

#define COPY_CHUNK (4u << 20)

typedef struct {
    ThugProgressFn fn;
    void          *user;
    uint64_t       done, total, last;
    int            cancelled;
} Progress;

static int progress_step(Progress *p, uint64_t n)
{
    p->done += n;
    if (p->fn && (p->done - p->last >= (8u << 20) || p->done == p->total)) {
        p->last = p->done;
        if (p->fn(p->done, p->total, p->user))
            p->cancelled = 1;
    }
    return p->cancelled;
}

/* One file out of the image, counting it towards the progress. */
static int copy_out(Iso *iso, const IsoFile *e, const char *dest, uint8_t *buf,
                    Progress *p, char *err, size_t err_n)
{
    FILE *o;
    uint64_t left = e->size, off = (uint64_t)e->sector * SECTOR;

    if (make_parents(dest) != 0) {
        snprintf(err, err_n, "Cannot create the folder for %s: %s.", dest, strerror(errno));
        return -1;
    }
    o = fopen(dest, "wb");
    if (!o) {
        snprintf(err, err_n, "Cannot write %s: %s.", dest, strerror(errno));
        return -1;
    }
    while (left > 0) {
        size_t n = left > COPY_CHUNK ? COPY_CHUNK : (size_t)left;
        if (iso_read(iso, off, buf, n) != 0) {
            fclose(o);
            snprintf(err, err_n, "The disc image is truncated (reading %s).", e->path);
            return -1;
        }
        if (fwrite(buf, 1, n, o) != n) {
            int en = errno;
            fclose(o);
            snprintf(err, err_n, "Cannot write %s: %s.", dest, strerror(en));
            return -1;
        }
        off += n;
        left -= n;
        if (progress_step(p, n)) {
            fclose(o);
            return -1;
        }
    }
    if (fclose(o) != 0) {
        snprintf(err, err_n, "Cannot write %s: %s.", dest, strerror(errno));
        return -1;
    }
    return 0;
}

static uint64_t free_bytes(const char *dir)
{
#if defined(_WIN32)
    ULARGE_INTEGER avail;
    if (GetDiskFreeSpaceExA(dir, &avail, NULL, NULL))
        return avail.QuadPart;
    return UINT64_MAX;
#else
    struct statvfs sv;
    if (statvfs(dir, &sv) == 0)
        return (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
    return UINT64_MAX;
#endif
}

static int is_root_xbe(const IsoFile *e)
{
    return strchr(e->path, '/') == NULL && has_suffix_ci(e->path, "default.xbe")
           && strlen(e->path) == strlen("default.xbe");
}

ThugGameStatus thug_game_prepare(ThugGameDir *gd, ThugProgressFn fn, void *user,
                                 char *err, size_t err_n)
{
    Iso iso;
    IsoList l = { 0 };
    Progress p = { 0 };
    const IsoFile *xbe = NULL;
    uint8_t *buf = NULL;
    uint64_t need = 0, have;
    ThugGameStatus st = THUG_GAME_FAILED;
    char path[1700], tmp[1720], hex[41];
    size_t i;

    if (err_n)
        err[0] = 0;
    if (gd->has_xbe && gd->xbe_ok)
        return THUG_GAME_READY;
    if (!gd->iso[0]) {
        if (gd->has_xbe) {
            snprintf(err, err_n,
                     "%s/default.xbe is not the version this port supports, the USA "
                     "retail release.\n\nIts SHA-1 is %s;\nthe supported one is %s.\n\n"
                     "Put your USA disc image (.iso) in %s and it will be unpacked again.",
                     gd->dir, gd->xbe_sha1, THUG_XBE_SHA1, gd->dir);
            return THUG_GAME_WRONG_XBE;
        }
        snprintf(err, err_n,
                 "Tony Hawk's Underground needs your game files.\n\n"
                 "Put your disc image (a .iso of the USA Xbox release) or the files "
                 "unpacked from it (default.xbe and the data folder) in:\n\n%s", gd->dir);
        return THUG_GAME_MISSING;
    }

    /* The image: an Xbox disc, holding the supported default.xbe. */
    if (iso_open(&iso, gd->iso, err, err_n) != 0)
        return THUG_GAME_BAD_IMAGE;
    if (iso_files(&iso, &l, err, err_n) != 0) {
        st = THUG_GAME_BAD_IMAGE;
        goto out;
    }
    for (i = 0; i < l.n; i++)
        if (is_root_xbe(&l.v[i]))
            xbe = &l.v[i];
    buf = (uint8_t *)malloc(COPY_CHUNK);
    if (!buf) {
        snprintf(err, err_n, "Out of memory.");
        goto out;
    }
    if (!xbe) {
        snprintf(err, err_n, "%s is an Xbox disc image, but it has no default.xbe.", gd->iso);
        st = THUG_GAME_BAD_IMAGE;
        goto out;
    }
    {
        Sha1 s;
        uint64_t left = xbe->size, off = (uint64_t)xbe->sector * SECTOR;
        sha1_init(&s);
        while (left > 0) {
            size_t n = left > COPY_CHUNK ? COPY_CHUNK : (size_t)left;
            if (iso_read(&iso, off, buf, n) != 0) {
                snprintf(err, err_n, "The disc image %s is truncated.", gd->iso);
                st = THUG_GAME_BAD_IMAGE;
                goto out;
            }
            sha1_update(&s, buf, n);
            off += n;
            left -= n;
        }
        sha1_hex(&s, hex);
        if (strcmp(hex, THUG_XBE_SHA1) != 0) {
            snprintf(err, err_n,
                     "%s is not the version of Tony Hawk's Underground this port "
                     "supports, the USA retail release.\n\nIts default.xbe has SHA-1 %s;\n"
                     "the supported one is %s.", gd->iso, hex, THUG_XBE_SHA1);
            st = THUG_GAME_BAD_IMAGE;
            goto out;
        }
    }

    /* Room for what is not there already (a re-run after an interrupted
     * unpack only writes the difference). */
    for (i = 0; i < l.n; i++) {
        uint64_t sz = 0;
        snprintf(path, sizeof path, "%s/%s", gd->dir, l.v[i].path);
        if (!is_file(path, &sz) || sz != l.v[i].size)
            need += l.v[i].size;
    }
    have = free_bytes(gd->dir);
    if (have != UINT64_MAX && have < need + (64ull << 20)) {
        snprintf(err, err_n,
                 "Not enough free space to unpack the game: it needs %.1f GB in %s, "
                 "and %.1f GB is free.", (double)need / 1e9, gd->dir, (double)have / 1e9);
        goto out;
    }

    /* Everything but default.xbe, then default.xbe under a temporary name
     * and renamed into place: its presence means the rest is there. */
    memset(&p, 0, sizeof p);
    p.fn = fn;
    p.user = user;
    p.total = l.total;
    for (i = 0; i < l.n; i++) {
        if (&l.v[i] == xbe)
            continue;
        snprintf(path, sizeof path, "%s/%s", gd->dir, l.v[i].path);
        if (copy_out(&iso, &l.v[i], path, buf, &p, err, err_n) != 0) {
            st = p.cancelled ? THUG_GAME_CANCELLED : THUG_GAME_FAILED;
            goto out;
        }
    }
    snprintf(path, sizeof path, "%s/default.xbe", gd->dir);
    snprintf(tmp, sizeof tmp, "%s/default.xbe.part", gd->dir);
    if (copy_out(&iso, xbe, tmp, buf, &p, err, err_n) != 0) {
        st = p.cancelled ? THUG_GAME_CANCELLED : THUG_GAME_FAILED;
        remove(tmp);
        goto out;
    }
#if defined(_WIN32)
    remove(path);
#endif
    if (rename(tmp, path) != 0) {
        snprintf(err, err_n, "Cannot write %s: %s.", path, strerror(errno));
        goto out;
    }
    thug_game_dir_scan(gd->dir, gd);
    if (!gd->xbe_ok) {
        snprintf(err, err_n, "The unpacked default.xbe does not match the disc image "
                 "(SHA-1 %s). Is the disk failing?", gd->xbe_sha1);
        goto out;
    }
    st = THUG_GAME_EXTRACTED;
out:
    if (st == THUG_GAME_CANCELLED)
        snprintf(err, err_n, "Unpacking the game was cancelled. It starts again the "
                 "next time the game is launched.");
    free(buf);
    free(l.v);
    iso_close(&iso);
    return st;
}
