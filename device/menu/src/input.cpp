#include "input.h"
#include "config.h"
#include "jni_input.h"
#include "log.h"

#include "imgui.h"

#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <mutex>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace nh::input {
namespace {

constexpr int kMaxSlots = 8;

int g_fd = -1;
char g_devname[128] = {0};
std::atomic<bool> g_running{false};
std::atomic<bool> g_grabbed{false};
pthread_t g_thread{};

// Reader thread writes, render thread reads. Raw device units are stored here and
// normalised in update(), so swap/invert changes take effect without a restart.
std::mutex g_mtx;
bool g_slot_active[kMaxSlots] = {false};
int g_cur_slot = 0;
float g_raw_x = 0.0f;
float g_raw_y = 0.0f;
bool g_have_pos = false;
bool g_btn_touch = false;
bool g_protocol_b = false;

// Set when the JNI backend won. The two sources are mutually exclusive: the JNI
// path needs no root, evdev does, and there is no point running both.
bool g_using_jni = false;

struct AxisRange {
    float min = 0.0f, max = 1.0f;
};
AxisRange g_range_x{0, 1080}, g_range_y{0, 1920};

bool bit_set(const unsigned long *bits, int bit) {
    return (bits[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

int open_touch_device() {
    DIR *d = opendir("/dev/input");
    if (!d) {
        NH_LOGE("cannot open /dev/input: %s", strerror(errno));
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        char path[128];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;

        unsigned long absbits[(ABS_MAX + 7) / (8 * sizeof(long))] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0) {
            close(fd);
            continue;
        }

        bool has_mt = bit_set(absbits, ABS_MT_POSITION_X) && bit_set(absbits, ABS_MT_POSITION_Y);
        bool has_st = bit_set(absbits, ABS_X) && bit_set(absbits, ABS_Y);
        if (!has_mt && !has_st) {
            close(fd);
            continue;
        }

        input_absinfo ai{};
        g_protocol_b = has_mt;
        if (ioctl(fd, EVIOCGABS(has_mt ? ABS_MT_POSITION_X : ABS_X), &ai) == 0)
            g_range_x = {(float)ai.minimum, (float)ai.maximum};
        if (ioctl(fd, EVIOCGABS(has_mt ? ABS_MT_POSITION_Y : ABS_Y), &ai) == 0)
            g_range_y = {(float)ai.minimum, (float)ai.maximum};
        if (g_range_x.max <= g_range_x.min)
            g_range_x = {0, 1080};
        if (g_range_y.max <= g_range_y.min)
            g_range_y = {0, 1920};

        char name[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
            strcpy(name, "?");

        strcpy(g_devname, path);
        NH_LOGI("touch %s ('%s') proto %s x[%.0f..%.0f] y[%.0f..%.0f]", path, name,
                has_mt ? "B" : "A", g_range_x.min, g_range_x.max, g_range_y.min, g_range_y.max);
        closedir(d);
        return fd;
    }

    closedir(d);
    NH_LOGE("no touch device under /dev/input (need root to read it)");
    return -1;
}

void *reader(void *) {
    struct input_event ev;
    while (g_running.load()) {
        ssize_t n = read(g_fd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            usleep(20000);
            continue;
        }

        std::lock_guard<std::mutex> lk(g_mtx);
        switch (ev.type) {
        case EV_ABS:
            switch (ev.code) {
            case ABS_MT_SLOT:
                g_cur_slot = (ev.value >= 0 && ev.value < kMaxSlots) ? ev.value : 0;
                break;
            case ABS_MT_TRACKING_ID:
                g_slot_active[g_cur_slot] = (ev.value >= 0);
                break;
            case ABS_MT_POSITION_X:
            case ABS_X:
                g_raw_x = (float)ev.value;
                g_have_pos = true;
                break;
            case ABS_MT_POSITION_Y:
            case ABS_Y:
                g_raw_y = (float)ev.value;
                g_have_pos = true;
                break;
            default:
                break;
            }
            break;
        case EV_KEY:
            if (ev.code == BTN_TOUCH)
                g_btn_touch = (ev.value != 0);
            break;
        default:
            break;
        }
    }
    return nullptr;
}

bool any_slot_active() {
    if (!g_protocol_b)
        return g_btn_touch;
    for (int i = 0; i < kMaxSlots; i++)
        if (g_slot_active[i])
            return true;
    return false;
}

} // namespace

void start() {
    // Prefer the JNI bridge: it needs no root, so it is the only option that
    // survives on a stock 'user' build where root would trip detection.
    if (jni_input::start()) {
        g_using_jni = true;
        NH_LOGI("touch source: %s", jni_input::name());
        return;
    }
    NH_LOGI("JNI touch unavailable, falling back to evdev (needs root)");

    g_fd = open_touch_device();
    if (g_fd < 0)
        return;
    g_running.store(true);
    if (pthread_create(&g_thread, nullptr, reader, nullptr) != 0) {
        NH_LOGE("pthread_create(reader) failed: %s", strerror(errno));
        g_running.store(false);
        close(g_fd);
        g_fd = -1;
    }
}

void stop() {
    if (g_using_jni) {
        jni_input::stop();
        g_using_jni = false;
        return;
    }
    if (!g_running.exchange(false))
        return;
    set_grab(false);
    pthread_join(g_thread, nullptr);
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

void set_grab(bool on) {
    // EVIOCGRAB is a root-only ioctl. On the JNI path touches are mirrored, not
    // stolen, so the game keeps seeing them - there is nothing to grab.
    if (g_using_jni)
        return;
    if (g_fd < 0 || g_grabbed.load() == on)
        return;
    if (ioctl(g_fd, EVIOCGRAB, (void *)(intptr_t)(on ? 1 : 0)) == 0) {
        g_grabbed.store(on);
        NH_LOGI("touch grab %s", on ? "ON (game no longer sees touches)" : "OFF");
    } else {
        NH_LOGW("EVIOCGRAB failed: %s", strerror(errno));
    }
}

bool is_grabbed() { return g_using_jni ? false : g_grabbed.load(); }
bool using_jni() { return g_using_jni; }

const char *device_name() {
    if (g_using_jni)
        return jni_input::name();
    return g_devname[0] ? g_devname : "(none)";
}

bool is_open() { return g_using_jni ? jni_input::is_active() : g_fd >= 0; }

void update(ImGuiIO &io, float display_w, float display_h) {
    if (g_using_jni) {
        jni_input::update(io, display_w, display_h);
        return;
    }

    bool down;
    float nx, ny;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        down = any_slot_active();
        float rx = g_raw_x, ry = g_raw_y;
        nx = (rx - g_range_x.min) / (g_range_x.max - g_range_x.min);
        ny = (ry - g_range_y.min) / (g_range_y.max - g_range_y.min);
    }

    if (g_cfg.touch_swap_xy) {
        float t = nx;
        nx = ny;
        ny = t;
    }
    if (g_cfg.touch_invert_x)
        nx = 1.0f - nx;
    if (g_cfg.touch_invert_y)
        ny = 1.0f - ny;

    nx = nx < 0 ? 0 : (nx > 1 ? 1 : nx);
    ny = ny < 0 ? 0 : (ny > 1 ? 1 : ny);

    // Position is published even while the finger is up so ImGui sees the release
    // at the coordinates it went down at, not a teleport.
    io.MousePos = ImVec2(nx * display_w, ny * display_h);
    io.MouseDown[0] = down;
}

} // namespace nh::input
