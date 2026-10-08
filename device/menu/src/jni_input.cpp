#include "jni_input.h"
#include "config.h"
#include "log.h"

#include "imgui.h"

#include <dlfcn.h>
#include <jni.h>

#include <atomic>
#include <mutex>

namespace nh::jni_input {

// Not in an anonymous namespace: the extern "C" JNI entry points at the bottom of
// this file have to reach them.
namespace detail {

JavaVM *g_vm = nullptr;
std::atomic<bool> g_active{false};
std::atomic<bool> g_repack{false}; // set by JNI_OnLoad (repack loads us via System.loadLibrary)

struct Touch {
    int action = -1;
    float x = 0.0f;
    float y = 0.0f;
    int vw = 0;
    int vh = 0;
};

std::mutex g_mtx;
Touch g_last; // written by the UI thread, read by the render thread

using GetCreatedJavaVMs_t = jint (*)(JavaVM **, jsize, jsize *);

// The repacked APK loads us through System.loadLibrary, so JNI_OnLoad hands us the
// VM. The ptrace route never runs JNI_OnLoad, so fall back to asking the runtime.
JavaVM *acquire_vm() {
    if (g_vm)
        return g_vm;
    void *art = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
    if (!art)
        art = dlopen("libart.so", RTLD_NOW);
    if (!art) {
        NH_LOGW("libart.so not mapped - not a Java process?");
        return nullptr;
    }
    auto fn = (GetCreatedJavaVMs_t)dlsym(art, "JNI_GetCreatedJavaVMs");
    if (!fn) {
        NH_LOGW("JNI_GetCreatedJavaVMs not exported by libart.so");
        return nullptr;
    }
    JavaVM *vm = nullptr;
    jsize n = 0;
    if (fn(&vm, 1, &n) != JNI_OK || n < 1 || !vm) {
        NH_LOGW("no created JavaVM in this process");
        return nullptr;
    }
    g_vm = vm;
    return g_vm;
}

void publish(int action, float x, float y, int vw, int vh) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_last.action = action;
    g_last.x = x;
    g_last.y = y;
    g_last.vw = vw;
    g_last.vh = vh;
}

} // namespace detail

bool start() {
    JavaVM *vm = detail::acquire_vm();
    if (!vm)
        return false;

    // Repack path: System.loadLibrary ran JNI_OnLoad, so nh.NhTouch lives in the
    // app's classloader and its dispatchTouchEvent calls our exported onMotion.
    // FindClass from this worker pthread would use the system classloader and miss
    // it, so do not gate on it here; NhLoader already drives NhTouch.install.
    if (detail::g_repack.load()) {
        detail::g_active.store(true);
        NH_LOGI("JNI touch bridge up (repack: nh.NhTouch drives onMotion)");
        return true;
    }

    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            NH_LOGW("AttachCurrentThread failed");
            return false;
        }
        // Deliberately never detached: this worker thread lives for the process
        // lifetime, and detaching would invalidate the JNIEnv we rely on.
    }

    jclass cls = env->FindClass("nh/NhTouch");
    if (!cls) {
        env->ExceptionClear();
        NH_LOGW("nh/NhTouch not found - ptrace build rather than the repacked APK");
        return false;
    }

    jmethodID install = env->GetStaticMethodID(cls, "install", "(Landroid/content/Context;)V");
    if (!install) {
        env->ExceptionClear();
        NH_LOGE("nh/NhTouch.install missing");
        return false;
    }

    // null Context: NhTouch falls back to ActivityThread.currentActivityThread()
    env->CallStaticVoidMethod(cls, install, (jobject) nullptr);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    detail::g_active.store(true);
    NH_LOGI("JNI touch bridge up (nh/NhTouch)");
    return true;
}

void stop() { detail::g_active.store(false); }
bool is_active() { return detail::g_active.load(); }
const char *name() { return "jni:nh/NhTouch"; }

void update(ImGuiIO &io, float display_w, float display_h) {
    detail::Touch t;
    {
        std::lock_guard<std::mutex> lk(detail::g_mtx);
        t = detail::g_last;
    }

    if (t.action < 0) {
        // No event yet. Park the cursor off-screen so ImGui does not treat (0,0)
        // as a permanent hover on the top-left corner.
        io.MousePos = ImVec2(-1.0f, -1.0f);
        io.MouseDown[0] = false;
        return;
    }

    float nx, ny;
    if (t.vw > 0 && t.vh > 0) {
        // view pixels -> normalised, then scaled to the EGL surface size, which
        // differs from the window whenever the game renders at another resolution
        nx = t.x / (float)t.vw;
        ny = t.y / (float)t.vh;
    } else {
        nx = display_w > 0.0f ? t.x / display_w : 0.0f;
        ny = display_h > 0.0f ? t.y / display_h : 0.0f;
    }

    if (g_cfg.touch_swap_xy) {
        float tmp = nx;
        nx = ny;
        ny = tmp;
    }
    if (g_cfg.touch_invert_x)
        nx = 1.0f - nx;
    if (g_cfg.touch_invert_y)
        ny = 1.0f - ny;

    io.MousePos = ImVec2(nx * display_w, ny * display_h);

    // MotionEvent: 0 DOWN, 1 UP, 2 MOVE, 3 CANCEL, 5 POINTER_DOWN, 6 POINTER_UP
    const int a = t.action;
    io.MouseDown[0] = (a == 0 || a == 2 || a == 5);
}

} // namespace nh::jni_input

// ---------------------------------------------------------------- JNI entries

// Fired on the app's UI thread for every MotionEvent the wrapped window sees.
extern "C" JNIEXPORT void JNICALL
Java_nh_NhTouch_onMotion(JNIEnv *, jclass, jint action, jfloat x, jfloat y, jint vw, jint vh) {
    nh::jni_input::detail::publish((int)action, (float)x, (float)y, (int)vw, (int)vh);
}

// Only called on the repacked-APK path, where System.loadLibrary loads us.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    nh::jni_input::detail::g_vm = vm;
    nh::jni_input::detail::g_repack.store(true);
    return JNI_VERSION_1_6;
}
