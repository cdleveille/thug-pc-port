/**
 * thug_extract.c -- build/bin/thug-extract: the game's own start-up check and
 * disc unpacking (src/host/game_data.c), as a command for scripts/build.sh.
 * Lifting needs game/default.xbe before the game itself can be built.
 *
 *   thug-extract [game-dir]      make game-dir ready: unpack the disc image in
 *                                it if default.xbe is missing or not the
 *                                supported build (default: found as the game
 *                                finds it, next to the executable)
 *   thug-extract --list <image>  list a disc image's files
 *
 * Exit status 0 when the directory is ready.
 */
#include "host/game_data.h"

#include <stdio.h>
#include <string.h>

static int progress(uint64_t done, uint64_t total, void *user)
{
    (void)user;
    fprintf(stderr, "\r  unpacking: %3d%%  (%.2f of %.2f GB)", total ? (int)(done * 100 / total) : 100,
            (double)done / 1e9, (double)total / 1e9);
    if (done == total)
        fprintf(stderr, "\n");
    return 0;
}

int main(int argc, char **argv)
{
    ThugGameDir gd;
    char err[2048], searched[1200];
    ThugGameStatus st;

    if (argc == 3 && !strcmp(argv[1], "--list")) {
        if (thug_iso_list(argv[2], stdout, err, sizeof err) != 0) {
            fprintf(stderr, "thug-extract: %s\n", err);
            return 1;
        }
        return 0;
    }
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fprintf(stderr, "usage: thug-extract [game-dir]\n"
                        "       thug-extract --list <image.iso>\n");
        return 2;
    }
    if (argc == 2)
        thug_game_dir_scan(argv[1], &gd);
    else if (!thug_game_dir_locate(&gd, searched, sizeof searched))
        fprintf(stderr, "thug-extract: no game files found; looked in:\n%s", searched);

    st = thug_game_prepare(&gd, progress, NULL, err, sizeof err);
    switch (st) {
    case THUG_GAME_READY:
        printf("%s: default.xbe is the supported build (USA retail)\n", gd.dir);
        return 0;
    case THUG_GAME_EXTRACTED:
        printf("%s: unpacked from %s; default.xbe verified\n", gd.dir, gd.iso);
        return 0;
    default:
        fprintf(stderr, "thug-extract: %s\n", err);
        return 1;
    }
}
