# Análise Técnica: Sistema de Landscape da Unreal Engine

> Baseado no código-fonte da UE 5.6+ em `D:\Github\UnrealEngine`.  
> Objetivo: entender os algoritmos para implementar funcionalidades equivalentes no RCO (C++ + OpenGL + GUE).

---

## Índice

1. [Armazenamento de Heightmap](#1-armazenamento-de-heightmap)
2. [Estrutura de Componentes](#2-estrutura-de-componentes)
3. [Sistema de LOD (CLOD)](#3-sistema-de-lod-clod)
4. [Crack Prevention](#4-crack-prevention)
5. [Material e Layer Blending](#5-material-e-layer-blending)
6. [Sculpting Tools](#6-sculpting-tools)
7. [Paint Tools](#7-paint-tools)
8. [Undo/Redo](#8-undoredo)
9. [Brush System](#9-brush-system)
10. [Rendering Pipeline](#10-rendering-pipeline)
11. [Constantes e Tamanhos](#11-constantes-e-tamanhos)
12. [Edit Layers (UE 5.x)](#12-edit-layers-ue-5x)
13. [O que o RCO já tem vs. gaps reais](#13-o-que-o-rco-já-tem-vs-gaps-reais)

---

## 1. Armazenamento de Heightmap

**Arquivos:** `LandscapeComponent.cpp`, `LandscapeEdit.cpp`

### Formato em disco

```
Textura 2D — formato R8G8 (16 bits por pixel)

Canal R = 8 bits mais significativos da altura
Canal G = 8 bits menos significativos da altura
Canal B = Normal X compactado (0-255 → -1.0 a +1.0)
Canal A = Normal Y compactado (0-255 → -1.0 a +1.0)

uint16 altura: 0–65535
- 32768 = altura zero (mid value)
- Cada unidade = 1/128 metros (LANDSCAPE_ZSCALE)
```

### Constantes

```cpp
#define LANDSCAPE_ZSCALE      (1.0f / 128.0f)
#define LANDSCAPE_INV_ZSCALE  128.0f

MaxValue = 65535   // uint16 max
MidValue = 32768   // zero height
```

### Leitura/escrita em disco (Editor)

- Classe `FLandscapeEditDataInterface` — gerencia lock/unlock dos mips
- `FLandscapeTextureDataInfo::GetMipData()` — ponteiro direto ao `UTexture2D::Source`
- Dirty regions rastreadas via `FUpdateTextureRegion2D`
- Upload parcial: só as regiões modificadas sobem para a GPU

### Carregamento na GPU (Runtime)

```cpp
UTexture2D* HeightmapTexture;       // uma por componente
FVector4 HeightmapScaleBias;        // UV scale+offset para este componente
```

---

## 2. Estrutura de Componentes

**Arquivos:** `Landscape.h`, `LandscapeComponent.h`

### Hierarquia

```
ALandscape  (actor root — detém edit layers e configuração global)
└── ULandscapeProxy  (gerencia o grid de componentes)
    ├── ULandscapeComponent  (render + heightmap + weightmaps)
    │   └── ULandscapeHeightfieldCollisionComponent  (física)
    │       └── Chaos::FHeightField  (dados de colisão cozinhados)
    └── ... (repetido para cada célula do grid)
```

Para mundos grandes: `ALandscapeStreamingProxy` substitui `ULandscapeProxy` em tiles streamados.  
UE 5+: `ULandscapeNaniteComponent` adiciona rendering GPU-driven para LOD 0.

### ULandscapeComponent — campos principais

```cpp
// Posicionamento
int32 SectionBaseX, SectionBaseY;   // Coordenada no grid global (em quads)
int32 ComponentSizeQuads = 63;       // Quads por componente (padrão)
int32 NumSubsections = 1;            // 1 ou 2 subseções
int32 SubsectionSizeQuads = 63;      // Quads por subseção

// Vértices por componente = (SubsectionSizeQuads + 1) * NumSubsections
//   Padrão: (63 + 1) * 1 = 64 vértices por lado = 4096 vértices/componente

// Heightmap
UTexture2D* HeightmapTexture;
FVector4 HeightmapScaleBias;         // UV scale+offset dentro da heightmap
FBox CachedLocalBox;

// Weightmaps (layer painting)
TArray<UTexture2D*> WeightmapTextures;
TArray<FWeightmapLayerAllocationInfo> WeightmapLayerAllocations;
FVector4 WeightmapScaleBias;
float WeightmapSubsectionOffset;

// LOD
TArray<double> MipToMipMaxDeltas;   // Erro de altura por transição de LOD
int32 ForcedLOD;
int32 LODBias;

// Materiais
TArray<UMaterialInstanceConstant*> MaterialInstances;
TArray<int8> LODIndexToMaterialIndex;

// Edit layers (UE 5+)
TMap<FGuid, FLandscapeLayerComponentData> LayersData;
```

### ULandscapeHeightfieldCollisionComponent

```cpp
int32 CollisionSizeQuads;
float CollisionScale;               // = ComponentSizeQuads / CollisionSizeQuads
int32 SimpleCollisionMipLevel;      // Mip reduzido para colisão simples

Chaos::FHeightFieldPtr HeightfieldGeometry;        // Full detail
Chaos::FHeightFieldPtr HeightfieldSimpleGeometry;  // Reduzido (perf)
```

---

## 3. Sistema de LOD (CLOD)

**Arquivos:** `LandscapeRender.cpp`, `LandscapeVertexFactory.ush`

### Número de LODs

```
SubsectionSizeVerts = SubsectionSizeQuads + 1 = 64

Número de LODs = log2(64) = 6

LOD 0 → 64×64 verts  (full resolution)
LOD 1 → 32×32 verts
LOD 2 → 16×16 verts
LOD 3 → 8×8   verts
LOD 4 → 4×4   verts
LOD 5 → 2×2   verts  (último utilizável)
LOD 6 → 1×1   verts  (ignorado — GetNumRelevantMips retorna 6)
```

### Parâmetros globais (Uniform Buffer)

```glsl
// FLandscapeUniformShaderParameters
int32  ComponentBaseX, ComponentBaseY;   // Posição deste componente no grid
int32  SubsectionSizeVerts;              // 64 (padrão)
int32  NumSubsections;                   // 1 ou 2
int32  LastLOD;                          // Índice máximo de LOD
float  InvLODBlendRange;                 // 1 / LODBlendRange
int32  RayTracingLODBias;

// FLandscapeContinuousLODParameters (por frame)
int2   Min;                              // Bounds do grid de componentes
int2   Size;                             // Dimensões do grid
float  SectionLODBias[];                 // LOD bias por componente (view-driven)
```

### Seleção de LOD por componente (CPU)

```cpp
// Baseado em:
// 1. Distância câmera → componente
// 2. LODDistributionSetting (escalonamento)
// 3. LOD0ScreenSizeOverride (cobertura de tela)
// 4. FarShadowStaticMeshLODBias (view bias global)

float GetSectionLod(uint ComponentIndex) {
    float ViewLODData = View.LandscapePerComponentData[ComponentIndex]; // CPU-computed
    int   LODBias     = View.FarShadowStaticMeshLODBias;
    return min(ViewLODData + LODBias, LandscapeParameters.LastLOD);
}
```

### CLOD — Continuous LOD no Vertex Shader

Cada vértice calcula seu próprio LOD fracionário com base em sua posição dentro do componente. Vértices perto das bordas do componente pegam influência do LOD do vizinho, criando uma transição suave.

```glsl
float CalcLOD(uint ComponentIndex, float2 xyLocalToSubsection, float2 Subsection) {
    // Converte para coordenadas normalizadas [0,1] dentro do componente
    float2 xy          = (xyLocalToSubsection + Subsection) / NumSubsections;
    float2 Delta       = xy * 2.0 - 1.0;                  // [-1, +1]
    float2 AbsDelta    = abs(Delta);
    float  BorderDist  = max(AbsDelta.x, AbsDelta.y);      // Distância à borda

    // BlendFactor: 0 no centro, 1 na borda
    float BlendFactor  = saturate(1.0 - (1.0 - BorderDist) * InvLODBlendRange);

    // Determina qual vizinho está mais próximo da borda
    int2 NeighborOffset = (AbsDelta.x > AbsDelta.y)
        ? int2(sign(Delta.x), 0)
        : int2(0, sign(Delta.y));

    float CenterLOD   = GetSectionLod(ComponentIndex);
    float NeighborLOD = GetNeighborSectionLod(NeighborOffset, CenterLOD);

    // Interpola entre LOD próprio e LOD do vizinho
    return lerp(CenterLOD, NeighborLOD, BlendFactor);
}
```

### Geomorphing

O vertex shader interpola a posição do vértice entre sua posição em LOD atual e a posição que teria em LOD mais baixo (pai). Isso elimina o *popping* visual durante transições de LOD.

```
LOD fracionário = 2.3
→ 70% posição LOD 2 + 30% posição LOD 3
→ Transição invisível enquanto câmera se afasta
```

---

## 4. Crack Prevention

**Problema:** vizinhos com LOD diferente criam gaps na borda onde os index buffers não se encontram.

**Soluções no UE:**

### 4.1 Borda usa o max(LOD próprio, LOD vizinho)

```glsl
float GetNeighborSectionLod(int2 NeighborOffset, float CenterLOD) {
    int2 NeighborPos = ComponentPos + NeighborOffset;
    NeighborPos      = clamp(NeighborPos, GridMin, GridMin + GridSize - 1);

    float NeighborLOD = GetSectionLod(LinearIndex(NeighborPos));

    // Na borda, ambos os componentes concordam em usar o LOD maior
    return max(CenterLOD, NeighborLOD);
}
```

### 4.2 Skirts (saias)

Os index buffers incluem vértices extras nas bordas que se estendem para baixo (−Z). Isso cobre visualmente qualquer gap residual entre componentes.

### 4.3 Edge Fixup (ULandscapeHeightmapTextureEdgeFixup)

Snapshots das alturas nas bordas são armazenados separadamente para garantir consistência ao fazer streaming de tiles adjacentes em momentos diferentes.

---

## 5. Material e Layer Blending

**Arquivos:** `LandscapeEdModeTools.h`, `LandscapeEdit.cpp`, shaders `LandscapeLayerBlend.ush`

### Armazenamento de Weightmaps

```cpp
// Cada layer de pintura = um canal de 8 bits (0–255) em uma texture RGBA
struct FWeightmapLayerAllocationInfo {
    ULandscapeLayerInfoObject* LayerInfo;
    uint8 WeightmapTextureIndex;   // índice da textura (pode haver várias)
    uint8 WeightmapTextureChannel; // canal: 0=R, 1=G, 2=B, 3=A
};

// Exemplo com 4 layers em uma única texture RGBA:
// Layer "Grass"  → Texture[0] canal R
// Layer "Rock"   → Texture[0] canal G
// Layer "Snow"   → Texture[0] canal B
// Layer "Dirt"   → Texture[0] canal A
```

Até 4 layers por texture; múltiplas textures são suportadas para mais layers.

### LB_HeightBlend (Height-based Blending)

Implementado em `UMaterialExpressionLandscapeLayerBlend`:

```glsl
// Para cada layer:
float weight = SampleWeightmap(LayerName);     // 0-1 (normalizado de uint8)
float height = HeightInput;                    // 0-1 vindo do material da layer

// Deslocamento do peso pelo height map
float modifiedWeight = clamp(
    lerp(-1.0, 1.0, weight) + height,
    0.0001,
    1.0
);

// Ao final, todos os pesos modificados são normalizados para soma = 1.0
```

Resultado: layers com "superfície elevada" (ex: pedras) sobrepõem layers mais baixas (ex: areia) nas bordas, criando transições naturais sem borda dura.

### Normalização dos pesos

```
Para cada pixel:
    soma = sum(weight[i] for all layers)
    if soma > 0:
        weight[i] = weight[i] / soma
```

Garante sempre que `sum(weights) == 1.0`.

### Blend Methods disponíveis

| Método | Comportamento |
|---|---|
| `None` | Sem normalização (raw weight) |
| `Additive` | `weight += contribution` |
| `PremultipliedAlphaBlending` | Alpha blending com grupos |
| `FinalWeightBlending` | Padrão: normaliza ao final |

---

## 6. Sculpting Tools

**Arquivos:** `LandscapeBrush.cpp`, `LandscapeEdModeBrushes.cpp`, `LandscapeEdModeTools.h`

### Modos de sculpting

| Modo | Algoritmo |
|---|---|
| **Raise/Lower** | `height ± (strength × brushAlpha)` por pixel |
| **Smooth** | Low-pass filter via FFT (Kiss FFT) — remove altas frequências |
| **Flatten** | Interpola `currentHeight → targetHeight` com `brushAlpha` |
| **Erosion** | Simula fluxo de água/gravidade; usa `Hardness` do `ULandscapeLayerInfoObject` |
| **Noise** | Perlin noise com múltiplas oitavas somado à altura atual |
| **Retopologize** | Suaviza mantendo volume (redistribuição de massa) |

### Parâmetros de Brush (UISettings)

```cpp
float BrushRadius;    // Tamanho total da pincelada (unidades de mundo)
float BrushFalloff;   // Largura da zona de transição (unidades de mundo)
float BrushStrength;  // Intensidade: 0.0 – 1.0
float BrushRotation;  // Rotação em graus (para alpha brushes)
```

### Pipeline por stroke

```
1. MouseButtonDown → BeginStroke()  → GEditor->BeginTransaction()
2. MouseMove       → ApplyBrush()   → calcula BrushData (alpha map)
                   → tool->Apply()  → modifica heightmap pixels
3. MouseButtonUp   → EndStroke()    → GEditor->EndTransaction()
```

---

## 7. Paint Tools

**Arquivos:** `LandscapeEdModeTools.h` — `FWeightmapEditAccessor`

### Algoritmo de pintura de layer

```
Para cada pixel na área do brush:
    brushAlpha = BrushData.GetAlpha(pixel)

    if apagando:
        newWeight = currentWeight × (1 - brushAlpha × strength)
    else:
        newWeight = currentWeight + (255 × brushAlpha × strength)

    SetWeightmapData(pixel, selectedLayer, clamp(newWeight, 0, 255))

    // Renormaliza as outras layers
    delta = newWeight - currentWeight
    for cada outra layer:
        other.weight = other.weight × (totalOld / totalNew)
```

### Pipeline de atualização de weightmap

```
1. GetMipData()           → lock da texture mip em CPU
2. Modificar array uint8  → aplica valores calculados
3. AddMipUpdateRegion()   → registra região suja
4. UpdateWeightmapMips()  → gera mips suavizados
5. UpdateTextureData()    → upload das regiões sujas para GPU
```

### Restrições de pintura

```cpp
enum ELandscapeLayerPaintingRestriction {
    None,                    // Sem restrição
    UseComponentAllowList,   // Só layers listadas no componente
    ExistingOnly,            // Só layers que já existem no componente
    UseMaxLayers             // Respeita MaxPaintedLayersPerComponent
};
```

---

## 8. Undo/Redo

**Arquivos:** `LandscapeBrush.cpp`, transação via `GEditor`

### Mecanismo geral

O UE usa o sistema de transações do editor. Cada stroke é uma transação:

```cpp
// Início do stroke:
GEditor->BeginTransaction(NSLOCTEXT("UnrealEd", "LandscapeMode", "Landscape Editing"));

// Modificações intermediárias são rastreadas automaticamente via UObject::Modify()
// Objetos modificados:
//   - ULandscapeComponent (HeightmapTexture dirty)
//   - UTexture2D (pixels do heightmap / weightmap)
//   - WeightmapLayerAllocations

// Fim do stroke:
GEditor->EndTransaction();
// → Ctrl+Z reverte para o estado anterior ao BeginTransaction
```

### Edit Layers Undo (UE 5+)

Cada `FLandscapeLayerComponentData` é serializado separadamente, permitindo reverter por layer. O merge é refeito ao desfazer.

### O que implementar no RCO

O GUE não tem `GEditor`. A abordagem equivalente:

```
struct HeightmapSnapshot {
    std::vector<float> data;   // cópia do heightmap antes do stroke
    int areaId;
};

std::vector<HeightmapSnapshot> undoStack;
std::vector<HeightmapSnapshot> redoStack;

BeginStroke()  → undoStack.push(snapshot atual)
EndStroke()    → redoStack.clear()
Ctrl+Z         → restaura undoStack.top() → move para redoStack
Ctrl+Y         → restaura redoStack.top() → move para undoStack
```

Limite razoável: 20–50 entradas. Para heightmap 512×512 × float = 1 MB por snapshot.

---

## 9. Brush System

**Arquivos:** `LandscapeBrush.h`, `LandscapeEdModeBrushes.cpp`

### Interface base

```cpp
class FLandscapeBrush {
    virtual void MouseMove(float LandscapeX, float LandscapeY) = 0;
    virtual FLandscapeBrushData ApplyBrush(
        const TArray<FLandscapeToolInteractorPosition>& Positions
    ) = 0;
    virtual float CalculateFalloff(
        float Distance, float Radius, float Falloff
    ) = 0;

    virtual void BeginStroke(float X, float Y, FLandscapeTool* Tool);
    virtual void EndStroke();
};

struct FLandscapeBrushData {
    FIntRect Bounds;           // Área afetada em quads (X1,Y1,X2,Y2)
    TArray<float> BrushAlpha;  // Um float [0,1] por pixel da área
};
```

### Tipos de falloff e suas fórmulas

```
Termos:
  r = Radius  (zona de força máxima)
  f = Falloff (largura da transição)
  d = Distance do ponto ao centro
```

**Linear:**
```
d < r:         alpha = 1.0
r <= d < r+f:  alpha = 1.0 - (d - r) / f
d >= r+f:      alpha = 0.0
```

**Smooth (Hermite/Smoothstep):**
```
y = LinearFalloff(d, r, f)
alpha = y * y * (3 - 2*y)
```

**Spherical:**
```
d < r:         alpha = 1.0
r <= d < r+f:  n = (d - r) / f
               alpha = sqrt(max(0, 1 - n*n))
d >= r+f:      alpha = 0.0
```

**Tip (lookup table):**  
Forma cônica — pico alto no centro, queda abrupta.

**Alpha Brush:**  
Usa uma textura 2D como máscara. Permite formas arbitrárias (pegadas, rachaduras, etc.).

### Brush Types disponíveis

```cpp
enum ELandscapeBrushType {
    Normal,     // Círculo com falloff
    Alpha,      // Textura alpha como máscara
    Component,  // Seleciona componentes inteiros
    Gizmo,      // Gizmo de transformação
    Splines     // Brush ao longo de splines
};
```

---

## 10. Rendering Pipeline

**Arquivos:** `LandscapeVertexFactory.ush`, shaders em `Engine/Shaders/Private/Landscape*.ush`

### Vertex Input

```glsl
struct FVertexFactoryInput {
    uint4 Position : ATTRIBUTE0;
    // .xy = coordenada no grid (VertexX, VertexY)
    // .zw = índice da subseção (SubX, SubY)
};
```

### Vertex Shader

```glsl
FVertexOutput VS(FVertexFactoryInput Input) {
    int2  GridCoord  = int2(Input.Position.xy);
    int2  Subsection = int2(Input.Position.zw);

    // 1. Calcula UV na heightmap deste componente
    float2 HeightmapUV = GridCoord * HeightmapScaleBias.xy + HeightmapScaleBias.zw;

    // 2. Amostra heightmap (R8G8 → uint16)
    float4 RawSample = HeightmapTexture.SampleLevel(Sampler, HeightmapUV, 0);
    uint16 PackedH   = uint(RawSample.r * 255) << 8 | uint(RawSample.g * 255);
    float  Height    = (float(PackedH) - 32768.0) * LANDSCAPE_ZSCALE;

    // 3. Calcula LOD contínua deste vértice (CLOD)
    float LOD = CalcLOD(ComponentIndex, GridCoord, Subsection);

    // 4. Geomorphing — interpola posição em direção ao LOD pai
    float3 Pos = ComputeVertexPositionWithMorphing(GridCoord, Height, LOD);

    // 5. Output
    Output.WorldPos    = LandscapeMatrix * float4(Pos, 1);
    Output.TexCoord    = ComputeLayerTexCoord(GridCoord);
    Output.WeightMapUV = ComputeWeightmapUV(GridCoord);
    Output.Normal      = UnpackNormal(RawSample.ba);
    Output.LOD         = LOD;
    return Output;
}
```

### Normal Unpacking

```glsl
float3 UnpackNormal(float2 BA) {
    float3 N;
    N.x = BA.x * 2.0 - 1.0;               // canal B → NX
    N.y = BA.y * 2.0 - 1.0;               // canal A → NY
    N.z = sqrt(max(0, 1 - dot(N.xy, N.xy)));
    return normalize(N);
}
```

### Pixel Shader — Layer Blending

```glsl
float3 FinalColor  = float3(0,0,0);
float3 FinalNormal = float3(0,0,1);

for (each layer i) {
    float weight   = SampleWeightmap(i, WeightMapUV);          // 0-1
    float3 albedo  = SampleLayerAlbedo(i, LayerTexCoord);
    float3 normal  = SampleLayerNormal(i, LayerTexCoord);
    float  height  = SampleLayerHeight(i, LayerTexCoord);      // para LB_HeightBlend

    // LB_HeightBlend modifica o peso pelo height
    float hWeight  = clamp(lerp(-1, 1, weight) + height, 0.0001, 1.0);

    FinalColor  += hWeight * albedo;
    FinalNormal += hWeight * normal;
}

// Normaliza
float totalW = sum(all hWeights);
FinalColor  /= totalW;
FinalNormal  = normalize(FinalNormal / totalW);
```

---

## 11. Constantes e Tamanhos

### Tamanhos padrão de componente

| Config | ComponentSizeQuads | SubsectionSizeQuads | NumSubsections | Verts/comp |
|---|---|---|---|---|
| Padrão | 63 | 63 | 1 | 64×64 |
| Alternativo | 63 | 31 | 2 | 64×64 |
| Pequeno | 31 | 31 | 1 | 32×32 |
| Muito pequeno | 15 | 15 | 1 | 16×16 |

### Escalas

```
1 heightmap unit = 1/128 metros  (LANDSCAPE_ZSCALE)
Height range: -256 m a +256 m (para escala padrão no UE)
```

### Colisão

```
CollisionMipLevel         = 0 → full resolution (padrão)
SimpleCollisionMipLevel   = 2 → 1/4 da resolução (para physics queries rápidas)
CollisionScale = ComponentSizeQuads / CollisionSizeQuads
```

### LOD Distribution

```cpp
float LOD0DistributionSetting = 1.75f;  // Reduz LOD 0 na tela mais agressivamente
float LODDistributionSetting  = 2.0f;   // Escalonamento geral de distâncias
float LODBlendRange           = 1.0f;   // Janela de geomorphing (em LOD units)
```

---

## 12. Edit Layers (UE 5.x)

**Arquivo:** `LandscapeEditLayerMergeRenderContext.h`

Sistema de edição **não-destrutiva**. Cada Edit Layer é um delta aplicado sobre o layer anterior. O resultado final é sempre calculado por merge na render thread.

```
BaseLayer (estado inicial)
  + EditLayer "Mountains" (raise)
  + EditLayer "Rivers" (lower/erode)
  + EditLayer "Roads" (flatten)
  = FinalHeightmap (calculado automaticamente)
```

### Structs

```cpp
// Por componente, por edit layer:
struct FLandscapeLayerComponentData {
    FHeightmapData HeightmapData;    // UTexture2D* para este layer
    FWeightmapData WeightmapData;    // TArray<UTexture2D*> para este layer
};

// Merge context (render thread):
class FLandscapeEditLayerMergeRenderContext {
    // Final = BaseLayer
    // for each active edit layer in order:
    //     Final = Compose(Final, EditLayer, BlendMode)
    void Merge(FRDGBuilder& GraphBuilder);
};
```

---

## 13. O que o RCO já tem vs. Gaps Reais

### Já implementado no RCO

| Feature | Status |
|---|---|
| Heightmap chunked (64×64 verts/chunk) | ✓ |
| LOD discreto por distância | ✓ |
| Triplanar mapping no shader | ✓ |
| LB_HeightBlend com 4 layers | ✓ |
| Splatmap RGBA (4 layers × 1 byte) | ✓ |
| Brushes Raise/Lower/Smooth/Flatten/Paint | ✓ |
| Falloff Gaussiano | ✓ (básico) |
| Radius ajustável (`[`/`]`) | ✓ |

### Gaps por prioridade

| Gap | Prioridade | Complexidade | Impacto |
|---|---|---|---|
| **Undo/Redo no terrain** | Alta | Baixa | Sem ele, qualquer erro é catastrófico |
| **Brush strength separado do radius** | Alta | Mínima | UX fundamental |
| **Falloff types (Linear/Smooth/Spherical)** | Média | Baixa | Visual e sensação do sculpt |
| **Normal recalculada por região** (não full) | Média | Média | Qualidade visual ao esculpir |
| **CLOD / Geomorphing** | Baixa | Alta | Elimina LOD popping |
| **Crack prevention via skirts** | Baixa | Média | Visível em terrenos acidentados |
| **Edit layers (não-destrutivo)** | Baixa | Alta | Workflow avançado |
| **Erosion / Noise brush** | Baixa | Média | Variedade no sculpting |

---

*Referências no código UE:*
- `Engine/Source/Runtime/Landscape/` — runtime core
- `Engine/Source/Editor/LandscapeEditor/` — sculpting tools
- `Engine/Shaders/Private/Landscape*.ush` — GLSL/HLSL equivalentes
