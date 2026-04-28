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

// Editable terrain used inside the GUE Zones tab.
//
// Wraps a Heightmap + Splatmap + materials and renders them with a simple
// forward shader (splatmap-blended albedo + directional lighting). Not the
// client's deferred PBR pipeline — just enough to edit zones visually.
//
// The layout is chunked (same topology as the terrain editor): 64×64 cells
// per chunk, 65×65 vertices. Chunks only rebuild when MarkDirtyRegion
// touches them, so brushing stays fast on 512×512+ terrains.
class EditableTerrain {
public:
    static constexpr int   kCells      = 64;
    static constexpr int   kVerts      = kCells + 1;
    static constexpr float kDefaultCS  = 2.f;
    static constexpr int   kMaxMats    = 4;

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

    // GPU upload of any dirty chunks + splatmap changes.
    void Update();

    // Render one frame with the GUE's own simple forward shader.
    // vp = proj * view. sunDir is world-space (already normalised).
    void RenderFrame(const glm::mat4& vp,
                     const glm::vec3& sunDir = glm::normalize(glm::vec3(0.5f,1.f,0.3f)));

    // Alternate path: submit every chunk to the client's deferred pipeline
    // so the Zones viewport can match the in-game terrain shading (PBR +
    // shadows + SSAO + volumetrics). Caller is responsible for Begin/End.
    void SubmitToPipeline(rco::renderer::Pipeline& pipeline);

    // Ray-terrain intersection. Returns true and fills hit if ray meets surface.
    bool Raycast(const glm::vec3& origin, const glm::vec3& dir, glm::vec3& hit) const;

    // Brush operations — caller provides the world-space hit point from Raycast.
    void ApplyBrush(float wx, float wz, float radius, float strength, float dt,
                    BrushMode mode, float flattenH = 0.f);

    // Paint one material layer into the splatmap.
    void Paint(float wx, float wz, float radius, float strength, float dt, int matIdx);

    // Accessors
    Heightmap&       heightmap()       { return heightmap_; }
    const Heightmap& heightmap() const { return heightmap_; }
    Splatmap&        splatmap()        { return splatmap_; }
    const std::vector<Material>& materials() const { return materials_; }

    // Set materials list by names (scans dist/client/data/terrain/materials/<n>/).
    // Replaces existing bound materials (keeping at most kMaxMats).
    void SetMaterialNames(const std::vector<std::string>& names);
    const std::vector<std::string>& materialNames() const { return materialNames_; }

    // Heightmap tiling value (UV scale divisor) used by the shader.
    float tiling = 8.f;

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
    std::vector<Material>     materials_;      // currently-loaded textures (<= kMaxMats)
    std::vector<std::string>  materialNames_;  // source names for persistence

    std::vector<Chunk>        chunks_;
    GLuint                    ebo_       = 0;
    int                       idxCount_  = 0;
    int                       cxCount_   = 0;
    int                       czCount_   = 0;

    GLuint                    prog_      = 0;
    GLuint                    defaultNormal_    = 0;  // flat (128,128,255)
    GLuint                    defaultRoughness_ = 0;  // grey (180)
    GLuint                    defaultAO_        = 0;  // white (255) — no occlusion
    GLuint                    defaultHeight_    = 0;  // white (255) — flat height-blend

    void InitShader();
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
