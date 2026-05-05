#include "editable_terrain.h"

#include "rco/renderer/pipeline.h"

#include <stb_image_write.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace gue {

// ─── Default placeholder textures ────────────────────────────────────────────

void EditableTerrain::EnsureDefaultTextures() {
    if (!defaultNormal_)    defaultNormal_    = MakeSolidTex(128, 128, 255);
    if (!defaultRoughness_) defaultRoughness_ = MakeSolidTex(180, 180, 180);
    if (!defaultAO_)        defaultAO_        = MakeSolidTex(255, 255, 255); // full AO = no occlusion
    if (!defaultHeight_)    defaultHeight_    = MakeSolidTex(255, 255, 255); // flat → splatmap drives blend
    if (!defaultMacro_)     defaultMacro_     = MakeSolidTex(128, 128, 128); // 0.5 gray = no overlay change
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
    // VBO layout: a_xz only (vec2 world XZ). Height + normals computed in VS
    // from the heightmap GPU texture — no normals/tangents/UVs in VBO.
    static constexpr int kFloats = 2;
    static constexpr int kStride = kFloats * sizeof(float);

    for (int z = 0; z < czCount_; z++) {
        for (int x = 0; x < cxCount_; x++) {
            Chunk& c = chunks_[z * cxCount_ + x];
            c.cx = x; c.cz = z; c.dirty = true;

            glCreateVertexArrays(1, &c.vao);
            glCreateBuffers(1, &c.vbo);
            glNamedBufferData(c.vbo,
                (GLsizeiptr)(kVerts * kVerts * kFloats * sizeof(float)),
                nullptr, GL_STATIC_DRAW);

            glVertexArrayVertexBuffer(c.vao, 0, c.vbo, 0, kStride);
            glVertexArrayElementBuffer(c.vao, ebo_);
            glEnableVertexArrayAttrib(c.vao, 0);
            glVertexArrayAttribFormat(c.vao, 0, 2, GL_FLOAT, GL_FALSE, 0);
            glVertexArrayAttribBinding(c.vao, 0, 0);
        }
    }
}

void EditableTerrain::BuildChunk(Chunk& c) {
    const int   baseX = c.cx * kCells;
    const int   baseZ = c.cz * kCells;
    const float cs    = heightmap_.cell_size;

    std::vector<float> verts;
    verts.reserve(kVerts * kVerts * 2);

    for (int z = 0; z < kVerts; z++) {
        for (int x = 0; x < kVerts; x++) {
            verts.push_back((baseX + x) * cs);  // world X
            verts.push_back((baseZ + z) * cs);  // world Z
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
    float cs = heightmap_.cell_size;
    int x0 = std::max(0,              (int)std::floor((wx - radius) / cs) - 1);
    int z0 = std::max(0,              (int)std::floor((wz - radius) / cs) - 1);
    int x1 = std::min(heightmap_.W - 1, (int)std::ceil( (wx + radius) / cs) + 1);
    int z1 = std::min(heightmap_.H - 1, (int)std::ceil( (wz + radius) / cs) + 1);
    heightmap_.UploadRegion(x0, z0, x1, z1);
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
            // Distinct debug colors per slot so painting is visible even when
            // no real texture files exist. Matches splatmap channels R/G/B/A.
            static const uint8_t kSlotRGB[4][3] = {
                {110, 155,  70},   // 0 R — grassy green
                {148, 116,  78},   // 1 G — dirt brown
                {130, 135, 148},   // 2 B — rocky grey-blue
                {195, 180, 120},   // 3 A — sandy
            };
            int si = (int)materials_.size() % 4;
            Material m;
            m.name      = name;
            m.albedo    = MakeSolidTex(kSlotRGB[si][0], kSlotRGB[si][1], kSlotRGB[si][2]);
            m.normal    = defaultNormal_;
            m.roughness = defaultRoughness_;
            m.ao        = defaultAO_;
            m.height    = defaultHeight_;
            materials_.push_back(std::move(m));
        }
    }
    if (materials_.empty()) {
        EnsureDefaultTextures();
        static const uint8_t kSlotRGB[4][3] = {
            {110, 155,  70},
            {148, 116,  78},
            {130, 135, 148},
            {195, 180, 120},
        };
        for (int si = 0; si < 1; ++si) {   // seed at least one
            Material m;
            m.name      = "default";
            m.albedo    = MakeSolidTex(kSlotRGB[si][0], kSlotRGB[si][1], kSlotRGB[si][2]);
            m.normal    = defaultNormal_;
            m.roughness = defaultRoughness_;
            m.ao        = defaultAO_;
            m.height    = defaultHeight_;
            materials_.push_back(std::move(m));
        }
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

void EditableTerrain::SetMaterialSlot(int slot, const TerrainMatSpec& spec) {
    if (slot < 0 || slot >= kMaxMats) return;
    EnsureDefaultTextures();

    static const uint8_t kSlotRGB[4][3] = {
        {110, 155,  70}, {148, 116,  78}, {130, 135, 148}, {195, 180, 120}
    };

    // Grow the vector to cover this slot with placeholder colours
    while ((int)materials_.size() <= slot) {
        int si = (int)materials_.size() % 4;
        Material ph;
        ph.albedo    = MakeSolidTex(kSlotRGB[si][0], kSlotRGB[si][1], kSlotRGB[si][2]);
        ph.normal    = defaultNormal_;
        ph.roughness = defaultRoughness_;
        ph.ao        = defaultAO_;
        ph.height    = defaultHeight_;
        materials_.push_back(std::move(ph));
    }

    // Free old textures — null shared defaults first to avoid double-delete
    Material& m = materials_[slot];
    if (m.normal    == defaultNormal_)    m.normal    = 0;
    if (m.roughness == defaultRoughness_) m.roughness = 0;
    if (m.ao        == defaultAO_)        m.ao        = 0;
    if (m.height    == defaultHeight_)    m.height    = 0;
    m.Unload();

    matIds_[slot]              = spec.media_id;
    matAlbedoPaths_[slot]      = spec.albedo_path;
    matNormalPaths_[slot]      = spec.normal_path;
    matOrmPaths_[slot]         = spec.roughness_path;
    matNormalStrengths_[slot]  = spec.normal_strength;
    tilings[slot]              = spec.tiling;
    m.name                     = spec.name;

    // Paths relative to dist/ root; GUE lives in dist/tools/, so prefix ../client/
    auto tryLoad = [](const std::string& relPath, bool srgb) -> GLuint {
        if (relPath.empty()) return 0;
        return LoadTex("../client/" + relPath, srgb);
    };

    m.albedo = !spec.albedo_path.empty()
        ? tryLoad(spec.albedo_path, true)
        : MakeSolidTex(kSlotRGB[slot % 4][0], kSlotRGB[slot % 4][1], kSlotRGB[slot % 4][2]);

    m.normal    = spec.normal_path.empty()    ? defaultNormal_    : tryLoad(spec.normal_path,    false);
    m.roughness = spec.roughness_path.empty() ? defaultRoughness_ : tryLoad(spec.roughness_path, false);
    m.ao        = defaultAO_;
    m.height    = defaultHeight_;
}

void EditableTerrain::ClearMaterialSlot(int slot) {
    if (slot < 0 || slot >= kMaxMats) return;
    EnsureDefaultTextures();
    static const uint8_t kSlotRGB[4][3] = {
        {110, 155,  70}, {148, 116,  78}, {130, 135, 148}, {195, 180, 120}
    };
    if (slot < (int)materials_.size()) {
        Material& m = materials_[slot];
        if (m.normal    == defaultNormal_)    m.normal    = 0;
        if (m.roughness == defaultRoughness_) m.roughness = 0;
        if (m.ao        == defaultAO_)        m.ao        = 0;
        if (m.height    == defaultHeight_)    m.height    = 0;
        m.Unload();
        m.name      = "";
        m.albedo    = MakeSolidTex(kSlotRGB[slot % 4][0], kSlotRGB[slot % 4][1], kSlotRGB[slot % 4][2]);
        m.normal    = defaultNormal_;
        m.roughness = defaultRoughness_;
        m.ao        = defaultAO_;
        m.height    = defaultHeight_;
    }
    matIds_[slot] = 0;
    matAlbedoPaths_[slot].clear();
    matNormalPaths_[slot].clear();
    matOrmPaths_[slot].clear();
    matNormalStrengths_[slot] = 2.5f;
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

    // Materials list — new format: "id tiling" (int id); old format: "name tiling" (folder name).
    // Auto-detected per token: if the first non-whitespace char is a digit, treat as ID.
    materialNames_.clear();
    matIds_.fill(0);
    tilings.fill(8.f);
    matAlbedoPaths_.fill({});
    matNormalPaths_.fill({});
    matOrmPaths_.fill({});
    matNormalStrengths_.fill(2.5f);
    bool hasIds = false;
    std::ifstream mt(mtPath);
    if (mt) {
        std::string line;
        int lineIdx = 0;
        while (std::getline(mt, line) && lineIdx < kMaxMats) {
            std::istringstream is(line);
            std::string token; float t = 8.f;
            if (!(is >> token)) continue;
            is >> t;
            if (token.empty()) continue;
            bool looksInt = std::isdigit((unsigned char)token[0]) || token[0] == '-';
            if (looksInt) {
                try { matIds_[lineIdx] = std::stoi(token); hasIds = true; } catch (...) {}
                // Extended format: id tiling albedo normal orm normal_strength
                std::string alb, nrm, orm, ns;
                if ((is >> alb) && alb != "-") matAlbedoPaths_[lineIdx] = alb;
                if ((is >> nrm) && nrm != "-") matNormalPaths_[lineIdx]  = nrm;
                if ((is >> orm) && orm != "-") matOrmPaths_[lineIdx]      = orm;
                if (is >> ns) {
                    try { matNormalStrengths_[lineIdx] = std::stof(ns); } catch (...) {}
                }
            } else {
                materialNames_.push_back(token);
            }
            tilings[lineIdx] = t;
            ++lineIdx;
        }
    }
    if (!hasIds && materialNames_.empty())
        materialNames_ = {"grass", "dirt", "rock"};

    // Load macro variation if it exists
    {
        std::string macroPath = base + "macro.png";
        if (std::filesystem::exists(macroPath)) {
            int mw, mh, mch;
            stbi_set_flip_vertically_on_load(false);
            unsigned char* px = stbi_load(macroPath.c_str(), &mw, &mh, &mch, 1);
            if (px) {
                EnsureDefaultTextures();
                if (macroTex_) glDeleteTextures(1, &macroTex_);
                glGenTextures(1, &macroTex_);
                glBindTexture(GL_TEXTURE_2D, macroTex_);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, mw, mh, 0, GL_RED, GL_UNSIGNED_BYTE, px);
                glGenerateMipmap(GL_TEXTURE_2D);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                macroStrength = 0.3f;
                stbi_image_free(px);
            }
        }
    }

    heightmap_.InitGPU();
    InitChunks();
    MarkDirtyAll();

    if (!hasIds) {
        // Old format: resolve folder names to textures now
        ReloadMaterials();
    } else {
        // New format: seed fallback colours for each slot; ZonesTab::LoadZone
        // will call SetMaterialSlot() per slot once it resolves IDs from the DB.
        EnsureDefaultTextures();
        DestroyMaterials();
        static const uint8_t kSlotRGB[4][3] = {
            {110, 155,  70}, {148, 116,  78}, {130, 135, 148}, {195, 180, 120}
        };
        for (int si = 0; si < kMaxMats; ++si) {
            Material m;
            m.albedo    = MakeSolidTex(kSlotRGB[si][0], kSlotRGB[si][1], kSlotRGB[si][2]);
            m.normal    = defaultNormal_;
            m.roughness = defaultRoughness_;
            m.ao        = defaultAO_;
            m.height    = defaultHeight_;
            materials_.push_back(std::move(m));
        }
    }
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
        bool allZero = std::all_of(matIds_.begin(), matIds_.end(), [](int i){ return i == 0; });
        if (allZero && !materialNames_.empty()) {
            // Preserve old-format zones that have folder names but no DB ids yet
            for (int i = 0; i < (int)materialNames_.size(); ++i)
                mt << materialNames_[i] << ' ' << tilings[i] << '\n';
        } else {
            // Extended format: "id tiling albedo_path normal_path orm_path"
            // Paths are relative to dist/ root — client CWD is dist/client/ so they resolve directly.
            auto dash = [](const std::string& s) -> const std::string& {
                static const std::string d = "-";
                return s.empty() ? d : s;
            };
            for (int i = 0; i < kMaxMats; ++i) {
                mt << matIds_[i] << ' ' << tilings[i]
                   << ' ' << dash(matAlbedoPaths_[i])
                   << ' ' << dash(matNormalPaths_[i])
                   << ' ' << dash(matOrmPaths_[i])
                   << ' ' << matNormalStrengths_[i]
                   << '\n';
            }
        }
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
                                 float dt, BrushMode mode, float flattenH,
                                 BrushFalloff falloff) {
    ::ApplyBrush(heightmap_, wx, wz, radius, strength, dt, mode, flattenH, falloff);
    MarkDirtyRegion(wx, wz, radius);
}

void EditableTerrain::Paint(float wx, float wz, float radius, float strength,
                            float dt, int matIdx, BrushFalloff falloff) {
    if (matIdx < 0 || matIdx >= kMaxMats) return;
    ::PaintSplatmap(splatmap_, wx, wz, radius, strength, dt, matIdx,
                    WorldW(), WorldH(), falloff);
}

void EditableTerrain::AutoPaintBySlope(int flat, int rock, float mn, float mx) {
    if (flat < 0 || flat >= kMaxMats || rock < 0 || rock >= kMaxMats || flat == rock) return;
    float cs = heightmap_.cell_size;
    for (int z = 0; z < splatmap_.H; z++) {
        for (int x = 0; x < splatmap_.W; x++) {
            float dhdx = (heightmap_.Get(x+1,z) - heightmap_.Get(x-1,z)) / (2.f * cs);
            float dhdz = (heightmap_.Get(x,z+1) - heightmap_.Get(x,z-1)) / (2.f * cs);
            float slope = glm::degrees(std::atan(std::sqrt(dhdx*dhdx + dhdz*dhdz)));
            float t = glm::smoothstep(mn, mx, slope);
            splatmap_.SetWeight(x, z, flat, 1.f - t);
            splatmap_.SetWeight(x, z, rock, t);
            for (int i = 0; i < 4; i++)
                if (i != flat && i != rock) splatmap_.SetWeight(x, z, i, 0.f);
        }
    }
    splatmap_.dirty = true;
}

// ─── Pipeline submission (PBR render path) ──────────────────────────────────

void EditableTerrain::SubmitToPipeline(rco::renderer::Pipeline& pipeline,
                                        const glm::vec3& cam_pos) {
    if (chunks_.empty()) return;
    Update();   // rebuild dirty chunks + upload splatmap

    rco::renderer::TerrainChunkSubmission base{};
    base.splatmap        = splatmap_.tex;
    base.tilings          = glm::vec4(tilings[0], tilings[1], tilings[2], tilings[3]);
    base.macro_variation  = macroTex_ ? macroTex_ : defaultMacro_;
    base.macro_strength   = macroStrength;
    base.terrain_origin   = glm::vec2(0.f);
    base.terrain_size     = glm::vec2(WorldW(), WorldH());
    base.heightmap_tex    = heightmap_.tex;
    base.cell_size        = heightmap_.cell_size;
    for (int i = 0; i < kMaxMats; ++i) {
        base.mat_normal_strength[i] = matNormalStrengths_[i];
        if (i < (int)materials_.size()) {
            base.mat_albedo[i]    = materials_[i].albedo;
            base.mat_normal[i]    = materials_[i].normal;
            base.mat_roughness[i] = materials_[i].roughness;
            base.mat_ao[i]        = materials_[i].ao;
            base.mat_height[i]    = materials_[i].height;
        }
    }

    constexpr float kLodBase = 128.f;

    for (const Chunk& c : chunks_) {
        rco::renderer::TerrainChunkSubmission s = base;
        s.vao         = c.vao;
        s.vbo         = c.vbo;
        s.ebo         = ebo_;
        s.index_count = idxCount_;
        s.model       = glm::mat4(1.f);

        float cx   = (c.cx * kCells + kCells * 0.5f) * heightmap_.cell_size;
        float cz   = (c.cz * kCells + kCells * 0.5f) * heightmap_.cell_size;
        float dist = glm::length(glm::vec2(cx - cam_pos.x, cz - cam_pos.z));
        s.lod_level = glm::clamp(std::log2(std::max(dist / kLodBase, 1.f)), 0.f, 3.f);

        pipeline.SubmitTerrainChunk(s);
    }
}

// ─── Macro variation ─────────────────────────────────────────────────────────

void EditableTerrain::GenerateMacro(int seed) {
    EnsureDefaultTextures();
    constexpr int MW = 512, MH = 512;
    std::vector<uint8_t> pixels(MW * MH);

    // Multi-octave value noise, mostly low-frequency (large patches)
    auto hash = [](int x, int z, int s) -> float {
        unsigned v = (unsigned)(x * 1619 + z * 31337 + s * 6971);
        v = (v ^ (v >> 16)) * 0x45d9f3bu;
        v = (v ^ (v >> 16)) * 0x45d9f3bu;
        return (float)(v & 0xFFFFu) / 65535.f;
    };
    auto valueNoise = [&](float x, float z, float freq, int s) {
        x *= freq; z *= freq;
        int   ix = (int)std::floor(x), iz = (int)std::floor(z);
        float fx = x - ix, fz = z - iz;
        float ux = fx*fx*(3-2*fx), uz = fz*fz*(3-2*fz);
        return glm::mix(glm::mix(hash(ix,   iz,   s), hash(ix+1, iz,   s), ux),
                        glm::mix(hash(ix,   iz+1, s), hash(ix+1, iz+1, s), ux), uz);
    };

    for (int z = 0; z < MH; z++) {
        for (int x = 0; x < MW; x++) {
            float nx = (float)x / MW, nz = (float)z / MH;
            float v = valueNoise(nx, nz, 3.f,  seed + 0) * 0.50f
                    + valueNoise(nx, nz, 7.f,  seed + 1) * 0.30f
                    + valueNoise(nx, nz, 15.f, seed + 2) * 0.20f;
            // Remap: centre at 0.5, ±0.3 variation
            v = 0.5f + (v - 0.5f) * 0.6f;
            pixels[z * MW + x] = (uint8_t)std::clamp((int)(v * 255.f + 0.5f), 0, 255);
        }
    }

    if (!macroTex_) glGenTextures(1, &macroTex_);
    glBindTexture(GL_TEXTURE_2D, macroTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, MW, MH, 0, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    macroStrength = 0.3f;
}

bool EditableTerrain::SaveMacro() const {
    if (areaName_.empty() || !macroTex_) return false;

    std::string base = "../client/data/areas/" + areaName_ + "/";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);

    // Read back from GPU
    glBindTexture(GL_TEXTURE_2D, macroTex_);
    GLint mw = 0, mh = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &mw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &mh);
    if (mw <= 0 || mh <= 0) return false;

    std::vector<uint8_t> pixels(mw * mh);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, pixels.data());

    std::string outPath = base + "macro.png";
    // stbi_write_png expects RGB/RGBA; for single-channel pass comp=1
    return stbi_write_png(outPath.c_str(), mw, mh, 1, pixels.data(), mw) != 0;
}

// ─── Destructor ──────────────────────────────────────────────────────────────

EditableTerrain::~EditableTerrain() {
    DestroyChunks();
    DestroyMaterials();
    splatmap_.Destroy();
    heightmap_.DestroyGPU();
    if (defaultNormal_)    glDeleteTextures(1, &defaultNormal_);
    if (defaultRoughness_) glDeleteTextures(1, &defaultRoughness_);
    if (defaultAO_)        glDeleteTextures(1, &defaultAO_);
    if (defaultHeight_)    glDeleteTextures(1, &defaultHeight_);
    if (defaultMacro_)     glDeleteTextures(1, &defaultMacro_);
    if (macroTex_)         glDeleteTextures(1, &macroTex_);
}

} // namespace gue
