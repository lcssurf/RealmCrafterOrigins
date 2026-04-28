#include "editable_terrain.h"

#include "rco/renderer/pipeline.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace gue {

// ─── Shader ──────────────────────────────────────────────────────────────────
//
// Simple splatmap-blended terrain shader for the editor viewport.
// Samples up to 4 albedo textures + the splatmap and mixes them by weight.

static const char* kTerrainVS = R"glsl(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(location = 0) uniform mat4 u_VP;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

void main() {
    vWorldPos = aPos;
    vNormal   = aNormal;
    vUV       = aUV;
    gl_Position = u_VP * vec4(aPos, 1.0);
}
)glsl";

static const char* kTerrainFS = R"glsl(
#version 460 core
layout(location = 0) in  vec3 vWorldPos;
layout(location = 1) in  vec3 vNormal;
layout(location = 2) in  vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(location = 1) uniform vec3  u_SunDir;      // world-space, normalised
layout(location = 2) uniform float u_Tiling;      // UV divisor
layout(location = 3) uniform vec4  u_TerrainSize; // xy=size, zw unused
layout(location = 4) uniform int   u_MatCount;    // 1..4

layout(binding = 0) uniform sampler2D u_Splat;
layout(binding = 1) uniform sampler2D u_Mat0;
layout(binding = 2) uniform sampler2D u_Mat1;
layout(binding = 3) uniform sampler2D u_Mat2;
layout(binding = 4) uniform sampler2D u_Mat3;

void main() {
    // Splatmap: sample by normalised world XZ
    vec2 splatUV = vWorldPos.xz / u_TerrainSize.xy;
    vec4 w       = texture(u_Splat, splatUV);

    // Renormalise weights so they always sum to 1
    float sum = max(w.r + w.g + w.b + w.a, 1e-5);
    w /= sum;

    // Tiled material UVs
    vec2 tuv = vUV / u_Tiling;

    vec3 col = texture(u_Mat0, tuv).rgb * w.r;
    if (u_MatCount >= 2) col += texture(u_Mat1, tuv).rgb * w.g;
    if (u_MatCount >= 3) col += texture(u_Mat2, tuv).rgb * w.b;
    if (u_MatCount >= 4) col += texture(u_Mat3, tuv).rgb * w.a;

    // Simple diffuse lighting
    float NdotL   = clamp(dot(normalize(vNormal), u_SunDir), 0.0, 1.0);
    float ambient = 0.35;
    col *= ambient + (1.0 - ambient) * NdotL;

    fragColor = vec4(col, 1.0);
}
)glsl";

static GLuint CompileOne(GLenum t, const char* src) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        std::fprintf(stderr, "[EditableTerrain] shader compile: %s\n", log);
    }
    return s;
}

void EditableTerrain::InitShader() {
    if (prog_) return;
    GLuint vs = CompileOne(GL_VERTEX_SHADER,   kTerrainVS);
    GLuint fs = CompileOne(GL_FRAGMENT_SHADER, kTerrainFS);
    prog_ = glCreateProgram();
    glAttachShader(prog_, vs); glAttachShader(prog_, fs);
    glLinkProgram(prog_);
    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog_, 512, nullptr, log);
        std::fprintf(stderr, "[EditableTerrain] shader link: %s\n", log);
    }
    glDeleteShader(vs); glDeleteShader(fs);
}

// ─── Default placeholder textures ────────────────────────────────────────────

void EditableTerrain::EnsureDefaultTextures() {
    if (!defaultNormal_)    defaultNormal_    = MakeSolidTex(128, 128, 255);
    if (!defaultRoughness_) defaultRoughness_ = MakeSolidTex(180, 180, 180);
    if (!defaultAO_)        defaultAO_        = MakeSolidTex(255, 255, 255); // full AO = no occlusion
    if (!defaultHeight_)    defaultHeight_    = MakeSolidTex(255, 255, 255); // flat → splatmap drives blend
}

// ─── Chunked mesh ────────────────────────────────────────────────────────────

void EditableTerrain::InitChunks() {
    DestroyChunks();

    cxCount_ = (heightmap_.W + kCells - 1) / kCells;
    czCount_ = (heightmap_.H + kCells - 1) / kCells;

    // Shared index buffer — same topology per chunk
    std::vector<uint32_t> idx;
    idx.reserve(kCells * kCells * 6);
    for (int z = 0; z < kCells; z++) {
        for (int x = 0; x < kCells; x++) {
            uint32_t tl = z * kVerts + x;
            uint32_t tr = tl + 1;
            uint32_t bl = (z + 1) * kVerts + x;
            uint32_t br = bl + 1;
            idx.push_back(tl); idx.push_back(bl); idx.push_back(tr);
            idx.push_back(tr); idx.push_back(bl); idx.push_back(br);
        }
    }
    idxCount_ = (int)idx.size();

    glCreateBuffers(1, &ebo_);
    glNamedBufferData(ebo_, (GLsizeiptr)(idx.size() * 4), idx.data(), GL_STATIC_DRAW);

    chunks_.resize(cxCount_ * czCount_);
    // Layout matches terrainGBuffer.vs — so the same VAO works with both the
    // GUE's simple shader and the client's pipeline: pos3 + n3 + uv2 + tan3.
    static constexpr int kFloats = 11;
    static constexpr int kStride = kFloats * sizeof(float);

    for (int z = 0; z < czCount_; z++) {
        for (int x = 0; x < cxCount_; x++) {
            Chunk& c = chunks_[z * cxCount_ + x];
            c.cx = x; c.cz = z; c.dirty = true;

            glCreateVertexArrays(1, &c.vao);
            glCreateBuffers(1, &c.vbo);
            glNamedBufferData(c.vbo,
                (GLsizeiptr)(kVerts * kVerts * kFloats * sizeof(float)),
                nullptr, GL_DYNAMIC_DRAW);

            glVertexArrayVertexBuffer(c.vao, 0, c.vbo, 0, kStride);
            glVertexArrayElementBuffer(c.vao, ebo_);
            glEnableVertexArrayAttrib(c.vao, 0);
            glVertexArrayAttribFormat(c.vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
            glVertexArrayAttribBinding(c.vao, 0, 0);
            glEnableVertexArrayAttrib(c.vao, 1);
            glVertexArrayAttribFormat(c.vao, 1, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
            glVertexArrayAttribBinding(c.vao, 1, 0);
            glEnableVertexArrayAttrib(c.vao, 2);
            glVertexArrayAttribFormat(c.vao, 2, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float));
            glVertexArrayAttribBinding(c.vao, 2, 0);
            glEnableVertexArrayAttrib(c.vao, 3);
            glVertexArrayAttribFormat(c.vao, 3, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float));
            glVertexArrayAttribBinding(c.vao, 3, 0);
        }
    }
}

void EditableTerrain::BuildChunk(Chunk& c) {
    const int   baseX = c.cx * kCells;
    const int   baseZ = c.cz * kCells;
    const float cs    = heightmap_.cell_size;

    std::vector<float> verts;
    verts.reserve(kVerts * kVerts * 11);

    for (int z = 0; z < kVerts; z++) {
        for (int x = 0; x < kVerts; x++) {
            int   gx = baseX + x;
            int   gz = baseZ + z;
            float h  = heightmap_.Get(gx, gz);

            float hl = heightmap_.Get(gx - 1, gz);
            float hr = heightmap_.Get(gx + 1, gz);
            float hd = heightmap_.Get(gx, gz - 1);
            float hu = heightmap_.Get(gx, gz + 1);
            glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.f * cs, hd - hu));

            float u = gx * cs;
            float v = gz * cs;

            // pos
            verts.push_back(gx * cs); verts.push_back(h);   verts.push_back(gz * cs);
            // normal
            verts.push_back(n.x);     verts.push_back(n.y); verts.push_back(n.z);
            // uv (world-scaled — shader divides by u_Tiling)
            verts.push_back(u);       verts.push_back(v);
            // tangent (flat — triplanar shader ignores it anyway)
            verts.push_back(1.f);     verts.push_back(0.f); verts.push_back(0.f);
        }
    }

    glNamedBufferSubData(c.vbo, 0,
        (GLsizeiptr)(verts.size() * sizeof(float)), verts.data());
    c.dirty = false;
}

void EditableTerrain::DestroyChunks() {
    for (Chunk& c : chunks_) {
        if (c.vao) glDeleteVertexArrays(1, &c.vao);
        if (c.vbo) glDeleteBuffers(1, &c.vbo);
    }
    chunks_.clear();
    if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
    cxCount_ = czCount_ = 0;
    idxCount_ = 0;
}

void EditableTerrain::DestroyMaterials() {
    // Shared defaults are owned by EditableTerrain, not by individual Material
    // entries. Null them out before Unload so glDeleteTextures doesn't fire
    // on the shared objects, leaving zombie IDs for subsequent draws.
    for (Material& m : materials_) {
        if (m.normal    == defaultNormal_)    m.normal    = 0;
        if (m.roughness == defaultRoughness_) m.roughness = 0;
        if (m.ao        == defaultAO_)        m.ao        = 0;
        if (m.height    == defaultHeight_)    m.height    = 0;
        m.Unload();
    }
    materials_.clear();
}

void EditableTerrain::MarkDirtyAll() {
    for (Chunk& c : chunks_) c.dirty = true;
}

void EditableTerrain::MarkDirtyRegion(float wx, float wz, float radius) {
    float chunkWorld = kCells * heightmap_.cell_size;
    int x0 = std::max(0,           (int)std::floor((wx - radius) / chunkWorld));
    int z0 = std::max(0,           (int)std::floor((wz - radius) / chunkWorld));
    int x1 = std::min(cxCount_-1,  (int)std::floor((wx + radius) / chunkWorld));
    int z1 = std::min(czCount_-1,  (int)std::floor((wz + radius) / chunkWorld));
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++)
            chunks_[z * cxCount_ + x].dirty = true;
}

// ─── Material scanning ───────────────────────────────────────────────────────

void EditableTerrain::ReloadMaterials() {
    EnsureDefaultTextures();
    DestroyMaterials();

    // Materials live under dist/client/data/terrain/materials/<name>/.
    // Scan per configured name; fall back to a 1×1 placeholder if missing.
    for (const std::string& name : materialNames_) {
        if ((int)materials_.size() >= kMaxMats) break;

        std::string dir = "../client/data/terrain/materials/" + name;
        auto scanned = ScanMaterials(dir, defaultNormal_, defaultRoughness_,
                                     defaultAO_, defaultHeight_);
        if (!scanned.empty()) {
            Material m = scanned.front();
            // Drop every extra entry ScanMaterials may have produced
            // (subfolder + root-level flat textures). Null out shared defaults
            // before Unload so we don't glDeleteTextures the shared objects.
            for (size_t i = 1; i < scanned.size(); ++i) {
                Material& s = scanned[i];
                if (s.normal    == defaultNormal_)    s.normal    = 0;
                if (s.roughness == defaultRoughness_) s.roughness = 0;
                if (s.ao        == defaultAO_)        s.ao        = 0;
                if (s.height    == defaultHeight_)    s.height    = 0;
                s.Unload();
            }
            m.name = name;
            materials_.push_back(std::move(m));
        } else {
            Material m;
            m.name      = name;
            m.albedo    = MakeSolidTex(130, 130, 130);
            m.normal    = defaultNormal_;
            m.roughness = defaultRoughness_;
            m.ao        = defaultAO_;
            m.height    = defaultHeight_;
            materials_.push_back(std::move(m));
        }
    }
    if (materials_.empty()) {
        EnsureDefaultTextures();
        Material m;
        m.name      = "default";
        m.albedo    = MakeSolidTex(110, 130, 90);
        m.normal    = defaultNormal_;
        m.roughness = defaultRoughness_;
        m.ao        = defaultAO_;
        m.height    = defaultHeight_;
        materials_.push_back(std::move(m));
    }
}

void EditableTerrain::SetMaterialNames(const std::vector<std::string>& names) {
    materialNames_.clear();
    for (const auto& n : names) {
        if ((int)materialNames_.size() >= kMaxMats) break;
        materialNames_.push_back(n);
    }
    ReloadMaterials();
}

// ─── Area I/O ────────────────────────────────────────────────────────────────

bool EditableTerrain::LoadArea(const std::string& areaName) {
    areaName_ = areaName;
    std::string base = "../client/data/areas/" + areaName + "/";
    std::string hmPath = base + "heightmap.bin";
    std::string spPath = base + "splatmap.bin";
    std::string mtPath = base + "materials.txt";

    // Heightmap — fall back to 512×512 flat if missing
    if (!heightmap_.Load(hmPath)) {
        heightmap_.Resize(512, 512, kDefaultCS);
    }

    // Splatmap — match heightmap dimensions if missing
    if (!splatmap_.Load(spPath)) {
        splatmap_.Resize(heightmap_.W, heightmap_.H);
    }
    // Mismatch between heightmap and splatmap sizes is possible when opening
    // an area edited with different dimensions — reset splatmap to match.
    if (splatmap_.W != heightmap_.W || splatmap_.H != heightmap_.H) {
        splatmap_.Resize(heightmap_.W, heightmap_.H);
    }

    // Materials list — each line "name tiling". If missing, use a reasonable
    // default set of three materials (red/green/blue channels of splatmap).
    materialNames_.clear();
    float tilingSum = 0.f; int tilingCount = 0;
    std::ifstream mt(mtPath);
    if (mt) {
        std::string line;
        while (std::getline(mt, line)) {
            std::istringstream is(line);
            std::string name; float t = 4.f;
            is >> name >> t;
            if (!name.empty() && (int)materialNames_.size() < kMaxMats) {
                materialNames_.push_back(name);
                tilingSum += t; ++tilingCount;
            }
        }
    }
    if (materialNames_.empty()) {
        materialNames_ = {"grass", "dirt", "rock"};
    }
    tiling = (tilingCount > 0) ? (tilingSum / tilingCount) : 8.f;

    InitShader();
    InitChunks();
    MarkDirtyAll();
    ReloadMaterials();
    return true;
}

bool EditableTerrain::SaveArea() const {
    if (areaName_.empty() || heightmap_.W == 0) return false;
    std::string base = "../client/data/areas/" + areaName_ + "/";

    std::error_code ec;
    std::filesystem::create_directories(base, ec);

    bool ok = true;
    ok &= heightmap_.Save(base + "heightmap.bin");
    ok &= splatmap_.Save(base + "splatmap.bin");

    std::ofstream mt(base + "materials.txt");
    if (mt) {
        for (const std::string& name : materialNames_)
            mt << name << ' ' << tiling << '\n';
    } else {
        ok = false;
    }
    return ok;
}

// ─── Update / Render ────────────────────────────────────────────────────────

void EditableTerrain::Update() {
    for (Chunk& c : chunks_)
        if (c.dirty) BuildChunk(c);
    splatmap_.Upload();
}

void EditableTerrain::RenderFrame(const glm::mat4& vp, const glm::vec3& sunDir) {
    if (chunks_.empty() || !prog_) return;
    Update();

    glUseProgram(prog_);
    glUniformMatrix4fv(0, 1, GL_FALSE, glm::value_ptr(vp));
    glUniform3fv      (1, 1, glm::value_ptr(sunDir));
    glUniform1f       (2, tiling);
    glUniform4f       (3, WorldW(), WorldH(), 0.f, 0.f);

    int matCount = std::min((int)materials_.size(), kMaxMats);
    glUniform1i(4, matCount);

    // Bind splatmap + material textures to fixed slots
    glBindTextureUnit(0, splatmap_.tex);
    for (int i = 0; i < kMaxMats; ++i) {
        GLuint t = (i < matCount) ? materials_[i].albedo : 0;
        glBindTextureUnit(1 + i, t);
    }

    for (const Chunk& c : chunks_) {
        glBindVertexArray(c.vao);
        glDrawElements(GL_TRIANGLES, idxCount_, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

// ─── Raycast (terrain-editor algorithm) ──────────────────────────────────────

bool EditableTerrain::Raycast(const glm::vec3& origin, const glm::vec3& dir,
                              glm::vec3& hit) const {
    if (heightmap_.W == 0) return false;
    float step = 1.5f, prevT = 0.f;
    for (float t = 0.f; t < 4000.f; t += step) {
        glm::vec3 p = origin + dir * t;
        if (p.x < 0 || p.z < 0 || p.x >= WorldW() || p.z >= WorldH()) {
            prevT = t; step = std::min(step * 1.05f, 20.f);
            continue;
        }
        if (p.y <= heightmap_.SampleWorld(p.x, p.z)) {
            float lo = prevT, hi = t;
            for (int i = 0; i < 20; i++) {
                float mid = (lo + hi) * 0.5f;
                glm::vec3 mp = origin + dir * mid;
                if (mp.y <= heightmap_.SampleWorld(mp.x, mp.z)) hi = mid; else lo = mid;
            }
            glm::vec3 hp = origin + dir * ((lo + hi) * 0.5f);
            hit = {hp.x, heightmap_.SampleWorld(hp.x, hp.z), hp.z};
            return true;
        }
        prevT = t; step = std::min(step * 1.02f, 20.f);
    }
    return false;
}

// ─── Brushes ─────────────────────────────────────────────────────────────────

void EditableTerrain::ApplyBrush(float wx, float wz, float radius, float strength,
                                 float dt, BrushMode mode, float flattenH) {
    ::ApplyBrush(heightmap_, wx, wz, radius, strength, dt, mode, flattenH);
    MarkDirtyRegion(wx, wz, radius);
}

void EditableTerrain::Paint(float wx, float wz, float radius, float strength,
                            float dt, int matIdx) {
    if (matIdx < 0 || matIdx >= kMaxMats) return;
    ::PaintSplatmap(splatmap_, wx, wz, radius, strength, dt, matIdx,
                    WorldW(), WorldH());
}

// ─── Pipeline submission (PBR render path) ──────────────────────────────────

void EditableTerrain::SubmitToPipeline(rco::renderer::Pipeline& pipeline) {
    if (chunks_.empty()) return;
    Update();   // rebuild dirty chunks + upload splatmap

    rco::renderer::TerrainChunkSubmission base{};
    base.splatmap       = splatmap_.tex;
    base.tiling         = tiling;
    base.terrain_origin = glm::vec2(0.f);
    base.terrain_size   = glm::vec2(WorldW(), WorldH());
    for (int i = 0; i < kMaxMats; ++i) {
        if (i < (int)materials_.size()) {
            base.mat_albedo[i]    = materials_[i].albedo;
            base.mat_normal[i]    = materials_[i].normal;
            base.mat_roughness[i] = materials_[i].roughness;
            base.mat_ao[i]        = materials_[i].ao;
            base.mat_height[i]    = materials_[i].height;
        }
    }

    for (const Chunk& c : chunks_) {
        rco::renderer::TerrainChunkSubmission s = base;
        s.vao         = c.vao;
        s.vbo         = c.vbo;
        s.ebo         = ebo_;
        s.index_count = idxCount_;
        s.model       = glm::mat4(1.f);
        pipeline.SubmitTerrainChunk(s);
    }
}

// ─── Destructor ──────────────────────────────────────────────────────────────

EditableTerrain::~EditableTerrain() {
    DestroyChunks();
    DestroyMaterials();
    splatmap_.Destroy();
    if (prog_) glDeleteProgram(prog_);
    if (defaultNormal_)    glDeleteTextures(1, &defaultNormal_);
    if (defaultRoughness_) glDeleteTextures(1, &defaultRoughness_);
    if (defaultAO_)        glDeleteTextures(1, &defaultAO_);
    if (defaultHeight_)    glDeleteTextures(1, &defaultHeight_);
}

} // namespace gue
