#pragma once

#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

namespace rco::ui {

enum class SpellFxKind : uint8_t {
    Fire      = 0,   // orange orb
    Heal      = 1,   // green pulse rings
    Lightning = 2,   // white-yellow bolt
};

struct SpellFx {
    glm::vec3    from;
    glm::vec3    to;
    float        start;    // glfwGetTime() at creation
    SpellFxKind  kind;
};

class SpellEffects {
public:
    // Add a spell effect.
    // from/to are world-space positions of caster and target.
    // For heals, from == to (self-cast).
    void Add(glm::vec3 from, glm::vec3 to, float now, SpellFxKind kind);

    // Render all active effects. Prunes expired ones.
    void Render(int screen_w, int screen_h,
                const glm::mat4& view, const glm::mat4& proj,
                float now);

    void Clear() { effects_.clear(); }

private:
    std::vector<SpellFx> effects_;

    static constexpr float kDuration = 0.65f;
};

} // namespace rco::ui
