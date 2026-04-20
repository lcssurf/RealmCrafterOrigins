# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

**RealmCrafter: Origins (RCO)** is a modern MMORPG engine built from scratch, using RealmCrafter Standard 1.26 as functional reference. The goal is to replicate and surpass every system of the original RC — terrain, networking, scripting, tools, client rendering — using a modern, maintainable stack.

The original RC codebase lives at `D:/Github/RealmCrafter-Standard-1.26/` and should be consulted whenever implementing a system that already exists there. Run the RC executables at `D:/Github/RealmCrafter-Standard-1.26/(Build) Game files/` to observe expected behavior before implementing it in RCO.

There is no protocol compatibility with the original RC client — RCO is a completely independent engine.

## Stack

| Layer | Technology | Notes |
|---|---|---|
| Client / Rendering | C++ + OpenGL 4.6 | No external engine (Unity/Godot/Unreal excluded by design) |
| Window & Input | GLFW + GLAD | Cross-platform |
| Math | GLM | Vectors, matrices, transforms |
| 3D Models | Assimp | Loads .glb/.fbx/.obj — models authored in Blender |
| Texture loading | STB Image + libktx | PNG/JPG for UI; KTX2/BC7 for game textures |
| Shaders | GLSL | All rendering via GLSL shaders |
| Shadow system | Cascaded Shadow Maps (CSM) | Crisp near shadows, soft far shadows |
| Editor / HUD UI | Dear ImGui | In-process editor and debug UI |
| Networking (client) | msquic | Microsoft's QUIC implementation for C++ |
| Networking (server) | quic-go | Native Go QUIC implementation |
| Server | Go 1.22+ | Authoritative game server |
| Scripting | Lua 5.4 via sol2 | Replaces RC's BriskVM; game logic written in Lua |
| Database | PostgreSQL via pgx | Persistent storage |
| Audio | miniaudio | Sound effects and music, header-only |
| Config | TOML | Client and server configuration files |
| Build (client/tools) | CMake + vcpkg | C++ dependency management |
| Build (server) | Go modules | |

## Asset Pipeline

```
Blender → export .glb → Assimp loads → OpenGL renders
```

All 3D models (characters, NPCs, items, scenery) are authored in Blender and exported as `.glb` (preferred) or `.fbx`. Assimp handles loading and delivers vertices, normals, UVs, and animations to the renderer.

### Textures

| Format | Usage |
|---|---|
| PNG | UI, icons |
| KTX2 / BC7 | Game textures (GPU-compressed) |
| HDR | Skybox, ambient lighting (IBL) |

### Materials — PBR

All materials use Physically Based Rendering. Each material consists of:

| Map | Purpose |
|---|---|
| Albedo | Base color |
| Normal | Surface detail without extra geometry |
| Roughness | Matte vs. glossy |
| Metallic | Metal vs. non-metal |
| AO | Ambient occlusion (contact shadows) |

Blender exports all PBR maps natively inside `.glb`. The GLSL shader reads each map and computes the physically correct result.

### Shadows & Lighting

| System | Detail |
|---|---|
| Cascaded Shadow Maps (CSM) | Crisp shadows near camera, soft at distance — standard for open-world MMORPGs |
| Directional Light | Sun / moon — one primary source |
| Point Lights | Torches, spells, effects |
| IBL (Image Based Lighting) | Ambient lighting via HDR skybox |

## Repository Structure

```
RealmCrafterOrigins/
├── client/                  # C++ client + rendering engine
│   ├── src/
│   │   ├── core/            # Engine loop, window, input
│   │   ├── renderer/        # OpenGL rendering
│   │   │   ├── terrain/     # Chunked terrain, LOD, triplanar shading
│   │   │   ├── actors/      # Character/NPC rendering
│   │   │   └── fx/          # Particles, environment effects
│   │   ├── net/             # Client networking (msquic/QUIC)
│   │   ├── ui/              # ImGui HUD and menus
│   │   └── scripting/       # Lua client hooks
│   ├── shaders/             # GLSL shaders (.vert / .frag)
│   ├── assets/              # Textures, models, sounds (gitignored if large)
│   └── CMakeLists.txt
├── server/                  # Go authoritative game server
│   ├── cmd/server/          # main.go entry point
│   ├── internal/
│   │   ├── net/             # QUIC networking, packet handling (quic-go)
│   │   ├── world/           # Areas, actors, items, spells
│   │   ├── accounts/        # Auth, character management
│   │   ├── scripting/       # Lua game logic execution
│   │   └── db/              # PostgreSQL persistence (pgx)
│   └── go.mod
├── tools/                   # World/content editors (C++ + ImGui)
│   ├── terrain-editor/      # Chunked terrain editor
│   ├── gue/                 # Grand Unified Editor (items, actors, spells, areas)
│   └── CMakeLists.txt
├── shared/                  # Protocol definitions, shared constants
│   └── protocol/            # Packet structs (source of truth for client/server)
├── scripts/                 # Default Lua game scripts
│   └── server/              # Game logic (replaces RC's Data/Server Data/Scripts/)
└── CLAUDE.md
```

## Building

### Prerequisites
- CMake 3.20+
- MSVC 2022 or GCC 13+
- Go 1.22+
- PostgreSQL 16+
- vcpkg (manages: GLFW, GLAD, GLM, ImGui, Assimp, msquic, sol2, Lua 5.4, miniaudio)

### Client & Tools
```bash
cd client
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Server
```bash
cd server
go build ./cmd/server
```

### Running locally
```bash
# Terminal 1 — server
cd server && ./server

# Terminal 2 — client
cd client/build && ./rco_client
```

## Testing

There is no protocol bridge with RC 1.26. Testing strategy:

- **Server only**: write Go tests in `server/internal/` with `go test ./...`
- **Client + server**: run both locally and connect the RCO client
- **Behavior reference**: run the original RC (`D:/Github/RealmCrafter-Standard-1.26/(Build) Game files/`) to observe expected behavior, then replicate in RCO
- **Visual reference**: read RC source modules at `D:/Github/RealmCrafter-Standard-1.26/Engine Source/` to understand original implementation before writing the new one

## Current State (updated as work progresses)

### Phase 1 — DONE ✓
- Go server boots, accepts QUIC connections (quic-go)
- Protocol: binary framing `[u16 type][u32 len][payload]`, ~20 packet types
- Accounts + DB: SQLite default (zero-install), PostgreSQL optional
- Client: GLFW window, OpenGL 4.6, Dear ImGui
- Login screen + character select (9 slots, create/delete)
- Player enters world, position synced server↔client at 10Hz

### Phase 2 — DONE ✓
- Procedural chunked terrain (8×8 chunks, 64×64 verts each, triplanar shading)
- Procedural skybox with sun disc + gradient
- Actor renderer via Assimp (.glb) — falls back to placeholder box
- Third-person orbital camera (RMB drag, scroll zoom)
- WASD movement with yaw facing direction
- NPC spawning on server, broadcast via PNewActor to all clients
- Multiple actors rendered in world (players + NPCs)

### Phase 3 — IN PROGRESS
1. **Chat** ✓ DONE — P_ChatMessage, area broadcast, ImGui fade UI
2. **Inventory + Items** ✓ DONE (basic) — item_templates + character_items DB, PInventoryUpdate, ImGui equip+bag window
3. **Basic combat (melee)** ✓ DONE — P_AttackActor/Dead/StatUpdate/FloatingNumber, NPC AI (wait/chase/aggro), respawn, damage formula, floating numbers, target indicator
4. **Player death + respawn** ← NEXT — death overlay, PRespawnPlayer packet, server teleports to spawn with full HP
5. **XP ao matar + level up** — P_XPUpdate, progressão em `world/progression.go` (tabelas isoladas, migrar para Lua quando scripting existir)
6. **Lua scripting** — server-side NPC/quest logic (replaces RC's BriskVM); progressão e XP serão os primeiros scripts portados
7. **Portals / ChangeArea** — zone transitions
8. **Spells** — cast, cooldown, effects
9. **Quests** — log, objectives
10. **Factions** — reputation, aggression
11. **Trading** — NPC shop, player-to-player

### Inventory — known gaps (RC parity backlog)
| Gap | Status | Notes |
|---|---|---|
| 46 slots (14 equip + 32 bag) | ✓ Done | Fixed from 45 |
| Drag-and-drop entre slots | ✓ Done | ImGui BeginDragDropSource/Target + PInventorySwap |
| Validação de slot por tipo | ✓ Done | canEquipInSlot() + server-side slotTypeMatches() |
| weapon_damage / armor_level por item | ✓ Done | DB + packet + tooltip |
| Durabilidade funcional (broken=vermelho) | ✓ Done | Slot fica vermelho, cannot equip |
| Stack split (mover X de Y) | Pending | RC: `"A" + SlotFrom + SlotTo + Amount` |
| Ícone/thumbnail por item | Pending | Needs texture system + ThumbnailTexID per item |
| Race/class restriction por item | Pending | RC: ExclusiveRace/ExclusiveClass + ActorHasSlot() |
| Item attributes (40 valores) | Pending | RC: Attributes.Value[0..39] bonuses on equip |
| Drop item no chão | Pending | RC: `"D" + Slot + Amount` → cria DroppedItem no mundo |
| P_InventoryUpdate "O" para outros | Pending | Broadcast equip appearance to other players |

### Combat — backlog (do zero, baseado no RC)
| Feature | Pacote RC | Status |
|---|---|---|
| Attack request + hit result | P_AttackActor = 18 | ✓ Done |
| Morte NPC/player | P_ActorDead = 19 | ✓ Done (NPC); player death overlay pending |
| HP sync | P_StatUpdate = 22 | ✓ Done |
| Damage numbers flutuantes | P_FloatingNumber = 48 | ✓ Done |
| 3 fórmulas de damage (força, arma, armor) | — | ✓ Done |
| CombatDelay (cooldown de ataque) | — | ✓ Done (800ms) |
| Aggro NPC (agressivo/defensivo/passivo) | — | ✓ Done |
| Respawn NPC após morte | — | ✓ Done (30s) |
| Player death overlay + respawn | P_RespawnPlayer = 108 | ← NEXT |
| XP ao matar + P_XPUpdate | P_XPUpdate = 32 | Pending — world/progression.go (migrar para Lua depois) |
| Animação de hit | P_AnimateActor = 30 | Pending |
| AICallForHelp (NPCs próximos ajudam) | — | Pending |
| GetArmourLevel (soma AP de todos equips) | — | ✓ Done (CachedArmor) |

### Decisão arquitetural — XP/progressão
Fórmulas de engine (dano, armor, hit chance) ficam em Go fixo — são invariantes do sistema.
Progressão (XP por kill, curva de level up, stats por level) vai para `world/progression.go` com tabelas isoladas e fáceis de localizar. Quando Lua scripting estiver pronto, esse arquivo é o primeiro a ser portado para scripts editáveis pelo GUE.

### Phase 4 — NOT STARTED
Tools only make sense after systems exist:
- Terrain editor (saves heightmaps → client loads instead of procedural)
- GUE (Grand Unified Editor) — items, actors, spells, areas
- Architecture / tree / rock editors

---

## Implementation Order

### Phase 1 — "Enter the World" ✓ DONE
1. **Server core** — Go server boots, accepts QUIC connections
2. **Protocol** — define login, character select, and enter-world packets in `shared/protocol/`
3. **Accounts + DB** — create account, create character, persist to SQLite/PostgreSQL
4. **Client window + loop** — C++ opens window, connects to server via QUIC
5. **Player in world** — position synced between client and server, basic movement

### Phase 2 — "See the World" ✓ DONE
6. **Terrain renderer** — chunked terrain with triplanar shading
7. **Skybox + lighting** — procedural sky, directional sun
8. **Actor renderer** — Assimp .glb loader, placeholder box fallback
9. **Camera** — third-person orbital camera
10. **NPCs in world** — server spawns, client renders multiple actors
11. **Movement sync** — client→server position at 10Hz, yaw from movement direction

### Phase 3 — "Interact" (current)
12. **Chat** — in-game text chat by area
13. **Inventory + items** — item templates, slots, equip
14. **Basic combat (melee)** — attack, damage, death
15. **Lua scripting** — NPC behavior, quest hooks
16. **Portals / ChangeArea** — zone transitions
17. **Spells + projectiles**
18. **Quests, factions, trading**

### Phase 4 — "Content"
19. **Tools (GUE / terrain editor)** — create areas, items, NPCs without touching code
20. **Remaining RC systems** (weather, mounts, party, etc.)

## RC Systems to Replicate

Every system from RC 1.26 must exist in RCO. Use RC source at `D:/Github/RealmCrafter-Standard-1.26/Engine Source/` as reference for each.

### Where each system lives (Server vs Client)

| System | RC Module | Server (Go) | Client (C++) | Status |
|---|---|---|---|---|
| Account creation & login | AccountsServer, MainMenu | Auth, bcrypt, DB | Login UI (ImGui) | ✓ Done |
| Character creation & selection | MainMenu, ClientLoaders | Char CRUD, DB | 9-slot UI | ✓ Done |
| Multiple character slots | AccountsServer | 9 slots in DB | Selection grid | ✓ Done |
| Player movement & position sync | GameServer, ClientNet | Validate, broadcast | WASD, yaw, 10Hz sync | ✓ Done |
| Areas / zones | ServerAreas, ClientAreas | Area map, transitions | Load terrain/scenery | Partial |
| Terrain (heightmap, textures) | RC Terrain Editor | — (client-side) | Chunked, triplanar | Procedural only |
| Environment (skybox, weather, day/night) | Environment3D | Weather state | Sky render | Skybox only ✓ |
| NPC actors & spawners | Actors, ServerAreas | Spawn, AI, broadcast | Render actor | Spawn ✓, AI pending |
| Third-person camera | Client | — | Orbital camera | ✓ Done |
| Player character renderer | Actors3D | — | Assimp .glb + fallback | ✓ Done |
| Chat system | GameServer, ClientNet | Broadcast by area | Chat UI (ImGui) | Server ✓, UI pending |
| Inventory & item system | Items, Inventories | Templates, slots, equip | ImGui window | Pending |
| Equipment & appearance | Actors3D | Equip slot tracking | Swap mesh/texture | Pending |
| Basic combat (melee) | ServerCombat, ClientCombat | Damage calc, HP, death | Anim, floating numbers | Pending |
| Lua scripting (server-side) | Scripting, BriskVM | sol2 + Lua 5.4 | — | Pending |
| Portals / teleportation | ServerAreas | P_ChangeArea handler | Trigger collision | Pending |
| Waypoints (NPC pathing) | ServerAreas | Linked list, patrol AI | — | Pending |
| Spells & magic | Spells | Cooldown, execution | Spellbook UI, FX | Pending |
| Projectiles | Projectiles | Template, homing, damage | Render, trail FX | Pending |
| Factions | GameServer | Rating matrix, aggression | Reputation UI | Pending |
| Trading | GameServer | Validate, exchange | Shop/trade UI | Pending |
| Quests | Scripting, ScriptingCommands | State, log entries | Quest log UI | Pending |
| HUD & game UI | Gooey | — | HP/EP bars, action bar | Partial |
| Radar / minimap | Radar | — | Minimap render | Pending |
| Audio (SFX + music) | Media | P_Sound, P_Music | miniaudio, 3D sound | Pending |
| Particles & FX | RottParticles, Projectiles3D | P_CreateEmitter | Particle system | Pending |
| Skeletal animation | Animations | — | Assimp anim, blend | Pending |
| Trees & vegetation | RCTrees | — | Instanced render | Pending |
| Rocks & scenery | RC Rock Editor | Scenery in area data | Load .glb, lightmap | Pending |
| Architecture / buildings | RC Architect | — | Load .glb | Pending |
| Weather / day-night | Environment3D | P_WeatherChange | Sky, fog, lighting | Pending |
| Mounts | Actors | Rider/Mount fields | Attach actor to actor | Pending |
| Party system | GameServer | Party tracking | Party HP bars | Pending |
| Grand Unified Editor (GUE) | GUE | — | ImGui editor app | Phase 4 |
| Terrain editor | RC Terrain Editor | — | Heightmap editor | Phase 4 |
| Architecture editor | RC Architect | — | Building placement | Phase 4 |
| Client update/patch system | UpdatesServer | File diff server | Auto-updater | Phase 4 |

### Key RC packet types (reference for protocol additions)

```
Already implemented:  1-5 (accounts/chars), 11,13,14 (actors), 12 (start game),
                      16 (chat), 60 (kicked), 100-105 (RCO extensions)

Still needed:
  9  = P_ChangeArea          (portal/zone transition)
  15 = P_InventoryUpdate     (inventory sync)
  17 = P_WeatherChange       (weather broadcast)
  18 = P_AttackActor         (melee attack request)
  19 = P_ActorDead           (death notification)
  20 = P_RightClick          (interact with NPC)
  21 = P_Dialog              (NPC dialog text)
  22 = P_StatUpdate          (attributes changed)
  23 = P_QuestLog            (quest entry update)
  26 = P_KnownSpellUpdate    (learn spell)
  27 = P_SpellUpdate         (cooldown update)
  28 = P_CreateEmitter       (particle effect)
  29 = P_Sound               (play SFX)
  30 = P_AnimateActor        (play animation)
  33 = P_ScreenFlash         (screen flash)
  34 = P_Music               (change music)
  35 = P_OpenTrading         (start trade)
  36 = P_ActorEffect         (buff/debuff icon)
  37 = P_Projectile          (launch projectile)
  38 = P_PartyUpdate         (party member status)
  39 = P_AppearanceUpdate    (equipment visual)
  48 = P_FloatingNumber      (damage/heal number)
  50 = P_Speech              (NPC voice line)
```

## Key Design Decisions

- **Server-authoritative**: all game logic runs on the server. Client sends input, receives state.
- **Protocol defined first**: always define packet structs in `shared/protocol/` before implementing either side.
- **Chunked terrain**: no vertex count cap — terrain uses chunked meshes with LOD.
- **Triplanar mapping**: GLSL shader eliminates texture stretching on slopes.
- **Lua scripting**: game designers write Lua scripts, not compiled code.
- **QUIC only**: no legacy protocol support. Both client and server speak QUIC exclusively.

## RC 1.26 Reference — What NOT to Replicate

| RC Limitation | RCO Solution |
|---|---|
| 250×250 vertex terrain cap (DirectX 8) | Chunked terrain with LOD |
| Texture stretching on slopes (top-down UV) | Triplanar mapping in GLSL |
| No shader support | GLSL shaders for everything |
| Binary config files (.dat) | TOML or JSON |
| Hardcoded 1024×768 in editors | Resolution-independent ImGui UI |
| BriskVM proprietary scripting | Lua 5.4 |
| ENet/RottNet UDP | QUIC (encrypted, multiplexed, modern) |
