#include "rco/renderer/model.h"
#include "rco/renderer/material.h"

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

namespace rco::renderer {

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
    skinned_ = false;
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
        std::string full = directory_ + "/" + path;
        int ch;
        px = stbi_load(full.c_str(), &w, &h, &ch, 4);
        if (!px) px = stbi_load(path.c_str(), &w, &h, &ch, 4);
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
    glBindVertexArray(0);
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
                glm::vec3 pos = InterpV(ch.pos, t);
                glm::quat rot = InterpQ(ch.rot, t);
                glm::vec3 scl = ch.scl.empty() ? glm::vec3(1.f) : InterpV(ch.scl, t);
                local = glm::translate(glm::mat4(1.f), pos)
                      * glm::mat4_cast(rot)
                      * glm::scale(glm::mat4(1.f), scl);
            }
        }

        global[ni] = (node.parent < 0) ? local : global[node.parent] * local;

        if (node.bone_idx >= 0 && node.bone_idx < max_out) {
            out_mats[node.bone_idx] = global_inv_ * global[ni] * offsetFor(node.bone_idx);
        }
    }
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

    for (unsigned i = 0; i < mesh->mNumVertices; ++i) {
        glm::vec3 pos = xformPos(mesh->mVertices[i]);
        verts.push_back(pos.x); verts.push_back(pos.y); verts.push_back(pos.z);
        if (mesh->HasNormals()) {
            glm::vec3 n = xformDir(mesh->mNormals[i]);
            verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
        } else { verts.push_back(0.f); verts.push_back(1.f); verts.push_back(0.f); }
        if (mesh->mTextureCoords[0]) {
            verts.push_back(mesh->mTextureCoords[0][i].x);
            verts.push_back(mesh->mTextureCoords[0][i].y);
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
        if (mat->GetTexture(AI_MATKEY_BASE_COLOR_TEXTURE, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
            sm.tex_albedo = LoadTex(scene, texPath.C_Str(), true);
        if (mat->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_HEIGHT,  0, &texPath) == AI_SUCCESS)
            sm.tex_normal = LoadTex(scene, texPath.C_Str(), false);
        if (mat->GetTexture(AI_MATKEY_METALLIC_TEXTURE, &texPath) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_METALNESS, 0, &texPath) == AI_SUCCESS)
            sm.tex_orm = LoadTex(scene, texPath.C_Str(), false);
        aiColor4D c;
        if (mat->Get(AI_MATKEY_BASE_COLOR, c) == AI_SUCCESS ||
            mat->Get(AI_MATKEY_COLOR_DIFFUSE, c) == AI_SUCCESS)
            sm.albedo_factor = {c.r, c.g, c.b};
        float rough=0.5f, metal=0.f;
        mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, rough);
        mat->Get(AI_MATKEY_METALLIC_FACTOR,  metal);
        sm.roughness_factor = rough;
        sm.metallic_factor  = metal;
    }

    // --- Build VAO ---
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

    glBindVertexArray(0);

    // Record per-bone offsets for this submesh. They will be padded with
    // identity for unused bone slots after all meshes have been loaded,
    // once the final bones_.size() is known (see Load()).
    sm.bone_offsets.reserve(sm_offsets.size());
    for (auto& [bidx, off] : sm_offsets) {
        if ((int)sm.bone_offsets.size() <= bidx)
            sm.bone_offsets.resize(bidx + 1, glm::mat4(1.f));
        sm.bone_offsets[bidx] = off;
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
bool Model::Load(const char* path, MaterialManager* mm) {
    EnsureDefaults();

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

    // Build the node hierarchy first (needed for bone→node linking in ProcessMesh).
    BuildNodeTree(scene->mRootNode, -1);
    global_inv_ = glm::inverse(ToGLM(scene->mRootNode->mTransformation));

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

    // Load animations after bones are known.
    if (scene->mNumAnimations > 0) {
        LoadAnimations(scene);
        skinned_ = !bones_.empty();
        if (skinned_)
            std::fprintf(stderr, "[model] '%s' — %d bones, %d clips\n",
                         path, (int)bones_.size(), (int)clips_.size());
    }

    // Pad each submesh's bone_offsets to the final bones_.size() with
    // identity matrices for bones this particular submesh doesn't reference.
    // Keeps ComputeBones' indexing simple: bone_offsets[bidx] is always safe.
    for (auto& sm : meshes_) {
        if (!sm.skinned) continue;
        if (sm.bone_offsets.size() < bones_.size())
            sm.bone_offsets.resize(bones_.size(), glm::mat4(1.f));
    }

    // Diagnostic dump — per-submesh info so we can see what landed where when
    // a multi-part model (armour etc.) shows parts in wrong positions.
    std::fprintf(stderr, "[model] '%s' — %d submeshes\n", path, (int)meshes_.size());
    for (size_t i = 0; i < meshes_.size(); ++i) {
        const auto& m = meshes_[i];
        std::fprintf(stderr,
            "  [%zu] verts=%d skinned=%d mat_idx=%d tex(a/n/o)=%u/%u/%u\n",
            i, m.idx_count / 3 /*tris*/,
            m.skinned ? 1 : 0, m.material_idx,
            m.tex_albedo, m.tex_normal, m.tex_orm);
    }
    // Node → bone map so we can see which nodes the skeleton lives under.
    std::fprintf(stderr, "[model] bone-bearing nodes (%d):\n", (int)bones_.size());
    for (const auto& an : anim_nodes_) {
        if (an.bone_idx < 0) continue;
        std::fprintf(stderr, "   bone%d  node='%s'  parent=%d\n",
                     an.bone_idx, an.name.c_str(), an.parent);
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
                key, m.tex_albedo, m.tex_normal, m.tex_orm);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// AppendAnimationsFrom — load clips from a separate file
// ---------------------------------------------------------------------------
int Model::AppendAnimationsFrom(const char* path, const char* name_override) {
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_LimitBoneWeights);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) ||
        scene->mNumAnimations == 0) {
        std::fprintf(stderr, "[model] AppendAnimationsFrom '%s': %s\n",
                     path, importer.GetErrorString());
        return -1;
    }

    int first = (int)clips_.size();
    for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai) {
        const aiAnimation* src = scene->mAnimations[ai];
        AnimClip clip;

        // Name: override > file's own name
        if (name_override && name_override[0] != '\0')
            clip.name = (scene->mNumAnimations == 1)
                        ? name_override
                        : std::string(name_override) + "_" + std::to_string(ai);
        else
            clip.name = src->mName.C_Str();

        double tps = src->mTicksPerSecond > 0.0 ? src->mTicksPerSecond : 25.0;
        clip.duration_sec = float(src->mDuration / tps);

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
        std::fprintf(stderr, "[model] Appended clip '%s' (%.2fs) from '%s'\n",
                     clip.name.c_str(), clip.duration_sec, path);
        clips_.push_back(std::move(clip));
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
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* px = stbi_load(path, &w, &h, &ch, 4);
    if (!px) {
        std::fprintf(stderr, "[model] OverrideMaterial: cannot load '%s': %s\n",
                     path, stbi_failure_reason());
        return 0;
    }
    GLuint tex;
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
    return tex;
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

    // The new textures are SHARED across all submeshes of this model (one
    // copy, reassigned to each SubMesh.tex_*). Track them in owned_textures_
    // so Destroy() frees them exactly once. Previously this method leaked
    // every prior OverrideMaterial call's textures.
    if (new_albedo) owned_textures_.push_back(new_albedo);
    if (new_normal) owned_textures_.push_back(new_normal);
    if (new_orm)    owned_textures_.push_back(new_orm);

    for (auto& m : meshes_) {
        if (new_albedo) m.tex_albedo = new_albedo;
        if (new_normal) m.tex_normal = new_normal;
        if (new_orm)    m.tex_orm    = new_orm;
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

// Load a PNG/JPG/etc into a GL texture with the requested colour space.
// Returns 0 on failure (missing file, bad decode, empty path).
static GLuint LoadTexFromDisk(const std::string& path, bool srgb) {
    if (path.empty()) return 0;
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) {
        std::fprintf(stderr, "[model] ApplyMaterials: can't read '%s'\n", path.c_str());
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
    return tex;
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

        // Track for Destroy()
        if (l.albedo) owned_textures_.push_back(l.albedo);
        if (l.normal) owned_textures_.push_back(l.normal);
        if (l.orm)    owned_textures_.push_back(l.orm);

        l.idx = mm.RegisterFromHandles(name, l.albedo, l.normal, l.orm);
        cache[name] = l;
        return l;
    };

    // Diagnostic: show what the caller provided vs what the submeshes want.
    std::fprintf(stderr, "[model] ApplyMaterialsByName: caller provided %zu material(s):",
                 by_name.size());
    for (const auto& [n, _] : by_name) std::fprintf(stderr, " '%s'", n.c_str());
    std::fprintf(stderr, "\n[model] submesh aiMaterial names:\n");

    int applied = 0;
    std::unordered_map<std::string, int> name_counts;
    for (auto& sm : meshes_) {
        ++name_counts[sm.material_name];
    }
    for (const auto& [n, c] : name_counts) {
        std::fprintf(stderr, "   '%s' × %d\n", n.c_str(), c);
    }

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
        if (l.orm)    sm.tex_orm    = l.orm;
        ++applied;
    }
    std::fprintf(stderr,
        "[model] ApplyMaterialsByName: %d submesh(es) matched to %zu material(s)\n",
        applied, cache.size());
}

} // namespace rco::renderer
