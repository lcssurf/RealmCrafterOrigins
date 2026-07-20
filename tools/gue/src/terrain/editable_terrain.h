#pragma once
#include "heightmap.h"
#include "splatmap.h"
#include "material.h"
#include "brush.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <memory>

namespace rco::renderer { class Pipeline; }

namespace gue {

// One PBR material slot sourced from the media_materials DB table.
// Paths are relative to the dist/ root (e.g. "assets/textures/grass_col.png").
// From GUE (dist/tools/) they are prefixed with "../client/" at load time.
struct TerrainMatSpec {
    int         media_id       = 0;
    std::string name;
    std::string albedo_path;
    std::string normal_path;
    std::string roughness_path;  // ORM or standalone roughness
    float       tiling         = 4.f;
    float       normal_strength = 2.5f;
};

// Editable terrain used inside the GUE Zones tab.
//
// Wraps a Heightmap + Splatmap + materials. Material count is NOT capped
// (Phase 1: authoring supports as many materials as the dev registers, see
// docs/TECH_DEBT.md "Terrain multi-material authoring (Phase 1)"); painting,
// persistence (materials.txt, splatmap.bin) and the live PBR preview all
// scale with however many are configured for the area.
//
// The layout is chunked (same topology as the terrain editor): 64×64 cells
// per chunk, 65×65 vertices. Each chunk's VBO stores only world-space XZ
// positions (2 floats/vertex). Height, normals and tangents are computed in
// the vertex shader by sampling the heightmap GPU texture — eliminating chunk
// seams and halving VBO memory. Brushing uploads only the changed heightmap
// region via UploadRegion; VBOs are never rebuilt after initial creation.
class EditableTerrain {
public:
    static constexpr int   kCells      = 64;
    static constexpr int   kVerts      = kCells + 1;
    static constexpr float kDefaultCS  = 2.f;

    EditableTerrain() = default;
    ~EditableTerrain();

    // Reads dist/client/data/areas/<name>/heightmap.bin + splatmap.bin.
    // Missing files → blank flat terrain of size 512×512, default splatmap.
    // Scans materials from materials.txt (or defaults if missing) so the first
    // few materials from dist/client/data/terrain/materials/<name>/ are loaded.
    bool LoadArea(const std::string& areaName);

    // Writes heightmap.bin + splatmap.bin back to dist/client/data/areas/<name>/.
    // Materials list is written to materials.txt.
    bool SaveArea() const;

    bool Loaded() const { return heightmap_.W > 0; }
    const std::string& Area() const { return areaName_; }

    // Debug tool (F11 in the Zones viewport, see ZonesTab::DrawFloatingToolbar
    // / DrawViewportContent): appends a single, on-demand report to `path` —
    // splatmap weights at 8 spread-out points per layer, plus a center-texel
    // sample per layer of the albedo material texture array. Each call
    // appends a "--- DUMP N ---" separated block rather than overwriting, so
    // a before/after pair (e.g. across adding a material) can be diffed in
    // one file. See docs/TECH_DEBT.md "Terrain multi-material authoring
    // (Phase 1)".
    void DumpDebugReport(const std::string& path) const;

    // GPU upload of any dirty chunks + splatmap changes.
    void Update();

    // Submit every chunk to the client's deferred pipeline so the Zones
    // viewport can preview shading (PBR + shadows + SSAO + volumetrics).
    // Always goes through the generalized N-material path (texture arrays)
    // regardless of material count — see docs/TECH_DEBT.md. Caller is
    // responsible for Begin/End.
    void SubmitToPipeline(rco::renderer::Pipeline& pipeline,
                          const glm::vec3& cam_pos = glm::vec3(0.f));

    // Ray-terrain intersection. Returns true and fills hit if ray meets surface.
    bool Raycast(const glm::vec3& origin, const glm::vec3& dir, glm::vec3& hit) const;

    // Brush operations — caller provides the world-space hit point from Raycast.
    void ApplyBrush(float wx, float wz, float radius, float strength, float dt,
                    BrushMode mode, float flattenH = 0.f,
                    BrushFalloff falloff = BrushFalloff::Smooth);

    // Paint one material layer into the splatmap. matIdx is unbounded — any
    // material registered via SetMaterialSlot/AddMaterialSlot can be painted.
    void Paint(float wx, float wz, float radius, float strength, float dt, int matIdx,
               BrushFalloff falloff = BrushFalloff::Smooth);

    // Fill splatmap based on terrain slope. Pixels below minDeg get flatLayer,
    // above maxDeg get rockLayer; blends smoothly between. All other layers → 0.
    void AutoPaintBySlope(int flatLayer, int rockLayer, float minDeg, float maxDeg);

    // Accessors
    Heightmap&       heightmap()       { return heightmap_; }
    const Heightmap& heightmap() const { return heightmap_; }
    Splatmap&        splatmap()        { return splatmap_; }
    const std::vector<Material>& materials() const { return materials_; }

    // Number of material slots currently registered (>= 1). Not capped —
    // Phase 1 authoring supports as many as the dev configures (16-50+).
    int NumMaterials() const { return (int)materials_.size(); }

    // Appends a brand-new material slot at the end. Returns its index.
    int AddMaterialSlot(const TerrainMatSpec& spec);

    // DB-backed per-slot material assignment. Loads textures immediately.
    // `slot` may equal NumMaterials() to append a new slot in place.
    void SetMaterialSlot (int slot, const TerrainMatSpec& spec);
    void ClearMaterialSlot(int slot);

    // Removes a slot entirely, shifting later slots down by one index.
    // Splatmap weight data for the removed slot is left in the stack
    // (simply unused) rather than redistributed — reloading the area later
    // reclaims the unused capacity.
    void RemoveMaterialSlot(int slot);

    int  materialId(int slot) const {
        return (slot >= 0 && slot < (int)matIds_.size()) ? matIds_[slot] : 0;
    }
    float materialNormalStrength(int slot) const {
        return (slot >= 0 && slot < (int)matNormalStrengths_.size()) ? matNormalStrengths_[slot] : 2.5f;
    }

    // Legacy: set materials by folder name (scans dist/client/data/terrain/materials/<n>/).
    // Still used when opening old-format materials.txt that contains string names.
    void SetMaterialNames(const std::vector<std::string>& names);
    const std::vector<std::string>& materialNames() const { return materialNames_; }

    // Per-material tiling values (UV scale divisor) used by the shader.
    // Sized to NumMaterials(); grows/shrinks alongside the other per-slot
    // vectors (matIds_, matAlbedoPaths_, ...).
    std::vector<float> tilings;

    // Macro variation — breaks tiling repetition. 0 = off.
    float macroStrength = 0.0f;

    // Generate a procedural macro variation texture (low-freq value noise).
    // Uploads to GPU immediately. Call SaveMacro() to persist.
    void GenerateMacro(int seed = 0);

    // Save the current macro texture as macro.png inside the area folder.
    // Returns false if no area loaded or no macro texture exists.
    bool SaveMacro() const;

    // World dimensions — convenience for camera / raycast logic.
    float WorldW() const { return heightmap_.WorldW(); }
    float WorldH() const { return heightmap_.WorldH(); }

private:
    struct Chunk {
        GLuint vao = 0, vbo = 0;
        int    cx = 0, cz = 0;
        bool   dirty = true;
    };

    std::string               areaName_;
    Heightmap                 heightmap_;
    Splatmap                  splatmap_;
    std::vector<Material>     materials_;      // currently-loaded textures, one per slot
    std::vector<std::string>  materialNames_;  // source names for persistence (legacy format)

    std::vector<int>         matIds_;              // media_materials.id per slot, 0 = none
    std::vector<std::string> matAlbedoPaths_;      // relative to dist/ root
    std::vector<std::string> matNormalPaths_;
    std::vector<std::string> matOrmPaths_;
    std::vector<float>       matNormalStrengths_;

    // Tracks whether each slot has a REAL material assigned (DB pick with a
    // non-empty spec, or a legacy folder name that actually resolved to
    // texture files) vs. still being an unconfigured placeholder (debug
    // solid colour, e.g. the green kSlotRGB[0] a freshly-added "+ Add
    // Material" slot starts with). Used to keep unconfigured trailing slots
    // out of the N-material shader path entirely — see SubmitToPipeline and
    // docs/TECH_DEBT.md "Terrain multi-material authoring (Phase 1)".
    std::vector<bool>       matConfigured_;

    // GPU texture arrays feeding the shader's generalized N-material path —
    // one array layer per material slot, rebuilt whenever materials_ changes.
    MaterialTextureArray matAlbedoArray_;
    MaterialTextureArray matNormalArray_;
    MaterialTextureArray matRoughnessArray_;
    MaterialTextureArray matAoArray_;
    MaterialTextureArray matHeightArray_;
    void RebuildMaterialArrays();

    // Grows materials_ and every parallel per-slot vector (+ the splatmap
    // stack) so index `slot` is valid, seeding new slots with a placeholder
    // debug colour. No-op if already large enough.
    void EnsureSlotCapacity(int slot);

    std::vector<Chunk>        chunks_;
    GLuint                    ebo_       = 0;
    int                       idxCount_  = 0;
    int                       cxCount_   = 0;
    int                       czCount_   = 0;

    GLuint                    defaultNormal_    = 0;  // flat (128,128,255)
    GLuint                    defaultRoughness_ = 0;  // grey (180)
    GLuint                    defaultAO_        = 0;  // white (255) — no occlusion
    GLuint                    defaultHeight_    = 0;  // white (255) — flat height-blend
    GLuint                    defaultMacro_     = 0;  // 0.5 gray — no overlay change
    GLuint                    macroTex_         = 0;  // generated or loaded macro variation

    void InitChunks();
    void BuildChunk(Chunk& c);
    void DestroyChunks();
    void DestroyMaterials();
    void EnsureDefaultTextures();
    void ReloadMaterials();

    void MarkDirtyRegion(float wx, float wz, float radius);
    void MarkDirtyAll();
};

} // namespace gue
