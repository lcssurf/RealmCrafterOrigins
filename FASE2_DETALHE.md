# Fase 2 detalhada — Split `Renderer` → `Engine` + `Pipeline`

Tutorial auto-contido. Execute os passos **em ordem**, na íntegra, sem pular.
Cada passo tem critério de sucesso no final.

## Pré-requisitos

- Fase 1 completa (todos os critérios de sucesso da Fase 1 passam)
- Existem em `shared/renderer/`:
  - `include/rco/renderer/{utilities,indirect,light,object,buffers,texture,material,mesh,shader,helpers}.h`
  - `src/{light,buffers,texture,material,mesh,shader,helpers,compile_shaders}.cpp`
  - `src/_orig_renderer.h` + `_orig_renderer.cpp` (temporários da Fase 1)
  - `src/_stub_camera.h` + `_stub_input.h` (temporários da Fase 1)
  - `include/rco/renderer/{shader_old,model_old}.h` + correspondentes `.cpp` (preservados)
- `rco_renderer` compila e `rco_client.exe` ainda roda com o renderer antigo.

## Regras gerais (aplicam a TODOS os arquivos criados nesta fase)

1. **Namespace obrigatório**: todo símbolo novo vive em `namespace rco::renderer { ... }`
2. **Zero GLFW, zero ImGui, zero `Input`**: o client é dono da janela e dos inputs. `Engine` e `Pipeline` NÃO chamam `glfw*`, `ImGui_*`, `Input::*`.
3. **Zero cena**: `Engine/Pipeline` não conhecem "Scene1", "sphere.obj", "batchedObjects". Cena vem de fora via API pública.
4. **`WINDOW_WIDTH`/`WINDOW_HEIGHT` sumiram**: no código novo usa-se `engine_->width_` e `engine_->height_`. Toda referência literal a `WINDOW_WIDTH`/`WINDOW_HEIGHT` dentro de funções copiadas de `_orig_renderer.cpp` vira `width_`/`height_` (Engine) ou `engine_->width()`/`engine_->height()` (Pipeline).
5. **Friend class**: `Pipeline` é `friend class Engine::Pipeline` (ou equivalente) para acessar `gfbo`, texturas G-buffer, shadow FBOs etc. sem centenas de getters. Escolha adotada: **`friend class Pipeline;`** declarado dentro de `Engine`.
6. **`#pragma once`** no topo de todo `.h`. `.cpp` inclui o próprio `.h` na primeira linha.

---

## Passo 2.1 — Criar `include/rco/renderer/engine.h`

Gerar `shared/renderer/include/rco/renderer/engine.h`:

```cpp
#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
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
    int         width              = 1280;
    int         height             = 720;
    GLuint      shadow_width       = 1024;
    GLuint      shadow_height      = 1024;
    std::string shader_dir         = "shaders/";
    bool        enable_debug_output = true;    // installs glDebugMessageCallback
    int         max_static_vertices = 5'000'000;
};

// Handle returned by UploadStaticMesh — passed back to SubmitStaticScene implicitly
// (client doesn't need to keep it unless doing partial rebuilds)
struct StaticMeshHandle {
    std::size_t batch_index = 0;
};

class Engine {
public:
    Engine() = default;
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // One-time init / teardown
    void Init(const EngineConfig& cfg);
    void Shutdown();

    // Resize all window-sized framebuffers
    void Resize(int w, int h);

    // IBL environment (HDRI → equirect env map + convolved irradiance)
    void LoadEnvironment(const std::string& hdr_path);

    // Static scene upload (rebuild when area changes)
    void BeginStaticScene();
    StaticMeshHandle UploadStaticMesh(const std::vector<Vertex>&   vertices,
                                      const std::vector<uint32_t>& indices,
                                      int material_index);
    void EndStaticScene();

    // Material manager (caller registers materials before uploading meshes)
    MaterialManager& materials() { return material_manager_; }

    // Dimensions
    int width()  const { return width_; }
    int height() const { return height_; }

    // G-buffer read-access (particles pass uses gDepth for depth test, etc.)
    GLuint gAlbedo() const { return gAlbedo_; }
    GLuint gNormal() const { return gNormal_; }
    GLuint gDepth()  const { return gDepth_;  }
    GLuint gRMA()    const { return gRMA_;    }
    GLuint finalImage() const { return legitFinalImage_; }

private:
    friend class Pipeline;

    // ---------- setup helpers ----------
    void installDebugCallback_();
    void createFramebuffers_();
    void destroyFramebuffers_();
    void createVAO_();

    // ---------- config ----------
    int    width_  = 0;
    int    height_ = 0;
    float  deviceAnisotropy_ = 0.0f;
    int    maxStaticVertices_ = 0;

    // ---------- GPU resources ----------
    GLuint vao_ = 0;
    std::unique_ptr<DynamicBuffer> vertexBuffer_;
    std::unique_ptr<DynamicBuffer> indexBuffer_;
    std::unique_ptr<StaticBuffer>  materialsBuffer_;
    std::unique_ptr<StaticBuffer>  drawIndirectBuffer_;
    MaterialManager                material_manager_;

    // temporary staging during BeginStaticScene/EndStaticScene
    struct StaticMeshRecord {
        DynamicBuffer::AllocHandle vtx_alloc;
        DynamicBuffer::AllocHandle idx_alloc;
        int material_index = 0;
    };
    std::vector<StaticMeshRecord> staticMeshes_;
    bool static_scene_open_ = false;

    // ---------- IBL ----------
    std::unique_ptr<Texture2D> envMap_hdri_;
    GLuint irradianceMap_ = 0;

    // ---------- bluenoise ----------
    std::unique_ptr<Texture2D> bluenoiseTex_;

    // ---------- deferred G-buffer ----------
    GLuint gfbo_ = 0;
    GLuint gAlbedo_ = 0;
    GLuint gNormal_ = 0;
    GLuint gDepth_  = 0;
    GLuint gRMA_    = 0;

    // ---------- HDR accumulator ----------
    GLuint hdrFbo_      = 0;
    GLuint hdrColorTex_ = 0;
    GLuint hdrDepthTex_ = 0;

    // ---------- shadow: base depth ----------
    GLuint shadowFbo_   = 0;
    GLuint shadowDepth_ = 0;
    GLuint shadowWidth_  = 0;
    GLuint shadowHeight_ = 0;
    GLuint shadowLevels_ = 0;

    // ---------- shadow: VSM ----------
    GLuint vshadowGoodFormatFbo_  = 0;
    GLuint vshadowDepthGoodFormat_ = 0;
    GLuint vshadowMomentBlur_     = 0;

    // ---------- shadow: ESM ----------
    GLuint eShadowFbo_       = 0;
    GLuint eExpShadowDepth_  = 0;
    GLuint eShadowDepthBlur_ = 0;

    // ---------- shadow: MSM ----------
    GLuint msmShadowFbo_         = 0;
    GLuint msmShadowMoments_     = 0;
    GLuint msmShadowMomentsBlur_ = 0;

    // ---------- SSAO ----------
    GLuint ssaoFbo_      = 0;
    GLuint ssaoTex_      = 0;
    GLuint ssaoBlurred_  = 0;

    // ---------- SSR ----------
    GLuint ssrFbo_      = 0;
    GLuint ssrTex_      = 0;
    GLuint ssrTexBlur_  = 0;
    GLuint ssrWidth_    = 0;
    GLuint ssrHeight_   = 0;

    // ---------- volumetrics ----------
    GLuint volumetricsFbo_       = 0;
    GLuint volumetricsTex_       = 0;
    GLuint volumetricsTexBlur_   = 0;
    GLuint volumetricsAtrousFbo_ = 0;
    GLuint volumetricsAtrousTex_ = 0;

    // ---------- tonemapping + postprocess ----------
    GLuint postprocessFbo_      = 0;
    GLuint postprocessColor_    = 0;
    GLuint postprocessPostSRGB_ = 0;
    GLuint legitFinalImage_     = 0;
    std::unique_ptr<StaticBuffer> histogramBuffer_;
    std::unique_ptr<StaticBuffer> exposureBuffer_;
};

} // namespace rco::renderer
```

**Sucesso**: arquivo existe, compila sozinho (só declarações; nenhum corpo).

---

## Passo 2.2 — Criar `src/engine.cpp`

Gerar `shared/renderer/src/engine.cpp`. Vamos preencher em 5 sub-passos.

### 2.2.a — Esqueleto do arquivo

```cpp
#include "rco/renderer/engine.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "rco/renderer/helpers.h"
#include "rco/renderer/shader.h"

namespace rco::renderer {

namespace {
// GL debug output callback
void GLAPIENTRY GLerrorCB(GLenum source, GLenum type, GLuint id, GLenum severity,
                          GLsizei /*length*/, const GLchar* message,
                          const void* /*userParam*/) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    std::cerr << "[GL] " << message << "\n";
}
} // namespace

// ... Init/Shutdown/Resize/LoadEnvironment/static-scene helpers go below ...

} // namespace rco::renderer
```

### 2.2.b — `Init` / `installDebugCallback_` / `Shutdown` / destructor

```cpp
void Engine::installDebugCallback_() {
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
    glDebugMessageCallback(GLerrorCB, nullptr);
}

void Engine::Init(const EngineConfig& cfg) {
    width_              = cfg.width;
    height_             = cfg.height;
    shadowWidth_        = cfg.shadow_width;
    shadowHeight_       = cfg.shadow_height;
    shadowLevels_       = (GLuint)glm::ceil(glm::log2(
        (float)glm::max(shadowWidth_, shadowHeight_)));
    maxStaticVertices_  = cfg.max_static_vertices;

    if (cfg.enable_debug_output) {
        installDebugCallback_();
    }
    glEnable(GL_MULTISAMPLE);
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &deviceAnisotropy_);

    // bluenoise (volumetric noise offset)
    TextureCreateInfo bnInfo{
        .path = "assets/textures/bluenoise_64.png",
        .sRGB = false, .generateMips = false, .HDR = false,
        .minFilter = GL_LINEAR, .magFilter = GL_LINEAR,
    };
    bluenoiseTex_ = std::make_unique<Texture2D>(bnInfo);

    // static scene buffers
    vertexBuffer_ = std::make_unique<DynamicBuffer>(
        sizeof(Vertex) * maxStaticVertices_, sizeof(Vertex));
    indexBuffer_ = std::make_unique<DynamicBuffer>(
        sizeof(uint32_t) * maxStaticVertices_, sizeof(uint32_t));

    createFramebuffers_();
    createVAO_();

    // Shader dir + compilation (the rco_renderer loads all registered shaders)
    Shader::SetShaderDir(cfg.shader_dir);
    CompileShaders();   // declared in rco/renderer/helpers.h
}

void Engine::Shutdown() {
    destroyFramebuffers_();
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    vertexBuffer_.reset();
    indexBuffer_.reset();
    materialsBuffer_.reset();
    drawIndirectBuffer_.reset();
    histogramBuffer_.reset();
    exposureBuffer_.reset();
    bluenoiseTex_.reset();
    envMap_hdri_.reset();
    if (irradianceMap_) { glDeleteTextures(1, &irradianceMap_); irradianceMap_ = 0; }
}

Engine::~Engine() {
    // Defensive: Shutdown() may have been called already; these are all 0 then.
    Shutdown();
}
```

### 2.2.c — `createFramebuffers_` (copiar de `_orig_renderer.cpp`, adaptar)

Fonte: `_orig_renderer.cpp::CreateFramebuffers` (linhas 686–890 do `glRenderer/Renderer.cpp`).

Transformações obrigatórias:

| Original | Novo |
|---|---|
| `ssao.fbo` | `ssaoFbo_` |
| `ssao.texture` / `ssao.textureBlurred` | `ssaoTex_` / `ssaoBlurred_` |
| `hdr.fbo` / `hdr.colorTex` / `hdr.depthTex` | `hdrFbo_` / `hdrColorTex_` / `hdrDepthTex_` |
| `hdr.NUM_BUCKETS` | literal `128` (inline na criação) |
| `hdr.exposureFactor` | literal `1.0f` |
| `hdr.exposureBuffer` | `exposureBuffer_` |
| `hdr.histogramBuffer` | `histogramBuffer_` |
| `ssr.fbo/tex/texBlur/framebuffer_width/height` | `ssrFbo_/ssrTex_/ssrTexBlur_/ssrWidth_/ssrHeight_` (width/height = `width_/2` / `height_/2`) |
| `volumetrics.fbo/tex/texBlur/atrousFbo/atrousTex` | `volumetricsFbo_/Tex_/TexBlur_/AtrousFbo_/AtrousTex_` |
| `volumetrics.framebuffer_width/_height` | `width_` / `height_` (removemos o cap fixo) |
| `shadowFbo/shadowDepth` | `shadowFbo_/shadowDepth_` |
| `SHADOW_WIDTH/SHADOW_HEIGHT/SHADOW_LEVELS` | `shadowWidth_/shadowHeight_/shadowLevels_` |
| `shadow_gen_mips` | parameter ainda não existe → usar `false` fixo nesta fase; migração para runtime-config fica para depois |
| `vshadowGoodFormatFbo / vshadowDepthGoodFormat / vshadowMomentBlur` | `vshadowGoodFormatFbo_ / vshadowDepthGoodFormat_ / vshadowMomentBlur_` |
| `eShadowFbo/eExpShadowDepth/eShadowDepthBlur` | `eShadowFbo_/eExpShadowDepth_/eShadowDepthBlur_` |
| `msmShadowFbo/msmShadowMoments/msmShadowMomentsBlur` | `msmShadowFbo_/msmShadowMoments_/msmShadowMomentsBlur_` |
| `deviceAnisotropy` | `deviceAnisotropy_` |
| `gAlbedo/gNormal/gRMA/gDepth/gfbo` | `gAlbedo_/gNormal_/gRMA_/gDepth_/gfbo_` |
| `WINDOW_WIDTH/WINDOW_HEIGHT` | `width_/height_` |
| `postprocessColor/postprocessPostSRGB/postprocessFbo/legitFinalImage` | `postprocessColor_/postprocessPostSRGB_/postprocessFbo_/legitFinalImage_` |
| `_countof(buffers)` | `std::size(buffers)` (+ `#include <iterator>`) |

Resto do corpo é idêntico ao original (todos os `glCreateTextures/glTextureStorage2D/…`). Copiar na íntegra depois do rename.

### 2.2.d — `destroyFramebuffers_` (inverso do `Cleanup` do `_orig_renderer`)

Fonte: linhas 922–978 do `_orig_renderer.cpp`. Copiar o corpo da `Cleanup()` **sem**:
- `ImGui_ImplOpenGL3_Shutdown/ImGui_ImplGlfw_Shutdown/ImGui::DestroyContext` (remover)
- `glfwDestroyWindow/glfwTerminate` (remover)

Aplicar o mesmo rename da tabela acima. Zerar cada handle após `glDelete*` para que double-call seja seguro:

```cpp
void Engine::destroyFramebuffers_() {
    if (volumetricsTex_)     { glDeleteTextures(1, &volumetricsTex_);     volumetricsTex_ = 0; }
    if (volumetricsTexBlur_) { glDeleteTextures(1, &volumetricsTexBlur_); volumetricsTexBlur_ = 0; }
    if (volumetricsFbo_)     { glDeleteFramebuffers(1, &volumetricsFbo_); volumetricsFbo_ = 0; }
    if (volumetricsAtrousTex_) { glDeleteTextures(1, &volumetricsAtrousTex_); volumetricsAtrousTex_ = 0; }
    if (volumetricsAtrousFbo_) { glDeleteFramebuffers(1, &volumetricsAtrousFbo_); volumetricsAtrousFbo_ = 0; }

    if (gAlbedo_) { glDeleteTextures(1, &gAlbedo_); gAlbedo_ = 0; }
    if (gNormal_) { glDeleteTextures(1, &gNormal_); gNormal_ = 0; }
    if (gDepth_)  { glDeleteTextures(1, &gDepth_);  gDepth_  = 0; }
    if (gRMA_)    { glDeleteTextures(1, &gRMA_);    gRMA_    = 0; }
    if (gfbo_)    { glDeleteFramebuffers(1, &gfbo_); gfbo_   = 0; }

    if (postprocessColor_)    { glDeleteTextures(1, &postprocessColor_);    postprocessColor_ = 0; }
    if (postprocessPostSRGB_) { glDeleteTextures(1, &postprocessPostSRGB_); postprocessPostSRGB_ = 0; }
    if (legitFinalImage_)     { glDeleteTextures(1, &legitFinalImage_);     legitFinalImage_ = 0; }
    if (postprocessFbo_)      { glDeleteFramebuffers(1, &postprocessFbo_);  postprocessFbo_ = 0; }

    if (ssrTex_)     { glDeleteTextures(1, &ssrTex_);     ssrTex_ = 0; }
    if (ssrTexBlur_) { glDeleteTextures(1, &ssrTexBlur_); ssrTexBlur_ = 0; }
    if (ssrFbo_)     { glDeleteFramebuffers(1, &ssrFbo_); ssrFbo_ = 0; }

    if (shadowDepth_) { glDeleteTextures(1, &shadowDepth_); shadowDepth_ = 0; }
    if (shadowFbo_)   { glDeleteFramebuffers(1, &shadowFbo_); shadowFbo_ = 0; }

    if (vshadowDepthGoodFormat_) { glDeleteTextures(1, &vshadowDepthGoodFormat_); vshadowDepthGoodFormat_ = 0; }
    if (vshadowMomentBlur_)      { glDeleteTextures(1, &vshadowMomentBlur_);      vshadowMomentBlur_ = 0; }
    if (vshadowGoodFormatFbo_)   { glDeleteFramebuffers(1, &vshadowGoodFormatFbo_); vshadowGoodFormatFbo_ = 0; }

    if (eExpShadowDepth_)  { glDeleteTextures(1, &eExpShadowDepth_);  eExpShadowDepth_  = 0; }
    if (eShadowDepthBlur_) { glDeleteTextures(1, &eShadowDepthBlur_); eShadowDepthBlur_ = 0; }
    if (eShadowFbo_)       { glDeleteFramebuffers(1, &eShadowFbo_);   eShadowFbo_ = 0; }

    if (msmShadowMoments_)     { glDeleteTextures(1, &msmShadowMoments_);     msmShadowMoments_ = 0; }
    if (msmShadowMomentsBlur_) { glDeleteTextures(1, &msmShadowMomentsBlur_); msmShadowMomentsBlur_ = 0; }
    if (msmShadowFbo_)         { glDeleteFramebuffers(1, &msmShadowFbo_);     msmShadowFbo_ = 0; }

    if (hdrColorTex_) { glDeleteTextures(1, &hdrColorTex_); hdrColorTex_ = 0; }
    if (hdrDepthTex_) { glDeleteTextures(1, &hdrDepthTex_); hdrDepthTex_ = 0; }
    if (hdrFbo_)      { glDeleteFramebuffers(1, &hdrFbo_);  hdrFbo_ = 0; }

    if (ssaoFbo_)     { glDeleteFramebuffers(1, &ssaoFbo_); ssaoFbo_ = 0; }
    if (ssaoTex_)     { glDeleteTextures(1, &ssaoTex_);     ssaoTex_ = 0; }
    if (ssaoBlurred_) { glDeleteTextures(1, &ssaoBlurred_); ssaoBlurred_ = 0; }
}

void Engine::Resize(int w, int h) {
    if (w == width_ && h == height_) return;
    width_  = w;
    height_ = h;
    destroyFramebuffers_();
    createFramebuffers_();
}
```

### 2.2.e — `createVAO_` (copiar de `_orig_renderer.cpp::CreateVAO`, linhas 892–910)

```cpp
void Engine::createVAO_() {
    glCreateVertexArrays(1, &vao_);
    glEnableVertexArrayAttrib(vao_, 0);
    glEnableVertexArrayAttrib(vao_, 1);
    glEnableVertexArrayAttrib(vao_, 2);
    glEnableVertexArrayAttrib(vao_, 3);
    glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
    glVertexArrayAttribFormat(vao_, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribFormat(vao_, 3, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, tangent));
    glVertexArrayAttribBinding(vao_, 0, 0);
    glVertexArrayAttribBinding(vao_, 1, 0);
    glVertexArrayAttribBinding(vao_, 2, 0);
    glVertexArrayAttribBinding(vao_, 3, 0);
}
```

(O original tinha `glEnableVertexArrayAttrib(vao, 4)` + `glVertexArrayAttribBinding(vao, 4, 0)` sem formato — morto. Removemos. Atributos 4/5 — bone_ids/weights — entram só na Fase 4 num VAO skinned separado.)

### 2.2.f — `LoadEnvironment` (adaptar de `_orig_renderer.cpp::LoadEnvironmentMap`, linhas 1501–1548)

Copiar o corpo tal-qual, mudando:
- Assinatura `LoadEnvironmentMap(std::string path)` → `LoadEnvironment(const std::string& hdr_path)`
- `envMap_hdri` → `envMap_hdri_`
- `irradianceMap` → `irradianceMap_`
- `convolve_image` vem de `helpers.h` — garantir o include

### 2.2.g — Static scene API (código novo)

```cpp
void Engine::BeginStaticScene() {
    vertexBuffer_->Reset();
    indexBuffer_->Reset();
    staticMeshes_.clear();
    static_scene_open_ = true;
}

StaticMeshHandle Engine::UploadStaticMesh(const std::vector<Vertex>& vertices,
                                          const std::vector<uint32_t>& indices,
                                          int material_index) {
    assert(static_scene_open_);
    StaticMeshRecord rec;
    rec.vtx_alloc = vertexBuffer_->Allocate(
        vertices.data(), vertices.size() * sizeof(Vertex));
    rec.idx_alloc = indexBuffer_->Allocate(
        indices.data(), indices.size() * sizeof(uint32_t));
    rec.material_index = material_index;
    StaticMeshHandle h{ staticMeshes_.size() };
    staticMeshes_.push_back(rec);
    return h;
}

void Engine::EndStaticScene() {
    assert(static_scene_open_);
    static_scene_open_ = false;

    // (re)build materialsBuffer from MaterialManager snapshot
    {
        auto mats = material_manager_.GetLinearMaterials();
        std::vector<BindlessMaterial> gpuMats;
        gpuMats.reserve(mats.size());
        for (const auto& [name, m] : mats) {
            gpuMats.push_back(BindlessMaterial{
                .albedoHandle           = m.albedoTex->GetBindlessHandle(),
                .roughnessHandle        = m.roughnessTex->GetBindlessHandle(),
                .metalnessHandle        = m.metalnessTex->GetBindlessHandle(),
                .normalHandle           = m.normalTex->GetBindlessHandle(),
                .ambientOcclusionHandle = m.ambientOcclusionTex->GetBindlessHandle(),
            });
        }
        materialsBuffer_ = std::make_unique<StaticBuffer>(
            gpuMats.data(), gpuMats.size() * sizeof(BindlessMaterial), 0);
    }

    // build drawIndirectBuffer
    std::vector<DrawElementsIndirectCommand> cmds;
    cmds.reserve(staticMeshes_.size());
    GLuint baseInstance = 0;
    for (const auto& rec : staticMeshes_) {
        const auto& vi = vertexBuffer_->GetAlloc(rec.vtx_alloc);
        const auto& ii = indexBuffer_->GetAlloc(rec.idx_alloc);
        cmds.push_back({
            .count         = static_cast<GLuint>(ii.size / sizeof(uint32_t)),
            .instanceCount = 1,
            .firstIndex    = static_cast<GLuint>(ii.offset / sizeof(uint32_t)),
            .baseVertex    = static_cast<GLuint>(vi.offset / sizeof(Vertex)),
            .baseInstance  = baseInstance++,
        });
    }
    drawIndirectBuffer_ = std::make_unique<StaticBuffer>(
        cmds.data(), cmds.size() * sizeof(DrawElementsIndirectCommand), 0);
}
```

> `DynamicBuffer::Reset()` **já existe** — confirmado na Fase 1.5.e. Se não existir com esse nome exato, usar o método equivalente que zera a lista de allocs.

**Sucesso do passo 2.2**: `engine.cpp` compila isoladamente (ainda sem `Pipeline`).

---

## Passo 2.3 — Criar `include/rco/renderer/pipeline.h`

Gerar `shared/renderer/include/rco/renderer/pipeline.h`:

```cpp
#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

#include "rco/renderer/light.h"

namespace rco::renderer {

class Engine;
class StaticBuffer;

struct FeatureConfig {
    bool ssao         = true;
    bool volumetrics  = true;
    bool ssr          = false;
    bool fxaa         = true;
    int  shadow_method = SHADOW_METHOD_ESM;
};

struct DynamicDrawRequest {
    GLuint    vao          = 0;   // 0 → use Engine's global VAO
    GLuint    vbo          = 0;
    GLuint    ebo          = 0;
    GLsizei   index_count  = 0;
    int       material_idx = 0;
    glm::mat4 model        = glm::mat4(1.0f);

    // optional skinning
    GLuint    bone_ssbo    = 0;
    int       bone_count   = 0;
};

class Pipeline {
public:
    explicit Pipeline(Engine& e);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void SetFeatures(const FeatureConfig& cfg);
    void SetSun(const glm::vec3& direction, const glm::vec3& color);

    // Per-frame
    void Begin(const glm::mat4& view,
               const glm::mat4& proj,
               const glm::vec3& cam_pos,
               float dt);

    void SubmitStaticScene();

    void SubmitDynamic(const DynamicDrawRequest& req);
    void SubmitSkinned(const DynamicDrawRequest& req);  // same struct, non-zero bone_ssbo

    void AddPointLight(const glm::vec3& pos, const glm::vec3& color, float radius);

    void End();

private:
    void computeLightMatrix_();

    void shadowPass_();
    void gBufferPass_();
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
    float         sunConstantC_ = 80.0f;     // ESM exponent
    float         vlightBleedFix_ = 0.9f;

    // Per-frame state (set in Begin)
    glm::mat4 view_     = glm::mat4(1.0f);
    glm::mat4 proj_     = glm::mat4(1.0f);
    glm::mat4 viewProj_ = glm::mat4(1.0f);
    glm::vec3 camPos_   = glm::vec3(0.0f);
    float     dt_       = 0.0f;

    glm::mat4 lightMat_ = glm::mat4(1.0f);

    // Point lights accumulated each frame (cleared on Begin)
    std::vector<PointLight>       localLights_;
    std::unique_ptr<StaticBuffer> lightSSBO_;

    // Dynamic submissions (non-static-scene): drawn after multi-draw indirect
    std::vector<DynamicDrawRequest> dynamicDraws_;
    std::vector<DynamicDrawRequest> skinnedDraws_;

    // Shadow filtered tex selected by method (set in shadowPass_)
    GLuint filteredShadowTex_ = 0;

    // Blur tuning — constants promoted from glRenderer (previously runtime-tweakable)
    int   blurPasses_   = 1;
    int   blurStrength_ = 5;

    // Volumetrics & SSAO a-trous tuning (promoted from glRenderer's VolumetricConfig/SSAOConfig)
    struct VolumetricTuning {
        GLint steps = 32;
        float intensity = 0.025f;
        float noiseOffset = 1.0f;
        float beerPower = 1.0f;
        float powderPower = 1.0f;
        float distanceScale = 1.0f;
        float heightOffset = 0.0f;
        float hfIntensity = 0.025f;
        int   atrous_passes = 1;
        float c_phi = 0.04f;
        float stepWidth = 1.0f;
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
        int   samples_near = 12;
        float delta = 0.001f;
        float range = 1.1f;
        float s = 1.8f;
        float k = 1.0f;
        int   atrous_passes = 3;
        float atrous_n_phi = 0.1f;
        float atrous_p_phi = 0.5f;
        float atrous_step_width = 1.0f;
        float atrous_kernel[5]  = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f };
        float atrous_offsets[5] = { -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };
    } ssao_{};

    struct SSRTuning {
        float rayStep = 0.15f;
        float minRayStep = 0.1f;
        float thickness = 0.0f;
        float searchDist = 15.0f;
        int   maxRaySteps = 30;
        int   binarySearchSteps = 5;
    } ssr_{};

    struct FXAATuning {
        float contrastThreshold = 0.0312f;
        float relativeThreshold = 0.125f;
        float pixelBlendStrength = 1.0f;
        float edgeBlendStrength = 1.0f;
    } fxaa_{};

    struct HDRTuning {
        float targetLuminance = 0.22f;
        float minExposure = 0.1f;
        float maxExposure = 100.0f;
        float exposureFactor = 1.0f;
        float adjustmentSpeed = 2.0f;
        int   numBuckets = 128;
    } hdr_{};

    int   numEnvSamples_ = 10;
};

} // namespace rco::renderer
```

**Sucesso**: header compila sozinho (friend declarado em `Engine`).

---

## Passo 2.4 — Criar `src/pipeline.cpp` — dividir `MainLoop` em passes

Fonte: `_orig_renderer.cpp::MainLoop` (linhas 72–684 de `glRenderer/Renderer.cpp`).

### 2.4.a — Esqueleto + `Begin`/`End` e boilerplate

```cpp
#include "rco/renderer/pipeline.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>

#include "rco/renderer/engine.h"
#include "rco/renderer/helpers.h"
#include "rco/renderer/indirect.h"
#include "rco/renderer/shader.h"

namespace rco::renderer {

Pipeline::Pipeline(Engine& e) : engine_(&e) {
    sun_.direction = glm::normalize(glm::vec3(1.0f, -0.5f, 0.0f));
    sun_.diffuse   = glm::vec3(1.0f);
}
Pipeline::~Pipeline() = default;

void Pipeline::SetFeatures(const FeatureConfig& cfg) { features_ = cfg; }
void Pipeline::SetSun(const glm::vec3& dir, const glm::vec3& col) {
    sun_.direction = glm::normalize(dir);
    sun_.diffuse   = col;
}

void Pipeline::Begin(const glm::mat4& view, const glm::mat4& proj,
                     const glm::vec3& cam_pos, float dt) {
    view_     = view;
    proj_     = proj;
    viewProj_ = proj * view;
    camPos_   = cam_pos;
    dt_       = dt;
    localLights_.clear();
    dynamicDraws_.clear();
    skinnedDraws_.clear();
}

void Pipeline::AddPointLight(const glm::vec3& pos, const glm::vec3& color, float radius) {
    PointLight p{};
    p.diffuse  = glm::vec4(color, 0.0f);
    p.position = glm::vec4(pos, 0.0f);
    p.linear   = 0.0f;
    p.quadratic= 1.0f / glm::max(0.001f, radius * radius);
    p.radiusSquared = radius * radius;
    localLights_.push_back(p);
}

void Pipeline::SubmitDynamic(const DynamicDrawRequest& r) { dynamicDraws_.push_back(r); }
void Pipeline::SubmitSkinned(const DynamicDrawRequest& r) { skinnedDraws_.push_back(r); }

// Placeholder — filled by client on its side; the base static-scene draw path uses the
// Engine's accumulated draw-indirect buffer and is invoked inside gBufferPass_ directly.
void Pipeline::SubmitStaticScene() {
    // no-op marker; the actual draw happens in gBufferPass_ which checks
    // engine_->drawIndirectBuffer_ != nullptr
}
```

### 2.4.b — `computeLightMatrix_` (linhas 163–165 do MainLoop)

```cpp
void Pipeline::computeLightMatrix_() {
    const glm::vec3 sunPos = -glm::normalize(sun_.direction) * 200.0f
                             + glm::vec3(0, 30, 0);
    lightMat_ = MakeLightMatrix(sun_, sunPos, glm::vec2(120.0f),
                                glm::vec2(1.0f, 350.0f));
}
```

### 2.4.c — `shadowPass_` (linhas 167–239)

Copiar o corpo dos dois blocos — "create shadow map pass" **e** o bloco de copy/blur logo após — colando dentro de `shadowPass_`:

Transformações obrigatórias:

| Original | Novo |
|---|---|
| `SHADOW_WIDTH/SHADOW_HEIGHT` | `engine_->shadowWidth_ / engine_->shadowHeight_` |
| `shadowFbo` | `engine_->shadowFbo_` |
| `batchedObjects` | loop sumiu (vide nota abaixo) |
| `drawIndirectBuffer` | `engine_->drawIndirectBuffer_` (pode ser `nullptr` — pular se for) |
| `vertexBuffer / indexBuffer` | `engine_->vertexBuffer_ / engine_->indexBuffer_` |
| `vao` | `engine_->vao_` |
| `shadowDepth` / `vshadowDepthGoodFormat` / `eExpShadowDepth` / `msmShadowMoments` | `engine_->shadowDepth_` / `engine_->vshadowDepthGoodFormat_` / `engine_->eExpShadowDepth_` / `engine_->msmShadowMoments_` |
| `vshadowGoodFormatFbo / eShadowFbo / msmShadowFbo` | `engine_->vshadowGoodFormatFbo_` / etc. |
| `shadow_method` | `features_.shadow_method` |
| `shadow_gen_mips` | `false` fixo |
| `BLUR_PASSES / BLUR_STRENGTH` | `blurPasses_ / blurStrength_` |
| `eConstant` | `sunConstantC_` |

**Nota sobre `batchedObjects`**: o shadow pass do original itera sobre cada mesh para montar uma matriz `lightMat * modelMatrix` por draw. Como agora **a lista de modelMatrices dos estáticos está embutida no `drawIndirectBuffer`** + material buffer, e cada `ObjectUniforms` carrega o `modelMatrix`, não precisamos mais do loop — o shader `shadowBindless` lê `bones[i]` ou `modelMatrices[i]` direto. Olhando o original com calma: ele de fato manda uma lista de `mat4` (uma por mesh) via SSBO binding 0. Precisamos manter isso. Solução:

```cpp
void Pipeline::shadowPass_() {
    if (!engine_->drawIndirectBuffer_) return;  // nothing to shadow

    glViewport(0, 0, engine_->shadowWidth_, engine_->shadowHeight_);
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->shadowFbo_);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    // One lightMat * modelMatrix per static mesh record
    std::vector<glm::mat4> uniforms;
    uniforms.reserve(engine_->staticMeshes_.size());
    // Static meshes currently have identity model matrix. If you later support
    // per-mesh model matrices, store them in StaticMeshRecord and multiply here.
    for (std::size_t i = 0; i < engine_->staticMeshes_.size(); ++i) {
        uniforms.emplace_back(lightMat_);
    }
    StaticBuffer uniformBuffer(uniforms.data(),
                               uniforms.size() * sizeof(glm::mat4), 0);
    auto& shadowBindlessShader = Shader::shaders["shadowBindless"];
    shadowBindlessShader->Bind();
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, uniformBuffer.ID());
    engine_->drawIndirectBuffer_->Bind(GL_DRAW_INDIRECT_BUFFER);
    glVertexArrayVertexBuffer(engine_->vao_, 0,
        engine_->vertexBuffer_->GetBufferHandle(), 0, sizeof(Vertex));
    glVertexArrayElementBuffer(engine_->vao_,
        engine_->indexBuffer_->GetBufferHandle());
    glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, 0,
        static_cast<GLsizei>(uniforms.size()),
        sizeof(DrawElementsIndirectCommand));

    // ---- shadow copy/blur (VSM/ESM/MSM) ----
    filteredShadowTex_ = 0;
    switch (features_.shadow_method) {
        case SHADOW_METHOD_VSM: filteredShadowTex_ = engine_->vshadowDepthGoodFormat_; break;
        case SHADOW_METHOD_ESM: filteredShadowTex_ = engine_->eExpShadowDepth_;        break;
        case SHADOW_METHOD_MSM: filteredShadowTex_ = engine_->msmShadowMoments_;       break;
        default: break;
    }
    if (features_.shadow_method == SHADOW_METHOD_VSM
     || features_.shadow_method == SHADOW_METHOD_ESM
     || features_.shadow_method == SHADOW_METHOD_MSM) {
        glBindTextureUnit(0, engine_->shadowDepth_);

        const char* shaderName = "none";
        GLuint fbo = 0;
        switch (features_.shadow_method) {
            case SHADOW_METHOD_VSM: shaderName = "vsm_copy"; fbo = engine_->vshadowGoodFormatFbo_; break;
            case SHADOW_METHOD_ESM: shaderName = "esm_copy"; fbo = engine_->eShadowFbo_;           break;
            case SHADOW_METHOD_MSM: shaderName = "msm_copy"; fbo = engine_->msmShadowFbo_;         break;
        }
        auto& copyShader = Shader::shaders[shaderName];
        copyShader->Bind();
        if (features_.shadow_method == SHADOW_METHOD_ESM) {
            copyShader->SetFloat("u_C", sunConstantC_);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        const GLuint W = engine_->shadowWidth_;
        const GLuint H = engine_->shadowHeight_;
        if (features_.shadow_method == SHADOW_METHOD_VSM) {
            blurTextureRG32f(engine_->vshadowDepthGoodFormat_, engine_->vshadowMomentBlur_,
                             W, H, blurPasses_, blurStrength_);
        } else if (features_.shadow_method == SHADOW_METHOD_ESM) {
            blurTextureR32f(engine_->eExpShadowDepth_, engine_->eShadowDepthBlur_,
                            W, H, blurPasses_, blurStrength_);
        } else if (features_.shadow_method == SHADOW_METHOD_MSM) {
            blurTextureRGBA32f(engine_->msmShadowMoments_, engine_->msmShadowMomentsBlur_,
                               W, H, blurPasses_, blurStrength_);
        }
    }
}
```

### 2.4.d — `gBufferPass_` (linhas 241–285 + draw de dynamics)

```cpp
void Pipeline::gBufferPass_() {
    glViewport(0, 0, engine_->width_, engine_->height_);
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->gfbo_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // ---- static scene (multi-draw indirect) ----
    if (engine_->drawIndirectBuffer_ && !engine_->staticMeshes_.empty()) {
        std::vector<ObjectUniforms> uniforms;
        uniforms.reserve(engine_->staticMeshes_.size());
        for (const auto& rec : engine_->staticMeshes_) {
            uniforms.push_back(ObjectUniforms{
                .modelMatrix   = glm::mat4(1.0f),    // identity (no per-mesh transform yet)
                .materialIndex = static_cast<uint32_t>(rec.material_index),
            });
        }
        StaticBuffer uniformBuffer(uniforms.data(),
                                   sizeof(ObjectUniforms) * uniforms.size(), 0);

        auto& sh = Shader::shaders["gBufferBindless"];
        sh->Bind();
        sh->SetMat4("u_viewProj", viewProj_);
        sh->SetBool ("u_materialOverride", false);
        sh->SetVec3 ("u_albedoOverride", glm::vec3(1.0f));
        sh->SetFloat("u_roughnessOverride", 0.5f);
        sh->SetFloat("u_metalnessOverride", 0.0f);
        sh->SetFloat("u_AOoverride", 0.0f);
        sh->SetFloat("u_ambientOcclusionOverride", 1.0f);
        uniformBuffer.BindBase(GL_SHADER_STORAGE_BUFFER, 0);
        engine_->materialsBuffer_->BindBase(GL_SHADER_STORAGE_BUFFER, 1);
        engine_->drawIndirectBuffer_->Bind(GL_DRAW_INDIRECT_BUFFER);
        glVertexArrayVertexBuffer(engine_->vao_, 0,
            engine_->vertexBuffer_->GetBufferHandle(), 0, sizeof(Vertex));
        glVertexArrayElementBuffer(engine_->vao_,
            engine_->indexBuffer_->GetBufferHandle());
        glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, 0,
            static_cast<GLsizei>(uniforms.size()),
            sizeof(DrawElementsIndirectCommand));
    }

    // ---- dynamic (non-skinned) draws ----
    //    Use the same gBufferBindless shader, but bind per-request VAO/EBO + model.
    if (!dynamicDraws_.empty()) {
        auto& sh = Shader::shaders["gBufferBindless"];
        sh->Bind();
        sh->SetMat4("u_viewProj", viewProj_);
        engine_->materialsBuffer_->BindBase(GL_SHADER_STORAGE_BUFFER, 1);
        for (const auto& r : dynamicDraws_) {
            ObjectUniforms u{ r.model, static_cast<uint32_t>(r.material_idx) };
            StaticBuffer ub(&u, sizeof(u), 0);
            ub.BindBase(GL_SHADER_STORAGE_BUFFER, 0);
            GLuint vao = r.vao ? r.vao : engine_->vao_;
            glBindVertexArray(vao);
            if (r.vbo) glVertexArrayVertexBuffer(vao, 0, r.vbo, 0, sizeof(Vertex));
            if (r.ebo) glVertexArrayElementBuffer(vao, r.ebo);
            glDrawElements(GL_TRIANGLES, r.index_count, GL_UNSIGNED_INT, nullptr);
        }
        glBindVertexArray(engine_->vao_);
    }

    // Skinned draws — require gBufferSkinned shader (introduced in Phase 4).
    // For Phase 2, if any skinned draws were submitted we fall back to the static shader
    // without skinning math (placeholder to keep the pipeline compiling).
    if (!skinnedDraws_.empty()) {
        auto& sh = Shader::shaders["gBufferBindless"];
        sh->Bind();
        sh->SetMat4("u_viewProj", viewProj_);
        engine_->materialsBuffer_->BindBase(GL_SHADER_STORAGE_BUFFER, 1);
        for (const auto& r : skinnedDraws_) {
            ObjectUniforms u{ r.model, static_cast<uint32_t>(r.material_idx) };
            StaticBuffer ub(&u, sizeof(u), 0);
            ub.BindBase(GL_SHADER_STORAGE_BUFFER, 0);
            GLuint vao = r.vao ? r.vao : engine_->vao_;
            glBindVertexArray(vao);
            if (r.vbo) glVertexArrayVertexBuffer(vao, 0, r.vbo, 0, sizeof(Vertex));
            if (r.ebo) glVertexArrayElementBuffer(vao, r.ebo);
            glDrawElements(GL_TRIANGLES, r.index_count, GL_UNSIGNED_INT, nullptr);
        }
        glBindVertexArray(engine_->vao_);
    }
}
```

> `DrawPbrSphereGrid` foi cortado de vez: era debug do glRenderer, não usamos.

### 2.4.e — `ssaoPass_` (linhas 287–340)

```cpp
void Pipeline::ssaoPass_() {
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->ssaoFbo_);
    float clearWhite[] = { 1, 1, 1, 1 };
    glClearNamedFramebufferfv(engine_->ssaoFbo_, GL_COLOR, 0, clearWhite);
    glBindTextureUnit(0, engine_->gDepth_);
    glBindTextureUnit(1, engine_->gNormal_);

    if (!features_.ssao) return;

    auto& ssaoShader = Shader::shaders["ssao"];
    ssaoShader->Bind();
    ssaoShader->SetMat4 ("u_invViewProj", glm::inverse(viewProj_));
    ssaoShader->SetMat4 ("u_view", view_);
    ssaoShader->SetUInt ("u_numSamples", ssao_.samples_near);
    ssaoShader->SetFloat("u_delta", ssao_.delta);
    ssaoShader->SetFloat("u_R", ssao_.range);
    ssaoShader->SetFloat("u_s", ssao_.s);
    ssaoShader->SetFloat("u_k", ssao_.k);
    glNamedFramebufferTexture(engine_->ssaoFbo_, GL_COLOR_ATTACHMENT0, engine_->ssaoTex_, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (ssao_.atrous_passes > 0) {
        glBindTextureUnit(1, engine_->gDepth_);
        glBindTextureUnit(2, engine_->gNormal_);
        auto& blur = Shader::shaders["atrous_ssao"];
        blur->Bind();
        blur->SetFloat("n_phi", ssao_.atrous_n_phi);
        blur->SetFloat("p_phi", ssao_.atrous_p_phi);
        blur->SetFloat("stepwidth", ssao_.atrous_step_width);
        blur->SetMat4 ("u_invViewProj", glm::inverse(viewProj_));
        blur->SetIVec2("u_resolution", engine_->width_, engine_->height_);
        blur->Set1FloatArray("kernel[0]", ssao_.atrous_kernel);
        blur->Set1FloatArray("offsets[0]", ssao_.atrous_offsets);
        for (int i = 0; i < ssao_.atrous_passes; ++i) {
            float offsets2[5];
            for (int j = 0; j < 5; ++j)
                offsets2[j] = ssao_.atrous_offsets[j] * glm::pow(2.0f, i);
            blur->Set1FloatArray("offsets[0]", offsets2);
            blur->SetBool("u_horizontal", false);
            glBindTextureUnit(0, engine_->ssaoTex_);
            glNamedFramebufferTexture(engine_->ssaoFbo_, GL_COLOR_ATTACHMENT0, engine_->ssaoBlurred_, 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            blur->SetBool("u_horizontal", true);
            glBindTextureUnit(0, engine_->ssaoBlurred_);
            glNamedFramebufferTexture(engine_->ssaoFbo_, GL_COLOR_ATTACHMENT0, engine_->ssaoTex_, 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
}
```

### 2.4.f — `globalLightPass_` (linhas 342–371)

```cpp
void Pipeline::globalLightPass_() {
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->hdrFbo_);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindTextureUnit(0, engine_->gNormal_);
    glBindTextureUnit(1, engine_->gAlbedo_);
    glBindTextureUnit(2, engine_->gRMA_);
    glBindTextureUnit(3, engine_->gDepth_);
    glBindTextureUnit(4, engine_->ssaoTex_);
    glBindTextureUnit(5, filteredShadowTex_);
    glBindTextureUnit(6, engine_->irradianceMap_);
    glBindTextureUnit(7, engine_->envMap_hdri_->GetID());

    glDisable(GL_DEPTH_TEST);
    auto& sh = Shader::shaders["gPhongGlobal"];
    sh->Bind();
    sh->SetInt  ("u_shadowMethod", features_.shadow_method);
    sh->SetFloat("u_C", sunConstantC_);
    sh->SetVec3 ("u_viewPos", camPos_);
    sh->SetIVec2("u_screenSize", engine_->width_, engine_->height_);
    sh->SetUInt ("u_samples", numEnvSamples_);
    sh->SetMat4 ("u_invViewProj", glm::inverse(viewProj_));
    sh->SetVec3 ("u_globalLight_diffuse",   sun_.diffuse);
    sh->SetVec3 ("u_globalLight_direction", sun_.direction);
    sh->SetMat4 ("u_lightMatrix", lightMat_);
    sh->SetFloat("u_lightBleedFix", vlightBleedFix_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}
```

### 2.4.g — `localLightsPass_` (linhas 373–394)

```cpp
void Pipeline::localLightsPass_() {
    if (localLights_.empty()) return;

    // Upload current frame's point lights
    lightSSBO_ = std::make_unique<StaticBuffer>(
        localLights_.data(), localLights_.size() * sizeof(PointLight), 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);

    auto& sh = Shader::shaders["gPhongManyLocal"];
    sh->Bind();
    sh->SetMat4("u_viewProj",     viewProj_);
    sh->SetMat4("u_invViewProj",  glm::inverse(viewProj_));
    sh->SetVec3("u_viewPos",      camPos_);
    sh->SetInt ("gNormal", 0);
    sh->SetInt ("gAlbedo", 1);
    sh->SetInt ("gRMA",    2);
    sh->SetInt ("gDepth",  3);
    lightSSBO_->BindBase(GL_SHADER_STORAGE_BUFFER, 0);

    // Light volume mesh — the glRenderer used a per-renderer `sphere` Mesh.
    // Phase 2 keeps an internal unit-sphere helper in helpers.h (see note below).
    GetUnitLightSphere().Draw(static_cast<GLsizei>(localLights_.size()));

    glCullFace(GL_BACK);
}
```

> **Dependência a adicionar em `helpers.h/.cpp`** nesta fase: uma função `GetUnitLightSphere()` que retorna uma `Mesh&` estática carregada do arquivo embutido `assets/models/light_sphere.obj` (pegar do glRenderer: `Resources/Models/goodSphere.obj`), carregado lazy na primeira chamada via `LoadObjMesh`. Isso substitui o membro `Renderer::sphere` sem exigir que o client conheça a mesh. Se preferir, essa helper pode só expor `.Draw(instanceCount)` que internamente emite `glDrawElementsInstanced`.

### 2.4.h — `skyboxPass_` (linhas 396–415)

```cpp
void Pipeline::skyboxPass_() {
    glBlitNamedFramebuffer(engine_->gfbo_, engine_->hdrFbo_,
        0, 0, engine_->width_, engine_->height_,
        0, 0, engine_->width_, engine_->height_,
        GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_ONE, GL_ZERO);

    auto& sh = Shader::shaders["hdri_skybox"];
    sh->Bind();
    sh->SetMat4 ("u_invViewProj", glm::inverse(viewProj_));
    sh->SetIVec2("u_screenSize", engine_->width_, engine_->height_);
    sh->SetVec3 ("u_camPos", camPos_);
    engine_->envMap_hdri_->Bind(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}
```

### 2.4.i — `volumetricPass_` (linhas 417–475)

Copiar o corpo, renomeando:
- `volumetrics.*` → `volumetric_.*` (tuning) **e** `engine_->volumetrics*_` (handles)
- `framebuffer_width/_height` (que eram const) → `engine_->width_ / engine_->height_`
- `gDepth/shadowDepth/bluenoiseTex` → `engine_->gDepth_ / engine_->shadowDepth_ / engine_->bluenoiseTex_->GetID()`
- `u_lightMatrix` usa `lightMat_`
- `u_invViewProj` usa `glm::inverse(viewProj_)`

Guard: `if (!features_.volumetrics) return;`

### 2.4.j — `ssrPass_` (linhas 476–505)

Copiar, renomear:
- `hdr.colorTex` → `engine_->hdrColorTex_`
- `gDepth/gRMA/gNormal` → `engine_->g*_`
- `cam.GetProj/GetView/GetViewProj` → `proj_ / view_ / viewProj_`
- `ssr.*` (tuning) → `ssr_.*`; `ssr.fbo/tex/framebuffer_*` → `engine_->ssr*_`

```cpp
void Pipeline::ssrPass_() {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glDepthMask(GL_FALSE);
    if (!features_.ssr) return;

    glViewport(0, 0, engine_->ssrWidth_, engine_->ssrHeight_);
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->ssrFbo_);
    glClear(GL_COLOR_BUFFER_BIT);
    auto& sh = Shader::shaders["ssr"];
    sh->Bind();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTextureUnit(0, engine_->hdrColorTex_);
    glBindTextureUnit(1, engine_->gDepth_);
    glBindTextureUnit(2, engine_->gRMA_);
    glBindTextureUnit(3, engine_->gNormal_);
    sh->SetMat4 ("u_proj", proj_);
    sh->SetMat4 ("u_view", view_);
    sh->SetMat4 ("u_invViewProj", glm::inverse(viewProj_));
    sh->SetFloat("rayStep", ssr_.rayStep);
    sh->SetFloat("minRayStep", ssr_.minRayStep);
    sh->SetFloat("thickness", ssr_.thickness);
    sh->SetFloat("searchDist", ssr_.searchDist);
    sh->SetInt  ("maxSteps", ssr_.maxRaySteps);
    sh->SetInt  ("binarySearchSteps", ssr_.binarySearchSteps);
    sh->SetIVec2("u_viewportSize", engine_->ssrWidth_, engine_->ssrHeight_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}
```

### 2.4.k — `compositePass_` (linhas 507–537)

```cpp
void Pipeline::compositePass_() {
    glViewport(0, 0, engine_->width_, engine_->height_);
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->postprocessFbo_);
    glBlendFunc(GL_ONE, GL_ONE);
    glClear(GL_COLOR_BUFFER_BIT);
    auto& fs = Shader::shaders["fstexture"];
    fs->Bind();
    fs->SetInt("u_texture", 0);
    glBindTextureUnit(0, engine_->hdrColorTex_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (features_.volumetrics) {
        GLuint volTex = (volumetric_.atrous_passes % 2 == 0)
            ? engine_->volumetricsTex_
            : engine_->volumetricsAtrousTex_;
        glBindTextureUnit(0, volTex);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    if (features_.ssr) {
        glBindTextureUnit(0, engine_->ssrTex_);
        glBlendFunc(GL_ONE, GL_ONE);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
}
```

### 2.4.l — `tonemappingPass_` (copiar `_orig_renderer.cpp::ApplyTonemapping`, linhas 1268–1324)

Transformações:

| Original | Novo |
|---|---|
| `hdr.*` (tuning) | `hdr_.*` |
| `hdr.histogramBuffer/exposureBuffer` | `engine_->histogramBuffer_/exposureBuffer_` |
| `postprocessColor/postprocessPostSRGB/postprocessFbo` | `engine_->postprocessColor_/postprocessPostSRGB_/postprocessFbo_` |
| `WINDOW_WIDTH/HEIGHT` | `engine_->width_/height_` |

Corpo igual, só com os renames.

### 2.4.m — `fxaaPass_` + `finalBlit_` (linhas 542–564)

```cpp
void Pipeline::fxaaPass_() {
    glBindFramebuffer(GL_FRAMEBUFFER, engine_->postprocessFbo_);
    glBlendFunc(GL_ONE, GL_ONE);
    glNamedFramebufferTexture(engine_->postprocessFbo_, GL_COLOR_ATTACHMENT0,
                              engine_->legitFinalImage_, 0);
    if (features_.fxaa) {
        glBindTextureUnit(0, engine_->postprocessPostSRGB_);
        auto& sh = Shader::shaders["fxaa"];
        sh->Bind();
        sh->SetVec2 ("u_invScreenSize",
                     1.0f / engine_->width_, 1.0f / engine_->height_);
        sh->SetFloat("u_contrastThreshold",   fxaa_.contrastThreshold);
        sh->SetFloat("u_relativeThreshold",   fxaa_.relativeThreshold);
        sh->SetFloat("u_pixelBlendStrength",  fxaa_.pixelBlendStrength);
        sh->SetFloat("u_edgeBlendStrength",   fxaa_.edgeBlendStrength);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    } else {
        drawFSTexture(engine_->postprocessPostSRGB_);
    }
    glNamedFramebufferTexture(engine_->postprocessFbo_, GL_COLOR_ATTACHMENT0,
                              engine_->postprocessColor_, 0);
}

void Pipeline::finalBlit_() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    drawFSTexture(engine_->legitFinalImage_);
}
```

### 2.4.n — `End` amarrando tudo

```cpp
void Pipeline::End() {
    // Per-frame GL state baseline (originally at lines 155-161)
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LEQUAL);
    glBindVertexArray(engine_->vao_);

    computeLightMatrix_();
    shadowPass_();
    gBufferPass_();
    ssaoPass_();
    globalLightPass_();
    localLightsPass_();
    skyboxPass_();
    volumetricPass_();
    ssrPass_();
    compositePass_();
    tonemappingPass_(dt_);
    fxaaPass_();
    finalBlit_();
}
```

**Sucesso do passo 2.4**: `pipeline.cpp` compila (ainda não integrado ao client — só a lib `rco_renderer`).

---

## Passo 2.5 — Remover arquivos temporários

Deletar (arquivos criados na Fase 1 como andaimes):

```
shared/renderer/src/_orig_renderer.h
shared/renderer/src/_orig_renderer.cpp
shared/renderer/src/_stub_camera.h
shared/renderer/src/_stub_input.h
```

Comando:

```bash
rm shared/renderer/src/_orig_renderer.h \
   shared/renderer/src/_orig_renderer.cpp \
   shared/renderer/src/_stub_camera.h \
   shared/renderer/src/_stub_input.h
```

> Os stubs `_stub_camera.h` e `_stub_input.h` existiam só para `_orig_renderer.cpp` compilar. `pipeline.cpp` recebe `view/proj/cam_pos` como parâmetros e nunca usa `Camera` ou `Input` — então os stubs saem junto.

**Sucesso**: os 4 arquivos não existem mais; `grep -r "_orig_renderer" shared/renderer/` retorna zero resultados.

---

## Passo 2.6 — Atualizar `shared/renderer/CMakeLists.txt`

Editar:

```cmake
cmake_minimum_required(VERSION 3.20)
project(rco_renderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(glad   CONFIG REQUIRED)
find_package(glm    CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(Stb    REQUIRED)

add_library(rco_renderer STATIC
    # Fase 1 — conversões
    src/light.cpp
    src/buffers.cpp
    src/texture.cpp
    src/material.cpp
    src/mesh.cpp
    src/shader.cpp
    src/helpers.cpp
    src/compile_shaders.cpp

    # Fase 2 — novos
    src/engine.cpp
    src/pipeline.cpp

    # Preservados (client ainda usa até a Fase 4 integrar Engine/Pipeline)
    src/shader_old.cpp
    src/model_old.cpp

    # STB impl
    src/stb_image_impl.cpp
)

target_include_directories(rco_renderer
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${Stb_INCLUDE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(rco_renderer
    PUBLIC  glad::glad glm::glm
    PRIVATE assimp::assimp
)
```

Mudanças em relação à Fase 1:
- **Removidos** `src/_orig_renderer.cpp` (tempo), os stubs de Camera/Input não eram compilados mesmo (só headers)
- **Adicionados** `src/engine.cpp` e `src/pipeline.cpp`

---

## Passo 2.7 — Validação (build + smoke)

```bash
cd D:/Github/RealmCrafterOrigins/client
cmake --build build --config Release
```

**Critérios de sucesso da Fase 2** (todos devem ser verdade):

- [ ] `cmake --build` compila `rco_renderer` com **0 erros**. Warnings aceitáveis.
- [ ] `rco_client.exe` ainda compila — ele continua usando `shader_old.cpp` + `model_old.cpp` + o caminho de renderização antigo. **Não** estamos trocando o client ainda; isso é Fase 4.
- [ ] `rco_client.exe` ainda roda. Visual idêntico ao que era antes da Fase 1.
- [ ] `grep -r "_orig_renderer\|_stub_camera\|_stub_input" shared/renderer/` não retorna nada.
- [ ] `grep -r "WINDOW_WIDTH\|WINDOW_HEIGHT" shared/renderer/src/engine.cpp shared/renderer/src/pipeline.cpp` não retorna nada (todos substituídos por `width_/height_`).
- [ ] `grep -r "glfw\|ImGui\|Input::" shared/renderer/src/engine.cpp shared/renderer/src/pipeline.cpp` não retorna nada.
- [ ] Arquivos novos/modificados:
  - `shared/renderer/include/rco/renderer/engine.h` (novo)
  - `shared/renderer/include/rco/renderer/pipeline.h` (novo)
  - `shared/renderer/src/engine.cpp` (novo)
  - `shared/renderer/src/pipeline.cpp` (novo)
  - `shared/renderer/src/helpers.cpp` (atualizado — `GetUnitLightSphere()` adicionada)
  - `shared/renderer/include/rco/renderer/helpers.h` (atualizado — declaração de `GetUnitLightSphere()`)
  - `shared/renderer/CMakeLists.txt` (atualizado)

**Smoke test mínimo** — para garantir que Engine/Pipeline ao menos linkam num binário real, criar um teste de sanidade `shared/renderer/tests/link_sanity.cpp` (opcional, mas recomendado):

```cpp
#include "rco/renderer/engine.h"
#include "rco/renderer/pipeline.h"

// Intentionally minimal — just verify symbol resolution at link time.
void rco_renderer_link_sanity() {
    rco::renderer::Engine e;
    rco::renderer::Pipeline p(e);
    (void)e; (void)p;
}
```

Adicioná-lo ao `target_sources` do `rco_renderer` garante erro de link imediato se algum método ficou sem definição.

Se algum critério falha, **não avançar para a Fase 3**. Corrigir primeiro.

---

## Problemas comuns e como resolver

**Erro: `undefined reference to Pipeline::...`**
→ Método declarado no header mas o corpo não foi colado. Rever o passo 2.4.

**Erro: `member "drawIndirectBuffer_" is private`** em `pipeline.cpp`
→ `friend class Pipeline;` não foi declarado dentro de `Engine`. Adicionar no bloco `private:` do `engine.h`.

**Erro: `no member named GetLinearMaterials`** em `engine.cpp`
→ `MaterialManager` no `material.h` da Fase 1 deve expor esse método (sinônimo do original do glRenderer). Se o nome ficou diferente na conversão, ajustar ali ou no caller.

**Erro: `DynamicBuffer::Reset` não existe**
→ A API do `DynamicBuffer` convertido na Fase 1.5.e tinha esse método. Se foi renomeado (ex.: `Clear`), ajustar em `Engine::BeginStaticScene`. Não adicionar método novo ao header sem rever a Fase 1.

**Erro: `GetUnitLightSphere` não declarado** em `pipeline.cpp`
→ Adicionar em `helpers.h` a declaração:
```cpp
class Mesh;
Mesh& GetUnitLightSphere();
```
e em `helpers.cpp` a definição:
```cpp
Mesh& GetUnitLightSphere() {
    static Mesh sphere = std::move(LoadObjMesh(
        "assets/models/light_sphere.obj",
        /* materialManager */ /* a MaterialManager global/dummy */)[0]);
    return sphere;
}
```
e copiar `glRenderer/Resources/Models/goodSphere.obj` para `dist/client/assets/models/light_sphere.obj`.
Se `LoadObjMesh` precisa de um `MaterialManager&`, usar uma instância estática no próprio `helpers.cpp` (a mesh é opaca; nunca vai ao shader com material).

**Erro: shadows quebradas / renderização só mostra céu**
→ `SubmitStaticScene` ainda não foi chamada pelo client, ou `BeginStaticScene/UploadStaticMesh/EndStaticScene` não rodou. Nesta fase isso é esperado: o client novo entra só na Fase 4.

**Visual do client mudou**
→ Não deveria. Se mudou, verificar se o `model_old.cpp/shader_old.cpp` ainda estão no build **e** se o caminho antigo do `rco_client` não foi alterado por engano.

**Aviso: `-Wunused-private-field`** em vários membros de `Pipeline`
→ Esperado (estruturas `SSRTuning`/`FXAATuning` etc. têm defaults e só alguns campos são lidos no momento). Suprimível ou ignorável.
