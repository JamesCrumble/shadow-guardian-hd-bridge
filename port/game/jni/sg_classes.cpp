/*
 * The Java side of Shadow Guardian HD 1.0.1, as far as the engine calls it.
 *
 * The engine does not FindClass its own classes: each nativeInit receives the
 * class it was declared on and looks its callbacks up there by name. The names
 * and descriptors are in the library's .rodata next to each other
 * (`strings -a -t x libshadowguardian.so`, 360608..3607a8, 360bc0..360bdc,
 * 382c80..382d78), the shapes are confirmed in classes.dex.
 *
 *   ShadowGuardian  sendAppToBackground Exit openBrowser isWifiEnabled
 *                   GetPhoneLanguage launchGLLive getManufacture notifyTrophy
 *                   launchIGP sendTrackingInfo; and for the licence check
 *                   (ALicenseCheck, "LicenseCheck") d da db dc
 *   GLResLoader     getResourceFull getResourceBytes getResourceLength
 *   Device          IsWifiEnable getHostName, plus the obfuscated a..f
 *   Context         TELEPHONY_SERVICE, getSystemService -> TelephonyManager
 *
 * Everything answers the quietest value an offline phone would give; nothing
 * returns NULL where the Java would have returned an object.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include <string>

#include "platform.h"
#include "jni.h"
#include "jni_internals.h"
#include "app_exit.h"
#include "io_paths.h"
#include "port_env.h"
#include "trace.h"

#include "sg.h"

#define SG_CLASS(name)                          \
    class name : public Object {                \
    public:                                     \
        static Class clazz;                     \
        Class *_getClass() { return &clazz; }   \
    }

SG_CLASS(SgShadowGuardian);
SG_CLASS(SgGameRenderer);
SG_CLASS(SgGameGLSurfaceView);
SG_CLASS(SgGLResLoader);
SG_CLASS(SgDevice);
SG_CLASS(AndroidTelephonyManager);

class AndroidContext : public Object {
public:
    static Class clazz;
    Class *_getClass() { return &clazz; }
    static String TELEPHONY_SERVICE;
};

#undef SG_CLASS

#define SG_PKG "com/gameloft/android/ANMP/GloftSGHP/ML/"

static SgShadowGuardian        g_activity;
static AndroidTelephonyManager g_telephony;

static int  g_width = 1280, g_height = 720;
static bool g_exit;

void sg_set_view_size(int w, int h) { g_width = w; g_height = h; }
bool sg_exit_requested(void) { return g_exit; }

static const char *jstr(jstring s)
{
    return s ? ((String *)s)->str : "";
}

static jbyteArray bytes(JNIEnv *env, const void *data, size_t n)
{
    jbyteArray a = env->NewByteArray((jsize)n);
    if (n)
        env->SetByteArrayRegion(a, 0, (jsize)n, (const jbyte *)data);
    return a;
}

static jbyteArray cstr_bytes(JNIEnv *env, const char *s)
{
    return bytes(env, s, strlen(s));
}

/* A fixed made-up identity: the engine salts its saves with it. */
static const char kDeviceId[] = "351066496380730";

/* ------------------------------------------------------ ShadowGuardian */

static void SG_sendAppToBackground(JNIEnv *e, jclass c) { (void)e; (void)c; trace("ShadowGuardian.sendAppToBackground()"); }
static void SG_Exit(JNIEnv *e, jclass c)
{
    (void)e; (void)c;
    trace("ShadowGuardian.Exit() - the game asked to quit");
    g_exit = true;
    android_app_request_exit("ShadowGuardian.Exit");
}
static void SG_openBrowser(JNIEnv *e, jclass c, jstring u) { (void)e; (void)c; trace("ShadowGuardian.openBrowser('%s') - ignored", jstr(u)); }
static jint SG_isWifiEnabled(JNIEnv *e, jclass c) { (void)e; (void)c; return 0; }
static jint SG_GetPhoneLanguage(JNIEnv *e, jclass c)
{
    (void)e; (void)c;
    /* Java: eng 0, fra 1, spa 2, ita 3, deu 4, por/bra 5, jpn 6, kor 7. */
    return (jint)port_getenv_long("LANG_INDEX", 0);
}
static void SG_launchGLLive(JNIEnv *e, jclass c, jint i) { (void)e; (void)c; trace("ShadowGuardian.launchGLLive(%d) - ignored", i); }
/* Java: 0 other, 1 motorola, 2 samsung, 3 htc, 4 sony, 5 sharp. The build is
 * the Samsung i9000 one (source paths in .rodata), so it answers samsung. */
static jint SG_getManufacture(JNIEnv *e, jclass c) { (void)e; (void)c; return (jint)port_getenv_long("MANUFACTURE", 2); }
static void SG_notifyTrophy(JNIEnv *e, jclass c, jint t) { (void)e; (void)c; trace("ShadowGuardian.notifyTrophy(%d)", t); }
static void SG_launchIGP(JNIEnv *e, jclass c, jint i) { (void)e; (void)c; trace("ShadowGuardian.launchIGP(%d) - ignored", i); }
static void SG_sendTrackingInfo(JNIEnv *e, jclass c) { (void)e; (void)c; }
static void SG_Pause(JNIEnv *e, jclass c) { (void)e; (void)c; }
static jboolean SG_isNeedGcOnFrame(JNIEnv *e, jclass c) { (void)e; (void)c; return JNI_FALSE; }
static jlong SG_GetCurrentTime(JNIEnv *e, jclass c)
{
    (void)e; (void)c;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The licence check's view of the activity. d() starts Tracking on Android;
 * db()/dc() return identity bytes (Device.a(), the GloftDRM key). */
static void SG_d(JNIEnv *e, jclass c) { (void)e; (void)c; trace("ShadowGuardian.d()"); }
static jobject SG_da(JNIEnv *e, jclass c) { (void)e; (void)c; trace("ShadowGuardian.da() -> activity"); return (jobject)&g_activity; }
static jbyteArray SG_db(JNIEnv *env, jclass c) { (void)c; trace("ShadowGuardian.db()"); return cstr_bytes(env, kDeviceId); }
static jbyteArray SG_dc(JNIEnv *env, jclass c) { (void)c; trace("ShadowGuardian.dc()"); return cstr_bytes(env, kDeviceId); }

static jobject Ctx_getSystemService(JNIEnv *env, jobject self, jstring name)
{
    (void)env; (void)self;
    trace("Context.getSystemService('%s')", jstr(name));
    return (jobject)&g_telephony;
}

const ManagedMethod sgShadowGuardianMethods[] = {
    ManagedMethod::RegisterStatic<&SG_sendAppToBackground>(SgShadowGuardian::clazz, "sendAppToBackground", "()V"),
    ManagedMethod::RegisterStatic<&SG_Exit>(SgShadowGuardian::clazz, "Exit", "()V"),
    ManagedMethod::RegisterStatic<&SG_openBrowser>(SgShadowGuardian::clazz, "openBrowser", "(Ljava/lang/String;)V"),
    ManagedMethod::RegisterStatic<&SG_isWifiEnabled>(SgShadowGuardian::clazz, "isWifiEnabled", "()I"),
    ManagedMethod::RegisterStatic<&SG_GetPhoneLanguage>(SgShadowGuardian::clazz, "GetPhoneLanguage", "()I"),
    ManagedMethod::RegisterStatic<&SG_launchGLLive>(SgShadowGuardian::clazz, "launchGLLive", "(I)V"),
    ManagedMethod::RegisterStatic<&SG_getManufacture>(SgShadowGuardian::clazz, "getManufacture", "()I"),
    ManagedMethod::RegisterStatic<&SG_notifyTrophy>(SgShadowGuardian::clazz, "notifyTrophy", "(I)V"),
    ManagedMethod::RegisterStatic<&SG_launchIGP>(SgShadowGuardian::clazz, "launchIGP", "(I)V"),
    ManagedMethod::RegisterStatic<&SG_sendTrackingInfo>(SgShadowGuardian::clazz, "sendTrackingInfo", "()V"),
    ManagedMethod::RegisterStatic<&SG_Pause>(SgShadowGuardian::clazz, "Pause", "()V"),
    ManagedMethod::RegisterStatic<&SG_isNeedGcOnFrame>(SgShadowGuardian::clazz, "isNeedGcOnFrame", "()Z"),
    ManagedMethod::RegisterStatic<&SG_GetCurrentTime>(SgShadowGuardian::clazz, "GetCurrentTime", "()J"),
    ManagedMethod::RegisterStatic<&SG_d>(SgShadowGuardian::clazz, "d", "()V"),
    ManagedMethod::RegisterStatic<&SG_da>(SgShadowGuardian::clazz, "da", "()Landroid/app/Activity;"),
    ManagedMethod::RegisterStatic<&SG_db>(SgShadowGuardian::clazz, "db", "()[B"),
    ManagedMethod::RegisterStatic<&SG_dc>(SgShadowGuardian::clazz, "dc", "()[B"),
    ManagedMethod::Register<&Ctx_getSystemService>(SgShadowGuardian::clazz, "getSystemService",
                                                  "(Ljava/lang/String;)Ljava/lang/Object;"),
    {NULL},
};

Class SgShadowGuardian::clazz = {
    .classpath       = SG_PKG "ShadowGuardian",
    .classname       = "ShadowGuardian",
    .managed_methods = sgShadowGuardianMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(SgShadowGuardian),
};

Class SgGameRenderer::clazz = {
    .classpath       = SG_PKG "GameRenderer",
    .classname       = "GameRenderer",
    .managed_methods = {NULL},
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(SgGameRenderer),
};

Class SgGameGLSurfaceView::clazz = {
    .classpath       = SG_PKG "GameGLSurfaceView",
    .classname       = "GameGLSurfaceView",
    .managed_methods = {NULL},
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(SgGameGLSurfaceView),
};

/* --------------------------------------------------------- GLResLoader */

/*
 * GLResLoader.getResourceFull(name), as the Java does it:
 *   ".//dir/name.ext" -> res/drawable/res_<name> (raw resource by base name)
 *   "./name"          -> assets/name of the APK
 * The extracted APK sits in the game directory (assets/, res/).
 */
static std::string resource_path(const char *name)
{
    std::string n(name);
    std::string game(io_game_dir());
    if (n.compare(0, 3, ".//") == 0) {
        std::string base = n.substr(3);
        size_t slash = base.rfind('/');
        if (slash != std::string::npos)
            base = base.substr(slash + 1);
        size_t dot = base.find('.');
        std::string stem = base.substr(0, dot);
        for (auto &ch : stem)
            ch = (char)tolower((unsigned char)ch);
        std::string ext = dot == std::string::npos ? "" : base.substr(dot);
        for (const char *d : {"res/drawable", "res/raw"}) {
            std::string p = game + "/" + d + "/res_" + stem + ext;
            struct stat st;
            if (stat(p.c_str(), &st) == 0)
                return p;
        }
        return game + "/res/drawable/res_" + stem + ext;
    }
    if (n.compare(0, 2, "./") == 0)
        n = n.substr(2);
    return game + "/assets/" + n;
}

static bool read_whole(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

static jbyteArray Res_getResourceFull(JNIEnv *env, jclass c, jstring name)
{
    (void)c;
    std::string data, path = resource_path(jstr(name));
    if (!read_whole(path, data)) {
        trace("GLResLoader.getResourceFull('%s') - %s missing, answered empty", jstr(name), path.c_str());
        return env->NewByteArray(0);
    }
    trace("GLResLoader.getResourceFull('%s') -> %zu bytes", jstr(name), data.size());
    return bytes(env, data.data(), data.size());
}

static jbyteArray Res_getResourceBytes(JNIEnv *env, jclass c, jstring name, jint off, jint len)
{
    (void)c;
    std::string data;
    read_whole(resource_path(jstr(name)), data);
    if (off < 0 || len < 0 || (size_t)off > data.size())
        return env->NewByteArray(0);
    size_t n = std::min((size_t)len, data.size() - (size_t)off);
    return bytes(env, data.data() + off, n);
}

static jint Res_getResourceLength(JNIEnv *env, jclass c, jstring name)
{
    (void)env; (void)c;
    struct stat st;
    std::string path = resource_path(jstr(name));
    jint n = stat(path.c_str(), &st) == 0 ? (jint)st.st_size : 0;
    trace("GLResLoader.getResourceLength('%s') -> %d", jstr(name), n);
    return n;
}

const ManagedMethod sgResLoaderMethods[] = {
    ManagedMethod::RegisterStatic<&Res_getResourceFull>(SgGLResLoader::clazz, "getResourceFull", "(Ljava/lang/String;)[B"),
    ManagedMethod::RegisterStatic<&Res_getResourceBytes>(SgGLResLoader::clazz, "getResourceBytes", "(Ljava/lang/String;II)[B"),
    ManagedMethod::RegisterStatic<&Res_getResourceLength>(SgGLResLoader::clazz, "getResourceLength", "(Ljava/lang/String;)I"),
    {NULL},
};

Class SgGLResLoader::clazz = {
    .classpath       = SG_PKG "GLResLoader",
    .classname       = "GLResLoader",
    .managed_methods = sgResLoaderMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(SgGLResLoader),
};

/* -------------------------------------------------------------- Device */

static jboolean Dev_IsWifiEnable(JNIEnv *e, jclass c) { (void)e; (void)c; return JNI_FALSE; }
static jbyteArray Dev_getHostName(JNIEnv *env, jclass c) { (void)c; return cstr_bytes(env, "GT-I9000_GT-I9000"); }
/* a: IMEI, b: operator, c: line number, d: game version, f: user agent - in
 * Device.InitDeviceValues order. Offline values. */
static jbyteArray Dev_a(JNIEnv *env, jclass c) { (void)c; return cstr_bytes(env, kDeviceId); }
static jbyteArray Dev_b(JNIEnv *env, jclass c) { (void)c; return cstr_bytes(env, ""); }
static jbyteArray Dev_c(JNIEnv *env, jclass c) { (void)c; return cstr_bytes(env, ""); }
static jbyteArray Dev_d(JNIEnv *env, jclass c) { (void)c; return cstr_bytes(env, "1.0.1"); }
static jbyteArray Dev_f(JNIEnv *env, jclass c) { (void)c; return cstr_bytes(env, "GT-I9000"); }
static void Dev_e(JNIEnv *e, jclass c, jint v) { (void)e; (void)c; (void)v; }
static jobject Dev_getUserAgent(JNIEnv *env, jclass c) { (void)c; return (jobject)env->NewStringUTF("GT-I9000"); }
static jobject Dev_getIMEI(JNIEnv *env, jclass c) { (void)c; return (jobject)env->NewStringUTF(kDeviceId); }

const ManagedMethod sgDeviceMethods[] = {
    ManagedMethod::RegisterStatic<&Dev_IsWifiEnable>(SgDevice::clazz, "IsWifiEnable", "()Z"),
    ManagedMethod::RegisterStatic<&Dev_getHostName>(SgDevice::clazz, "getHostName", "()[B"),
    ManagedMethod::RegisterStatic<&Dev_a>(SgDevice::clazz, "a", "()[B"),
    ManagedMethod::RegisterStatic<&Dev_b>(SgDevice::clazz, "b", "()[B"),
    ManagedMethod::RegisterStatic<&Dev_c>(SgDevice::clazz, "c", "()[B"),
    ManagedMethod::RegisterStatic<&Dev_d>(SgDevice::clazz, "d", "()[B"),
    ManagedMethod::RegisterStatic<&Dev_f>(SgDevice::clazz, "f", "()[B"),
    ManagedMethod::RegisterStatic<&Dev_e>(SgDevice::clazz, "e", "(I)V"),
    ManagedMethod::RegisterStatic<&Dev_getUserAgent>(SgDevice::clazz, "getUserAgent", "()Ljava/lang/String;"),
    ManagedMethod::RegisterStatic<&Dev_getIMEI>(SgDevice::clazz, "getIMEI", "()Ljava/lang/String;"),
    {NULL},
};

Class SgDevice::clazz = {
    .classpath       = SG_PKG "GLUtils/Device",
    .classname       = "Device",
    .managed_methods = sgDeviceMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(SgDevice),
};

/* ------------------------------------------- Context / TelephonyManager */

String AndroidContext::TELEPHONY_SERVICE("phone");

const FieldId androidContextFields[] = {
    REGISTER_STATIC_FIELD(AndroidContext, TELEPHONY_SERVICE),
    {NULL},
};

const ManagedMethod androidContextMethods[] = {
    ManagedMethod::Register<&Ctx_getSystemService>(AndroidContext::clazz, "getSystemService",
                                                  "(Ljava/lang/String;)Ljava/lang/Object;"),
    {NULL},
};

Class AndroidContext::clazz = {
    .classpath       = "android/content/Context",
    .classname       = "Context",
    .managed_methods = androidContextMethods,
    .native_methods  = {NULL},
    .fields          = androidContextFields,
    .instance_size   = sizeof(AndroidContext),
};

static jobject Tel_getDeviceId(JNIEnv *env, jobject self)
{
    (void)self;
    return (jobject)env->NewStringUTF(kDeviceId);
}

const ManagedMethod androidTelephonyMethods[] = {
    ManagedMethod::Register<&Tel_getDeviceId>(AndroidTelephonyManager::clazz, "getDeviceId", "()Ljava/lang/String;"),
    {NULL},
};

Class AndroidTelephonyManager::clazz = {
    .classpath       = "android/telephony/TelephonyManager",
    .classname       = "TelephonyManager",
    .managed_methods = androidTelephonyMethods,
    .native_methods  = {NULL},
    .fields          = {NULL},
    .instance_size   = sizeof(AndroidTelephonyManager),
};

/* ------------------------------------------------------------ registry */

void *sg_class_shadowguardian(void) { return &SgShadowGuardian::clazz; }
void *sg_class_renderer(void)       { return &SgGameRenderer::clazz; }
void *sg_class_surfaceview(void)    { return &SgGameGLSurfaceView::clazz; }
void *sg_class_resloader(void)      { return &SgGLResLoader::clazz; }
void *sg_class_device(void)         { return &SgDevice::clazz; }

static int register_sg_classes(void)
{
    ClassRegistry::register_class(SgShadowGuardian::clazz);
    ClassRegistry::register_class(SgGameRenderer::clazz);
    ClassRegistry::register_class(SgGameGLSurfaceView::clazz);
    ClassRegistry::register_class(SgGLResLoader::clazz);
    ClassRegistry::register_class(SgDevice::clazz);
    ClassRegistry::register_class(AndroidContext::clazz);
    ClassRegistry::register_class(AndroidTelephonyManager::clazz);
    return 0;
}

static const int registered = register_sg_classes();
