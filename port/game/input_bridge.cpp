/*
 * Input for Shadow Guardian HD: a touch phone.
 *
 * The game draws its own on-screen controls and takes raw touches through
 * GameGLSurfaceView.nativeOnTouch(action, x, y, pointerId). The action codes
 * are the Java layer's own, read out of GameGLSurfaceView.onTouchEvent in the
 * dex: 1 a finger down (ACTION_DOWN / ACTION_POINTER_DOWN), 2 a move (sent for
 * every finger), 0 a finger up. Keys go to ShadowGuardian.nativeOnKeyDown/Up
 * with Android key codes (4 = Back).
 *
 * The host sends "touch a x y id" and "key code down|up" lines; on the PC the
 * mouse is one finger. Gamepad controls are not mapped yet.
 */

#include <string.h>

#include <SDL2/SDL.h>

#include "so_util.h"
#include "jni.h"
#include "input_bridge.h"
#include "trace.h"

#include "sg.h"

typedef void (*fn_touch_t)(JNIEnv *, jclass, jint, jint, jint, jint);
typedef void (*fn_key_t)(JNIEnv *, jclass, jint);

static JNIEnv    *g_env;
static fn_touch_t g_touch;
static fn_key_t   g_key_down, g_key_up;
static int        g_width, g_height;
static long       g_ticks;
static float      g_cursor_x, g_cursor_y;
static bool       g_mouse_down;

void android_input_init(so_module *mod, _JNIEnv *env, int width, int height)
{
    g_env    = (JNIEnv *)env;
    g_width  = width;
    g_height = height;
    g_touch    = (fn_touch_t)so_symbol(mod, SG_JNI("GameGLSurfaceView", "nativeOnTouch"));
    g_key_down = (fn_key_t)  so_symbol(mod, SG_JNI("ShadowGuardian", "nativeOnKeyDown"));
    g_key_up   = (fn_key_t)  so_symbol(mod, SG_JNI("ShadowGuardian", "nativeOnKeyUp"));
    trace("input: touch=%p keys=%p/%p", (void *)g_touch, (void *)g_key_down, (void *)g_key_up);
}

/* The engine's App exists after the first rendered frames (nativeRender runs
 * appInit); touches before that would land on null state. */
static bool ready(void) { return g_ticks >= 30; }

extern "C" float gl_screen_aspect_fix(void);
extern "C" float gl_ui_unmap_x(float nd, float ny, int id, int action);
extern "C" int gl_screen_panel_width(void);

bool android_input_inject_touch(int action, int x, int y, int id)
{
    if (!ready() || !g_touch)
        return false;
    /* Screen adaptation: the wrapper sends x in panel pixels; the interface was moved per element when it was drawn,
     * so the touch goes back through the same layout (gl_bridge.cpp) to the engine's own 5:3 pixel. */
    if (gl_screen_aspect_fix() < 0.999f && gl_screen_panel_width() > 0 && g_width > 0 && g_height > 0) {
        float nd = 2.0f * (float)x / (float)gl_screen_panel_width() - 1.0f;
        float ny = 1.0f - 2.0f * (float)y / (float)g_height;
        float n = gl_ui_unmap_x(nd, ny, id, action);
        n = n < -1.0f ? -1.0f : n > 1.0f ? 1.0f : n;
        x = (int)((n + 1.0f) * 0.5f * (float)g_width + 0.5f);
    }
    g_touch(g_env, (jclass)sg_class_surfaceview(), action, x, y, id);
    return true;
}

bool android_input_inject_key(int code, bool down)
{
    if (!ready())
        return false;
    fn_key_t f = down ? g_key_down : g_key_up;
    trace("input: key %d %s", code, down ? "down" : "up");
    if (f)
        f(g_env, (jclass)sg_class_shadowguardian(), code);
    return f != NULL;
}

bool android_input_inject_control(const char *name, bool down)
{
    if (name && (strcmp(name, "b") == 0 || strcmp(name, "select") == 0))
        return android_input_inject_key(4 /* KEYCODE_BACK */, down);
    return false;
}

bool android_input_inject_stick(const char *name, float x, float y)
{
    (void)name; (void)x; (void)y;
    return false;
}

void android_input_set_pad(bool connected) { (void)connected; }
bool android_input_in_menu(void) { return false; }

bool android_input_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_QUIT:
        return false;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (ev->button.button == SDL_BUTTON_LEFT) {
            g_mouse_down = ev->type == SDL_MOUSEBUTTONDOWN;
            android_input_inject_touch(g_mouse_down ? 1 : 0, ev->button.x, ev->button.y, 0);
        }
        break;
    case SDL_MOUSEMOTION:
        if (g_mouse_down)
            android_input_inject_touch(2, ev->motion.x, ev->motion.y, 0);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (ev->key.keysym.sym == SDLK_ESCAPE)
            android_input_inject_key(4, ev->type == SDL_KEYDOWN);
        break;
    default:
        break;
    }
    return true;
}

void android_input_tick(void) { g_ticks++; }

/* The cursor overlay and the autopilot are not used by this port. */
extern "C" void android_input_cursor_position(float *x, float *y, int *visible)
{
    if (x) *x = g_cursor_x;
    if (y) *y = g_cursor_y;
    if (visible) *visible = 0;
}
void android_input_cursor_set(float x, float y) { g_cursor_x = x; g_cursor_y = y; }
void android_input_cursor_press(bool down)
{
    android_input_inject_touch(down ? 1 : 0, (int)g_cursor_x, (int)g_cursor_y, 0);
}
void android_input_autopilot_tick(long frame) { (void)frame; }
void android_input_autopilot_sample(long frame) { (void)frame; }
long android_input_autopilot_keys(void) { return 0; }
long android_input_autopilot_scenes(void) { return 0; }
