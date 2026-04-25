# Fase 5 detalhada — Integrar `Engine/Pipeline` nas tools

Tutorial auto-contido. Execute os passos **em ordem**, na íntegra, sem pular.
Cada passo tem critério de sucesso no final.

## Pré-requisitos

- Fases 1, 2, 3 e 4 completas.
- `rco_client.exe` roda com pipeline deferred novo.
- `rco_renderer.lib` expõe `Engine`, `Pipeline`, `TerrainChunkSubmission`, `DynamicDrawRequest`.
- `dist/client/shaders/` contém os 35 shaders do glRenderer + `terrainGBuffer.vs/.fs` (Fase 4.2) + `gBufferSkinned` variant registrada (Fase 4.3).
- `dist/client/assets/ibl/default.hdr` e `dist/client/assets/textures/bluenoise_64.png` presentes.
- **Decisão confirmada antes de começar**: GUE e terrain-editor precisam ler de `dist/client/shaders/`, `dist/client/assets/ibl/` e `dist/client/assets/textures/` em vez de terem cópias próprias. Como cada exe chdirs para sua própria pasta (`dist/tools/`), o caminho relativo é `../client/shaders/`, `../client/assets/ibl/`, etc. Isso é o padrão do projeto — confirmado em `CLAUDE.md`.

## Regras gerais (aplicam a TODA a Fase 5)

1. **Reutilizar shaders do client**: nenhuma tool ganha cópia própria de shader. Todas apontam para `../client/shaders/` via `EngineConfig::shader_dir = "../client/shaders/"`.
2. **Reutilizar HDRI do client**: `engine.LoadEnvironment("../client/assets/ibl/default.hdr");`
3. **Compartilhar ImTextureID**: tools que desenham 3D dentro de uma child-window ImGui (GUE, terrain-editor com preview) **NÃO** fazem `finalBlit` — leem `engine.finalImage()` e passam para `ImGui::Image()`. O framebuffer 0 fica reservado pro ImGui principal.
4. **Zero shaders inlineados**: qualquer `const char* vs = "..."` dentro de tool vira arquivo `.vs/.fs` em `../client/shaders/` **registrado em `compile_shaders.cpp`** e carregado pelo engine.
5. **Brush / overlay de editor** vai no **forward-pass callback** de `Pipeline::End(forward_pass)`, mesmo mecanismo que as particles do client usam na Fase 4.5.
6. **STB conflict**: `tools/terrain-editor/src/stb_impl.cpp` deve ser removido do build — o `rco_renderer` já provê `stb_image_impl.cpp`. Dupla definição de `STB_IMAGE_IMPLEMENTATION` quebra o link.

---

## Passo 5.0 — Expandir a API do `Pipeline` para renderização offscreen

Antes de tocar nas tools, precisamos de um overload do `End` que permita **não** blitar pra framebuffer 0 — essencial para tools que desenham dentro de uma `ImGui::Image()`.

### 5.0.a — `pipeline.h`: novo overload

Adicionar em `shared/renderer/include/rco/renderer/pipeline.h`:

```cpp
struct EndConfig {
    bool blit_to_default = true;              // if false, user reads engine.finalImage()
    std::function<void()> forward_pass = {};  // runs between composite and tonemap
};

void End();                                   // existing (Fase 2) — kept as-is
void End(const std::function<void()>& forward_pass);  // Fase 4.5 overload
void End(const EndConfig& cfg);               // Fase 5 overload — superset of above
```

### 5.0.b — `pipeline.cpp`: implementar

Substituir as implementações anteriores por:

```cpp
void Pipeline::End() { End(EndConfig{}); }

void Pipeline::End(const std::function<void()>& forward_pass) {
    EndConfig cfg{};
    cfg.forward_pass = forward_pass;
    End(cfg);
}

void Pipeline::End(const EndConfig& cfg) {
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
    terrainPass_();
    ssaoPass_();
    globalLightPass_();
    localLightsPass_();
    skyboxPass_();
    volumetricPass_();
    ssrPass_();
    compositePass_();

    if (cfg.forward_pass) {
        glBindFramebuffer(GL_FRAMEBUFFER, engine_->postprocessFbo_);
        glViewport(0, 0, engine_->width_, engine_->height_);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_LEQUAL);
        cfg.forward_pass();
        glDepthMask(GL_TRUE);
    }

    tonemappingPass_(dt_);
    fxaaPass_();
    if (cfg.blit_to_default) {
        finalBlit_();
    }
    // if !blit_to_default: caller reads engine.finalImage() as ImTextureID
}
```

**Sucesso do 5.0**:
- `rco_renderer` recompila.
- `rco_client.exe` continua rodando igual (default `EndConfig` preserva comportamento da Fase 4).

---

## Passo 5.1 — Terrain editor: migrar para `Engine + Pipeline`

**Objetivo**: `rco_terrain.exe` para de ter shaders inlineados e renderer próprio; passa a usar `Engine + Pipeline` com a mesma infra do client. Visual idêntico ao client no modo edição, com brush preview como overlay forward.

### 5.1.a — `tools/terrain-editor/CMakeLists.txt`

Reescrever:

```cmake
cmake_minimum_required(VERSION 3.20)
project(rco_terrain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Shared renderer — same lib the client uses
add_subdirectory(
    "${CMAKE_SOURCE_DIR}/../../shared/renderer"
    "${CMAKE_BINARY_DIR}/_rco_renderer"
)

# IMPORTANT: do NOT glob stb_impl.cpp — rco_renderer already defines STB_IMAGE_IMPLEMENTATION
set(TE_SOURCES
    src/main.cpp
    src/heightmap.cpp
    src/terrain_renderer.cpp
)
add_executable(rco_terrain ${TE_SOURCES})

find_package(glfw3  CONFIG REQUIRED)
find_package(glad   CONFIG REQUIRED)
find_package(imgui  CONFIG REQUIRED)
find_package(glm    CONFIG REQUIRED)
find_package(Stb    REQUIRED)

target_include_directories(rco_terrain PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    ${Stb_INCLUDE_DIR}
)

target_link_libraries(rco_terrain PRIVATE
    rco_renderer
    glfw
    glad::glad
    imgui::imgui
    glm::glm
)

if(WIN32)
    target_link_libraries(rco_terrain PRIVATE ws2_32)
endif()

set(RCO_TOOLS_DIST "${CMAKE_SOURCE_DIR}/../../dist/tools")
set_target_properties(rco_terrain PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY           "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY                "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${RCO_TOOLS_DIST}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${RCO_TOOLS_DIST}"
)
```

Mudanças-chave:
- Removido `file(GLOB_RECURSE)` (evita pegar `stb_impl.cpp`)
- Lista explícita de sources (deleta `stb_impl.cpp` do build)
- Adicionado `add_subdirectory` do `rco_renderer` (igual ao GUE)
- Adicionado `rco_renderer` no `target_link_libraries`

### 5.1.b — Deletar `tools/terrain-editor/src/stb_impl.cpp`

```bash
rm tools/terrain-editor/src/stb_impl.cpp
```

`rco_renderer` já traz `stb_image_impl.cpp` com `STB_IMAGE_IMPLEMENTATION`. Deixar os dois causa link error.

### 5.1.c — Remover shaders inlineados de `main.cpp`

Na `main.cpp` do terrain-editor, deletar:

- Todo o bloco `// Shaders — embedded` (linhas 53–267, ou onde começam os `const char*` com GLSL)
- A função `BuildShader()` inteira (linhas 269–285)
- Todas as chamadas `glUniform*(shader, ...)` no loop de render (aprox. linhas 491–514)
- A variável `GLuint shader = BuildShader();` (linha 406)
- `glUseProgram(shader);` e `glDeleteProgram(shader);`

O que sobra do loop de render: só o setup de entrada (heightmap/splatmap), UI ImGui, câmera. A parte 3D vai para o pipeline.

### 5.1.d — Adaptar `TerrainRenderer` para submissão pelo `Pipeline`

Editar `tools/terrain-editor/src/terrain_renderer.h`:

```cpp
#pragma once
#include "heightmap.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>

namespace rco::renderer { class Pipeline; struct TerrainChunkSubmission; }

class TerrainRenderer {
public:
    static constexpr int kCells = 64;
    static constexpr int kVerts = kCells + 1;

    struct Chunk {
        GLuint vao = 0, vbo = 0;
        int    cx = 0, cz = 0;
        bool   dirty = true;
    };

    void Init(const Heightmap& hmap);
    void Destroy();

    void MarkDirtyAll();
    void MarkDirtyRegion(float wx, float wz, float radius, const Heightmap& hmap);
    void Update(const Heightmap& hmap);

    // NEW: replaces Render(). Fills submission template with splatmap/materials
    // and pushes one entry per chunk into the pipeline.
    void Submit(rco::renderer::Pipeline& pipeline,
                GLuint splatmap_tex,
                const GLuint mat_albedo[4],
                const GLuint mat_normal[4],
                const GLuint mat_roughness[4],
                float tiling,
                glm::vec2 terrain_origin,
                glm::vec2 terrain_size) const;

    // Accessors needed so a Submit caller can iterate chunks directly if desired.
    int cx() const { return cx_; }
    int cz() const { return cz_; }

private:
    std::vector<Chunk> chunks_;
    GLuint             ebo_ = 0;
    int                idx_count_ = 0;
    int                cx_ = 0, cz_ = 0;

    void BuildChunk(Chunk& c, const Heightmap& hmap);
};
```

Editar `terrain_renderer.cpp`:

- Remover `Render()` e adicionar `Submit()` usando a assinatura de `rco::renderer::TerrainChunkSubmission` (definida na Fase 4.2.d):

```cpp
#include "rco/renderer/pipeline.h"   // TerrainChunkSubmission, Pipeline

void TerrainRenderer::Submit(rco::renderer::Pipeline& pipeline,
                             GLuint splatmap_tex,
                             const GLuint mat_albedo[4],
                             const GLuint mat_normal[4],
                             const GLuint mat_roughness[4],
                             float tiling,
                             glm::vec2 terrain_origin,
                             glm::vec2 terrain_size) const {
    rco::renderer::TerrainChunkSubmission base{};
    base.splatmap       = splatmap_tex;
    base.tiling         = tiling;
    base.terrain_origin = terrain_origin;
    base.terrain_size   = terrain_size;
    for (int i = 0; i < 4; ++i) {
        base.mat_albedo[i]    = mat_albedo[i];
        base.mat_normal[i]    = mat_normal[i];
        base.mat_roughness[i] = mat_roughness[i];
    }

    for (const Chunk& c : chunks_) {
        rco::renderer::TerrainChunkSubmission s = base;
        s.vao         = c.vao;
        s.vbo         = c.vbo;
        s.ebo         = ebo_;
        s.index_count = idx_count_;
        s.model       = glm::mat4(1.0f);   // editor has no per-chunk translation
        pipeline.SubmitTerrainChunk(s);
    }
}
```

> **Problema real** que aparece aqui: o `TerrainRenderer` do editor usa um VBO com **6 floats/vertex** (pos + normal), enquanto `terrainGBuffer.vs` espera o layout do `Vertex` padrão (pos + normal + uv + tangent = 11 floats). Precisamos alinhar o VBO. Opções:
>
> - **Opção A**: `BuildChunk` passa a gerar 11 floats/vertex com `uv = {gx * cs / tiling, gz * cs / tiling}` e `tangent = {1, 0, 0}`. VAO passa a usar 4 atributos. Custo: mais banda, mas o shader precisa disso.
> - **Opção B**: criar um `terrainGBuffer_editor.vs` que lê só pos + normal e calcula UV/tangent a partir de `gl_Position`. Mais código, mas VBO menor.
>
> **Decisão**: **Opção A**. O editor já é menos crítico em performance; simplicidade vence.

Ajustes em `BuildChunk` (adicionar `uv` e `tangent`):

```cpp
void TerrainRenderer::BuildChunk(Chunk& c, const Heightmap& hmap) {
    const int   baseX = c.cx * CC;
    const int   baseZ = c.cz * CC;
    const float cs    = hmap.cell_size;

    // 11 floats per vertex: pos(3) + normal(3) + uv(2) + tangent(3)
    std::vector<float> verts;
    verts.reserve(CV * CV * 11);

    for (int z = 0; z < CV; z++) {
        for (int x = 0; x < CV; x++) {
            int   gx = baseX + x;
            int   gz = baseZ + z;
            float h  = hmap.Get(gx, gz);

            float hl = hmap.Get(gx - 1, gz);
            float hr = hmap.Get(gx + 1, gz);
            float hd = hmap.Get(gx, gz - 1);
            float hu = hmap.Get(gx, gz + 1);
            glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.f * cs, hd - hu));

            // world-space UV — caller will apply tiling in shader anyway
            float u = gx * cs;
            float v = gz * cs;

            // pos
            verts.push_back(gx * cs);  verts.push_back(h);      verts.push_back(gz * cs);
            // normal
            verts.push_back(n.x);      verts.push_back(n.y);    verts.push_back(n.z);
            // uv
            verts.push_back(u);        verts.push_back(v);
            // tangent (flat — triplanar will dominate)
            verts.push_back(1.f);      verts.push_back(0.f);    verts.push_back(0.f);
        }
    }

    glBindVertexArray(c.vao);
    glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, verts.size() * sizeof(float), verts.data());
    glBindVertexArray(0);

    c.dirty = false;
}
```

Ajustes em `Init` para o VAO e o `glBufferData`:

```cpp
glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
glBufferData(GL_ARRAY_BUFFER,
    CV * CV * 11 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

// layout(location=0) vec3 aPos
glEnableVertexAttribArray(0);
glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
    11 * sizeof(float), (void*)0);
// layout(location=1) vec3 aNormal
glEnableVertexAttribArray(1);
glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
    11 * sizeof(float), (void*)(3 * sizeof(float)));
// layout(location=2) vec2 aUV
glEnableVertexAttribArray(2);
glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE,
    11 * sizeof(float), (void*)(6 * sizeof(float)));
// layout(location=3) vec3 aTangent
glEnableVertexAttribArray(3);
glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE,
    11 * sizeof(float), (void*)(8 * sizeof(float)));
```

### 5.1.e — Criar brush-overlay shader como forward pass

O terrain-editor atual desenha um anel sob o cursor escalado pelo brush radius. No código antigo isso era uniform no shader de terreno (`uBrushPos`, `uBrushRadius`, `uBrushActive`). Agora vai como forward pass depois do deferred.

Criar `dist/client/shaders/brush_overlay.vs`:

```glsl
#version 460
layout(location = 0) in vec3 a_position;

uniform mat4 u_viewProj;
uniform vec3 u_brushPos;
uniform float u_brushRadius;

out vec3 v_worldPos;

void main() {
    // a_position.xz is unit circle (-1..1); y is 0.
    vec3 wp = vec3(u_brushPos.x + a_position.x * u_brushRadius,
                   u_brushPos.y + 0.02,     // slight lift to avoid z-fighting
                   u_brushPos.z + a_position.z * u_brushRadius);
    v_worldPos = wp;
    gl_Position = u_viewProj * vec4(wp, 1.0);
}
```

Criar `dist/client/shaders/brush_overlay.fs`:

```glsl
#version 460
in vec3 v_worldPos;
uniform vec4 u_color;
out vec4 fragColor;
void main() {
    fragColor = u_color;
}
```

Registrar em `shared/renderer/src/compile_shaders.cpp`:

```cpp
Shader::shaders["brush_overlay"].emplace(Shader({
    { "brush_overlay.vs", GL_VERTEX_SHADER,   "" },
    { "brush_overlay.fs", GL_FRAGMENT_SHADER, "" }
}));
```

Geometria (unit ring) — criar em `main.cpp` do terrain-editor uma função helper:

```cpp
struct RingGeometry {
    GLuint vao = 0, vbo = 0;
    int vertex_count = 0;

    void Init(int segments = 64) {
        std::vector<float> pts;
        pts.reserve(segments * 3);
        for (int i = 0; i < segments; ++i) {
            float a = (float)i / (segments - 1) * 6.2831853f;
            pts.push_back(std::cos(a));
            pts.push_back(0.f);
            pts.push_back(std::sin(a));
        }
        vertex_count = segments;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, pts.size() * 4, pts.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
        glBindVertexArray(0);
    }

    void Draw() const {
        glBindVertexArray(vao);
        glDrawArrays(GL_LINE_LOOP, 0, vertex_count);
        glBindVertexArray(0);
    }

    void Destroy() {
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo) glDeleteBuffers(1, &vbo);
        vao = vbo = 0;
    }
};
```

### 5.1.f — Reescrever o loop de render do `main.cpp`

Substituir todo o bloco que antes fazia `glUseProgram(shader); glUniformMatrix4fv(...); terrain.Render();` por:

```cpp
// One-time init (right after window + GL loaded)
rco::renderer::Engine engine;
rco::renderer::EngineConfig ecfg{};
ecfg.width      = winW;
ecfg.height     = winH;
ecfg.shader_dir = "../client/shaders/";
engine.Init(ecfg);
engine.LoadEnvironment("../client/assets/ibl/default.hdr");

auto pipeline = std::make_unique<rco::renderer::Pipeline>(engine);

RingGeometry brush_ring;
brush_ring.Init(64);

// Per frame (inside main loop)
while (!glfwWindowShouldClose(win)) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // ... UI windows (brush panel, material panel, save/load) as before ...

    // ... brush sculpt / paint logic (unchanged: writes to hmap + splatmap) ...

    terrain.Update(hmap);   // only rebuilds dirty chunks

    // ---- 3D frame via Engine+Pipeline ----
    glm::mat4 view = g_cam.GetView();
    glm::mat4 proj = g_cam.GetProj((float)winW / (float)winH);
    glm::vec3 sunDir = glm::normalize(glm::vec3(0.4f, 1.0f, 0.25f));

    pipeline->Begin(view, proj, g_cam.pos, dt);
    pipeline->SetSun(-sunDir, glm::vec3(1.0f, 0.95f, 0.80f));

    // Build per-material texture arrays from the editor's material list.
    GLuint mat_alb[4]={0,0,0,0}, mat_nor[4]={0,0,0,0}, mat_rou[4]={0,0,0,0};
    for (int i = 0; i < (int)materials.size() && i < 4; ++i) {
        mat_alb[i] = materials[i].albedo_tex;
        mat_nor[i] = materials[i].normal_tex;
        mat_rou[i] = materials[i].roughness_tex;
    }
    terrain.Submit(*pipeline, splatmap_tex,
                   mat_alb, mat_nor, mat_rou,
                   /*tiling*/ 4.0f,
                   glm::vec2(0.0f),
                   glm::vec2(hmap.WorldW(), hmap.WorldH()));

    // Brush overlay — forward pass (rendered with deferred depth already in place)
    bool brushOnTerrain = /* existing hit test */ true;
    glm::vec3 brushPos = /* existing: raycast cursor → terrain */;
    pipeline->End({
        .blit_to_default = true,            // write the final image to the window
        .forward_pass = [&]() {
            if (!brushOnTerrain) return;
            auto& sh = rco::renderer::Shader::shaders["brush_overlay"];
            sh->Bind();
            sh->SetMat4 ("u_viewProj",    proj * view);
            sh->SetVec3 ("u_brushPos",    brushPos);
            sh->SetFloat("u_brushRadius", g_brushRadius);
            sh->SetVec4 ("u_color",       1.0f, 0.4f, 0.0f, 0.9f);
            glLineWidth(2.0f);
            brush_ring.Draw();
        },
    });

    // ImGui — rendered on top of the blit (framebuffer 0 already has the scene)
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(win);
}

// Shutdown
brush_ring.Destroy();
pipeline.reset();
engine.Shutdown();
terrain.Destroy();
```

### 5.1.g — Splatmap texture format

O `terrain-editor` hoje aloca o splatmap via `splatmap.h`. Confirmar que está em formato **RGBA8** e cada canal = peso do material 0/1/2/3 (a soma nem precisa bater 1 — o shader normaliza). Se estiver em outro formato, ajustar a alocação.

**Sucesso do 5.1**:
- `rco_terrain.exe` compila (build-terrain-editor.bat). Sem `stb_impl.cpp` não tem mais unresolved/duplicate em STB.
- Abrir o terrain-editor: janela abre, terrain aparece com PBR + shadows + SSAO (visual igual ao client InGame).
- Mover cursor sobre o terrain: anel laranja aparece acompanhando o cursor, respeitando depth test (oculto atrás de colinas).
- Sculpt (Raise/Lower/Smooth/Flatten): terreno deforma em tempo real, chunks dirty rebuildam, visual atualiza.
- Paint: pintar splatmap muda a cor do terreno no shader deferred.
- Save/Load dos `.bin` continua funcionando sem regressão.
- Nenhum link error tipo "multiple definition of stbi_load".

---

## Passo 5.2 — GUE preview: migrar de forward-shader antigo para `Engine + Pipeline`

**Objetivo**: `preview_viewport.{h,cpp}` para de usar `actor.vert/.frag` (que foi deletado na Fase 4.6) e passa a renderizar via `Engine + Pipeline` num FBO offscreen lido como `ImTextureID`.

> O `preview_viewport.{h,cpp}` atualmente está untracked no git e já tem uma primeira versão que **não vai mais compilar** depois da Fase 4.6 porque `actor.vert/.frag` somem. Esta fase é obrigatória, não opcional.

### 5.2.a — Adicionar uma instância `Engine` compartilhada no GUE

Em `tools/gue/src/main.cpp`, depois de `gladLoadGL()`, criar o Engine uma única vez (antes do loop):

```cpp
#include "rco/renderer/engine.h"
#include "rco/renderer/pipeline.h"

rco::renderer::Engine engine;
{
    rco::renderer::EngineConfig ecfg{};
    ecfg.width      = 1280;                       // any; Resize() below adjusts to preview size
    ecfg.height     = 720;
    ecfg.shader_dir = "../client/shaders/";
    engine.Init(ecfg);
    engine.LoadEnvironment("../client/assets/ibl/default.hdr");
}
auto pipeline = std::make_unique<rco::renderer::Pipeline>(engine);
```

No shutdown:

```cpp
pipeline.reset();
engine.Shutdown();
```

Passar `engine` + `pipeline` para a `PreviewViewport` (via pointers ou referências).

### 5.2.b — Reescrever `preview_viewport.h`

```cpp
#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <rco/renderer/model.h>
#include <string>
#include <array>

namespace rco::renderer { class Engine; class Pipeline; }

namespace gue {

class PreviewViewport {
public:
    ~PreviewViewport();

    // engine + pipeline are created by the GUE main; PreviewViewport borrows them.
    void Init(rco::renderer::Engine*   engine,
              rco::renderer::Pipeline* pipeline);

    bool LoadModel(const std::string& path);

    void OverrideMaterial(const std::string& albedo,
                          const std::string& normal,
                          const std::string& orm,
                          float albedo_r, float albedo_g, float albedo_b,
                          float roughness, float metallic);

    void DrawImGui();
    void Clear();
    const std::string& CurrentPath() const { return current_path_; }

private:
    void RenderToEngineFrame(int w, int h);
    void UploadBones_();

    rco::renderer::Engine*   engine_   = nullptr;
    rco::renderer::Pipeline* pipeline_ = nullptr;

    rco::renderer::Model  model_;
    GLuint                bone_ssbo_ = 0;
    std::string           current_path_;

    // Camera state (spherical around model)
    float cam_yaw_   = 0.f;
    float cam_pitch_ = -15.f;
    float cam_dist_  = 2.5f;
    glm::vec3 cam_target_{0.f, 1.f, 0.f};

    std::array<glm::mat4, rco::renderer::kMaxBones> bones_;

    // Animation state
    int   clip_idx_  = -1;
    float anim_t_    = 0.f;
    bool  playing_   = true;
};

} // namespace gue
```

Removidos: `Shader shader_`, `shader_ok_`, `fbo_`, `color_`, `depth_rb_`, `fbo_w_`, `fbo_h_`, `EnsureFbo()`, `wireframe_`, `show_grid_`.

> O engine **já gerencia o FBO interno** (`legitFinalImage_` é o output). Não precisamos de um FBO próprio no preview. Só precisamos chamar `engine_->Resize(w, h)` antes de renderizar com o tamanho do painel e ler `engine_->finalImage()`.

### 5.2.c — Reescrever `preview_viewport.cpp`

```cpp
#include "preview_viewport.h"

#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <filesystem>

#include "rco/renderer/engine.h"
#include "rco/renderer/pipeline.h"

namespace gue {

static std::string ResolveClientAsset(const std::string& p) {
    if (p.empty()) return p;
    std::filesystem::path path(p);
    if (path.is_absolute()) return p;
    if (p.rfind("../", 0) == 0 || p.rfind("./", 0) == 0) return p;
    return std::string("../client/") + p;
}

PreviewViewport::~PreviewViewport() {
    if (bone_ssbo_) glDeleteBuffers(1, &bone_ssbo_);
    model_.Destroy();
}

void PreviewViewport::Init(rco::renderer::Engine*   engine,
                            rco::renderer::Pipeline* pipeline) {
    engine_   = engine;
    pipeline_ = pipeline;
    bones_.fill(glm::mat4(1.f));

    glCreateBuffers(1, &bone_ssbo_);
    glNamedBufferData(bone_ssbo_,
        sizeof(glm::mat4) * rco::renderer::kMaxBones,
        nullptr, GL_DYNAMIC_DRAW);
}

void PreviewViewport::Clear() {
    model_.Destroy();
    current_path_.clear();
    clip_idx_ = -1;
    anim_t_   = 0.f;
}

bool PreviewViewport::LoadModel(const std::string& path) {
    if (path == current_path_) return true;

    model_.Destroy();
    current_path_ = path;
    clip_idx_ = -1;
    anim_t_   = 0.f;

    if (path.empty()) return true;

    std::string resolved = ResolveClientAsset(path);
    bool ok = model_.Load(resolved.c_str());
    if (ok && model_.HasAnimations() && model_.ClipCount() > 0) {
        clip_idx_ = 0;
    }
    return ok;
}

void PreviewViewport::OverrideMaterial(const std::string& albedo,
                                       const std::string& normal,
                                       const std::string& orm,
                                       float ar, float ag, float ab,
                                       float roughness, float metallic) {
    if (!model_.IsLoaded()) return;
    model_.OverrideMaterial(
        ResolveClientAsset(albedo),
        ResolveClientAsset(normal),
        ResolveClientAsset(orm),
        ar, ag, ab, roughness, metallic);
}

void PreviewViewport::UploadBones_() {
    if (!model_.IsLoaded()) return;

    if (clip_idx_ >= 0) {
        float dur = model_.ClipDuration(clip_idx_);
        if (playing_) anim_t_ = std::fmod(anim_t_, std::max(dur, 1e-3f));
        model_.ComputeBones(clip_idx_, anim_t_,
                            bones_.data(), rco::renderer::kMaxBones);
    } else {
        bones_.fill(glm::mat4(1.f));   // bind pose
    }
    glNamedBufferSubData(bone_ssbo_, 0,
        sizeof(glm::mat4) * rco::renderer::kMaxBones,
        bones_.data());
}

void PreviewViewport::RenderToEngineFrame(int w, int h) {
    if (!engine_ || !pipeline_) return;
    if (w <= 0 || h <= 0) return;

    engine_->Resize(w, h);   // idempotent when size unchanged

    // Camera from spherical coords
    float yaw   = glm::radians(cam_yaw_);
    float pitch = glm::radians(cam_pitch_);
    glm::vec3 offset = {
        cam_dist_ * std::cos(pitch) * std::sin(yaw),
        cam_dist_ * std::sin(pitch),
        cam_dist_ * std::cos(pitch) * std::cos(yaw),
    };
    glm::vec3 eye  = cam_target_ + offset;
    glm::mat4 view = glm::lookAt(eye, cam_target_, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(55.0f),
                                      (float)w / (float)h, 0.05f, 200.0f);

    // ImGui provides the frame time — approximate dt here
    float dt = ImGui::GetIO().DeltaTime;
    if (playing_) anim_t_ += dt;

    pipeline_->Begin(view, proj, eye, dt);
    pipeline_->SetSun(glm::vec3(-0.4f, -1.0f, -0.3f), glm::vec3(1.0f, 0.95f, 0.80f));

    if (model_.IsLoaded()) {
        UploadBones_();
        const auto& meshes = model_.meshes();    // see Actor migration notes in Fase 4.4.a
        if (!meshes.empty()) {
            const auto& m = meshes.front();

            rco::renderer::DynamicDrawRequest req{};
            req.vao          = m.vao;
            req.vbo          = m.vbo;
            req.ebo          = m.ebo;
            req.index_count  = m.idx_count;
            req.material_idx = 0;
            req.model        = glm::mat4(1.0f);
            req.bone_ssbo    = bone_ssbo_;
            req.bone_count   = rco::renderer::kMaxBones;
            pipeline_->SubmitSkinned(req);
        }
    }

    pipeline_->End({ .blit_to_default = false });   // keep default FBO intact for ImGui
}

void PreviewViewport::DrawImGui() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int w = std::max(16, (int)avail.x);
    int h = std::max(16, (int)avail.y);

    // Pre-record the GL state ImGui expects; our Render() changes viewport + FBO.
    GLint prev_fbo; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_vp[4]; glGetIntegerv(GL_VIEWPORT, prev_vp);

    RenderToEngineFrame(w, h);

    // Restore state — mandatory before ImGui's next draw call
    glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);

    // Display engine's final image inside the ImGui panel
    ImTextureID tex = (ImTextureID)(intptr_t)engine_->finalImage();
    // UV flip: OpenGL textures are bottom-up, ImGui expects top-down
    ImGui::Image(tex, ImVec2((float)w, (float)h),
                 ImVec2(0, 1), ImVec2(1, 0));

    // Orbit camera via left-drag inside the viewport rectangle
    if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
        cam_yaw_   -= d.x * 0.3f;
        cam_pitch_ += d.y * 0.3f;
        cam_pitch_  = std::clamp(cam_pitch_, -85.f, 85.f);
    }
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f) {
            cam_dist_ *= (wheel > 0.f) ? 0.9f : 1.1f;
            cam_dist_  = std::clamp(cam_dist_, 0.3f, 30.f);
        }
    }

    // Anim controls
    if (model_.IsLoaded() && model_.ClipCount() > 0) {
        ImGui::Checkbox("Play", &playing_);
        ImGui::SameLine();
        if (ImGui::BeginCombo("Clip",
            (clip_idx_ >= 0) ? model_.ClipName(clip_idx_).c_str() : "(none)")) {
            for (int i = 0; i < model_.ClipCount(); ++i) {
                bool sel = (i == clip_idx_);
                if (ImGui::Selectable(model_.ClipName(i).c_str(), sel)) {
                    clip_idx_ = i;
                    anim_t_   = 0.f;
                }
            }
            ImGui::EndCombo();
        }
    }
}

} // namespace gue
```

### 5.2.d — Atualizar callers no `main.cpp` do GUE

Substituir `pv.Init("shaders")` por `pv.Init(&engine, pipeline.get())`.

### 5.2.e — Observação sobre `Engine::Resize` custo

`engine_->Resize(w, h)` destrói e recria todos os FBOs. Isso é caro. A `PreviewViewport::DrawImGui` só chama `Resize` quando `w/h` muda (o próprio `Resize` tem guarda no passo 2.2.b: `if (w == width_ && h == height_) return;`).

Se o user redimensionar o painel ImGui continuamente arrastando o splitter, haverá N frames com recriação de FBO → FPS cai. Aceitável no editor; se for problema, adicionar debounce (só aplica Resize se manteve o tamanho por N frames).

### 5.2.f — Caveat: `Engine` único compartilhado com o ImGui do GUE

Como o GUE já usa ImGui para toda sua UI, **o Engine não pode** tocar nos estados de ImGui_ImplOpenGL3_RenderDrawData. Como o Engine foi feito na Fase 2 sem nada de ImGui, isso já está garantido. Mas reforçar nos problemas comuns.

**Sucesso do 5.2**:
- `rco_gue.exe` compila.
- Abrir a aba Media → Actor Defs → selecionar uma definição de actor com um mesh carregado.
- Painel 3D no lado direito mostra o modelo com PBR + shadows + HDRI reflection.
- Orbit com left-drag, zoom com scroll.
- Se o actor def tem clips, combobox de clip animação funcionando (play/pause, troca de clip).
- ImGui principal continua renderizando normalmente (nenhum glitch no resto da UI).

---

## Passo 5.3 — Validação final da Fase 5

**Critérios de sucesso da Fase 5** (todos devem ser verdade):

- [ ] `rco_terrain.exe` compila sem erros e roda.
- [ ] `rco_gue.exe` compila sem erros e roda.
- [ ] Visual do terrain-editor é **igual** ao visual do client InGame para o mesmo heightmap+splatmap (mesmo engine, mesmo shader).
- [ ] Brush overlay do terrain-editor desenha ring sobre o terreno respeitando profundidade.
- [ ] Sculpt/paint do terrain-editor funciona em tempo real (chunks dirty rebuildam, pipeline re-renderiza).
- [ ] GUE preview 3D mostra actor com PBR e animação, dentro de um painel ImGui.
- [ ] Nenhum shader inlineado em nenhuma tool.
- [ ] Nenhuma tool linka `stb_impl.cpp` próprio — todas usam o `stb_image_impl.cpp` do `rco_renderer`.
- [ ] `grep -r "actor.vert\|actor.frag\|skybox.vert\|skybox.frag" tools/` não retorna nada.
- [ ] Fechar cada exe — nenhum crash no shutdown.
- [ ] Arquivos deletados:
  - `tools/terrain-editor/src/stb_impl.cpp`

---

## Problemas comuns e como resolver

**Erro de link: `stbi_load` já definido em ambos `stb_image_impl.cpp` e `stb_impl.cpp`**
→ O `rco_terrain.exe` está buildando `tools/terrain-editor/src/stb_impl.cpp` além do `stb_image_impl.cpp` do `rco_renderer`. Deletar `stb_impl.cpp` do disco **E** do CMakeLists. O `GLOB_RECURSE` antigo precisa ter saído conforme o 5.1.a.

**Preview do GUE aparece preto**
→ Três causas prováveis:
1. `engine_->finalImage()` é 0 porque `engine.Init` falhou silenciosamente. Conferir logs — `installDebugCallback_` deve imprimir algo se IBL load falhou.
2. `engine_->Resize(w, h)` não foi chamado antes do primeiro `pipeline->End()`, então framebuffers têm 0×0. `DrawImGui` já garante isso — conferir que não foi removido.
3. UV do `ImGui::Image` invertido (mostra a parte de trás da textura, que foi clear). Garantir `ImVec2(0, 1), ImVec2(1, 0)` na `ImGui::Image`.

**ImGui do GUE quebra após abrir o preview (janelas borram, texto some)**
→ A Pipeline deixou algum estado GL em mau jeito. A sequência no `DrawImGui` de salvar `prev_fbo` + `prev_vp` e restaurar **antes** do `ImGui::Image` é obrigatória. Adicionalmente, restaurar `glBindVertexArray(0)` e `glUseProgram(0)` se o ImGui reclamar.

**Terrain editor: terrain aparece em cor sólida, sem blend de materiais**
→ `splatmap_tex` é 0 ou o formato não é RGBA8. Debugar imprimindo o ID da textura. Verificar o shader `terrainGBuffer.fs` — `texture(u_splatmap, uvT)` precisa retornar pesos em [0,1]. Se a sua splatmap foi criada como R8 ou RG8, só o primeiro material vai aparecer.

**Terrain editor: brush ring desenha mas fica atrás do terreno mesmo quando o cursor está sobre ele**
→ A forward pass do `brush_overlay` está rodando com `glDepthFunc(GL_LEQUAL)` mas a geometria do ring tem y = `brushPos.y + 0.02`. Se o heightmap tem escala grande (cell_size * algo), 0.02 pode ser pequeno demais. Aumentar o lift para `+ 0.2` no `brush_overlay.vs`.

**GUE preview travando FPS quando o usuário arrasta o splitter do painel**
→ `Engine::Resize` é chamado N vezes por segundo. Adicionar debounce: só chamar Resize se o tamanho estabilizou por 2+ frames. Simples:
```cpp
static int stable_frames = 0;
static int last_w = 0, last_h = 0;
if (w == last_w && h == last_h) stable_frames++;
else { stable_frames = 0; last_w = w; last_h = h; }
if (stable_frames >= 2) engine_->Resize(w, h);
```

**Terrain-editor não acha `../client/shaders/`**
→ O exe deve chdir para sua própria pasta no startup (`dist/tools/`). Se o `main.cpp` não faz isso, adicionar chamada ao helper `SetCwdToExeDir()` (tem em `client/src/core/paths.h/.cpp` ou versão inline igual ao GUE). Sem isso, paths relativos resolvem contra o diretório de launch, que é qualquer coisa.

**`Engine` é criado antes de `gladLoadGL`**
→ Crash imediato no primeiro `glEnable`. O `Engine::Init` assume que GL já está carregado. Garantir ordem:
```
glfwMakeContextCurrent(win);
gladLoadGL(...);
engine.Init(...);
```

**`rco_renderer` aparece 2x no link do terrain-editor e do GUE quando buildados juntos** (multiple definition of ...)
→ Cada tool faz `add_subdirectory(shared/renderer)` com seu próprio `CMAKE_BINARY_DIR/_rco_renderer`. CMake deveria dedupar — se não acontece, adicionar `include_guard(GLOBAL)` no topo do `shared/renderer/CMakeLists.txt`, ou buildar cada tool em dir de build separado.

**Shadow no preview do GUE é weird (muito dura / muito aliased)**
→ O `shadow_width/height` default do Engine é 1024. Para um painel pequeno com um actor preenchendo metade, isso pode aliasar. Baixar temporariamente para 512 com `EngineConfig::shadow_width = 512` no init do GUE.

**Preview gasta 30% de CPU mesmo quando a aba Media está fechada**
→ `DrawImGui` só roda quando a janela está visível (ImGui já faz isso), mas a animação está avançando fora. Gate no `playing_`: se a view não está hovered e `playing_` é false, não chamar `RenderToEngineFrame`. Ou cache o último frame e reusar.
