#pragma once
#include <glm/glm.hpp>
#include <string>
#include <array>
#include "renderer/model.h"
#include "renderer/shader.h"

namespace rco::renderer {

class Actor {
public:
    void Init(const char* shader_dir, const char* model_path);

    // Load animations from a separate file.
    // name: clip name used for PlayAnim(); "" keeps the file's embedded name.
    void LoadAnim(const char* path, const char* name = "");

    // Play an animation by name.
    //   loop     : true = loop forever; false = play once then switch to return_to
    //   return_to: clip name to switch to after a one-shot finishes ("" = keep last)
    //   restart  : force restart even if the same clip is already playing
    void PlayAnim(const std::string& name,
                  bool loop          = true,
                  const std::string& return_to = "",
                  bool restart       = false);

    void Update(float dt);
    void Render(const glm::mat4& view, const glm::mat4& proj,
                const glm::vec3& cam_pos, const glm::vec3& sun_dir);

    // Render using external state/time (for shared actor instances).
    // Does not modify this actor's playback state.
    void RenderAs(const std::string& anim_name, float anim_t, bool loop,
                  const glm::mat4& view, const glm::mat4& proj,
                  const glm::vec3& cam_pos, const glm::vec3& sun_dir);

    void Destroy();

    glm::vec3 position = {0.f, 0.f, 0.f};
    float     yaw      = 0.f;
    float     scale    = 1.f;

    const std::string& CurrentAnim() const { return cur_name_; }
    float              AnimTime()    const { return anim_t_; }

private:
    Model  model_;
    Shader shader_;

    std::string cur_name_;     // currently playing clip name
    int         clip_idx_ = -1;
    float       anim_t_   = 0.f;
    bool        loop_     = true;
    std::string return_to_;    // clip to play after one-shot ends

    std::array<glm::mat4, kMaxBones> bone_mats_;

    int  FindClip(const std::string& name) const;
    void UploadBones();
    void SetupModelUniforms(const glm::mat4& view, const glm::mat4& proj,
                            const glm::vec3& cam_pos, const glm::vec3& sun_dir);
};

} // namespace rco::renderer
