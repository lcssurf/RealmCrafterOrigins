#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include <vector>

#include "rco/renderer/engine.h"

namespace rco::renderer {

class StaticBuffer;

struct FeatureConfig {
    bool ssao          = true;
    bool volumetrics   = true;
    bool ssr           = false;
    bool fxaa          = true;
    // PCF (default) is more robust than ESM/VSM/MSM when most of the shadow
    // map is empty (depth=1) because only a few actors project shadows —
    // exponential variants blur the near-infinite background into the
    // foreground depth and produce no shadows on the ground.
    int  shadow_method = SHADOW_METHOD_PCF;
};

struct DynamicDrawRequest {
    GLuint    vao         = 0;
    GLuint    vbo         = 0;
    GLuint    ebo         = 0;
    GLsizei   index_count = 0;
    int       material_idx = 0;
    glm::mat4 model        = glm::mat4(1.0f);

    // Direct albedo texture — bypasses the bindless SSBO material system.
    // When non-zero, bound to unit 10 and used via a plain sampler2D in
    // gBufferBindless.fs. Avoids driver issues with newly-created bindless
    // handles for embedded (GLB) textures.
    GLuint    tex_albedo  = 0;

    // optional skinning
    GLuint bone_ssbo  = 0;
    int    bone_count = 0;

    // 0 = world/static readability, 1 = character readability.
    float readability_mask = 0.0f;
};

// One CPU-side entry per skinned actor instance submitted via Actor::Submit().
// The pipeline groups entries by (vao, ebo) and issues one
// glDrawElementsInstanced per group, collapsing N draw calls into 1.
// bones[64] must match kMaxBones in model.h.
struct SkinnedInstancedEntry {
    GLuint    vao;
    GLuint    ebo;
    GLsizei   index_count;
    int       material_idx;
    glm::mat4 model;
    glm::mat4 bones[64];
    float     readability_mask = 0.0f;
};

struct TerrainChunkSubmission {
    GLuint    vao         = 0;
    GLuint    vbo         = 0;
    GLuint    ebo         = 0;
    GLsizei   index_count = 0;
    glm::mat4 model       = glm::mat4(1.0f);

    GLuint    splatmap    = 0;

    // 4 materials (albedo/normal/roughness/ao/height). Unused slots can be 0.
    GLuint    mat_albedo[4]    = {0,0,0,0};
    GLuint    mat_normal[4]    = {0,0,0,0};
    GLuint    mat_roughness[4] = {0,0,0,0};
    GLuint    mat_ao[4]        = {0,0,0,0};
    GLuint    mat_height[4]    = {0,0,0,0};

    glm::vec4 tilings               = glm::vec4(4.0f);           // per-layer tiling (xyzw = layers 0-3)
    GLuint    macro_variation        = 0;                         // grayscale texture covering full terrain
    float     macro_strength         = 0.0f;
    float     mat_normal_strength[4] = {2.5f, 2.5f, 2.5f, 2.5f}; // per-material normal intensity
    glm::vec2 terrain_origin = glm::vec2(0.0f);
    glm::vec2 terrain_size   = glm::vec2(1.0f);
    GLuint    heightmap_tex  = 0;               // R32F heightmap — read by vertex shader
    float     cell_size      = 2.0f;
    float     lod_level      = 0.0f;           // fractional LOD (0=full, 1=half, 2=quarter …)
};

class Pipeline {
public:
    struct CharacterReadabilityTuning {
        float shadowLift  = 0.30f; // lifts shadowed characters for combat readability
        float rimStrength = 0.18f; // subtle silhouette edge light
        float rimExponent = 2.4f;  // controls rim falloff
        float minNdotL    = 0.10f; // keeps a minimum key light on characters
        float ambientBoost = 0.12f; // ambient gain only for characters
    };

    struct SceneLookTuning {
        // Multiplies indirect lighting (irradiance + specular prefilter) from IBL.
        float iblIntensity = 1.00f;
        // Multiplies the visible skybox only (does not affect IBL probes).
        float skyIntensity = 1.00f;
        // Lifts world shadows to avoid black crush on legacy content.
        float worldShadowLift = 0.20f;
        // Balances directional sun against IBL/fill lighting.
        float directScale = 1.00f;
        float ambientScale = 1.00f;
        // Neutral albedo-based fill independent of HDR directionality.
        float flatAmbient = 0.10f;
        // Keeps world faces readable even when their normals point away from the sun.
        float worldMinNdotL = 0.06f;
        // Raises very dark legacy albedo into a physically usable range.
        float albedoMinLuma = 0.18f;
        float albedoLiftStrength = 0.0f;
        // Tames harsh highlights from legacy/partial PBR materials.
        float specularScale = 0.60f;
        // Overall HDR exposure before tonemapping.
        float exposureFactor = 1.00f;
        // Multiplies area sun color before it reaches the pipeline.
        float sunIntensity = 1.10f;
    };

    explicit Pipeline(Engine& e);
    ~Pipeline();
    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void SetFeatures(const FeatureConfig& cfg);
    const FeatureConfig& Features() const { return features_; }
    void SetSun(const glm::vec3& direction, const glm::vec3& color);
    void SetCharacterReadability(const CharacterReadabilityTuning& cfg);
    const CharacterReadabilityTuning& CharacterReadability() const { return characterReadability_; }
    void SetSceneLook(const SceneLookTuning& cfg);
    const SceneLookTuning& SceneLook() const { return sceneLook_; }
    void SetColorGrading(float contrast,
                         float saturation,
                         float vibrance,
                         float blackPoint,
                         float vignetteStrength,
                         float vignetteSoftness);

    // Debug visualisation. 0=full lighting (default).
    // 1=albedo, 2=normal, 3=depth, 4=AO, 5=shadow, 6=irradiance,
    // 7=NoL, 8=albedo*NoL, 9=envDiffuse, 10=direct light (no shadow).
    void SetDebugMode(int mode) { debugMode_ = mode; }
    int  DebugMode() const { return debugMode_; }

    struct FrameStats {
        int triangles  = 0;
        int draw_calls = 0;
    };
    FrameStats LastFrameStats() const { return frame_stats_; }

    void Begin(const glm::mat4& view,
               const glm::mat4& proj,
               const glm::vec3& cam_pos,
               float dt);

    void SubmitStaticScene();
    void SubmitDynamic(const DynamicDrawRequest& req);
    void SubmitSkinned(const DynamicDrawRequest& req);
    void SubmitSkinnedInstanced(const SkinnedInstancedEntry& e);
    void SubmitTerrainChunk(const TerrainChunkSubmission& chunk);

    void AddPointLight(const glm::vec3& pos, const glm::vec3& color, float radius);

    struct EndConfig {
        // When false, the caller reads engine.finalImage() (ImTextureID for tools).
        // When true (default), the final composed image is blitted to framebuffer 0.
        bool blit_to_default = true;

        // Runs between compositePass_ and tonemappingPass_, with postprocessFbo_
        // bound (gDepth_ attached). Use for particles / brush overlays / transparents.
        std::function<void()> forward_pass = {};
    };

    void End();
    // Forward-pass variant: invokes `forward_pass` between the deferred composite
    // and the tonemap/FXAA/blit. The callback draws into postprocessFbo_ which has
    // the G-buffer depth attached, so depth test is coherent with the deferred scene.
    void End(const std::function<void()>& forward_pass);
    // Full control — lets tools opt out of blit_to_default.
    void End(const EndConfig& cfg);

private:
    void computeLightMatrix_();
    void shadowPass_();
    void gBufferPass_();
    void terrainPass_();
    void ssaoPass_();
    void globalLightPass_();
    void localLightsPass_();
    void skyboxPass_();
    void volumetricPass_();
    void ssrPass_();
    void compositePass_();
    void tonemappingPass_(float dt);
    void fxaaPass_();
    void finalBlit_();

    Engine*       engine_ = nullptr;
    FeatureConfig features_{};
    DirLight      sun_{};
    float         sunConstantC_   = 80.0f;
    float         vlightBleedFix_ = 0.9f;

    glm::mat4 view_     = glm::mat4(1.0f);
    glm::mat4 proj_     = glm::mat4(1.0f);
    glm::mat4 viewProj_ = glm::mat4(1.0f);
    glm::vec3 camPos_   = glm::vec3(0.0f);
    float     dt_       = 0.0f;

    glm::mat4 lightMat_ = glm::mat4(1.0f);

    std::vector<PointLight>       localLights_;
    std::unique_ptr<StaticBuffer> lightSSBO_;

    std::vector<DynamicDrawRequest>     dynamicDraws_;
    std::vector<DynamicDrawRequest>     skinnedDraws_;
    std::vector<SkinnedInstancedEntry>  skinnedInstancedEntries_;
    std::vector<TerrainChunkSubmission> terrainChunks_;

    GLuint filteredShadowTex_ = 0;

    int blurPasses_   = 1;
    int blurStrength_ = 5;

    int debugMode_ = 0;

    FrameStats frame_stats_;
    FrameStats pending_stats_;

    struct VolumetricTuning {
        GLint steps        = 24; // 16 | 24 | 32
        float intensity    = 0.006f; // subtle default; avoids over-fogging gameplay
        float noiseOffset  = 1.0f;
        float beerPower    = 1.0f; // higher makes light falloff sharper, lower makes it softer
        float powderPower  = 1.0f; // higher makes light scatter more forward, lower makes it more isotropic
        float distanceScale = 1.0f;
        float heightOffset = 0.0f; // raises the volumetric effect above the ground, helps hide artifacts when using few steps
        float hfIntensity  = 0.012f; // lower by default for cleaner readability
        int   atrous_passes = 2;
        float c_phi        = 0.04f;
        float stepWidth    = 1.0f;
        float atrouskernel[25] = {
            1.0f/256,  4.0f/256,  6.0f/256,  4.0f/256,  1.0f/256,
            4.0f/256, 16.0f/256, 24.0f/256, 16.0f/256,  4.0f/256,
            6.0f/256, 24.0f/256, 36.0f/256, 24.0f/256,  6.0f/256,
            4.0f/256, 16.0f/256, 24.0f/256, 16.0f/256,  4.0f/256,
            1.0f/256,  4.0f/256,  6.0f/256,  4.0f/256,  1.0f/256,
        };
        glm::vec2 atrouskerneloffsets[25] = {
            {-2,2},{-1,2},{0,2},{1,2},{2,2},
            {-2,1},{-1,1},{0,1},{1,1},{2,1},
            {-2,0},{-1,0},{0,0},{1,0},{2,0},
            {-2,-1},{-1,-1},{0,-1},{1,-1},{2,-1},
            {-2,-2},{-1,-2},{0,-2},{1,-2},{2,-2},
        };
    } volumetric_{};

    struct SSAOTuning {
        int   samples_near     = 12;
        float delta            = 0.001f;
        float range            = 1.1f;
        float s                = 2.2f;
        float k                = 1.0f;
        int   atrous_passes    = 3;
        float atrous_n_phi     = 0.1f;
        float atrous_p_phi     = 0.5f;
        float atrous_step_width = 1.0f;
        float atrous_kernel[5]  = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };
        float atrous_offsets[5] = { -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };
    } ssao_{};

    struct SSRTuning {
        float rayStep          = 0.15f;
        float minRayStep       = 0.1f;
        float thickness        = 0.0f;
        float searchDist       = 15.0f;
        int   maxRaySteps      = 30;
        int   binarySearchSteps = 5;
    } ssr_{};

    struct FXAATuning {
        float contrastThreshold  = 0.0312f;
        float relativeThreshold  = 0.125f;
        float pixelBlendStrength = 1.0f;
        float edgeBlendStrength  = 1.0f;
    } fxaa_{};

    struct HDRTuning {
        float targetLuminance  = 0.22f;
        float minExposure      = 0.1f;
        float maxExposure      = 100.0f;
        float exposureFactor   = 0.92f;
        float adjustmentSpeed  = 2.0f;
        int   numBuckets       = 128;
    } hdr_{};

    struct ColorGradingTuning {
        float contrast         = 1.00f;
        float saturation       = 1.03f;
        float vibrance         = 0.12f;
        float blackPoint       = 0.005f;
        float vignetteStrength = 0.08f;
        float vignetteSoftness = 0.45f;
    } color_{};

    struct LightingBalanceTuning {
        float directScale  = 0.88f;
        float ambientScale = 1.00f;
    } lightBalance_{};

    CharacterReadabilityTuning characterReadability_{};
    SceneLookTuning sceneLook_{};

    int numEnvSamples_ = 10;
};

} // namespace rco::renderer
