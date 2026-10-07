#pragma once

#include "imgui.h"

// Must be called before the first ImGui_ImplOpenGL3_NewFrame().
void nh_ui_init_fonts(ImGuiIO &io, int display_h);

void nh_ui_draw(); // builds the red bar + menu for this frame
void nh_ui_request_unload();
bool nh_ui_unload_requested();
