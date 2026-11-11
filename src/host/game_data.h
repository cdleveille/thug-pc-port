/**
 * game_data.h -- finding the game's files, and unpacking them from the
 * player's disc image when that is all there is.
 *
 * The game reads its files from a directory (the Xbox's D: drive): game/,
 * holding default.xbe and data/. A player may instead put their disc image
 * (.iso, a full Redump dump or a trimmed XISO) in that directory, or beside
 * the executable; it is then unpacked into game/ once (created if need be),
 * and default.xbe is checked against the one build this port supports.
 *
 * No SDL here: build/bin/thug-extract (tools/thug_extract.c) uses the same
 * code before the game itself can be built, since lifting needs default.xbe.
 */
#ifndef THUG_GAME_DATA_H
#define THUG_GAME_DATA_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-1 of the USA retail default.xbe: every address in this port is for it. */
#define THUG_XBE_SHA1 "2951de954c96a8b962ba1cde34548fa82ae668d7"

typedef struct {
    char dir[1024];       /* the game directory */
    char iso[1280];       /* a disc image in it, or "" */
    int  has_xbe;         /* dir/default.xbe exists */
    int  xbe_ok;          /* ... and is the supported build */
    char xbe_sha1[41];    /* its SHA-1, when it exists */
} ThugGameDir;

/* What dir holds. */
void thug_game_dir_scan(const char *dir, ThugGameDir *out);

/* Where the game's files are: $THUG_GAME_DIR if set, else the first of
 * <exe dir>/game, <exe dir>/../game, <exe dir>/../../game (a build tree's
 * build/bin/) and ./game that holds default.xbe or a disc image. A disc image
 * beside the executable counts as <exe dir>/game's when that folder holds
 * none (or does not exist yet: thug_game_prepare creates it). Returns 1 with
 * *out scanned, or 0 with *out describing the first candidate (where the
 * files should go) and `searched` listing every place looked, one per line. */
int thug_game_dir_locate(ThugGameDir *out, char *searched, size_t searched_n);

/* The directory this executable is in ("." if unknown). */
void thug_exe_dir(char *out, size_t n);

/* SHA-1 of a file as 40 lowercase hex digits. 0 on success. */
int thug_sha1_file(const char *path, char hex[41]);

/* Called every few megabytes while unpacking; return nonzero to stop. */
typedef int (*ThugProgressFn)(uint64_t done, uint64_t total, void *user);

typedef enum {
    THUG_GAME_READY = 0,      /* default.xbe present and supported */
    THUG_GAME_EXTRACTED,      /* unpacked from the disc image just now */
    THUG_GAME_MISSING,        /* neither files nor a disc image */
    THUG_GAME_WRONG_XBE,      /* default.xbe is another build, no image to fix it */
    THUG_GAME_BAD_IMAGE,      /* the image is not THUG (USA) or not readable */
    THUG_GAME_FAILED,         /* unpacking failed (space, I/O) */
    THUG_GAME_CANCELLED,
} ThugGameStatus;

/* Make gd->dir runnable: nothing to do if its default.xbe is the supported
 * build; otherwise unpack gd->iso into it (checking the image's default.xbe
 * first, and writing it last, so its presence means the unpack finished).
 * err gets a one-paragraph explanation for anything but READY/EXTRACTED.
 * gd is rescanned on success. */
ThugGameStatus thug_game_prepare(ThugGameDir *gd, ThugProgressFn fn, void *user,
                                 char *err, size_t err_n);

/* List an image's files (size, path) to out. 0 on success. */
int thug_iso_list(const char *iso, FILE *out, char *err, size_t err_n);

#ifdef __cplusplus
}
#endif

#endif /* THUG_GAME_DATA_H */
