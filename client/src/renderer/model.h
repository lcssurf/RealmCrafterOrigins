#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <string>
#include <map>
#include "renderer/shader.h"

struct aiNode;
struct aiScene;
struct aiMesh;

namespace rco::renderer {

static constexpr int kMaxBones = 64;

// ---------------------------------------------------------------------------
// Geometry submesh (one draw call)
// ---------------------------------------------------------------------------
struct SubMesh {
    GLuint vao      = 0;
    GLuint vbo      = 0;  // pos/normal/uv/tangent
    GLuint bone_vbo = 0;  // ivec4 bone_ids + vec4 bone_weights; 0 if not skinned
    GLuint ebo      = 0;
    int    idx_count = 0;
    bool   skinned   = false;

    GLuint tex_albedo = 0;
    GLuint tex_normal = 0;
    GLuint tex_orm    = 0;

    glm::vec3 albedo_factor    = {0.72f, 0.68f, 0.60f};
    float     roughness_factor = 0.5f;
    float     metallic_factor  = 0.0f;
};

// ---------------------------------------------------------------------------
// Animation structures
// ---------------------------------------------------------------------------
struct AnimKey3 { double t; glm::vec3 v; };
struct AnimKeyQ { double t; glm::quat v; };

struct BoneChannel {
    std::string            name; // node this channel drives
    std::vector<AnimKey3>  pos;
    std::vector<AnimKeyQ>  rot;
    std::vector<AnimKey3>  scl;
};

struct AnimClip {
    std::string               name;
    float                     duration_sec = 0.f;
    std::vector<BoneChannel>  channels;
    std::map<std::string,int> chan_map; // node name → channels[] index
};

// Node in the bone hierarchy stored in topological order (parent before child).
struct AnimNode {
    std::string name;
    int         parent   = -1;
    glm::mat4   local    = glm::mat4(1.f); // bind-pose local transform
    int         bone_idx = -1;             // index into bones_[] or -1
};

struct BoneInfo {
    glm::mat4 offset = glm::mat4(1.f); // inverse bind-pose matrix
};

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
class Model {
public:
    ~Model();

    bool Load(const char* path);
    void Draw(const Shader& shader) const;
    void Destroy();
    bool IsLoaded()      const { return !meshes_.empty(); }
    bool  HasAnimations()       const { return !clips_.empty(); }
    int   ClipCount()           const { return (int)clips_.size(); }
    const std::string& ClipName(int i)     const { return clips_[i].name; }
    float ClipDuration(int i)   const {
        return (i >= 0 && i < (int)clips_.size()) ? clips_[i].duration_sec : 0.f;
    }

    // Fill out_mats[0..kMaxBones-1] with the final bone matrices for the
    // given clip at time_sec (loops). Call before Draw() each frame.
    void ComputeBones(int clip_idx, float time_sec,
                      glm::mat4* out_mats, int max_out) const;

    // Load animations from a separate file and append them to this model's
    // clip list. name_override replaces the clip name embedded in the file
    // (pass "" to keep the file's own name). Returns the index of the first
    // appended clip, or -1 on failure.
    int AppendAnimationsFrom(const char* path, const char* name_override = "");

private:
    std::vector<SubMesh>       meshes_;
    std::string                directory_;

    std::vector<AnimNode>      anim_nodes_;
    std::map<std::string,int>  node_map_;
    std::vector<BoneInfo>      bones_;
    std::map<std::string,int>  bone_map_;
    std::vector<AnimClip>      clips_;
    glm::mat4                  global_inv_ = glm::mat4(1.f);
    bool                       skinned_    = false;

    void    BuildNodeTree(aiNode* node, int parent_idx);
    void    LoadAnimations(const aiScene* scene);
    void    ProcessNode(aiNode* node, const aiScene* scene,
                        std::vector<std::vector<glm::ivec4>>& bids,
                        std::vector<std::vector<glm::vec4>>&  bwts);
    SubMesh ProcessMesh(aiMesh* mesh, const aiScene* scene,
                        std::vector<glm::ivec4>& bids,
                        std::vector<glm::vec4>&  bwts);
    GLuint  LoadTex(const aiScene* scene, const std::string& path, bool srgb) const;
    void    GeneratePlaceholder();
    static void FreeMesh(SubMesh& m);
};

} // namespace rco::renderer
