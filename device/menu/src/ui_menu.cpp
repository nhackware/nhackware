#include "ui_menu.h"
#include "config.h"
#include "input.h"
#include "log.h"

#include <atomic>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace input = nh::input;

namespace {

std::atomic<bool> g_unload{false};
float g_scale = 1.0f;

const char *const kFontCandidates[] = {
    "/system/fonts/Roboto-Regular.ttf",
    "/system/fonts/NotoSans-Regular.ttf",
    "/system/fonts/DroidSans.ttf",
};

void draw_feature(const Feature &f) {
    switch (f.type) {
    case FType::Toggle:
        ImGui::Checkbox(f.label, (bool *)f.val);
        break;
    case FType::SliderInt:
        ImGui::SliderInt(f.label, (int *)f.val, (int)f.v_min, (int)f.v_max);
        break;
    case FType::SliderFloat:
        ImGui::SliderFloat(f.label, (float *)f.val, f.v_min, f.v_max, "%.2f");
        break;
    case FType::Combo:
        ImGui::Combo(f.label, (int *)f.val, f.items, f.item_count);
        break;
    }
}

// The red strip pinned to the bottom edge. Tapping it flips g_menu_open.
void draw_bar(const ImGuiIO &io) {
    const float h = (float)g_cfg.bar_height * g_scale;
    const float w = io.DisplaySize.x;
    const float a = g_cfg.menu_alpha;

    ImGui::SetNextWindowPos(ImVec2(0.0f, io.DisplaySize.y - h));
    ImGui::SetNextWindowSize(ImVec2(w, h));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.72f, 0.06f, 0.06f, 0.94f * a));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.30f, 0.30f, 0.9f * a));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, a));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * g_scale, 2.0f * g_scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::Begin("##nh_bar", nullptr, flags);

    const char *label = g_menu_open ? "NHACKWARE   \xe2\x80\x94   tap to close"
                                    : "NHACKWARE   \xe2\x80\x94   tap to open";
    const float tw = ImGui::CalcTextSize(label).x;
    ImGui::SetCursorPosX((w - tw) * 0.5f);
    ImGui::TextUnformatted(label);

    // fps on the right edge, purely informational
    char fps[32];
    snprintf(fps, sizeof(fps), "%.0f fps", io.DeltaTime > 0.0f ? 1.0f / io.DeltaTime : 0.0f);
    const float fw = ImGui::CalcTextSize(fps).x;
    ImGui::SameLine(w - fw - 8.0f * g_scale);
    ImGui::TextUnformatted(fps);

    if (ImGui::IsWindowHovered() && ImGui::IsMouseReleased(0)) {
        g_menu_open = !g_menu_open;
        NH_LOGI("menu %s", g_menu_open ? "opened" : "closed");
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
}

void draw_menu(const ImGuiIO &io) {
    const float mw = io.DisplaySize.x * 0.68f;
    const float mh = io.DisplaySize.y * 0.62f;

    ImGui::SetNextWindowSize(ImVec2(mw, mh), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.16f, io.DisplaySize.y * 0.08f),
                            ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_cfg.menu_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * g_scale);

    bool open = true;
    if (!ImGui::Begin("nhackware", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        g_menu_open = open;
        return;
    }
    if (!open)
        g_menu_open = false;

    int n = 0;
    const Feature *feats = nh_features(&n);

    if (ImGui::BeginTabBar("##nh_tabs")) {
        // one tab per distinct `tab` string in the feature table, in table order
        const char *seen[16];
        int nseen = 0;
        for (int i = 0; i < n; i++) {
            const char *tab = feats[i].tab;
            bool dup = false;
            for (int k = 0; k < nseen; k++)
                if (strcmp(seen[k], tab) == 0) {
                    dup = true;
                    break;
                }
            if (dup || nseen >= 16)
                continue;
            seen[nseen++] = tab;

            if (!ImGui::BeginTabItem(tab))
                continue;
            for (int j = 0; j < n; j++) {
                if (strcmp(feats[j].tab, tab) != 0)
                    continue;
                draw_feature(feats[j]);
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("system")) {
            ImGui::Text("process   %s", nh_process_name());
            ImGui::Text("config    %s", nh_config_path());
            ImGui::Text("touch     %s", input::is_open() ? input::device_name() : "NOT FOUND");
            ImGui::Text("grabbed   %s", input::is_grabbed() ? "yes" : "no");
            ImGui::Text("display   %.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
            ImGui::Spacing();

            if (ImGui::Button("save config", ImVec2(150 * g_scale, 0)))
                nh_config_save();
            ImGui::SameLine();
            if (ImGui::Button("reload config", ImVec2(150 * g_scale, 0)))
                nh_config_load();
            ImGui::SameLine();
            if (ImGui::Button("close menu", ImVec2(150 * g_scale, 0)))
                g_menu_open = false;

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                               "unload unhooks eglSwapBuffers and stops the overlay.");
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                               "The .so stays mapped until the process exits.");
            if (ImGui::Button("UNLOAD", ImVec2(200 * g_scale, 0))) {
                nh_config_save();
                nh_ui_request_unload();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace

void nh_ui_init_fonts(ImGuiIO &io, int display_h) {
    io.Fonts->Clear();

    g_scale = (float)display_h / 1080.0f;
    if (g_scale < 0.85f)
        g_scale = 0.85f;
    if (g_scale > 2.2f)
        g_scale = 2.2f;

    float size = 17.0f * g_scale;
    ImFont *font = nullptr;
    // Cyrillic range also covers basic Latin, so Russian + English both render.
    for (const char *path : kFontCandidates) {
        if (access(path, R_OK) != 0)
            continue;
        font = io.Fonts->AddFontFromFileTTF(path, size, nullptr, io.Fonts->GetGlyphRangesCyrillic());
        if (font) {
            NH_LOGI("font %s @ %.1fpx (scale %.2f)", path, size, g_scale);
            break;
        }
    }
    if (!font) {
        font = io.Fonts->AddFontDefault();
        NH_LOGW("no system TTF readable, falling back to the default bitmap font (no Cyrillic)");
    }
}

void nh_ui_draw() {
    const ImGuiIO &io = ImGui::GetIO();

    if (g_menu_open || g_cfg.bar_visible)
        draw_bar(io);

    if (g_menu_open)
        draw_menu(io);
}

void nh_ui_request_unload() { g_unload.store(true); }
bool nh_ui_unload_requested() { return g_unload.load(); }
