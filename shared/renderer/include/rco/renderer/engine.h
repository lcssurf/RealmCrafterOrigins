#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rco/renderer/buffers.h"
#include "rco/renderer/indirect.h"
#include "rco/renderer/light.h"
#include "rco/renderer/material.h"
#include "rco/renderer/mesh.h"
#include "rco/renderer/texture.h"

namespace rco::renderer {

class Pipeline;

// Shadow methods (same values as glRenderer — used by shaders)
constexpr int SHADOW_METHOD_PCF = 0;
constexpr int SHADOW_METHOD_VSM = 1;
constexpr int SHADOW_METHOD_ESM = 2;
constexpr int SHADOW_METHOD_MSM = 3;

struct EngineConfig {
    int         width               = 1280;
    int         height              = 720;
    GLuint      shadow_width        = 1024;
    GLuint      shadow_height       = 1024;
    std::string shader_dir          = "shaders/";
    bool        enable_debug_output = true;
    int         max_static_vertices = 5'000'000;
};

struct StaticMeshHandle {
    std::size_t batch_index = 0;
};

class Engine {
public:
    Engine() = default;
    ~Engine();
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    void Init(const EngineConfig& cfg);
    void Shutdown();
    void Resize(int w, int h);

    void LoadEnvironment(const std::string& hdr_path);
    void ForceReloadEnvironment();
    bool ConsumeLoadEnvironmentSkipped(std::string* out_path = nullptr);

    void BeginStaticScene();
    StaticMeshHandle UploadStaticMesh(const std::vector<Vertex>&   vertices,
                                      const std::vector<uint32_t>& indices,
                                      int material_index);
    void EndStaticScene();

    MaterialManager& materials() { return material_manager_; }

    // Marks materials SSBO as stale. The buffer is rebuilt once on the next
    // frame flush, coalescing many writes into a single GL upload.
    void MarkMaterialsDirty();
    // Rebuilds materials SSBO when dirty. Returns true when a rebuild occurred.
    bool FlushMaterialsBufferIfDirty(uint64_t* out_dur_us = nullptr);

    int    width()       const { return width_;  }
    int    height()      const { return height_; }
    GLuint gAlbedo()       const { return gAlbedo_; }
    GLuint gNormal()       const { return gNormal_; }
    GLuint gDepth()        const { return gDepth_;  }
    GLuint gRMA()          const { return gRMA_;    }
    GLuint finalImage()    const { return legitFinalImage_; }
    GLuint envCubemap()      const { return envCubemap_; }
    GLuint irradianceCube()  const { return irradianceCube_; }
    GLuint prefilterCube()   const { return prefilterCube_; }
    GLuint brdfLUT()         const { return brdfLUT_; }

private:
    friend class Pipeline;

    void installDebugCallback_();
    void createFramebuffers_();
    void destroyFramebuffers_();
    void createVAO_();
    void RebuildMaterialsBufferImpl();

    int    width_             = 0;
    int    height_            = 0;
    float  deviceAnisotropy_  = 0.0f;
    int    maxStaticVertices_ = 0;

    GLuint vao_ = 0;
    std::unique_ptr<DynamicBuffer> vertexBuffer_;
    std::unique_ptr<DynamicBuffer> indexBuffer_;
    std::unique_ptr<StaticBuffer>  materialsBuffer_;
    std::unique_ptr<StaticBuffer>  drawIndirectBuffer_;
    MaterialManager                material_manager_;
    bool                           materials_dirty_ = false;

    struct StaticMeshRecord {
        uint64_t vtx_alloc     = 0;
        uint64_t idx_alloc     = 0;
        int      material_index = 0;
    };
    std::vector<StaticMeshRecord> staticMeshes_;
    bool static_scene_open_ = false;

    std::unique_ptr<Texture2D> envMap_hdri_;
    std::string last_env_path_;
    std::string last_skipped_env_path_;
    bool force_reload_environment_ = false;
    bool last_load_environment_skipped_ = false;
    GLuint irradianceMap_ = 0;

    // IBL cubemap suite (replaces the equirectangular 2D approach).
    GLuint envCubemap_         = 0;   // GL_RGB16F cubemap, 512² + mips
    GLuint irradianceCube_     = 0;   // GL_RGB16F cubemap, 32²
    GLuint prefilterCube_      = 0;   // GL_RGB16F cubemap, 128² + 5 mips
    GLuint brdfLUT_            = 0;   // GL_RG16F 2D, 512²
    GLuint iblCubeVAO_         = 0;   // unit cube VAO for capture passes
    GLuint iblCubeVBO_         = 0;
    int    envCubemapSize_     = 512;
    int    irradianceCubeSize_ = 32;
    int    prefilterCubeSize_  = 128;
    int    prefilterMipLevels_ = 5;
    int    brdfLUTSize_        = 512;

    std::unique_ptr<Texture2D> bluenoiseTex_;

    // G-buffer
    GLuint gfbo_     = 0;
    GLuint gAlbedo_  = 0;
    GLuint gNormal_  = 0;
    GLuint gDepth_   = 0;
    GLuint gRMA_     = 0;

    // HDR accumulator
    GLuint hdrFbo_      = 0;
    GLuint hdrColorTex_ = 0;
    GLuint hdrDepthTex_ = 0;

    // Shadow depth
    GLuint shadowFbo_    = 0;
    GLuint shadowDepth_  = 0;
    GLuint shadowWidth_  = 0;
    GLuint shadowHeight_ = 0;
    GLuint shadowLevels_ = 0;

    // VSM
    GLuint vshadowGoodFormatFbo_   = 0;
    GLuint vshadowDepthGoodFormat_ = 0;
    GLuint vshadowMomentBlur_      = 0;

    // ESM
    GLuint eShadowFbo_       = 0;
    GLuint eExpShadowDepth_  = 0;
    GLuint eShadowDepthBlur_ = 0;

    // MSM
    GLuint msmShadowFbo_         = 0;
    GLuint msmShadowMoments_     = 0;
    GLuint msmShadowMomentsBlur_ = 0;

    // SSAO
    GLuint ssaoFbo_     = 0;
    GLuint ssaoTex_     = 0;
    GLuint ssaoBlurred_ = 0;

    // SSR (half resolution)
    GLuint ssrFbo_     = 0;
    GLuint ssrTex_     = 0;
    GLuint ssrTexBlur_ = 0;
    GLuint ssrWidth_   = 0;
    GLuint ssrHeight_  = 0;

    // Volumetrics
    GLuint volumetricsFbo_       = 0;
    GLuint volumetricsTex_       = 0;
    GLuint volumetricsTexBlur_   = 0;
    GLuint volumetricsAtrousFbo_ = 0;
    GLuint volumetricsAtrousTex_ = 0;

    // Tonemapping + postprocess
    GLuint postprocessFbo_      = 0;
    GLuint postprocessColor_    = 0;
    GLuint postprocessPostSRGB_ = 0;
    GLuint legitFinalImage_     = 0;
    std::unique_ptr<StaticBuffer> histogramBuffer_;
    std::unique_ptr<StaticBuffer> exposureBuffer_;
};

} // namespace rco::renderer
