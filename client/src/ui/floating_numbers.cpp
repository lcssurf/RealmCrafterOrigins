#include "floating_numbers.h"
#include <imgui.h>
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace rco::ui {

void FloatingNumbers::Add(float wx, float wy, float wz, int32_t value, bool is_crit) {
    nums_.push_back({wx, wy, wz, value, is_crit, static_cast<float>(ImGui::GetTime())});
}

void FloatingNumbers::Render(int screen_w, int screen_h,
                             const glm::mat4& view, const glm::mat4& proj,
                             float now) {
    // Cull expired numbers.
    nums_.erase(
        std::remove_if(nums_.begin(), nums_.end(),
            [now](const FloatNum& f) { return now - f.born > kLifetime; }),
        nums_.end());

    if (nums_.empty()) return;

    auto* dl = ImGui::GetForegroundDrawList();

    for (const auto& fn : nums_) {
        float age   = now - fn.born;
        float alpha = std::max(0.f, 1.f - age / kLifetime);
        float rise  = age * 40.f; // px upward drift

        // Project world position to screen.
        glm::vec4 clip = proj * view * glm::vec4(fn.wx, fn.wy, fn.wz, 1.f);
        if (clip.w <= 0.f) continue;

        float sx = (clip.x / clip.w + 1.f) * 0.5f * static_cast<float>(screen_w);
        float sy = (1.f - clip.y / clip.w) * 0.5f * static_cast<float>(screen_h) - rise;

        char buf[16];
        float font_size;
        ImU32 color;

        if (fn.value == -1) {
            snprintf(buf, sizeof(buf), "Miss");
            font_size = 13.f;
            color = IM_COL32(200, 200, 200, static_cast<int>(alpha * 255));
        } else if (fn.is_crit) {
            snprintf(buf, sizeof(buf), "!%d!", fn.value);
            font_size = 20.f;
            color = IM_COL32(255, 220, 0, static_cast<int>(alpha * 255)); // gold
        } else {
            snprintf(buf, sizeof(buf), "%d", fn.value);
            font_size = 15.f;
            color = IM_COL32(255, 80, 80, static_cast<int>(alpha * 255)); // red
        }

        // Centre the text on the projected point.
        ImVec2 text_sz = ImGui::GetFont()->CalcTextSizeA(font_size, 1e9f, 0.f, buf);
        dl->AddText(ImGui::GetFont(), font_size,
                    {sx - text_sz.x * 0.5f, sy},
                    color, buf);
    }
}

} // namespace rco::ui
