# RCO — Systems & Features

Documento vivo de arquitetura e status de cada sistema do engine. Cada seção cobre um sistema: status atual, subpartes, decisões técnicas e pendências.

---

## Índice

- [Character Controller](#character-controller) — `[ ] pendente` *(pré-requisito do navmesh e height sampling)*
- [Server-side Height Sampling](#server-side-height-sampling) — `[ ] pendente` *(pré-requisito do navmesh)*
- [Navmesh / Pathfinding](#navmesh--pathfinding) — `[ ] pendente`
- *(mais sistemas aqui conforme forem documentados)*

---

## Character Controller

**Status:** `[ ] pendente`
**Prioridade:** antes do navmesh e do height sampling — é a base do movimento do player
**Objetivo:** o player não consegue subir slopes íngremes como uma escada rolante. Slopes acima do threshold causam sliding. Pulos e quedas obedecem gravidade. Pequenos degraus de terreno são absorvidos suavemente.

### Problema atual

O cliente apenas samplea o heightmap e seta `player.y = terrain_height(x, z)` a cada frame. Não há verificação de ângulo, não há gravidade, não há sliding. O player sobe qualquer montanha verticalmente.

### Visão geral

O character controller não é física completa — é um conjunto de regras específicas para movimento de personagem em MMO. Roda inteiramente no **cliente** (predição); o servidor valida via height sampling.

```
Input (WASD / click-to-move)
    → Calcular velocidade desejada
    → Slope check: ângulo do terreno na posição destino
        → walkable  → mover + snap ao terreno
        → unwalkable → sliding (projetar velocidade na superfície)
    → Aplicar gravidade (se no ar)
    → Step absorption (pequenos degraus)
    → Posição final → enviar PStandardUpdate
```

---

### Part 1 — Slope Detection

**Responsabilidade:** dada a posição (x, z) do player, determinar se o terreno é walkable ou não.

**Localização:** `client/src/renderer/terrain/terrain.h/.cpp` *(já tem `SampleHeight`; adicionar `SampleNormal`)*

```cpp
// Normal do terreno em (x, z) — cross product das derivadas do heightmap
glm::vec3 Terrain::SampleNormal(float x, float z) const;

// Ângulo entre a normal e o eixo Y (0° = plano, 90° = parede)
float Terrain::SlopeAngle(float x, float z) const;
```

**Threshold walkable:** `kMaxWalkableAngle = 50.f` graus (ajustável)

- `SlopeAngle <= 50°` → walkable, movimento normal
- `SlopeAngle > 50°` → unwalkable, aplicar sliding

---

### Part 2 — Sliding

**Responsabilidade:** quando o player tenta entrar num slope unwalkable, a velocidade é projetada na superfície da slope. O player desliza para baixo ao longo do terreno em vez de subir ou ficar preso.

**Onde vive:** `client/src/core/main.cpp` → extrair para `client/src/core/player_controller.h/.cpp`

**Matemática:**
```cpp
// Normal do terreno na posição
glm::vec3 n = terrain.SampleNormal(x, z);

// Projetar velocidade desejada no plano da slope
// v_slide = v - dot(v, n) * n
glm::vec3 v_slide = velocity - glm::dot(velocity, n) * n;

// Adicionar componente de gravidade ao longo da slope
// gravity_along_slope = g - dot(g, n) * n  onde g = {0, -9.8, 0}
glm::vec3 g = {0.f, -9.8f, 0.f};
glm::vec3 g_slide = g - glm::dot(g, n) * n;

velocity = v_slide + g_slide * dt;
```

---

### Part 3 — Gravidade e Estado Aéreo

**Responsabilidade:** quando o player não está no chão (pulou ou caiu de uma borda), gravidade é aplicada acumulativamente até atingir o terreno novamente.

**Estado do player:**
```cpp
enum GroundState { kOnGround, kInAir };

struct PlayerController {
    glm::vec3  velocity      = {};
    GroundState ground_state = kOnGround;
    float      vertical_vel  = 0.f;   // velocidade vertical atual (m/s)
};
```

**Tick:**
```cpp
if (ground_state == kOnGround) {
    float terrain_y = terrain.SampleHeight(x, z);
    if (position.y > terrain_y + kGroundSnapThreshold) {
        ground_state = kInAir;  // caiu de uma borda
    }
} else {
    vertical_vel -= kGravity * dt;   // kGravity = 9.8f
    position.y   += vertical_vel * dt;

    float terrain_y = terrain.SampleHeight(x, z);
    if (position.y <= terrain_y) {
        position.y   = terrain_y;
        vertical_vel = 0.f;
        ground_state = kOnGround;
    }
}
```

**Constantes (ajustáveis):**
| Constante | Valor inicial | Descrição |
|-----------|--------------|-----------|
| `kGravity` | `18.f` | Aceleração de queda (u/s²) — maior que real para feel de jogo |
| `kGroundSnapThreshold` | `0.3f` | Distância mínima do chão para considerar "no ar" |
| `kTerminalVelocity` | `60.f` | Velocidade máxima de queda |

---

### Part 4 — Pulo

**Responsabilidade:** quando o player pressiona Espaço (ou tecla configurável), aplica impulso vertical se estiver no chão.

**Localização:** `client/src/core/player_controller.h/.cpp`

```cpp
void PlayerController::Jump() {
    if (ground_state != kOnGround) return;  // sem double jump por default
    vertical_vel  = kJumpImpulse;           // kJumpImpulse = 12.f (ajustável)
    ground_state  = kInAir;
}
```

**Estados durante o pulo:**
```
Espaço pressionado (no chão)
    → vertical_vel = +kJumpImpulse
    → ground_state = kInAir
    → gravidade aplicada a cada frame
    → vertical_vel decresce até negativo (apogeu → queda)
    → toca terreno → ground_state = kOnGround, vertical_vel = 0
```

**Variações a definir:**
- Double jump? (padrão: não)
- Jump cancelado em slope > threshold? (padrão: sim)
- Animação de pulo → acionar `PlayAnim("Jump", false, "Idle")` no `AnimController`

---

### Part 5 — Step Absorption

**Responsabilidade:** pequenas variações de altura no terreno (degraus < `kMaxStepHeight`) são absorvidas suavemente sem o player "pular" sobre eles.

**Localização:** `client/src/core/player_controller.h/.cpp`

```cpp
// Se a diferença de altura entre posição atual e destino for <= kMaxStepHeight,
// interpolar o Y suavemente em vez de setar diretamente.
static constexpr float kMaxStepHeight = 0.4f;

float target_y = terrain.SampleHeight(new_x, new_z);
float delta_y  = target_y - position.y;

if (std::abs(delta_y) <= kMaxStepHeight) {
    position.y = glm::mix(position.y, target_y, kStepSmoothSpeed * dt);
} else {
    position.y = target_y;  // desnível grande: snap direto
}
```

---

### Part 6 — Extração do `main.cpp`

**Responsabilidade:** o movimento do player hoje está embutido em `main.cpp`. Toda a lógica de Parts 1–5 deve viver em `player_controller.h/.cpp`.

**Localização:** `client/src/core/player_controller.h/.cpp`

**Interface:**
```cpp
class PlayerController {
public:
    void Update(float dt, const Terrain& terrain);
    void Jump();
    bool IsOnGround() const;
    bool IsJumping()  const;

    glm::vec3 position = {};
    float     yaw      = 0.f;
    float     speed    = 5.f;
};
```

`main.cpp` passa a apenas:
1. Ler input (WASD, Espaço, click-to-move)
2. Chamar `player_ctrl.Update(dt, terrain)`
3. Ler `player_ctrl.position` para sync de rede e câmera

---

### Relação com outros sistemas

| Sistema | Relação |
|---|---|
| Server-side Height Sampling | Servidor valida o Y reportado pelo player usando o mesmo heightmap |
| Navmesh | NPCs — não usa character controller. Player — não usa navmesh |
| AnimController | Pulo aciona `"Jump"` one-shot; queda aciona `"Fall"` se existir |

---

### Pendências — Character Controller

- [ ] `kGravity`, `kJumpImpulse`, `kMaxWalkableAngle` — valores finais dependem de testes com o modelo e câmera
- [ ] Double jump? Definir se alguma classe/habilidade terá
- [ ] Animação de queda (`"Fall"`) — existe no modelo?
- [ ] Coyote time (janela de ~100ms após sair de uma borda onde o pulo ainda é permitido)
- [ ] Jump buffering (pulo pressionado levemente antes de tocar o chão é executado ao aterrissar)
- [ ] Sincronização do estado aéreo com o servidor — enviar flag `in_air` no `PStandardUpdate`?

---

## Server-side Height Sampling

**Status:** `[ ] pendente`
**Prioridade:** antes do navmesh
**Objetivo:** o servidor sabe o Y correto de qualquer posição (X, Z) numa área, para que NPCs não flutuem nem afundem ao se mover em terreno com desnível.

### Problema atual

`moveNPCToward()` em `area.go` atualiza apenas X e Z. O Y do NPC é fixado no valor do spawn e nunca recalculado. Em terreno plano passa despercebido; em terreno com desníveis o NPC flutua ou afunda visivelmente.

### Solução

O cliente já resolve isso: `heightmap.cpp` expõe `SampleWorld(x, z)` que faz interpolação bilinear no heightmap. O servidor precisa da mesma capacidade.

### Subpartes

**Part 1 — Loader de heightmap no servidor (Go)**

Lê `heightmap.bin` da área no startup, mantém os dados em memória.

**Localização:** `server/internal/world/heightmap.go`

```go
type Heightmap struct {
    data     []float32
    width    int
    height   int
    cellSize float32
}

func LoadHeightmap(path string) (*Heightmap, error)
func (h *Heightmap) SampleWorld(x, z float32) float32
```

`SampleWorld` faz a mesma interpolação bilinear do cliente:
1. Converte (x, z) em coordenadas de grid → célula (cx, cz) + frações (fx, fz)
2. Lê os 4 vértices vizinhos
3. Retorna `bilinear(h00, h10, h01, h11, fx, fz)`

**Part 2 — Integração com `Area`**

`Area` carrega o heightmap da sua área no startup junto com portais e coldata.

```go
type Area struct {
    // ...campos existentes...
    heightmap *Heightmap  // nil se área não tem heightmap
}
```

**Part 3 — Aplicação no movimento de NPC**

Em `moveNPCToward()`, após calcular o novo X/Z, samplear o Y:

```go
newX := npc.X + dx * speed * dt
newZ := npc.Z + dz * speed * dt
newY := npc.Y  // fallback
if area.heightmap != nil {
    newY = area.heightmap.SampleWorld(newX, newZ)
}
npc.X, npc.Y, npc.Z = newX, newY, newZ
```

### Relação com navmesh

Quando o navmesh estiver implementado, o Y passa a vir do polígono atual (`NavAgent.Tick` já retorna a posição com Y correto derivado do mesh). O `Heightmap.SampleWorld` vira fallback para áreas sem navmesh baked ou para casos de emergência (NPC fora do mesh).

### Pendências

- [ ] Confirmar que o formato `heightmap.bin` lido pelo Go bate exatamente com o do cliente (`RCHM` magic, `float32[]` row-major)
- [ ] Decidir se o servidor lê heightmap de `dist/client/data/areas/<area>/` ou de um path configurável
- [ ] Áreas sem heightmap (indoor, instâncias) — Y fixo ou coldata mesh?

---

## Navmesh / Pathfinding

**Status:** `[ ] pendente`
**Objetivo:** NPCs navegam pelo terreno sem atravessar paredes ou ficarem presos, usando um grafo de polígonos walkable baked no GUE e A* no servidor.

### Visão geral

```
[GUE]                               [Servidor Go]
Part 1: Geo Extract                 Part 4: A* Pathfinder
Part 2: Navmesh Bake      →         Part 5: Nav Agent
Part 3: Serialização (.navmesh)
Part 6: Visualização (GUE)
```

**Fluxo de dados:**
```
heightmap.bin + coldata.bin
    → Part 1 → triângulos candidatos
    → Part 2 → NavMesh (verts + polys + adjacência)
    → Part 3 → navmesh.navmesh (disco)
    → Part 4 → sequência de waypoints
    → Part 5 → posição nova por tick
    → Part 6 → overlay no viewport GUE
```

**Ordem de implementação:**
```
Part 3 (formato)  →  Part 1  →  Part 2  →  Part 6
                  →  Part 4  →  Part 5
```
Part 3 primeiro porque define o contrato entre todas as outras.
Part 1→2→6 (pipeline GUE) e Part 4→5 (pipeline servidor) são paralelas.

---

### Part 1 — Geometry Extraction

**Status:** `[ ]`
**Responsabilidade:** ler heightmap + coldata e produzir lista de triângulos candidatos classificados como walkable/unwalkable.

**Localização:** `tools/gue/src/nav/geo_extract.h/.cpp`

**Inputs:**
- `heightmap.bin` da área atual
- `coldata.bin` da área atual (boxes, spheres, mesh tris)

**Output:** `std::vector<NavTriCandidate>` em memória

```cpp
struct NavTriCandidate {
    glm::vec3 v[3];
    bool      walkable;
};
```

**Critérios de walkable:**
- Normal do triângulo com eixo Y: `dot(normal, {0,1,0}) > cos(45°)` → walkable
- Triângulo sobrepõe qualquer shape da coldata → unwalkable

**Notas:**
- Samplear o heightmap em grid regular (ex: 1 tri por célula de 1×1 world unit)
- Shapes de coldata são aplicados como "pintura" sobre os candidatos já gerados

---

### Part 2 — Navmesh Bake

**Status:** `[ ]`
**Responsabilidade:** transformar os triângulos do Part 1 no polígono navegável final — simplificar, remover ilhas desconectadas, calcular adjacência.

**Localização:** `tools/gue/src/nav/navmesh_bake.h/.cpp`

**Input:** `std::vector<NavTriCandidate>` (apenas os walkable)
**Output:** `NavMesh` (definido no Part 3)

**Etapas internas:**
1. Filtrar apenas `walkable == true`
2. Calcular adjacência: dois polys são vizinhos se compartilham exatamente dois vértices (aresta)
3. Flood-fill para remover ilhas menores que threshold
4. *(Futuro)* Merge de triângulos coplanares adjacentes em polígonos maiores → reduz nós no A*

**Integração GUE:**
- Botão "Bake NavMesh" no painel Terrain do Zones tab
- Chama Part 1 → Part 2 → Part 3 (save) em sequência

---

### Part 3 — Serialização (`.navmesh`)

**Status:** `[ ]`
**Responsabilidade:** formato binário compartilhado entre GUE (C++) e servidor (Go).

**Caminho em disco:** `dist/client/data/areas/<area>/navmesh.navmesh`

**Localização:**
- C++: `tools/gue/src/nav/navmesh_io.h/.cpp`
- Go: `server/internal/nav/navmesh.go`

**Formato binário:**

| Campo     | Tipo           | Notas                    |
|-----------|----------------|--------------------------|
| magic     | u32            | `0x524E4156` ("RNAV")   |
| version   | u32            | `1`                      |
| num_verts | u32            |                          |
| verts     | float32[3] × N | x, y, z                  |
| num_polys | u32            |                          |
| polys     | Poly × N       | ver abaixo               |

```c
// C++
struct NavPoly {
    uint32_t v[3];        // índices em verts[]
    int32_t  neighbor[3]; // índice do poly vizinho por aresta, -1 = sem vizinho
};
```

```go
// Go
type NavPoly struct {
    V        [3]uint32
    Neighbor [3]int32
}
type NavMesh struct {
    Verts []Vec3
    Polys []NavPoly
}
```

---

### Part 4 — Pathfinding A*

**Status:** `[ ]`
**Responsabilidade:** dado ponto A e ponto B, retornar sequência de waypoints no navmesh.

**Localização:** `server/internal/nav/`

```
nav/
  navmesh.go      — structs + Load() lê .navmesh do disco
  pathfinder.go   — A* sobre polígonos
  funnel.go       — string pulling (suavização de caminho)
```

**A\* sobre polígonos:**
- Nós = polígonos (não vértices)
- Custo = distância entre centróides dos polys
- Heurística = distância euclidiana até o destino
- Vizinhos = `NavPoly.Neighbor[]` (excluindo -1)

**Funnel algorithm (string pulling):**
- Pega a sequência de polys do A* e as arestas compartilhadas entre eles
- Passa um "fio" pelos vértices das arestas → elimina ziguezagues desnecessários
- Resultado: caminho em linha reta quando possível, curvando só onde necessário

**Interface pública:**
```go
func LoadNavMesh(path string) (*NavMesh, error)
func (nm *NavMesh) FindPath(from, to Vec3) ([]Vec3, bool)
// retorna false se nenhum caminho encontrado
```

---

### Part 5 — Path Following / Agent

**Status:** `[ ]`
**Responsabilidade:** estado de movimento por NPC — consome waypoints do Part 4 e produz posição nova a cada AI tick.

**Localização:** `server/internal/nav/agent.go`

```go
type NavAgent struct {
    Waypoints   []Vec3
    WaypointIdx int
    Speed       float32
    Arrived     bool
}

func (a *NavAgent) RequestPath(nm *NavMesh, from, to Vec3)
func (a *NavAgent) Tick(dt float32, currentPos Vec3) Vec3
func (a *NavAgent) HasArrived() bool
func (a *NavAgent) Reset()
```

**Integração com `area.go`:**
- `WorldActor` ganha campo `NavAgent`
- `moveNPCToward()` é substituído por `actor.NavAgent.Tick(dt, pos)`
- Re-patha quando destino mudou mais que `rePathThreshold` unidades

**Re-path triggers:**
- Alvo (player) se moveu mais que N unidades desde o último path
- NPC ficou preso (posição quase igual por X ticks consecutivos)
- NPC saiu do navmesh (fallback: linha reta temporária)

---

### Part 6 — Visualização (GUE)

**Status:** `[ ]`
**Responsabilidade:** overlay do navmesh no viewport do zone editor para debug.

**Localização:** `tools/gue/src/nav/navmesh_vis.h/.cpp`

**Visual:**
- Polys walkable: verde semitransparente (`rgba(0, 1, 0, 0.25)`)
- Arestas: verde escuro sólido
- Caminho de debug (NPC selecionado): linha amarela

**Integração:**
- Toggle "Show NavMesh" no painel Terrain do GUE
- Mesmo padrão do colVis — VBO batched, um `glDrawArrays` por frame
- Carrega o `.navmesh` da área atual após o bake ou ao abrir a área

---

### Pendências — Navmesh

- [ ] **Pré-requisito:** Server-side Height Sampling implementado (ver seção acima)
- [ ] Tamanho do grid de sampling no Part 1 (1.0u? 0.5u?) — tradeoff precisão vs. tamanho do arquivo
- [ ] Threshold de ângulo walkable (45°? 40°?)
- [ ] Tamanho mínimo de ilha para remover no Part 2
- [ ] `rePathThreshold` para o agent (a cada quantas unidades o alvo se move re-patha?)
- [ ] Avoidance dinâmico NPC-NPC (RVO) — deixar para depois do core funcionar
- [ ] Áreas indoor (dungeons sem heightmap) — geração manual ou a partir de coldata mesh
