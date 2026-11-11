/**
 * settings.h -- the player's settings, kept in a small INI file.
 *
 * Linux:   $XDG_CONFIG_HOME/thug-pc-port/settings.ini (~/.config/...)
 * Windows: %APPDATA%\thug-pc-port\settings.ini
 *
 * thug_settings_load() runs first thing in main(). A THUG_* environment
 * variable set by the player wins over the file for that run; otherwise the
 * file's value is exported as that variable, so code that reads the
 * variable at start-up (the renderer, the widescreen default in main.c)
 * sees the saved setting. The in-game overlay (F1) changes the live values
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
    int   widescreen;     /* takes effect at the next start               THUG_WIDESCREEN */
    int   fullscreen;     /* THUG_FULLSCREEN */
    int   show_fps;       /* small counter while the overlay is closed    THUG_SHOW_FPS */
    int   vblank_timing;  /* the original whole-vblank game timing        THUG_VBLANK_TIMING */
    int   msaa;           /* antialiasing samples: 1 (off), 2, 4, 8       THUG_MSAA */
    float brightness;     /* gamma on the presented frame, 1 = as drawn   THUG_BRIGHTNESS */
    int   volume;         /* master volume, 0-100                         THUG_VOLUME */
    int   mute_unfocused; /* silent while the window is in the background THUG_MUTE_UNFOCUSED */
    int   rumble;         /* vibration strength, 0 (off) - 100            THUG_RUMBLE */
} ThugSettings;

/* The live values. Written by the overlay (executor thread) and F11 (main
 * thread); each field is one aligned word. */
extern volatile ThugSettings g_thug_settings;

void        thug_settings_load(void);
int         thug_settings_save(void);   /* 0 on success */
const char *thug_settings_path(void);

/* Every setting at its default, except fullscreen, which is left as it is
 * (the overlay's "Restore defaults" should not change the window mode). */
void        thug_settings_reset(void);

/* Push the audio and controller settings to the runtime: the master volume
 * (silenced while unfocused, if asked) and the vibration strength. Any
 * thread; main() calls it at start-up, the overlay when they change, the
 * window loop when the focus changes. */
void        thug_settings_apply_audio_input(void);
void        thug_settings_set_focused(int focused);

#ifdef __cplusplus
}
#endif

#endif /* THUG_SETTINGS_H */
