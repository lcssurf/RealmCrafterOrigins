#include "chat_bubbles.h"

#include <imgui.h>
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

namespace rco::ui {

void ChatBubbles::Add(float wx, float wy, float wz, const std::string& text, float now) {
    // Update existing bubble from the same speaker position.
    for (auto& b : bubbles_) {
        if (fabsf(b.wx - wx) < 0.5f && fabsf(b.wz - wz) < 0.5f) {
            b.text  = text;
            b.start = now;
            return;
        }
    }
    bubbles_.push_back({wx, wy, wz, text, now});
}

void ChatBubbles::Render(int sw, int sh, const glm::mat4& view, const glm::mat4& proj, float now) {
    if (bubbles_.empty()) return;

    auto* dl = ImGui::GetForegroundDrawList();
    float fsw = static_cast<float>(sw);
    float fsh = static_cast<float>(sh);

    auto it = bubbles_.begin();
    while (it != bubbles_.end()) {
        float t = (now - it->start) / kDuration;
        if (t >= 1.f) { it = bubbles_.erase(it); continue; }

        // Fade in over 0.1s, full alpha until 0.75, fade out.
        float alpha = 1.f;
        if      (t < 0.1f) alpha = t / 0.1f;
        else if (t > 0.75f) alpha = 1.f - (t - 0.75f) / 0.25f;
        uint8_t a  = static_cast<uint8_t>(alpha * 255.f);
        uint8_t ab = static_cast<uint8_t>(alpha * 190.f);

        glm::vec4 clip = proj * view * glm::vec4(it->wx, it->wy, it->wz, 1.f);
        if (clip.w <= 0.f) { ++it; continue; }
        float sx = (clip.x / clip.w + 1.f) * 0.5f * fsw;
        float sy = (1.f - clip.y / clip.w) * 0.5f * fsh;

        // Clamp text.
        std::string display = it->text;
        if (static_cast<int>(display.size()) > kMaxChars)
            display = display.substr(0, kMaxChars - 3) + "...";

        ImVec2 ts  = ImGui::CalcTextSize(display.c_str());
        float  pad = 7.f;
        float  bx0 = sx - ts.x * 0.5f - pad;
        float  by0 = sy - ts.y - pad * 2.f - 6.f;
        float  bx1 = sx + ts.x * 0.5f + pad;
        float  by1 = sy - 6.f;

        // Background + border.
        dl->AddRectFilled({bx0, by0}, {bx1, by1}, IM_COL32(18, 18, 28, ab), 6.f);
        dl->AddRect      ({bx0, by0}, {bx1, by1}, IM_COL32(180, 180, 255, a), 6.f, 0, 1.2f);

        // Small triangle pointer pointing down toward the character head.
        dl->AddTriangleFilled(
            {sx - 5.f, by1}, {sx + 5.f, by1}, {sx, by1 + 7.f},
            IM_COL32(18, 18, 28, ab));

        // Text.
        dl->AddText({sx - ts.x * 0.5f, by0 + pad}, IM_COL32(240, 240, 255, a), display.c_str());

        ++it;
    }
}

} // namespace rco::ui
