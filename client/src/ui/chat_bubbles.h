#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <glm/glm.hpp>
#include <imgui.h>
#include "game_state.h"

namespace rco::ui {

struct ChatBubble {
    uint32_t    actor_rid;
    float       y_offset;
    std::string text;
    float       start;
};

class ChatBubbles {
public:
    void Add(uint32_t actor_rid, float y_offset, const std::string& text, float now);
    template <typename WorldActorMap>
    void Render(int sw, int sh, const glm::mat4& view, const glm::mat4& proj, float now,
                const rco::PlayerState& player,
                const WorldActorMap& world_actors) {
        if (bubbles_.empty()) return;

        auto* dl = ImGui::GetForegroundDrawList();
        float fsw = static_cast<float>(sw);
        float fsh = static_cast<float>(sh);

        auto it = bubbles_.begin();
        while (it != bubbles_.end()) {
            float t = (now - it->start) / kDuration;
            if (t >= 1.f) { it = bubbles_.erase(it); continue; }

            float wx = 0.f, wy = 0.f, wz = 0.f;
            bool found = false;
            if (it->actor_rid == player.runtimeId) {
                wx = player.x;
                wy = player.y;
                wz = player.z;
                found = true;
            } else {
                auto actor_it = world_actors.find(it->actor_rid);
                if (actor_it != world_actors.end()) {
                    wx = actor_it->second.x;
                    wy = actor_it->second.y;
                    wz = actor_it->second.z;
                    found = true;
                }
            }
            if (!found) { ++it; continue; }
            wy += it->y_offset;

            // Fade in over 0.1s, full alpha until 0.75, fade out.
            float alpha = 1.f;
            if      (t < 0.1f) alpha = t / 0.1f;
            else if (t > 0.75f) alpha = 1.f - (t - 0.75f) / 0.25f;
            uint8_t a  = static_cast<uint8_t>(alpha * 255.f);
            uint8_t ab = static_cast<uint8_t>(alpha * 190.f);

            glm::vec4 clip = proj * view * glm::vec4(wx, wy, wz, 1.f);
            if (clip.w <= 0.f) { ++it; continue; }
            float sx = (clip.x / clip.w + 1.f) * 0.5f * fsw;
            float sy = (1.f - clip.y / clip.w) * 0.5f * fsh;

            std::string display = it->text;
            if (static_cast<int>(display.size()) > kMaxChars)
                display = display.substr(0, kMaxChars - 3) + "...";

            ImVec2 ts  = ImGui::CalcTextSize(display.c_str());
            float  pad = 7.f;
            float  bx0 = sx - ts.x * 0.5f - pad;
            float  by0 = sy - ts.y - pad * 2.f - 6.f;
            float  bx1 = sx + ts.x * 0.5f + pad;
            float  by1 = sy - 6.f;

            dl->AddRectFilled({bx0, by0}, {bx1, by1}, IM_COL32(18, 18, 28, ab), 6.f);
            dl->AddRect      ({bx0, by0}, {bx1, by1}, IM_COL32(180, 180, 255, a), 6.f, 0, 1.2f);
            dl->AddTriangleFilled(
                {sx - 5.f, by1}, {sx + 5.f, by1}, {sx, by1 + 7.f},
                IM_COL32(18, 18, 28, ab));
            dl->AddText({sx - ts.x * 0.5f, by0 + pad}, IM_COL32(240, 240, 255, a), display.c_str());

            ++it;
        }
    }
    void Clear() { bubbles_.clear(); }

private:
    std::vector<ChatBubble> bubbles_;
    static constexpr float kDuration = 5.f;
    static constexpr int   kMaxChars = 42;
};

} // namespace rco::ui
