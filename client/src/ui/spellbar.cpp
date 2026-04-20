#include "spellbar.h"

#include <imgui.h>
#include <cstdio>
#include <cmath>

namespace rco::ui {

void SpellBar::Clear() { slots.clear(); }

void SpellBar::AddSpell(uint16_t id, const std::string& name, uint8_t spell_type,
                        uint16_t ep_cost, uint32_t cooldown_ms) {
    SpellSlot s;
    s.id          = id;
    s.name        = name;
    s.spell_type  = spell_type;
    s.ep_cost     = ep_cost;
    s.cooldown_ms = cooldown_ms;
    slots.push_back(s);
}

void SpellBar::Render(int screen_w, int screen_h, uint32_t combat_target,
                      float now, bool player_dead, int32_t player_ep) {
    if (slots.empty()) return;

    constexpr float kSlotW = 60.f;
    constexpr float kSlotH = 60.f;
    constexpr float kGap   = 4.f;

    int   count   = static_cast<int>(slots.size());
    float total_w = count * kSlotW + (count - 1) * kGap;
    float ox      = (screen_w - total_w) * 0.5f;
    float oy      = static_cast<float>(screen_h) - kSlotH - 14.f;

    ImGui::SetNextWindowPos({ox - 8.f, oy - 8.f}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({total_w + 16.f, kSlotH + 16.f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##spellbar", nullptr,
        ImGuiWindowFlags_NoDecoration    |
        ImGuiWindowFlags_NoNav           |
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar     |
        ImGuiWindowFlags_NoInputs);

    auto* dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < count; ++i) {
        auto& s = slots[i];

        float x0 = ox + i * (kSlotW + kGap);
        float y0 = oy;
        float x1 = x0 + kSlotW;
        float y1 = y0 + kSlotH;
        ImVec2 p0{x0, y0}, p1{x1, y1};

        // --- Background ---
        dl->AddRectFilled(p0, p1, IM_COL32(20, 22, 45, 210), 5.f);

        // --- Cooldown overlay (top-to-bottom fill) ---
        if (s.cooldown_ms > 0) {
            float cd_sec  = s.cooldown_ms / 1000.f;
            float elapsed = now - s.last_cast;
            float ratio   = 1.f - (elapsed / cd_sec);
            if (ratio > 0.f) {
                if (ratio > 1.f) ratio = 1.f;
                dl->AddRectFilled(p0, {x1, y0 + kSlotH * ratio},
                                  IM_COL32(0, 0, 0, 150), 5.f);
            }
        }

        // --- Spell name (abbreviated to 6 chars) ---
        char abbr[8];
        snprintf(abbr, sizeof(abbr), "%.6s", s.name.c_str());
        ImVec2 ts = ImGui::CalcTextSize(abbr);
        dl->AddText({x0 + (kSlotW - ts.x) * 0.5f, y0 + 5.f},
                    IM_COL32(240, 240, 240, 220), abbr);

        // --- EP cost ---
        char ep_lbl[10];
        snprintf(ep_lbl, sizeof(ep_lbl), "%d EP", (int)s.ep_cost);
        ImVec2 es = ImGui::CalcTextSize(ep_lbl);
        dl->AddText({x0 + (kSlotW - es.x) * 0.5f, y1 - es.y - 4.f},
                    IM_COL32(80, 200, 255, 200), ep_lbl);

        // --- Hotkey number (bottom-left) ---
        char hk[4];
        snprintf(hk, sizeof(hk), "%d", i + 1);
        dl->AddText({x0 + 3.f, y1 - 14.f}, IM_COL32(160, 160, 160, 160), hk);

        // --- Border (highlight if usable) ---
        bool can_cast = !player_dead &&
                        (now - s.last_cast) >= (s.cooldown_ms / 1000.f) &&
                        player_ep >= static_cast<int32_t>(s.ep_cost) &&
                        (s.spell_type != 0 || combat_target != 0);
        ImU32 border = can_cast ? IM_COL32(100, 140, 255, 200)
                                : IM_COL32(60,  60,  90,  180);
        dl->AddRect(p0, p1, border, 5.f, 0, 1.5f);

        // --- Hotkey trigger (1–9) ---
        if (!player_dead && (i + 1) <= 9 && !ImGui::GetIO().WantTextInput) {
            ImGuiKey key = static_cast<ImGuiKey>(ImGuiKey_1 + i);
            if (ImGui::IsKeyPressed(key, false)) {
                float cd_sec = s.cooldown_ms / 1000.f;
                bool off_cooldown = (now - s.last_cast) >= cd_sec;
                bool has_ep       = player_ep >= static_cast<int32_t>(s.ep_cost);
                uint32_t target   = (s.spell_type == 1) ? 0 : combat_target;
                bool has_target   = (s.spell_type == 1) || (combat_target != 0);
                if (off_cooldown && has_ep && has_target && on_cast) {
                    on_cast(s.id, target);
                    s.last_cast = now;
                }
            }
        }
    }

    ImGui::End();
}

} // namespace rco::ui
