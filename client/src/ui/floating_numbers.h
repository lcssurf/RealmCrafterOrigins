#pragma once
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

namespace rco::ui {

struct FloatNum {
    float    wx, wy, wz;
    int32_t  value;    // -1 = miss
    bool     is_crit;
    float    born;     // ImGui time
};

class FloatingNumbers {
public:
    void Add(float wx, float wy, float wz, int32_t value, bool is_crit);

    // Project world positions to screen and draw all live numbers.
    // Call inside an ImGui frame (uses GetForegroundDrawList).
    void Render(int screen_w, int screen_h,
                const glm::mat4& view, const glm::mat4& proj,
                float now);

private:
    static constexpr float kLifetime = 2.5f;
    std::vector<FloatNum> nums_;
};

} // namespace rco::ui
