#include "spellbar.h"

#include <imgui.h>
#include <cstdio>
#include <cmath>

namespace rco::ui {

void SpellBar::Clear() { slots.clear(); pending_ground_spell = 0; }

void SpellBar::AddSpell(uint16_t id, const std::string& name, uint8_t spell_type,
                        uint16_t mp_cost, uint32_t cooldown_ms,
                        uint8_t aoe_type, float aoe_radius, float range) {
    SpellSlot s;
    s.id          = id;
    s.name        = name;
    s.spell_type  = spell_type;
    s.aoe_type    = aoe_type;
    s.aoe_radius  = aoe_radius;
    s.range       = range;
    s.mp_cost     = mp_cost;
    s.cooldown_ms = cooldown_ms;
    slots.push_back(s);
}

void SpellBar::Render(int screen_w, int screen_h, uint32_t combat_target,
                      float now, bool player_dead, int32_t player_mp,
                      float target_dist) {
    hovered_range = 0.f; hovered_aoe_radius = 0.f; hovered_aoe_type = 0;
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

        // --- MP cost ---
        char mp_lbl[10];
        snprintf(mp_lbl, sizeof(mp_lbl), "%d MP", (int)s.mp_cost);
        ImVec2 es = ImGui::CalcTextSize(mp_lbl);
        dl->AddText({x0 + (kSlotW - es.x) * 0.5f, y1 - es.y - 4.f},
                    IM_COL32(80, 200, 255, 200), mp_lbl);

        // --- Hotkey number (bottom-left) ---
        char hk[4];
        snprintf(hk, sizeof(hk), "%d", i + 1);
        dl->AddText({x0 + 3.f, y1 - 14.f}, IM_COL32(160, 160, 160, 160), hk);

        // --- Hover detection: expose range/aoe info for world-space circles ---
        ImVec2 mp = ImGui::GetMousePos();
        bool slot_hovered = mp.x >= x0 && mp.x <= x1 && mp.y >= y0 && mp.y <= y1;
        if (slot_hovered) {
            hovered_range      = s.range;
            hovered_aoe_radius = s.aoe_radius;
            hovered_aoe_type   = s.aoe_type;
        }

        // --- Border: usability + range ---
        bool  awaiting  = (pending_ground_spell == s.id);
        bool  off_cd    = (now - s.last_cast) >= (s.cooldown_ms / 1000.f);
        bool  has_mp    = player_mp >= static_cast<int32_t>(s.mp_cost);
        bool  is_heal   = (s.spell_type == 1);
        bool  is_ground = (s.aoe_type == 2);
        bool  has_tgt   = is_heal || is_ground || (combat_target != 0);
        bool  in_range  = is_heal || (s.range <= 0.f) ||
                          (combat_target != 0 && target_dist <= s.range);
        bool  can_cast  = !player_dead && off_cd && has_mp && has_tgt && in_range;
        ImU32 border;
        if (awaiting)
            border = IM_COL32(255, 220, 40, 220);
        else if (!in_range && has_tgt)
            border = IM_COL32(200, 60, 60, 200);   // red = out of range
        else if (can_cast)
            border = IM_COL32(100, 140, 255, 200);
        else
            border = IM_COL32(60, 60, 90, 180);
        dl->AddRect(p0, p1, border, 5.f, 0, awaiting ? 2.5f : 1.5f);

        // ESC cancels ground targeting
        if (pending_ground_spell == s.id &&
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            pending_ground_spell = 0;
            s.last_cast = -99999.f; // refund cooldown
        }
    }

    ImGui::End();
}

} // namespace rco::ui
