#include "aim.hpp"
#include "../game/game.hpp"
#include "../game/player.hpp"
#include "../game/math.hpp"
#include "../ui/cfg.hpp"
#include "../other/diag.hpp"
#include <cmath>
#include <vector>

namespace aim {

// Unity-style look quaternion: yaw about Y, pitch about X (q = qyaw * qpitch).
// Sign/convention may need a device-side tweak; logged for tuning.
static Vector4 look_quat(float yaw, float pitch) {
    float cy = std::cos(yaw * .5f), sy = std::sin(yaw * .5f);
    float cp = std::cos(pitch * .5f), sp = std::sin(pitch * .5f);
    return player::quaternion_multiply(Vector4(0, sy, 0, cy), Vector4(sp, 0, 0, cp));
}

// Address of the rotation quaternion (Vector4) inside the local view transform entry.
static bool view_rotation_address(uint64_t native, uint64_t& rot_addr) {
    const auto& L = player::g_transform_layout;
    if (L.pid != proc::pid || L.data_offset < 0) return false;
    uint64_t data = rpm<uint64_t>(native + (uint64_t)L.data_offset);
    int index = rpm<int>(native + (uint64_t)L.index_offset);
    uint64_t entries = rpm<uint64_t>(data + (uint64_t)L.entries_offset);
    if (!plausible(data) || !plausible(entries) || index < 0 || index > 8192) return false;
    // TransformEntry = { Vector4 pos; Vector4 rot; Vector4 scale } -> rot at +16
    rot_addr = entries + (uint64_t)index * sizeof(player::TransformEntry) + 16;
    return true;
}

void run() {
    if (!cfg::aim::enable && !cfg::trigger::enable) return;
    std::vector<uint64_t> P = players();
    if (P.empty()) return;
    uint64_t local = 0;
    for (uint64_t p : P) if (player::local(p)) { local = p; break; }
    if (!local) return;
    int lteam = player::team(local);
    Vector3 eye = player::position(local);
    eye.y += 1.6f;

    uint64_t native = 0, rot_addr = 0;
    for (uint64_t m : player::transform_candidates(local)) {
        uint64_t n = rpm<uint64_t>(m + v100::unity_cached_ptr);
        if (n && view_rotation_address(n, rot_addr)) { native = n; break; }
    }
    if (!native) return;
    Vector3 cur_pos; Vector4 cur_rot;
    if (!player::decode_transform_pose(native, cur_pos, cur_rot)) return;
    Vector3 forward = player::rotate(cur_rot, Vector3(0, 0, 1));

    uint64_t best = 0; float best_ang = 1e9f; Vector3 best_head;
    for (uint64_t p : P) {
        if (p == local || player::health(p) <= 0) continue;
        int t = player::team(p);
        if (cfg::aim::teamcheck && lteam >= 1 && lteam <= 2 && t == lteam) continue;
        Vector3 pos = player::position(p);
        Vector3 head = pos; head.y += cfg::aim::head ? 1.6f : 1.0f;
        Vector3 d(head.x - eye.x, head.y - eye.y, head.z - eye.z);
        float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < .001f) continue;
        float cosang = (forward.x * d.x + forward.y * d.y + forward.z * d.z) / len;
        cosang = cosang < -1 ? -1 : cosang > 1 ? 1 : cosang;
        float ang = std::acos(cosang) * 57.29578f;
        if (ang < best_ang) { best_ang = ang; best = p; best_head = head; }
    }
    if (!best) return;

    bool in_fov = best_ang <= cfg::aim::fov * .5f;
    bool trigger_hit = cfg::trigger::enable && best_ang <= cfg::trigger::radius * .05f;
    if (!cfg::aim::enable || !in_fov) return;

    Vector3 d(best_head.x - eye.x, best_head.y - eye.y, best_head.z - eye.z);
    float horiz = std::sqrt(d.x * d.x + d.z * d.z);
    Vector4 want = look_quat(std::atan2(d.x, d.z), std::atan2(d.y, horiz));

    // trigger locked -> instant; else smoothed
    float t = trigger_hit ? 1.f : std::min(1.f, std::max(.05f, cfg::aim::smooth * .2f));
    Vector4 q(cur_rot.x + (want.x - cur_rot.x) * t,
              cur_rot.y + (want.y - cur_rot.y) * t,
              cur_rot.z + (want.z - cur_rot.z) * t,
              cur_rot.w + (want.w - cur_rot.w) * t);
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > .01f) {
        q.x /= n; q.y /= n; q.z /= n; q.w /= n;
        wpm(rot_addr, q);
        static long long last = 0;
        if (diag::every(last, 3000))
            diag::log("AIM", "target=0x%llx ang=%.1f yaw=%.2f pitch=%.2f t=%.2f",
                      (unsigned long long)best, best_ang,
                      std::atan2(d.x, d.z), std::atan2(d.y, horiz), t);
    }
}

}
