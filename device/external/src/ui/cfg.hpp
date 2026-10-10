#pragma once
#include "imgui.h"

namespace cfg {

namespace aim {
    inline bool enable = false;
    inline bool teamcheck = true;   // do not aim at teammates
    inline bool head = true;        // aim at head, else chest
    inline float fov = 90.f;        // degrees
    inline float smooth = 5.f;      // higher = snappier
}

namespace trigger {
    inline bool enable = false;
    inline float radius = 14.f;     // px from screen center to fire
}

namespace esp {
    inline bool box = true;
    inline bool name = false;
    inline bool health = true;
    inline bool distance = false;
    inline int box_type = 0;
    inline float box_rounding = 0.f;
    inline ImVec4 box_col = ImVec4(162/255.f, 144/255.f, 225/255.f, 1.f);
    inline ImVec4 name_col = ImVec4(1.f, 1.f, 1.f, 1.f);
    inline ImVec4 health_col = ImVec4(162/255.f, 144/255.f, 225/255.f, 1.f);
    inline ImVec4 distance_col = ImVec4(162/255.f, 144/255.f, 225/255.f, 1.f);
}

}


