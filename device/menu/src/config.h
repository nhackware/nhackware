#pragma once

// Everything the menu exposes. Adding a feature = add a field here + one row in
// the table in config.cpp; the UI is generated from the table.

struct MenuConfig {
    // --- combat ---
    bool aimbot = false;
    float aim_fov = 90.0f;
    float aim_smooth = 5.0f;
    int aim_bone = 0; // index into kBoneNames
    bool aim_visible_only = true;
    bool triggerbot = false;
    int trigger_delay_ms = 45;

    // --- visual ---
    bool esp = false;
    bool esp_box = true;
    bool esp_skeleton = false;
    bool esp_name = true;
    bool esp_health = true;
    bool esp_distance = true;
    bool esp_chams = false;
    float esp_max_dist = 250.0f;

    // --- misc ---
    bool no_recoil = false;
    bool rapid_fire = false;
    float speed_mult = 1.0f;

    // --- menu itself ---
    float menu_alpha = 1.0f;
    bool bar_visible = true;
    int bar_height = 26;
    bool touch_grab_when_open = true;

    // --- touch mapping (device specific) ---
    bool touch_swap_xy = false;
    bool touch_invert_x = false;
    bool touch_invert_y = false;
};

extern MenuConfig g_cfg;
extern bool g_menu_open;

enum class FType { Toggle, SliderInt, SliderFloat, Combo };

struct Feature {
    const char *tab;
    const char *label;
    FType type;
    void *val; // bool* / int* / float*
    float v_min;
    float v_max;
    const char *const *items;
    int item_count;
};

// Table driving both the UI and INI (de)serialisation.
const Feature *nh_features(int *count);

void nh_config_load();
void nh_config_save();
const char *nh_config_path();

// Process name of the host app (from /proc/self/cmdline), used for the config path.
const char *nh_process_name();

extern const char *const kBoneNames[];
extern const int kBoneCount;
