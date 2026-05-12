# Fase 4 detalhada — Integrar `Engine/Pipeline` no client

Tutorial auto-contido. Execute os passos **em ordem**, na íntegra, sem pular.
Cada passo tem critério de sucesso no final.

**É aqui que o visual do novo renderer aparece pela primeira vez.**
O primeiro momento em que algo na tela muda é o passo **4.2** (terrain).

## Pré-requisitos

- Fases 1, 2 e 3 completas.
- `rco_renderer.lib` linka. `rco_client.exe` ainda roda com o caminho antigo.
- `dist/client/shaders/` tem os 35 shaders do glRenderer + os shaders antigos do RCO (`terrain.vert/.frag`, `actor.vert/.frag`, `skybox.vert/.frag`, `particle.vert/.frag`).
- `dist/client/assets/ibl/default.hdr` existe.
- `dist/client/assets/textures/bluenoise_64.png` existe.

## Regras gerais (aplicam a TODA a Fase 4)

1. **Paralelo antes de substituir**: no passo 4.1 o `Engine`/`Pipeline` ficam lado-a-lado com o renderer antigo. Só no 4.2 o terrain troca de trilho. No 4.6 o código antigo sai.
2. **Sem quebrar o client entre passos**: ao fim de cada subpasso, `rco_client.exe` compila e roda. Se um subpasso exige mudar duas pontas (shader + código), altere shader primeiro, recompile shaders, depois troque o código — nunca o contrário.
3. **Shaders centralizados**: todo shader novo é **registrado em `shared/renderer/src/compile_shaders.cpp`** (criado na Fase 1.5.j), nunca instanciado direto como `Shader s; s.Init(...)`.
4. **`Pipeline` é per-frame**: `Begin` → `Submit*` → `End`. Submissions não persistem entre frames. Ponto-fixo é o `Engine::*StaticScene` (terrain), que é persistente mas reconstruído só quando a área muda.
5. **Particles ficam como forward pass** — após o deferred, usam `engine.gDepth()` para depth test. Não entram no gbuffer.

---

## Passo 4.1 — Init do `Engine/Pipeline` paralelo ao renderer antigo

**Objetivo**: ter `Engine` + `Pipeline` vivos na `main.cpp`, **sem ainda renderizar nada com eles**. Nenhuma mudança visual.

### 4.1.a — Incluir headers e declarar membros

Em `client/src/core/main.cpp`, adicionar após o bloco de includes do renderer existente (perto da linha 38):

```cpp
#include "rco/renderer/engine.h"
#include "rco/renderer/pipeline.h"
```

Na mesma struct/grupo onde moram `skybox`, `terrain`, `player_actor`, `particles` (linhas 146–158), adicionar:

```cpp
rco::renderer::Engine   engine;
// Pipeline precisa ser construído APÓS Engine::Init. Usar unique_ptr para adiar.
std::unique_ptr<rco::renderer::Pipeline> pipeline;
```

### 4.1.b — Inicialização no lazy-init

No bloco `if (!renderer_ready)` (linha 938), **antes** de `skybox.Init("shaders")`:

```cpp
// --- Novo Engine/Pipeline (paralelo ao renderer antigo nesta fase) ---
rco::renderer::EngineConfig ecfg{};
ecfg.width      = window.Width();
ecfg.height     = window.Height();
ecfg.shader_dir = "shaders/";
engine.Init(ecfg);
engine.LoadEnvironment("assets/ibl/default.hdr");
pipeline = std::make_unique<rco::renderer::Pipeline>(engine);
```

### 4.1.c — Tratar resize (opcional mas recomendado já aqui)

Se o client já tem callback de resize (`glfwSetFramebufferSizeCallback` ou equivalente em `window.h`), chamar `engine.Resize(w, h)` dentro do callback. Se não tem, deixar para depois (janela fixa nesta fase).

### 4.1.d — Cleanup no shutdown

Perto de `player_actor.Destroy(); terrain.Destroy(); skybox.Destroy();` (linhas 1829–1831) adicionar **antes**:

```cpp
pipeline.reset();
engine.Shutdown();
```

**Sucesso do 4.1**:
- `rco_client.exe` compila.
- Logar no jogo, entrar num char, entrar InGame → tudo renderiza exatamente como antes da Fase 4.
- Nenhum crash no shutdown.
- Adicionar `std::fprintf(stderr, "[engine] init ok\n");` depois do `pipeline = ...` e confirmar que a linha aparece.

---

## Passo 4.2 — Terrain via `Pipeline` (**PRIMEIRO VISUAL**)

**Objetivo**: o terrain deixa de usar seu próprio shader e passa a escrever no G-buffer via `terrainGBuffer.vs/.fs`. A partir deste passo o terrain ganha PBR + CSM + SSAO + IBL de graça. Actors ainda usam o renderer antigo (conviverão até o 4.4).

### 4.2.a — Criar o shader `terrainGBuffer.vs`

Arquivo: `dist/client/shaders/terrainGBuffer.vs`

```glsl
#version 460

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec3 a_tangent;

uniform mat4 u_viewProj;
uniform mat4 u_model;

out vec3 v_worldPos;
out vec3 v_normal;
out vec3 v_tangent;
out vec2 v_uv;

void main() {
    vec4 wp = u_model * vec4(a_position, 1.0);
    v_worldPos = wp.xyz;
    v_normal   = mat3(u_model) * a_normal;
    v_tangent  = mat3(u_model) * a_tangent;
    v_uv       = a_uv;
    gl_Position = u_viewProj * wp;
}
```

### 4.2.b — Criar o shader `terrainGBuffer.fs`

Arquivo: `dist/client/shaders/terrainGBuffer.fs`

Triplanar + LB_HeightBlend de 4 materiais, escrevendo nos 3 attachments do G-buffer.

```glsl
#version 460

in vec3 v_worldPos;
in vec3 v_normal;
in vec3 v_tangent;
in vec2 v_uv;

// Splatmap: weights for 4 materials in RGBA
uniform sampler2D u_splatmap;

// Per-material textures
uniform sampler2D u_mat0_albedo;   uniform sampler2D u_mat0_normal;   uniform sampler2D u_mat0_roughness;
uniform sampler2D u_mat1_albedo;   uniform sampler2D u_mat1_normal;   uniform sampler2D u_mat1_roughness;
uniform sampler2D u_mat2_albedo;   uniform sampler2D u_mat2_normal;   uniform sampler2D u_mat2_roughness;
uniform sampler2D u_mat3_albedo;   uniform sampler2D u_mat3_normal;   uniform sampler2D u_mat3_roughness;

uniform float u_tiling;           // meters per tile (default 4)
uniform vec2  u_terrainOrigin;    // world-space XZ of splatmap origin
uniform vec2  u_terrainSize;      // total world-space XZ extent

layout(location = 0) out vec4 gAlbedo;
layout(location = 1) out vec2 gNormal;   // oct-encoded
layout(location = 2) out vec4 gRMA;      // roughness, metalness, ao, unused

// --- helpers ---
vec3 triplanar(vec3 p, vec3 n, sampler2D t, float tile) {
    vec3 bw = abs(n);
    bw = max(bw - 0.2, 0.0);
    bw /= dot(bw, vec3(1.0));
    vec3 cx = texture(t, p.yz / tile).rgb;
    vec3 cy = texture(t, p.xz / tile).rgb;
    vec3 cz = texture(t, p.xy / tile).rgb;
    return cx * bw.x + cy * bw.y + cz * bw.z;
}
float triplanarR(vec3 p, vec3 n, sampler2D t, float tile) {
    vec3 bw = abs(n);
    bw = max(bw - 0.2, 0.0);
    bw /= dot(bw, vec3(1.0));
    return texture(t, p.yz / tile).r * bw.x
         + texture(t, p.xz / tile).r * bw.y
         + texture(t, p.xy / tile).r * bw.z;
}
vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return (n.z >= 0.0) ? n.xy
           : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0,
                                      n.y >= 0.0 ? 1.0 : -1.0);
}

void main() {
    // Sample splatmap in terrain-local UV (XZ plane)
    vec2 uvT = (v_worldPos.xz - u_terrainOrigin) / u_terrainSize;
    vec4 w = texture(u_splatmap, uvT);
    // LB_HeightBlend — here we approximate with weight blend; true LB takes heightmap
    // samples. The RCO terrain already does LB_HeightBlend with a height term — if
    // that term is in a 4th texture channel adjust here. Weight-normalize:
    float wsum = max(w.r + w.g + w.b + w.a, 1e-4);
    w /= wsum;

    vec3 n  = normalize(v_normal);
    vec3 wp = v_worldPos;

    vec3  alb0 = triplanar (wp, n, u_mat0_albedo,    u_tiling);
    vec3  alb1 = triplanar (wp, n, u_mat1_albedo,    u_tiling);
    vec3  alb2 = triplanar (wp, n, u_mat2_albedo,    u_tiling);
    vec3  alb3 = triplanar (wp, n, u_mat3_albedo,    u_tiling);
    float r0   = triplanarR(wp, n, u_mat0_roughness, u_tiling);
    float r1   = triplanarR(wp, n, u_mat1_roughness, u_tiling);
    float r2   = triplanarR(wp, n, u_mat2_roughness, u_tiling);
    float r3   = triplanarR(wp, n, u_mat3_roughness, u_tiling);

    vec3  alb = alb0*w.r + alb1*w.g + alb2*w.b + alb3*w.a;
    float r   = r0  *w.r + r1  *w.g + r2  *w.b + r3  *w.a;

    gAlbedo = vec4(alb, 1.0);
    gNormal = octEncode(n);
    gRMA    = vec4(r, 0.0, 1.0, 0.0);   // metal=0, AO=1
}
```

> Se o terrain atual tem um blend mais rico (UE5 LB true com heightmap per-camada), portar o shader `terrain.frag` atual para este formato substituindo só o cálculo de lighting pelas três saídas de G-buffer. Essa é a regra: **mantém-se a física/blend; substitui-se só o output**.

### 4.2.c — Registrar no `compile_shaders.cpp`

Em `shared/renderer/src/compile_shaders.cpp`, adicionar ao final do `CompileShaders()`:

```cpp
Shader::shaders["terrainGBuffer"].emplace(Shader({
    { "terrainGBuffer.vs", GL_VERTEX_SHADER,   "" },
    { "terrainGBuffer.fs", GL_FRAGMENT_SHADER, "" }
}));
```

### 4.2.d — Adicionar `SubmitTerrainChunk` ao `Pipeline`

Em `shared/renderer/include/rco/renderer/pipeline.h`, adicionar próximo de `DynamicDrawRequest`:

```cpp
struct TerrainChunkSubmission {
    GLuint    vao         = 0;
    GLuint    vbo         = 0;
    GLuint    ebo         = 0;
    GLsizei   index_count = 0;
    glm::mat4 model       = glm::mat4(1.0f);

    GLuint    splatmap    = 0;

    // 4 materials (albedo/normal/roughness). Unused slots can be 0.
    GLuint    mat_albedo[4]    = {0,0,0,0};
    GLuint    mat_normal[4]    = {0,0,0,0};
    GLuint    mat_roughness[4] = {0,0,0,0};

    float     tiling          = 4.0f;
    glm::vec2 terrain_origin  = glm::vec2(0.0f);
    glm::vec2 terrain_size    = glm::vec2(1.0f);
};
```

E no `class Pipeline`:

```cpp
void SubmitTerrainChunk(const TerrainChunkSubmission& c);
```

Campo privado:

```cpp
std::vector<TerrainChunkSubmission> terrainChunks_;
```

Nova função privada:

```cpp
void terrainPass_();
```

Em `shared/renderer/src/pipeline.cpp`:

```cpp
void Pipeline::SubmitTerrainChunk(const TerrainChunkSubmission& c) {
    terrainChunks_.push_back(c);
}
```

E em `Begin()` limpar o vetor:

```cpp
terrainChunks_.clear();
```

Implementar `terrainPass_`:

```cpp
void Pipeline::terrainPass_() {
    if (terrainChunks_.empty()) return;

    glBindFramebuffer(GL_FRAMEBUFFER, engine_->gfbo_);
    glViewport(0, 0, engine_->width_, engine_->height_);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    auto& sh = Shader::shaders["terrainGBuffer"];
    sh->Bind();
    sh->SetMat4("u_viewProj", viewProj_);

    for (const auto& c : terrainChunks_) {
        sh->SetMat4 ("u_model",          c.model);
        sh->SetFloat("u_tiling",         c.tiling);
        sh->SetVec2 ("u_terrainOrigin",  c.terrain_origin.x, c.terrain_origin.y);
        sh->SetVec2 ("u_terrainSize",    c.terrain_size.x,   c.terrain_size.y);
        glBindTextureUnit(0, c.splatmap);
        sh->SetInt("u_splatmap", 0);
        for (int i = 0; i < 4; ++i) {
            glBindTextureUnit(1 + i*3 + 0, c.mat_albedo[i]);
            glBindTextureUnit(1 + i*3 + 1, c.mat_normal[i]);
            glBindTextureUnit(1 + i*3 + 2, c.mat_roughness[i]);
        }
        sh->SetInt("u_mat0_albedo",  1);  sh->SetInt("u_mat0_normal",  2);  sh->SetInt("u_mat0_roughness",  3);
        sh->SetInt("u_mat1_albedo",  4);  sh->SetInt("u_mat1_normal",  5);  sh->SetInt("u_mat1_roughness",  6);
        sh->SetInt("u_mat2_albedo",  7);  sh->SetInt("u_mat2_normal",  8);  sh->SetInt("u_mat2_roughness",  9);
        sh->SetInt("u_mat3_albedo", 10);  sh->SetInt("u_mat3_normal", 11);  sh->SetInt("u_mat3_roughness", 12);

        glBindVertexArray(c.vao);
        glVertexArrayVertexBuffer(c.vao, 0, c.vbo, 0, sizeof(Vertex));
        glVertexArrayElementBuffer(c.vao, c.ebo);
        glDrawElements(GL_TRIANGLES, c.index_count, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(engine_->vao_);
}
```

Chamar **depois** do `gBufferPass_()` e **antes** do `ssaoPass_()` em `Pipeline::End()`:

```cpp
gBufferPass_();
terrainPass_();          // <-- NEW
ssaoPass_();
```

> O `terrainPass_` escreve nos mesmos attachments (`gAlbedo/gNormal/gRMA`), então SSAO, lighting, sombras e IBL passam a aplicar no terrain transparentemente.

### 4.2.e — Adaptar `client/src/renderer/terrain/terrain.h`

Remover:
- `Shader shader_;`
- `bool Init(const char* shader_dir, int grid_w = 8, int grid_h = 8);` — muda assinatura
- `void Render(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cam_pos, const glm::vec3& sun_dir);`

Adicionar:
- `bool Init(int grid_w = 8, int grid_h = 8);` (sem shader dir — shaders são do engine)
- `void Submit(class Pipeline& pipeline) const;`

Resto do header fica. Include do `shader.h` sai; adicionar forward decl `class Pipeline;` dentro do namespace.

### 4.2.f — Adaptar `client/src/renderer/terrain/terrain.cpp`

- Remover todo código que compila `terrain.vert/terrain.frag` (o `Init(shader_dir,...)` atual).
- `Init(int grid_w, int grid_h)`: cria `grid_w*grid_h` chunks (como já faz), só sem carregar shader.
- Substituir `Render(view, proj, cam_pos, sun_dir)` por `Submit(pipeline)`:

```cpp
#include "rco/renderer/pipeline.h"

void Terrain::Submit(rco::renderer::Pipeline& pipeline) const {
    // Pad material arrays to exactly 4 entries (unused slots = 0 texture)
    rco::renderer::TerrainChunkSubmission base{};
    for (int i = 0; i < 4; ++i) {
        if (i < (int)materials_.size()) {
            base.mat_albedo[i]    = materials_[i].albedo;
            base.mat_normal[i]    = materials_[i].normal    ? materials_[i].normal    : def_normal_;
            base.mat_roughness[i] = materials_[i].roughness ? materials_[i].roughness : def_roughness_;
        } else {
            base.mat_albedo[i]    = 0;
            base.mat_normal[i]    = def_normal_;
            base.mat_roughness[i] = def_roughness_;
        }
    }
    base.tiling         = materials_.empty() ? 4.0f : materials_[0].tiling;
    base.splatmap       = splatmap_tex_;
    base.terrain_origin = { hmap_origin_x_, hmap_origin_z_ };
    base.terrain_size   = { hmap_size_x_,   hmap_size_z_   };

    for (const auto& chunk : chunks_) {
        rco::renderer::TerrainChunkSubmission c = base;
        c.vao         = chunk->vao();
        c.vbo         = chunk->vbo();
        c.ebo         = chunk->ebo();
        c.index_count = chunk->index_count();
        c.model       = chunk->model_matrix();
        pipeline.SubmitTerrainChunk(c);
    }
}
```

> `TerrainChunk::vao()/vbo()/ebo()/index_count()/model_matrix()` — se esses getters não existem, adicionar no `TerrainChunk` (já existem internos; só expor).

### 4.2.g — Ligar no `main.cpp`

No lazy-init (linha 939), trocar:

```cpp
if (skybox.Init("shaders") && terrain.Init("shaders")) {
```

por:

```cpp
if (skybox.Init("shaders") && terrain.Init()) {
```

No loop de render, **remover** a linha:

```cpp
terrain.Render(view, proj, camera.Position(), sun);
```

E **substituir** por: no início do frame InGame (antes de qualquer `Submit*` / `Render`):

```cpp
pipeline->Begin(view, proj, camera.Position(), dt);
pipeline->SetSun(-sun, glm::vec3(1.0f));    // deferred spec expects light direction
```

No lugar antigo do `terrain.Render`:

```cpp
terrain.Submit(*pipeline);
```

**Importante**: os `player_actor.Render / actor->RenderAs / skybox.Render / particles.Render` **continuam funcionando nesta fase**. Não remover ainda. `pipeline->End()` é chamado **depois** de todos esses calls (ver 4.2.h).

### 4.2.h — Ordem do frame durante 4.2

Durante a Fase 4.2, o `pipeline->End()` **escreve sobre o framebuffer 0**, o que sobrescreveria os drawings antigos. Para coexistir sem conflito, criar uma flag temporária:

```cpp
// Temporário — ativar durante validação da 4.2
constexpr bool kUseNewPipelineForTerrain = true;
```

Ordem:

```cpp
pipeline->Begin(view, proj, camera.Position(), dt);
pipeline->SetSun(-sun, glm::vec3(1.0f));
if (kUseNewPipelineForTerrain) {
    terrain.Submit(*pipeline);
    pipeline->End();
    // pipeline writes the final composited frame to framebuffer 0
} else {
    terrain.Render(view, proj, camera.Position(), sun);
}

// Actors / skybox / particles still use the old forward path,
// drawing ON TOP of the pipeline's output into framebuffer 0.
player_actor.Render(...);
skybox.Render(view, proj);
particles.Render(view, proj);
```

Isso dá coexistência: terrain novo (deferred+PBR+CSM+SSAO+IBL) + actors/skybox/particles antigos sobrepostos. Visualmente os actors vão aparecer como "flat shading" sobre um terrain bonito. Isso é esperado nesta fase.

**Sucesso do 4.2**:
- `rco_client.exe` compila.
- Entrar InGame e ver:
  - Terrain com shadow maps projetadas pelo sol (antes: flat diffuse)
  - Terrain com SSAO nos cantos entre materiais diferentes
  - Reflexão HDRI vagamente perceptível em materiais metálicos/úmidos
  - Tonemap ACES aplicado (céu não estoura branco)
- FPS pode cair (2-3x); esperado até limpeza na Fase 6.
- Actors/skybox antigos sobrepostos, parecendo desconectados do lighting. Esperado.
- Screenshot lado-a-lado com `kUseNewPipelineForTerrain=false` mostra a diferença.

Se nada aparece na tela, ir direto ao RenderDoc — muito provável o `Pipeline::End()` estar pisando num FBO errado ou o `SetSun` com direção invertida.

---

## Passo 4.3 — Variante `gBufferSkinned` do shader

**Objetivo**: preparar o shader skinned. Ainda não usamos — só compilamos e registramos. Zero mudança visual.

### 4.3.a — Editar `dist/client/shaders/gBufferBindless.vs`

No topo do shader, após `#version 460`, adicionar os inputs condicionais. O arquivo original não tem suporte a skinning — adicioná-lo via `#ifdef`:

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
layout(std430, binding = 2) readonly buffer BoneMatrices {
    mat4 bones[];
};
#endif

// ... rest of original file: SSBOs at bindings 0/1, uniforms, main() ...
```

Dentro de `main()`, antes de computar `gl_Position`:

```glsl
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

// ... use pos, normal, tangent instead of a_position/a_normal/a_tangent
// for the rest of the original main() ...
```

> Se o original lê `a_position` em vários lugares diferentes, substituir **todas** por `pos`. Mesma coisa para `a_normal` → `normal` e `a_tangent` → `tangent`. Ctrl+F na íntegra.

### 4.3.b — Registrar as duas variantes em `compile_shaders.cpp`

Substituir/adicionar em `shared/renderer/src/compile_shaders.cpp`:

```cpp
// Static (no skinning)
Shader::shaders["gBufferBindless"].emplace(Shader({
    { "gBufferBindless.vs", GL_VERTEX_SHADER,   "" },
    { "gBufferBindless.fs", GL_FRAGMENT_SHADER, "" }
}));

// Skinned (same .vs source, compiled with HAS_SKINNING)
Shader::shaders["gBufferSkinned"].emplace(Shader({
    { "gBufferBindless.vs", GL_VERTEX_SHADER,   "#define HAS_SKINNING\n" },
    { "gBufferBindless.fs", GL_FRAGMENT_SHADER, "" }
}));
```

O sistema de preamble foi construído na Fase 1.5.i exatamente para isso — zero código novo de infra.

**Sucesso do 4.3**:
- Build completa sem erros.
- No log de inicialização do renderer, confirmar que ambas `gBufferBindless` e `gBufferSkinned` são compiladas com sucesso.
- Visual idêntico ao fim do 4.2.

---

## Passo 4.4 — Actors via `Pipeline::SubmitSkinned`

**Objetivo**: actors passam a escrever no G-buffer usando `gBufferSkinned`. Primeiro momento em que actors têm PBR + shadows corretos.

### 4.4.a — Expor VAO/VBO/EBO/bone_vbo no `SubMesh`

`shared/renderer/include/rco/renderer/model.h` — já expõe `SubMesh` com `vao/vbo/ebo/bone_vbo/idx_count/skinned`. Não precisa mudar.

O que precisamos **adicionar** em `Model`:

```cpp
// Read-only access for submission (used by Pipeline integration in client code).
const std::vector<SubMesh>& meshes() const { return meshes_; }
```

Já existe? Se sim, nada a fazer. Se não, adicionar.

### 4.4.b — Adicionar `BoneMatrices` SSBO no `Actor`

Em `client/src/renderer/actors/actor.h`:

```cpp
private:
    GLuint bone_ssbo_ = 0;

    void  EnsureBoneSSBO_();
    void  UploadBonesToSSBO_();
```

Em `actor.cpp`:

```cpp
void Actor::EnsureBoneSSBO_() {
    if (bone_ssbo_) return;
    glCreateBuffers(1, &bone_ssbo_);
    glNamedBufferData(bone_ssbo_,
        sizeof(glm::mat4) * kMaxBones,
        nullptr, GL_DYNAMIC_DRAW);
}

void Actor::UploadBonesToSSBO_() {
    EnsureBoneSSBO_();
    glNamedBufferSubData(bone_ssbo_, 0,
        sizeof(glm::mat4) * kMaxBones,
        bone_mats_.data());
}
```

Em `Actor::Destroy`:

```cpp
if (bone_ssbo_) { glDeleteBuffers(1, &bone_ssbo_); bone_ssbo_ = 0; }
```

### 4.4.c — Adicionar `Actor::Submit` / `Actor::SubmitAs`

Em `actor.h`, substituir `Render` e `RenderAs`:

```cpp
// Replaces Render()
void Submit(class Pipeline& pipeline);

// Replaces RenderAs() — uses external animation state (shared actor instances)
void SubmitAs(const std::string& anim_name, float anim_t, bool loop,
              class Pipeline& pipeline);
```

`SetupModelUniforms` some (a pipeline decide a matriz via SSBO 0 — `ObjectUniforms`).

Em `actor.cpp`:

```cpp
#include "rco/renderer/pipeline.h"

void Actor::Submit(Pipeline& pipeline) {
    // bone_mats_ already updated by Update()
    UploadBonesToSSBO_();

    // World model matrix
    glm::mat4 model = glm::translate(glm::mat4(1.0f), position);
    model = glm::rotate(model, yaw, glm::vec3(0, 1, 0));
    model = glm::scale(model, glm::vec3(scale));

    DynamicDrawRequest req{};
    // Assumption: single-submesh body for now (slot 0). If multi-mesh, loop.
    assert(!model_.meshes().empty());
    const auto& m = model_.meshes().front();
    req.vao          = m.vao;
    req.vbo          = m.vbo;
    req.ebo          = m.ebo;
    req.index_count  = m.idx_count;
    req.material_idx = 0;               // MaterialManager index — TODO wire up below
    req.model        = model;
    req.bone_ssbo    = bone_ssbo_;
    req.bone_count   = kMaxBones;
    pipeline.SubmitSkinned(req);
}

void Actor::SubmitAs(const std::string& anim_name, float anim_t, bool loop,
                     Pipeline& pipeline) {
    int clip = FindClip(anim_name);
    if (clip >= 0) {
        float dur = model_.ClipDuration(clip);
        float t   = loop ? std::fmod(anim_t, std::max(dur, 1e-3f))
                         : std::min(anim_t, dur);
        model_.ComputeBones(clip, t, bone_mats_.data(), kMaxBones);
    }
    Submit(pipeline);
}
```

### 4.4.d — Material indexing para actors

O `gBufferSkinned` lê `u_material_index` do `ObjectUniforms` SSBO (binding 0) e indexa o `materialsBuffer` (binding 1). Para actors, adicionar o material ao `MaterialManager` do engine **uma vez no init**, guardar o índice no Actor, e submeter.

Em `Actor::Init`, após `model_.Load(model_path)`:

```cpp
material_index_ = pipeline_or_engine->materials().RegisterFromTextures(
    model_.meshes().front().tex_albedo,
    model_.meshes().front().tex_normal,
    model_.meshes().front().tex_orm);
```

> `MaterialManager::RegisterFromTextures(albedo, normal, orm)` — adicionar se não existe. Se o Material manager da Fase 1 só aceita paths/Texture2D, extender com uma overload que aceita GLuint texture IDs diretos (já criadas por Assimp).

Alternativa simpler: manter o bind direto de texturas para skinned draws por enquanto (como dynamic non-skinned faz em FASE 2.4.d), sem tocar MaterialManager. O shader ignora materialsBuffer e lê de uniforms. Isso é uma rota mais rápida mas exige um segundo `gBufferSkinned_uniform` shader. Decisão: **ir pela rota MaterialManager** porque mantém um caminho único.

Se o custo disso pesar, marcar como débito e seguir para a rota alternativa.

### 4.4.e — Substituir `SubmitSkinned` placeholder em `pipeline.cpp`

A Fase 2 deixou `skinnedDraws_` caindo no `gBufferBindless`. Agora corrigir:

Em `pipeline.cpp::gBufferPass_()`, o bloco de `skinnedDraws_`:

```cpp
if (!skinnedDraws_.empty()) {
    auto& sh = Shader::shaders["gBufferSkinned"];   // <-- trocar shader
    sh->Bind();
    sh->SetMat4("u_viewProj", viewProj_);
    engine_->materialsBuffer_->BindBase(GL_SHADER_STORAGE_BUFFER, 1);
    for (const auto& r : skinnedDraws_) {
        ObjectUniforms u{ r.model, static_cast<uint32_t>(r.material_idx) };
        StaticBuffer ub(&u, sizeof(u), 0);
        ub.BindBase(GL_SHADER_STORAGE_BUFFER, 0);
        if (r.bone_ssbo) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, r.bone_ssbo);
        }
        GLuint vao = r.vao ? r.vao : engine_->vao_;
        glBindVertexArray(vao);
        if (r.vbo) glVertexArrayVertexBuffer(vao, 0, r.vbo, 0, sizeof(Vertex));
        if (r.ebo) glVertexArrayElementBuffer(vao, r.ebo);
        glDrawElements(GL_TRIANGLES, r.index_count, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(engine_->vao_);
}
```

**Cuidado crítico**: o VAO usado aqui precisa ter os atributos 4 e 5 (bone_ids/bone_weights) habilitados. O VAO global do `Engine` tem só 0-3. O `SubMesh` do Assimp (criado no `Model::ProcessMesh`) **já** tem o VAO próprio com atributos 0-5 configurados (ver `model.cpp`). Só precisamos **confirmar** que é o VAO dele que está sendo usado — ou seja, `r.vao = m.vao` (não 0).

Se não estiver, corrigir `Model::ProcessMesh` para configurar o VAO próprio com os 6 atributos. Provavelmente já está.

### 4.4.f — Ligar no `main.cpp`

Substituir (linha 1107):
```cpp
player_actor.Render(view, proj, camera.Position(), sun);
```
por:
```cpp
player_actor.Submit(*pipeline);
```

Substituir (linhas 1163–1170):
```cpp
e.actor->RenderAs(e.anim_name, e.anim_t, loop_flag,
                  view, proj, camera.Position(), sun);
...
player_actor.RenderAs(e.anim_name, e.anim_t, loop_flag,
                      view, proj, camera.Position(), sun);
```
por:
```cpp
e.actor->SubmitAs(e.anim_name, e.anim_t, loop_flag, *pipeline);
...
player_actor.SubmitAs(e.anim_name, e.anim_t, loop_flag, *pipeline);
```

**Critical**: `pipeline->End()` deve ser chamado **depois** de todos os `Submit*` (actors + terrain). Reorganizar o frame:

```cpp
pipeline->Begin(view, proj, camera.Position(), dt);
pipeline->SetSun(-sun, glm::vec3(1.0f));

terrain.Submit(*pipeline);
player_actor.Submit(*pipeline);
for (auto& [rid, e] : world_actors) {
    if (e.actor) e.actor->SubmitAs(e.anim_name, e.anim_t, loop_flag, *pipeline);
    else          player_actor.SubmitAs(e.anim_name, e.anim_t, loop_flag, *pipeline);
}

pipeline->End();   // writes final image to framebuffer 0

// Skybox + particles still old-path, drawing on top (will be moved in 4.5/4.6)
skybox.Render(view, proj);
particles.Update(...); particles.Render(view, proj);
```

**Sucesso do 4.4**:
- Actors aparecem com PBR + shadows no terrain.
- Target ring / floating numbers / HUD ImGui ainda funcionam (ImGui desenha direto em framebuffer 0).
- Actors com animação walking/idle continuam animando corretamente (bones no SSBO).
- Nenhum crash ao trocar de área (o VAO é per-submesh, não se confunde com o global).

Se actors aparecem deformados ("explodidos"), é quase certo: bones no SSBO não estão batendo com os IDs do vertex attribute — conferir offset de `bone_ids` no VBO vs `offsetof(SkinnedVertex, bone_ids)` do Model::ProcessMesh.

---

## Passo 4.5 — Particles via callback forward após deferred

**Objetivo**: particles param de renderizar direto em framebuffer 0 (que agora tem o output final do pipeline); passam a renderizar entre o composite e o FXAA, no `postprocessFbo_` que tem `gDepth_` como depth attachment. Isso dá **depth test correto** (particles ficam atrás de colinas).

### 4.5.a — Overload de `Pipeline::End` com callback

Em `pipeline.h`:

```cpp
#include <functional>

void End();
void End(const std::function<void()>& forward_pass);
```

Em `pipeline.cpp`:

```cpp
void Pipeline::End() { End({}); }

void Pipeline::End(const std::function<void()>& forward_pass) {
    // ... same baseline GL state + deferred passes up through compositePass_ ...
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

    // ---- Forward pass for user draws (particles, transparents) ----
    if (forward_pass) {
        glBindFramebuffer(GL_FRAMEBUFFER, engine_->postprocessFbo_);
        // postprocessFbo_ already has gDepth as DEPTH_ATTACHMENT (see FASE 2 createFramebuffers_)
        glViewport(0, 0, engine_->width_, engine_->height_);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);           // read depth, don't write (standard for additive particles)
        glDepthFunc(GL_LEQUAL);
        forward_pass();
        glDepthMask(GL_TRUE);
    }

    tonemappingPass_(dt_);
    fxaaPass_();
    finalBlit_();
}
```

### 4.5.b — Ligar particles no callback

Em `main.cpp`, no frame InGame, substituir:

```cpp
pipeline->End();
...
particles.Update(static_cast<float>(now), dt);
particles.Render(view, proj);
```

por:

```cpp
particles.Update(static_cast<float>(now), dt);
pipeline->End([&]() {
    particles.Render(view, proj);
});
```

### 4.5.c — Validar que `ParticleSystem::Render` não muda estado nocivo

Ler `particles.cpp` rapidamente:
- Não deve chamar `glBindFramebuffer` (estamos controlando).
- Deve respeitar `glDepthMask(GL_FALSE)` já setado (não reativar).
- `glBlendFunc(GL_SRC_ALPHA, GL_ONE)` para additive está OK.

Se a função faz `glBindFramebuffer(0)` em algum lugar, **remover**.

**Sucesso do 4.5**:
- Particles aparecem com profundidade correta (emitter atrás de colina fica oculto).
- Tonemap é aplicado nos particles também (HDR → sRGB).
- FXAA suaviza bordas de particles.

---

## Passo 4.6 — Remover skybox próprio

O `Pipeline::skyboxPass_` já desenha o HDRI skybox (Fase 2, lines 397–415 do `_orig_renderer`). Então o `skybox.h/.cpp` do RCO pode sair.

### 4.6.a — Remover referências em `main.cpp`

- Remover `#include "../renderer/skybox.h"` (linha 36)
- Remover `rco::renderer::Skybox skybox;` (linha 146)
- Remover `skybox.Init("shaders") && ` do lazy-init (linha 939)
- Remover `skybox.Render(view, proj);` (linha 1201)
- Remover `skybox.Destroy();` (linha 1831)

### 4.6.b — Deletar arquivos

```bash
rm client/src/renderer/skybox.h
rm client/src/renderer/skybox.cpp
rm dist/client/shaders/skybox.vert
rm dist/client/shaders/skybox.frag
```

### 4.6.c — Remover do `client/CMakeLists.txt`

Em `client/CMakeLists.txt`, remover `src/renderer/skybox.cpp` da lista de `add_executable`.

### 4.6.d — Configurar sun no pipeline a partir da variável antiga

Nos locais onde o `skybox.sun_dir` era usado como fonte da direção do sol para iluminação (por ex., `player_actor.Render(..., sun)`), trocar para um `glm::vec3 sun` local computado no main:

```cpp
const glm::vec3 sun = glm::normalize(glm::vec3(0.3f, 1.0f, 0.5f));
pipeline->SetSun(-sun, glm::vec3(1.0f, 0.95f, 0.80f));  // dir + color
```

(Se o client depois ganhar TOD dinâmico, essa função vira um setter animado.)

**Sucesso do 4.6**:
- Build sem erros.
- Em game, ver HDRI environment (não mais o gradiente procedural).
- Sun disc visível no HDRI.
- Refletividade do terrain batendo com a luz do HDRI.
- Nenhum overdraw de dois skyboxes (o procedural sumiu).

---

## Passo 4.7 — Validação final da Fase 4

**Critérios de sucesso da Fase 4** (todos devem ser verdade):

- [ ] `rco_client.exe` compila com 0 erros.
- [ ] Entrar no jogo (login → char select → InGame) sem crash.
- [ ] Terrain visível com PBR + shadows + SSAO + IBL.
- [ ] Player actor e NPCs animando e com shadows coerentes.
- [ ] HDRI skybox visível (sun disc do arquivo HDR).
- [ ] Particles (exemplo: morrer em combate → emitter "Explosion") com depth test correto.
- [ ] FPS ≥ 60 num hardware dev razoável (GTX 1060+).
- [ ] Nenhuma referência a `skybox`, `terrain.vert/frag`, `actor.vert/frag` no código (`grep -r` limpo).
- [ ] `pipeline.reset(); engine.Shutdown();` no caminho de saída — não crashar.
- [ ] Remove, do `client/CMakeLists.txt`: `src/renderer/skybox.cpp`, e qualquer referência a `rco_renderer` apontando para `shader_old.cpp`/`model_old.cpp` se eles não forem mais usados.

**Pontos abertos que FICAM PARA A FASE 6**:
- Shaders antigos (`terrain.vert/frag`, `actor.vert/frag`, `particle.vert/frag`) ainda em `dist/client/shaders/` — deletar na limpeza.
- `shader_old.cpp`/`model_old.cpp` — remover se nada mais os importa.
- Material override para NPCs (o `Actor::OverrideMaterial` ainda escreve textures locais ao Model, não via MaterialManager). Reescrever para usar `MaterialManager::RegisterFromTextures` corretamente.
- Multi-mesh actor rendering: atualmente Actor submete só o submesh [0]. Para armor/weapons em slots separados, iterar `model_.meshes()` com matrices per-slot.

---

## Problemas comuns e como resolver

**Tela preta ao entrar InGame**
→ `pipeline->End()` foi chamado mas nenhum `Submit*` tem conteúdo. Primeiro debug: comentar `pipeline->End()`, confirmar que renderer antigo (se ainda estiver em paralelo) desenha. Se desenha, o problema é no `End()`. Abrir RenderDoc e ver qual passe está escrevendo preto.

**Actors aparecem "explodidos" / deformados**
→ Bone matrices fora de ordem. Confirmar que `Actor::UploadBonesToSSBO_` manda exatamente `kMaxBones` mat4 na mesma ordem que o vertex shader lê `bones[a_bone_ids.x]`. Se o shader espera coluna-major e `glm::mat4` envia coluna-major (é o padrão), OK. Se actor tem mais de 64 bones, ou o Assimp `ProcessMesh` colocou bone ids ≥ 64, vão aparecer glitches. Clampar bone_id no import.

**Actors sem sombra**
→ O shader `shadowBindless` (criação do shadow map) só desenha os static meshes (via `drawIndirectBuffer_`). Para actors, precisamos também gerar sombras — significa adicionar um draw-loop de skinnedDraws_ no `shadowPass_()` usando um `shadowSkinned` shader variant. Marcar como débito na Fase 5 ou implementar direto — o risco é que sem isso NPC fica sem sombra.

**FPS < 30**
→ Múltiplas causas possíveis. Abrir RenderDoc e olhar duração por passe:
- shadowPass > 4 ms: reduzir `shadowWidth_/shadowHeight_` no `EngineConfig` (1024 → 512) durante dev.
- volumetricPass > 6 ms: `features_.volumetrics = false`.
- SSR desativado? Garantir `features_.ssr = false` por padrão.

**`glNamedBufferSubData` com `bone_ssbo_=0` → GL_INVALID_OPERATION**
→ `EnsureBoneSSBO_` não foi chamado. Garantir que `Submit`/`SubmitAs` chamam `UploadBonesToSSBO_()` (que internamente chama `EnsureBoneSSBO_()`).

**Terrain aparece todo preto (gAlbedo=0)**
→ `splatmap_tex_` é 0 ou sua amostragem está fora de [0,1]. Temporariamente no `terrainGBuffer.fs` retornar `gAlbedo = vec4(1, 0, 1, 1);` — se vira magenta confirma que o shader roda e só as texturas estão erradas. Se segue preto, é o pass inteiro não rodando (terrain chunks vazios — conferir se `terrain.Submit(*pipeline)` está sendo chamado).

**Aviso: `no depth buffer in default framebuffer` ao renderer particles**
→ Esse aviso só apareceria se particles voltassem ao framebuffer 0. Confirmar que o callback em `pipeline->End([&](){ particles.Render(...); })` está sendo usado e que `postprocessFbo_` foi criado com `GL_DEPTH_ATTACHMENT = gDepth_` (Fase 2 `createFramebuffers_`, bloco de postprocess).

**Shaders compilam mas o link falha: "uniform binding 2 already in use"**
→ O `gBufferBindless.vs` original pode estar usando binding 2 para algo que não seja bones. Renumerar: escolher um binding livre (ex.: 3) para `BoneMatrices` e atualizar o `glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, r.bone_ssbo)` em `pipeline.cpp::gBufferPass_()`.

**`Actor::Submit` tenta usar `pipeline` mas `pipeline` é `unique_ptr` nulo**
→ Submit chamado antes do lazy-init. Proteger com `if (!renderer_ready) continue;` antes de todos os `Submit*`.
