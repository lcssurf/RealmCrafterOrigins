# Arquitetura: Landscape RCO — Paridade de Qualidade com UE

> Baseado na análise do código UE (`D:\Github\UnrealEngine`) e do estado atual do RCO  
> (`tools/gue/src/terrain/`, `dist/client/shaders/terrainGBuffer.*`, `client/src/renderer/terrain/`).

---

## Diagnóstico: por que o UE terrain parece melhor

O shader do cliente já está num nível avançado: triplanar mapping, LB_HeightBlend, normal blend por octahedral encoding, PBR GBuffer. O gap de qualidade não é falta de feature — são problemas estruturais que degradam o resultado.

### Problema 1 — Tiling único para todos os layers

**Arquivo:** `terrainGBuffer.fs:98` — `float tile = u_tiling;`

Todos os 4 layers compartilham o mesmo UV scale. No UE cada layer tem seu próprio tiling. Isso é crítico porque materiais têm escalas naturais diferentes:

```
Grama fina  → tiling alto  (ex: 20) — detalhe pequeno
Rocha       → tiling médio (ex: 10) — detalhe médio  
Neve        → tiling baixo (ex:  5) — grandes manchas
```

Com tiling único, ou a grama fica borrada ou a rocha fica com padrão repetitivo óbvio. **É o maior gap visual do terrain atual.**

---

### Problema 2 — Repetição de tiling visível à distância

O shader aplica o mesmo tiling em qualquer distância. À medida que a câmera se afasta, o padrão repetitivo do tile se torna óbvio (o cérebro humano detecta padrões repetitivos de imediato).

O UE usa duas técnicas para mascarar isso:

**a) Macro variation texture** — uma textura de baixa frequência (não-tileable) sobreposta ao material que quebra a uniformidade. Costuma ser uma textura de variação de cor/darkening 512×512 não repetida que cobre o terreno inteiro.

**b) Distance-based tiling fade** — a frequência do tiling aumenta com a distância (ou o material faz blend para uma cor base flat sem tiling). Isso é feito com `dFdx`/`dFdy` para detectar o LOD da textura automaticamente, ou com `distance(cameraPos, worldPos)`.

---

### Problema 3 — Normais com descontinuidade em bordas de chunk

**Arquivo:** `editable_terrain.cpp:BuildChunk()` — normais calculadas via diferenças finitas em CPU.

Quando só o chunk A é marcado dirty e reconstruído, seus vértices de borda calculam normais usando vizinhos que estão no chunk B (não reconstruído). Se o chunk B teve altura diferente da última vez que foi construído, as normais de borda não coincidem.

Resultado: seam visível (linha) nas fronteiras dos chunks em terrenos inclinados.

O UE resolve isso porque as normais são derivadas da heightmap texture no shader — contínua entre chunks, sem seam possível.

---

### Problema 4 — Editor mostra um terrain diferente do jogo

**GUE shader** (`editable_terrain.cpp:kTerrainFS`): blending linear simples, sem triplanar, sem LB_HeightBlend, tiling uniforme.

**Cliente shader** (`terrainGBuffer.fs`): triplanar + LB_HeightBlend + normals corretas.

O que o artista vê ao pintar no GUE é completamente diferente do que o jogador vê. No UE o editor usa o mesmo material shader do runtime.

---

### Problema 5 — LOD com popping visual

O cliente usa LOD discreto: chunks distantes pulam para um nível com menos vértices abruptamente. Isso cria "flicker" visível.

O UE usa CLOD (Continuous LOD): cada vértice interpola suavemente entre a sua posição em LOD N e LOD N+1, controlado pela fração do LOD calculada no vertex shader. A transição é invisível.

---

### Problema 6 — Pesos do splatmap não somam exatamente 1.0

**Arquivo:** `splatmap.h:PaintSplatmap()`

A redistribuição dos outros canais ao pintar um layer usa `weights[i] - weights[i] * ratio`. Para floats com operações múltiplas, a soma pode drift de 1.0. O GBuffer shader divide por `wsum = max(r+g+b+a, 1e-4)` para compensar, mas o drift acumulado após muitas pinceladas cria artefatos sutis de cor.

---

### Problema 7 — Normal map comprimida para 8 bits no VBO

As normais dos vértices do terrain são calculadas por `glm::normalize(vec3(hl-hr, 2*cs, hd-hu))` e armazenadas como `float` no VBO (3 × 4 bytes = 12 bytes por vértice só para a normal). No UE as normais são recalculadas no VS a partir da heightmap texture: custo zero de memória no VBO, qualidade maior porque usa a textura filtrada (não finitas-diferenças com step = 1 célula).

---

## Arquitetura Alvo

### Visão geral das camadas

```
┌──────────────────────────────────────────────────────────────────┐
│  ZonesTab   — input, UI, undo/redo, brush state                  │
│                                                                   │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  EditableTerrain                                           │  │
│  │                                                             │  │
│  │  Heightmap  ──── R32F GPU texture ──────► VS height sample │  │
│  │  Splatmap   ──── RGBA8 GPU texture ─────► FS weight sample │  │
│  │  Materials[4]    per-layer tiling[4]                       │  │
│  │  Chunks[]        VAO/VBO (XZ only — Y from texture)        │  │
│  │  MacroVariation  one texture covering full terrain         │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                   │
│  TerrainUndoStack   (heightmap + splatmap snapshots, 30 entries) │
└──────────────────────────────────────────────────────────────────┘

Cliente (runtime, separado):
  terrainGBuffer.fs  ──  mesmo shader, estendido com per-layer tiling
                         + macro variation + distance fade
```

---

## Plano de Implementação

### ✅ Fase 1 — Per-layer tiling  *(maior impacto visual, baixa complexidade)*

**O que muda:**

`u_tiling` (float único) → `u_tiling[4]` (vec4, um por layer)

**`terrainGBuffer.fs`:**

```glsl
// ANTES:
uniform float u_tiling;
// ...
vec3 alb = triplanar(wp, bw, u_mat0_albedo, tile) * W.r + ...

// DEPOIS:
uniform vec4 u_tilings;   // xyzw = tiling por layer
// ...
vec3 alb = triplanar(wp, bw, u_mat0_albedo, u_tilings.x) * W.r
         + triplanar(wp, bw, u_mat1_albedo, u_tilings.y) * W.g
         + triplanar(wp, bw, u_mat2_albedo, u_tilings.z) * W.b
         + triplanar(wp, bw, u_mat3_albedo, u_tilings.w) * W.a;
// idem para normal, roughness, ao, height
```

**`terrain.h`:** `MatTex::tiling` já existe por slot — apenas passar como vec4 no draw call.

**`editable_terrain.h`:**

```cpp
// Substituir:  float tiling = 8.f;
// Por:
std::array<float, kMaxMats> tilings = {8.f, 8.f, 8.f, 8.f};
```

**Persistência:** `materials.txt` já suporta `<id> <tiling>` por linha — sem mudança de formato.

**GUE shader** (`editable_terrain.cpp:kTerrainFS`): mesma mudança, `u_Tiling` → `u_Tilings` vec4.

**Arquivos:** `terrainGBuffer.fs`, `terrain.cpp`, `editable_terrain.h/.cpp`, `zones.cpp` (UI sliders)

---

### ✅ Fase 2 — Macro variation (quebrar repetição de tiling)

**O que é:** Uma textura única de 512×512 que cobre o terreno inteiro (não se repete). Contém variação de luminosidade/cor de baixa frequência — manchas escuras e claras, variação sutil de saturação. Multiplica (overlay blend) sobre a cor final do terrain.

**Por que funciona:** O cérebro reconhece padrões repetitivos porque cada tile é idêntico. A macro variation garante que cada região do terrain seja única, mesmo que os tiles se repitam.

**`terrainGBuffer.fs` — adicionar após o blending:**

```glsl
uniform sampler2D u_macroVariation;   // nova textura
uniform float     u_macroStrength;    // 0.0 = off, 0.3 = sutil, 1.0 = forte

// Sample da macro variation em UV do terrain (sem tiling — cobre o terrain inteiro)
vec3 macro = texture(u_macroVariation, uvT).rgb;  // uvT já existe (splatmap UV)

// Overlay blend: escurece onde macro < 0.5, clareia onde macro > 0.5
// Fórmula: se c < 0.5: 2*base*c; se c >= 0.5: 1 - 2*(1-base)*(1-c)
vec3 blended = mix(
    2.0 * alb * macro,
    1.0 - 2.0 * (1.0 - alb) * (1.0 - macro),
    step(0.5, macro)
);
alb = mix(alb, blended, u_macroStrength);
```

**Onde gerar a textura:** o GUE pode gerar automaticamente uma macro variation procedural (value noise de baixa frequência) ao criar uma área, ou o artista importa uma textura customizada. Salvar como `dist/client/data/areas/<name>/macro.png`.

**`EditableTerrain`:** adicionar `GLuint macroTex_` e campo `float macroStrength = 0.25f`.

**Arquivos:** `terrainGBuffer.fs`, `terrain.cpp/.h`, `editable_terrain.h/.cpp`, `zones.cpp` (slider de strength + botão "Generate Macro")

---

### ✅ Fase 3 — Distance tiling fade (eliminar padrão repetitivo em distância)

**O que é:** À medida que o fragment fica distante da câmera, o tiling é reduzido (escalado) de forma que o padrão visual seja o mesmo tamanho em espaço de tela.

**Implementação via MIP automático do GLSL:**

```glsl
// terrainGBuffer.fs — adicionar uniform:
uniform vec3  u_cameraPos;
uniform float u_tilingFadeStart;   // distância onde começa o fade (ex: 80)
uniform float u_tilingFadeEnd;     // distância onde tiling fica 1 (ex: 300)

// No main(), antes de amostrar as texturas:
float camDist = distance(v_worldPos, u_cameraPos);
// Reduz tiling à distância — equivale a aumentar o UV scale
float distFactor = 1.0 + 3.0 * smoothstep(u_tilingFadeStart, u_tilingFadeEnd, camDist);
// distFactor: 1.0 perto, 4.0 longe — os tiles ficam 4x maiores em distância

vec4 effectiveTilings = u_tilings * distFactor;
// usar effectiveTilings no lugar de u_tilings
```

**Combinado com macro variation**, isso elimina quase completamente a aparência repetitiva.

**Arquivos:** `terrainGBuffer.fs`, `terrain.cpp` (passar cameraPos + fade params)

---

### ✅ Fase 4 — Normais derivadas da heightmap no vertex shader

**Por que:** Resolve o seam de normais entre chunks (Problema 3) e elimina 12 bytes/vértice do VBO.

**Arquitetura:**

```
ANTES:
  CPU: float heights[] → BuildChunk() → VBO com pos(xyz) + normal(xyz) + uv(xy) + tangent(xyz)
  = 11 floats/vértice = 44 bytes/vértice

DEPOIS:
  CPU: float heights[] → GLuint heightTex (R32F, upload por região dirty)
  GPU: VS amostra heightTex → calcula normal por diferenças finitas no shader
  VBO com apenas pos_xz(xy) + uv(xy) = 4 floats/vértice = 16 bytes/vértice
  (Y é lido da textura no VS)
```

**`heightmap.h` — adicionar upload GPU:**

```cpp
class Heightmap {
public:
    // ... campos existentes ...
    GLuint tex = 0;   // R32F texture, dimensões W×H

    // Chama glTexSubImage2D apenas na região dirty (x0,z0)→(x1,z1)
    void UploadRegion(int x0, int z0, int x1, int z1);
    void UploadFull();
    void InitGPU();    // glGenTextures + glTexImage2D(R32F)
    void DestroyGPU();
};
```

**Vertex shader novo (`terrainGBuffer.vs`):**

```glsl
layout(location = 0) in vec2 a_gridPos;   // XZ em células (não posição mundial)

uniform sampler2D u_heightmap;
uniform vec2      u_hmSize;        // W, H em células
uniform float     u_cellSize;      // 2.0
uniform vec2      u_hmOrigin;      // offset do terrain no mundo

void main() {
    vec2 uv  = a_gridPos / u_hmSize;
    float h  = texture(u_heightmap, uv).r;

    // Normais por diferenças finitas no shader (continuum — sem seam)
    vec2  dxUV = vec2(1.0 / u_hmSize.x, 0.0);
    vec2  dzUV = vec2(0.0, 1.0 / u_hmSize.y);
    float hL   = texture(u_heightmap, uv - dxUV).r;
    float hR   = texture(u_heightmap, uv + dxUV).r;
    float hD   = texture(u_heightmap, uv - dzUV).r;
    float hU   = texture(u_heightmap, uv + dzUV).r;

    vec3 normal  = normalize(vec3(hL - hR, 2.0 * u_cellSize, hD - hU));
    vec3 tangent = normalize(vec3(2.0 * u_cellSize, hR - hL, 0.0));

    vec3 worldPos = vec3(
        u_hmOrigin.x + a_gridPos.x * u_cellSize,
        h,
        u_hmOrigin.y + a_gridPos.y * u_cellSize
    );

    v_worldPos = worldPos;
    v_normal   = normal;
    v_tangent  = tangent;
    v_uv       = worldPos.xz;   // world-space UV para triplanar
    gl_Position = u_viewProj * vec4(worldPos, 1.0);
}
```

**`BuildChunk` simplificado:**

```cpp
// Apenas XZ coords — 2 floats por vértice
struct TerrainVert { float x, z; };
// VBO: kVerts × kVerts × 8 bytes (era 44 bytes → 5.5× menor)
```

**MarkDirtyRegion → UploadRegion:** em vez de marcar chunk como dirty para rebuild, faz `heightmap_.UploadRegion(x0, z0, x1, z1)` diretamente. O VBO nunca precisa ser reconstruído para brush de altura.

> ⚠️ Esta é a refatoração mais ampla. Mudar em branch separada. Clientes já existentes continuam funcionando — apenas o VBO layout e o VS mudam.

**Arquivos:** `terrainGBuffer.vs`, `heightmap.h/.cpp`, `editable_terrain.h/.cpp`, `terrain_chunk.h/.cpp`, `terrain.cpp/.h`

---

### ✅ Fase 5 — Editor shader = cliente shader

**Problema:** o GUE mostra um terrain diferente do jogo.

**Solução:** o GUE já tem `SubmitToPipeline()` que usa o pipeline do cliente. O `DrawViewport` do GUE deve usar esse path como padrão, não o shader simples inline.

```cpp
// editable_terrain.cpp:RenderFrame — renomear para RenderSimple (uso debug apenas)
// O viewport do zones usa SubmitToPipeline por padrão

// zones.cpp:DrawViewport:
// ANTES: renderer_.terrain().RenderFrame(vp, sunDir);
// DEPOIS: 
if (usePbrPreview_) {
    renderer_.terrain().SubmitToPipeline(*pipeline_);
    pipeline_->Flush(...);   // render deferred
} else {
    renderer_.terrain().RenderFrame(vp, sunDir);   // quick debug
}
```

**Também:** replicar as Fases 1–3 no shader inline do editor para que mesmo o modo simples esteja alinhado.

**Arquivos:** `editable_terrain.cpp`, `zones.cpp`, `zones.h` (flag `usePbrPreview_`)

---

### ✅ Fase 6 — Splatmap RGBA8 (não RGBA32F)

**Problema atual:** `splatmap_.data` usa `float` (32 bits por canal = 128 bits por pixel). O UE usa 8 bits por canal (32 bits por pixel) — 4× mais leve.

A precisão de 8 bits (256 níveis) é completamente suficiente para pesos visuais. Nenhum artista consegue distinguir `0.502` de `0.498` visualmente.

**`splatmap.h`:**

```cpp
struct Splatmap {
    // Substituir:  std::vector<float> data;   // W*H*4 floats
    // Por:
    std::vector<uint8_t> data;   // W*H*4 bytes — 4× menor

    float* At(...)   // REMOVER
    uint8_t* AtU(int x, int z) { return &data[(z * W + x) * 4]; }
    void SetWeight(int x, int z, int ch, float v) {
        AtU(x,z)[ch] = (uint8_t)std::clamp((int)(v * 255.f + 0.5f), 0, 255);
    }
    float GetWeight(int x, int z, int ch) const {
        return data[(z * W + x) * 4 + ch] / 255.f;
    }

    // Upload: GL_RGBA8 (não GL_RGBA32F)
    glTexImage2D(..., GL_RGBA8, ..., GL_RGBA, GL_UNSIGNED_BYTE, data.data());
};
```

**Normalização exata:** com uint8 a soma dos pesos pode ser 253–257 (erro de arredondamento). O shader já divide por `wsum = max(sum, 1e-4)` — continua correto.

**Formato de arquivo:** o RSPM atual salva `float*` diretamente. Mudar para bytes requer versão 2 do formato (magic diferente: `RSPM` → `RSP2`) com conversão automática ao ler arquivos antigos.

**Memória:** 512×512 splatmap: `float` = 4 MB → `uint8` = 1 MB. GPU: mesma melhoria.

**Arquivos:** `splatmap.h`, `editable_terrain.cpp` (upload format), `terrain.cpp` (idem)

---

### ✅ Fase 7 — Undo/Redo para terrain  *(UX crítico)*

**Design:**

```cpp
// zones.h — novo bloco:
struct TerrainSnapshot {
    std::vector<float>   heights;   // cópia de heightmap_.heights
    std::vector<uint8_t> splat;     // cópia de splatmap_.data (após Fase 6: bytes)
};

static constexpr int kMaxTerrainUndo = 30;
std::vector<TerrainSnapshot> terrainUndo_;
std::vector<TerrainSnapshot> terrainRedo_;

// Capturar ANTES do primeiro frame de cada stroke (não a cada frame)
bool terrainStrokeActive_ = false;
```

**Fluxo no `DrawViewport`:**

```cpp
// MouseButtonDown (início do stroke):
if (!terrainStrokeActive_) {
    terrainStrokeActive_ = true;
    TerrainSnapshot snap;
    snap.heights = terrain.heightmap().heights;   // cópia O(N)
    snap.splat   = terrain.splatmap().data;
    if (terrainUndo_.size() >= kMaxTerrainUndo)
        terrainUndo_.erase(terrainUndo_.begin());
    terrainUndo_.push_back(std::move(snap));
    terrainRedo_.clear();
}

// MouseButtonUp:
terrainStrokeActive_ = false;

// Ctrl+Z (apenas quando zoneMode_ == kModeTerrain):
if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z) && !terrainUndo_.empty()) {
    // snapshot do estado atual vai pro redo
    TerrainSnapshot cur;
    cur.heights = terrain.heightmap().heights;
    cur.splat   = terrain.splatmap().data;
    terrainRedo_.push_back(std::move(cur));
    // restaura
    auto& prev = terrainUndo_.back();
    terrain.heightmap().heights = prev.heights;
    terrain.splatmap().data     = prev.splat;
    terrain.splatmap().dirty    = true;
    terrain.MarkDirtyAll();
    terrainUndo_.pop_back();
}
// Ctrl+Y: inverso
```

**Custo:** após Fase 6, cada snapshot = 1 MB (heights) + 1 MB (splat) = 2 MB. 30 snapshots = 60 MB. Aceitável.

**Arquivos:** `zones.h`, `zones.cpp`

---

### ✅ Fase 8 — Falloff types  *(sensação de sculpt)*

```cpp
// brush.h — substituir GaussWeight por sistema extensível:

enum class BrushFalloff { Smooth, Gaussian, Linear, Spherical };

inline float CalcFalloff(float dist, float radius, BrushFalloff type) {
    float t = 1.f - std::clamp(dist / radius, 0.f, 1.f);
    switch (type) {
    case BrushFalloff::Linear:
        return t;
    case BrushFalloff::Smooth:        // Hermite — padrão UE
        return t * t * (3.f - 2.f * t);
    case BrushFalloff::Spherical:     // projeção de esfera
        return std::sqrt(std::max(0.f, 1.f - (1.f-t)*(1.f-t)));
    default:                          // Gaussian
        float sigma = radius * 0.35f;
        return std::exp(-(dist*dist) / (2.f*sigma*sigma));
    }
}
```

**UI:** `ImGui::Combo("Falloff##tf", &brushFalloff_, "Smooth\0Gaussian\0Linear\0Spherical\0");`

**Arquivos:** `brush.h`, `zones.h` (campo `int brushFalloff_ = 0`), `zones.cpp`

---

### ✅ Fase 9 — Brush Noise + Auto-paint por slope  *(workflow)*

**Noise brush:**

```cpp
// brush.h:
case BrushMode::Noise: {
    // Value noise 2D sem dependência externa
    float nx = (float)gx / (radius / heightmap_.cell_size * 0.5f);
    float nz = (float)gz / (radius / heightmap_.cell_size * 0.5f);
    auto hash = [](int x, int z) {
        unsigned s = (unsigned)(x*1619 + z*31337);
        s = (s^(s>>16)) * 0x45d9f3b; s ^= s>>16;
        return ((float)(s & 0xFFFF) / 65535.f) * 2.f - 1.f;
    };
    int ix = (int)nx, iz = (int)nz;
    float fx = nx-ix, fz = nz-iz;
    float ux = fx*fx*(3-2*fx), uz = fz*fz*(3-2*fz);
    float noise = glm::mix(glm::mix(hash(ix,iz), hash(ix+1,iz), ux),
                           glm::mix(hash(ix,iz+1), hash(ix+1,iz+1), ux), uz);
    hmap.Set(x, z, h + noise * w * 4.f);
    break;
}
```

**Auto-paint por slope:**

```cpp
// editable_terrain.h:
void AutoPaintBySlope(int flatLayer, int rockLayer, float minDeg, float maxDeg);
```

```cpp
// editable_terrain.cpp:
void EditableTerrain::AutoPaintBySlope(int flat, int rock, float mn, float mx) {
    float cs = heightmap_.cell_size;
    for (int z = 0; z < splatmap_.H; z++) {
        for (int x = 0; x < splatmap_.W; x++) {
            float dhdx = (heightmap_.Get(x+1,z) - heightmap_.Get(x-1,z)) / (2.f*cs);
            float dhdz = (heightmap_.Get(x,z+1) - heightmap_.Get(x,z-1)) / (2.f*cs);
            float slope = glm::degrees(std::atan(std::sqrt(dhdx*dhdx + dhdz*dhdz)));
            float t = glm::smoothstep(mn, mx, slope);
            splatmap_.SetWeight(x, z, flat, 1.f - t);
            splatmap_.SetWeight(x, z, rock, t);
            for (int i = 0; i < 4; i++)
                if (i != flat && i != rock) splatmap_.SetWeight(x, z, i, 0.f);
        }
    }
    splatmap_.dirty = true;
}
```

**UI:** botão "Auto-paint slope" + sliders `slopeFlat / slopeRock` + combos de layer.

---

### ✅ Fase 10 — CLOD / Geomorphing  *(eliminar LOD popping)*

Esta é a fase mais complexa e depende da Fase 4 (heightmap como textura GPU).

**Com a heightmap como textura**, o vertex shader pode calcular a posição em qualquer LOD dinamicamente:

```glsl
// terrainGBuffer.vs — adicionar após a Fase 4:
uniform float u_lodLevel;       // LOD fracionário: ex. 1.7
uniform float u_lodBlendRange;  // zona de morph: 1.0

// Amostra height em resolução LOD N (skipando pixels)
float SampleLOD(sampler2D hm, vec2 baseUV, float lodN) {
    float step = exp2(lodN);                    // 1, 2, 4, 8...
    vec2 snappedUV = round(baseUV * u_hmSize / step) * step / u_hmSize;
    return texture(hm, snappedUV).r;
}

// No main():
float lodFrac  = fract(u_lodLevel);
float lodFloor = floor(u_lodLevel);
float hCur  = SampleLOD(u_heightmap, uv, lodFloor);
float hNext = SampleLOD(u_heightmap, uv, lodFloor + 1.0);
// Morph: interpola suavemente entre LODs
float morphAlpha = smoothstep(1.0 - u_lodBlendRange, 1.0, lodFrac);
float h = mix(hCur, hNext, morphAlpha);
```

**CPU:** calcular `u_lodLevel` por chunk baseado na distância câmera → chunk center.

---

## Ordem de implementação recomendada

| # | Fase | Impacto | Complexidade | Dias |
|---|---|---|---|---|
| ~~1~~ | ~~Per-layer tiling~~ | ★★★★★ | ~~Baixa~~ | ✅ |
| ~~2~~ | ~~Undo/Redo terrain~~ | ★★★★★ | ~~Baixa~~ | ✅ |
| ~~3~~ | ~~Falloff types~~ | ★★★★☆ | ~~Mínima~~ | ✅ |
| ~~4~~ | ~~Macro variation~~ | ★★★★☆ | ~~Média~~ | ✅ |
| ~~5~~ | ~~Distance tiling fade~~ | ★★★★☆ | ~~Baixa~~ | ✅ |
| ~~6~~ | ~~Splatmap RGBA8~~ | ★★★☆☆ | ~~Baixa~~ | ✅ |
| ~~7~~ | ~~Editor = client shader~~ | ★★★☆☆ | ~~Média~~ | ✅ |
| ~~8~~ | ~~Normais no VS (heightmap texture)~~ | ★★★☆☆ | ~~Alta~~ | ✅ |
| ~~9~~ | ~~Noise + Auto-paint slope~~ | ★★★☆☆ | ~~Média~~ | ✅ |
| ~~10~~ | ~~CLOD geomorphing~~ | ★★☆☆☆ | ~~Alta~~ | ✅ |

> As fases 1–6 somam ~6 dias de trabalho e eliminam ~90% da diferença visual percebida em relação ao UE.  
> As fases 7–10 são refinamentos de qualidade e workflow.

---

## Resumo dos arquivos alterados por fase

| Arquivo | Fases |
|---|---|
| `dist/client/shaders/terrainGBuffer.fs` | 1, 2, 3, 5 |
| `dist/client/shaders/terrainGBuffer.vs` | 4, 10 |
| `client/src/renderer/terrain/terrain.h/.cpp` | 1, 2, 3, 4 |
| `tools/gue/src/terrain/heightmap.h` | 4 |
| `tools/gue/src/terrain/splatmap.h` | 6 |
| `tools/gue/src/terrain/brush.h` | 3, 9 |
| `tools/gue/src/terrain/editable_terrain.h/.cpp` | 1, 2, 4, 9 |
| `tools/gue/src/tabs/zones.h` | 3, 7 |
| `tools/gue/src/tabs/zones.cpp` | 3, 5, 7, 9 |
