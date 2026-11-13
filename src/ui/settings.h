/**
 * settings.h -- the player's settings, kept in a small INI file.
 *
 * Linux:   $XDG_CONFIG_HOME/thug-pc-port/settings.ini (~/.config/...)
 * Windows: %APPDATA%\thug-pc-port\settings.ini
 *
 * thug_settings_load() runs first thing in main(). A THUG_* environment
 * variable set by the player wins over the file for that run; otherwise the
 * file's value is exported as that variable, so code that reads the
 * variable at start-up (the renderer, the dashboard's widescreen flag in
 * main.c) sees the saved setting. The in-game overlay (F1) changes the live values
 * and saves the file when it closes.
 */
#ifndef THUG_SETTINGS_H
#define THUG_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float res_scale;      /* internal resolution / 480 lines; 0 = native  THUG_RES_SCALE */
    int   vsync;          /* THUG_VSYNC */
    int   fps_limit;      /* 0 = off                                      THUG_FPS_LIMIT */
    int   aniso;          /* anisotropic filtering, 1 = off               THUG_ANISO */
    float aspect;         /* picture shape: 4/3, 16/10 or 16/9 (file: "16:9")  THUG_ASPECT */
    int   fullscreen;     /* THUG_FULLSCREEN */
    int   show_fps;       /* small counter while the overlay is closed    THUG_SHOW_FPS */
    int   vblank_timing;  /* the original whole-vblank game timing        THUG_VBLANK_TIMING */
    int   msaa;           /* antialiasing samples: 1 (off), 2, 4, 8       THUG_MSAA */
    float brightness;     /* gamma on the presented frame, 1 = as drawn   THUG_BRIGHTNESS */
    int   volume;         /* master volume, 0-100                         THUG_VOLUME */
    int   mute_unfocused; /* silent while the window is in the background THUG_MUTE_UNFOCUSED */
    int   rumble;         /* vibration strength, 0 (off) - 100            THUG_RUMBLE */
    int   ui_upscale;     /* smooth text and icons (xBR)                  THUG_UI_UPSCALE */
    /* Linux: SDL video on X11 (XWayland on a Wayland desktop) before
     * Wayland. Read at start-up only; the file has the line on Linux alone. */
    int   x11;            /*                                              THUG_X11 */
    /* The window as it was last left, in window (desktop-scaled) units:
     * restored at the next start. 0 x 0 is the built-in default size; the
     * position is THUG_WINDOW_POS_UNSET where the desktop places windows
     * itself (always on Wayland). The size is the un-maximised, windowed one. */
    int   window_w, window_h;  /*                                     THUG_WINDOW_W, _H */
    int   window_x, window_y;  /*                                     THUG_WINDOW_X, _Y */
    int   window_maximized;    /*                                     THUG_WINDOW_MAXIMIZED */
} ThugSettings;

#define THUG_WINDOW_POS_UNSET (-99999)

/* The live values. Written by the overlay (executor thread) and F11 (main
 * thread); each field is one aligned word. */
extern volatile ThugSettings g_thug_settings;

void        thug_settings_load(void);
int         thug_settings_save(void);   /* 0 on success */
const char *thug_settings_path(void);

/* Every setting at its default, except the window (fullscreen, size,
 * position, maximised), which is left as it is: "Restore defaults" in the
 * overlay should not move or resize the window. */
void        thug_settings_reset(void);

/* Push the audio and controller settings to the runtime: the master volume
 * (silenced while unfocused, if asked) and the vibration strength. Any
 * thread; main() calls it at start-up, the overlay when they change, the
 * window loop when the focus changes. */
void        thug_settings_apply_audio_input(void);
void        thug_settings_set_focused(int focused);

/* The aspect ratios offered, widest last; the live value is
 * g_thug_settings.aspect, one of these. */
typedef struct {
    const char *name;     /* as the file and the overlay write it: "16:9" */
    float       value;
} ThugAspect;
extern const ThugAspect thug_aspects[];
extern const int        thug_aspect_count;
/* The aspect a text names ("16:10", "1.6"), snapped to the nearest offered
 * one; 0 for text that is no aspect. */
float       thug_aspect_parse(const char *text);
/* The name of the offered aspect nearest to a value. */
const char *thug_aspect_name(float value);
/* Wider than 4:3: the game runs its own widescreen mode (HUD, menus). */
int         thug_aspect_is_wide(float value);

#ifdef __cplusplus
}
#endif

#endif /* THUG_SETTINGS_H */
