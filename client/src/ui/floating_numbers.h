#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <glm/glm.hpp>

namespace rco::ui {

enum class FloatType {
    Damage,
    DamageCrit,
    DamageSpecial,
    DamageSpecialCrit,
    DamageGuarded,
    Miss,
    Dodge,
    Parry,
};

struct Item {
    float wx, wy, wz;
    std::string text;
    FloatType type;
    float spawn_time;
};

class FloatingNumbers {
public:
    void AddDamage(float wx, float wy, float wz, int32_t value, bool is_crit, bool is_special);
    void AddText(float wx, float wy, float wz, const char* text, FloatType type);
    void AddGuarded(float wx, float wy, float wz, int32_t reduced_value);

    // Backward-compatible wrapper.
    void Add(float wx, float wy, float wz, int32_t value, bool is_crit);

    // Project world positions to screen and draw all live numbers.
    // Call inside an ImGui frame (uses GetForegroundDrawList).
    void Render(int screen_w, int screen_h,
                const glm::mat4& view, const glm::mat4& proj,
                float now);

private:
    static constexpr float kLifetime = 2.5f;
    std::vector<Item> nums_;
};

} // namespace rco::ui
