#include "overlay.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "proc.h"
#include "ui_menu.h"

#include "dobby.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <atomic>
#include <chrono>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace nh::overlay {
namespace {

using eglSwapBuffers_t = EGLBoolean (*)(EGLDisplay, EGLSurface);

eglSwapBuffers_t o_eglSwapBuffers = nullptr;
void *g_egl_handle = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_ui_alive{false};
std::atomic<bool> g_init_failed{false};
std::atomic<int> g_in_hook{0};

ImGuiContext *g_ctx = nullptr;
EGLContext g_gl_ctx = EGL_NO_CONTEXT;
int g_display_w = 0;
int g_display_h = 0;

using clk = std::chrono::steady_clock;
clk::time_point g_last_frame = clk::now();

bool init_imgui(EGLDisplay dpy, EGLSurface surface) {
    EGLint w = 0, h = 0;
    if (eglQuerySurface(dpy, surface, EGL_WIDTH, &w) != EGL_TRUE ||
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h) != EGL_TRUE) {
        NH_LOGE("eglQuerySurface failed");
        return false;
    }
    g_display_w = w;
    g_display_h = h;

    const char *ver = (const char *)glGetString(GL_VERSION);
    NH_LOGI("GL: %s", ver ? ver : "(null)");

    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   // we persist feature state ourselves
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendPlatformName = "nhmenu-evdev";

    nh_ui_init_fonts(io, h);

    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.08f, 0.97f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.55f, 0.05f, 0.05f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.78f, 0.08f, 0.08f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.24f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.70f, 0.10f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.85f, 0.12f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.90f, 0.15f, 0.15f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.80f, 0.10f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.20f, 0.20f, 1.0f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.14f, 0.14f, 0.17f, 1.0f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.60f, 0.08f, 0.08f, 1.0f);

    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        NH_LOGE("ImGui_ImplOpenGL3_Init failed (GLES3 required)");
        ImGui::DestroyContext(g_ctx);
        g_ctx = nullptr;
        return false;
    }

    g_gl_ctx = eglGetCurrentContext();
    g_last_frame = clk::now();
    g_ui_alive.store(true);
    NH_LOGI("overlay ready %dx%d", w, h);
    return true;
}

// Must run with the game's EGL context current, i.e. only from inside the hook.
void shutdown_imgui() {
    if (!g_ctx)
        return;
    ImGui::SetCurrentContext(g_ctx);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext(g_ctx);
    g_ctx = nullptr;
    g_gl_ctx = EGL_NO_CONTEXT;
    g_active.store(false);
    g_ui_alive.store(false);
    NH_LOGI("overlay torn down");
}

EGLBoolean hk_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    // Depth-count so uninstall() can wait until we are no longer inside the hook.
    g_in_hook.fetch_add(1);

    if (g_shutdown.load()) {
        // Tear down here rather than from the unloading thread: this is the only
        // place where a GL context is guaranteed to be current.
        if (g_ctx)
            shutdown_imgui();
    } else if (!g_ctx && !g_init_failed.load()) {
        if (!init_imgui(dpy, surface))
            g_init_failed.store(true); // don't retry (and log) every frame
    } else if (g_ctx) {
        EGLContext cur = eglGetCurrentContext();
        if (cur != g_gl_ctx) {
            // The game switched GL contexts (common on resume). Rebuild the backend
            // against the new one instead of drawing into a dead context.
            NH_LOGW("GL context changed, rebuilding overlay");
            ImGui::SetCurrentContext(g_ctx);
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplOpenGL3_Init("#version 300 es");
            g_gl_ctx = cur;
        }

        ImGui::SetCurrentContext(g_ctx);
        ImGuiIO &io = ImGui::GetIO();

        auto now = clk::now();
        float dt = std::chrono::duration<float>(now - g_last_frame).count();
        g_last_frame = now;
        if (dt <= 0.0f || dt > 0.5f)
            dt = 1.0f / 60.0f;
        io.DeltaTime = dt;
        io.DisplaySize = ImVec2((float)g_display_w, (float)g_display_h);

        input::update(io, (float)g_display_w, (float)g_display_h);
        input::set_grab(g_cfg.touch_grab_when_open && g_menu_open);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        nh_ui_draw();

        if (nh_ui_unload_requested())
            g_shutdown.store(true);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        g_active.store(true);
    }

    g_in_hook.fetch_sub(1);
    // Draw BEFORE presenting: our geometry lands on top of the game's frame in the
    // back buffer, then the original swap publishes it.
    return o_eglSwapBuffers(dpy, surface);
}

void *resolve_egl(const char *sym) {
    if (!g_egl_handle) {
        g_egl_handle = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD);
        if (!g_egl_handle)
            g_egl_handle = dlopen("libEGL.so", RTLD_NOW);
        if (!g_egl_handle) {
            char full[512];
            if (nh_mapped_module_path("libEGL.so", full, sizeof(full))) {
                NH_LOGI("namespace lookup failed, using mapped path %s", full);
                g_egl_handle = dlopen(full, RTLD_NOW | RTLD_NOLOAD);
            }
        }
    }
    if (!g_egl_handle) {
        NH_LOGE("libEGL.so not loaded in this process");
        return nullptr;
    }
    void *p = dlsym(g_egl_handle, sym);
    if (!p)
        NH_LOGE("dlsym(%s) failed", sym);
    return p;
}

} // namespace

bool install() {
    if (g_installed.load())
        return true;

    void *target = resolve_egl("eglSwapBuffers");
    if (!target)
        return false;

    // Dobby's ABI is plain void*: int DobbyHook(void *addr, void *fake, void **orig)
    if (DobbyHook(target, (void *)hk_eglSwapBuffers, (void **)&o_eglSwapBuffers) != 0) {
        NH_LOGE("DobbyHook(eglSwapBuffers) failed");
        return false;
    }
    g_installed.store(true);
    NH_LOGI("hooked eglSwapBuffers @ %p", target);
    return true;
}

void uninstall() {
    if (!g_installed.load())
        return;

    g_shutdown.store(true);

    // Wait for the render thread to leave the hook AND finish the ImGui teardown.
    // If the game stops swapping (backgrounded) we bail out and leave the GL
    // objects behind rather than touching them without a current context.
    for (int i = 0; i < 500 && (g_in_hook.load() != 0 || g_ui_alive.load()); i++)
        usleep(2000);

    if (g_ui_alive.load())
        NH_LOGW("game stopped swapping; skipping ImGui teardown to avoid a contextless GL call");

    void *target = resolve_egl("eglSwapBuffers");
    if (target)
        DobbyDestroy(target);
    o_eglSwapBuffers = nullptr;
    g_installed.store(false);
    NH_LOGI("unhooked eglSwapBuffers");
}

bool is_installed() { return g_installed.load(); }
bool is_active() { return g_active.load(); }

} // namespace nh::overlay
