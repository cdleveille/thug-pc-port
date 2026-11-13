/**
 * overlay.cpp -- the in-game settings overlay. See overlay.h.
 *
 * F1 (or L3 + R3, both sticks clicked, on a pad) opens and closes it; Esc or B
 * closes it too. While it is open the game sees its pad at rest
 * (xbox_InputSetBlocked) and gets no keyboard input, and the overlay takes
 * keyboard, mouse and pad navigation. Every change applies at once, except
 * that the game picks its widescreen or 4:3 menu and HUD layout at start-up
 * (16:10 and 16:9 share one). The settings file is written when the overlay
 * closes.
 */
#include "overlay.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glad/gl.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include "host/restart.h"
extern "C" {
#include "settings.h"
#include "gpu/nv2a_pb_gl.h"
void xbox_InputSetBlocked(int on);
void xbox_InputKeysClear(void);
}

/* ── State shared between the threads ─────────────────────────────────── */

static SDL_mutex       *s_lock;
static SDL_Event        s_queue[256];
static int              s_queue_n;
static volatile int     s_open;
static volatile int     s_fs_request = -1;
static volatile float   s_mouse_sx = 1.0f, s_mouse_sy = 1.0f;
static volatile int     s_save_pending;
static volatile int     s_restart_request;  /* "Restart to apply changes" chosen */
static float            s_aspect_at_boot;   /* the game set its layout from this */
static int              s_x11_at_boot;      /* SDL picked its video driver from this */

/* The aspect needs a restart: the game picks its 4:3 or widescreen menu and
 * HUD layout as it boots (16:10 and 16:9 share one). */
static bool aspect_restart_pending(void)
{
    return thug_aspect_is_wide(g_thug_settings.aspect) != thug_aspect_is_wide(s_aspect_at_boot);
}

/* So does the display server (Linux): SDL picks its video driver once.
 * SDL_VIDEODRIVER, set by the player, overrides the choice, so then there
 * is nothing to restart for. */
static bool x11_restart_pending(void)
{
#if defined(__linux__)
    const char *vd = getenv("SDL_VIDEODRIVER");
    return (g_thug_settings.x11 != 0) != (s_x11_at_boot != 0) && !(vd && *vd);
#else
    return false;
#endif
}

static bool restart_pending(void)
{
    return aspect_restart_pending() || x11_restart_pending();
}

/* Pad state (main thread): the controller buttons held now, one bit per
 * SDL_GameControllerButton, on any pad; and whether the game's pad stays
 * blocked until they are all let go. Closing with L3+R3 or B leaves those
 * buttons held, and the game would read them as a fresh press. */
static uint32_t s_pad_held;
static int      s_unblock_on_release;

static bool pad_held(int button)
{
    return (s_pad_held >> button) & 1u;
}

/* ── Render-thread state ──────────────────────────────────────────────── */

static int      s_inited;
static float    s_font_px;              /* size the font atlas was built at */
static ImGuiStyle s_base_style;
static uint64_t s_last_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── Main thread ──────────────────────────────────────────────────────── */

static void queue_event(const SDL_Event *ev)
{
    SDL_LockMutex(s_lock);
    if (s_queue_n < (int)(sizeof s_queue / sizeof s_queue[0]))
        s_queue[s_queue_n++] = *ev;
    SDL_UnlockMutex(s_lock);
}

static void set_open(int open)
{
    if (open == s_open)
        return;
    s_open = open;
    /* Closing hands the pad back once nothing is held (see s_pad_held). */
    s_unblock_on_release = !open && s_pad_held != 0;
    if (!s_unblock_on_release)
        xbox_InputSetBlocked(open);
    if (open) {
        /* The overlay takes the keys from here on, key-ups included: let
         * go of every key the game saw held, or one held now would stay
         * held for the game after the overlay closes. */
        xbox_InputKeysClear();
        /* Start the overlay with the pointer where it is now. */
        SDL_Event m;
        int x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        memset(&m, 0, sizeof m);
        m.type = SDL_MOUSEMOTION;
        m.motion.x = x;
        m.motion.y = y;
        queue_event(&m);
    } else {
        s_save_pending = 1;
    }
    /* The cursor follows thug_overlay_is_open() (host_window.c). */
}

extern "C" void thug_overlay_window_resized(SDL_Window *win)
{
    int ww = 0, wh = 0, dw = 0, dh = 0;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    if (ww > 0 && wh > 0 && dw > 0 && dh > 0) {
        s_mouse_sx = (float)dw / (float)ww;
        s_mouse_sy = (float)dh / (float)wh;
    }
}

extern "C" int thug_overlay_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_KEYDOWN:
        if (ev->key.keysym.sym == SDLK_F1 && !ev->key.repeat) {
            set_open(!s_open);
            return 1;
        }
        if (s_open && ev->key.keysym.sym == SDLK_ESCAPE && !ev->key.repeat) {
            set_open(0);
            return 1;
        }
        /* F11 / Alt+Enter stay with the window. */
        if (ev->key.keysym.sym == SDLK_F11
                || (ev->key.keysym.sym == SDLK_RETURN && (ev->key.keysym.mod & KMOD_ALT)))
            return 0;
        if (s_open) { queue_event(ev); return 1; }
        return 0;
    case SDL_KEYUP:
        if (ev->key.keysym.sym == SDLK_F1)
            return 1;
        if (s_open) { queue_event(ev); return 1; }
        return 0;
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
        if (s_open) { queue_event(ev); return 1; }
        return 0;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        int down = ev->type == SDL_CONTROLLERBUTTONDOWN;
        int b = ev->cbutton.button;
        if (b >= 0 && b < 32) {
            if (down) s_pad_held |= 1u << b;
            else      s_pad_held &= ~(1u << b);
        }
        if (!down && s_unblock_on_release && !s_pad_held) {
            s_unblock_on_release = 0;
            xbox_InputSetBlocked(0);
        }
        /* L3 + R3 (both sticks clicked) opens and closes the overlay. */
        if (down && (b == SDL_CONTROLLER_BUTTON_LEFTSTICK || b == SDL_CONTROLLER_BUTTON_RIGHTSTICK)
                && pad_held(SDL_CONTROLLER_BUTTON_LEFTSTICK)
                && pad_held(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
            set_open(!s_open);
            return 1;
        }
        if (s_open && down && ev->cbutton.button == SDL_CONTROLLER_BUTTON_B) {
            set_open(0);
            return 1;
        }
        if (s_open) queue_event(ev);
        return 0;    /* the game's pad is blocked separately */
    }
    case SDL_CONTROLLERAXISMOTION:
        if (s_open) queue_event(ev);
        return 0;
    case SDL_CONTROLLERDEVICEREMOVED:
        /* A pad pulled out sends no button-ups: forget what it held. */
        s_pad_held = 0;
        if (s_unblock_on_release) {
            s_unblock_on_release = 0;
            xbox_InputSetBlocked(0);
        }
        return 0;
    default:
        return 0;
    }
}

extern "C" int thug_overlay_is_open(void)
{
    return s_open;
}

extern "C" int thug_overlay_take_restart_request(void)
{
    int r = s_restart_request;
    s_restart_request = 0;
    return r;
}

extern "C" int thug_overlay_take_fullscreen_request(void)
{
    int r = s_fs_request;
    if (r >= 0)
        s_fs_request = -1;
    if (s_save_pending) {
        s_save_pending = 0;
        if (thug_settings_save() != 0)
            fprintf(stderr, "[thug] could not write %s\n", thug_settings_path());
    }
    return r;
}

/* ── Render thread: input ─────────────────────────────────────────────── */

static ImGuiKey key_of(SDL_Keycode k)
{
    switch (k) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

static ImGuiKey pad_key_of(Uint8 b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return ImGuiKey_GamepadFaceDown;
    case SDL_CONTROLLER_BUTTON_B: return ImGuiKey_GamepadFaceRight;
    case SDL_CONTROLLER_BUTTON_X: return ImGuiKey_GamepadFaceLeft;
    case SDL_CONTROLLER_BUTTON_Y: return ImGuiKey_GamepadFaceUp;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ImGuiKey_GamepadL1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ImGuiKey_GamepadR1;
    default: return ImGuiKey_None;
    }
}

static void stick(ImGuiIO &io, ImGuiKey neg, ImGuiKey pos, Sint16 v)
{
    const float dead = 8000.0f;
    float f = (float)v;
    io.AddKeyAnalogEvent(neg, f < -dead, f < -dead ? (-f - dead) / (32768.0f - dead) : 0.0f);
    io.AddKeyAnalogEvent(pos, f > dead, f > dead ? (f - dead) / (32767.0f - dead) : 0.0f);
}

static void drain_events(ImGuiIO &io)
{
    SDL_Event evs[256];
    int n, i;

    SDL_LockMutex(s_lock);
    n = s_queue_n;
    memcpy(evs, s_queue, (size_t)n * sizeof evs[0]);
    s_queue_n = 0;
    SDL_UnlockMutex(s_lock);

    for (i = 0; i < n; i++) {
        const SDL_Event *ev = &evs[i];
        switch (ev->type) {
        case SDL_MOUSEMOTION:
            io.AddMousePosEvent((float)ev->motion.x * s_mouse_sx,
                                (float)ev->motion.y * s_mouse_sy);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int b = ev->button.button == SDL_BUTTON_LEFT ? 0
                  : ev->button.button == SDL_BUTTON_RIGHT ? 1
                  : ev->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            io.AddMousePosEvent((float)ev->button.x * s_mouse_sx,
                                (float)ev->button.y * s_mouse_sy);
            if (b >= 0)
                io.AddMouseButtonEvent(b, ev->type == SDL_MOUSEBUTTONDOWN);
            break;
        }
        case SDL_MOUSEWHEEL:
            io.AddMouseWheelEvent((float)ev->wheel.x, (float)ev->wheel.y);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            int down = ev->type == SDL_KEYDOWN;
            SDL_Keymod m = (SDL_Keymod)ev->key.keysym.mod;
            ImGuiKey k = key_of(ev->key.keysym.sym);
            io.AddKeyEvent(ImGuiMod_Ctrl, (m & KMOD_CTRL) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (m & KMOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (m & KMOD_ALT) != 0);
            if (k != ImGuiKey_None)
                io.AddKeyEvent(k, down);
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            ImGuiKey k = pad_key_of(ev->cbutton.button);
            if (k != ImGuiKey_None)
                io.AddKeyEvent(k, ev->type == SDL_CONTROLLERBUTTONDOWN);
            break;
        }
        case SDL_CONTROLLERAXISMOTION:
            if (ev->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
                stick(io, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ev->caxis.value);
            else if (ev->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
                stick(io, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ev->caxis.value);
            break;
        default:
            break;
        }
    }
}

/* ── Render thread: setup ─────────────────────────────────────────────── */

static const char *find_font(void)
{
    static const char *const k_fonts[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",
#endif
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    };
    for (size_t i = 0; i < sizeof k_fonts / sizeof k_fonts[0]; i++) {
        FILE *f = fopen(k_fonts[i], "rb");
        if (f) {
            fclose(f);
            return k_fonts[i];
        }
    }
    return NULL;
}

/* Font and style for a drawable height: 1080 lines is scale 1. */
static void ensure_scale(int dh)
{
    ImGuiIO &io = ImGui::GetIO();
    float ui = (float)dh / 1080.0f, px;
    const char *path;

    if (ui < 0.75f)
        ui = 0.75f;
    px = floorf(20.0f * ui + 0.5f);
    if (px == s_font_px)
        return;
    /* The first atlas is uploaded by the backend's first NewFrame; a later
     * one replaces the texture here. */
    if (s_font_px != 0.0f)
        ImGui_ImplOpenGL3_DestroyFontsTexture();
    io.Fonts->Clear();
    path = find_font();
    if (!path || !io.Fonts->AddFontFromFileTTF(path, px)) {
        ImFontConfig cfg;
        cfg.SizePixels = px;
        io.Fonts->AddFontDefault(&cfg);
    }
    if (s_font_px != 0.0f)
        ImGui_ImplOpenGL3_CreateFontsTexture();
    ImGui::GetStyle() = s_base_style;
    ImGui::GetStyle().ScaleAllSizes(ui);
    s_font_px = px;
}

static void init_imgui(void)
{
    ImGuiIO *io;
    ImGuiStyle *st;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    io = &ImGui::GetIO();
    io->IniFilename = NULL;              /* nothing to remember between runs */
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard
                     | ImGuiConfigFlags_NavEnableGamepad
                     | ImGuiConfigFlags_NoMouseCursorChange;
    io->BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui::StyleColorsDark();
    st = &ImGui::GetStyle();
    st->WindowRounding = 6.0f;
    st->FrameRounding = 4.0f;
    st->GrabRounding = 4.0f;
    st->WindowPadding = ImVec2(14, 12);
    st->ItemSpacing = ImVec2(10, 8);
    st->Colors[ImGuiCol_WindowBg].w = 0.92f;
    s_base_style = *st;
    ImGui_ImplOpenGL3_Init("#version 330 core");
    s_last_ns = now_ns();
    s_inited = 1;
}

/* ── Render thread: the UI ────────────────────────────────────────────── */

/* A "(?)" with a tooltip, beside the row's label. */
static void help_marker(const char *text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void draw_fps_counter(const Nv2aGlFrameInfo &fi)
{
    ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
                        | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav
                        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::SetNextWindowPos(ImVec2(12, 12));
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("##fps", NULL, fl))
        ImGui::Text("%.0f fps  %.1f ms", fi.fps, fi.frame_ms);
    ImGui::End();
}

static void draw_settings(const Nv2aGlFrameInfo &fi, int dw, int dh)
{
    volatile ThugSettings &S = g_thug_settings;
    float aspect = nv2a_pb_gl_aspect();
    float native = nv2a_pb_gl_native_scale();
    float label_w;

    ImGui::SetNextWindowPos(ImVec2((float)dw * 0.5f, (float)dh * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30.0f, 0.0f), ImGuiCond_Always);
    if (!ImGui::Begin("Tony Hawk's Underground  -  settings", NULL,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    label_w = ImGui::GetFontSize() * 13.0f;

    /* ---- Display ---- */
    ImGui::SeparatorText("Display");
    {
        char cur[32];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Aspect ratio");
        help_marker("The shape of the picture. Pick your display's: 16:9 for most "
                    "monitors and TVs, 16:10 for many laptops, 4:3 for the "
                    "original. The field of view widens to fit: the same view up "
                    "and down, more at the sides. In 16:9 and 16:10 the menus and "
                    "HUD use the game's own widescreen layout.");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        snprintf(cur, sizeof cur, "%s", thug_aspect_name(S.aspect));
        if (ImGui::BeginCombo("##aspect", cur)) {
            for (int i = 0; i < thug_aspect_count; i++) {
                const ThugAspect &a = thug_aspects[i];
                bool sel = fabsf(S.aspect - a.value) < 0.01f;
                if (ImGui::Selectable(a.name, sel) && !sel) {
                    S.aspect = a.value;
                    nv2a_pb_gl_set_aspect(a.value);
                    aspect = a.value;
                }
            }
            ImGui::EndCombo();
        }
        /* The game picks its widescreen or standard layout at start-up;
         * switching between 16:10 and 16:9 needs nothing more. */
        if (aspect_restart_pending()) {
            ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "menus and HUD follow after a restart");
        }
    }
    {
        /* Common sizes for each shape, as the internal picture's height
         * (the game's 480 lines times the scale); the width follows the
         * aspect. Labels are the usual names (854x480 is 853.3). */
        struct Res { int w, h; };
        static const Res k_4_3[]   = { { 2880, 2160 }, { 2048, 1536 }, { 1920, 1440 },
                                       { 1600, 1200 }, { 1400, 1050 }, { 1280, 960 },
                                       { 1024, 768 }, { 800, 600 }, { 640, 480 } };
        static const Res k_16_10[] = { { 3840, 2400 }, { 2560, 1600 }, { 1920, 1200 },
                                       { 1680, 1050 }, { 1440, 900 }, { 1280, 800 },
                                       { 768, 480 } };
        static const Res k_16_9[]  = { { 3840, 2160 }, { 2560, 1440 }, { 1920, 1080 },
                                       { 1600, 900 }, { 1280, 720 }, { 854, 480 } };
        const Res *list;
        size_t n;
        char cur[64], item[64];
        int native_h = (int)floorf(native * 480.0f + 0.5f);
        float sc = nv2a_pb_gl_scale();
        int cur_h = (int)floorf(sc * 480.0f + 0.5f);

        if (aspect < 1.4f) {
            list = k_4_3;   n = sizeof k_4_3 / sizeof k_4_3[0];
        } else if (aspect < 1.7f) {
            list = k_16_10; n = sizeof k_16_10 / sizeof k_16_10[0];
        } else {
            list = k_16_9;  n = sizeof k_16_9 / sizeof k_16_9[0];
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Resolution");
        help_marker("The size the game is drawn at, before it is scaled to the "
                    "window. Native matches your display's height. Higher is "
                    "sharper and costs more; 480 lines is the original.");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        if (S.res_scale <= 0.0f) {
            snprintf(cur, sizeof cur, "Native  (%dx%d)", (int)(native_h * aspect + 0.5f), native_h);
        } else {
            snprintf(cur, sizeof cur, "%dx%d", (int)(cur_h * aspect + 0.5f), cur_h);
            for (size_t i = 0; i < n; i++)
                if (list[i].h == cur_h)
                    snprintf(cur, sizeof cur, "%dx%d", list[i].w, list[i].h);
        }
        if (ImGui::BeginCombo("##res", cur)) {
            snprintf(item, sizeof item, "Native  (%dx%d)", (int)(native_h * aspect + 0.5f), native_h);
            if (ImGui::Selectable(item, S.res_scale <= 0.0f)) {
                S.res_scale = 0.0f;
                nv2a_pb_gl_set_scale(0.0f);
            }
            for (size_t i = 0; i < n; i++) {
                int h = list[i].h;
                bool sel = S.res_scale > 0.0f && cur_h == h;
                snprintf(item, sizeof item, "%dx%d%s", list[i].w, h,
                         h == 480 ? "  (original)" : "");
                if (ImGui::Selectable(item, sel)) {
                    S.res_scale = (float)h / 480.0f;
                    nv2a_pb_gl_set_scale(S.res_scale);
                }
            }
            ImGui::EndCombo();
        }
    }
    {
        bool fs = S.fullscreen != 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Fullscreen");
        ImGui::SameLine(label_w);
        if (ImGui::Checkbox("##fs", &fs)) {
            S.fullscreen = fs;
            s_fs_request = fs;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("F11");
    }
#if defined(__linux__)
    {
        /* Linux only: Windows has one window system. */
        static const char *const k_names[] = { "Wayland", "X11" };
        const char *running = SDL_GetCurrentVideoDriver();
        const char *vd = getenv("SDL_VIDEODRIVER");
        int x11 = S.x11 != 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Display server");
        help_marker("How the game talks to your desktop. Wayland is the default. "
                    "X11 runs the game through XWayland on a Wayland desktop. With "
                    "NVIDIA's driver X11 is faster (about 10% more frames per "
                    "second measured): only there can the driver do its OpenGL "
                    "work on a thread of its own. With fractional display "
                    "scaling, X11 can look softer if your desktop scales X11 "
                    "programs itself. Applies after a restart; if the chosen one "
                    "is not available, the game uses the other.");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##dispserver", k_names[x11])) {
            for (int i = 0; i < 2; i++)
                if (ImGui::Selectable(k_names[i], x11 == i))
                    S.x11 = i;
            ImGui::EndCombo();
        }
        ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::SameLine();
        if (vd && *vd)
            ImGui::TextDisabled("SDL_VIDEODRIVER=%s decides instead", vd);
        else if (x11_restart_pending())
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "applies after a restart");
        else
            ImGui::TextDisabled("running on %s", running && !strcmp(running, "x11") ? "X11"
                                : running && !strcmp(running, "wayland") ? "Wayland"
                                : running ? running : "?");
    }
#endif
    {
        bool v = nv2a_pb_gl_vsync() != 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("VSync");
        ImGui::SameLine(label_w);
        if (ImGui::Checkbox("##vsync", &v)) {
            S.vsync = v;
            nv2a_pb_gl_set_vsync(v);
        }
    }
    {
        static const int k_limits[] = { 0, 30, 60, 75, 120, 144, 165, 240 };
        char cur[32];
        int lim = nv2a_pb_gl_fps_limit();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Frame rate limit");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        if (lim) snprintf(cur, sizeof cur, "%d fps", lim);
        else snprintf(cur, sizeof cur, "Off");
        if (ImGui::BeginCombo("##limit", cur)) {
            for (size_t i = 0; i < sizeof k_limits / sizeof k_limits[0]; i++) {
                char it[32];
                if (k_limits[i]) snprintf(it, sizeof it, "%d fps", k_limits[i]);
                else snprintf(it, sizeof it, "Off");
                if (ImGui::Selectable(it, lim == k_limits[i])) {
                    S.fps_limit = k_limits[i];
                    nv2a_pb_gl_set_fps_limit(k_limits[i]);
                }
            }
            ImGui::EndCombo();
        }
    }
    {
        char cur[32];
        int a = nv2a_pb_gl_aniso(), amax = nv2a_pb_gl_aniso_max();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Anisotropic filtering");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        if (a > 1) snprintf(cur, sizeof cur, "%dx", a);
        else snprintf(cur, sizeof cur, "Off");
        if (ImGui::BeginCombo("##aniso", cur)) {
            for (int v = 1; v <= amax && v <= 16; v *= 2) {
                char it[32];
                if (v > 1) snprintf(it, sizeof it, "%dx", v);
                else snprintf(it, sizeof it, "Off");
                if (ImGui::Selectable(it, a == v)) {
                    S.aniso = v;
                    nv2a_pb_gl_set_aniso(v);
                }
            }
            ImGui::EndCombo();
        }
    }
    {
        char cur[32];
        int m = nv2a_pb_gl_msaa(), mmax = nv2a_pb_gl_msaa_max();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Antialiasing");
        help_marker("Multisample antialiasing smooths the jagged edges of polygons. "
                    "Cut-out textures such as fences, foliage and hair are smoothed "
                    "too. 4x is a good balance; 8x is the smoothest. The cost is "
                    "small at 1080p and below, and a few percent at 4K.");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        if (m > 1) snprintf(cur, sizeof cur, "%dx MSAA", m);
        else snprintf(cur, sizeof cur, "Off");
        if (ImGui::BeginCombo("##msaa", cur)) {
            for (int v = 1; v <= mmax && v <= 8; v *= 2) {
                char it[32];
                if (v > 1) snprintf(it, sizeof it, "%dx MSAA", v);
                else snprintf(it, sizeof it, "Off");
                if (ImGui::Selectable(it, m == v)) {
                    S.msaa = v;
                    nv2a_pb_gl_set_msaa(v);
                }
            }
            ImGui::EndCombo();
        }
    }
    {
        bool up = nv2a_pb_gl_ui_upscale() != 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Smooth text and icons");
        help_marker("The game's text, icons and logos are drawn for a 640x480 TV, "
                    "so enlarged they look soft and stair-stepped. On, they are "
                    "enlarged with xBR, an upscaler made for low-resolution game "
                    "art, which draws their outlines as smooth curves. It smooths "
                    "edges rather than adding detail; the difference grows with "
                    "the resolution.");
        ImGui::SameLine(label_w);
        if (ImGui::Checkbox("##uiup", &up)) {
            S.ui_upscale = up;
            nv2a_pb_gl_set_ui_upscale(up);
        }
    }
    {
        int pct = (int)floorf(S.brightness * 100.0f + 0.5f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Brightness");
        help_marker("Lifts or lowers the midtones; black stays black and white stays "
                    "white. 100% is the game as it was drawn. Raise it if dark "
                    "areas are hard to make out on your display.");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##bright", &pct, 50, 200, "%d%%",
                             ImGuiSliderFlags_AlwaysClamp)) {
            pct = (pct + 2) / 5 * 5;           /* steps of 5 from the pad */
            S.brightness = (float)pct / 100.0f;
            nv2a_pb_gl_set_brightness(S.brightness);
        }
    }

    /* ---- Audio ---- */
    ImGui::SeparatorText("Audio");
    {
        int v = S.volume;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Volume");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##vol", &v, 0, 100, v ? "%d%%" : "Muted",
                             ImGuiSliderFlags_AlwaysClamp)) {
            S.volume = v;
            thug_settings_apply_audio_input();
        }
    }
    {
        bool mu = S.mute_unfocused != 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Mute in background");
        help_marker("Silence the game while another window has the focus.");
        ImGui::SameLine(label_w);
        if (ImGui::Checkbox("##muteunf", &mu)) {
            S.mute_unfocused = mu;
            thug_settings_apply_audio_input();
        }
    }

    /* ---- Controller ---- */
    ImGui::SeparatorText("Controller");
    {
        int r = S.rumble;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Vibration");
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##rumble", &r, 0, 100, r ? "%d%%" : "Off",
                             ImGuiSliderFlags_AlwaysClamp)) {
            S.rumble = r;
            thug_settings_apply_audio_input();
        }
    }

    /* ---- Game ---- */
    ImGui::SeparatorText("Game");
    {
        bool vt = S.vblank_timing != 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Original 60 Hz timing");
        help_marker("The Xbox game counts time in whole 60 Hz frames, so above 60 fps "
                    "it runs fast. Off (the default), game time follows the real clock "
                    "at any frame rate. On restores the original arithmetic; use it "
                    "with a 60 fps limit.");
        ImGui::SameLine(label_w);
        if (ImGui::Checkbox("##vt", &vt))
            S.vblank_timing = vt;
    }
    {
        bool sf = S.show_fps != 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("FPS counter");
        ImGui::SameLine(label_w);
        if (ImGui::Checkbox("##showfps", &sf))
            S.show_fps = sf;
    }

    /* ---- Performance ---- */
    ImGui::SeparatorText("Performance");
    ImGui::Text("%.1f fps   %.2f ms   %ux%u", fi.fps, fi.frame_ms, fi.out_w, fi.out_h);
    ImGui::Text("%u draws   %.1fK triangles per frame", fi.draws, fi.tris / 1000.0f);
    if (fi.hist_n > 1) {
        float worst = 0.0f;
        char label[48];
        for (int i = 0; i < fi.hist_n; i++)
            if (fi.hist[i] > worst) worst = fi.hist[i];
        snprintf(label, sizeof label, "frame time (worst %.1f ms)", worst);
        ImGui::PlotLines("##ft", fi.hist, fi.hist_n, 0, label, 0.0f,
                         worst > 33.4f ? worst : 33.4f,
                         ImVec2(-1, ImGui::GetFontSize() * 4.0f));
    }

    ImGui::Separator();
    /* Only when something waits for a restart, and this build can do one. */
    if (restart_pending() && thug_restart_supported()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.45f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.55f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.38f, 0.06f, 1.0f));
        if (ImGui::Button("Restart to apply changes"))
            ImGui::OpenPopup("Restart now?");
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
    }
    if (ImGui::BeginPopupModal("Restart now?", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("Restart the game to apply the changes?\n"
                               "Progress since your last save will be lost.");
        if (ImGui::Button("Restart")) {
            s_restart_request = 1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    if (ImGui::Button("Restore defaults"))
        ImGui::OpenPopup("Restore defaults?");
    if (ImGui::BeginPopupModal("Restore defaults?", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("Put every setting back to its default?\n"
                               "(Fullscreen stays as it is.)");
        if (ImGui::Button("Restore")) {
            thug_settings_reset();
            nv2a_pb_gl_set_scale(S.res_scale);
            nv2a_pb_gl_set_vsync(S.vsync);
            nv2a_pb_gl_set_fps_limit(S.fps_limit);
            nv2a_pb_gl_set_aniso(S.aniso);
            nv2a_pb_gl_set_msaa(S.msaa);
            nv2a_pb_gl_set_ui_upscale(S.ui_upscale);
            nv2a_pb_gl_set_brightness(S.brightness);
            nv2a_pb_gl_set_aspect(S.aspect);
            thug_settings_apply_audio_input();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("F1 or Esc closes (pad: L3+R3, B). F11: fullscreen.");
    ImGui::TextDisabled("Saved to %s", thug_settings_path());
    ImGui::End();
}

/* The renderer's overlay hook: executor thread, default framebuffer bound. */
static void overlay_render(int dw, int dh)
{
    Nv2aGlFrameInfo fi;
    uint64_t t;

    if (!s_open && !g_thug_settings.show_fps) {
        if (s_inited) {                       /* drop input queued meanwhile */
            SDL_LockMutex(s_lock);
            s_queue_n = 0;
            SDL_UnlockMutex(s_lock);
        }
        return;
    }
    if (!s_inited)
        init_imgui();
    ensure_scale(dh);

    ImGuiIO &io = ImGui::GetIO();
    t = now_ns();
    io.DeltaTime = (float)((double)(t - s_last_ns) / 1e9);
    if (io.DeltaTime <= 0.0f)
        io.DeltaTime = 1.0f / 1000.0f;
    s_last_ns = t;
    io.DisplaySize = ImVec2((float)dw, (float)dh);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.MouseDrawCursor = false;
    drain_events(io);

    nv2a_pb_gl_frame_info(&fi);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    if (s_open)
        draw_settings(fi, dw, dh);
    else
        draw_fps_counter(fi);
    ImGui::Render();
    glViewport(0, 0, dw, dh);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

extern "C" void thug_overlay_install(SDL_Window *win)
{
    s_lock = SDL_CreateMutex();
    s_aspect_at_boot = g_thug_settings.aspect;
    s_x11_at_boot = g_thug_settings.x11;
    thug_overlay_window_resized(win);
    nv2a_pb_gl_set_overlay(overlay_render);
    if (getenv("THUG_OVERLAY_OPEN"))      /* testing: start with it open */
        set_open(1);
}
