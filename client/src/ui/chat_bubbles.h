#pragma once

#include <vector>
#include <string>
#include <glm/glm.hpp>

namespace rco::ui {

struct ChatBubble {
    float       wx, wy, wz;
    std::string text;
    float       start;
};

class ChatBubbles {
public:
    void Add(float wx, float wy, float wz, const std::string& text, float now);
    void Render(int sw, int sh, const glm::mat4& view, const glm::mat4& proj, float now);
    void Clear() { bubbles_.clear(); }

private:
    std::vector<ChatBubble> bubbles_;
    static constexpr float kDuration = 5.f;
    static constexpr int   kMaxChars = 42;
};

} // namespace rco::ui
