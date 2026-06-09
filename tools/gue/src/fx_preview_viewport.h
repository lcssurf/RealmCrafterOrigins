#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <imgui.h>

#include "rco/renderer/particles.h"

namespace gue {

class FXPreviewViewport {
public:
    void Shutdown();

    // Define FX parameters to preview (called when selection/editor state changes).
    void SetParams(const rco::renderer::FXParams& params);

    // Draws the preview in an ImGui image of the given size.
    // Updates particles, renders to internal FBO, and shows texture with ImGui::Image.
    void Draw(ImVec2 size);

private:
    void EnsureFbo_(int w, int h);
    void HandleOrbitInput_(ImVec2 imageMin, ImVec2 imageSize);
    glm::mat4 ViewMatrix_() const;
    glm::mat4 ProjMatrix_(float aspect) const;

    GLuint fbo_ = 0;
    GLuint color_ = 0;
    GLuint depth_ = 0;
    int w_ = 0;
    int h_ = 0;

    rco::renderer::ParticleSystem particles_;
    bool particles_init_ = false;

    rco::renderer::FXParams params_;
    bool has_params_ = false;

    // Periodic respawn keeps the preview continuously visible.
    float last_spawn_time_ = -1e9f;
    float spawn_period_ = 0.0f;

    // Orbital camera.
    float cam_yaw_ = 0.6f;
    float cam_pitch_ = 0.3f;
    float cam_dist_ = 12.0f;
    glm::vec3 cam_target_ = {0.f, 2.f, 0.f};
};

} // namespace gue
