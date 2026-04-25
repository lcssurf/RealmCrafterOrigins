# Migração do Renderer RCO → glRenderer (Vendored Fork)

## Estratégia

**Vendorar** o glRenderer como código nosso dentro de `shared/renderer/engine/`.
O diretório original `glRenderer/` fica congelado como referência e nunca é tocado.

```
RealmCrafterOrigins/
├── glRenderer/                  ← ORIGINAL — referência, não modificar
│
├── shared/renderer/             ← NOSSO — vendored fork
│   ├── include/rco/renderer/
│   │   ├── engine.h             ← contexto compartilhado (shaders, FBOs)
│   │   ├── pipeline.h           ← API por-frame (Submit/Begin/End)
│   │   ├── shader.h             ← adaptado (sem shaderc)
│   │   ├── texture.h            ← portado de Texture.ixx
│   │   ├── mesh.h               ← portado de Mesh.ixx + Assimp
│   │   ├── material.h           ← portado de Material.ixx
│   │   ├── light.h              ← portado de Light.ixx
│   │   ├── buffers.h            ← Static + Dynamic
│   │   ├── indirect.h           ← DrawElementsIndirectCommand
│   │   ├── model.h              ← já existe (Assimp)
│   │   └── utilities.h          ← Timer, hash_combine
│   ├── src/
│   │   └── ... (implementações)
│   └── CMakeLists.txt           ← static lib rco_renderer
│
├── client/                      ← RCO — consome rco_renderer
├── tools/gue/                   ← RCO — consome rco_renderer
├── tools/terrain-editor/        ← RCO — consome rco_renderer
├── server/                      ← Go, intocado
│
└── dist/client/
    ├── shaders/                 ← 35 shaders copiados do glRenderer
    └── assets/ibl/default.hdr   ← HDRI de ambiente
```

---

## Mapeamento arquivo-a-arquivo (glRenderer → shared/renderer)

| Original (glRenderer) | Destino (shared/renderer/) | Ação |
|---|---|---|
| `Renderer.h/.cpp` | `include/rco/renderer/engine.h` + `pipeline.h` + `src/engine.cpp` + `src/pipeline.cpp` | **Split**: contexto vs por-frame |
| `main.cpp` | — | **Deletar** (não tem main, é lib) |
| `Shader.h/.cpp` | `include/rco/renderer/shader.h` + `src/shader.cpp` | Remover shaderc, adicionar include inliner |
| `Texture.ixx` | `include/rco/renderer/texture.h` + `src/texture.cpp` | `.ixx` → `.h/.cpp` |
| `Mesh.ixx` | `include/rco/renderer/mesh.h` + `src/mesh.cpp` | `.ixx` → `.h/.cpp`, TinyOBJ substituído por Assimp (nosso `model.h` já faz isso) |
| `Material.ixx` | `include/rco/renderer/material.h` + `src/material.cpp` | `.ixx` → `.h/.cpp` |
| `Light.ixx` | `include/rco/renderer/light.h` | `.ixx` → `.h` (header-only) |
| `Object.ixx` | `include/rco/renderer/object.h` | `.ixx` → `.h` (Transform, ObjectBatched) |
| `StaticBuffer.ixx` | `include/rco/renderer/buffers.h` + `src/buffers.cpp` | Parte 1 |
| `DynamicBuffer.ixx` | `include/rco/renderer/buffers.h` + `src/buffers.cpp` | Parte 2 (mesmo arquivo) |
| `IndirectDraw.ixx` | `include/rco/renderer/indirect.h` | `.ixx` → `.h` (header-only) |
| `Utilities.ixx` | `include/rco/renderer/utilities.h` | Timer, hash_combine, rng |
| `RendererHelpers.ixx` | `src/helpers.cpp` + `src/compile_shaders.cpp` | Split: blur helpers + CompileShaders |
| `Camera.ixx` | — | **Não copiar** — RCO já tem `client/src/renderer/camera.h` |
| `Input.h/.cpp` | — | **Não copiar** — RCO já tem input via GLFW no main |
| `Resources/Shaders/*` | `dist/client/shaders/*` | Copiar 35 arquivos GLSL |
| `Resources/IBL/*.hdr` | `dist/client/assets/ibl/default.hdr` | Copiar 1 HDRI |
| `Resources/Textures/bluenoise_64.png` | `dist/client/assets/textures/bluenoise_64.png` | Copiar |

---

## Mudanças necessárias na cópia vendored

### Shader — remover shaderc

Original usa `shaderc::Compiler` para:
1. Resolver `#include "..."` nos shaders
2. Substituição via regex para variantes do gaussian blur

Substituir por:
```cpp
// Include inliner (~25 linhas)
std::string resolveIncludes(const std::string& src, const std::string& shaderDir) {
    std::string out;
    std::istringstream ss(src);
    std::string line;
    while (std::getline(ss, line)) {
        auto pos = line.find("#include");
        if (pos != std::string::npos) {
            auto q1 = line.find('"', pos);
            auto q2 = line.find('"', q1 + 1);
            std::string inc = line.substr(q1 + 1, q2 - q1 - 1);
            std::ifstream f(shaderDir + "/" + inc);
            std::string content((std::istreambuf_iterator<char>(f)), {});
            out += resolveIncludes(content, shaderDir); // recursivo
        } else {
            out += line + "\n";
        }
    }
    return out;
}

// Variantes via preamble (substitui regex_replace)
void Shader::compileStage(GLenum type, const std::string& file,
                          const std::string& preamble) {
    std::string src = loadFile(file);
    src = resolveIncludes(src, shaderDir_);

    const char* sources[] = { preamble.c_str(), src.c_str() };
    GLint lengths[] = { (GLint)preamble.size(), (GLint)src.size() };
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, sources, lengths);
    glCompileShader(s);
    // ... check status, attach ...
}
```

O `ShaderInfo` do glRenderer com `replace` vector vira `ShaderInfo` com string `preamble`:
```cpp
// Original:
{ "gaussian.cs", GL_COMPUTE_SHADER, {{"#define KERNEL_RADIUS 3", "#define KERNEL_RADIUS 6"}} }
// Adaptado:
{ "gaussian.cs", GL_COMPUTE_SHADER, "#define KERNEL_RADIUS 6\n#define FORMAT RGBA32f\n" }
```

E o arquivo `gaussian.cs` na `dist/client/shaders/` precisa ter os defaults removidos do topo
(já que agora o preamble os define):
```glsl
// Topo do gaussian.cs:
#version 460 core
#ifndef KERNEL_RADIUS
#define KERNEL_RADIUS 3
#endif
#ifndef FORMAT
#define FORMAT RG32f
#endif
// ... resto do shader usa KERNEL_RADIUS e FORMAT ...
```

### Renderer — split em Engine + Pipeline

O `Renderer` original mistura responsabilidades. Vamos separar:

**`Engine`** (vive o processo inteiro):
- Compila todos os shaders
- Gera o VAO global
- Cria todos os framebuffers (gfbo, hdr, shadow, ssao, volumetrics, ssr, postprocess)
- Carrega bluenoise texture
- Aloca vertexBuffer/indexBuffer/materialsBuffer globais
- Carrega IBL environment

**`Pipeline`** (por frame, stateless entre frames):
- `Begin(view, proj, cam_pos, dt)`
- `Submit(mesh, material, model)`
- `SubmitSkinned(mesh, material, model, bones)`
- `AddPointLight(pos, color, radius)`
- `SetSunDirection(dir)` / `SetSunColor(color)`
- `End()` — executa os 13 passes

O `Pipeline` referencia o `Engine` via ponteiro.

### main.cpp e scene loading — deletar

Remover completamente da cópia:
- `main.cpp` (inteiro)
- `Renderer::Run()`
- `Renderer::InitWindow()` / `InitGL()` / `InitImGui()` / `Cleanup()`
- `Renderer::MainLoop()` (a lógica vira `Pipeline::End()`)
- `Renderer::InitScene()` / `LoadScene1()` / `LoadScene2()` / `Scene1Lights()` / `Scene2Lights()`
- `Renderer::SetupBuffers()` (vira `Engine::UploadStaticMesh()`)
- `Renderer::DrawUI()` (deletar — RCO tem sua própria UI ImGui)
- `Renderer::DrawPbrSphereGrid()` (deletar — era debug)

### WINDOW_WIDTH / WINDOW_HEIGHT — parâmetros runtime

Todas as referências a `WINDOW_WIDTH` e `WINDOW_HEIGHT` nos `.cpp`:
- Viram `engine_->width_` e `engine_->height_`
- `Engine::Resize(w, h)` recria framebuffers

### Mesh — TinyOBJ → Assimp

O `shared/renderer/include/rco/renderer/model.h` já carrega via Assimp (glb/fbx/b3d/obj).

Adaptar o novo `mesh.h` para:
1. Manter o `Vertex` layout idêntico ao glRenderer (position/normal/uv/tangent)
2. `Mesh` class estática → cria VBO/EBO próprios (para meshes individuais)
3. `MeshInfo` batched → aloca no DynamicBuffer global (multi-draw indirect)
4. Função `LoadBatchedFromAssimp(const Model&, MaterialManager&, DynamicBuffer&, DynamicBuffer&)`
   que pega um `Model` já carregado pelo `model.h` existente e faz upload batched.

Assim, `model.h/.cpp` (carregamento Assimp) e `mesh.h/.cpp` (upload para GPU batched) coexistem.

### Paths de shaders — configurável

No `Shader::loadFile` do glRenderer, o path é hardcoded:
```cpp
static constexpr const char* shader_dir_ = "./Resources/Shaders/";
```

Adaptar para:
```cpp
static inline std::string shader_dir_ = "shaders/"; // default
static void SetShaderDir(const std::string& dir) { shader_dir_ = dir; }
```

O RCO chama `Shader::SetShaderDir("shaders/")` antes do `Engine::Init` — o exe já faz
chdir para sua própria pasta no startup, então `shaders/` resolve pra `dist/client/shaders/`.

### IBL path — parâmetro

`Renderer::LoadEnvironmentMap(std::string path)` vira `Engine::LoadEnvironment(const char* path)`.
RCO chama com `"assets/ibl/default.hdr"`.

---

## API pública do Engine

```cpp
namespace rco::renderer {

struct EngineConfig {
    int width  = 1280;
    int height = 720;
    int shadow_resolution = 1024;
    std::string shader_dir = "shaders/";
    bool enable_debug_output = true; // GL debug callback
};

struct FeatureConfig {
    bool ssao        = true;
    bool volumetrics = true;
    bool ssr         = false;
    bool fxaa        = true;
    int  shadow_method = SHADOW_METHOD_ESM;
};

class Engine {
public:
    void Init(const EngineConfig& cfg);
    void Resize(int w, int h);
    void Shutdown();

    // Recursos carregados uma vez
    void LoadEnvironment(const char* hdr_path);

    // Materiais e meshes
    MaterialManager& materials();
    DynamicBuffer&   vertexBuffer();
    DynamicBuffer&   indexBuffer();
    StaticBuffer&    drawIndirectBuffer();
    StaticBuffer&    materialsBuffer();

    // Upload estático (rebuild quando scene muda)
    void BeginStaticScene();  // limpa vertex/index buffers
    MeshHandle UploadStaticMesh(const std::vector<Vertex>& v,
                                const std::vector<uint32_t>& i,
                                int material_index);
    void EndStaticScene();    // gera drawIndirectBuffer e materialsBuffer

    // Acesso direto (para passes customizados)
    GLuint gAlbedo() const;
    GLuint gNormal() const;
    GLuint gDepth()  const;
    GLuint finalImage() const;
};

class Pipeline {
public:
    explicit Pipeline(Engine& e);

    // Config ajustável por frame
    void SetFeatures(const FeatureConfig& cfg);
    void SetSun(glm::vec3 direction, glm::vec3 color);

    // Por frame
    void Begin(const glm::mat4& view, const glm::mat4& proj,
               const glm::vec3& cam_pos, float dt);

    // Objetos estáticos (já uploaded via Engine::UploadStaticMesh)
    // Submete todos via multi-draw indirect
    void SubmitStaticScene();

    // Objetos dinâmicos (actors skinned, props móveis)
    void SubmitDynamic(GLuint vao, GLuint ebo, int index_count,
                       int material_index, const glm::mat4& model);
    void SubmitSkinned(GLuint vao, GLuint ebo, int index_count,
                       int material_index, const glm::mat4& model,
                       GLuint bone_ssbo, int bone_count);

    // Luzes
    void AddPointLight(glm::vec3 pos, glm::vec3 color, float radius);

    void End();  // executa os 13 passes, escreve no framebuffer padrão
};

} // namespace rco::renderer
```

**Fluxo no client:**
```cpp
// Init (uma vez)
rco::renderer::Engine engine;
engine.Init({.width = w, .height = h});
engine.LoadEnvironment("assets/ibl/default.hdr");

// Upload de terrain + props estáticos (toda vez que area muda)
engine.BeginStaticScene();
for (auto& chunk : terrain.chunks) engine.UploadStaticMesh(...);
for (auto& prop  : area.props)     engine.UploadStaticMesh(...);
engine.EndStaticScene();

rco::renderer::Pipeline pipeline(engine);

// Por frame
pipeline.Begin(camera.view(), camera.proj(), camera.pos(), dt);
pipeline.SetSun(sun_dir, sun_color);

pipeline.SubmitStaticScene();                       // terrain + props

for (auto& actor : world_actors) {                  // actors skinned
    pipeline.SubmitSkinned(actor.vao, actor.ebo, actor.count,
                           actor.mat_idx, actor.model, actor.boneSSBO,
                           actor.boneCount);
}

for (auto& em : emitters) {                         // point lights
    pipeline.AddPointLight(em.pos, em.color, em.radius);
}

pipeline.End();

// Particles (passe forward APÓS End — usa depth do G-buffer)
particles.Render(engine.gDepth(), camera);

glfwSwapBuffers(window);
```

---

## Passos de execução

### FASE 1 — Cópia e conversão (mecânico)

**1.1 — Copiar assets**
```
glRenderer/Resources/Shaders/*           → dist/client/shaders/
glRenderer/Resources/Textures/bluenoise_64.png → dist/client/assets/textures/
glRenderer/Resources/IBL/<um.hdr>        → dist/client/assets/ibl/default.hdr
```

**1.2 — Copiar código fonte**

Copiar os arquivos do glRenderer para `shared/renderer/` com os nomes novos:
```
glRenderer/Renderer.h       → shared/renderer/include/rco/renderer/_orig_renderer.h (temp)
glRenderer/Renderer.cpp     → shared/renderer/src/_orig_renderer.cpp (temp)
glRenderer/Shader.h/.cpp    → shared/renderer/include/rco/renderer/shader.h + src/shader.cpp
glRenderer/Texture.ixx      → shared/renderer/include/rco/renderer/texture.h + src/texture.cpp
glRenderer/Mesh.ixx         → shared/renderer/include/rco/renderer/mesh.h + src/mesh.cpp
glRenderer/Material.ixx     → shared/renderer/include/rco/renderer/material.h + src/material.cpp
glRenderer/Light.ixx        → shared/renderer/include/rco/renderer/light.h
glRenderer/Object.ixx       → shared/renderer/include/rco/renderer/object.h
glRenderer/StaticBuffer.ixx + DynamicBuffer.ixx → shared/renderer/include/rco/renderer/buffers.h + src/buffers.cpp
glRenderer/IndirectDraw.ixx → shared/renderer/include/rco/renderer/indirect.h
glRenderer/Utilities.ixx    → shared/renderer/include/rco/renderer/utilities.h
glRenderer/RendererHelpers.ixx → shared/renderer/src/helpers.cpp + src/compile_shaders.cpp
```

**1.3 — Converter `.ixx` → `.h/.cpp`**

Em cada `.ixx` convertido:
1. Remover `module; ... export module X;` — substituir por `#pragma once` + includes
2. Remover `export` de cada symbol
3. Remover `import X;` — substituir por `#include "X.h"`
4. Separar código em `.h` (declarações) e `.cpp` (implementações)

**1.4 — Copiar `shader.h` e adaptar (remover shaderc)**

Remover:
```cpp
#include <shaderc/shaderc.hpp>
class IncludeHandler : public shaderc::CompileOptions::IncluderInterface ...
std::vector<uint32_t> spvPreprocessAndCompile(...) ...
std::string preprocessShader(...) ...
```

Substituir pela função `resolveIncludes` e compilação via multi-string `glShaderSource`
(mostrado acima).

Adaptar `ShaderInfo` de `replace` vector para `preamble` string.

**1.5 — Atualizar `compile_shaders.cpp`**

Converter cada registro de shader do glRenderer para o novo formato:
```cpp
// Antes (shaderc com regex):
Shader::shaders["gaussian_blur6"].emplace(Shader({
    { "gaussian.cs", GL_COMPUTE_SHADER,
      {{"#define KERNEL_RADIUS 3", "#define KERNEL_RADIUS 6"}} } }));

// Depois (preamble):
Shader::shaders["gaussian_blur6"].emplace(Shader({
    { "gaussian.cs", GL_COMPUTE_SHADER,
      "#define KERNEL_RADIUS 6\n" } }));
```

Adicionar guards `#ifndef` nos arquivos `gaussian.cs` para aceitar os defines do preamble.

---

### FASE 2 — Split Renderer → Engine + Pipeline

Partindo de `_orig_renderer.h/.cpp` (temporário da fase 1), criar:

**2.1 — `engine.h/.cpp`**

Copiar do `_orig_renderer`:
- `InitGL` (parte sem glfw/imgui)
- `CreateFramebuffers`
- `CreateVAO`
- `LoadEnvironmentMap`
- `Cleanup` (sem glfw)

Converter membros:
- `window` → remover
- `cursorVisible`/`vsyncEnabled` → remover (cliente controla)
- Todos os framebuffers (`gfbo`, `hdr`, `shadow*`, `ssao`, `volumetrics`, `ssr`, `postprocess*`) → públicos ou getters
- `WINDOW_WIDTH/HEIGHT` → `width_/height_`
- Método novo `Resize(w, h)` que recria FBOs

**2.2 — `pipeline.h/.cpp`**

Copiar do `_orig_renderer::MainLoop`:
- Do `glBindVertexArray(vao)` até `glfwSwapBuffers`
- Removendo: `Input::Update`, `cam.Update`, debug keys (1-9, P/O/I/U, C, ESC), DrawUI

Estruturar em funções privadas:
- `shadowPass()` — linhas 167–239
- `gBufferPass()` — linhas 243–285
- `ssaoPass()` — linhas 287–340
- `globalLightPass()` — linhas 353–371
- `localLightsPass()` — linhas 373–394
- `skyboxPass()` — linhas 397–415
- `volumetricPass()` — linhas 418–475
- `ssrPass()` — linhas 481–505
- `compositePass()` — linhas 508–537
- `tonemappingPass()` — `ApplyTonemapping`
- `fxaaPass()` — linhas 545–560
- `finalBlit()` — linha 563–564

`End()` chama todos em sequência.

**2.3 — Remover `_orig_renderer.h/.cpp`**

Após copiar tudo que importa, deletar os arquivos temporários.

---

### FASE 3 — CMakeLists.txt do rco_renderer

Expandir `shared/renderer/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(rco_renderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glad   CONFIG REQUIRED)
find_package(glm    CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(Stb    REQUIRED)

add_library(rco_renderer STATIC
    src/shader.cpp
    src/texture.cpp
    src/mesh.cpp
    src/material.cpp
    src/buffers.cpp
    src/model.cpp
    src/stb_image_impl.cpp
    src/helpers.cpp
    src/compile_shaders.cpp
    src/engine.cpp
    src/pipeline.cpp
)

target_include_directories(rco_renderer
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${Stb_INCLUDE_DIR}"
)

target_link_libraries(rco_renderer
    PUBLIC  glad::glad glm::glm
    PRIVATE assimp::assimp
)

# Bindless textures — extensão obrigatória em tempo de compilação
# Verificação de runtime em Engine::Init
```

---

### FASE 4 — Integrar no client

**4.1 — `client/CMakeLists.txt`** — já linka `rco_renderer` (sem mudança).

**4.2 — `main.cpp` — substituir init de render**

Depois de `gladLoadGL`:
```cpp
rco::renderer::Engine engine;
engine.Init({
    .width = window_width,
    .height = window_height,
    .shader_dir = "shaders/",
});
engine.LoadEnvironment("assets/ibl/default.hdr");

rco::renderer::Pipeline pipeline(engine);
```

**4.3 — Terrain (LB_HeightBlend no G-buffer)**

O terrain atual do RCO já faz **UE5 LB_HeightBlend de 4 materiais + triplanar mapping**
no shader forward. Isso é o padrão AAA (Unreal, Witcher 3, Red Dead 2). Mantemos essa
técnica — só portamos a saída para o G-buffer em vez de calcular iluminação.

**Novo shader par**: `dist/client/shaders/terrainGBuffer.vs/.fs`

Derivado do `gBufferBindless.vs/.fs`, mas:
- Input adicional: splatmap texture (RGBA8, pesos dos 4 materiais)
- Input adicional: 4× albedo/normal/roughness dos materiais do terrain (bindless handles)
- No `.fs`: faz LB_HeightBlend dos 4 materiais (igual ao shader forward atual), aplica
  triplanar mapping em slopes íngremes, escreve no G-buffer:
  - `out vec4 gAlbedo`    (RGB final do blend)
  - `out vec2 gNormal`    (normal blendada, oct-encoded)
  - `out vec4 gRMA`       (roughness/metalness/AO do blend)

**Registrar no `compile_shaders.cpp`**:
```cpp
Shader::shaders["terrainGBuffer"].emplace(Shader({
    { "terrainGBuffer.vs", GL_VERTEX_SHADER,   "" },
    { "terrainGBuffer.fs", GL_FRAGMENT_SHADER, "" }
}));
```

**Integração no Pipeline**:

Adicionar método separado de submissão de terrain no Pipeline — não entra no
multi-draw indirect dos objetos estáticos porque usa shader diferente e texturas
extras (splatmap + 4 materiais):

```cpp
struct TerrainChunk {
    GLuint vao, ebo;
    int index_count;
    glm::mat4 model;
    GLuint splatmap_tex;
    int material_indices[4]; // índices no materialsBuffer para os 4 layers
};

void Pipeline::SubmitTerrainChunk(const TerrainChunk& c);
```

Internamente, o `Pipeline::End()` executa um passe de terrain logo após o G-buffer
pass normal, antes do SSAO, usando o shader `terrainGBuffer`. Como escreve nos mesmos
attachments do G-buffer, o resto do pipeline (SSAO, lighting, sombras, etc.) aplica
transparente — o terrain ganha PBR + shadows + SSAO + IBL gratuitamente.

**No client**, `client/src/renderer/terrain/terrain.cpp`:
- `Vertex` com position + normal + uv (tangent computado no shader) no formato do
  `rco::renderer::Vertex`
- Cada chunk mantém seu VAO/EBO próprios (não é uploaded no buffer global)
- A cada frame: `pipeline.SubmitTerrainChunk(chunk)` para cada chunk visível

**4.4 — Actors (Linear Blend Skinning via `#define HAS_SKINNING`)**

Skinning padrão AAA: **LBS com 4 bones por vértice**, bone matrices em SSBO.
A mesma fonte de `gBufferBindless.vs` é compilada duas vezes — uma sem skinning
(estáticos) e outra com (actors animados).

**Editar `dist/client/shaders/gBufferBindless.vs`** para suportar o define:

```glsl
#version 460
#extension GL_ARB_bindless_texture : require

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec3 a_tangent;

#ifdef HAS_SKINNING
layout(location = 4) in ivec4 a_bone_ids;
layout(location = 5) in vec4  a_bone_weights;
layout(std430, binding = 2) readonly buffer BoneMatrices { mat4 bones[]; };
#endif

// ... outros uniforms/SSBOs existentes ...

void main() {
    vec3 pos     = a_position;
    vec3 normal  = a_normal;
    vec3 tangent = a_tangent;

    #ifdef HAS_SKINNING
    mat4 skin = bones[a_bone_ids.x] * a_bone_weights.x
              + bones[a_bone_ids.y] * a_bone_weights.y
              + bones[a_bone_ids.z] * a_bone_weights.z
              + bones[a_bone_ids.w] * a_bone_weights.w;
    pos     = vec3(skin * vec4(a_position, 1.0));
    normal  = mat3(skin) * a_normal;
    tangent = mat3(skin) * a_tangent;
    #endif

    // ... resto igual ao original do glRenderer ...
}
```

**Registrar as duas variantes em `compile_shaders.cpp`**:

```cpp
// Estático (já existe no glRenderer)
Shader::shaders["gBufferBindless"].emplace(Shader({
    { "gBufferBindless.vs", GL_VERTEX_SHADER,   "" },
    { "gBufferBindless.fs", GL_FRAGMENT_SHADER, "" }
}));

// Skinned (MESMA fonte, preamble com #define)
Shader::shaders["gBufferSkinned"].emplace(Shader({
    { "gBufferBindless.vs", GL_VERTEX_SHADER,   "#define HAS_SKINNING\n" },
    { "gBufferBindless.fs", GL_FRAGMENT_SHADER, "" }
}));
```

Usa o sistema de preamble que a gente já implementou na Fase 1.5 para os gaussian blur
— zero código novo de infraestrutura.

**VAO para actors skinned**:

O VAO global do Engine tem atributos 0-3. Actors skinned usam um segundo VAO
(`vao_skinned`) com atributos 0-5:

```cpp
glEnableVertexArrayAttrib(vao_skinned, 4); // bone_ids
glEnableVertexArrayAttrib(vao_skinned, 5); // bone_weights
glVertexArrayAttribIFormat(vao_skinned, 4, 4, GL_INT,   offsetof(SkinnedVertex, bone_ids));
glVertexArrayAttribFormat (vao_skinned, 5, 4, GL_FLOAT, GL_FALSE, offsetof(SkinnedVertex, bone_weights));
glVertexArrayAttribBinding(vao_skinned, 4, 0);
glVertexArrayAttribBinding(vao_skinned, 5, 0);
```

`SkinnedVertex` é `Vertex` + `ivec4 bone_ids` + `vec4 bone_weights` (16 bytes extras).

**No client**, `client/src/renderer/actors/actor.cpp`:
- Assimp já fornece bone data — `Model` do `shared/renderer/model.h` já expõe isso
- Converter para `SkinnedVertex` na hora do upload
- Bone matrices computadas em CPU (CalcBones / ComputeBones) são upadas num SSBO
  por actor a cada frame
- `pipeline.SubmitSkinned(actor.vao, actor.ebo, count, mat_idx, model, bone_ssbo, bone_count)`

**No Pipeline**:
- `Submit*` normal usa shader `gBufferBindless` + VAO atributos 0-3
- `SubmitSkinned` usa shader `gBufferSkinned` + VAO atributos 0-5 + bind do bone SSBO
  no binding 2 antes do `glDrawElements`
- Actors skinned fazem `glDrawElements` individual (não entram no multi-draw indirect
  porque cada um tem seu próprio bone SSBO)

**4.5 — Particles** — não muda

`client/src/renderer/particles.cpp` continua como passe forward separado.
Renderiza após `pipeline.End()` usando `engine.gDepth()` para depth test.

**4.6 — Remover skybox próprio**

`client/src/renderer/skybox.h/.cpp` — deletar. Pipeline já faz skybox HDRI.

---

### FASE 5 — Integrar nas tools

**5.1 — `tools/terrain-editor/CMakeLists.txt`**

Adicionar:
```cmake
add_subdirectory(
    "${CMAKE_SOURCE_DIR}/../../shared/renderer"
    "${CMAKE_BINARY_DIR}/_rco_renderer"
)
target_link_libraries(rco_terrain PRIVATE rco_renderer)
```

Remover `src/stb_impl.cpp` do build (conflito com `stb_image_impl.cpp` do rco_renderer).

**5.2 — Substituir `terrain_renderer.h/.cpp`**

Usar `Engine` + `Pipeline` igual ao client. Visual idêntico.

**5.3 — GUE preview**

Implementar `tools/gue/src/preview_viewport.cpp` usando FBO offscreen.
Adicionar ao `Engine`:
```cpp
void SetRenderTarget(GLuint fbo);  // redireciona o blit final
```

Preview chama `SetRenderTarget(offscreen_fbo)` antes de `pipeline.End()`,
depois exibe via `ImGui::Image((ImTextureID)finalImage, panelSize)`.

---

### FASE 6 — Limpeza

- Remover `client/src/renderer/skybox.h/.cpp`
- Remover shaders antigos de `dist/client/shaders/` que não são mais usados
- Remover `tools/terrain-editor/src/stb_impl.cpp`
- Remover `client/src/renderer/terrain/terrain_chunk.h/.cpp` se o terrain virou submissão direta

---

## Checklist de validação

Client:
- [ ] Terrain aparece no mundo com PBR lighting
- [ ] Actors skinned renderizam com animação
- [ ] Sombras suaves (ESM/MSM)
- [ ] SSAO nos cantos
- [ ] HDRI skybox (não mais procedural)
- [ ] ACES tonemapping (céu não estoura branco)
- [ ] Point lights dos emitters iluminam a cena
- [ ] Volumetric fog visível com sol rasante
- [ ] Particles funcionam (passe forward)
- [ ] FPS aceitável (≥60 na máquina de dev)

GUE:
- [ ] Preview 3D de actor def mostra modelo com PBR correto

Terrain Editor:
- [ ] Visual idêntico ao client
- [ ] Sculpt e paint ainda funcionam

---

## Ordem de execução

```
Fase 1  (copiar + converter .ixx)          → ~1 dia — mecânico
Fase 2  (split Renderer → Engine+Pipeline) → ~1 dia — refactor
Fase 3  (CMakeLists do rco_renderer)       → ~1h
Fase 4  (integrar no client)               → ~3 dias — terrain LB_HeightBlend + actors skinned
  4.1 Init pipeline                        → ~1h
  4.2 Shader terrainGBuffer + submit       → ~1 dia
  4.3 VAO skinned + gBufferSkinned variant → ~1 dia
  4.4 Actors skinned upload + bone SSBO    → ~4h
  4.5 Particles passe forward              → ~2h
Fase 5  (integrar nas tools)               → ~1 dia
Fase 6  (limpeza)                          → ~2h
```

**Total: ~6-8 dias de trabalho concentrado.**

Total estimado: **~5-7 dias de trabalho concentrado**.

---

## Por que essa abordagem funciona

1. **glRenderer original nunca é tocado** — fica como referência permanente
2. **Todo o pipeline deferred PBR vem pronto** — não reinventamos nada
3. **Visual idêntico aos prints** — é o mesmo código, só reorganizado
4. **Shaders copiados sem modificação** (exceto guards nos `#define` para preamble)
5. **Assimp já existe no RCO** — não precisamos do TinyOBJ
6. **Sem dependências novas** — shaderc eliminado via include inliner simples
7. **Cliente, GUE, terrain editor** — todos usam a mesma `rco::renderer::Engine`
