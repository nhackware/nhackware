// libnhmenu.so entry point. Loaded by nhinject via a remote dlopen; the
// constructor only spawns a worker because dlopen runs with the linker lock held.
#include "config.h"
#include "input.h"
#include "log.h"
#include "overlay.h"
#include "proc.h"
#include "ui_menu.h"

#include <atomic>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace input = nh::input;
namespace overlay = nh::overlay;

namespace {

std::atomic<bool> g_started{false};

bool wait_for_egl(int timeout_s) {
    for (int i = 0; i < timeout_s * 10; i++) {
        if (dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD))
            return true;
        char full[512];
        if (nh_mapped_module_path("libEGL.so", full, sizeof(full)))
            return true;
        usleep(100000);
    }
    return false;
}

void *worker(void *) {
    NH_LOGI("=== nhmenu loaded into %s (pid %d) ===", nh_process_name(), (int)getpid());

    nh_config_load();

    if (!wait_for_egl(30)) {
        NH_LOGE("libEGL.so never appeared - is this process actually rendering?");
        g_started.store(false);
        return nullptr;
    }

    input::start();
    if (!input::is_open())
        NH_LOGW("continuing without touch input - menu will be visible but not clickable");

    if (!overlay::install()) {
        NH_LOGE("overlay install failed, aborting");
        input::stop();
        g_started.store(false);
        return nullptr;
    }

    // Idle until the UI asks to be unloaded, or the process goes away.
    for (int i = 0; i < 24 * 3600 * 2; i++) {
        if (nh_ui_unload_requested())
            break;
        usleep(500000);
    }

    NH_LOGI("unloading...");
    input::set_grab(false);
    overlay::uninstall();
    input::stop();
    nh_config_save();

    NH_LOGI("overlay down; the .so stays mapped until the process exits");
    g_started.store(false);
    return nullptr;
}

} // namespace

extern "C" __attribute__((constructor)) void nh_menu_ctor() {
    if (g_started.exchange(true))
        return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, worker, nullptr) != 0) {
        NH_LOGE("pthread_create failed");
        g_started.store(false);
    }
    pthread_attr_destroy(&attr);
}
