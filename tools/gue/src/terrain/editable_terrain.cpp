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

namespace {
// Debug colours cycled every 4 slots so painting stays visually distinct
// even past the original 4-material layout, and when no real texture files
// exist for a slot yet.
constexpr uint8_t kSlotRGB[4][3] = {
    {110, 155,  70},   // grassy green
    {148, 116,  78},   // dirt brown
    {130, 135, 148},   // rocky grey-blue
    {195, 180, 120},   // sandy
};
} // namespace

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
    matConfigured_.clear();
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

// ─── Material texture arrays (feed the shader's generalized N-material path) ─

void EditableTerrain::RebuildMaterialArrays() {
    int n = (int)materials_.size();
    if (n == 0) {
        matAlbedoArray_.Destroy();
        matNormalArray_.Destroy();
        matRoughnessArray_.Destroy();
        matAoArray_.Destroy();
        matHeightArray_.Destroy();
        return;
    }

    matAlbedoArray_.Resize(n, /*srgb=*/true);
    matNormalArray_.Resize(n, false);
    matRoughnessArray_.Resize(n, false);
    matAoArray_.Resize(n, false);
    matHeightArray_.Resize(n, false);

    // Defensive: every construction path for materials_[i] (EnsureSlotCapacity,
    // SetMaterialSlot, ReloadMaterials, LoadArea's placeholder seeding) already
    // assigns valid default*_ handles to normal/roughness/ao/height — none of
    // them should ever be 0 here. Clamp anyway so a future code path that
    // forgets to set one of these can never feed a 0 GL id into the texture
    // array upload. See docs/TECH_DEBT.md "Terrain multi-material authoring
    // (Phase 1)".
    for (int i = 0; i < n; ++i) {
        GLuint normal    = materials_[i].normal    ? materials_[i].normal    : defaultNormal_;
        GLuint roughness = materials_[i].roughness ? materials_[i].roughness : defaultRoughness_;
        GLuint ao        = materials_[i].ao        ? materials_[i].ao        : defaultAO_;
        GLuint height     = materials_[i].height    ? materials_[i].height    : defaultHeight_;
        matAlbedoArray_.SetLayerFromGLTexture(i, materials_[i].albedo);
        matNormalArray_.SetLayerFromGLTexture(i, normal);
        matRoughnessArray_.SetLayerFromGLTexture(i, roughness);
        matAoArray_.SetLayerFromGLTexture(i, ao);
        matHeightArray_.SetLayerFromGLTexture(i, height);
    }

    matAlbedoArray_.GenerateMipmaps();
    matNormalArray_.GenerateMipmaps();
    matRoughnessArray_.GenerateMipmaps();
    matAoArray_.GenerateMipmaps();
    matHeightArray_.GenerateMipmaps();
}

// ─── Material scanning ───────────────────────────────────────────────────────

void EditableTerrain::ReloadMaterials() {
    EnsureDefaultTextures();
    DestroyMaterials();

    // Materials live under dist/client/data/terrain/materials/<name>/.
    // Scan per configured name — no cap on how many; falls back to a
    // debug-coloured placeholder if a folder is missing.
    for (const std::string& name : materialNames_) {
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
            matConfigured_.push_back(true); // real texture files found
        } else {
            // Distinct debug colors per slot so painting is visible even when
            // no real texture files exist.
            int si = (int)materials_.size() % 4;
            Material m;
            m.name      = name;
            m.albedo    = MakeSolidTex(kSlotRGB[si][0], kSlotRGB[si][1], kSlotRGB[si][2]);
            m.normal    = defaultNormal_;
            m.roughness = defaultRoughness_;
            m.ao        = defaultAO_;
            m.height    = defaultHeight_;
            materials_.push_back(std::move(m));
            matConfigured_.push_back(false); // debug placeholder, not a real material
        }
    }
    if (materials_.empty()) {
        EnsureDefaultTextures();
        Material m;
        m.name      = "default";
        m.albedo    = MakeSolidTex(kSlotRGB[0][0], kSlotRGB[0][1], kSlotRGB[0][2]);
        m.normal    = defaultNormal_;
        m.roughness = defaultRoughness_;
        m.ao        = defaultAO_;
        m.height    = defaultHeight_;
        materials_.push_back(std::move(m));
        matConfigured_.push_back(false); // debug placeholder, not a real material
    }

    while ((int)tilings.size()             < (int)materials_.size()) tilings.push_back(8.f);
    while ((int)matIds_.size()             < (int)materials_.size()) matIds_.push_back(0);
    while ((int)matAlbedoPaths_.size()     < (int)materials_.size()) matAlbedoPaths_.push_back({});
    while ((int)matNormalPaths_.size()     < (int)materials_.size()) matNormalPaths_.push_back({});
    while ((int)matOrmPaths_.size()        < (int)materials_.size()) matOrmPaths_.push_back({});
    while ((int)matNormalStrengths_.size() < (int)materials_.size()) matNormalStrengths_.push_back(2.5f);
    while ((int)matConfigured_.size()      < (int)materials_.size()) matConfigured_.push_back(false);

    splatmap_.EnsureMaterialCount((int)materials_.size());
    RebuildMaterialArrays();
}

void EditableTerrain::SetMaterialNames(const std::vector<std::string>& names) {
    materialNames_ = names;
    ReloadMaterials();
}

void EditableTerrain::EnsureSlotCapacity(int slot) {
    while ((int)materials_.size() <= slot) {
        int si = (int)materials_.size() % 4;
        Material ph;
        ph.albedo    = MakeSolidTex(kSlotRGB[si][0], kSlotRGB[si][1], kSlotRGB[si][2]);
        ph.normal    = defaultNormal_;
        ph.roughness = defaultRoughness_;
        ph.ao        = defaultAO_;
        ph.height    = defaultHeight_;
        materials_.push_back(std::move(ph));
        matIds_.push_back(0);
        matAlbedoPaths_.push_back({});
        matNormalPaths_.push_back({});
        matOrmPaths_.push_back({});
        matNormalStrengths_.push_back(2.5f);
        tilings.push_back(8.f);
        matConfigured_.push_back(false); // fresh placeholder — not a real material yet
    }
    splatmap_.EnsureMaterialCount((int)materials_.size());
}

int EditableTerrain::AddMaterialSlot(const TerrainMatSpec& spec) {
    int slot = (int)materials_.size();
    SetMaterialSlot(slot, spec);
    return slot;
}

void EditableTerrain::SetMaterialSlot(int slot, const TerrainMatSpec& spec) {
    if (slot < 0) return;
    EnsureDefaultTextures();
    EnsureSlotCapacity(slot);

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

    // Single source of truth for "does this slot actually have a real
    // texture, or is it still the debug placeholder colour" — used for BOTH
    // m.albedo's choice AND matConfigured_ below. These used to be two
    // separate conditions (m.albedo checked albedo_path; matConfigured_
    // additionally accepted a nonzero media_id on its own) — a DB material
    // with a valid id but an unresolved/empty albedo_path made
    // matConfigured_ true (shader starts reading this slot) while m.albedo
    // was still the placeholder (e.g. the green kSlotRGB[0]), permanently
    // showing the placeholder colour once picked. See docs/TECH_DEBT.md
    // "Terrain multi-material authoring (Phase 1)".
    bool hasRealAlbedo = !spec.albedo_path.empty();

    m.albedo = hasRealAlbedo
        ? tryLoad(spec.albedo_path, true)
        : MakeSolidTex(kSlotRGB[slot % 4][0], kSlotRGB[slot % 4][1], kSlotRGB[slot % 4][2]);

    m.normal    = spec.normal_path.empty()    ? defaultNormal_    : tryLoad(spec.normal_path,    false);
    m.roughness = spec.roughness_path.empty() ? defaultRoughness_ : tryLoad(spec.roughness_path, false);
    m.ao        = defaultAO_;
    m.height    = defaultHeight_;

    // A slot is only "configured" once it actually has a real texture in
    // m.albedo — never true while m.albedo is still the placeholder, so the
    // shader can never read a placeholder-filled layer.
    matConfigured_[slot] = hasRealAlbedo;

    RebuildMaterialArrays();
}

void EditableTerrain::ClearMaterialSlot(int slot) {
    if (slot < 0 || slot >= (int)materials_.size()) return;
    EnsureDefaultTextures();
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

    matIds_[slot] = 0;
    matAlbedoPaths_[slot].clear();
    matNormalPaths_[slot].clear();
    matOrmPaths_[slot].clear();
    matNormalStrengths_[slot] = 2.5f;
    matConfigured_[slot] = false; // back to placeholder — excluded from the shader

    RebuildMaterialArrays();
}

void EditableTerrain::RemoveMaterialSlot(int slot) {
    if (slot < 0 || slot >= (int)materials_.size()) return;

    Material& m = materials_[slot];
    if (m.normal    == defaultNormal_)    m.normal    = 0;
    if (m.roughness == defaultRoughness_) m.roughness = 0;
    if (m.ao        == defaultAO_)        m.ao        = 0;
    if (m.height    == defaultHeight_)    m.height    = 0;
    m.Unload();

    materials_.erase(materials_.begin() + slot);
    matIds_.erase(matIds_.begin() + slot);
    matAlbedoPaths_.erase(matAlbedoPaths_.begin() + slot);
    matNormalPaths_.erase(matNormalPaths_.begin() + slot);
    matOrmPaths_.erase(matOrmPaths_.begin() + slot);
    matNormalStrengths_.erase(matNormalStrengths_.begin() + slot);
    tilings.erase(tilings.begin() + slot);
    matConfigured_.erase(matConfigured_.begin() + slot);
    // Splatmap weight storage for the removed slot is left in the stack
    // (unused channels are simply ignored by painting/blend) rather than
    // redistributed — the stack never shrinks mid-session.

    RebuildMaterialArrays();
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

    // Materials list — new format: "id tiling albedo normal orm normal_strength"
    // (int id); old format: "name tiling" (folder name). Auto-detected per
    // token: if the first non-whitespace char is a digit, treat as ID.
    // No cap on the number of lines — Phase 1 authoring supports N materials.
    materialNames_.clear();
    matIds_.clear();
    matAlbedoPaths_.clear();
    matNormalPaths_.clear();
    matOrmPaths_.clear();
    matNormalStrengths_.clear();
    tilings.clear();
    bool hasIds = false;
    std::ifstream mt(mtPath);
    if (mt) {
        std::string line;
        while (std::getline(mt, line)) {
            std::istringstream is(line);
            std::string token; float t = 8.f;
            if (!(is >> token)) continue;
            is >> t;
            if (token.empty()) continue;

            int id = 0;
            std::string alb, nrm, orm;
            float ns = 2.5f;
            bool looksInt = std::isdigit((unsigned char)token[0]) || token[0] == '-';
            if (looksInt) {
                try { id = std::stoi(token); hasIds = true; } catch (...) {}
                std::string a, n, o, nsStr;
                if ((is >> a) && a != "-") alb = a;
                if ((is >> n) && n != "-") nrm = n;
                if ((is >> o) && o != "-") orm = o;
                if (is >> nsStr) {
                    try { ns = std::stof(nsStr); } catch (...) {}
                }
            } else {
                materialNames_.push_back(token);
            }

            matIds_.push_back(id);
            matAlbedoPaths_.push_back(alb);
            matNormalPaths_.push_back(nrm);
            matOrmPaths_.push_back(orm);
            matNormalStrengths_.push_back(ns);
            tilings.push_back(t);
        }
    }
    if (!hasIds && materialNames_.empty()) {
        materialNames_      = {"grass", "dirt", "rock"};
        matIds_              = {0, 0, 0};
        matAlbedoPaths_.resize(3);
        matNormalPaths_.resize(3);
        matOrmPaths_.resize(3);
        matNormalStrengths_  = {2.5f, 2.5f, 2.5f};
        tilings              = {8.f, 8.f, 8.f};
    }

    int totalMats = std::max((int)matIds_.size(), (int)materialNames_.size());
    if (totalMats > 0) splatmap_.EnsureMaterialCount(totalMats);

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
        int n = (int)matIds_.size();
        for (int si = 0; si < n; ++si) {
            Material m;
            int c = si % 4;
            m.albedo    = MakeSolidTex(kSlotRGB[c][0], kSlotRGB[c][1], kSlotRGB[c][2]);
            m.normal    = defaultNormal_;
            m.roughness = defaultRoughness_;
            m.ao        = defaultAO_;
            m.height    = defaultHeight_;
            materials_.push_back(std::move(m));
            // Seeded correctly up front: slots with a saved DB id are about
            // to be resolved by ZonesTab::LoadZone's SetMaterialSlot loop
            // (which will confirm/update this); slots with id==0 (e.g. an
            // extra slot saved without ever picking a material) stay
            // unconfigured, excluded from the shader's material count.
            matConfigured_.push_back(matIds_[si] != 0);
        }
        splatmap_.EnsureMaterialCount(n);
        RebuildMaterialArrays();
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
                mt << materialNames_[i] << ' ' << (i < (int)tilings.size() ? tilings[i] : 8.f) << '\n';
        } else {
            // Extended format: "id tiling albedo_path normal_path orm_path normal_strength"
            // Paths are relative to dist/ root — client CWD is dist/client/ so they resolve directly.
            auto dash = [](const std::string& s) -> const std::string& {
                static const std::string d = "-";
                return s.empty() ? d : s;
            };
            for (int i = 0; i < (int)matIds_.size(); ++i) {
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
    if (matIdx < 0 || matIdx >= (int)materials_.size()) return;
    ::PaintSplatmap(splatmap_, wx, wz, radius, strength, dt, matIdx,
                    WorldW(), WorldH(), falloff);
}

void EditableTerrain::AutoPaintBySlope(int flat, int rock, float mn, float mx) {
    int n = (int)materials_.size();
    if (flat < 0 || flat >= n || rock < 0 || rock >= n || flat == rock) return;
    float cs = heightmap_.cell_size;
    int numSlots = splatmap_.NumMaterialSlots();
    for (int z = 0; z < splatmap_.H; z++) {
        for (int x = 0; x < splatmap_.W; x++) {
            float dhdx = (heightmap_.Get(x+1,z) - heightmap_.Get(x-1,z)) / (2.f * cs);
            float dhdz = (heightmap_.Get(x,z+1) - heightmap_.Get(x,z-1)) / (2.f * cs);
            float slope = glm::degrees(std::atan(std::sqrt(dhdx*dhdx + dhdz*dhdz)));
            float t = glm::smoothstep(mn, mx, slope);
            splatmap_.SetWeight(x, z, flat, 1.f - t);
            splatmap_.SetWeight(x, z, rock, t);
            for (int i = 0; i < numSlots; i++)
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
    base.macro_variation  = macroTex_ ? macroTex_ : defaultMacro_;
    base.macro_strength   = macroStrength;
    base.terrain_origin   = glm::vec2(0.f);
    base.terrain_size     = glm::vec2(WorldW(), WorldH());
    base.heightmap_tex    = heightmap_.tex;
    base.cell_size        = heightmap_.cell_size;

    // Phase 1: the generalized N-material path (texture arrays) is now its
    // own shader program (terrainGBufferExt.fs, see pipeline.cpp
    // terrainPass_), fully independent from the legacy exact-4-material
    // program (terrainGBuffer.fs). The gray/black bug that motivated
    // temporarily disabling this path (docs/TECH_DEBT.md "Terrain
    // multi-material authoring (Phase 1)", Bug 124.3) traced back to both
    // paths sharing one linked GLSL program — stale/colliding sampler unit
    // state between draws, not the data or math itself (both were verified
    // correct by direct GPU readback). With separate programs that failure
    // mode is structurally impossible, so the ext path is re-enabled.
    constexpr bool kExtPathEnabled = true;

    // Unconfigured trailing slots (e.g. right after "+ Add Material", before
    // the dev picks a texture) must NOT be sent to the shader at all —
    // otherwise their debug placeholder colour (kSlotRGB[slot%4], e.g. the
    // green (110,155,70) reused by slot 4) blends in with real weight,
    // showing through even where nothing was painted with that material.
    // numConfigured = index of the highest CONFIGURED slot + 1, so trailing
    // unconfigured slots are simply excluded from num_materials/the arrays
    // below (a hole in the MIDDLE, e.g. a cleared slot before the last one,
    // is a separate pre-existing edge case not addressed here).
    int numConfigured = 0;
    for (int i = 0; i < (int)materials_.size(); ++i)
        if (matConfigured_[i]) numConfigured = i + 1;

    if (kExtPathEnabled && numConfigured > 4) {
        base.num_materials            = numConfigured;
        base.ext_splatmap_array       = splatmap_.arrayTex;
        base.ext_num_splat_groups     = splatmap_.NumGroups();
        base.ext_mat_albedo_array     = matAlbedoArray_.tex;
        base.ext_mat_normal_array     = matNormalArray_.tex;
        base.ext_mat_roughness_array  = matRoughnessArray_.tex;
        base.ext_mat_ao_array         = matAoArray_.tex;
        base.ext_mat_height_array     = matHeightArray_.tex;
        base.ext_tilings              = tilings;
        base.ext_mat_normal_strength  = matNormalStrengths_;
    } else {
        // Legacy exact-4-material path — identical to how the editor
        // rendered before Phase 1. num_materials stays 0 (default). Only
        // taken when there are <=4 materials, now that kExtPathEnabled=true.
        base.splatmap = splatmap_.layers.empty() ? 0 : splatmap_.layers[0].tex;
        base.tilings = glm::vec4(
            tilings.size() > 0 ? tilings[0] : 8.f,
            tilings.size() > 1 ? tilings[1] : 8.f,
            tilings.size() > 2 ? tilings[2] : 8.f,
            tilings.size() > 3 ? tilings[3] : 8.f);
        for (int i = 0; i < 4 && i < (int)materials_.size(); ++i) {
            base.mat_albedo[i]          = materials_[i].albedo;
            base.mat_normal[i]          = materials_[i].normal;
            base.mat_roughness[i]       = materials_[i].roughness;
            base.mat_ao[i]              = materials_[i].ao;
            base.mat_height[i]          = materials_[i].height;
            base.mat_normal_strength[i] = i < (int)matNormalStrengths_.size() ? matNormalStrengths_[i] : 2.5f;
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

// ─── Debug dump (F11) ────────────────────────────────────────────────────────

void EditableTerrain::DumpDebugReport(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return;

    static int dumpCount = 0;
    ++dumpCount;

    std::fprintf(f, "\n=== DUMP %d === area=\"%s\" NumMaterials=%d ===\n",
                 dumpCount, areaName_.c_str(), (int)materials_.size());

    splatmap_.WriteDebugReport(f);

    std::fprintf(f, "-- Material albedo array: tex=%u, %d layer(s) --\n",
                 matAlbedoArray_.tex, (int)materials_.size());
    if (matAlbedoArray_.tex != 0) {
        int cx = MaterialTextureArray::kRes / 2, cy = MaterialTextureArray::kRes / 2;
        for (int i = 0; i < (int)materials_.size(); ++i) {
            uint8_t px[4] = {0, 0, 0, 0};
            glGetTextureSubImage(matAlbedoArray_.tex, 0, cx, cy, i, 1, 1, 1,
                                  GL_RGBA, GL_UNSIGNED_BYTE, sizeof(px), px);
            std::fprintf(f, "  layer %d (materials_[%d].albedo GL id=%u, name=\"%s\") center texel = R=%u G=%u B=%u A=%u\n",
                         i, i, materials_[i].albedo, materials_[i].name.c_str(),
                         px[0], px[1], px[2], px[3]);
        }
    } else {
        std::fprintf(f, "  (no albedo array texture yet)\n");
    }

    std::fclose(f);
}

// ─── Destructor ──────────────────────────────────────────────────────────────

EditableTerrain::~EditableTerrain() {
    DestroyChunks();
    DestroyMaterials();
    matAlbedoArray_.Destroy();
    matNormalArray_.Destroy();
    matRoughnessArray_.Destroy();
    matAoArray_.Destroy();
    matHeightArray_.Destroy();
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
