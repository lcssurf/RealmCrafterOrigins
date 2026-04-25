# GUE: Original (RC 1.26) vs RCO — Comparação Completa

Análise lado-a-lado dos dois editores. Fontes:

- **Original (RC 1.26)**: `D:/Github/RealmCrafter-Standard-1.26/Engine Source/GUE/` — ver também `GUE_ORIGINAL_REFERENCE.md`.
- **RCO**: `D:/Github/RealmCrafterOrigins/tools/gue/` — ~3.600 linhas, 5 abas + editor de terreno separado.

TL;DR: o GUE original tinha escopo monolítico (15 abas cobrindo build/assets/editor-3D/seasons/interface/network), ~44k linhas de Blitz3D que editavam **arquivos `.dat` binários**. O RCO GUE é **CRUD direto sobre SQLite**, apenas 5 abas e foco nos sistemas já implementados pelo engine; o restante do escopo (zonas, partículas, interface, facções) ou está em outras ferramentas (`tools/terrain-editor/`), ou ainda não foi portado, ou foi eliminado por design.

---

## Índice

- [1. Escopo e filosofia](#1-escopo-e-filosofia)
- [2. Stack técnico](#2-stack-técnico)
- [3. Modelo de persistência](#3-modelo-de-persistência)
- [4. Matriz de abas — original × RCO](#4-matriz-de-abas--original--rco)
- [5. Comparação por aba presente em ambos](#5-comparação-por-aba-presente-em-ambos)
  - [5.1 Items](#51-items)
  - [5.2 Abilities / Spells](#52-abilities--spells)
  - [5.3 Actors / NPCs](#53-actors--npcs)
  - [5.4 Media](#54-media)
  - [5.5 Areas / Zones](#55-areas--zones)
- [6. O que existe só no original (ainda não portado)](#6-o-que-existe-só-no-original-ainda-não-portado)
- [7. O que existe só no RCO (novidades)](#7-o-que-existe-só-no-rco-novidades)
- [8. Layout / UI / UX](#8-layout--ui--ux)
- [9. Padrões de código](#9-padrões-de-código)
- [10. Tabela-resumo final](#10-tabela-resumo-final)

---

## 1. Escopo e filosofia

| Eixo                     | Original RC                                                           | RCO                                                                          |
|--------------------------|-----------------------------------------------------------------------|------------------------------------------------------------------------------|
| **Papel**                | Editor monolítico — única ferramenta para TUDO: build, assets, scripts, zonas, UI do jogo | Editor de dados do servidor — CRUD em `rco.db`, nada mais |
| **Contém editor 3D?**    | Sim (aba Zones — coloca scenery, terreno, água, portais…)             | Não. O editor de terreno é ferramenta **separada** em `tools/terrain-editor/` |
| **Compila o jogo?**      | Sim — botões "Build full client", "Build server", "Generate updates"  | Não. Build via `build-*.bat` ou CMake fora do GUE                             |
| **Edita UI do jogo?**    | Sim (aba Interface — layout do HUD in-game)                           | Não. HUD é código C++ (`client/src/ui/*`)                                    |
| **Edita scripts?**       | Faz dropdowns populados de `.rsl`, mas escrever o código vai fora     | Faz a mesma coisa indiretamente (Lua `.lua` em pastas)                        |
| **Requer jogo rodando?** | Não                                                                   | Não (DB aberto via sqlite3 direto)                                            |
| **Monta instalador?**    | Sim — gera pasta `Game\` e `Server\` prontos para distribuir          | Não (função fora do GUE)                                                     |
| **Principais receptores**| Designers do jogo + operadores de servidor + mapeadores 3D            | Game designer trabalhando em balanceamento e spawns de NPC                    |

**Por que a diferença**: o RC original carregava o custo de ser **a única ferramenta do ecossistema** — sem Visual Studio, sem Unity, sem browser DB viewer. No RCO, DB externo + IDEs modernas assumem 80% do que o GUE original fazia, sobrando para o RCO GUE exatamente o que é **específico do domínio do jogo**: templates de itens, spells, NPCs e composição de actors.

---

## 2. Stack técnico

| Camada                | Original RC                                    | RCO                                                       |
|-----------------------|------------------------------------------------|-----------------------------------------------------------|
| Linguagem             | Blitz3D (`.bb`)                                 | C++ 17/20                                                 |
| Tamanho do código     | ~44.327 linhas em 22 arquivos                   | ~3.667 linhas em 14 arquivos                              |
| Build system          | Blitz Compiler proprietário                     | CMake + vcpkg                                             |
| Gráficos              | DirectX 7 (via Blitz3D)                         | OpenGL 4.6 (via GLAD)                                     |
| Janela                | Blitz3D built-in                                | GLFW                                                      |
| UI framework          | `F-UI.bb` proprietário (24k linhas)             | Dear ImGui 1.89+                                          |
| Render 3D no editor   | Blitz3D scene graph direto                     | Deferred renderer compartilhado com o cliente (`rco::renderer::Engine` + `Pipeline`) |
| DB                    | Arquivos `.dat` binários com offsets fixos     | SQLite 3 via `sqlite3_prepare_v2` + prepared statements   |
| File dialog           | Blitz GetOpenFileName wrapper                   | Win32 `OPENFILENAMEW` direto (`file_import.cpp`)          |
| Skybox preview        | Mesh + cloud sprites                           | IBL HDR (`default.hdr`)                                   |
| Resolução             | **Fixa** 1024×768                               | **Redimensionável** (início 1200×700, layout fluido)      |
| Multi-plataforma      | Windows-only (BlitzMax Win32 calls)             | Windows-only hoje (`_WIN32` guards), portável em tese     |
| Persistência de state | `imgui.ini`? — não aplicável (Blitz3D)          | `io.IniFilename = nullptr` — **nunca** grava ini          |
| CWD                   | Diretório de execução ativo                     | `SetCwdToExeDir()` — anchored no exe em `dist/tools/`     |

---

## 3. Modelo de persistência

### Original — arquivos `.dat` binários

Cada aba grava seu próprio arquivo. Exemplos:

```
Data\Server Data\Items.dat         ← aba Items
Data\Server Data\Actors.dat        ← aba Actors
Data\Server Data\Spells.dat        ← aba Abilities
Data\Server Data\Areas\Xyz.dat     ← cada zona tem arquivo próprio
Data\Server Data\Misc.dat          ← settings globais com offsets fixos
Data\Game Data\Interface.dat       ← layout do HUD
Data\Emitter Configs\*.dat         ← um arquivo por emitter
```

O arquivo `Misc.dat` é particularmente frágil: escreve-se com `SeekFile F, 9 : WriteShort ...` (offset mágico 9 = combat delay, 11 = combat formula, 12 = damage weapon, 13 = damage armour, 14 = rating adjust, 15 = accounts enabled, etc). Mudar layout requer cuidado manual de compatibilidade.

### RCO — SQLite `dist/server/rco.db`

Tudo em **uma base SQLite** com WAL mode. Cada aba = 1 ou mais tabelas:

| Aba GUE   | Tabelas                                                                             |
|-----------|-------------------------------------------------------------------------------------|
| Items     | `item_templates`                                                                    |
| Spells    | `spell_templates`                                                                   |
| Actors    | `npc_spawns` (com FK para `media_actor_defs`)                                       |
| Areas     | `area_config`, `area_portals`                                                       |
| Media     | `media_models`, `media_materials`, `media_anim_clips`, `media_actor_defs`, `media_actor_meshes`, `media_actor_anims` |

Abrir DB, salvar com `INSERT` ou `UPDATE`. Sem offsets, sem parsing, sem risco de quebra de layout. Schema migra automaticamente via `CREATE TABLE IF NOT EXISTS` + `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` (tratado com `EnsureTable`/`EnsureTables`).

**Ganho**: inspeção via `sqlite3 rco.db`, edição fora do GUE via qualquer SQLite browser, backup = `cp rco.db rco.db.bak`, migrations triviais.

**Perda**: no original, `.dat` é o "formato canônico do jogo" — client lê os mesmos arquivos. No RCO o client nunca toca o DB (tudo filtrado pelo server via packets), então o SQLite é **exclusivo** do server/ferramentas.

---

## 4. Matriz de abas — original × RCO

| # | Original (15 abas)  | RCO (5 abas)       | Status no RCO                                                    |
|---|---------------------|--------------------|------------------------------------------------------------------|
| 1 | Project             | —                  | **Removido.** Build via bat/CMake fora do GUE.                   |
| 2 | Media               | **Media**          | ✓ presente, **mais rico**: 4 sub-abas Models/Materials/Clips/Actor Defs |
| 3 | Particles           | —                  | Faltando. Emitters hoje são hard-coded em C++ (`client/src/renderer/particles.h/.cpp` + constantes `EmitterType`). |
| 4 | Combat              | —                  | Faltando. Delay/fórmula vivem em código Go (`progression.go`). |
| 5 | Projectiles         | —                  | Faltando. Não há sistema de projéteis server-authoritative ainda. |
| 6 | Factions            | —                  | Faltando. Sistema de facções não implementado.                   |
| 7 | Animations          | **Media → Anim Clips**| ✓ reimplementado dentro de Media (um clip = 1 linha em `media_anim_clips`). Sets de animação são sub-rows de Actor Defs. |
| 8 | Attributes          | —                  | Stats do jogo são fixos (HP/EP/XP em `progression.go`), sem customização via GUE. |
| 9 | Actors              | **Actors**         | ✓ presente mas escopo diferente: no original = raças/classes template; no RCO = **spawns de NPC** (instâncias no mundo). A "definição" do actor virou Media → Actor Defs. |
| 10| Items               | **Items**          | ✓ presente, mais simples (sem gubbins, sem attribute bonuses por item) |
| 11| Days & seasons      | —                  | Faltando. Ciclo dia/noite não implementado (cliente tem só skybox procedural). |
| 12| Zones               | `tools/terrain-editor/`| Parcialmente. Editor 3D saiu do GUE. Config de área (música, fog) virou aba **Areas** do GUE. Portals também. Scenery/Triggers/Waypoints/Water/SoundZones/ColBox — **nenhum destes está portado**. |
| 13| Abilities (Spells)  | **Spells**         | ✓ presente, **mais rico em AoE** (tem `aoe_type` + `aoe_radius` no template) |
| 14| Interface           | —                  | Removido. HUD do cliente é C++ puro.                             |
| 15| Other               | Menu `File`        | Simplificado. Só "Change DB path" agora. Hosts/money/gubbin remap não existem. |

---

## 5. Comparação por aba presente em ambos

Para cada aba que existe em ambos, listo **os campos do original** e **os do RCO** lado a lado. ✓ = presente, ✗ = ausente, → = renomeado/convertido.

### 5.1 Items

Fonte original: `GUE.bb:948-1073`. Fonte RCO: `tools/gue/src/tabs/items.cpp`.

| Campo                            | Original                       | RCO                            |
|----------------------------------|--------------------------------|--------------------------------|
| Sub-abas                         | 5 (General/Specific/Appearance/Attributes/Other) | **Nenhuma** — tudo em um formulário |
| Navegação                        | Combo "Current item" + Prev/Next `<<` `>>` | ListBox com typeIcon `[W]/[A]/[C]/[M]` |
| Name                             | ✓                              | ✓                              |
| Item type                        | Weapon/Armour/Ring/Potion/Food/Image/Other (7 tipos) | Weapon/Armor/Consumable/Misc (4 tipos) |
| Inventory slot                   | ✓ dinâmico (combo filtrado por tipo) | ✓ 10 slots explícitos (Weapon/Shield/Hat/Chest/Hands/Belt/Legs/Feet/Ring/Amulet) |
| Value                            | ✓ 0–10.000.000                 | ✓ `InputInt` sem upper bound (>0 enforced) |
| Mass (kg)                        | ✓                              | ✗ removido                     |
| Weapon: damage                   | ✓                              | ✓                              |
| Weapon: type (1H/2H/Ranged)      | ✓                              | ✓ (`None/One-Hand/Two-Hand/Ranged`) |
| Weapon: damage type              | ✓ combo com `DamageTypes$()`   | ✗ removido (no engine damage é genérico) |
| Weapon: projectile (ranged)      | ✓                              | ✗ removido                     |
| Weapon: ranged animation         | ✓                              | ✗ removido                     |
| Weapon: range                    | ✓                              | ✗ removido (spells têm range, weapons são melee) |
| Armor: level                     | ✓                              | ✓ (armor_level)                |
| Potion/Food: effect duration     | ✓                              | ✗ removido                     |
| Image display                    | ✓                              | ✗ removido                     |
| Misc data (string)               | ✓                              | ✗ removido                     |
| Thumbnail texture                | ✓                              | ✗ removido (items não têm ícone em UI ainda) |
| Mesh male / female               | ✓ (dois meshes separados)      | ✗ removido                     |
| Gubbin + show on equip           | ✓                              | ✗ gubbins não existem no RCO   |
| Attributes bonuses (40 slots)    | ✓ lista + spinner              | ✗ removido                     |
| Stackable                        | ✓                              | ✓                              |
| Max stack                        | ✗ (original: apenas boolean)   | ✓ **novo no RCO** (1–99)       |
| Takes damage (durability)        | ✓                              | ✗ implícito no engine (não configurável) |
| Exclusive race                   | ✓                              | ✗ removido                     |
| Exclusive class                  | ✓                              | ✗ removido                     |
| Script on right-click            | ✓ (`.rsl` combo)               | ✗ substituído por type fixo (ex: Consumable → heal automático) |
| Function to start script in      | ✓                              | ✗                              |

**UX**: original = combo sequencial estilo "preencher ficha", RCO = master-detail paradigm (lista à esquerda, form à direita, padrão ImGui moderno). RCO tem **dirty flag** (botão Save desabilitado até editar) e botão **Revert**; original salva em runtime (cada mudança grava imediatamente?) — na verdade original tem um botão "Save items" global no topo.

### 5.2 Abilities / Spells

Fonte original: `GUE.bb:1497-1543`. Fonte RCO: `tools/gue/src/tabs/spells.cpp`.

| Campo                    | Original                               | RCO                                                     |
|--------------------------|----------------------------------------|---------------------------------------------------------|
| Name                     | ✓                                      | ✓                                                       |
| Description              | ✓ textbox 700×20                       | ✗ removido                                              |
| Type                     | ✗ (todas as spells são "abilities" genéricas) | ✓ **novo no RCO** — Damage / Heal / Buff / Debuff |
| Damage min/max           | ✗ (engine script decide)               | ✓ **novo no RCO** (campo no template)                   |
| EP cost                  | ✗ (script decide)                      | ✓ **novo no RCO**                                       |
| Cooldown                 | ✓ "Recharge time" 0–60 **seconds**     | ✓ `cooldown_ms` em **milissegundos**                    |
| Range                    | ✗                                      | ✓ **novo no RCO** float                                 |
| Icon                     | ✓ "Display icon" texture picker       | ✓ `icon` ID int (não é picker, é ID numérico)           |
| AoE type                 | ✗                                      | ✓ **novo no RCO** — Single/AoE around target/Ground-targeted |
| AoE radius               | ✗                                      | ✓ **novo no RCO**                                       |
| Exclusive race           | ✓                                      | ✗                                                       |
| Exclusive class          | ✓                                      | ✗                                                       |
| Script                   | ✓ `.rsl` via combo                     | ✓ **convenção** — arquivo em `scripts/server/spells/<name>.lua` (sem picker) |
| Function to start in     | ✓                                      | ✗ **convenção** — função padrão `on_cast`               |

**Filosofia**: no original, os valores (damage, EP, duration) vivem **no script** (BriskVM). O template serve só como registro. No RCO, os valores estão **no template** (DB-driven) e o script Lua lê com `Spell.damage_min(ctx)` etc — padrão data-oriented. Isso é melhor para balanceamento (GUE > editar Lua a cada ajuste de número).

### 5.3 Actors / NPCs

Aqui o escopo mudou radicalmente. Ver sub-seção.

**Original — aba Actors**: define **raças/classes template** (ex: "Human Warrior", "Orc Berserker"). Cada actor = combinação Race+Class com aparência (body/hair/face/body textures × 5 variações × gênero), animação set, sons, inventário permitido, atributos base, resistências.

Os "NPCs vivos" no original são waypoints com `CSpawnActor` apontando para um Actor template (aba Zones → Waypoint options).

**RCO — aba Actors**: define **spawns de NPC** diretamente (posição X/Y/Z/Yaw/Area + stats de comportamento). A "aparência template" mudou-se para **Media → Actor Defs** (composição de meshes + material + anim map).

| Campo original              | Onde está no RCO                                                    |
|-----------------------------|---------------------------------------------------------------------|
| Race                        | Campo free-text na aba Actors (descritivo, sem semântica)           |
| Class                       | Idem                                                                |
| Description blurb           | Removido                                                            |
| Gender                      | Removido                                                            |
| Home faction                | Removido (sistema de facções não existe)                            |
| Aggressiveness              | ✓ presente — Passive/Defensive/Aggressive/Dialog-Only               |
| Attack range                | ✓ presente (campo `attack_range`)                                   |
| Trade Mode                  | Removido — merchant é por dialog Lua que chama `Dialog.open_shop()` |
| Environment type            | Removido                                                            |
| Start area                  | → campo `area_name` na aba Actors                                   |
| Start portal                | Removido (usa coords X/Y/Z diretas)                                 |
| XP multiplier               | Removido                                                            |
| Is playable                 | Removido (player usa modelo fixo "player_actor")                    |
| Is rideable                 | Removido (mounts não existem)                                       |
| Male/Female animation sets  | → **Media → Actor Defs → anim_map** (composição flexível)           |
| Male/Female speech sounds   | Removido                                                            |
| Inventory slot allowed      | Removido                                                            |
| Male/Female body mesh       | → **Media → Actor Defs → mesh_slots (slot 0 = Body)**               |
| Hair/Face/Body texture      | → **Media → Actor Defs → mesh_slots (slots 1, Chest etc)**          |
| Beard / Gubbin              | Slots Helm/Weapon/Shield/Attachment no RCO (11 slots totais)        |
| Blood texture               | Removido                                                            |
| Actor scale                 | → `scale` por Model no Media                                        |
| Polygonal collision         | Removido                                                            |
| Attribute values (40)       | Removido                                                            |
| Attribute max (40)          | Removido                                                            |
| Resistance values (20)      | Removido                                                            |
| 3D preview                  | → Media → Actor Defs tem preview 3D com IBL + skin animation        |

**Campos novos do RCO:**
- `aggressive_range` — radius de detecção (original tinha só `AttackRange`, aggro era implícito)
- `respawn_delay_ms` — tempo de respawn (no original isso era propriedade do waypoint, não do actor)
- `actor_def_id` — FK para Media → Actor Defs (equivale à aparência)
- `level` — nível do NPC (no original isso era inferido de atributos)

**Trade-off**: RCO perdeu **muita** customização de aparência (5 variações de hair/face/body), ganhou **composição arbitrária** (qualquer número de mesh slots com qualquer model + material + animation clip). Na prática o original era **rígido** (sempre 5 slots de hair, sempre 1 beard) e o RCO é **flexível** (você compõe o que quiser).

### 5.4 Media

Fonte original: `GUE.bb:349-385` + `Modules/Media.bb`. Fonte RCO: `tools/gue/src/tabs/media.cpp` (1.221 linhas).

**Original — aba Media (única, com combo de tipo)**:

- Combo "View": 3D Meshes / Textures / Sounds / Music
- Lista de subpastas + lista de arquivos da pasta
- Preview 3D ou 2D
- Botões Add New File / Remove File

Cada asset ganha um **ID inteiro** (até 65535) que outras abas referenciam. Sem composição — um actor armazena "mesh ID 42 = male body" e só.

**RCO — aba Media (com 4 sub-abas + 3D preview real)**:

| Sub-aba        | Conteúdo                                                                 |
|----------------|--------------------------------------------------------------------------|
| **Models**     | Mesh files (name + file_path + scale). 3D preview à direita com IBL.     |
| **Materials**  | Albedo/Normal/ORM paths + PBR factors (albedo tint RGB, roughness, metallic). |
| **Anim Clips** | Nome + source file (vazio = embedded no Body) + clip_override.           |
| **Actor Defs** | Composição: nome + lista de `(slot, model_id, material_id)` + map `(action → clip_id)`. 3D preview com bone animation. |

| Recurso                           | Original                            | RCO                                          |
|-----------------------------------|-------------------------------------|----------------------------------------------|
| Banco de meshes                   | ✓ com ID                            | ✓ tabela `media_models`                      |
| Banco de texturas                 | ✓ com ID                            | Parcial — texturas vivem **dentro de materials**, não independentes |
| Banco de sons                     | ✓                                   | ✗ sons são arquivos em pasta, sem DB (client usa enum `SoundID`) |
| Banco de músicas                  | ✓                                   | ✗ idem (enum `MusicTrack`)                   |
| Materiais PBR                     | ✗ (RC tinha texturas simples)       | ✓ **novo no RCO** — full PBR                 |
| Animation clips                   | ✓ (aba Animations separada)         | ✓ sub-aba Media → Anim Clips                 |
| Composição de actor               | ✗ (aba Actors era rígida)           | ✓ **novo no RCO** — Actor Defs flexíveis     |
| Preview 3D com animação           | ✓ (aba Actors → Preview)            | ✓ no Media → Models e Actor Defs             |
| Import de arquivo                 | File dialog → copia para pasta      | ✓ `PickAndImportAsset()` — Win32 dialog; **copia automaticamente para `assets/<subdir>/`** se for fora. Se já for interno, referencia direto. |
| Preview 2D de textura             | ✓ (via `MediaPreviewQuad`)          | ✗ (só dentro do contexto de material)        |
| Tocar sons                        | ✓ Play/Stop + volume slider         | ✗                                            |
| Tocar músicas                     | ✓                                   | ✗                                            |
| Subpastas hierárquicas            | ✓ (via ListBox `LMediaFolder`)      | ✗ flat list (ordenado por nome)              |
| Navegação                         | Duas listas (pasta + arquivo)       | Uma lista por sub-aba                        |
| Layout                            | 3 painéis fixos (esquerda/preview/labels) | 2-3 painéis com split dinâmico por `ImGui::GetContentRegionAvail() * ratio` |

**Ganho do RCO**: composição de actor via Media → Actor Defs é **muito** mais poderosa que o original. Um único Body model ("HumanMale.glb") pode ser compartilhado por 50 NPCs com armors/animações diferentes sem duplicar assets.

**Perda do RCO**: sem players de áudio, sem browser independente de texturas, sem hierarquia de pastas. Adicionar uma textura solta obriga a criar um material.

### 5.5 Areas / Zones

Mudança mais radical do projeto.

**Original — aba Zones**: editor 3D completo (`GUE.bb:1170-1496`, ~326 linhas só de setup + ~850 linhas de handlers de mouse/teclado). 10 modos (Scenery/Terrain/Emitters/Water/ColBox/SoundZone/Trigger/Waypoint/Portal/Environment/Other), transform Select/Move/Rotate/Scale com undo, camera WASD+numpad+mouselook, painel direito contextual de 165×580 px que troca com o modo.

**RCO — aba Areas** (`tools/gue/src/tabs/areas.cpp`, 485 linhas): apenas CRUD de config de área + portais. Sem 3D view.

| Recurso original (Zones)                  | No RCO                                                    |
|-------------------------------------------|-----------------------------------------------------------|
| Viewport 3D com camera livre              | ✗ **removido do GUE**. Existe só no `tools/terrain-editor/` (escopo diferente — sculpt/paint de heightmap) |
| Scenery placement                         | ✗ não implementado                                        |
| Terrain heightmap sculpt                  | → `tools/terrain-editor/` (ferramenta separada)           |
| Terrain material paint (splatmap)         | → `tools/terrain-editor/` (4-material blend UE5)          |
| Emitters placement                        | ✗ emitters hoje são spawnados por evento server-side      |
| Water                                     | ✗                                                         |
| Collision boxes                           | ✗ (colisão é heightmap + slope)                           |
| Sound zones                               | ✗ (música per-area via `area_config.music_track`)         |
| Triggers                                  | ✗                                                         |
| Waypoints (patrulha NPC)                  | ✗ NPCs têm AI de chase simples, sem paths                 |
| Portals                                   | ✓ **aba Areas → seção Portals** (CRUD puro)               |
| Environment (fog/ambient/light direction) | Parcial — só `fog_density` na config                      |
| Weather                                   | ✗                                                         |
| Other (entry/exit script, PvP, load image)| ✗ (PvP não existe)                                        |

**Aba Areas do RCO tem**:

- Lista à esquerda de áreas (`area_config.name`)
- Painel à direita com:
  - **Área**: Music Track (combo 0=Stop / 1=Starter Zone / 2=Forest / 3=Combat) + Fog Density (slider 0–1)
  - **Portals**: lista compacta + form de criação/edição com:
    - Trigger Volume (XZ plane): X, Z, Radius
    - Destination: Target Area combo + dest_x/y/z + dest_yaw

Comparado ao original: no original, portais eram objetos 3D visíveis na zona. No RCO são **trigger circles** em XZ plane (cilindro vertical infinito), config via formulário 2D.

**Coisas que estão fora do GUE mas no engine**: fog_density é lido pelo server em `db.LoadAreaConfigs()`. Portal trigger é checado em `world/portal.go:CheckPortal()`.

---

## 6. O que existe só no original (ainda não portado)

- **Project tab**: build full client, build installer, generate updates, build server (com opção MySQL).
- **Particles editor**: emitter configs editáveis (spawn rate, lifespan, colors, velocity shaping, forces, texture animation). No RCO os emitters são 6 enums fixos em C++ (`EmitterType` 0–5).
- **Combat tab**: combat delay, dano em arma/armadura ao uso, combat formulas, info style (chat message/floating number), rating adjust on kill.
- **Projectiles**: mesh + 2 emitters (trail/impact), homing, hit chance, damage, speed. Hoje o RCO tem só `PProjectile=37` reservado mas não implementado.
- **Factions tab**: até 100 facções com matriz de rating assimétrica (A→B ≠ B→A).
- **Animations tab (separada)**: sets nomeados com (start frame, end frame, speed) por animação. No RCO cada clip é uma linha em `media_anim_clips`, não em set.
- **Attributes tab**: até 40 atributos/skills customizáveis. No RCO HP/EP/XP são fixos (não editáveis sem mexer em Go).
- **Days & seasons**: calendário jogável (anos/meses/estações), múltiplos sóis/luas com rise/set por estação, cor de luz. No RCO não há ciclo dia/noite.
- **Interface tab**: layout do HUD via mover quads com mouse + sliders RGB/alpha. No RCO o HUD é código C++ com posições fixas.
- **Zones (editor 3D monolítico)**: scenery, triggers, waypoints, sound zones, water, colboxes, weather.
- **Other tab**: host/porta do server, money tiers, gubbin bone remapping, chat bubbles config.

**Abas do original com contrapartida conceitual no RCO (mas fora do GUE)**:
- Terrain editing → `tools/terrain-editor/`
- Scripts → arquivos `.lua` em `dist/server/scripts/` editados no VS Code
- UI layout → código C++ em `client/src/ui/`

---

## 7. O que existe só no RCO (novidades)

- **Sub-abas de Media com composição**: o conceito "Actor Def" com mesh_slots + anim_map é **mais poderoso** que o modelo rígido do original (que tinha 1 body + 5 hairs + 5 beards + 6 gubbins hardcoded).
- **Materials PBR**: Albedo/Normal/ORM + factors (tint, roughness, metallic). Original usava DirectX 7 single-texture.
- **Preview 3D compartilhado com o engine do cliente**: `PreviewViewport` usa o mesmo `rco::renderer::Engine` + `Pipeline` + IBL do cliente (`preview_viewport.cpp`). No original, preview do actor é um scene graph Blitz3D separado.
- **Skeletal animation com SSBO**: bones enviados via `bone_ssbo_` para o shader. 64 bones max, 4 weights/vertex.
- **AoE fields em spells**: `aoe_type` + `aoe_radius` armazenados no template (não precisam ser redefinidos no Lua).
- **Ground-targeted AoE como primitiva**: client mostra reticle circle na terrain, LMB para cast, ESC para cancelar.
- **Spawn system DB-driven**: no original NPCs são hardcoded em waypoints de zone; no RCO são rows em `npc_spawns` editáveis via GUE Actors tab.
- **WAL mode**: `PRAGMA journal_mode=WAL` permite server e GUE abrirem o DB ao mesmo tempo.
- **Import automático de assets**: `PickAndImportAsset()` detecta se arquivo já está em `dist/client/assets/` e, se não estiver, **copia** para `assets/<subdir>/` (models/textures/anims). Original exige pasta pré-existente.
- **File menu**: "Change DB path..." permite trocar de DB em runtime sem relaunch. Útil para trabalhar com múltiplas bases (dev, stage, snapshot).
- **Auto-detect DB path**: tenta `../server/rco.db`, `rco.db`, `../../dist/server/rco.db`. Se tudo falhar, abre dialog modal.
- **Revert button**: toda aba tem "Revert" para desfazer mudanças antes de Save (original não tem).
- **Dirty flag visual**: Save desabilitado até haver mudanças, Delete em vermelho (`PushStyleColor`).
- **Guardas referenciais em Delete**: Items se recusa a deletar item que está em `character_items`; Spells se recusa a deletar spell em `character_known_spells`.
- **Agrupamento visual por área em Actors**: lista de NPCs com header colorido por `area_name` (`TextColored({0.6f, 0.9f, 1.f, 1.f}, ...)`).
- **Music track combo por área**: `area_config.music_track` é enum (0=Stop, 1=Starter Zone, 2=Forest, 3=Combat).

---

## 8. Layout / UI / UX

### Original

- **Janela** 1024×768 com borders (quads Top/Bottom/Left/Right em `z=10`) — visual "emoldurado".
- Toda posição em **pixels absolutos** (`FUI_Button(TItems, 150, 20, 100, 20, "Delete item")`).
- **15 abas fixas** em tab strip superior sempre visível.
- Layout "old Windows" — groupboxes com título, controles alinhados à esquerda com label separado.
- Abas complexas (Actors, Items) têm **sub-tab interna** para organização.
- 3D viewport é um `FUI_View` dentro da aba — mouse-pick usa offset mágico `MouseX() - 45, MouseY() - 100` (frame + tab header).
- Atalhos F1–F4 para mudar transform mode (Select/Move/Rotate/Scale) na aba Zones.
- **Undo system** (Ctrl+Z) na aba Zones — grava struct `Undo` com Action/Info/InfoX/Y/Z.
- Cores de destaque: borders vermelhos/azuis/amarelos para tipo de objeto (portals vermelhos, waypoints, triggers azuis).
- Save buttons por aba (cada aba tem seu botão) + flags globais `ItemsSaved/ActorsSaved/...`
- Dialogs modais via `FUI_CustomMessageBox("msg", "title", MB_OK | MB_YESNO)`.

### RCO

- **Janela redimensionável** (inicia 1200×700, layout responde a `GetContentRegionAvail()`).
- **ImGui dark theme** (`ImGui::StyleColorsDark()`).
- Root window **em tela cheia** via `SetNextWindowPos(vp->Pos)` + `SetNextWindowSize(vp->Size)` com flags `NoTitleBar | NoResize | NoMove | NoBringToFrontOnFocus | MenuBar`.
- Menu bar: apenas `File > Change DB path...` e status do DB.
- **5 abas** em `BeginTabBar("##main_tabs")` superior, sem sub-abas **exceto Media** (4 sub-abas).
- **Master-detail pattern** consistente em toda aba: lista à esquerda (240px ou 180px) + form à direita.
- **Lista usa typeIcons** curtos `[W]/[A]/[C]/[M]` (items), `[D]/[H]/[B]/[X]` + emoji `⊕` `◎` (spells), `[A]/[D]/[ ]` (actors).
- **Section headers**: `ImGui::TextColored({0.8f, 0.9f, 1.f, 1.f}, "Mesh Slots")` para cyan-like labels.
- **Tooltips textuais**: `ImGui::TextDisabled("Melee ≈ 2  |  Ranged ≈ 15–25")` em cinza abaixo de inputs — dá contexto sem pesar a UI.
- **Dirty state**: `ImGui::BeginDisabled(!dirty)` desabilita botão Save até haver mudanças. Botão Delete sempre **vermelho** (`PushStyleColor(ImGuiCol_Button, {0.65f, 0.1f, 0.1f, 1.f})`).
- **Dialogs**: modais nativos ImGui com `SetNextWindowPos` centralizado, `ImGuiWindowFlags_NoResize | NoCollapse`.
- **Status line**: texto desabilitado no topo direito da aba mostra última mensagem (`Loaded N items`, `Saved 'X' (id=42)`, `Cannot delete: item 5 is in 3 inventory slot(s)`).
- **Sem `imgui.ini`**: `io.IniFilename = nullptr`.
- **Sem undo** — Revert é o mais próximo que existe (mas só funciona antes de Save).

### Tabela de comparação UI/UX

| Atributo                          | Original                                      | RCO                                                         |
|-----------------------------------|-----------------------------------------------|-------------------------------------------------------------|
| Resolução                         | Fixa 1024×768                                 | Redimensionável, layout fluido                              |
| Padrão visual                     | Windows 98-ish, groupboxes explícitos         | ImGui dark, flat, section-based                             |
| Organização                       | 15 abas monolíticas com sub-tab aninhada       | 5 abas com 4 sub-abas (só em Media)                         |
| Navegação dentro de lista         | Combo "Current X" + botões `<<` `>>`         | ListBox/Selectable clicável com scroll                      |
| Campos condicionais               | Sempre visíveis, valores alternam              | Renderização condicional (`if (item_type == 0) ...`)       |
| Save workflow                     | Botão "Save" global por aba                   | Save por entidade com dirty flag                            |
| Delete workflow                   | Botão normal (sem confirmação)                | Botão vermelho; referential guard com mensagem              |
| Undo                              | Sim, só em Zones                              | Não (só Revert pré-save)                                    |
| Atalhos de teclado                | F1-F4 + Ctrl+Z/D + Del + numpad (Zones)       | Enter/Esc básicos do ImGui                                  |
| Preview 3D                        | Sim em Actors/Media/Particles                 | Sim em Media (Models + Actor Defs) com IBL + bones          |
| Feedback de erro                  | MessageBox modal                              | Status bar inline + console stderr                          |
| Hot-reload de mudanças            | Requer clicar Save                             | `needFetch_ = true` refaz a consulta                        |
| Multi-window                      | Não (tudo dentro de WMain)                    | Não (ImGui root window único)                               |
| File dialog                       | FUI wrapper do Win32                           | Win32 `OPENFILENAMEW` direto                                |
| Confirmação destrutiva            | "Also add subfolders?" YES/NO quando relevante| Nenhuma (dirty Save/Revert protege)                         |

---

## 9. Padrões de código

### Original

Cada aba tem:

- **Criação de widgets** (centenas de linhas em `GUE.bb`):
  ```
  BItemNew = FUI_Button(TItems, 20, 20, 100, 20, "New item")
  TItemName = FUI_TextBox(TItemsGeneral, 95, 50, 400, 20)
  ```
- **Load/Save** em módulos separados (`Items.bb`, `Actors.bb`):
  ```
  Function LoadItems(File$)
      F = ReadFile(File$)
      For i = 1 To TotalItems
          It.Item = New Item
          It\Name$ = ReadString$(F)
          It\Type = ReadByte(F)
          ...
  ```
- **Event loop** que processa `Case <WidgetID>` por widget:
  ```
  Case SItemValue
      F = OpenFile("Data\Server Data\Items.dat")
      SeekFile F, <offset do campo value do item selecionado>
      WriteInt F, E\EventData
      CloseFile F
  ```

Cada mudança escreve **imediatamente** ao arquivo (sem botão Save em muitos casos) — padrão "ação → effect".

### RCO

Cada aba é uma **classe C++** no namespace `gue`:

```cpp
class ItemsTab {
public:
    void Draw(sqlite3* db);
private:
    void Fetch(sqlite3* db);
    bool Save(sqlite3* db, ItemTemplate& t);
    bool Delete(sqlite3* db, int id);

    std::vector<ItemTemplate> items_;
    int          selected_  = -1;
    ItemTemplate editing_;
    bool         dirty_     = false;
    bool         needFetch_ = true;
    bool         showNew_   = false;
    ItemTemplate newItem_;
    char         statusMsg_[128] = {};
};
```

Padrão consistente em todas as 5 abas:
1. `Fetch()` — um `SELECT ... ORDER BY id` com `sqlite3_prepare_v2` + loop `sqlite3_step`
2. `Save()` — INSERT se `id == 0`, UPDATE senão; sempre usa prepared statements com binds tipados
3. `Delete()` — com guarda referencial opcional
4. `Draw()` — renderização imgui condicional (`if (showNew_) ... else if (selected_ >= 0) ... else ...`)
5. `DrawFields()` — função estática livre (não método) que recebe a estrutura por ref e retorna `bool changed`

**Estado de UI sempre membro da classe**, nada global. Estado do RC original é **todo global** (todas as variáveis `Global CActorSelected`, `Global SItemValue`…).

### Reuso

- Original: `F-UI.bb` é o único framework de widgets. Reuso vem dele.
- RCO: `gue::MediaTab::EnsureTables()` é chamado por `ActorsTab::Draw()` via `media->EnsureTables(db)`. Actor picker (combo de Actor Defs) é implementado em `actors.cpp` acessando `media->ActorDefs()` — **cross-tab dependency** é explícita (ponteiro passado pelo `main.cpp`).

### Tratamento de erro

- Original: `RuntimeError("Could not open <X>!")` aborta o programa.
- RCO: `std::snprintf(statusMsg_, sizeof(statusMsg_), "Fetch error: %s", sqlite3_errmsg(db))` — não crasha, mostra erro na status line.

### Configuração de schema/migração

- Original: cada vez que muda o layout do `.dat`, precisa escrever código de upgrade explícito ou quebrar DBs existentes.
- RCO: `EnsureTable()` é `CREATE TABLE IF NOT EXISTS` + `ALTER TABLE ... ADD COLUMN ... IF NOT EXISTS`. Migração **automática e idempotente** (o `ADD COLUMN` com `DEFAULT` é seguro em Postgres e SQLite).

---

## 10. Tabela-resumo final

| Dimensão                        | Original RC 1.26                        | RCO                                       | Vencedor           |
|---------------------------------|-----------------------------------------|-------------------------------------------|--------------------|
| Escopo                          | Editor monolítico total                 | Editor focal de dados server              | Original (amplitude) |
| Cobertura de features do engine | ~100% do RC 1.26                        | ~40% do que seria "tudo"                  | Original           |
| Código: linhas/tamanho          | 44.327 linhas                           | 3.667 linhas                              | RCO (simplicidade) |
| Manutenibilidade                | Blitz3D + arquivos `.dat` binários      | C++ moderno + SQLite                      | RCO                |
| Portabilidade                   | Windows-only (DirectX 7)                | Portável em tese (Windows-only só por file dialog) | RCO         |
| Editor 3D in-app                | Sim, zona completa                      | Não (delegado ao terrain-editor)          | Original           |
| Composição de actors            | Rígida (5 hairs × 5 faces × 2 genders) | Flexível (N slots × arbitrary models)     | RCO                |
| Rendering quality               | DirectX 7 single texture                | PBR + IBL + deferred                      | RCO                |
| Velocidade de iteração          | Salvar .dat → restart server            | Edit DB direto com WAL, server lê no restart | Empate          |
| Preview de animação             | Sim (Blitz3D animset)                   | Sim (4 weights/vertex skinning com SSBO)   | Empate técnico (RCO tecnicamente melhor) |
| Atalhos de teclado              | Ricos (F1-4, Ctrl+Z/D, numpad)          | Básicos (ImGui default)                    | Original           |
| Undo                            | Sim (Zones)                             | Não                                        | Original           |
| Import de assets                | Copy manual + adicionar ao banco        | Automático (dialog → copy + register)     | RCO                |
| Scripts integrados              | Picker de `.rsl` em todo lugar         | Convenção de path (`scripts/.../<id>.lua`) | Empate (diferentes filosofias) |
| Visual polish                   | Windows98 + borders custom              | Dark modern flat                           | RCO (subjective)   |
| Feedback de erro                | MessageBox modal bloqueante             | Status line + sqlite errmsg                | RCO                |
| Guardas referenciais            | Nenhuma                                 | Delete bloqueado se em uso                 | RCO                |
| Build do jogo                   | In-GUE (Build full install/server)      | CMake + bats externos                      | Empate (trade-off)|
| Build do servidor               | Com opção MySQL embed                   | `go build`                                 | Empate             |
| Performance                     | DirectX 7 em tudo                       | GL 4.6 deferred, só aba ativa renderiza   | RCO                |
| DB inspection externa           | Não (formato proprietário)              | Qualquer SQLite browser                    | RCO                |

---

**Fim do arquivo.** O RCO GUE não tenta ser "o original modernizado" — tenta ser **a ferramenta certa para o subset do jogo que está implementado**. As 10 abas que faltaram (Project, Particles, Combat, Projectiles, Factions, Attributes, Days & seasons, Interface, Other, e a parte 3D de Zones) ou não fazem mais sentido (Project → CMake), ou esperam o engine implementar o sistema antes (Factions, Particles, Quests…), ou foram delegadas a ferramentas dedicadas (Zones → terrain-editor). Quando um desses sistemas chegar ao RCO, ele ganha uma aba nova — não uma reescrita.
