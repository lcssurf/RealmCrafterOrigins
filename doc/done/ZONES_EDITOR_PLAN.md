# Plano de Implementação — Aba Zones no GUE

Editor 3D completo de zonas integrado ao GUE, equivalente funcional da aba Zones do RC 1.26,
porém rodando em C++ / OpenGL 4.6 / Dear ImGui / SQLite.

---

## Visão geral

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  [Items] [Spells] [Media] [Actors] [Areas] [Zones]  ← nova aba              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Toolbar: [New][Save][Copy][Delete][Undo] | modos | combo zona        │   │
│  ├─────────────────────────────────────────┬────────────────────────────┤   │
│  │                                         │                            │   │
│  │   Viewport 3D (FBO → ImGui::Image)      │   Painel direito           │   │
│  │   ─ terrain como backdrop               │   (contextual por modo)    │   │
│  │   ─ objetos coloridos/meshes            │                            │   │
│  │   ─ câmera free-fly                     │                            │   │
│  │   ─ seleção / placement por click       │                            │   │
│  │                                         │                            │   │
│  ├─────────────────────────────────────────┴────────────────────────────┤   │
│  │ [Select F1][Move F2][Rotate F3][Scale F4] [Precise] Speed: __  X/Y/Z │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Novos arquivos a criar

```
tools/gue/src/
  tabs/zones.h/.cpp         ← ZonesTab (aba principal)
  zone_camera.h             ← free-fly camera (WASD + mouselook)
  zone_renderer.h/.cpp      ← FBO, terrain backdrop, primitivas coloridas, scenery
  zone_scene.h/.cpp         ← todos os objetos da cena (structs + coleções + undo)
```

### Arquivos modificados

```
tools/gue/src/main.cpp      ← registrar ZonesTab + instanciar
tools/gue/CMakeLists.txt    ← adicionar novos .cpp

server/internal/db/db.go    ← tabelas novas (ver seção DB)
server/internal/world/area.go ← Trigger, SoundZone, Waypoint, etc.
server/cmd/server/main.go   ← carregar tudo na inicialização
server/internal/net/client.go ← PvP check, trigger check, PAreaConfig
server/internal/protocol/packets.go ← PAreaConfig = 117
client/src/net/protocol.h   ← kPAreaConfig = 117
client/src/core/main.cpp    ← handler kPAreaConfig
```

---

## DB — tabelas novas

```sql
-- Props 3D posicionados na zona
CREATE TABLE zone_scenery (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    area_name      TEXT    NOT NULL DEFAULT '',
    model_id       INTEGER NOT NULL DEFAULT 0,   -- FK media_models
    material_id    INTEGER NOT NULL DEFAULT 0,   -- 0 = embedded
    x, y, z        REAL    NOT NULL DEFAULT 0,
    pitch, yaw, roll REAL  NOT NULL DEFAULT 0,
    sx, sy, sz     REAL    NOT NULL DEFAULT 1,
    collision      INTEGER NOT NULL DEFAULT 1,   -- 0=none 1=sphere 2=box 3=polygon
    anim_mode      INTEGER NOT NULL DEFAULT 0,   -- 0=none 1=loop 2=ping-pong 3=on-select
    inventory_size INTEGER NOT NULL DEFAULT 0,
    ownable        INTEGER NOT NULL DEFAULT 0,
    locked         INTEGER NOT NULL DEFAULT 0
);

-- Planos de água
CREATE TABLE zone_water (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    area_name      TEXT    NOT NULL DEFAULT '',
    x, y, z        REAL    NOT NULL DEFAULT 0,
    scale_x, scale_z REAL  NOT NULL DEFAULT 16,
    color_r        INTEGER NOT NULL DEFAULT 0,
    color_g        INTEGER NOT NULL DEFAULT 100,
    color_b        INTEGER NOT NULL DEFAULT 150,
    opacity        INTEGER NOT NULL DEFAULT 50,    -- 0-100
    tex_path       TEXT    NOT NULL DEFAULT '',
    tex_scale      REAL    NOT NULL DEFAULT 15,
    damage         INTEGER NOT NULL DEFAULT 0,
    damage_type    INTEGER NOT NULL DEFAULT 0
);

-- Emitters de partículas
CREATE TABLE zone_emitters (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    area_name      TEXT    NOT NULL DEFAULT '',
    config_name    TEXT    NOT NULL DEFAULT '',   -- nome do emitter config
    x, y, z        REAL    NOT NULL DEFAULT 0
);

-- Caixas de colisão invisíveis
CREATE TABLE zone_colboxes (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    area_name      TEXT    NOT NULL DEFAULT '',
    x, y, z        REAL    NOT NULL DEFAULT 0,
    scale_x        REAL    NOT NULL DEFAULT 5,
    scale_y        REAL    NOT NULL DEFAULT 2,
    scale_z        REAL    NOT NULL DEFAULT 5
);

-- Zonas de som ambiente
CREATE TABLE area_sound_zones (   -- já planejado
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    area_name        TEXT    NOT NULL DEFAULT '',
    x, z             REAL    NOT NULL DEFAULT 0,
    radius           REAL    NOT NULL DEFAULT 15,
    sound_name       TEXT    NOT NULL DEFAULT '',
    volume           INTEGER NOT NULL DEFAULT 100,
    loop_interval_ms INTEGER NOT NULL DEFAULT 0
);

-- Trigger zones de script
CREATE TABLE area_triggers (      -- já planejado
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    area_name    TEXT    NOT NULL DEFAULT '',
    x, z         REAL    NOT NULL DEFAULT 0,
    radius       REAL    NOT NULL DEFAULT 5,
    script       TEXT    NOT NULL DEFAULT '',
    func         TEXT    NOT NULL DEFAULT '',
    trigger_once INTEGER NOT NULL DEFAULT 0
);

-- Waypoints de patrulha NPC
CREATE TABLE zone_waypoints (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    area_name      TEXT    NOT NULL DEFAULT '',
    x, y, z        REAL    NOT NULL DEFAULT 0,
    next_a         INTEGER NOT NULL DEFAULT -1,  -- id do próximo WP (ramo A)
    next_b         INTEGER NOT NULL DEFAULT -1,  -- id do próximo WP (ramo B)
    pause_sec      INTEGER NOT NULL DEFAULT 0,
    spawn_actor_id INTEGER NOT NULL DEFAULT 0,   -- FK npc_spawns (0 = nenhum)
    spawn_script   TEXT    NOT NULL DEFAULT '',
    spawn_click_script TEXT NOT NULL DEFAULT '',
    spawn_death_script TEXT NOT NULL DEFAULT '',
    spawn_delay_sec INTEGER NOT NULL DEFAULT 5,
    spawn_max      INTEGER NOT NULL DEFAULT 1,
    spawn_range    REAL    NOT NULL DEFAULT 0
);

-- Colunas de script adicionadas a npc_spawns (ALTER TABLE)
-- npc_spawns já existe; apenas acrescentamos os campos de script
ALTER TABLE npc_spawns ADD COLUMN spawn_script  TEXT NOT NULL DEFAULT '';
ALTER TABLE npc_spawns ADD COLUMN spawn_func    TEXT NOT NULL DEFAULT '';
ALTER TABLE npc_spawns ADD COLUMN click_script  TEXT NOT NULL DEFAULT '';
ALTER TABLE npc_spawns ADD COLUMN click_func    TEXT NOT NULL DEFAULT '';
ALTER TABLE npc_spawns ADD COLUMN death_script  TEXT NOT NULL DEFAULT '';
ALTER TABLE npc_spawns ADD COLUMN death_func    TEXT NOT NULL DEFAULT '';

-- area_config expandido (ALTER TABLE nas colunas novas)
ALTER TABLE area_config ADD COLUMN is_outdoor    INTEGER NOT NULL DEFAULT 1;
ALTER TABLE area_config ADD COLUMN pvp_enabled   INTEGER NOT NULL DEFAULT 0;
ALTER TABLE area_config ADD COLUMN fog_near      REAL    NOT NULL DEFAULT 300;
ALTER TABLE area_config ADD COLUMN fog_far       REAL    NOT NULL DEFAULT 600;
ALTER TABLE area_config ADD COLUMN fog_r         REAL    NOT NULL DEFAULT 0.7;
ALTER TABLE area_config ADD COLUMN fog_g         REAL    NOT NULL DEFAULT 0.75;
ALTER TABLE area_config ADD COLUMN fog_b         REAL    NOT NULL DEFAULT 0.8;
ALTER TABLE area_config ADD COLUMN ambient_r     INTEGER NOT NULL DEFAULT 80;
ALTER TABLE area_config ADD COLUMN ambient_g     INTEGER NOT NULL DEFAULT 80;
ALTER TABLE area_config ADD COLUMN ambient_b     INTEGER NOT NULL DEFAULT 90;
ALTER TABLE area_config ADD COLUMN gravity       REAL    NOT NULL DEFAULT 1.0;
ALTER TABLE area_config ADD COLUMN entry_script  TEXT    NOT NULL DEFAULT '';
ALTER TABLE area_config ADD COLUMN exit_script   TEXT    NOT NULL DEFAULT '';
ALTER TABLE area_config ADD COLUMN weather_rain  INTEGER NOT NULL DEFAULT 0;
ALTER TABLE area_config ADD COLUMN weather_snow  INTEGER NOT NULL DEFAULT 0;
ALTER TABLE area_config ADD COLUMN weather_fog   INTEGER NOT NULL DEFAULT 0;
ALTER TABLE area_config ADD COLUMN weather_storm INTEGER NOT NULL DEFAULT 0;
ALTER TABLE area_config ADD COLUMN weather_wind  INTEGER NOT NULL DEFAULT 0;
```

---

## Arquitetura dos novos arquivos

### `zone_camera.h`

Câmera free-fly. Sem dependência de GLFW — opera sobre `dx/dy` do mouse e bitmask de teclas
passados pelo ZonesTab a cada frame.

```cpp
struct ZoneCamera {
    glm::vec3 pos    = {0, 5, 0};
    float     pitch  = -20.f;
    float     yaw    = 0.f;
    float     speed  = 10.f;  // unidades/s

    void Update(float dt, bool fwd, bool back, bool left, bool right,
                bool up, bool down, float mouseDX, float mouseDY);

    glm::mat4 ViewMatrix() const;
    glm::vec3 Forward()    const;
    // Ray desde a câmera passando pelo ponto (nx,ny) normalizado [-1,1]
    glm::vec3 ScreenRay(float nx, float ny, float fovY, float aspect) const;
};
```

### `zone_renderer.h/.cpp`

FBO dedicado ao viewport da zona. Renderiza:

1. **Terrain backdrop** — lê `dist/client/data/areas/<name>/heightmap.bin` e constrói
   mesh de grid via VAO simples. Usa `basic.vs/fs` com cor cinza/verde.
2. **Primitivas coloridas** — esfera, cilindro, cubo (gerados por código) para portais,
   triggers, sound zones, collision boxes, water, waypoints. Usa `basic.vs/fs`.
3. **Meshes de scenery** — instâncias de `rco::renderer::Model` (um por prop na cena).
   Renderizados com o pipeline deferred compartilhado.
4. **Linhas** — links entre waypoints, via GL_LINES.
5. **Selection highlight** — outline vermelho sobre o objeto selecionado (stencil ou escala +ε).

```cpp
class ZoneRenderer {
public:
    void Init(rco::renderer::Engine*, rco::renderer::Pipeline*);
    void Resize(int w, int h);
    void RenderFrame(const ZoneCamera&, const ZoneScene&, int selectedID, int selType);
    ImTextureID GetTexture() const;  // resultado do FBO como ImTextureID

    // Ray cast contra o terrain → retorna posição mundo ou {NaN} se miss
    glm::vec3 RaycastTerrain(glm::vec3 origin, glm::vec3 dir);
private:
    // FBO + color + depth attachments
    GLuint fbo_, colorTex_, depthRbo_;
    int fbW_ = 0, fbH_ = 0;

    // Terrain mesh (reconstruído quando área muda)
    GLuint terrainVAO_, terrainVBO_, terrainIBO_;
    int    terrainIdxCount_ = 0;
    std::string loadedArea_;

    // Primitive VAOs (sphere, box, cylinder, line)
    GLuint sphereVAO_, boxVAO_, cylVAO_;
    int sphereIdxCount_, boxIdxCount_, cylIdxCount_;

    // Shader para primitivas coloridas (basic.vs/fs)
    GLuint primProg_;

    void BuildTerrainMesh(const std::string& areaName);
    void BuildPrimitiveVAOs();
    void DrawSphere(const glm::vec3& pos, float r, const glm::vec4& col, const glm::mat4& vp);
    void DrawBox   (const glm::vec3& pos, const glm::vec3& scale, const glm::vec4& col, const glm::mat4& vp);
    void DrawLine  (const glm::vec3& a, const glm::vec3& b, const glm::vec4& col, const glm::mat4& vp);
};
```

### `zone_scene.h/.cpp`

Todos os objetos colocados na zona atual, em memória. Carregados do DB ao trocar de área.

```cpp
// Cada tipo de objeto da zona
struct ZScenery   { int id; int model_id, material_id; glm::vec3 pos, rot, scale; int collision, animMode, invSize; bool ownable, locked; };
struct ZPortal    { int id; glm::vec3 pos; float radius; std::string name, linkArea, linkPortal; };
struct ZTrigger   { int id; glm::vec2 xz; float radius; std::string script, func; bool once; };
struct ZSoundZone { int id; glm::vec2 xz; float radius; std::string soundName; int volume, loopMs; };
struct ZColBox    { int id; glm::vec3 pos, scale; };
struct ZWater     { int id; glm::vec3 pos; glm::vec2 scale; glm::vec3 color; int opacity; std::string texPath; float texScale; int damage, dmgType; };
struct ZEmitter   { int id; glm::vec3 pos; std::string configName; };
struct ZWaypoint  { int id; glm::vec3 pos; int nextA, nextB; int pauseSec; int spawnActorId; std::string spawnScript, clickScript, deathScript; int spawnDelaySec, spawnMax; float spawnRange; };
struct ZEnvConfig { /* todos os campos de area_config */ };

// NPC spawn com scripts — espelha npc_spawns + colunas novas
struct ZNpcSpawn {
    int         id;
    int         actor_def_id;
    std::string name, race, class_;
    int         level;
    glm::vec3   pos;
    float       yaw;
    int         aggressiveness;       // 0=passive 1=defensive 2=aggressive 3=dialog
    float       aggroRange;
    float       attackRange;
    int         respawnDelayMs;
    // Scripts (novos campos)
    std::string spawnScript,  spawnFunc;   // quando o NPC spawna
    std::string clickScript,  clickFunc;   // quando player clica direito / dialoga
    std::string deathScript,  deathFunc;   // quando o NPC morre
};

struct ZoneScene {
    std::string              areaName;
    ZEnvConfig               env;
    std::vector<ZScenery>    scenery;
    std::vector<ZPortal>     portals;
    std::vector<ZTrigger>    triggers;
    std::vector<ZSoundZone>  soundZones;
    std::vector<ZColBox>     colBoxes;
    std::vector<ZWater>      water;
    std::vector<ZEmitter>    emitters;
    std::vector<ZWaypoint>   waypoints;
    std::vector<ZNpcSpawn>   npcs;         // ← novo
    bool dirty = false;

    void LoadFromDB(sqlite3*, const std::string& area, MediaTab*);
    void SaveToDB  (sqlite3*);
};
```

### `tabs/zones.h/.cpp` — a aba principal

```cpp
class ZonesTab {
public:
    void SetRenderer(rco::renderer::Engine*, rco::renderer::Pipeline*);
    void Draw(sqlite3*, MediaTab*);
private:
    // Sub-sistemas
    ZoneCamera   cam_;
    ZoneRenderer renderer_;
    ZoneScene    scene_;

    // Estado de edição
    int   zoneMode_   = kModeScenery;  // qual modo de placement/seleção
    int   xformMode_  = kXformSelect;  // select/move/rotate/scale
    int   selectedID_ = -1;
    int   selectedType_ = -1;

    // Undo stack
    struct UndoEntry { std::string action; /* ... */ };
    std::vector<UndoEntry> undoStack_;

    // Câmera
    bool  mouseLook_  = false;
    float camSpeed_   = 20.f;

    // Lista de áreas
    std::vector<std::string> areaList_;
    int selectedArea_ = -1;
    bool needFetch_   = true;

    // Helpers de desenho
    void DrawToolbar(sqlite3*);
    void DrawViewport(sqlite3*, MediaTab*);   // o FBO + input handling
    void DrawRightPanel(sqlite3*, MediaTab*); // painel contextual
    void DrawBottomBar();

    // Painéis por modo
    void DrawPanelScenery  (sqlite3*, MediaTab*, bool placement);
    void DrawPanelTerrain  (sqlite3*,            bool placement);
    void DrawPanelEmitters (sqlite3*,            bool placement);
    void DrawPanelWater    (sqlite3*,            bool placement);
    void DrawPanelColBox   (sqlite3*,            bool placement);
    void DrawPanelSoundZone(sqlite3*,            bool placement);
    void DrawPanelTrigger  (sqlite3*,            bool placement);
    void DrawPanelWaypoint (sqlite3*, MediaTab*, bool placement);
    void DrawPanelPortal   (sqlite3*,            bool placement);
    void DrawPanelEnvironment(sqlite3*);
    void DrawPanelOther    (sqlite3*);

    // Actions
    void PlaceObject    (const glm::vec3& worldPos, sqlite3*, MediaTab*);
    void SelectAt       (float mx, float my);
    void DeleteSelected (sqlite3*);
    void DuplicateSelected(sqlite3*);
    void Undo();

    // Fetch
    void FetchAreaList(sqlite3*);
    void LoadArea(sqlite3*, MediaTab*, const std::string& name);
};
```

---

## Modos e painel direito

Replicando exatamente os modos do RC original:

| # | Botão (ícone) | Tooltip | Painel placement | Painel options (após selecionar) |
|---|---|---|---|---|
| 1 | 🏠 | Scenery mode | Mesh picker + Align to ground | Anim mode, Inventory, Collision, Ownable, Locked, Duplicate |
| 2 | 🗻 | Terrain mode | Heightmap picker + Reset flat | Colour map, Detail map, Detail scale, Max tris, Morphing, Shading |
| 3 | ⚡ | Emitters mode | ListBox de emitter configs | Change texture |
| 4 | 🌊 | Water mode | (right-click to place) | Cor RGB, Surface tex, Tex scale, Opacity, Damage + type |
| 5 | 🔒 | Collision box | (right-click to place) | (só scale via F4) |
| 6 | 🎵 | Sound zone | Sound picker + Music picker | Sound name, Repeat time, Volume |
| 7 | 🎁 | Triggers | Script combo + Func combo | Script, Func, Trigger ID |
| 8 | ↔ | Waypoints | (right-click to place) | Set next A/B + N, Pause, Spawn actor + scripts, Delay, Max, Range |
| 9 | 🔵 | Portals | Name + Linked area + Linked portal | Portal name, Linked area, Linked portal |
| 10| 👾 | NPCs/Mobs | Actor Def picker + Level + Agg | Todos os campos + 3 script slots |
| 11| ⭐ | Environment | Sub-tabs: FX / Weather / Other | (inline) |
| 12| ⚙ | Other | (inline) | Entry/exit script, PvP, Load image/music, Map tex |

---

## Cores dos objetos 3D na viewport

| Tipo | Forma | Cor |
|---|---|---|
| Portal | Esfera | Azul (0.2, 0.4, 1.0) alpha 0.5 |
| Trigger | Esfera | Laranja (1.0, 0.5, 0.0) alpha 0.4 |
| Sound zone | Esfera | Amarelo (1.0, 1.0, 0.0) alpha 0.4 |
| Collision box | Cubo | Vermelho escuro (0.8, 0.1, 0.1) alpha 0.4 |
| Water | Plano | Azul-turquesa (0.0, 0.5, 0.8) alpha configurável |
| Waypoint | Diamante (cubo rotacionado) | Ciano (0.0, 0.8, 1.0) |
| Link A (WP) | Linha | Azul (0.0, 0.4, 1.0) |
| Link B (WP) | Linha | Laranja (1.0, 0.5, 0.0) |
| Emitter | Cone | Amarelo-esverdeado (0.8, 1.0, 0.2) alpha 0.6 |
| NPC Passivo | Diamante | Verde (0.1, 0.9, 0.1) |
| NPC Defensivo | Diamante | Amarelo (1.0, 0.9, 0.0) |
| NPC Agressivo | Diamante | Vermelho (1.0, 0.2, 0.1) |
| NPC Dialog-only | Diamante | Lilás (0.7, 0.3, 1.0) |
| Raio de aggro | Círculo (linha) | Igual cor do NPC, alpha 0.3 |
| Raio de ataque | Círculo (linha) | Branco (1.0, 1.0, 1.0) alpha 0.2 |
| Selecionado | Outline | Vermelho (1.0, 0.0, 0.0) |

---

## Câmera e controles

| Input | Ação |
|---|---|
| RMB hold | Ativa mouselook (trava cursor, mostra pitch/yaw) |
| RMB + LMB | Move frente |
| RMB + RMB... | (já pressionado) |
| WASD | Move câmera no modo free-fly |
| Scroll | Ajusta velocidade da câmera |
| SPACE (mouselook off) | Não usado aqui (diferente do RC; usamos RMB hold) |
| LMB (select mode) | Seleciona objeto |
| RMB (placement mode, sem mouselook) | Coloca objeto no ponto do raycast |
| F1 | Select mode |
| F2 | Move mode |
| F3 | Rotate mode |
| F4 | Scale mode |
| Del | Deleta selecionado |
| Ctrl+D | Duplica selecionado |
| Ctrl+Z | Undo |
| Tab | Cicla pelo próximo objeto do mesmo tipo |
| Enter | Reset transform (posição/rotation/scale para default) |
| Arrow keys | Move / rotaciona / escala (depende do xformMode) |
| A / Z | Move Y cima/baixo |
| Numpad 8/2/4/6/9/7/1/3 | Move câmera (alternativo) |

---

## Sistema de Undo

```cpp
enum UndoAction { kCreate, kDelete, kMove, kRotate, kScale, kPropChange };

struct UndoEntry {
    UndoAction  action;
    int         objectType;  // tipo do objeto (scenery, portal, trigger…)
    int         objectID;    // id no DB (ou -1 se ainda não salvo)
    glm::vec3   prevPos, prevRot, prevScale;
    std::string prevProp;    // JSON serializado para property changes
};
```

- Stack de até 50 entradas.
- `Ctrl+Z` desempilha e reverte.
- Toda ação que modifica a cena empilha uma entrada ANTES de executar.
- Transformações contínuas (arrastar mouse) só empilham no `MouseButtonUp`.

---

## Raycast e seleção

### Placement (right-click)
1. Calcular ray = `camera.ScreenRay(mx_ndc, my_ndc, fovY, aspect)`.
2. Interseção ray × terrain (iteração de heightmap, ou DDA).
3. Posicionar novo objeto no ponto de impacto.
4. Se `Ctrl` pressionado: colocar à frente da câmera (sem raycast).

### Seleção (left-click, select mode)
Para cada objeto na cena, fazer ray × bounding volume (esfera ou AABB):
```
Para cada ZScenery:  ray × AABB do modelo (ou esfera de raio scale.x)
Para cada ZPortal:   ray × esfera de raio portal.radius
Para cada ZTrigger:  ray × esfera de raio trigger.radius (XZ apenas)
...
```
Seleciona o objeto mais próximo (menor `t` do ray).

### Move (left-click + drag, move mode)
1. Projeta posição do objeto no plano XZ da câmera.
2. Ao arrastar, reconstrói o ponto de impacto no mesmo plano (ou no terrain).
3. Atualiza posição em tempo real; empilha Undo no `MouseButtonUp`.

---

## Fases de implementação

Cada fase é independente e entregável por si só.

### Fase 0 — Viewport base + câmera (2–3 dias)

- Criar `zone_camera.h`.
- Criar `zone_renderer.h/.cpp` só com FBO + câmera + fundo preto.
- Criar `zones.h/.cpp` com viewport embutido e toolbar vazia.
- Registrar `ZonesTab` em `main.cpp`.
- Resultado: aba Zones existe no GUE, viewport preto navegável.

### Fase 1 — Terrain backdrop (1–2 dias)

- `ZoneRenderer::BuildTerrainMesh(areaName)` lê `dist/client/data/areas/<area>/heightmap.bin`.
- Constrói VAO de grid (N×N quads) com alturas.
- Renderiza com `basic.vs/fs` em cinza.
- Combo "Current zone" carrega o terrain correto.
- Resultado: navegar sobre o terrain da zona escolhida.

### Fase 2 — Primitivas coloridas + seleção (2 dias)

- `BuildPrimitiveVAOs()` — gera sphere, box, cylinder por código.
- Renderizar esferas/caixas no `RenderFrame`.
- Raycast básico (ray × esfera) para seleção.
- Selecionado fica em vermelho.
- Resultado: objetos visualizáveis e selecionáveis na cena.

### Fase 3 — Portals (2 dias)

- DB: `area_portals` já existe.
- `ZoneScene::LoadFromDB` carrega portais.
- Painel direito: modo Portal — placement com nome + linked area + linked portal.
- Right-click coloca portal (esfera azul).
- Selecionar: mostra painel options.
- Move/scale funciona.
- Salvar ao DB.
- Resultado: portais editáveis visualmente.

### Fase 4 — Triggers + Sound Zones (2 dias)

- DB: `area_triggers` + `area_sound_zones`.
- Placement, options panel, move, scale.
- Esferas laranjas (triggers) e amarelas (sound zones).
- Script/func popula combo de `.lua` em `dist/server/scripts/`.
- Resultado: triggers e sound zones editáveis.

### Fase 5 — Collision Boxes (1 dia)

- DB: `zone_colboxes`.
- Placement, move F2, scale F4.
- Caixa vermelha semi-transparente.

### Fase 6 — Waypoints (3 dias)

- DB: `zone_waypoints`.
- Placement como diamante ciano.
- Painel options: Set next A / Set next B (clica em outro waypoint para linkar).
- Linhas azuis (link A) e laranjas (link B) entre waypoints linkados.
- Campos: pause, spawn actor (combo dos npc_spawns), scripts, delay, max, range.
- Resultado: waypoints de patrulha completamente configuráveis.

### Fase 7 — Scenery (3–4 dias)

- DB: `zone_scenery`.
- Painel placement: picker do Media → Models + checkbox "Align to ground".
- Placement: carrega `rco::renderer::Model` do arquivo, instancia na cena.
- Cada ZScenery tem seu próprio `rco::renderer::Model` (ou compartilhado por model_id).
- Renderiza no pipeline deferred (ou basic shader por enquanto).
- Painel options: Anim mode, Inventory size, Collision, Ownable, Locked, Duplicate.
- Move / Rotate / Scale totalmente funcionais.
- Resultado: colocar props 3D do Media na zona.

### Fase 7b — NPCs/Mobs (2–3 dias)

- DB: adicionar `spawn_script/func`, `click_script/func`, `death_script/func` a `npc_spawns` via `ALTER TABLE`.
- `ZoneScene::LoadFromDB` carrega todos os `npc_spawns` da área ativa.
- Modo **NPCs** no toolbar (modo 10):
  - **Painel placement**:
    - Actor Def picker (combo de `media_actor_defs`)
    - Race / Class (texto livre)
    - Level spinner
    - Aggressiveness combo (Passive / Defensive / Aggressive / Dialog-only)
    - Aggro range + Attack range
    - Respawn delay
  - Right-click no terreno → cria spawn no ponto de impacto.
- **Painel options** (NPC selecionado):
  - Todos os campos acima
  - Yaw spinner (direção que o NPC olha ao spawnar)
  - **Seção "Scripts"**:
    - Spawn script + func (chamado quando o NPC aparece no mundo)
    - Click script + func (chamado quando player interage / right-click)
    - Death script + func (chamado quando o NPC morre)
  - Cada campo script é combo de `.lua` em `dist/server/scripts/` + campo func livre
- **Visual na viewport**:
  - Diamante colorido por aggressiveness (verde/amarelo/vermelho/lilás)
  - Círculo de aggro range (linha tracejada, cor do NPC, alpha 0.3)
  - Círculo de attack range (linha branca, alpha 0.2)
  - Seta de yaw apontando para onde o NPC olha
  - Texto do nome flutuando acima (billboard simples)
- Move F2: reposiciona no terreno (Y automático pelo heightmap).
- Delete: remove da `npc_spawns`.
- Resultado: todos os NPCs da zona visíveis, posicionados e configurados sem sair do viewport.

### Relação com a aba Actors atual

A aba **Actors** permanece como **lista tabular** — útil para:
- Ver todos os NPCs do projeto inteiro de uma vez (não por zona)
- Edits em massa (mudar level de 50 NPCs de uma vez)
- Criar/deletar NPCs sem abrir o editor 3D

O **Zones editor** é para **placement e configuração visual por zona**.
Ambas editam a mesma tabela `npc_spawns`; mudanças em uma aba refletem na outra após Refresh.

### Fase 8 — Water (2 dias)

- DB: `zone_water`.
- Placement: plano semi-transparente.
- Painel options: cor RGB, textura de superfície, escala, opacidade, dano + tipo.
- Geometria subdividida (aspecto visual).

### Fase 9 — Emitters (1 dia)

- DB: `zone_emitters`.
- Listbox de configs de emitter (ler pasta `dist/server/scripts/` ou tabela futura).
- Cone amarelo-esverdeado como visual.

### Fase 10 — Environment + Other (2 dias)

- Modo Environment: sub-tabs FX / Weather / Other editam `area_config`.
- Fog near/far, fog color RGB, ambient RGB, is_outdoor, gravity.
- Weather: sliders rain/snow/fog/storm/wind.
- Other: entry/exit script, PvP, load image/music.
- Alterações refletem em `area_config` no DB.

### Fase 11 — Undo (2 dias)

- Stack de `UndoEntry`.
- `Ctrl+Z` para todas as ações das fases anteriores.
- Duplicar `Ctrl+D`.

### Fase 12 — Terrain mode (1 dia)

- Modo Terrain: mostra heightmap picker + botão Reset.
- Abre o terrain-editor externo (`rco_terrain.exe`) com a área correta pré-carregada.
  - Opção: embutir heightmap editing no próprio GUE no futuro (fora do escopo agora).

---

## Server — o que muda

### `PAreaConfig = 117` (S→C, novo packet)

Enviado após `PStartGame` e após cada `PChangeArea`.

```
payload:
  pvp_enabled  u8
  fog_near     f32
  fog_far      f32
  fog_r        u8   (0-255)
  fog_g        u8
  fog_b        u8
  ambient_r    u8
  ambient_g    u8
  ambient_b    u8
  is_outdoor   u8
```

### `area_config` expandida

`main.go` aplica todos os campos de `AreaConfig` à `world.Area`:
- `PvPEnabled` → `handleAttackActor` verifica antes de deixar player atacar player.
- `EntryScript` / `ExitScript` → chamados no `triggerPortal`.
- Fog/Ambient/IsOutdoor → enviados via `PAreaConfig`.

### Triggers (novo)

`handleStandardUpdate` verifica `area.CheckTrigger(actor)` após atualizar posição.
Se dentro de um trigger, chama `scripts.Fire(trigger.Script, trigger.Func, actor)`.
Triggers com `TriggerOnce=true` são removidos da área após disparar.

### Waypoints (novo)

Carregados em `main.go` via `LoadAreaWaypoints()`. NPC AI usa waypoints para patrulha
em vez do movimento aleatório atual (`area.go` AI loop).

### Sound Zones (future)

Carregadas no servidor. Futuro: enviar posições ao cliente via novo packet para
reprodução de áudio posicional (fora do escopo desta fase).

---

## Ordem de implementação sugerida

```
Fase 0 → 1 → 2 → 3 → 4 → 5 → 6 → 7 → 7b → 8 → 9 → 10 → 11 → 12
  base terrain prim portals trig  col  WP  scen  NPCs water emit  env  undo terrain-mode
```

Cada fase é um commit independente com funcionalidade completa e testável.
Fases 0–6 cobrem toda a lógica de jogo (portals, triggers, waypoints).
Fases 7–9 cobrem o visual (scenery, water, emitters).
Fases 10–12 cobrem configuração e polish.

---

## Relacionamento com a aba Areas atual

Depois que a aba Zones estiver completa, a aba **Areas** atual pode ser:

- **Opção A**: mantida como lista rápida de áreas (criar/deletar área, ver portals de forma tabular) — complementa o Zones sem duplicar.
- **Opção B**: removida — o Zones faz tudo.

Recomendação: manter a aba Areas só para CRUD de `area_config` (criar nova área, editar nome) e deixar todo o placement no Zones.

---

## Estimativa de esforço

| Fase | Dias |
|---|---|
| 0 — Viewport base + câmera | 2–3 |
| 1 — Terrain backdrop | 1–2 |
| 2 — Primitivas + seleção | 2 |
| 3 — Portais | 2 |
| 4 — Triggers + Sound zones | 2 |
| 5 — Collision boxes | 1 |
| 6 — Waypoints | 3 |
| 7 — Scenery | 3–4 |
| 7b — NPCs/Mobs + scripts | 2–3 |
| 8 — Water | 2 |
| 9 — Emitters | 1 |
| 10 — Environment + Other | 2 |
| 11 — Undo | 2 |
| 12 — Terrain mode | 1 |
| **Total** | **~28–33 dias** |

**Ponto "utilizável" inicial** (~12–13 dias): portals + triggers + waypoints + collision boxes + NPCs com scripts — a lógica de jogo completa antes do visual.

Implementando na ordem das fases, o editor já é utilizável após ~10 dias (portais, triggers, waypoints, collision boxes).

---

## Arquivos modificados no db.go (já iniciado)

O `db.go` já recebeu os novos tipos `AreaConfig` expandido, `AreaTrigger`, `AreaSoundZone`.
Faltam adicionar: `AreaWaypoint`, `AreaScenery`, `AreaWater`, `AreaEmitter`, `AreaColBox` + funções `Load*` correspondentes.

---

*Fim do plano. Confirme, ajuste ou priorize antes de iniciar o código.*
