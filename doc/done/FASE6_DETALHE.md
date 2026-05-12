# Fase 6 detalhada — Limpeza pós-migração

Tutorial auto-contido. Execute os passos **em ordem**, na íntegra.
Todo passo é destrutivo (deleção de arquivos, remoção de código). **Nenhum dele muda visual** — se algo mudar, reverter e investigar antes de prosseguir.

## Pré-requisitos

- Fases 1, 2, 3, 4 e 5 completas.
- `rco_client.exe`, `rco_terrain.exe`, `rco_gue.exe` compilam e rodam.
- Todas as funcionalidades testadas no critério de sucesso das fases anteriores funcionando.
- **Commit limpo antes de começar** (`git commit -am "pre-fase-6 snapshot"`): esta fase remove 8+ arquivos. Ter um ponto de retorno é essencial.

## Regras gerais (aplicam a TODA a Fase 6)

1. **Build após cada sub-passo**: qualquer passo que termina com falha de compilação volta atrás.
2. **Visual invariante**: o output de pixels do client não muda em nenhum sub-passo. Se a Fase 6 mudou algo na tela, o lixo removido estava vivo. Reverter e investigar.
3. **Grep antes de deletar**: todo passo que remove arquivo mostra um grep para confirmar que nada o referencia.
4. **Não deletar `particles.vert/.frag`**: particles continuam em forward pass próprio (decidido na Fase 4.5); shader próprio é intencional.
5. **Não deletar `terrain_chunk.h/.cpp`**: `TerrainChunk` ainda é usado pelo `client/src/renderer/terrain/terrain.cpp` para guardar VAO/VBO/EBO de cada chunk — só o *shader* de terrain saiu; o chunk data permanece.

---

## Passo 6.1 — Deletar shaders antigos de `dist/client/shaders/`

### 6.1.a — Lista de arquivos a deletar

O `dist/client/shaders/` hoje (pré-Fase-6) tem os 8 shaders originais do RCO:

```
actor.frag   actor.vert
particle.frag particle.vert
skybox.frag   skybox.vert
terrain.frag  terrain.vert
```

Depois da Fase 1.2, 35+ shaders do glRenderer foram copiados. Depois da 4.2 e 5.1 mais 2 pares foram adicionados (`terrainGBuffer.*`, `brush_overlay.*`). Agora removemos os **substituídos**:

| Shader | Substituto | Delete? |
|---|---|---|
| `actor.vert/.frag` | `gBufferSkinned` (variante de `gBufferBindless` com `#define HAS_SKINNING`) | **sim** |
| `terrain.vert/.frag` | `terrainGBuffer.vs/.fs` | **sim** |
| `skybox.vert/.frag` | `hdri_skybox.fs` (do glRenderer, já registrado) | **sim** |
| `particle.vert/.frag` | nenhum — particles continuam forward-pass próprio | **NÃO deletar** |

### 6.1.b — Grep de referências

Antes de deletar, confirmar que nenhum código mais aponta para esses nomes:

```bash
grep -r "actor.vert\|actor.frag"     client/ tools/ shared/
grep -r "terrain.vert\|terrain.frag" client/ tools/ shared/
grep -r "skybox.vert\|skybox.frag"   client/ tools/ shared/
```

**Resultado esperado**: zero matches em cada um. Se aparecer algo em `tools/gue/src/preview_viewport.cpp`, a Fase 5.2 não foi completada corretamente — voltar pra lá.

### 6.1.c — Deletar

```bash
rm dist/client/shaders/actor.vert
rm dist/client/shaders/actor.frag
rm dist/client/shaders/terrain.vert
rm dist/client/shaders/terrain.frag
rm dist/client/shaders/skybox.vert
rm dist/client/shaders/skybox.frag
```

**Sucesso do 6.1**:
- Os 6 arquivos acima não existem mais.
- `particle.vert` e `particle.frag` **ainda existem**.
- `rco_client.exe` roda sem erro de "shader not found" (os 35 do glRenderer + `terrainGBuffer` + `brush_overlay` + particles resolve tudo).
- Visual idêntico.

---

## Passo 6.2 — Deletar `shader_old` e `model_old`

A Fase 1.4 renomeou `shader.h/.cpp` e `model.h/.cpp` do RCO original para `*_old` para preservá-los durante a transição. Depois da Fase 4, o client usa as versões novas (vindas das conversões da Fase 1.5.i e do `model.h` já existente do RCO) via `Engine`/`Pipeline`. Os `*_old` viraram código morto.

### 6.2.a — Grep de referências

```bash
grep -rn "shader_old\|model_old" client/ tools/ shared/
```

**Resultado esperado**: zero matches. Se aparecer algo, NÃO DELETAR — alguma parte do código ainda não migrou.

Um caso que o grep vai pegar: o próprio `shared/renderer/CMakeLists.txt` tem as linhas `src/shader_old.cpp` e `src/model_old.cpp` (adicionadas na Fase 1.4 e mantidas nas Fases 2, 3, 4, 5). Esperado — trataremos em 6.2.c.

### 6.2.b — Deletar arquivos

```bash
rm shared/renderer/include/rco/renderer/shader_old.h
rm shared/renderer/include/rco/renderer/model_old.h
rm shared/renderer/src/shader_old.cpp
rm shared/renderer/src/model_old.cpp
```

### 6.2.c — Atualizar `shared/renderer/CMakeLists.txt`

Remover do bloco `add_library(rco_renderer STATIC ...)`:

```cmake
# Preservados (client ainda usa até a Fase 4 integrar Engine/Pipeline)
src/shader_old.cpp
src/model_old.cpp
```

Estado final do `CMakeLists.txt`:

```cmake
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

    # Fase 2 — core
    src/engine.cpp
    src/pipeline.cpp

    # Shared
    src/model.cpp
    src/stb_image_impl.cpp
)
```

> **Atenção**: `src/model.cpp` continua no build — ele é o `Model` do RCO atual (skinned/Assimp), NÃO o `model_old.cpp`. O `model.h` em `include/rco/renderer/model.h` também permanece. Só os `_old` saem.

**Sucesso do 6.2**:
- `rco_renderer.lib` recompila sem erros.
- `rco_client.exe`, `rco_terrain.exe`, `rco_gue.exe` todos linkam e rodam.
- Visual idêntico.

---

## Passo 6.3 — Remover feature flag temporária `kUseNewPipelineForTerrain`

A Fase 4.2.h introduziu uma constante `kUseNewPipelineForTerrain` em `client/src/core/main.cpp` para permitir coexistência do renderer antigo com o pipeline novo durante a validação. Após a Fase 4.4 essa flag está sempre `true` e o caminho `false` virou código morto.

### 6.3.a — Remover de `main.cpp`

Apagar a linha:

```cpp
constexpr bool kUseNewPipelineForTerrain = true;
```

E o `if (kUseNewPipelineForTerrain) { ... } else { ... }`. Manter apenas o bloco `true` (agora incondicional):

```cpp
pipeline->Begin(view, proj, camera.Position(), dt);
pipeline->SetSun(-sun, glm::vec3(1.0f));
terrain.Submit(*pipeline);
// ... actors ...
pipeline->End(...);
```

**Grep de sanidade**:
```bash
grep -n "kUseNewPipelineForTerrain\|kUseNewPipeline" client/
```
Zero matches esperado.

**Sucesso do 6.3**: `rco_client.exe` compila, roda igual. Nada no client depende mais do caminho antigo.

---

## Passo 6.4 — Remover APIs mortas de `Actor`

A Fase 4.4 substituiu `Actor::Render` e `Actor::RenderAs` por `Actor::Submit` e `Actor::SubmitAs`, mas por precaução deixou as versões antigas no header — agora removemos.

### 6.4.a — Grep antes de remover

```bash
grep -rn "\.Render(" client/src/core/main.cpp | grep -v "pipeline\|chat\|inventory\|spellbar\|float_nums\|spell_fx\|chat_bubbles\|login_screen\|char_select\|particles"
grep -rn "\.RenderAs(" client/
```

**Resultado esperado**: zero matches pra `Actor::Render` e `Actor::RenderAs`. As `\.Render(` remanescentes são de UI (ImGui windows), chat, spellbar etc. — não são do Actor.

Se sobrou `player_actor.Render(...)` ou `.RenderAs(...)` em algum lugar, a Fase 4.4 não foi finalizada.

### 6.4.b — Remover do `actor.h`

Apagar:
- `void Render(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cam_pos, const glm::vec3& sun_dir);`
- `void RenderAs(const std::string& anim_name, float anim_t, bool loop, const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cam_pos, const glm::vec3& sun_dir);`
- `void SetupModelUniforms(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cam_pos, const glm::vec3& sun_dir);`
- `Shader shader_;` (o shader do actor não existe mais — ele submete pelo pipeline)
- `#include "rco/renderer/shader.h"` (não precisa mais)

### 6.4.c — Remover do `actor.cpp`

Apagar as implementações correspondentes: `Actor::Render`, `Actor::RenderAs`, `Actor::SetupModelUniforms`, qualquer compilação de shader em `Actor::Init` (já devia ter sido removida na Fase 4.4, conferir), e a parte antiga de `UploadBones` se ela ainda existe separada da `UploadBonesToSSBO_`.

O `Actor::Init` pós-limpeza deve ter só: `model_.Load(model_path)` e `material_index_ = engine->materials().RegisterFromTextures(...)`.

**Sucesso do 6.4**:
- `rco_client.exe` compila.
- Actors continuam renderizando.
- `grep -rn "Actor::Render\|Actor::RenderAs\|SetupModelUniforms" client/` retorna zero.

---

## Passo 6.5 — Remover APIs mortas de `Terrain`

Mesma ideia — a Fase 4.2 substituiu `Terrain::Render` por `Terrain::Submit(Pipeline&)`. Remover o resto.

### 6.5.a — Remover do `terrain.h`

Apagar:
- `void Render(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cam_pos, const glm::vec3& sun_dir);`
- `Shader shader_;`
- `#include "rco/renderer/shader.h"`

Manter (ainda em uso):
- `Init(int grid_w, int grid_h)` — versão nova
- `LoadFromEditor`
- `SampleHeight`
- `Destroy`
- `Submit(Pipeline&)`
- Tudo dos materiais e splatmap (a própria Terrain carrega texturas do disco via `LoadLinearTex`/`LoadSRGBTex`)

### 6.5.b — Remover do `terrain.cpp`

- Deletar `Terrain::Render(...)` inteira.
- Em `Terrain::Init`: apagar qualquer `shader_.Load(...)` e chamadas de `glGetUniformLocation` — se ainda existirem.
- O resto da classe (`GenerateProcedural`, `RebuildChunksFromHmap`, `MakeSolidTex`, etc.) permanece — são helpers de dado, não de render.

**Grep de sanidade**:
```bash
grep -rn "Terrain::Render\|terrain\.Render(" client/
```
Zero matches.

**Sucesso do 6.5**:
- `rco_client.exe` compila, terrain aparece igual.
- Terrain não tem mais objeto `Shader` dentro.

---

## Passo 6.6 — Verificar que `terrain_chunk.{h,cpp}` sobrevive

`client/src/renderer/terrain/terrain_chunk.{h,cpp}` **não** é código morto. Confirmar:

```bash
grep -rn "TerrainChunk\|terrain_chunk" client/
```

Deve aparecer em:
- `client/src/renderer/terrain/terrain.h` (include + uso de `std::unique_ptr<TerrainChunk>`)
- `client/src/renderer/terrain/terrain.cpp` (instanciação, `new TerrainChunk`, `chunk->vao()`, etc.)
- Nas assinaturas de `Terrain::Submit` quando passa chunk.vao/vbo/ebo pro `TerrainChunkSubmission`.

Se o grep retornar só isso: **não mexer**. `TerrainChunk` guarda o VAO/VBO/EBO de cada chunk do cliente, coisa que o `Engine` não sabe (ele opera com static scene batched ou submissões dinâmicas; chunks de terrain são o segundo caso).

### 6.6.a — Garantir getters expostos

A Fase 4.2.e cobra getters (`chunk->vao() / vbo() / ebo() / index_count() / model_matrix()`) no `TerrainChunk`. Se não foram adicionados na Fase 4, adicionar agora. Senão o `Terrain::Submit` nem compila.

No `terrain_chunk.h`:
```cpp
GLuint vao()          const { return vao_; }
GLuint vbo()          const { return vbo_; }
GLuint ebo()          const { return ebo_; }
int    index_count()  const { return idx_count_; }
glm::mat4 model_matrix() const { return model_; }
```

**Sucesso do 6.6**: nenhuma mudança de arquivo; só verificação.

---

## Passo 6.7 — Revisar `client/CMakeLists.txt`

Conferir que o CMakeLists não arrasta nada morto:

```bash
grep -n "skybox\|actor_old\|shader_old\|model_old" client/CMakeLists.txt
```

Zero matches esperado. Se aparecer `src/renderer/skybox.cpp` (deveria ter saído na Fase 4.6), remover agora:

```cmake
# antes
add_executable(rco_client
    ...
    src/renderer/skybox.cpp   # <-- REMOVER
    src/renderer/particles.cpp
    ...
)
```

**Grep final complementar**:
```bash
grep -rn "#include.*skybox" client/
```
Zero matches.

**Sucesso do 6.7**: nenhuma referência a `skybox` ou `*_old` em nenhum CMakeLists.

---

## Passo 6.8 — Purga de includes órfãos

Depois das Fases 4/5/6 removendo membros e classes, vários `#include` ficam pendurados. Rodar uma varredura manual.

### 6.8.a — Lista dos suspeitos por arquivo

| Arquivo | Incluía | Ainda precisa? |
|---|---|---|
| `client/src/core/main.cpp` | `"../renderer/skybox.h"` | **não** — já removido na Fase 4.6 |
| `client/src/renderer/actors/actor.h` | `"rco/renderer/shader.h"` | **não** — actor não toca shader |
| `client/src/renderer/terrain/terrain.h` | `"rco/renderer/shader.h"` | **não** — terrain não toca shader |
| `tools/gue/src/preview_viewport.h` | `"rco/renderer/shader.h"` | **não** — preview usa pipeline |
| `tools/terrain-editor/src/main.cpp` | qualquer `glShader*` manual | **não** — builder antigo saiu na 5.1.c |

Para cada entrada: abrir o arquivo, apagar o include se o conteúdo não depende mais dele.

### 6.8.b — Teste iterativo

Depois de remover N includes:

```bash
cd client && cmake --build build --config Release
```

Se compilar, os includes eram mesmo mortos. Se falhar com "undefined class X": reverter só aquele include e continuar.

**Sucesso do 6.8**: compilação limpa + header dependency reduzida.

---

## Passo 6.9 — Grep de sanidade final

Rodar todos esses e garantir zero matches (resultado esperado entre parênteses):

```bash
# Nada do transition
grep -rn "_old\." shared/renderer/                              # 0
grep -rn "_orig_renderer\|_stub_camera\|_stub_input" shared/    # 0
grep -rn "kUseNewPipelineForTerrain" client/                    # 0

# Nenhum shader antigo referenciado
grep -rn "actor\.\(vert\|frag\)\|terrain\.\(vert\|frag\)\|skybox\.\(vert\|frag\)" \
     client/ tools/ shared/                                     # 0

# Nenhuma API antiga
grep -rn "Actor::Render\|Actor::RenderAs\|SetupModelUniforms" client/   # 0
grep -rn "Terrain::Render" client/                              # 0
grep -rn "Skybox"  client/                                      # 0

# Nenhum TODO de migração deixado aberto
grep -rn "TODO FASE\|TODO migra\|FIXME pipeline" client/ shared/ tools/ # 0
```

Se alguma linha retornar, listar e decidir caso-a-caso: ou é bug legítimo remanescente (corrigir), ou é falso positivo (ajustar o grep).

---

## Passo 6.10 — Validação final + screenshot comparison

### 6.10.a — Build clean

```bash
rm -rf client/build tools/gue/build tools/terrain-editor/build shared/renderer/build
```

(No Windows: `rmdir /s /q client\build`, etc.)

Então rebuild cada um:

```bash
cd client && cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake && cmake --build build --config Release
cd tools/gue && cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake && cmake --build build --config Release
cd tools/terrain-editor && cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake && cmake --build build --config Release
```

Todos compilam do zero sem erros.

### 6.10.b — Smoke test funcional

Rodar cada exe e checar:

- `rco_server.exe` inicia e aceita conexão.
- `rco_client.exe`:
  - login + char select + entrar no mundo
  - terrain visível com PBR+shadows+IBL
  - player actor animando (Idle, Walk, Attack, Death)
  - NPCs animando e atacando
  - Particles (matar NPC → Explosion emitter com depth test correto)
  - Skybox HDRI (não mais gradiente procedural)
  - Portal atravessa áreas
  - UI ImGui (inventory, char sheet, chat, spellbar) renderizando em cima
  - FPS ≥ 60 em hw dev
- `rco_terrain.exe`:
  - abrir, carregar área, sculpt + paint funcionando
  - brush overlay ring visível e com depth test
  - save produz `.bin` válidos que o client carrega
- `rco_gue.exe`:
  - todas as abas (Items, Spells, Actors, Areas, Media) funcionando
  - Media > Actor Defs preview renderiza o modelo com PBR

### 6.10.c — Screenshot diff

Comparar screenshots do pré-Fase-6 (do commit snapshot inicial) com screenshots pós-Fase-6:

```
screenshot-pre-fase6.png
screenshot-pos-fase6.png
```

Diff via `magick compare -metric RMSE pre.png pos.png diff.png` (ou equivalente). O diff deve ser **idêntico** ou pixel-perfeito (RMSE < 1.0). Se apareceu diferença, **algo removido ainda era usado** — reverter e identificar.

### 6.10.d — Commitar

```bash
git add -A
git commit -m "fase-6: limpeza pós-migração do renderer"
```

---

## Critérios de sucesso da Fase 6

- [ ] Os 6 shaders antigos (`actor.*`, `terrain.*`, `skybox.*`) deletados de `dist/client/shaders/`.
- [ ] `particle.vert/.frag` **preservados**.
- [ ] 4 arquivos `*_old` deletados de `shared/renderer/`.
- [ ] `shared/renderer/CMakeLists.txt` sem referências a `*_old`.
- [ ] `kUseNewPipelineForTerrain` removido de `main.cpp`.
- [ ] `Actor::Render/RenderAs/SetupModelUniforms` removidos.
- [ ] `Terrain::Render` e membro `shader_` removidos.
- [ ] Nenhum `#include "../renderer/skybox.h"` em `main.cpp`.
- [ ] Todos os 9 greps do passo 6.9 retornam zero matches.
- [ ] Build clean de todos os targets (client + 2 tools) sem erros.
- [ ] Smoke test funcional completo passa.
- [ ] Screenshot diff vs pré-Fase-6 é idêntico (pixel-perfect).

Quando todos os critérios passam, a migração do renderer está **concluída**.

---

## Estado final do repositório após a Fase 6

```
shared/renderer/
├── include/rco/renderer/
│   ├── engine.h          ✓ Fase 2
│   ├── pipeline.h        ✓ Fase 2
│   ├── shader.h          ✓ Fase 1.5.i (convertido)
│   ├── model.h           ✓ RCO (skinned)
│   ├── texture.h         ✓ Fase 1.5.f
│   ├── mesh.h            ✓ Fase 1.5.h
│   ├── material.h        ✓ Fase 1.5.g
│   ├── light.h           ✓ Fase 1.5.c
│   ├── object.h          ✓ Fase 1.5.d
│   ├── buffers.h         ✓ Fase 1.5.e
│   ├── indirect.h        ✓ Fase 1.5.b
│   ├── utilities.h       ✓ Fase 1.5.a
│   └── helpers.h         ✓ Fase 1.5.j + Fase 2 (GetUnitLightSphere)
└── src/
    ├── engine.cpp        ✓ Fase 2
    ├── pipeline.cpp      ✓ Fase 2
    ├── shader.cpp        ✓ Fase 1.5.i
    ├── model.cpp         ✓ RCO
    ├── texture.cpp       ✓ Fase 1.5.f
    ├── mesh.cpp          ✓ Fase 1.5.h
    ├── material.cpp      ✓ Fase 1.5.g
    ├── light.cpp         ✓ Fase 1.5.c
    ├── buffers.cpp       ✓ Fase 1.5.e
    ├── helpers.cpp       ✓ Fase 1.5.j + Fase 2
    ├── compile_shaders.cpp  ✓ Fase 1.5.j + Fase 4 + Fase 5
    └── stb_image_impl.cpp   ✓ pré-existente

dist/client/shaders/   ← 35 shaders glRenderer + terrainGBuffer.* + brush_overlay.* + particle.*

client/src/renderer/
├── actors/
│   ├── actor.h          ✓ submete via Pipeline (sem Shader interno)
│   └── actor.cpp
├── terrain/
│   ├── terrain.h        ✓ submete via Pipeline (sem Shader interno)
│   ├── terrain.cpp
│   ├── terrain_chunk.h  ✓ mantido (per-chunk VAO/VBO)
│   └── terrain_chunk.cpp
├── camera.h / .cpp      ✓ mantido
├── particles.h / .cpp   ✓ mantido (forward pass via callback)
└── (skybox.h/.cpp)      ✗ deletado (Fase 4.6)

tools/terrain-editor/src/
├── main.cpp             ✓ sem shaders inlineados; usa Engine+Pipeline
├── terrain_renderer.*   ✓ Submit(pipeline) em vez de Render()
├── heightmap.*          ✓ mantido
├── splatmap.h           ✓ mantido
├── material.h           ✓ mantido
├── brush.h / camera.h   ✓ mantido
└── (stb_impl.cpp)       ✗ deletado (Fase 5.1.b)

tools/gue/src/
├── main.cpp             ✓ Engine+Pipeline compartilhados com preview
├── preview_viewport.*   ✓ renderiza via Engine, exibe via ImGui::Image
├── file_import.*        ✓ mantido
└── tabs/ ...            ✓ mantido
```

---

## Problemas comuns e como resolver

**Visual mudou após a Fase 6**
→ Algo removido ainda era usado. Abrir RenderDoc, capturar frame, comparar com frame pré-Fase-6. Reverter o último passo e tentar isoladamente.

**Build do `rco_terrain` falha após 6.2: "cannot find shader_old.h"**
→ `preview_viewport.cpp` ou `main.cpp` do terrain-editor ainda tem `#include "rco/renderer/shader_old.h"`. Buscar e remover — foi esquecido na Fase 5.

**`actor.h` inclui `shader.h` mas o client não compila mais**
→ Se remover `#include "rco/renderer/shader.h"` de `actor.h` quebrar compilação de alguém que INCLUIU `actor.h` e precisava do `Shader` por transitividade, isso é uma dependência escondida que precisa ser explicitada. Adicionar `#include "rco/renderer/shader.h"` no arquivo que usa, não voltar o include em `actor.h`.

**Screenshot diff mostra diferença pequena mas não zero**
→ Pode ser:
1. Ordem de passes mudou sutilmente — verificar `pipeline.cpp::End` não teve refactor durante a Fase 6.
2. FPS afeta `dt` afeta animação — comparar em frame estático (pause).
3. Tonemap histogram precisa de alguns frames para estabilizar — capturar screenshot depois de 3s no mesmo lugar.

**Depois de deletar `shader_old.h/.cpp`, `git status` mostra arquivo deletado mas ainda aparece no Visual Studio**
→ Cache do CMake. Deletar a pasta de build e reconfigurar:
```bash
rm -rf client/build
cd client && cmake -B build ...
```

**Grep do passo 6.9 ainda pega `particle.vert` ou `particle.frag`**
→ O grep estava mal escrito. Revisar — deve ser só `actor.\(vert\|frag\)`, `terrain.\(vert\|frag\)`, `skybox.\(vert\|frag\)`. Particles continuam vivos.

**`particles.cpp` aparentemente quebrou depois do 6.5**
→ `particles.h` incluía `"rco/renderer/shader.h"` (usa `Shader` próprio internamente) — NÃO remover esse include. Particles **continuam** gerenciando seu próprio shader no forward pass. Apenas `actor.h` e `terrain.h` perderam o include, não `particles.h`.

**O client compila mas o terrain-editor não linka (`undefined symbol: CompileShaders`)**
→ O `compile_shaders.cpp` está dentro do `rco_renderer.lib` — o terrain-editor deveria linkar essa lib. Conferir `target_link_libraries(rco_terrain PRIVATE rco_renderer ...)` no CMakeLists (Fase 5.1.a).

**Após tudo, FPS caiu em relação ao pré-Fase-1**
→ Esperado. Pipeline deferred tem custo base maior que o forward antigo. Ganho visual (PBR + CSM + SSAO + IBL + volumetrics) compensa. Se for problema, desligar features que não agregam ao RCO:
- `FeatureConfig::volumetrics = false` (~3 ms/frame)
- `FeatureConfig::ssr = false` (já é default)
- Baixar `shadow_width/_height` pra 512 em `EngineConfig`
- Reduzir `volumetric_.atrous_passes` para 0
