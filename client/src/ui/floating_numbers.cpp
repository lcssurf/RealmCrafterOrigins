#include "floating_numbers.h"
#include <imgui.h>
#include <algorithm>

namespace rco::ui {

void FloatingNumbers::AddDamage(float wx, float wy, float wz, int32_t value, bool is_crit, bool is_special) {
    FloatType type = FloatType::Damage;
    if (is_special) {
        type = is_crit ? FloatType::DamageSpecialCrit : FloatType::DamageSpecial;
    } else {
        type = is_crit ? FloatType::DamageCrit : FloatType::Damage;
    }
    nums_.push_back({wx, wy, wz, std::to_string(value), type, static_cast<float>(ImGui::GetTime())});
}

void FloatingNumbers::AddText(float wx, float wy, float wz, const char* text, FloatType type) {
    nums_.push_back({wx, wy, wz, text ? text : "", type, static_cast<float>(ImGui::GetTime())});
}

void FloatingNumbers::AddGuarded(float wx, float wy, float wz, int32_t reduced_value) {
    nums_.push_back({wx, wy, wz, std::to_string(reduced_value), FloatType::DamageGuarded, static_cast<float>(ImGui::GetTime())});
}

void FloatingNumbers::Add(float wx, float wy, float wz, int32_t value, bool is_crit) {
    AddDamage(wx, wy, wz, value, is_crit, false);
}

void FloatingNumbers::Render(int screen_w, int screen_h,
                             const glm::mat4& view, const glm::mat4& proj,
                             float now) {
    // Cull expired numbers.
    nums_.erase(
        std::remove_if(nums_.begin(), nums_.end(),
            [now](const Item& f) { return now - f.spawn_time > kLifetime; }),
        nums_.end());

    if (nums_.empty()) return;

    auto* dl = ImGui::GetForegroundDrawList();

    for (const auto& item : nums_) {
        float age = now - item.spawn_time;
        float rise = age * 30.f;

        const float fade_window = 0.5f;
        float remaining = kLifetime - age;
        float fade = 1.f;
        if (remaining < fade_window) {
            fade = std::clamp(remaining / fade_window, 0.f, 1.f);
        }
        if (fade <= 0.f) continue;

        // Project world position to screen.
        glm::vec4 clip = proj * view * glm::vec4(item.wx, item.wy, item.wz, 1.f);
        if (clip.w <= 0.f) continue;

        float sx = (clip.x / clip.w + 1.f) * 0.5f * static_cast<float>(screen_w);
        float sy = (1.f - clip.y / clip.w) * 0.5f * static_cast<float>(screen_h) - rise;

        std::string draw_text = item.text;
        float font_size;
        int r = 255, g = 255, b = 255, base_alpha = 255;
        bool add_crit_suffix = false;
        switch (item.type) {
            case FloatType::Damage:
                r = 255; g = 100; b = 100; base_alpha = 230;
                font_size = 18.f;
                break;
            case FloatType::DamageCrit:
                r = 255; g = 215; b = 0; base_alpha = 255;
                font_size = 26.f;
                add_crit_suffix = true;
                break;
            case FloatType::DamageSpecial:
                r = 255; g = 165; b = 50; base_alpha = 230;
                font_size = 20.f;
                break;
            case FloatType::DamageSpecialCrit:
                r = 255; g = 230; b = 80; base_alpha = 255;
                font_size = 28.f;
                add_crit_suffix = true;
                break;
            case FloatType::DamageGuarded:
                r = 100; g = 220; b = 100; base_alpha = 220;
                font_size = 14.f;
                break;
            case FloatType::Miss:
                r = 180; g = 180; b = 180; base_alpha = 200;
                font_size = 16.f;
                if (draw_text.empty()) draw_text = "Miss";
                break;
            case FloatType::Dodge:
                r = 120; g = 200; b = 255; base_alpha = 220;
                font_size = 18.f;
                if (draw_text.empty()) draw_text = "Dodge";
                break;
            case FloatType::Parry:
                r = 200; g = 130; b = 255; base_alpha = 220;
                font_size = 18.f;
                if (draw_text.empty()) draw_text = "Parry";
                break;
            default:
                font_size = 16.f;
                break;
        }
        if (add_crit_suffix) {
            draw_text += "!";
        }
        int alpha = static_cast<int>(base_alpha * fade);
        if (alpha < 0) alpha = 0;
        if (alpha > 255) alpha = 255;
        ImU32 color = IM_COL32(r, g, b, alpha);
        if (draw_text.empty()) continue;

        // Centre the text on the projected point.
        ImVec2 text_sz = ImGui::GetFont()->CalcTextSizeA(font_size, 1e9f, 0.f, draw_text.c_str());
        dl->AddText(ImGui::GetFont(), font_size,
                    {sx - text_sz.x * 0.5f, sy},
                    color, draw_text.c_str());
    }
}

} // namespace rco::ui
