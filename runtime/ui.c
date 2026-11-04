// The game window and the host main thread's event loop (the guest runs on its own threads). The window is created
// before the guest starts so the renderer can attach to it from the game's graphics thread.
//   MHGU_HIDDEN=1     no visible window (test runs; controls then come only from MHGU_INPUT)
// Display mode, size and the 3D resolution come from the settings (runtime/config.c, mhgurecomp.ini).
#include <stdlib.h>

#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "config.h"
#include "app_actions.h"
#include "haptics.h"
#include "overlay.h"
#include "mods.h"
#include "perfstats.h"
#include "rt.h"
#include "touch.h"
#include "hle/input.h"

static int launch_argc;
static char **launch_argv;
static SDL_AtomicInt requested_action, action_failed;
void ui_capture_launch_args(int argc, char **argv) { launch_argc = argc; launch_argv = argv; }
void ui_request_action(bool restart) { SDL_SetAtomicInt(&requested_action, restart ? 2 : 1); }
bool ui_action_failed(void) { return SDL_SetAtomicInt(&action_failed, 0) != 0; }
#ifdef __ANDROID__
void ui_request_licenses(void) { SDL_SetAtomicInt(&requested_action, 3); }
void ui_request_game_folder(void) { SDL_SetAtomicInt(&requested_action, 4); }
void ui_request_save_import(void) { SDL_SetAtomicInt(&requested_action, 5); }
void ui_request_share_log(void) { SDL_SetAtomicInt(&requested_action, 6); }
void ui_request_save_export(void) { SDL_SetAtomicInt(&requested_action, 7); }
void ui_request_gpu_driver(void) { SDL_SetAtomicInt(&requested_action, 8); }
void ui_request_import(int kind) { SDL_SetAtomicInt(&requested_action, 9 + kind); }  // (0 cheats, 1 a mod, 2 DLC)
static void activity_call(const char *method) {  // (a static void method of GameActivity: as restart_application)
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, method, "()V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, cls, m);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}
#endif
#ifdef __ANDROID__
static void frame_cap_to_activity(float fps) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "setFrameCap", "(F)V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, cls, m, (jfloat)fps);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
}
#endif
static bool restart_application(void) {
#ifdef __ANDROID__
    // No process of our own here: GameActivity.restartApp() starts PhoenixActivity (process ":phoenix"), which waits
    // for this one to leave (the caller's exit(0): the atexit pipeline-cache save) and launches the app again. (The
    // activity's own class, not FindClass: the app's classes are not in a native thread's class loader.)
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) { LOG("[ui] restart failed: no activity (%s)\n", SDL_GetError()); return false; }
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, "restartApp", "()V") : NULL;
    if (m) (*env)->CallStaticVoidMethod(env, cls, m);
    bool ok = m && !(*env)->ExceptionCheck(env);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    LOG(ok ? "[ui] restart: PhoenixActivity relaunches the app once this process has left\n"
           : "[ui] restart failed: GameActivity.restartApp() (see logcat)\n");
    return ok;
#else
    char exe[4096], wait[80];
    if (!plat_exe_path(exe, sizeof exe)) return false;
    const char **args = calloc((size_t)launch_argc + 3, sizeof *args);
    if (!args) return false;
    int n = 0;
    args[n++] = exe;
    for (int i = 1; i < launch_argc; ++i)
        if (strncmp(launch_argv[i], "--recomp-wait=", 14)) args[n++] = launch_argv[i];
    snprintf(wait, sizeof wait, "--recomp-wait=%lu", plat_process_id());
    args[n++] = wait;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    SDL_Process *child = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    free(args);
    if (!child) { LOG("[ui] restart failed: %s\n", SDL_GetError()); return false; }
    LOG("[ui] restart child %lld waiting for %lu\n", (long long)SDL_GetNumberProperty(
        SDL_GetProcessProperties(child), SDL_PROP_PROCESS_PID_NUMBER, 0), plat_process_id());
    SDL_DestroyProcess(child);
    return true;
#endif
}

volatile uint64_t g_present_count;
static SDL_Window *window;

SDL_Window *ui_window(void) { return window; }

// The window's size when windowed: the settings' size (or the last windowed size), no larger than the desktop's free
// area with the window frame.
static int win_w, win_h;
static void fit_desktop(int *w, int *h) {
    SDL_Rect r;
    if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &r)) return;
    int top = 0, left = 0, bottom = 0, right = 0;
    if (window) SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);
    if (!top) top = 32, left = right = bottom = 8;  // (not known before the window exists: typical Windows 10 frame)
    r.w -= left + right, r.h -= top + bottom;
    if (*w <= r.w && *h <= r.h) return;
    int fw = r.w, fh = r.w * *h / *w;
    if (fh > r.h) fh = r.h, fw = r.h * *w / *h;
    LOG("[ui] %dx%d does not fit the desktop: window %dx%d\n", *w, *h, fw, fh);
    *w = fw, *h = fh;
}

// Full screen as the settings say (exclusive: the monitor mode closest to Resolution; else borderless desktop), or
// windowed. The cursor only shows in a window.
static void set_fullscreen(bool on) {
    bool was = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
    if (on && !was) SDL_GetWindowSize(window, &win_w, &win_h);
    if (on && g_config.display_mode == DISPLAY_FULLSCREEN) {
        SDL_DisplayMode m;
        SDL_DisplayID d = SDL_GetDisplayForWindow(window);
        if (SDL_GetClosestFullscreenDisplayMode(d, g_config.width, g_config.height, 0.0f, false, &m)) {
            SDL_SetWindowFullscreenMode(window, &m);
            LOG("[ui] fullscreen %dx%d @ %.0f Hz\n", m.w, m.h, m.refresh_rate);
        } else {
            SDL_SetWindowFullscreenMode(window, NULL);
            LOG("[ui] no fullscreen mode near %dx%d (%s): borderless instead\n", g_config.width, g_config.height,
                SDL_GetError());
        }
    } else if (on) {
        SDL_SetWindowFullscreenMode(window, NULL);
    }
    SDL_SetWindowFullscreen(window, on);
    if (!on && was) {  // (SDL restores the frame's size as the client size after an exclusive mode: set it again)
        SDL_SyncWindow(window);
        fit_desktop(&win_w, &win_h);
        SDL_SetWindowSize(window, win_w, win_h);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (on) SDL_HideCursor(); else SDL_ShowCursor();
}

#ifdef __ANDROID__
// The activity's lifecycle, as it happens (an event watch: these must be acted on before the call returns): the
// renderer draws nothing while the window is gone and makes its surface again after (nvn/vulkan/device.c); with
// [Application] PauseInBackground the game stops meanwhile too (runtime/hle/vi.c).
extern volatile int g_android_window_state;
void vi_set_paused(bool on);
void vk_pipeline_cache_checkpoint(void);  // (nvn/vulkan/device.c)
static bool android_lifecycle(void *ud, SDL_Event *e) {
    (void)ud;
    switch (e->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        __atomic_store_n(&g_android_window_state, 1, __ATOMIC_RELEASE);
        // (SDL's main thread now blocks until the app is back: the buttons and sticks it gave the game would stay held,
        // and the fingers' ups will not come)
        input_set(0, 0, 0, 0, 0);
        touch_reset();
        haptics_stop();  // (a rumble playing would run out its last pulse)
        if (config_live_pause_background()) vi_set_paused(true);
        // (a phone short of memory may end the app while it is away: the pipeline cache is written now, on its own thread)
        vk_pipeline_cache_checkpoint();
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        __atomic_store_n(&g_android_window_state, 2, __ATOMIC_RELEASE);
        vi_set_paused(false);  // (after the state: the renderer's first frame back makes the surface again)
        break;
    default: break;
    }
    return true;
}
#endif

void ui_init(void) {
#ifdef __ANDROID__
    // Touches are not mouse clicks here (a click is X / A and the mouse turns the camera: host_input.c), and the
    // window is the screen, in landscape.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) FATAL("SDL_Init: %s\n", SDL_GetError());
    int sdl = SDL_GetVersion();
#ifdef MHGU_SDL_BUNDLED
    extern char g_sdl_choice[];  // (runtime/platform/linux_sdl.c)
    LOG("[ui] SDL %d.%d.%d, %s; video %s\n", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl),
        SDL_VERSIONNUM_MICRO(sdl), g_sdl_choice, SDL_GetCurrentVideoDriver());
#else
    LOG("[ui] SDL %d.%d.%d; video %s\n", SDL_VERSIONNUM_MAJOR(sdl), SDL_VERSIONNUM_MINOR(sdl), SDL_VERSIONNUM_MICRO(sdl),
        SDL_GetCurrentVideoDriver());
#endif
    config_load();
    // Initial background input policy. SDL documents this hint as changeable at runtime.
    // Subsequent saved changes are consumed by the host thread in ui_run().
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_config.background_input ? "1" : "0");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) FATAL("SDL_InitSubSystem: %s\n", SDL_GetError());
    win_w = g_config.width, win_h = g_config.height;
#ifndef __ANDROID__  // (there the window is the screen)
    fit_desktop(&win_w, &win_h);
#endif
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifndef MHGU_NULL_BACKEND
    flags |= SDL_WINDOW_VULKAN;
#endif
    if (getenv("MHGU_HIDDEN")) flags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow("Monster Hunter Generations Ultimate", win_w, win_h, flags);
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    if (g_config.display_mode != DISPLAY_WINDOWED) set_fullscreen(true);
#ifdef __ANDROID__
    SDL_AddEventWatch(android_lifecycle, NULL);
#endif
}

// Alt+Enter: windowed <-> the full-screen mode of the settings (Windowed in the settings: borderless).
void ui_toggle_fullscreen(void) {
    set_fullscreen(!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
}

int ui_run(void) {
    int background = config_live_background_input();
    for (;;) {
        int wanted_background = config_live_background_input();
        if (wanted_background != background) {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, wanted_background ? "1" : "0");
            background = wanted_background;
        }
        SDL_Event e;
#ifdef __ANDROID__
        // (SDL's WaitEventTimeout spins on Android - a whole core in its joystick polling and timed mutex waits, measured
        // on a Galaxy S25: the events are taken as they are, then a 3 ms sleep)
        bool got = SDL_PollEvent(&e);
        if (!got) SDL_Delay(3), got = SDL_PollEvent(&e);
        if (got) {
#else
        if (SDL_WaitEventTimeout(&e, 4)) {
#endif
            do {
                if (e.type == SDL_EVENT_QUIT) {
                    if (!config_confirm_exit() && !overlay_settings_dirty()) { LOG("window closed\n"); exit(0); }
                    overlay_request_close();
                    continue;
                }
                host_input_event(&e);
            } while (SDL_PollEvent(&e));
        }
        int action = SDL_SetAtomicInt(&requested_action, 0);
        if (action == 1) exit(0);
#ifdef __ANDROID__
        if (action == 3) activity_call("showLicenses");
        if (action == 4) activity_call("showGameFolder");
        if (action == 5) activity_call("showSaveImport");
        if (action == 6) activity_call("showShareLog");
        if (action == 7) activity_call("showSaveExport");
        if (action == 8) activity_call("showDrivers");
        if (action == 9) activity_call("importCheats");
        if (action == 10) activity_call("importMod");
        if (action == 11) activity_call("importDlc");
        static float screen_for = -1;  // (the screen's refresh rate follows the FPS limit: GameActivity.setFrameCap)
        float fps_now = config_live_fps();
        if (fps_now != screen_for) screen_for = fps_now, frame_cap_to_activity(fps_now);
#endif
        if (action == 2) {
            if (restart_application()) exit(0);
            SDL_SetAtomicInt(&action_failed, 1);
        }
        host_input_update();
        cheats_frame();
        perfstats_poll();  // ([Performance] ShowStats: the phone's temperature, every 2 s at most)
    }
}
