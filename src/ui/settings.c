/**
 * settings.c -- the player's settings file. See settings.h.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L        /* setenv, mkdir */
#endif
#include "settings.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#define setenv_once(n, v) ((void)(getenv(n) ? 0 : _putenv_s((n), (v))))
#else
#include <sys/stat.h>
#define mkdir_one(p) mkdir((p), 0755)
#define setenv_once(n, v) ((void)setenv((n), (v), 0))
#endif

/* The owner's choice (2026-11-11): 1920x1080 at 16:9, vsync, a 60 fps cap,
 * 16x anisotropic filtering and 4x MSAA, everything else as the game was. */
#define THUG_SETTINGS_DEFAULTS {                                  \
    2.25f,  /* res_scale: 1080 lines (1920x1080 at 16:9) */         \
    1,      /* vsync */                                             \
    60,     /* fps_limit */                                         \
    16,     /* aniso */                                             \
    16.0f / 9.0f,  /* aspect */                                     \
    0,      /* fullscreen */                                        \
    0,      /* show_fps */                                          \
    0,      /* vblank_timing */                                     \
    4,      /* msaa: 4x */                                          \
    1.0f,   /* brightness */                                        \
    100,    /* volume */                                            \
    0,      /* mute_unfocused */                                    \
    100,    /* rumble */                                            \
    0, 0,   /* window size: the built-in default */                 \
    THUG_WINDOW_POS_UNSET, THUG_WINDOW_POS_UNSET,                   \
    0,      /* window_maximized */                                  \
}

volatile ThugSettings g_thug_settings = THUG_SETTINGS_DEFAULTS;

/* xinput_xbox.h and apu.h are the toolkit's; only these two are needed. */
void xbox_InputSetRumbleScale(float scale);
void xbox_ApuSetVolume(float gain);

static volatile int s_focused = 1;

void thug_settings_reset(void)
{
    static const ThugSettings d = THUG_SETTINGS_DEFAULTS;
    ThugSettings keep;
    memcpy(&keep, (const void *)&g_thug_settings, sizeof keep);
    memcpy((void *)&g_thug_settings, &d, sizeof d);
    g_thug_settings.fullscreen = keep.fullscreen;
    g_thug_settings.window_w = keep.window_w;
    g_thug_settings.window_h = keep.window_h;
    g_thug_settings.window_x = keep.window_x;
    g_thug_settings.window_y = keep.window_y;
    g_thug_settings.window_maximized = keep.window_maximized;
}

void thug_settings_apply_audio_input(void)
{
    int v = g_thug_settings.volume, r = g_thug_settings.rumble;
    if (!s_focused && g_thug_settings.mute_unfocused)
        v = 0;
    xbox_ApuSetVolume((float)(v < 0 ? 0 : v > 100 ? 100 : v) / 100.0f);
    xbox_InputSetRumbleScale((float)(r < 0 ? 0 : r > 100 ? 100 : r) / 100.0f);
}

void thug_settings_set_focused(int focused)
{
    s_focused = focused ? 1 : 0;
    thug_settings_apply_audio_input();
}

const ThugAspect thug_aspects[] = {
    { "4:3",   4.0f / 3.0f },
    { "16:10", 16.0f / 10.0f },
    { "16:9",  16.0f / 9.0f },
};
const int thug_aspect_count = (int)(sizeof thug_aspects / sizeof thug_aspects[0]);

static const ThugAspect *aspect_nearest(float v)
{
    const ThugAspect *best = &thug_aspects[0];
    int i;
    for (i = 1; i < thug_aspect_count; i++)
        if (fabsf(thug_aspects[i].value - v) < fabsf(best->value - v))
            best = &thug_aspects[i];
    return best;
}

float thug_aspect_parse(const char *text)
{
    float v;
    const char *colon;

    if (!text || !*text)
        return 0.0f;
    colon = strchr(text, ':');
    if (colon) {
        float w = (float)atof(text), h = (float)atof(colon + 1);
        v = h > 0.0f ? w / h : 0.0f;
    } else {
        v = (float)atof(text);
    }
    if (!(v >= 1.0f && v <= 3.0f))
        return 0.0f;
    return aspect_nearest(v)->value;
}

const char *thug_aspect_name(float value)
{
    return aspect_nearest(value)->name;
}

int thug_aspect_is_wide(float value)
{
    return value > 4.0f / 3.0f + 0.01f;
}

static char s_path[1024];

const char *thug_settings_path(void)
{
    const char *base;

    if (s_path[0])
        return s_path;
#if defined(_WIN32)
    base = getenv("APPDATA");
    snprintf(s_path, sizeof s_path, "%s\\thug-pc-port\\settings.ini",
             base && *base ? base : ".");
#else
    base = getenv("XDG_CONFIG_HOME");
    if (base && *base) {
        snprintf(s_path, sizeof s_path, "%s/thug-pc-port/settings.ini", base);
    } else {
        base = getenv("HOME");
        snprintf(s_path, sizeof s_path, "%s/.config/thug-pc-port/settings.ini",
                 base && *base ? base : ".");
    }
#endif
    return s_path;
}

/* One setting: its key in the file, the environment variable, and the field. */
enum { T_INT, T_FLOAT, T_ASPECT };
typedef struct {
    const char *key, *env;
    int type;
    volatile void *field;
} Item;

static const Item k_items[] = {
    { "resolution_scale", "THUG_RES_SCALE",     1, &g_thug_settings.res_scale },
    { "vsync",            "THUG_VSYNC",         0, &g_thug_settings.vsync },
    { "fps_limit",        "THUG_FPS_LIMIT",     0, &g_thug_settings.fps_limit },
    { "anisotropy",       "THUG_ANISO",         0, &g_thug_settings.aniso },
    { "aspect_ratio",     "THUG_ASPECT",        T_ASPECT, &g_thug_settings.aspect },
    { "fullscreen",       "THUG_FULLSCREEN",    0, &g_thug_settings.fullscreen },
    { "show_fps",         "THUG_SHOW_FPS",      0, &g_thug_settings.show_fps },
    { "vblank_timing",    "THUG_VBLANK_TIMING", 0, &g_thug_settings.vblank_timing },
    { "antialiasing",     "THUG_MSAA",          0, &g_thug_settings.msaa },
    { "brightness",       "THUG_BRIGHTNESS",    1, &g_thug_settings.brightness },
    { "volume",           "THUG_VOLUME",        0, &g_thug_settings.volume },
    { "mute_in_background", "THUG_MUTE_UNFOCUSED", 0, &g_thug_settings.mute_unfocused },
    { "vibration",        "THUG_RUMBLE",        0, &g_thug_settings.rumble },
    { "window_width",     "THUG_WINDOW_W",      0, &g_thug_settings.window_w },
    { "window_height",    "THUG_WINDOW_H",      0, &g_thug_settings.window_h },
    { "window_x",         "THUG_WINDOW_X",      0, &g_thug_settings.window_x },
    { "window_y",         "THUG_WINDOW_Y",      0, &g_thug_settings.window_y },
    { "window_maximized", "THUG_WINDOW_MAXIMIZED", 0, &g_thug_settings.window_maximized },
};
#define N_ITEMS (sizeof k_items / sizeof k_items[0])

static void set_from_text(const Item *it, const char *v)
{
    switch (it->type) {
    case T_FLOAT:
        *(volatile float *)it->field = (float)atof(v);
        break;
    case T_ASPECT: {
        float a = thug_aspect_parse(v);
        if (a > 0.0f)                       /* text that is no aspect: kept */
            *(volatile float *)it->field = a;
        break;
    }
    default:
        *(volatile int *)it->field = atoi(v);
        break;
    }
}

static void format_value(const Item *it, char *buf, size_t n)
{
    switch (it->type) {
    case T_FLOAT:
        snprintf(buf, n, "%g", (double)*(volatile float *)it->field);
        break;
    case T_ASPECT:
        snprintf(buf, n, "%s", thug_aspect_name(*(volatile float *)it->field));
        break;
    default:
        snprintf(buf, n, "%d", *(volatile int *)it->field);
        break;
    }
}

/* Before aspect_ratio there was widescreen = 0 / 1 (THUG_WIDESCREEN): read it
 * where the newer line or variable is absent. */
static float widescreen_aspect(const char *v)
{
    return (v[0] == '0' && v[1] == 0) ? 4.0f / 3.0f : 16.0f / 9.0f;
}

void thug_settings_load(void)
{
    FILE *f = fopen(thug_settings_path(), "r");
    char line[256];
    size_t i;
    int have_aspect = 0;
    float old_ws = 0.0f;

    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='), *k = line, *v, *end;
            if (!eq || line[0] == '#' || line[0] == ';' || line[0] == '[')
                continue;
            *eq = 0;
            v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            for (end = eq - 1; end >= k && (*end == ' ' || *end == '\t'); end--)
                *end = 0;
            while (*v == ' ' || *v == '\t') v++;
            for (end = v + strlen(v) - 1; end >= v && (*end == '\n' || *end == '\r'
                                                       || *end == ' '); end--)
                *end = 0;
            for (i = 0; i < N_ITEMS; i++)
                if (!strcmp(k, k_items[i].key))
                    set_from_text(&k_items[i], v);
            if (!strcmp(k, "aspect_ratio"))
                have_aspect = 1;
            else if (!strcmp(k, "widescreen"))
                old_ws = widescreen_aspect(v);
        }
        fclose(f);
    }
    if (!have_aspect && old_ws > 0.0f)
        g_thug_settings.aspect = old_ws;
    {
        const char *e = getenv("THUG_WIDESCREEN");
        if (e && *e && !(getenv("THUG_ASPECT") && *getenv("THUG_ASPECT")))
            g_thug_settings.aspect = widescreen_aspect(e);
    }
    /* The environment wins for this run; otherwise export the file's value
     * for the code that reads the variable at start-up. */
    for (i = 0; i < N_ITEMS; i++) {
        const char *e = getenv(k_items[i].env);
        if (e && *e) {
            set_from_text(&k_items[i], e);
        } else {
            char buf[32];
            format_value(&k_items[i], buf, sizeof buf);
            setenv_once(k_items[i].env, buf);
        }
    }
}

int thug_settings_save(void)
{
    const char *path = thug_settings_path();
    char dir[sizeof s_path], tmp[sizeof s_path + 8];
    char *p;
    FILE *f;
    size_t i;

    /* mkdir -p the directory. */
    snprintf(dir, sizeof dir, "%s", path);
    p = strrchr(dir, '/');
#if defined(_WIN32)
    if (!p || (strrchr(dir, '\\') && strrchr(dir, '\\') > p))
        p = strrchr(dir, '\\');
#endif
    if (p) {
        *p = 0;
        for (p = dir + 1; *p; p++)
            if (*p == '/' || *p == '\\') {
                char c = *p;
                *p = 0;
                mkdir_one(dir);
                *p = c;
            }
        mkdir_one(dir);
    }
    /* Write a temporary file and rename it over the old one, so a crash
     * mid-write never leaves half a file. */
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f)
        return -1;
    fprintf(f, "# Tony Hawk's Underground PC port settings (F1 in game).\n"
               "# A THUG_* environment variable overrides its line for one run.\n");
    for (i = 0; i < N_ITEMS; i++) {
        char buf[32];
        format_value(&k_items[i], buf, sizeof buf);
        fprintf(f, "%s = %s\n", k_items[i].key, buf);
    }
    if (fclose(f) != 0)
        return -1;
#if defined(_WIN32)
    remove(path);
#endif
    return rename(tmp, path) == 0 ? 0 : -1;
}
