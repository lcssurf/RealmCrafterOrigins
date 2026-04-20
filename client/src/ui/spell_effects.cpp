#include "spell_effects.h"

#include <imgui.h>
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

namespace rco::ui {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool worldToScreen(const glm::mat4& view, const glm::mat4& proj,
                           float sw, float sh, glm::vec3 world,
                           ImVec2& out) {
    glm::vec4 clip = proj * view * glm::vec4(world, 1.f);
    if (clip.w <= 0.f) return false;
    out.x = (clip.x / clip.w + 1.f) * 0.5f * sw;
    out.y = (1.f - clip.y / clip.w) * 0.5f * sh;
    return true;
}

static ImU32 withAlpha(ImU32 col, uint8_t a) {
    return (col & 0x00FFFFFF) | (static_cast<ImU32>(a) << 24);
}

// ---------------------------------------------------------------------------
// SpellEffects
// ---------------------------------------------------------------------------

void SpellEffects::Add(glm::vec3 from, glm::vec3 to, float now, SpellFxKind kind) {
    effects_.push_back({from, to, now, kind});
}

void SpellEffects::Render(int screen_w, int screen_h,
                          const glm::mat4& view, const glm::mat4& proj,
                          float now) {
    if (effects_.empty()) return;

    auto* dl = ImGui::GetForegroundDrawList();
    float sw  = static_cast<float>(screen_w);
    float sh  = static_cast<float>(screen_h);

    auto it = effects_.begin();
    while (it != effects_.end()) {
        float t = (now - it->start) / kDuration; // 0→1 over lifetime
        if (t >= 1.f) { it = effects_.erase(it); continue; }

        // ----------------------------------------------------------------
        // Phase split: 0→0.65 = travel, 0.65→1.0 = impact burst
        // ----------------------------------------------------------------
        constexpr float kTravelEnd = 0.65f;

        switch (it->kind) {

            // ---- Fire: orange orb + tail + red burst ------------------
            case SpellFxKind::Fire: {
                if (t < kTravelEnd) {
                    float p = t / kTravelEnd; // 0→1 during travel
                    // ease-in-out
                    float ep = p * p * (3.f - 2.f * p);
                    glm::vec3 pos = glm::mix(it->from, it->to, ep);

                    // Draw tail (3 fading dots)
                    for (int i = 2; i >= 0; --i) {
                        float tp = ep - i * 0.06f;
                        if (tp < 0.f) tp = 0.f;
                        glm::vec3 tp3 = glm::mix(it->from, it->to, tp * tp * (3.f - 2.f * tp));
                        ImVec2 sp;
                        if (!worldToScreen(view, proj, sw, sh, tp3 + glm::vec3(0, 0.5f, 0), sp)) continue;
                        float r   = (i == 0) ? 7.f : (i == 1 ? 5.f : 3.f);
                        uint8_t a = (uint8_t)(220 - i * 60);
                        ImU32 c   = withAlpha(IM_COL32(255, 140, 30, 255), a);
                        dl->AddCircleFilled(sp, r, c);
                    }

                    // Core orb
                    ImVec2 sp;
                    if (worldToScreen(view, proj, sw, sh, pos + glm::vec3(0, 0.5f, 0), sp)) {
                        dl->AddCircleFilled(sp, 9.f, IM_COL32(255, 220, 80, 240));
                        dl->AddCircle(sp, 12.f, IM_COL32(255, 90, 20, 180), 0, 2.f);
                    }
                } else {
                    // Impact burst
                    float p = (t - kTravelEnd) / (1.f - kTravelEnd); // 0→1
                    float r = p * 45.f;
                    uint8_t a = (uint8_t)((1.f - p) * 200.f);
                    ImVec2 sp;
                    if (worldToScreen(view, proj, sw, sh, it->to + glm::vec3(0, 0.5f, 0), sp)) {
                        dl->AddCircle(sp, r,         withAlpha(IM_COL32(255, 120, 20, 255), a), 0, 3.f);
                        dl->AddCircle(sp, r * 0.6f,  withAlpha(IM_COL32(255, 220, 80, 255), a), 0, 2.f);
                    }
                }
                break;
            }

            // ---- Heal: expanding green rings around caster ------------
            case SpellFxKind::Heal: {
                // Two rings with offset phases
                for (int ring = 0; ring < 2; ++ring) {
                    float rt = t - ring * 0.25f;
                    if (rt < 0.f || rt > 1.f) continue;
                    float r   = rt * 55.f;
                    uint8_t a = (uint8_t)((1.f - rt) * 210.f);
                    ImVec2 sp;
                    if (worldToScreen(view, proj, sw, sh, it->from + glm::vec3(0, 0.8f, 0), sp)) {
                        dl->AddCircle(sp, r,        withAlpha(IM_COL32(80, 255, 120, 255), a), 0, 2.5f);
                        dl->AddCircle(sp, r * 0.5f, withAlpha(IM_COL32(180, 255, 180, 255), a / 2), 0, 1.5f);
                    }
                }
                // Bright center flash at start
                if (t < 0.2f) {
                    float ft = 1.f - t / 0.2f;
                    uint8_t a = (uint8_t)(ft * 200.f);
                    ImVec2 sp;
                    if (worldToScreen(view, proj, sw, sh, it->from + glm::vec3(0, 1.0f, 0), sp)) {
                        dl->AddCircleFilled(sp, 8.f * ft, withAlpha(IM_COL32(120, 255, 160, 255), a));
                    }
                }
                break;
            }

            // ---- Lightning: white-yellow bolt + flash -----------------
            case SpellFxKind::Lightning: {
                if (t < kTravelEnd) {
                    float p = t / kTravelEnd;
                    // Bolt: draw several zig-zag segments
                    constexpr int kSegs = 8;
                    ImVec2 prev{};
                    bool   first = true;
                    for (int s = 0; s <= kSegs; ++s) {
                        float frac = static_cast<float>(s) / kSegs;
                        if (frac > p) frac = p;  // only draw up to current progress
                        glm::vec3 base = glm::mix(it->from, it->to, frac);
                        // Add zig-zag offset perpendicular to travel
                        float offset = (s % 2 == 0) ? 0.3f : -0.3f;
                        glm::vec3 perp = glm::normalize(glm::vec3(it->to.z - it->from.z, 0, -(it->to.x - it->from.x)));
                        glm::vec3 pt = base + perp * offset + glm::vec3(0, 0.5f, 0);

                        ImVec2 sp;
                        if (!worldToScreen(view, proj, sw, sh, pt, sp)) { first = true; continue; }
                        uint8_t a = (uint8_t)(200 * (1.f - frac * 0.3f));
                        if (!first) {
                            dl->AddLine(prev, sp, withAlpha(IM_COL32(255, 255, 120, 255), a), 2.5f);
                            dl->AddLine(prev, sp, withAlpha(IM_COL32(255, 255, 255, 255), a / 2), 1.f);
                        }
                        prev  = sp;
                        first = false;
                    }
                } else {
                    // White flash at target
                    float p = (t - kTravelEnd) / (1.f - kTravelEnd);
                    float r  = (1.f - p) * 40.f;
                    uint8_t a = (uint8_t)((1.f - p) * 220.f);
                    ImVec2 sp;
                    if (worldToScreen(view, proj, sw, sh, it->to + glm::vec3(0, 0.5f, 0), sp)) {
                        dl->AddCircleFilled(sp, r * 0.3f, withAlpha(IM_COL32(255, 255, 255, 255), a));
                        dl->AddCircle(sp, r, withAlpha(IM_COL32(200, 220, 255, 255), a), 0, 2.f);
                    }
                }
                break;
            }
        }

        ++it;
    }
}

} // namespace rco::ui
