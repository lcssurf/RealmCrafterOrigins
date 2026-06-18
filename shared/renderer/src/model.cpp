#include "rco/renderer/model.h"
#include "rco/renderer/material.h"
#include "rco/renderer/mesh_consolidate.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <stb_image.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <cctype>
#include <memory>
#include <unordered_map>
#include <chrono>
#include <cstdlib>
#include <vector>

namespace rco::renderer {

static bool VerboseAssetLogsEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("RCO_ASSET_LOG_VERBOSE");
        if (!v) return false;
        return std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 ||
               std::strcmp(v, "TRUE") == 0 || std::strcmp(v, "on") == 0 ||
               std::strcmp(v, "ON") == 0;
    }();
    return enabled;
}

// ---------------------------------------------------------------------------
// Animation file cache — raw parsed data keyed by file path.
// Channels have the pipe-prefix stripped but are NOT filtered by any specific
// model's node_map_ — that filtering happens at AppendAnimationsFrom() time so
// different body skeletons can share the same cached FBX parse.
// ---------------------------------------------------------------------------
struct RawAnimChannel {
    std::string           name;
    std::vector<AnimKey3> pos;
    std::vector<AnimKeyQ> rot;
    std::vector<AnimKey3> scl;
};
struct RawAnimClip {
    std::string                 fbx_name;
    float                       duration_sec = 0.f;
    float                       fps          = 30.f;
    std::vector<RawAnimChannel> channels;
};
struct AnimFileCache { std::vector<RawAnimClip> clips; };
static std::unordered_map<std::string, std::shared_ptr<AnimFileCache>> g_anim_file_cache;

// ---------------------------------------------------------------------------
// Global texture cache — keyed by "path|srgb" so the same file loaded as
// sRGB (albedo) vs. linear (normal/ORM) gets distinct entries.
// Textures live for the process lifetime (appropriate for a game that always
// needs them). Cached handles are NEVER pushed into Model::owned_textures_
// because multiple models share the same GL object.
// ---------------------------------------------------------------------------
static std::unordered_map<std::string, GLuint> g_tex_cache;

static GLuint LoadTexCached(const std::string& path, bool srgb) {
    if (path.empty()) return 0;
    std::string key = path + (srgb ? "|srgb" : "|lin");
    auto it = g_tex_cache.find(key);
    if (it != g_tex_cache.end()) {
        return it->second;
    }
    if (VerboseAssetLogsEnabled())
        std::fprintf(stderr, "[tex-cache] MISS '%s'\n", path.c_str());
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) {
        std::fprintf(stderr, "[tex-cache] ERROR can't read '%s': %s\n",
                     path.c_str(), stbi_failure_reason());
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLenum internal = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_REPEAT);
    stbi_image_free(px);
    g_tex_cache[key] = tex;
    return tex;
}

static std::string NormalizeSlashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

static std::vector<std::string> BuildTextureCandidates(const std::string& model_dir,
                                                       const std::string& in_path) {
    std::vector<std::string> out;
    auto push_unique = [&](const std::string& p) {
        if (p.empty()) return;
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };

    std::string path = NormalizeSlashes(in_path);
    while (path.rfind("./", 0) == 0) path.erase(0, 2);
    while (!path.empty() && path.front() == '/') path.erase(path.begin());

    std::string basename = path;
    if (auto slash = basename.find_last_of('/'); slash != std::string::npos)
        basename = basename.substr(slash + 1);

    // 1) Standard relative-to-model-dir resolution.
    push_unique(model_dir + "/" + path);
    // 2) As-provided relative path.
    push_unique(path);
    // 3) Legacy absolute path fallback: use basename in model dir.
    push_unique(model_dir + "/" + basename);
    // 4) Generic texture roots used in this project.
    push_unique("assets/textures/" + basename);
    push_unique("assets/textures/textures/" + basename);

    // 5) If exporter embedded a ".../textures/..." path, keep only tail after textures/.
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string token = "textures/";
    if (auto pos = lower.find(token); pos != std::string::npos) {
        std::string tail = path.substr(pos + token.size());
        push_unique(model_dir + "/" + tail);
        push_unique("assets/textures/" + tail);
        push_unique("assets/textures/textures/" + tail);
    }

    return out;
}

// ---------------------------------------------------------------------------
// Shared default textures
// ---------------------------------------------------------------------------
static GLuint s_def_albedo = 0;
static GLuint s_def_normal = 0;
static GLuint s_def_orm    = 0;

static GLuint MakeTex1x1(uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool srgb) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    const uint8_t px[4] = {r, g, b, a};
    GLenum internal = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, internal, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

static void EnsureDefaults() {
    if (s_def_albedo) return;
    s_def_albedo = MakeTex1x1(255, 255, 255, 255, true);
    s_def_normal = MakeTex1x1(128, 128, 255, 255, false);
    // ORM default: R=AO(1), G=Roughness multiplier(1), B=Metallic multiplier(1).
    // The shader does `roughness = orm.g * uRoughnessFactor`, so texture=1
    // makes the factor uniform the final value. Previously G was 128 (which
    // halved the factor into an unintended mirror-like surface) and B was 0
    // (which zeroed out the metallic factor entirely).
    s_def_orm    = MakeTex1x1(255, 255, 255, 255, false);
}

// ---------------------------------------------------------------------------
// Assimp ↔ GLM helpers
// ---------------------------------------------------------------------------
static glm::mat4 ToGLM(const aiMatrix4x4& m) {
    glm::mat4 r;
    r[0][0]=m.a1; r[1][0]=m.a2; r[2][0]=m.a3; r[3][0]=m.a4;
    r[0][1]=m.b1; r[1][1]=m.b2; r[2][1]=m.b3; r[3][1]=m.b4;
    r[0][2]=m.c1; r[1][2]=m.c2; r[2][2]=m.c3; r[3][2]=m.c4;
    r[0][3]=m.d1; r[1][3]=m.d2; r[2][3]=m.d3; r[3][3]=m.d4;
    return r;
}

static glm::quat ToGLM(const aiQuaternion& q) {
    return glm::quat(q.w, q.x, q.y, q.z);
}

// ---------------------------------------------------------------------------
// Keyframe interpolation helpers
// ---------------------------------------------------------------------------
static glm::vec3 InterpV(const std::vector<AnimKey3>& keys, double t) {
    if (keys.empty()) return glm::vec3(0.f);
    if (t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t)  return keys.back().v;
    for (int i = 0; i + 1 < (int)keys.size(); ++i) {
        if (t < keys[i+1].t) {
            float f = float((t - keys[i].t) / (keys[i+1].t - keys[i].t));
            return glm::mix(keys[i].v, keys[i+1].v, f);
        }
    }
    return keys.back().v;
}

static glm::quat InterpQ(const std::vector<AnimKeyQ>& keys, double t) {
    if (keys.empty()) return glm::quat(1.f,0.f,0.f,0.f);
    if (t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t)  return keys.back().v;
    for (int i = 0; i + 1 < (int)keys.size(); ++i) {
        if (t < keys[i+1].t) {
            float f = float((t - keys[i].t) / (keys[i+1].t - keys[i].t));
            return glm::slerp(keys[i].v, keys[i+1].v, f);
        }
    }
    return keys.back().v;
}

// ---------------------------------------------------------------------------
// Placeholder box (blue, 0.5×1.8×0.3, feet at y=0)
// Vertex layout: pos(3) normal(3) uv(2) tangent(3) = 11 floats
// ---------------------------------------------------------------------------
static const float kBoxVerts[] = {
    -0.25f,0.f,0.15f,  0,0,1, 0,0, 1,0,0,
     0.25f,0.f,0.15f,  0,0,1, 1,0, 1,0,0,
     0.25f,1.8f,0.15f, 0,0,1, 1,1, 1,0,0,
    -0.25f,1.8f,0.15f, 0,0,1, 0,1, 1,0,0,

     0.25f,0.f,-0.15f, 0,0,-1, 0,0, -1,0,0,
    -0.25f,0.f,-0.15f, 0,0,-1, 1,0, -1,0,0,
    -0.25f,1.8f,-0.15f,0,0,-1, 1,1, -1,0,0,
     0.25f,1.8f,-0.15f,0,0,-1, 0,1, -1,0,0,

    -0.25f,0.f,-0.15f, -1,0,0, 0,0, 0,0,1,
    -0.25f,0.f, 0.15f, -1,0,0, 1,0, 0,0,1,
    -0.25f,1.8f,0.15f, -1,0,0, 1,1, 0,0,1,
    -0.25f,1.8f,-0.15f,-1,0,0, 0,1, 0,0,1,

     0.25f,0.f, 0.15f,  1,0,0, 0,0, 0,0,-1,
     0.25f,0.f,-0.15f,  1,0,0, 1,0, 0,0,-1,
     0.25f,1.8f,-0.15f, 1,0,0, 1,1, 0,0,-1,
     0.25f,1.8f, 0.15f, 1,0,0, 0,1, 0,0,-1,

    -0.25f,1.8f,0.15f,  0,1,0, 0,0, 1,0,0,
     0.25f,1.8f,0.15f,  0,1,0, 1,0, 1,0,0,
     0.25f,1.8f,-0.15f, 0,1,0, 1,1, 1,0,0,
    -0.25f,1.8f,-0.15f, 0,1,0, 0,1, 1,0,0,

    -0.25f,0.f,-0.15f, 0,-1,0, 0,0, 1,0,0,
     0.25f,0.f,-0.15f, 0,-1,0, 1,0, 1,0,0,
     0.25f,0.f, 0.15f, 0,-1,0, 1,1, 1,0,0,
    -0.25f,0.f, 0.15f, 0,-1,0, 0,1, 1,0,0,
};
static const unsigned kBoxIdx[] = {
    0,1,2,2,3,0, 4,5,6,6,7,4, 8,9,10,10,11,8,
    12,13,14,14,15,12, 16,17,18,18,19,16, 20,21,22,22,23,20,
};

// ---------------------------------------------------------------------------
// FreeMesh / Destroy
// ---------------------------------------------------------------------------
Model::~Model() { Destroy(); }

void Model::FreeMesh(SubMesh& m) {
    if (m.vao)      glDeleteVertexArrays(1, &m.vao);
    if (m.vbo)      glDeleteBuffers(1, &m.vbo);
    if (m.bone_vbo) glDeleteBuffers(1, &m.bone_vbo);
    if (m.ebo)      glDeleteBuffers(1, &m.ebo);
    auto del = [](GLuint t) {
        if (t && t != s_def_albedo && t != s_def_normal && t != s_def_orm)
            glDeleteTextures(1, &t);
    };
    del(m.tex_albedo);
    del(m.tex_normal);
    del(m.tex_orm);
    del(m.tex_opacity);
    del(m.tex_ao);
    m = {};
}

void Model::Destroy() {
    for (auto& m : meshes_) FreeMesh(m);
    meshes_.clear();
    anim_nodes_.clear();
    node_map_.clear();
    bones_.clear();
    bone_map_.clear();
    clips_.clear();
    bone_world_transforms_.clear();
    skinned_    = false;
    aabb_max_y_ = 0.f;
    aabb_min_   = glm::vec3( 1e30f);
    aabb_max_   = glm::vec3(-1e30f);
    for (GLuint t : owned_textures_) {
        if (t) glDeleteTextures(1, &t);
    }
    owned_textures_.clear();
}

// ---------------------------------------------------------------------------
// Texture loader
// ---------------------------------------------------------------------------
GLuint Model::LoadTex(const aiScene* scene, const std::string& path, bool srgb) const {
    stbi_set_flip_vertically_on_load(false);
    unsigned char* px = nullptr;
    int w = 0, h = 0;

    if (!path.empty() && path[0] == '*') {
        int idx = std::stoi(path.substr(1));
        if (idx < 0 || idx >= (int)scene->mNumTextures) return 0;
        const aiTexture* t = scene->mTextures[idx];
        if (t->mHeight == 0) {
            int ch;
            px = stbi_load_from_memory(
                reinterpret_cast<const stbi_uc*>(t->pcData),
                static_cast<int>(t->mWidth), &w, &h, &ch, 4);
            if (!px)
                std::fprintf(stderr,
                    "[model] embedded tex '%s' fmt='%s' size=%u decode failed: %s\n",
                    path.c_str(), t->achFormatHint, t->mWidth, stbi_failure_reason());
        } else {
            w = t->mWidth; h = t->mHeight;
            px = new unsigned char[w * h * 4];
            for (int i = 0; i < w * h; ++i) {
                const aiTexel& tx = t->pcData[i];
                px[i*4+0]=tx.r; px[i*4+1]=tx.g;
                px[i*4+2]=tx.b; px[i*4+3]=tx.a;
            }
        }
    } else {
        int ch;
        std::vector<std::string> tried;
        const auto candidates = BuildTextureCandidates(directory_, path);
        for (const auto& c : candidates) {
            tried.push_back(c);
            px = stbi_load(c.c_str(), &w, &h, &ch, 4);
            if (px) break;
        }
        if (!px && VerboseAssetLogsEnabled()) {
            std::fprintf(stderr, "[model] texture not found: source='%s' tried=%zu\n",
                path.c_str(), tried.size());
            for (const auto& t : tried)
                std::fprintf(stderr, "  -> '%s'\n", t.c_str());
        }
    }
    if (!px) return 0;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLenum internal = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    if (!path.empty() && path[0] == '*') {
        const aiTexture* t = scene->mTextures[std::stoi(path.substr(1))];
        if (t->mHeight == 0) stbi_image_free(px); else delete[] px;
    } else {
        stbi_image_free(px);
    }
    return tex;
}

// ---------------------------------------------------------------------------
// Placeholder
// ---------------------------------------------------------------------------
void Model::GeneratePlaceholder() {
    EnsureDefaults();
    SubMesh m;
    m.albedo_factor    = {0.45f, 0.60f, 0.80f};
    m.roughness_factor = 0.6f;
    m.idx_count        = (int)(sizeof(kBoxIdx) / sizeof(kBoxIdx[0]));

    GLint prev_vao_ph = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao_ph);
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glGenBuffers(1, &m.ebo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kBoxVerts), kBoxVerts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(kBoxIdx), kBoxIdx, GL_STATIC_DRAW);
    constexpr int stride = 11 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);  glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)12); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)24); glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, (void*)32); glEnableVertexAttribArray(3);
    glBindVertexArray((GLuint)prev_vao_ph);
    meshes_.push_back(m);
}

// ---------------------------------------------------------------------------
// Node tree (depth-first; parent always inserted before children)
// ---------------------------------------------------------------------------
void Model::BuildNodeTree(aiNode* node, int parent_idx) {
    int idx = (int)anim_nodes_.size();
    AnimNode an;
    an.name   = node->mName.C_Str();
    an.parent = parent_idx;
    an.local  = ToGLM(node->mTransformation);
    node_map_[an.name] = idx;
    anim_nodes_.push_back(an);

    for (unsigned i = 0; i < node->mNumChildren; ++i)
        BuildNodeTree(node->mChildren[i], idx);
}

// ---------------------------------------------------------------------------
// Animation clips
// ---------------------------------------------------------------------------
void Model::LoadAnimations(const aiScene* scene) {
    for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation* src = scene->mAnimations[ai];
        AnimClip clip;
        clip.name = src->mName.C_Str();
        double tps = src->mTicksPerSecond > 0.0 ? src->mTicksPerSecond : 25.0;
        clip.duration_sec = float(src->mDuration / tps);
        clip.fps          = float(tps);

        for (unsigned ci = 0; ci < src->mNumChannels; ++ci) {
            const aiNodeAnim* ch = src->mChannels[ci];
            BoneChannel bc;
            bc.name = ch->mNodeName.C_Str();
            for (unsigned k = 0; k < ch->mNumPositionKeys; ++k)
                bc.pos.push_back({ch->mPositionKeys[k].mTime / tps,
                    {ch->mPositionKeys[k].mValue.x,
                     ch->mPositionKeys[k].mValue.y,
                     ch->mPositionKeys[k].mValue.z}});
            for (unsigned k = 0; k < ch->mNumRotationKeys; ++k)
                bc.rot.push_back({ch->mRotationKeys[k].mTime / tps,
                    ToGLM(ch->mRotationKeys[k].mValue)});
            for (unsigned k = 0; k < ch->mNumScalingKeys; ++k)
                bc.scl.push_back({ch->mScalingKeys[k].mTime / tps,
                    {ch->mScalingKeys[k].mValue.x,
                     ch->mScalingKeys[k].mValue.y,
                     ch->mScalingKeys[k].mValue.z}});

            clip.chan_map[bc.name] = (int)clip.channels.size();
            clip.channels.push_back(std::move(bc));
        }
        clips_.push_back(std::move(clip));
    }
}

// ---------------------------------------------------------------------------
// AliasClip — rename an embedded clip to a game action name
// ---------------------------------------------------------------------------
void Model::AliasClip(const std::string& native_name, const std::string& new_name) {
    for (auto& clip : clips_) {
        if (clip.name == native_name) {
            if (log_bones)
                std::fprintf(stderr, "[model] AliasClip '%s' -> '%s'\n",
                             native_name.c_str(), new_name.c_str());
            clip.name = new_name;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// ComputeBones — evaluate animation, fill out_mats
// ---------------------------------------------------------------------------
void Model::ComputeBones(int clip_idx, float time_sec, int mesh_idx,
                         glm::mat4* out_mats, int max_out) const {
    for (int i = 0; i < std::min(max_out, kMaxBones); ++i)
        out_mats[i] = glm::mat4(1.f);

    if (bones_.empty()) return;

    // Pick which offset table to use. If mesh_idx is out of range or the
    // submesh carries no per-mesh offsets (e.g. unskinned), fall back to the
    // legacy global bones_[].offset — preserves behaviour for single-mesh
    // models where per-submesh offsets weren't needed.
    const std::vector<glm::mat4>* offsets = nullptr;
    if (mesh_idx >= 0 && mesh_idx < (int)meshes_.size() &&
        !meshes_[mesh_idx].bone_offsets.empty()) {
        offsets = &meshes_[mesh_idx].bone_offsets;
    }

    auto offsetFor = [&](int bidx) -> glm::mat4 {
        if (offsets && bidx < (int)offsets->size()) return (*offsets)[bidx];
        return bones_[bidx].offset;
    };

    // When no clip is playing we still want static bone placement (bind pose)
    // so vertices attached to bones with non-identity rest position end up
    // visible. Compute global[] using only node.local transforms.
    const AnimClip* clip = (clip_idx >= 0 && clip_idx < (int)clips_.size())
                         ? &clips_[clip_idx] : nullptr;
    float t = 0.f;
    if (clip) {
        t = clip->duration_sec > 0.f ? std::fmod(time_sec, clip->duration_sec) : 0.f;
    }

    std::vector<glm::mat4> global(anim_nodes_.size(), glm::mat4(1.f));
    for (int ni = 0; ni < (int)anim_nodes_.size(); ++ni) {
        const AnimNode& node = anim_nodes_[ni];

        glm::mat4 local = node.local;
        if (clip) {
            auto it = clip->chan_map.find(node.name);
            if (it != clip->chan_map.end()) {
                const BoneChannel& ch = clip->channels[it->second];
                glm::quat rot = InterpQ(ch.rot, t);
                glm::vec3 scl = ch.scl.empty() ? glm::vec3(1.f) : InterpV(ch.scl, t);
                glm::vec3 pos;
                if (node.name == "mixamorig:Hips") {
                    // Apply Hips translation as delta from the clip's own frame-0,
                    // anchored to the body's bind-pose Hips position. This retargets
                    // clips exported from a differently-proportioned reference rig
                    // (e.g. Mixamo Y Bot) onto the Dwarf skeleton without vertical shift.
                    glm::vec3 anim_pos  = InterpV(ch.pos, t);
                    glm::vec3 anim_base = ch.pos.empty() ? glm::vec3(0.f) : glm::vec3(ch.pos[0].v);
                    pos = hips_bind_pos_ + (anim_pos - anim_base);
                } else {
                    // Non-root bones: translation comes from the body's bind pose so
                    // the skeleton's proportions are preserved regardless of the clip's rig.
                    pos = glm::vec3(node.local[3]);
                }
                local = glm::translate(glm::mat4(1.f), pos)
                      * glm::mat4_cast(rot)
                      * glm::scale(glm::mat4(1.f), scl);
            }
        }

        global[ni] = (node.parent < 0) ? local : global[node.parent] * local;

        if (node.bone_idx >= 0 && node.bone_idx < max_out) {
            glm::mat4 bone_world = global_inv_ * global[ni];
            if (node.bone_idx < (int)bone_world_transforms_.size())
                bone_world_transforms_[node.bone_idx] = bone_world;
            out_mats[node.bone_idx] = bone_world * offsetFor(node.bone_idx);
        }
    }

}

// ---------------------------------------------------------------------------
// ComputeBlendedBones — blend two clips in LOCAL bone space before composing
// ---------------------------------------------------------------------------

// Extract rotation + scale from a bind-pose local matrix (used when a node
// has no channel in one of the two clips being blended).
static void BindPoseRS(const glm::mat4& local, glm::quat& r_out, glm::vec3& s_out) {
    glm::vec3 c0(local[0]), c1(local[1]), c2(local[2]);
    s_out.x = glm::length(c0);
    s_out.y = glm::length(c1);
    s_out.z = glm::length(c2);
    glm::mat3 rot;
    rot[0] = c0 / (s_out.x > 1e-4f ? s_out.x : 1.f);
    rot[1] = c1 / (s_out.y > 1e-4f ? s_out.y : 1.f);
    rot[2] = c2 / (s_out.z > 1e-4f ? s_out.z : 1.f);
    r_out = glm::quat_cast(rot);
}

void Model::ComputeBlendedBones(int clip_from, float t_from,
                                int clip_to,   float t_to,
                                float alpha,   int mesh_idx,
                                glm::mat4* out_mats, int max_out) const {
    // Edge cases — no need to blend
    if (alpha <= 0.f) { ComputeBones(clip_from, t_from, mesh_idx, out_mats, max_out); return; }
    if (alpha >= 1.f) { ComputeBones(clip_to,   t_to,   mesh_idx, out_mats, max_out); return; }

    for (int i = 0; i < std::min(max_out, kMaxBones); ++i)
        out_mats[i] = glm::mat4(1.f);

    if (bones_.empty()) return;

    const std::vector<glm::mat4>* offsets = nullptr;
    if (mesh_idx >= 0 && mesh_idx < (int)meshes_.size() &&
        !meshes_[mesh_idx].bone_offsets.empty())
        offsets = &meshes_[mesh_idx].bone_offsets;

    auto offsetFor = [&](int bidx) -> glm::mat4 {
        if (offsets && bidx < (int)offsets->size()) return (*offsets)[bidx];
        return bones_[bidx].offset;
    };

    const AnimClip* ca = (clip_from >= 0 && clip_from < (int)clips_.size()) ? &clips_[clip_from] : nullptr;
    const AnimClip* cb = (clip_to   >= 0 && clip_to   < (int)clips_.size()) ? &clips_[clip_to]   : nullptr;

    const float ta = ca ? (ca->duration_sec > 0.f ? std::fmod(t_from, ca->duration_sec) : 0.f) : 0.f;
    const float tb = cb ? (cb->duration_sec > 0.f ? std::fmod(t_to,   cb->duration_sec) : 0.f) : 0.f;

    std::vector<glm::mat4> global(anim_nodes_.size(), glm::mat4(1.f));

    for (int ni = 0; ni < (int)anim_nodes_.size(); ++ni) {
        const AnimNode& node = anim_nodes_[ni];

        // ── Sample clip A in local space ──────────────────────────────────────
        glm::quat ra; glm::vec3 sa, pa = glm::vec3(node.local[3]);
        {
            bool has_chan = false;
            if (ca) {
                auto it = ca->chan_map.find(node.name);
                if (it != ca->chan_map.end()) {
                    const BoneChannel& ch = ca->channels[it->second];
                    ra = InterpQ(ch.rot, ta);
                    sa = ch.scl.empty() ? glm::vec3(1.f) : InterpV(ch.scl, ta);
                    if (node.name == "mixamorig:Hips") {
                        glm::vec3 ap = InterpV(ch.pos, ta);
                        glm::vec3 ab = ch.pos.empty() ? glm::vec3(0.f) : glm::vec3(ch.pos[0].v);
                        pa = hips_bind_pos_ + (ap - ab);
                    }
                    has_chan = true;
                }
            }
            if (!has_chan) BindPoseRS(node.local, ra, sa);
        }

        // ── Sample clip B in local space ──────────────────────────────────────
        glm::quat rb; glm::vec3 sb, pb = glm::vec3(node.local[3]);
        {
            bool has_chan = false;
            if (cb) {
                auto it = cb->chan_map.find(node.name);
                if (it != cb->chan_map.end()) {
                    const BoneChannel& ch = cb->channels[it->second];
                    rb = InterpQ(ch.rot, tb);
                    sb = ch.scl.empty() ? glm::vec3(1.f) : InterpV(ch.scl, tb);
                    if (node.name == "mixamorig:Hips") {
                        glm::vec3 ap = InterpV(ch.pos, tb);
                        glm::vec3 ab = ch.pos.empty() ? glm::vec3(0.f) : glm::vec3(ch.pos[0].v);
                        pb = hips_bind_pos_ + (ap - ab);
                    }
                    has_chan = true;
                }
            }
            if (!has_chan) BindPoseRS(node.local, rb, sb);
        }

        // ── NLERP blend in local space (UE FastLerp: lerp + normalize + antipodal) ──
        if (glm::dot(ra, rb) < 0.f) rb = -rb;
        glm::quat r_blend = glm::normalize(glm::mix(ra, rb, alpha));
        glm::vec3 s_blend = glm::mix(sa, sb, alpha);
        glm::vec3 p_blend = glm::mix(pa, pb, alpha);

        glm::mat4 local = glm::translate(glm::mat4(1.f), p_blend)
                        * glm::mat4_cast(r_blend)
                        * glm::scale(glm::mat4(1.f), s_blend);

        global[ni] = (node.parent < 0) ? local : global[node.parent] * local;

        if (node.bone_idx >= 0 && node.bone_idx < max_out) {
            glm::mat4 bone_world = global_inv_ * global[ni];
            if (node.bone_idx < (int)bone_world_transforms_.size())
                bone_world_transforms_[node.bone_idx] = bone_world;
            out_mats[node.bone_idx] = bone_world * offsetFor(node.bone_idx);
        }
    }
}

// ---------------------------------------------------------------------------
// GetBoneWorldTransform
// ---------------------------------------------------------------------------

bool Model::GetBoneWorldTransform(const std::string& name, glm::mat4* out) const {
    auto it = bone_map_.find(name);
    if (it == bone_map_.end()) return false;
    int bidx = it->second;
    if (bidx < 0 || bidx >= (int)bone_world_transforms_.size()) return false;
    if (out) *out = bone_world_transforms_[bidx];
    return true;
}

// ---------------------------------------------------------------------------
// ProcessMesh — geometry + bone weights
// ---------------------------------------------------------------------------
SubMesh Model::ProcessMesh(aiMesh* mesh, const aiScene* scene,
                           std::vector<glm::ivec4>& bids_out,
                           std::vector<glm::vec4>&  bwts_out,
                           const glm::mat4&         node_transform) {
    std::vector<float>    verts;
    std::vector<unsigned> indices;
    verts.reserve(mesh->mNumVertices * 11);

    // For skinned meshes, the bone matrices already express each vertex's
    // world-space placement — applying the node transform on top would double
    // it. For static submeshes (no bones), the node's accumulated transform
    // must be baked into the geometry so multi-part models don't collapse to
    // the origin.
    const bool skinned = mesh->mNumBones > 0;
    const glm::mat4  xform = skinned ? glm::mat4(1.f) : node_transform;
    const glm::mat3  nxform = skinned ? glm::mat3(1.f)
                                       : glm::mat3(glm::transpose(glm::inverse(xform)));

    auto xformPos = [&](const aiVector3D& v) -> glm::vec3 {
        glm::vec4 p = xform * glm::vec4(v.x, v.y, v.z, 1.f);
        return glm::vec3(p);
    };
    auto xformDir = [&](const aiVector3D& v) -> glm::vec3 {
        return glm::normalize(nxform * glm::vec3(v.x, v.y, v.z));
    };

    // Detect which UV channel the base-color texture references and any
    // KHR_texture_transform applied. glTF exporters (Unreal, Blender) commonly
    // route material textures through TEXCOORD_1 (their TEXCOORD_0 is reserved
    // for lightmaps). Sampling the wrong channel produces garbled-looking
    // textures; ignoring the per-material UV transform causes visible seams /
    // misalignment.
    int uvIndex = 0;
    glm::vec2 uv_scale  = glm::vec2(1.f, 1.f);
    glm::vec2 uv_offset = glm::vec2(0.f, 0.f);
    // True when uv_offset/uv_scale are in glTF's V=0-at-top convention and
    // need flip-aware math when applied (the Assimp-detected KHR transform).
    // False for sidecar overrides whose values are post-flip and apply
    // directly: u_out = u * scale + offset.
    bool uv_in_gltf_convention = false;
    if (mesh->mMaterialIndex < scene->mNumMaterials) {
        aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];
        int idx = 0;
        if (mat->Get(AI_MATKEY_UVWSRC(aiTextureType_BASE_COLOR, 0), idx) == AI_SUCCESS ||
            mat->Get(AI_MATKEY_UVWSRC(aiTextureType_DIFFUSE,    0), idx) == AI_SUCCESS) {
            uvIndex = idx;
        }
        if (uvIndex >= AI_MAX_NUMBER_OF_TEXTURECOORDS ||
            !mesh->mTextureCoords[uvIndex]) {
            uvIndex = 0;
        }

        // KHR_texture_transform: Assimp exposes it as aiUVTransform — translation,
        // scaling, and rotation packed into a 2D affine. We support the common
        // case (offset + scale; rotation is rare for static meshes).
        aiUVTransform tr{};
        if (mat->Get(AI_MATKEY_UVTRANSFORM(aiTextureType_BASE_COLOR, 0), tr) == AI_SUCCESS ||
            mat->Get(AI_MATKEY_UVTRANSFORM(aiTextureType_DIFFUSE,    0), tr) == AI_SUCCESS) {
            uv_scale  = glm::vec2(tr.mScaling.x,     tr.mScaling.y);
            uv_offset = glm::vec2(tr.mTranslation.x, tr.mTranslation.y);
            uv_in_gltf_convention = true;  // KHR is in glTF top-left convention
        }
        // The .uv sidecar (written by the GUE) takes precedence and stores
        // post-flip values that go straight into the shader-equivalent math.
        if (uv_sidecar_active_) {
            uv_scale  = uv_sidecar_scale_;
            uv_offset = uv_sidecar_offset_;
            uv_in_gltf_convention = false;
        }
        // Record the post-flip equivalent of whatever transform was actually
        // applied — this is what the GUE composes live slider deltas against
        // when persisting a new sidecar.
        if (uv_in_gltf_convention) {
            uv_effective_scale_  = uv_scale;
            uv_effective_offset_ = glm::vec2(
                uv_offset.x,
                1.f - uv_scale.y - uv_offset.y);
        } else {
            uv_effective_scale_  = uv_scale;
            uv_effective_offset_ = uv_offset;
        }
        if (VerboseAssetLogsEnabled()) {
            std::fprintf(stderr,
                "[uv] mesh='%s' uvIndex=%d offset=(%.4f,%.4f) scale=(%.4f,%.4f) "
                "sidecar=%d gltf_conv=%d effective_off=(%.4f,%.4f) effective_scale=(%.4f,%.4f)\n",
                mesh->mName.C_Str(), uvIndex,
                uv_offset.x, uv_offset.y, uv_scale.x, uv_scale.y,
                uv_sidecar_active_ ? 1 : 0, uv_in_gltf_convention ? 1 : 0,
                uv_effective_offset_.x, uv_effective_offset_.y,
                uv_effective_scale_.x,  uv_effective_scale_.y);
        }
    }

    for (unsigned i = 0; i < mesh->mNumVertices; ++i) {
        glm::vec3 pos = xformPos(mesh->mVertices[i]);
        if (pos.y > aabb_max_y_) aabb_max_y_ = pos.y;
        aabb_min_ = glm::min(aabb_min_, pos);
        aabb_max_ = glm::max(aabb_max_, pos);
        verts.push_back(pos.x); verts.push_back(pos.y); verts.push_back(pos.z);
        if (mesh->HasNormals()) {
            glm::vec3 n = xformDir(mesh->mNormals[i]);
            verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
        } else { verts.push_back(0.f); verts.push_back(1.f); verts.push_back(0.f); }
        if (mesh->mTextureCoords[uvIndex]) {
            float u = mesh->mTextureCoords[uvIndex][i].x;
            float v = mesh->mTextureCoords[uvIndex][i].y;
            if (uv_in_gltf_convention) {
                // Assimp-detected KHR_texture_transform is authored against the
                // glTF V=0-at-top convention, but aiProcess_FlipUVs has already
                // inverted V (v_assimp = 1 - v_glTF). Substitute and simplify
                // to apply the transform post-flip:
                //   v_out = v_assimp * scale + (1 - scale - offset).
                u = u * uv_scale.x + uv_offset.x;
                v = v * uv_scale.y + (1.0f - uv_scale.y - uv_offset.y);
            } else {
                // Sidecar values are post-flip, applied directly.
                u = u * uv_scale.x + uv_offset.x;
                v = v * uv_scale.y + uv_offset.y;
            }
            verts.push_back(u); verts.push_back(v);
        } else { verts.push_back(0.f); verts.push_back(0.f); }
        if (mesh->HasTangentsAndBitangents()) {
            glm::vec3 t = xformDir(mesh->mTangents[i]);
            verts.push_back(t.x); verts.push_back(t.y); verts.push_back(t.z);
        } else { verts.push_back(1.f); verts.push_back(0.f); verts.push_back(0.f); }
    }

    for (unsigned i = 0; i < mesh->mNumFaces; ++i)
        for (unsigned j = 0; j < mesh->mFaces[i].mNumIndices; ++j)
            indices.push_back(mesh->mFaces[i].mIndices[j]);

    // --- Bone weights ---
    bool has_bones = mesh->mNumBones > 0;
    bids_out.assign(mesh->mNumVertices, glm::ivec4(0));
    bwts_out.assign(mesh->mNumVertices, glm::vec4(0.f));
    // Count how many weights we've filled per vertex (max 4).
    std::vector<int> slot(mesh->mNumVertices, 0);

    // Per-submesh offset matrices, keyed by the global bone index. Each aiMesh
    // carries its OWN mOffsetMatrix per bone encoding that mesh's bind-pose
    // world placement; using a single global offset picks the first mesh's
    // placement for every part and leaves the rest disassembled.
    std::vector<std::pair<int, glm::mat4>> sm_offsets;

    if (has_bones) {
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            aiBone* ab = mesh->mBones[b];
            std::string bname = ab->mName.C_Str();

            int bidx;
            auto it = bone_map_.find(bname);
            if (it == bone_map_.end()) {
                bidx = (int)bones_.size();
                if (bidx >= kMaxBones) continue;
                bone_map_[bname] = bidx;
                BoneInfo bi;
                bi.offset = ToGLM(ab->mOffsetMatrix);  // kept for backwards-compat
                bones_.push_back(bi);
                // Link bone to its node in the tree
                auto nit = node_map_.find(bname);
                if (nit != node_map_.end())
                    anim_nodes_[nit->second].bone_idx = bidx;
                else
                    std::fprintf(stderr, "[bone-miss] '%s' bone has no matching node in tree!\n", bname.c_str());
            } else {
                bidx = it->second;
            }

            // Record this mesh's own offset for this bone.
            sm_offsets.emplace_back(bidx, ToGLM(ab->mOffsetMatrix));

            for (unsigned w = 0; w < ab->mNumWeights; ++w) {
                unsigned vi = ab->mWeights[w].mVertexId;
                int      s  = slot[vi];
                if (s >= 4) continue;
                bids_out[vi][s] = bidx;
                bwts_out[vi][s] = ab->mWeights[w].mWeight;
                slot[vi]++;
            }
        }
        // Normalize weights so they sum to 1
        for (unsigned i = 0; i < mesh->mNumVertices; ++i) {
            float sum = bwts_out[i].x + bwts_out[i].y + bwts_out[i].z + bwts_out[i].w;
            if (sum > 0.f) bwts_out[i] /= sum;
            else bwts_out[i].x = 1.f; // fallback: full weight to bone 0
        }
    }

    // --- PBR material ---
    SubMesh sm;
    sm.idx_count = (int)indices.size();
    sm.skinned   = has_bones;
    if (mesh->mMaterialIndex < scene->mNumMaterials) {
        aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];
        aiString mname;
        if (mat->Get(AI_MATKEY_NAME, mname) == AI_SUCCESS)
            sm.material_name = mname.C_Str();
        aiString texPath;
        std::string albedo_path_str;
        bool has_base_color_factor = false;
        if (mat->GetTexture(AI_MATKEY_BASE_COLOR_TEXTURE, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
            albedo_path_str = texPath.C_Str();
            sm.tex_albedo = LoadTex(scene, texPath.C_Str(), true);
        }
        if (mat->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_HEIGHT,  0, &texPath) == AI_SUCCESS)
            sm.tex_normal = LoadTex(scene, texPath.C_Str(), false);
        std::string mr_path_str;
        std::string rough_path_str;
        if (mat->GetTexture(AI_MATKEY_METALLIC_TEXTURE, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_METALNESS, 0, &texPath) == AI_SUCCESS) {
            mr_path_str = texPath.C_Str();
            sm.tex_orm = LoadTex(scene, texPath.C_Str(), false);
        }
        if (mat->GetTexture(AI_MATKEY_ROUGHNESS_TEXTURE, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &texPath) == AI_SUCCESS) {
            rough_path_str = texPath.C_Str();
        }
        sm.orm_packed = false;
        if (sm.tex_orm) {
            if (!rough_path_str.empty() && rough_path_str == mr_path_str) {
                sm.orm_packed = true;
            } else if (rough_path_str.empty() && source_is_gltf_) {
                // glTF typically stores roughness+metalness in a single texture.
                sm.orm_packed = true;
            }
        }
        // Dedicated AO texture (glTF separates occlusionTexture from
        // metallicRoughnessTexture). Unreal exports always have these split, and
        // their metallicRoughness.r channel is undefined — using it as AO drives
        // the ambient term to ~0 and makes the whole model look black.
        if (mat->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_LIGHTMAP,          0, &texPath) == AI_SUCCESS) {
            // Skip when AO and metallicRoughness reference the same texture
            // (true ORM-packed); the shader's ormPacked path handles it.
            if (!sm.orm_packed || mr_path_str != texPath.C_Str())
                sm.tex_ao = LoadTex(scene, texPath.C_Str(), false);
        }
        if (mat->GetTexture(aiTextureType_OPACITY, 0, &texPath) == AI_SUCCESS) {
            // glTF alphaMode=MASK exporters (Unreal, Blender) often expose the
            // base-color texture itself as the "opacity" texture so we can use
            // its alpha channel for cutout. Our bindless shader samples the .r
            // channel from a dedicated grayscale opacity map, so feeding it the
            // base-color RGBA produces wrong discards (dark pixels disappear,
            // making the whole mesh black). Skip when paths match.
            if (albedo_path_str != texPath.C_Str())
                sm.tex_opacity = LoadTex(scene, texPath.C_Str(), false);
            else if (VerboseAssetLogsEnabled())
                std::fprintf(stderr,
                    "[mat] '%s' opacity == albedo path; skipping (glTF MASK alpha mode)\n",
                    sm.material_name.c_str());
        }
        aiColor4D c;
        if (mat->Get(AI_MATKEY_BASE_COLOR, c) == AI_SUCCESS ||
            mat->Get(AI_MATKEY_COLOR_DIFFUSE, c) == AI_SUCCESS) {
            sm.albedo_factor = {c.r, c.g, c.b};
            has_base_color_factor = true;
        }
        float rough = 1.f, metal = 0.f;
        const bool has_rough = (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, rough) == AI_SUCCESS);
        const bool has_metal = (mat->Get(AI_MATKEY_METALLIC_FACTOR,  metal) == AI_SUCCESS);

        // Normalize imported PBR factors for stability across mixed/legacy assets.
        if (!has_base_color_factor) {
            // Avoid black-tinted materials when base color factor is missing.
            sm.albedo_factor = glm::vec3(1.f);
        } else {
            sm.albedo_factor.r = glm::clamp(sm.albedo_factor.r, 0.f, 1.f);
            sm.albedo_factor.g = glm::clamp(sm.albedo_factor.g, 0.f, 1.f);
            sm.albedo_factor.b = glm::clamp(sm.albedo_factor.b, 0.f, 1.f);
        }

        if (!has_rough) {
            // Without explicit roughness, let ORM texture drive (factor=1)
            // or use a matte fallback for legacy materials.
            rough = (sm.tex_orm && sm.orm_packed) ? 1.f : 0.85f;
        }
        if (!has_metal) {
            // Legacy props are usually dielectric.
            metal = 0.f;
        }
        sm.roughness_factor = glm::clamp(rough, 0.f, 1.f);
        sm.metallic_factor  = glm::clamp(metal, 0.f, 1.f);
    }

    // --- Build VAO ---
    GLint prev_vao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGenVertexArrays(1, &sm.vao);
    glGenBuffers(1, &sm.vbo);
    glGenBuffers(1, &sm.ebo);
    glBindVertexArray(sm.vao);

    glBindBuffer(GL_ARRAY_BUFFER, sm.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sm.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size()*sizeof(unsigned), indices.data(), GL_STATIC_DRAW);

    constexpr int stride = 11 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);  glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)12); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)24); glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, stride, (void*)32); glEnableVertexAttribArray(3);

    if (has_bones) {
        // Interleaved bone buffer: [ivec4 ids | vec4 weights] per vertex
        struct BoneVertex { glm::ivec4 ids; glm::vec4 wts; };
        std::vector<BoneVertex> bv(mesh->mNumVertices);
        for (unsigned i = 0; i < mesh->mNumVertices; ++i) {
            bv[i].ids = bids_out[i];
            bv[i].wts = bwts_out[i];
        }
        glGenBuffers(1, &sm.bone_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, sm.bone_vbo);
        glBufferData(GL_ARRAY_BUFFER, bv.size()*sizeof(BoneVertex), bv.data(), GL_STATIC_DRAW);

        constexpr int bstride = sizeof(BoneVertex);
        // location 4 = ivec4 bone ids
        glVertexAttribIPointer(4, 4, GL_INT, bstride, (void*)0);
        glEnableVertexAttribArray(4);
        // location 5 = vec4 bone weights
        glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, bstride, (void*)sizeof(glm::ivec4));
        glEnableVertexAttribArray(5);
    }

    glBindVertexArray((GLuint)prev_vao);

    // Record per-bone offsets for this submesh. They will be padded with
    // identity for unused bone slots after all meshes have been loaded,
    // once the final bones_.size() is known (see Load()).
    sm.bone_offsets.reserve(sm_offsets.size());
    for (auto& [bidx, off] : sm_offsets) {
        if ((int)sm.bone_offsets.size() <= bidx)
            sm.bone_offsets.resize(bidx + 1, glm::mat4(1.f));
        sm.bone_offsets[bidx] = off;
    }

    sm.raw_verts        = std::move(verts);
    sm.raw_indices      = std::move(indices);
    sm.raw_vertex_count = (int)mesh->mNumVertices;

    // Preserve bone data for consolidation. For non-skinned submeshes bids_out
    // and bwts_out are already zeroed (assigned at the top of this function),
    // so the loop is uniform regardless of has_bones.
    {
        const unsigned nv = mesh->mNumVertices;
        sm.raw_bone_ids.resize(nv * 4);
        sm.raw_bone_weights.resize(nv * 4);
        for (unsigned i = 0; i < nv; ++i) {
            sm.raw_bone_ids    [i*4+0] = bids_out[i].x;
            sm.raw_bone_ids    [i*4+1] = bids_out[i].y;
            sm.raw_bone_ids    [i*4+2] = bids_out[i].z;
            sm.raw_bone_ids    [i*4+3] = bids_out[i].w;
            sm.raw_bone_weights[i*4+0] = bwts_out[i].x;
            sm.raw_bone_weights[i*4+1] = bwts_out[i].y;
            sm.raw_bone_weights[i*4+2] = bwts_out[i].z;
            sm.raw_bone_weights[i*4+3] = bwts_out[i].w;
        }
    }

    return sm;
}

void Model::ProcessNode(aiNode* node, const aiScene* scene,
                        std::vector<std::vector<glm::ivec4>>& bids,
                        std::vector<std::vector<glm::vec4>>&  bwts,
                        const glm::mat4& parent_xform) {
    const glm::mat4 world = parent_xform * ToGLM(node->mTransformation);
    for (unsigned i = 0; i < node->mNumMeshes; ++i) {
        bids.emplace_back();
        bwts.emplace_back();
        meshes_.push_back(ProcessMesh(scene->mMeshes[node->mMeshes[i]], scene,
                                      bids.back(), bwts.back(), world));
    }
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        ProcessNode(node->mChildren[i], scene, bids, bwts, world);
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
// Try to read a "<model_path>.uv" sidecar containing 4 floats:
// "offset_x offset_y scale_x scale_y". When present, the values override the
// glTF KHR_texture_transform detected by Assimp — useful when an exporter
// (Unreal, older Blender) omits or mis-applies the transform. Returns true
// only when the file exists AND parses successfully.
static bool ReadUVSidecar(const char* model_path,
                          glm::vec2& out_offset, glm::vec2& out_scale) {
    std::string p = std::string(model_path) + ".uv";
    FILE* f = std::fopen(p.c_str(), "r");
    if (!f) return false;
    float ox = 0.f, oy = 0.f, sx = 1.f, sy = 1.f;
    int n = std::fscanf(f, "%f %f %f %f", &ox, &oy, &sx, &sy);
    std::fclose(f);
    if (n != 4) return false;
    out_offset = glm::vec2(ox, oy);
    out_scale  = glm::vec2(sx, sy);
    if (VerboseAssetLogsEnabled())
        std::fprintf(stderr,
            "[uv-sidecar] '%s' override offset=(%.4f,%.4f) scale=(%.4f,%.4f)\n",
            p.c_str(), ox, oy, sx, sy);
    return true;
}

bool Model::Load(const char* path, MaterialManager* mm) {
    EnsureDefaults();
    source_is_gltf_ = false;
    if (path && *path) {
        std::string lower(path);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        source_is_gltf_ = (lower.size() >= 5 &&
                           (lower.rfind(".gltf") == lower.size() - 5 ||
                            lower.rfind(".glb")  == lower.size() - 4));
    }

    // Sidecar override (per-model UV transform persisted by the GUE).
    glm::vec2 sidecar_offset(0.f);
    glm::vec2 sidecar_scale (1.f);
    const bool has_sidecar = ReadUVSidecar(path, sidecar_offset, sidecar_scale);
    uv_sidecar_offset_ = sidecar_offset;
    uv_sidecar_scale_  = sidecar_scale;
    uv_sidecar_active_ = has_sidecar;

    Assimp::Importer importer;
    // Normalise to metres. FBX from Maya/3ds Max typically stores positions
    // in centimetres (UnitScaleFactor = 100); Blender stores in the scene
    // unit (usually metres). glTF/GLB is always metres by spec. Setting
    // aiProcess_GlobalScale + factor=1.0 tells Assimp to read the file's own
    // unit metadata and divide positions by it, so a 1.8 m character stays
    // 1.8 units regardless of source DCC.
    importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);

    // By default Assimp splits every FBX node into up to a dozen auxiliary
    // nodes ("_$AssimpFbx$_Translation", "_$AssimpFbx$_PreRotation", etc.)
    // so each FBX pivot can be animated independently. For our use case this
    // just creates deep hierarchies where armour/accessory submeshes end up
    // in the wrong place when we bake the accumulated transform. Collapsing
    // every pivot into the node's own mTransformation mirrors what Unreal
    // does via bBakePivotInVertex = false.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate      |
        aiProcess_FlipUVs          |
        aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_GlobalScale      |
        aiProcess_OptimizeGraph    | // merge redundant transform-only nodes
        aiProcess_LimitBoneWeights); // clamp to 4 bones/vertex

    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        std::fprintf(stderr, "[model] Cannot load '%s': %s — using placeholder\n",
                     path, importer.GetErrorString());
        GeneratePlaceholder();
        return false;
    }

    std::string p(path);
    size_t slash = p.find_last_of("/\\");
    directory_ = (slash != std::string::npos) ? p.substr(0, slash) : ".";

    if (VerboseAssetLogsEnabled())
        std::fprintf(stderr, "[body-load] '%s' scene has %u animations\n", path, scene->mNumAnimations);

    // Build the node hierarchy first (needed for bone→node linking in ProcessMesh).
    BuildNodeTree(scene->mRootNode, -1);
    global_inv_ = glm::inverse(ToGLM(scene->mRootNode->mTransformation));

    {
        auto nit = node_map_.find("mixamorig:Hips");
        if (nit != node_map_.end()) {
            const glm::mat4& m = anim_nodes_[nit->second].local;
            hips_bind_pos_ = glm::vec3(m[3]);
            if (VerboseAssetLogsEnabled())
                std::fprintf(stderr, "[body-node] 'mixamorig:Hips' local_pos=(%.4f,%.4f,%.4f)\n",
                             hips_bind_pos_.x, hips_bind_pos_.y, hips_bind_pos_.z);
        } else {
            if (VerboseAssetLogsEnabled())
                std::fprintf(stderr, "[body-node] 'mixamorig:Hips' NOT FOUND in node tree\n");
        }
    }

    if (log_bones) {
        // bind-pose comparison: root transform diagonal + Hips local position.
        std::function<aiNode*(aiNode*, const char*)> findNode =
            [&](aiNode* n, const char* name) -> aiNode* {
                if (std::strstr(n->mName.C_Str(), name)) return n;
                for (unsigned i = 0; i < n->mNumChildren; ++i)
                    if (auto* f = findNode(n->mChildren[i], name)) return f;
                return nullptr;
            };
        const aiMatrix4x4& rt = scene->mRootNode->mTransformation;
        std::fprintf(stderr, "[body-bind] '%s' root_transform_diag=(%.6f,%.6f,%.6f)",
                     path, rt.a1, rt.b2, rt.c3);
        aiNode* hips = findNode(scene->mRootNode, "mixamorig:Hips");
        if (hips) {
            const aiMatrix4x4& ht = hips->mTransformation;
            std::fprintf(stderr, " hips_local_pos=(%.4f,%.4f,%.4f)\n", ht.a4, ht.b4, ht.c4);
        } else {
            std::fprintf(stderr, " hips=NOT_FOUND\n");
        }
    }

    // Process meshes (bone indices are registered into bone_map_ / bones_ here).
    std::vector<std::vector<glm::ivec4>> bids;
    std::vector<std::vector<glm::vec4>>  bwts;
    // Start the static-mesh transform accumulation with global_inv_ so that
    // baked vertices end up in the same "root-normalised" space as skinned
    // meshes (see ComputeBones — bones are pre-multiplied by global_inv_).
    // Without this, FBX files that carry an axis-conversion rotation on the
    // root node leave static submeshes laid flat / rotated 90° while the
    // skinned body renders upright.
    ProcessNode(scene->mRootNode, scene, bids, bwts, global_inv_);

    if (log_bones) {
        int mxcount = 0, linked = 0;
        for (const auto& an : anim_nodes_) {
            if (an.name.find("mixamorig") != std::string::npos) ++mxcount;
            if (an.bone_idx >= 0) ++linked;
        }
        std::fprintf(stderr, "[model] '%s' total_nodes=%d mixamorig=%d bones_linked=%d/%d\n",
                     path, (int)anim_nodes_.size(), mxcount, linked, (int)bones_.size());
    }

    // Load animations after bones are known.
    if (scene->mNumAnimations > 0) {
        LoadAnimations(scene);
        skinned_ = !bones_.empty();
    }

    // Pad each submesh's bone_offsets to the final bones_.size() with
    // identity matrices for bones this particular submesh doesn't reference.
    // Keeps ComputeBones' indexing simple: bone_offsets[bidx] is always safe.
    for (auto& sm : meshes_) {
        if (!sm.skinned) continue;
        if (sm.bone_offsets.size() < bones_.size())
            sm.bone_offsets.resize(bones_.size(), glm::mat4(1.f));
    }
    bone_world_transforms_.assign(bones_.size(), glm::mat4(1.f));

    if (log_bones) {
        std::fprintf(stderr, "[model] bone_map (%d bones):\n", (int)bone_map_.size());
        for (const auto& [name, idx] : bone_map_)
            std::fprintf(stderr, "  [%2d] '%s'\n", idx, name.c_str());
    }

    // TESTE 2: log inverse-bind offset translation for key bones
    {
        static const char* kLogBones[] = {
            "mixamorig:Hips", "mixamorig:LeftUpLeg", "mixamorig:Spine"
        };
        for (const char* bname : kLogBones) {
            auto it = bone_map_.find(bname);
            if (it == bone_map_.end()) {
                if (VerboseAssetLogsEnabled())
                    std::fprintf(stderr, "[bone-offset] body bone='%s' NOT FOUND\n", bname);
                continue;
            }
            const glm::mat4& off = bones_[it->second].offset;
            if (VerboseAssetLogsEnabled())
                std::fprintf(stderr, "[bone-offset] body bone='%s' offset_translation=(%.4f,%.4f,%.4f)\n",
                             bname, off[3][0], off[3][1], off[3][2]);
        }
    }

    // Register each submesh's textures in the shared MaterialManager so the
    // deferred pipeline can sample them via its bindless SSBO.
    if (mm) {
        for (size_t i = 0; i < meshes_.size(); ++i) {
            auto& m = meshes_[i];
            // Unique name per (path, submesh) so duplicate loads dedup rather
            // than append.
            char key[512];
            std::snprintf(key, sizeof(key), "%s#%zu", path, i);
            m.material_idx = mm->RegisterFromHandles(
                key, m.tex_albedo, m.tex_normal, m.tex_orm, m.tex_opacity, m.tex_ao,
                m.orm_packed, m.albedo_factor, m.roughness_factor, m.metallic_factor, 1.0f);
        }
    }

    // === [3b] Mesh consolidation (see shared/renderer/src/mesh_consolidate.cpp) ===
    ConsolidateMeshes(*this, path);

    // Free temporary CPU geometry copies.
    for (auto& m : meshes_) {
        m.raw_verts        = {};
        m.raw_indices      = {};
        m.raw_bone_ids     = {};
        m.raw_bone_weights = {};
        m.raw_vertex_count = 0;
    }

    return true;
}

// ---------------------------------------------------------------------------
// AppendAnimationsFrom — load clips from a separate file, with FBX parse cache
// ---------------------------------------------------------------------------
int Model::AppendAnimationsFrom(const char* path, const char* name_override) {
    if (name_override && name_override[0] != '\0') {
        for (int i = 0; i < (int)clips_.size(); ++i) {
            if (clips_[i].name == name_override)
                return i;
        }
    }

    auto t0 = std::chrono::steady_clock::now();

    // ── File cache: parse FBX once, reuse raw channels for every actor ────
    std::shared_ptr<AnimFileCache> fcache;
    {
        auto it = g_anim_file_cache.find(path);
        if (it != g_anim_file_cache.end()) {
            fcache = it->second;
        } else {
            if (VerboseAssetLogsEnabled())
                std::fprintf(stderr, "[anim-cache] MISS '%s' — parsing FBX\n", path);

            Assimp::Importer importer;
            // preserve_pivots=false collapses _$AssimpFbx$_* aux nodes so
            // channel names match the body model's node tree.
            importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
            importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
            const aiScene* scene = importer.ReadFile(path,
                aiProcess_Triangulate | aiProcess_LimitBoneWeights | aiProcess_GlobalScale);

            if (!scene) {
                std::fprintf(stderr, "[model] AppendAnimationsFrom '%s': FILE NOT LOADED — %s\n",
                             path, importer.GetErrorString());
                return -1;
            }
            // AI_SCENE_FLAGS_INCOMPLETE is expected for animation-only "Without Skin" exports.
            if (scene->mNumAnimations == 0) {
                std::fprintf(stderr,
                    "[model] AppendAnimationsFrom '%s': FILE LOADED but mNumAnimations==0 "
                    "(try downloading with an animation selected on Mixamo)\n", path);
                return -1;
            }

            if (log_bones) {
                std::function<aiNode*(aiNode*, const char*)> findNode =
                    [&](aiNode* n, const char* name) -> aiNode* {
                        if (std::strstr(n->mName.C_Str(), name)) return n;
                        for (unsigned i = 0; i < n->mNumChildren; ++i)
                            if (auto* f = findNode(n->mChildren[i], name)) return f;
                        return nullptr;
                    };
                const char* label = (name_override && name_override[0]) ? name_override : path;
                const aiMatrix4x4& rt = scene->mRootNode->mTransformation;
                std::fprintf(stderr, "[clip-bind] '%s' root_transform_diag=(%.6f,%.6f,%.6f)",
                             label, rt.a1, rt.b2, rt.c3);
                aiNode* hips = findNode(scene->mRootNode, "mixamorig:Hips");
                if (hips) {
                    const aiMatrix4x4& ht = hips->mTransformation;
                    std::fprintf(stderr, " hips_local_pos=(%.4f,%.4f,%.4f)\n", ht.a4, ht.b4, ht.c4);
                } else {
                    std::fprintf(stderr, " hips=NOT_FOUND\n");
                }
            }

            fcache = std::make_shared<AnimFileCache>();
            for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai) {
                const aiAnimation* src = scene->mAnimations[ai];
                RawAnimClip raw;
                raw.fbx_name = src->mName.C_Str();
                double tps = src->mTicksPerSecond > 0.0 ? src->mTicksPerSecond : 25.0;
                raw.duration_sec = float(src->mDuration / tps);
                raw.fps          = float(tps);

                for (unsigned ci = 0; ci < src->mNumChannels; ++ci) {
                    const aiNodeAnim* ch = src->mChannels[ci];
                    RawAnimChannel rc;
                    // Mixamo "With Skin" packs names as "ArmatureName|BoneName" — strip prefix.
                    std::string raw_name = ch->mNodeName.C_Str();
                    size_t pipe = raw_name.rfind('|');
                    rc.name = (pipe != std::string::npos) ? raw_name.substr(pipe + 1) : raw_name;
                    for (unsigned k = 0; k < ch->mNumPositionKeys; ++k)
                        rc.pos.push_back({ch->mPositionKeys[k].mTime / tps,
                            {ch->mPositionKeys[k].mValue.x,
                             ch->mPositionKeys[k].mValue.y,
                             ch->mPositionKeys[k].mValue.z}});
                    for (unsigned k = 0; k < ch->mNumRotationKeys; ++k)
                        rc.rot.push_back({ch->mRotationKeys[k].mTime / tps,
                            ToGLM(ch->mRotationKeys[k].mValue)});
                    for (unsigned k = 0; k < ch->mNumScalingKeys; ++k)
                        rc.scl.push_back({ch->mScalingKeys[k].mTime / tps,
                            {ch->mScalingKeys[k].mValue.x,
                             ch->mScalingKeys[k].mValue.y,
                             ch->mScalingKeys[k].mValue.z}});
                    raw.channels.push_back(std::move(rc));
                }
                fcache->clips.push_back(std::move(raw));
            }
            g_anim_file_cache[path] = fcache;
        }
    }

    // ── Pick longest clip when name_override is set (skip Mixamo accessories) ─
    int target_ai = -1;
    if (name_override && name_override[0] != '\0') {
        double longest = -1.0;
        for (int ai = 0; ai < (int)fcache->clips.size(); ++ai) {
            double dur = fcache->clips[ai].duration_sec;
            if (dur > longest) { longest = dur; target_ai = ai; }
        }
    }

    // ── Build AnimClip(s), filtering channels against this body's node_map_ ──
    int first = (int)clips_.size();
    for (int ai = 0; ai < (int)fcache->clips.size(); ++ai) {
        if (target_ai >= 0 && ai != target_ai) continue;

        const RawAnimClip& raw = fcache->clips[ai];
        AnimClip clip;
        clip.name         = (name_override && name_override[0] != '\0') ? name_override : raw.fbx_name;
        clip.duration_sec = raw.duration_sec;
        clip.fps          = raw.fps;

        for (const RawAnimChannel& rc : raw.channels) {
            // Skip channels absent from this body's skeleton (e.g. extra finger bones).
            if (node_map_.find(rc.name) == node_map_.end()) {
                if (VerboseAssetLogsEnabled())
                    std::fprintf(stderr,
                        "[anim-skip] clip='%s' channel='%s' (no matching bone in body)\n",
                        clip.name.c_str(), rc.name.c_str());
                continue;
            }
            BoneChannel bc;
            bc.name = rc.name;
            bc.pos  = rc.pos;
            bc.rot  = rc.rot;
            bc.scl  = rc.scl;
            clip.chan_map[bc.name] = (int)clip.channels.size();
            clip.channels.push_back(std::move(bc));
        }

        {
            auto hit = clip.chan_map.find("mixamorig:Hips");
            if (hit != clip.chan_map.end()) {
                const auto& hc = clip.channels[hit->second];
                if (!hc.pos.empty() && VerboseAssetLogsEnabled())
                    std::fprintf(stderr, "[clip-frame0] '%s' Hips_pos=(%.4f,%.4f,%.4f)\n",
                                 clip.name.c_str(), hc.pos[0].v.x, hc.pos[0].v.y, hc.pos[0].v.z);
            }
        }
        if (log_bones) {
            std::fprintf(stderr, "[model] Appended clip '%s' (%.2fs) from '%s'\n",
                         clip.name.c_str(), clip.duration_sec, path);
            std::fprintf(stderr, "[clip-channels] '%s' total=%d\n",
                         clip.name.c_str(), (int)clip.channels.size());
            for (int ci = 0; ci < (int)clip.channels.size(); ++ci)
                std::fprintf(stderr, "  ch[%d]  '%s'\n", ci, clip.channels[ci].name.c_str());
        }
        clips_.push_back(std::move(clip));
    }
    {
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        (void)dt;
    }
    return first;
}

// ---------------------------------------------------------------------------
// OverrideMaterial — standalone PNG/JPG loader + per-submesh overwrite.
//
// Kept separate from LoadTex (which handles embedded Assimp textures). Any
// texture this creates is owned by the submeshes and freed via FreeMesh().
// ---------------------------------------------------------------------------

static GLuint LoadFileTexture(const char* path, bool srgb) {
    return LoadTexCached(path ? std::string(path) : std::string(), srgb);
}

void Model::OverrideMaterial(const std::string& albedo_path,
                             const std::string& normal_path,
                             const std::string& orm_path,
                             float albedo_r, float albedo_g, float albedo_b,
                             float roughness, float metallic) {
    GLuint new_albedo = albedo_path.empty() ? 0 : LoadFileTexture(albedo_path.c_str(), true);
    GLuint new_normal = normal_path.empty() ? 0 : LoadFileTexture(normal_path.c_str(), false);
    GLuint new_orm    = orm_path.empty()    ? 0 : LoadFileTexture(orm_path.c_str(),    false);

    // When an external albedo texture is supplied, don't tint it with the
    // submesh's default skin-tone factor — use white unless the server
    // explicitly provides a non-default tint.
    const bool use_default_albedo_factor =
        albedo_r == 0.f && albedo_g == 0.f && albedo_b == 0.f;
    glm::vec3 tint = use_default_albedo_factor
                     ? (new_albedo ? glm::vec3(1.f) : glm::vec3(0.72f, 0.68f, 0.60f))
                     : glm::vec3(albedo_r, albedo_g, albedo_b);

    // Textures come from the global cache — lifetime managed by g_tex_cache,
    // not by this model. Don't push into owned_textures_ or Destroy() would
    // delete handles shared by every other model using the same texture.

    for (auto& m : meshes_) {
        if (new_albedo) m.tex_albedo = new_albedo;
        if (new_normal) m.tex_normal = new_normal;
        if (new_orm) {
            m.tex_orm = new_orm;
            m.orm_packed = true;
        }
        m.albedo_factor    = tint;
        m.roughness_factor = roughness;
        m.metallic_factor  = metallic;
    }
}

// ---------------------------------------------------------------------------
// MaterialNames — distinct aiMaterial names across all submeshes.
// ---------------------------------------------------------------------------
std::vector<std::string> Model::MaterialNames() const {
    std::vector<std::string> out;
    for (const auto& m : meshes_) {
        if (m.material_name.empty()) continue;
        if (std::find(out.begin(), out.end(), m.material_name) == out.end())
            out.push_back(m.material_name);
    }
    return out;
}

// ---------------------------------------------------------------------------
// ApplyMaterialsByName — load external PBR textures from disk, register them
// in the MaterialManager, and assign per-submesh material indices by name.
// ---------------------------------------------------------------------------

static GLuint LoadTexFromDisk(const std::string& path, bool srgb) {
    return LoadTexCached(path, srgb);
}

void Model::ApplyMaterialsByName(
    MaterialManager& mm,
    const std::unordered_map<std::string, MaterialPaths>& by_name) {

    // Cache (per call) so two submeshes using the same material name don't
    // re-load the PNGs twice.
    struct Loaded {
        GLuint albedo = 0;
        GLuint normal = 0;
        GLuint orm    = 0;
        int    idx    = -1;
    };
    std::unordered_map<std::string, Loaded> cache;

    auto loadFor = [&](const std::string& name) -> Loaded {
        auto cit = cache.find(name);
        if (cit != cache.end()) return cit->second;

        auto pit = by_name.find(name);
        if (pit == by_name.end()) { cache[name] = {}; return {}; }
        const MaterialPaths& p = pit->second;

        Loaded l;
        l.albedo = LoadTexFromDisk(p.albedo, /*srgb*/true);
        l.normal = LoadTexFromDisk(p.normal, /*srgb*/false);
        l.orm    = LoadTexFromDisk(p.orm,    /*srgb*/false);

        // Textures come from g_tex_cache — not owned by this model.
        l.idx = mm.RegisterFromHandles(name, l.albedo, l.normal, l.orm,
                                       0, 0, true, glm::vec3(1.0f), 1.0f, 0.0f, 1.0f);
        cache[name] = l;
        return l;
    };

    int applied = 0;

    for (auto& sm : meshes_) {
        if (sm.material_name.empty()) continue;
        Loaded l = loadFor(sm.material_name);
        if (l.idx < 0) continue;

        // Write to BOTH representations so every render path sees the same
        // textures:
        //   material_idx   → deferred Lit pipeline (bindless SSBO lookup)
        //   tex_albedo/... → Simple forward shader (direct sampler2D bind)
        // Without this, FBX models that ship with no embedded textures come
        // through as tex_albedo=0 and the Simple shader falls back to the
        // flat tint colour.
        sm.material_idx = l.idx;
        if (l.albedo) sm.tex_albedo = l.albedo;
        if (l.normal) sm.tex_normal = l.normal;
        if (l.orm) {
            sm.tex_orm = l.orm;
            sm.orm_packed = true;
        }
        ++applied;
    }
    if (log_bones)
        std::fprintf(stderr,
            "[model] ApplyMaterialsByName: %d submesh(es) matched to %zu material(s)\n",
            applied, cache.size());
}

} // namespace rco::renderer
