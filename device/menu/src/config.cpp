#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

MenuConfig g_cfg;
bool g_menu_open = false;

const char *const kBoneNames[] = {"head",   "neck",  "chest",
                                  "pelvis", "l_arm", "r_arm"};
const int kBoneCount = (int)(sizeof(kBoneNames) / sizeof(kBoneNames[0]));

#define T(tab, label, field) \
    { tab, label, FType::Toggle, &g_cfg.field, 0, 0, nullptr, 0 }
#define SI(tab, label, field, lo, hi) \
    { tab, label, FType::SliderInt, &g_cfg.field, lo, hi, nullptr, 0 }
#define SF(tab, label, field, lo, hi) \
    { tab, label, FType::SliderFloat, &g_cfg.field, lo, hi, nullptr, 0 }
#define CB(tab, label, field, items, n) \
    { tab, label, FType::Combo, &g_cfg.field, 0, (float)n, items, n }

static const Feature kFeatures[] = {
    // combat
    T("combat", "aimbot", aimbot),
    SF("combat", "fov", aim_fov, 1.0f, 360.0f),
    SF("combat", "smooth", aim_smooth, 1.0f, 40.0f),
    CB("combat", "bone", aim_bone, kBoneNames, kBoneCount),
    T("combat", "visible only", aim_visible_only),
    T("combat", "triggerbot", triggerbot),
    SI("combat", "trigger delay (ms)", trigger_delay_ms, 0, 500),

    // visual
    T("visual", "esp master", esp),
    T("visual", "box", esp_box),
    T("visual", "skeleton", esp_skeleton),
    T("visual", "names", esp_name),
    T("visual", "health", esp_health),
    T("visual", "distance", esp_distance),
    T("visual", "chams", esp_chams),
    SF("visual", "max distance", esp_max_dist, 10.0f, 2000.0f),

    // misc
    T("misc", "no recoil", no_recoil),
    T("misc", "rapid fire", rapid_fire),
    SF("misc", "speed x", speed_mult, 0.1f, 10.0f),

    // menu
    SF("menu", "menu alpha", menu_alpha, 0.2f, 1.0f),
    T("menu", "red bar visible", bar_visible),
    SI("menu", "bar height (px)", bar_height, 12, 80),
    T("menu", "grab touch while open", touch_grab_when_open),

    // touch
    T("touch", "swap x/y", touch_swap_xy),
    T("touch", "invert x", touch_invert_x),
    T("touch", "invert y", touch_invert_y),
};

const Feature *nh_features(int *count) {
    if (count)
        *count = (int)(sizeof(kFeatures) / sizeof(kFeatures[0]));
    return kFeatures;
}

const char *nh_process_name() {
    static char name[256] = {0};
    if (name[0])
        return name;
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f)
        return "unknown";
    if (fgets(name, sizeof(name), f)) {
        // cmdline is NUL separated; take argv[0]
        name[strcspn(name, "\n")] = 0;
    } else {
        strcpy(name, "unknown");
    }
    fclose(f);
    return name;
}

// Preferred: the host app's own files dir (survives, is readable by the app uid).
// Fallbacks for when we injected as root and cannot create that dir.
const char *nh_config_path() {
    static char path[512] = {0};
    if (path[0])
        return path;

    char dir[512];
    char cand[512];
    snprintf(dir, sizeof(dir), "/data/data/%s/files", nh_process_name());
    snprintf(cand, sizeof(cand), "%s/nhmenu.ini", dir);

    // Prefer the host app's own files dir so the config is owned by the app uid
    // and survives; fall back to somewhere the injector (root) can always write.
    if (access(cand, F_OK) == 0 || access(dir, W_OK) == 0)
        return strcpy(path, cand);
    if (access("/data/local/tmp", W_OK) == 0)
        return strcpy(path, "/data/local/tmp/nhmenu.ini");
    return strcpy(path, "/sdcard/nhmenu.ini");
}

static void write_val(FILE *f, const Feature &ft) {
    switch (ft.type) {
    case FType::Toggle:
        fprintf(f, "%s = %d\n", ft.label, *(bool *)ft.val ? 1 : 0);
        break;
    case FType::SliderInt:
    case FType::Combo:
        fprintf(f, "%s = %d\n", ft.label, *(int *)ft.val);
        break;
    case FType::SliderFloat:
        fprintf(f, "%s = %.6f\n", ft.label, *(float *)ft.val);
        break;
    }
}

void nh_config_save() {
    const char *p = nh_config_path();
    FILE *f = fopen(p, "w");
    if (!f) {
        NH_LOGW("cannot write config %s", p);
        return;
    }
    fprintf(f, "# nhmenu config\n");
    int n = 0;
    const Feature *feats = nh_features(&n);
    for (int i = 0; i < n; i++)
        write_val(f, feats[i]);
    fclose(f);
    NH_LOGI("config saved -> %s", p);
}

static const Feature *find_by_label(const Feature *feats, int n, const char *label) {
    for (int i = 0; i < n; i++)
        if (strcmp(feats[i].label, label) == 0)
            return &feats[i];
    return nullptr;
}

void nh_config_load() {
    const char *p = nh_config_path();
    FILE *f = fopen(p, "r");
    if (!f) {
        NH_LOGI("no config at %s, using defaults", p);
        return;
    }
    int n = 0;
    const Feature *feats = nh_features(&n);

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        while (*key == ' ')
            key++;
        size_t kl = strlen(key);
        while (kl && (key[kl - 1] == ' ' || key[kl - 1] == '\t'))
            key[--kl] = 0;
        while (*val == ' ')
            val++;
        val[strcspn(val, "\r\n")] = 0;

        const Feature *ft = find_by_label(feats, n, key);
        if (!ft)
            continue;
        switch (ft->type) {
        case FType::Toggle:
            *(bool *)ft->val = (atoi(val) != 0);
            break;
        case FType::SliderInt:
        case FType::Combo: {
            int iv = atoi(val);
            if (iv < (int)ft->v_min)
                iv = (int)ft->v_min;
            if (iv > (int)ft->v_max)
                iv = (int)ft->v_max;
            *(int *)ft->val = iv;
            break;
        }
        case FType::SliderFloat: {
            float fv = (float)atof(val);
            if (fv < ft->v_min)
                fv = ft->v_min;
            if (fv > ft->v_max)
                fv = ft->v_max;
            *(float *)ft->val = fv;
            break;
        }
        }
    }
    fclose(f);
    NH_LOGI("config loaded from %s", p);
}
