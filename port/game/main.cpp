/*
 * Shadow Guardian HD 1.0.1 (Gameloft, 2011) - entry point.
 *
 * The game is driven, not hosted: libshadowguardian.so exports JNI_OnLoad and
 * a set of Java_* entry points, and on Android its Java layer calls them. Here
 * that Java layer is this file. Engine: Gameloft's own "pig" (pig::, clara::
 * levels, vox:: sound, Lua scripts), GLES2 only - the GLESv1_CM in DT_NEEDED
 * has no imports behind it.
 *
 *   $ readelf -d libshadowguardian.so | grep NEEDED
 *   libc libstdc++ libm libGLESv1_CM libGLESv2 libdl liblog
 *
 * The order below is the APK's own (classes.dex):
 *
 *   ShadowGuardian.onCreate     nativeGetInfo(country, deviceId, manufacturer_model,
 *                               release, MANUFACTURER, MODEL, PRODUCT),
 *                               GLResLoader.init -> nativeInit, Device.init -> nativeInit
 *   GameRenderer.onSurfaceCreated   nativeInit(manufacture, width, height, language),
 *                                   then ShadowGuardian.nativeInit()
 *   GameRenderer.onSurfaceChanged   nativeResize(width, height)
 *   GameRenderer.onDrawFrame        nativeRender()
 *
 * The frame is shown by GLSurfaceView after every onDrawFrame; the library
 * imports no EGL at all, so the port swaps after each nativeRender.
 *
 * The APK also loads libStormGLOFT.so: a third-party "All GPU" shim (inline
 * hooks plus a GLSL compiler that rewrites textures and shaders for other
 * GPUs). The GLES bridge plays that role here, so it is not loaded.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "port_version.h"
#include "so_util.h"
#include "khronos/gles2.h"

#include "jni.h"

#include "app_exit.h"
#include "crash.h"
#include "emulator_control.h"
#include "fix_path.h"
#include "input_bridge.h"
#include "port_env.h"
#include "trace.h"
#include "gl_bridge.h"

#include "sg.h"

extern "C" long android_gl_textures_uploaded(void);
extern "C" long android_gl_draw_calls(void);
extern "C" void viewport_scale_init(int phys_w, int phys_h);

static const char *kNativeLib    = "libshadowguardian.so";
static const char *kNativeLibDir = "lib/armeabi-v7a";

/* The Galaxy S panel this build was made for. The Android wrapper overrides it
 * (SG_WIDTH / SG_HEIGHT) to the phone's aspect ratio. */
static int kWidth  = 800;
static int kHeight = 480;

/* ---------------------------------------------------------- the module */

static so_module *g_module;

so_module *sg_module(void) { return g_module; }

/* One of portbase's ten; C++ linkage on purpose (src/symtab_pthread.cpp). */
so_module *port_guest_module(void) { return g_module; }

/* Every import nothing answers, listed at once rather than one per crash.
 * Undefined weak symbols resolve to zero, as with a real linker. */
static int report_unresolved_symbols(so_module *mod)
{
    int missing = 0;
    for (int i = 0; i < mod->num_dynsym; i++) {
        Elf_Sym *sym = &mod->dynsym[i];
        if (sym->st_shndx != SHN_UNDEF)
            continue;
        const char *name = mod->dynstr + sym->st_name;
        if (!name || !*name || so_resolve_link(mod, name))
            continue;
        if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
            trace("weak import left null: %s", name);
            continue;
        }
        fprintf(stderr, "unresolved symbol: %s\n", name);
        missing++;
    }
    fflush(stderr);
    return missing;
}

extern "C" int so_after_relocate(so_module *mod)
{
    g_module = mod;
    trace("module loaded");

    /* Before INIT_ARRAY (860 bytes of static constructors) runs. */
    crash_report_init(mod, kNativeLib);

    int missing = report_unresolved_symbols(mod);
    if (missing == 0)
        return 0;
    fatal("%d import(s) of %s have no implementation (listed above).",
          missing, mod->soname ? mod->soname : "the module");
    return 1;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        printf("%s\n", SG_PORT_VERSION);
        return 0;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    trace("Shadow Guardian HD port v%s", SG_PORT_VERSION);

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <game-directory>\n"
                "\n"
                "The directory holds your own copy of the game: lib/armeabi-v7a/\n"
                "and assets/ from the APK, and the cache - GloftSGHP.zip as it is,\n"
                "or unpacked into GloftSGHP/.\n", argv[0]);
        return 2;
    }

    const char *game_dir = argv[1];
    io_set_game_dir(game_dir);
    sg_prepare_storage();
    sg_zip_mount_open(port_getenv("CACHE_ZIP"));

    {
        long w = port_getenv_long("WIDTH", kWidth), h = port_getenv_long("HEIGHT", kHeight);
        if (w >= 320 && w <= 4096 && h >= 240 && h <= 4096) {
            kWidth = (int)w;
            kHeight = (int)h;
        }
        char dim[16];
        snprintf(dim, sizeof(dim), "%d", kWidth);
        setenv(PORT_ENV_PREFIX "_SCREEN_W", dim, 0);
        snprintf(dim, sizeof(dim), "%d", kHeight);
        setenv(PORT_ENV_PREFIX "_SCREEN_H", dim, 0);
    }
    sg_set_view_size(kWidth, kHeight);

    char lib_dir[PATH_MAX], lib_path[PATH_MAX];
    snprintf(lib_dir,  sizeof(lib_dir),  "%s/%s", game_dir, kNativeLibDir);
    snprintf(lib_path, sizeof(lib_path), "%s/%s", lib_dir, kNativeLib);

    struct stat st;
    if (stat(lib_path, &st) != 0) {
        fatal("'%s' does not exist.\n"
              "       This does not look like an extracted Shadow Guardian HD tree.", lib_path);
        return 1;
    }
    trace("native library found: %s (%lld bytes)", lib_path, (long long)st.st_size);
    trace("data directory: %s", sg_data_dir());

    /* GL before the module is linked: its gl* imports are bound by asking the
     * driver (or the bridge) for each entry point. */
    SDL_Window   *window = NULL;
    SDL_GLContext gl     = NULL;

    const Uint32 sdl_flags = SDL_INIT_VIDEO;
    if (SDL_Init(sdl_flags) != 0) {
        trace("SDL_Init(0x%x) failed: %s", sdl_flags, SDL_GetError());
    } else {
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

        window = SDL_CreateWindow("Shadow Guardian HD",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  kWidth, kHeight,
                                  gl_bridge_enabled() ? 0 : SDL_WINDOW_OPENGL);
        if (!window)
            trace("SDL_CreateWindow failed: %s", SDL_GetError());
        else if (gl_bridge_enabled()) {
            if (!gl_bridge_init()) {
                fatal("could not connect to the GL host.");
                return 1;
            }
            gl = (SDL_GLContext)(uintptr_t)1;
        }
        else if (!(gl = SDL_GL_CreateContext(window)))
            trace("no GLES 2.0 context: %s", SDL_GetError());
    }

    if (gl) {
        load_gles2_funcs();
        /* portbase's own diagnostics resolve through the GLES1 table; keep both
         * in the same dispatch as the GLES2 context (see NOVA 3's main.cpp). */
        setenv(PORT_ENV_PREFIX "_GL_SINGLE_DISPATCH", "1", 0);
        load_gles1_funcs();

        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window, &w, &h);
        trace("drawable %dx%d", w, h);
        viewport_scale_init(w, h);

        const GLubyte *(*get_string)(GLenum) =
            (const GLubyte *(*)(GLenum))SDL_GL_GetProcAddress("glGetString");
        if (get_string) {
            const char *ver = (const char *)get_string(0x1F02);
            const char *rnd = (const char *)get_string(0x1F01);
            trace("GL_VERSION=%s | GL_RENDERER=%s", ver ? ver : "?", rnd ? rnd : "?");
        }
    }

    JavaVM *vm  = NULL;
    JNIEnv *env = NULL;
    if (JNI_CreateJavaVM(&vm, &env, NULL) != JNI_OK || !vm || !env) {
        fatal("could not create the JNI environment.");
        return 1;
    }

    /* NULL VM: JNI_OnLoad is called below, on this thread, in Java's order. */
    so_set_options(NULL, lib_dir);
    so_module *mod = so_load_module(kNativeLib, NULL, NULL);
    if (!mod) {
        fatal("could not load '%s' from '%s'.", kNativeLib, lib_dir);
        fflush(NULL);
        _exit(1);
    }
    trace("so_load_module returned (text_base=%p size=%zu)",
          (void *)mod->text_base, (size_t)mod->text_size);
    trace("port main() is at %p", (void *)&main);

    /* ---------------------------------------------------------------- *
     * The boot sequence, in the game's own order.
     * ---------------------------------------------------------------- */

    typedef void (*fn_v)(JNIEnv *, jclass);
    typedef void (*fn_ii)(JNIEnv *, jclass, jint, jint);
    typedef void (*fn_iiii)(JNIEnv *, jclass, jint, jint, jint, jint);
    typedef void (*fn_info)(JNIEnv *, jclass, jstring, jstring, jstring, jstring,
                            jstring, jstring, jstring);

    auto JNI_OnLoad     = (int (*)(JavaVM *, void *))so_symbol(mod, "JNI_OnLoad");
    auto sg_get_info    = (fn_info)so_symbol(mod, SG_JNI("ShadowGuardian", "nativeGetInfo"));
    auto res_init       = (fn_v)   so_symbol(mod, SG_JNI("GLResLoader", "nativeInit"));
    auto device_init    = (fn_v)   so_symbol(mod, SG_JNI("GLUtils_Device", "nativeInit"));
    auto renderer_init  = (fn_iiii)so_symbol(mod, SG_JNI("GameRenderer", "nativeInit"));
    auto sg_init        = (fn_v)   so_symbol(mod, SG_JNI("ShadowGuardian", "nativeInit"));
    auto renderer_resize= (fn_ii)  so_symbol(mod, SG_JNI("GameRenderer", "nativeResize"));
    auto renderer_render= (fn_v)   so_symbol(mod, SG_JNI("GameRenderer", "nativeRender"));

    if (!JNI_OnLoad || !renderer_init || !sg_init || !renderer_render) {
        fatal("%s is missing an entry point the port drives:\n"
              "       JNI_OnLoad=%p GameRenderer.nativeInit=%p ShadowGuardian.nativeInit=%p nativeRender=%p",
              kNativeLib, (void *)JNI_OnLoad, (void *)renderer_init, (void *)sg_init,
              (void *)renderer_render);
        fflush(NULL);
        _exit(1);
    }

    jclass cls_sg       = (jclass)sg_class_shadowguardian();
    jclass cls_renderer = (jclass)sg_class_renderer();
    jclass cls_res      = (jclass)sg_class_resloader();
    jclass cls_device   = (jclass)sg_class_device();

    trace("-> JNI_OnLoad at +0x%08lx", (unsigned long)((uintptr_t)JNI_OnLoad - mod->text_base));
    JNI_OnLoad(vm, NULL);
    trace("JNI_OnLoad returned");

    if (sg_get_info) {
        /* onCreate: country, device id ("GL_00" when the IMEI is unreadable),
         * MANUFACTURER_MODEL, Build.VERSION.RELEASE, MANUFACTURER, MODEL, PRODUCT. */
        /* The engine picks its shader set by device: a Galaxy S (GT-I9000) reads shaders_galaxy_s/, anything
         * else shaders/. The original cache ships both, the community one (UK+RU) only shaders/ - with a
         * Galaxy S it finds no render passes and crashes at the first menu material. So the device follows
         * the data; SG_DEVICE_MAKER / SG_DEVICE_MODEL override it. */
        const bool galaxy = sg_data_has_dir("shaders_galaxy_s");
        const char *maker = port_getenv("DEVICE_MAKER"), *model = port_getenv("DEVICE_MODEL");
        if (!maker || !*maker) maker = galaxy ? "samsung" : "OnePlus";
        if (!model || !*model) model = galaxy ? "GT-I9000" : "CPH2655";
        trace("device for the engine: %s %s (%s)", maker, model,
              galaxy ? "the data has shaders_galaxy_s/" : "no shaders_galaxy_s/ in the data: shaders/");
        char combined[96];
        snprintf(combined, sizeof(combined), "%s_%s", maker, model);
        sg_get_info(env, cls_sg,
                    env->NewStringUTF("US"), env->NewStringUTF("GL_00"),
                    env->NewStringUTF(combined), env->NewStringUTF("2.3.6"),
                    env->NewStringUTF(maker), env->NewStringUTF(model),
                    env->NewStringUTF(model));
        trace("ShadowGuardian.nativeGetInfo returned");
    }
    if (res_init) {
        res_init(env, cls_res);
        trace("GLResLoader.nativeInit returned");
    }
    if (device_init) {
        device_init(env, cls_device);
        trace("Device.nativeInit returned");
    }

    android_input_init(mod, env, kWidth, kHeight);

    const int manufacture = (int)port_getenv_long("MANUFACTURE", 2);
    const int language    = (int)port_getenv_long("LANG_INDEX", 0);
    trace("-> GameRenderer.nativeInit(%d, %d, %d, %d)", manufacture, kWidth, kHeight, language);
    renderer_init(env, cls_renderer, manufacture, kWidth, kHeight, language);
    trace("GameRenderer.nativeInit returned");

    sg_init(env, cls_sg);
    trace("ShadowGuardian.nativeInit returned");

    if (renderer_resize) {
        renderer_resize(env, cls_renderer, kWidth, kHeight);
        trace("GameRenderer.nativeResize(%d, %d) returned", kWidth, kHeight);
    }

    emulator_control_init();

    /* SG_FRAME_LIMIT stops after that many frames (automated runs). */
    const long frame_limit = port_getenv_long("FRAME_LIMIT", 0);

    long frames = 0;
    while (frame_limit == 0 || frames < frame_limit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (!android_input_event(&ev))
                goto done;
        }
        if (android_app_exit_requested() || sg_exit_requested())
            goto done;
        if (!emulator_control_tick(frames))
            goto done;
        android_input_tick();

        if (frames < 5)
            trace("-> GameRenderer.nativeRender #%ld", frames + 1);

        gl_bridge_profile_step(1);
        renderer_render(env, cls_renderer);
        gl_bridge_profile_step(0);
        frames++;

        if (frames <= 5)
            trace("<- GameRenderer.nativeRender #%ld returned", frames);

        if (window) {
            int w = 0, h = 0;
            SDL_GL_GetDrawableSize(window, &w, &h);
            emulator_control_after_draw(frames, w, h);
            if (gl_bridge_enabled())
                gl_bridge_swap();
            else
                SDL_GL_SwapWindow(window);
        }

        if (frames <= 5 || frames % 100 == 0)
            trace("frames=%ld textures=%ld draws=%ld zip=%ld/%ld", frames,
                  android_gl_textures_uploaded(), android_gl_draw_calls(),
                  sg_zip_mount_served(), sg_zip_mount_inflated());
    }

done:
    emulator_control_shutdown(frames);
    trace("summary frames=%ld textures=%ld draws=%ld zip served=%ld inflated=%ld",
          frames, android_gl_textures_uploaded(), android_gl_draw_calls(),
          sg_zip_mount_served(), sg_zip_mount_inflated());
    trace("run finished: %ld frames (%s)", frames,
          android_app_exit_requested() || sg_exit_requested() ? "the game asked to exit"
                                                              : "the loader stopped driving it");
    /* The engine's own threads are still running; leave teardown to the kernel. */
    fflush(NULL);
    _exit(0);
}
