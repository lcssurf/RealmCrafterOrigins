# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

**RealmCrafter: Origins (RCO)** is a modern MMORPG engine built from scratch, using RealmCrafter Standard 1.26 as functional reference. The goal is to replicate and surpass every system of the original RC — terrain, networking, scripting, tools, client rendering — using a modern, maintainable stack.

The original RC codebase lives at `D:/Github/RealmCrafter-Standard-1.26/` and should be consulted whenever implementing a system that already exists there. Run the RC executables at `D:/Github/RealmCrafter-Standard-1.26/(Build) Game files/` to observe expected behavior before implementing it in RCO.

There is no protocol compatibility with the original RC client — RCO is a completely independent engine.

---

## Stack

| Layer | Technology | Notes |
|---|---|---|
| Client / Rendering | C++ + OpenGL 4.6 | No external engine |
| Window & Input | GLFW + GLAD | Cross-platform |
| Math | GLM | Vectors, matrices, transforms |
| 3D Models | Assimp | Loads .glb/.fbx/.obj |
| Texture loading | STB Image + libktx | PNG/JPG for UI; KTX2/BC7 for game |
| Shaders | GLSL | All rendering via GLSL shaders |
| Shadow system | Cascaded Shadow Maps (CSM) | Crisp near, soft far |
| Editor / HUD UI | Dear ImGui | In-process editor and debug UI |
| Networking (client) | msquic | Microsoft QUIC for C++ |
| Networking (server) | quic-go | Native Go QUIC |
| Server | Go 1.22+ | Authoritative game server |
| Scripting (server) | gopher-lua (Lua 5.1) | Replaces RC's BriskVM — NOT sol2 |
| Database | SQLite (dev) / PostgreSQL (prod) | SQLite is default, zero-install |
| Audio | miniaudio | Header-only, integrated |
| Particles | GL_POINTS (additive blend) | `client/src/renderer/particles.h/.cpp` |
| Config | TOML | Client and server configuration |
| Build (client) | CMake + vcpkg | vcpkg root: C:/vcpkg |
| Build (server) | Go modules | |
| GUE (editor tool) | C++ + Dear ImGui + SQLite direct | `tools/gue/` |
| Terrain editor | C++ + Dear ImGui + OpenGL 4.6 | `tools/terrain-editor/` |

> **Note on scripting**: The stack originally planned sol2 (C++ Lua), but server scripting uses **gopher-lua** (pure Go, Lua 5.1). Client-side scripting is not yet implemented.

---

## Layout (source vs runtime)

The project is split into **source code** (versioned, edited in IDE) and **runtime/ship tree** (`dist/`, what actually runs and what gets zipped to deliver the game).

```
RealmCrafterOrigins/
├── client/     # C++ source (src/, CMakeLists.txt)
├── server/     # Go source (cmd/, internal/, go.mod)
├── tools/      # GUE + terrain editor C++ source
│
└── dist/                     # EVERYTHING runtime lives here
    ├── client/               # what ships to players
    │   ├── rco_client.exe
    │   ├── *.dll             # assimp, glfw, msquic, etc (deployed by vcpkg applocal)
    │   ├── shaders/          # GLSL source files (runtime, edited in place)
    │   ├── assets/           # models, UI textures, audio
    │   └── data/             # terrain heightmaps + material libraries
    │
    ├── server/               # private — never ships to players
    │   ├── server.exe
    │   ├── config.toml
    │   ├── rco.db            # SQLite — all stats/NPCs/spells live here
    │   ├── scripts/          # Lua server logic (damage formulas, AI, dialogs)
    │   └── certs/            # TLS keys (optional; self-signed is generated at startup)
    │
    └── tools/                # dev-only — never ships
        ├── rco_gue.exe
        ├── rco_terrain.exe
        └── *.dll
```

**Every exe `chdir`s to its own directory on startup** (via `SetCwdToExeDir()` helper in C++ and `anchorCwdToExeDir()` in Go). This means:
- All relative paths in code (`shaders/`, `assets/`, `data/`, `scripts/`, `config.toml`) resolve relative to where the exe is installed — not relative to where it was launched.
- GUE reads DB via `../server/rco.db` (sibling folder).
- Terrain editor writes via `../client/data/areas/` (sibling folder).

## Building

```bash
# Server (Go)
cd server && go build -o ../dist/server/server.exe ./cmd/server

# Client (Windows, vcpkg at C:/vcpkg)
cd client
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release      # outputs to dist/client/

# GUE + Terrain editor
cd tools/gue
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release      # outputs to dist/tools/

cd tools/terrain-editor
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release      # outputs to dist/tools/

# Or use the .bat helpers at repo root: build-client.bat, build-server.bat, build-gue.bat, build-terrain-editor.bat

# Run
cd dist/server  && ./server.exe           # Terminal 1
cd dist/client  && ./rco_client.exe       # Terminal 2
cd dist/tools   && ./rco_gue.exe          # GUE (edits dist/server/rco.db directly)
cd dist/tools   && ./rco_terrain.exe      # Terrain editor (writes dist/client/data/)
```

## Shipping a public build

```bash
7z a rco-client-v0.1.zip dist/client/     # only the client folder
```

Nothing from `dist/server/` or `dist/tools/` ever ships — those are internal ops artifacts.

## Security model

- **Client never reads the DB.** All data (spell names, item stats, NPC info) arrives via filtered packets. `spell_templates.damage_min/max` never leaves the server.
- **Damage formulas and AI live in Lua**, in `dist/server/scripts/`. Client decompilation reveals nothing about game balance.
- **Lua scripts are server-only**. Reverse engineering the client exe doesn't expose any game-logic code.
- **TLS**: QUIC requires TLS. A self-signed cert is generated automatically if `certs/` is empty; for production put a real cert+key there and reference them in `dist/server/config.toml`.

---

## Quick Reference Map

> Use this to find where each system lives before reading the full codebase.

### Protocol (packet IDs)

**Source of truth:** `server/internal/protocol/packets.go` (Go) and `client/src/net/protocol.h` (C++)

| ID | Name | Direction | Status |
|---|---|---|---|
| 1 | PCreateAccount | C→S | ✓ |
| 2 | PVerifyAccount | C→S | ✓ |
| 3-5 | PFetchCharacter, PCreate/DeleteChar | C→S | ✓ |
| 9 | PChangeArea | S→C | ✓ |
| 11 | PNewActor | S→C | ✓ — payload: rid(u32)+name(str)+race(str)+class(str)+level(u16)+x/y/z/yaw(f32)+hp/hp_max(i32)+actor_type(u8) + appearance { num_meshes(u8) + [slot(u8)+model_path(str)+scale(f32)+albedo/normal/orm(str)+albedo_rgb(f32)+roughness/metallic(f32)] + num_anims(u8) + [action(str)+source_path(str)+clip_override(str)] }. num_meshes=0 → client uses default fallback model. |
| 12 | PStartGame | S→C | ✓ |
| 13 | PActorGone | S→C | ✓ |
| 14 | PStandardUpdate | C↔S | ✓ |
| 15 | PInventoryUpdate | S→C | ✓ |
| 16 | PChatMessage | C↔S | ✓ |
| 18 | PAttackActor | C→S | ✓ |
| 19 | PActorDead | S→C | ✓ |
| 20 | PRightClick | C→S | ✓ |
| 21 | PDialog | S→C | ✓ |
| 22 | PStatUpdate | S→C | ✓ |
| 24 | PGoldChange | S→C | ✓ |
| 26 | PKnownSpells | S→C | ✓ — payload: count(u8) + [id(u16)+name(str)+type(u8)+ep(u16)+cd(u32)+range(f32)+icon(u8)+aoe_type(u8)+aoe_radius(f32)] |
| 28 | PCreateEmitter | S→C | ✓ — payload: type(u8)+x(f32)+y(f32)+z(f32)+dur(u16) |
| 29 | PSound | S→C | ✓ — payload: sound_id(u8)+volume(u8) |
| 32 | PXPUpdate | S→C | ✓ |
| 34 | PMusic | S→C | ✓ — payload: track(u8)+volume(u8) |
| 48 | PFloatingNumber | S→C | ✓ |
| 49 | PRepositionActor | S→C | ✓ |
| 60 | PKickedPlayer | S→C | ✓ |
| 100-105 | Login/char results, ping/pong | both | ✓ |
| 106 | PInventorySwap | C→S | ✓ |
| 107 | PUseItem | C→S | ✓ |
| 108 | PRespawnPlayer | C→S | ✓ |
| 109 | PPortalInfo | S→C | ✓ |
| 110 | PCastSpell | C→S | ✓ — payload: spell_id(u16)+target_rid(u32)+ground_x(f32)+ground_z(f32) — ground coords are 0 for non-ground AoE |
| 111 | PWorldItem | S→C | ✓ (dropped item spawned) |
| 112 | PDialogChoice | C→S | ✓ |
| 113 | PPickupItem | C→S | ✓ (pick up world item) |
| 114 | PRemoveWorldItem | S→C | ✓ (despawn / picked up) |
| 115 | POpenShop | S→C | ✓ |
| 116 | PShopAction | C→S | ✓ (buy=0 / sell=1) |
| 23 | PQuestLog | S→C | ✗ not implemented |
| 17 | PWeatherChange | S→C | ✗ not implemented |
| 30 | PAnimateActor | S→C | ✓ — payload: rid(u32)+anim_name(str); clip names are arbitrary strings (e.g. "Idle","Walk","Attack","Death") |
| 37 | PProjectile | S→C | ✗ not implemented |
| 38 | PPartyUpdate | S→C | ✗ not implemented |
| 39 | PAppearanceUpdate | S→C | ✗ not implemented |

### EmitterType constants (PCreateEmitter)

| Value | Name | Triggered by |
|---|---|---|
| 0 | Fire | — |
| 1 | Explosion | NPC death (melee + spell) |
| 2 | Heal | — |
| 3 | Portal | Portal traversal |
| 4 | Blood | — |
| 5 | Smoke | — |

### SoundID constants (PSound)

| Value | Name | Triggered by |
|---|---|---|
| 0 | SwordHit | Melee attack lands |
| 1 | SpellFire | Fireball cast |
| 2 | SpellHeal | Heal cast |
| 3 | SpellLight | Lightning Bolt cast |
| 4 | NPCDeath | NPC killed |
| 5 | PlayerDeath | Player dies |
| 6 | LevelUp | Level up |
| 7 | PickupItem | F-key item pickup |
| 8 | Portal | Portal traversal |
| 9 | BuyItem | Shop buy |
| 10 | SellItem | Shop sell |

### MusicTrack constants (PMusic)

| Value | Name |
|---|---|
| 0 | Stop |
| 1 | StarterZone |
| 2 | Forest |
| 3 | Combat |

### Server — Go files by system

| System | File(s) |
|---|---|
| Entry point / DB-driven NPC spawning / drop+shop setup | `server/cmd/server/main.go` |
| QUIC server, client dispatch loop | `server/internal/net/server.go` |
| All packet handlers (per-client state machine) | `server/internal/net/client.go` |
| Packet framing helpers (Reader/Writer) | `server/internal/net/codec.go` |
| Actor struct (player + NPC) — incl. AttackRange, AggressiveRange, Appearance | `server/internal/world/actor.go` |
| Appearance, MeshSlot, AnimBinding structs | `server/internal/world/actor.go` |
| buildAppearance() — resolves actor_def_id via Media registry | `server/cmd/server/main.go` |
| Area (actors map, broadcast, regen, AI loop + NPC movement) | `server/internal/world/area.go` |
| Mob drop tables, DroppedItem, RollDrops | `server/internal/world/drops.go` |
| NPC shop definitions, GetShop, FindShopItem | `server/internal/world/shop.go` |
| Portal struct + CheckPortal | `server/internal/world/portal.go` |
| Combat: ProcessAttack, BroadcastAttack, InMeleeRange | `server/internal/world/combat.go` |
| Spell effects: ApplyDamage, ApplyHeal, ActorsInRadius, broadcasts | `server/internal/world/spell.go` |
| World: NextRuntimeID, GetOrCreateArea, FindActor | `server/internal/world/world.go` |
| XP formula, level-up, stats-per-level | `server/internal/world/progression.go` |
| Packet framing (buildFrame, pb builder) | `server/internal/world/frame.go` |
| All packet ID constants | `server/internal/protocol/packets.go` |
| DB open, migrations, all CRUD — incl. AddStackableItem, LoadActorDef, GetMediaModel, GetMediaMaterial, GetMediaAnimClip | `server/internal/db/db.go` |
| Lua registry, spell/event dispatch, Cast(), PatchAoEFromDB() | `server/internal/scripting/registry.go` |
| Lua API bindings (Spell, Combat, Actor, Dialog, Event, Log) | `server/internal/scripting/api.go` |
| Account service (bcrypt, login) | `server/internal/accounts/accounts.go` |

### Client — C++ files by system

| System | File(s) |
|---|---|
| **Main game loop** (all packet handlers, game state, ImGui, per-NPC Actor instantiation) | `client/src/core/main.cpp` |
| QUIC connection wrapper | `client/src/net/connection.h/.cpp` |
| Packet reader/writer | `client/src/net/codec.h` |
| **All packet ID constants** | `client/src/net/protocol.h` |
| Bag window (I) + Character sheet (C) | `client/src/ui/inventory.h/.cpp` |
| Chat window (fade, history, send) | `client/src/ui/chat.h/.cpp` |
| Spell bar (icons, cooldown overlay, ground-AoE targeting) | `client/src/ui/spellbar.h/.cpp` |
| Floating damage numbers | `client/src/ui/floating_numbers.h/.cpp` |
| Spell visual effects (Fire/Heal/Lightning) | `client/src/ui/spell_effects.h/.cpp` |
| Chat bubbles over actors | `client/src/ui/chat_bubbles.h/.cpp` |
| Game state enum, PlayerState struct | `client/src/ui/game_state.h` |
| Login screen UI | `client/src/ui/login_screen.h/.cpp` |
| Character select UI | `client/src/ui/char_select.h/.cpp` |
| Third-person orbital camera | `client/src/renderer/camera.h/.cpp` |
| Chunked terrain (LOD, triplanar) | `client/src/renderer/terrain/terrain.h/.cpp` |
| Actor renderer + animation state machine | `client/src/renderer/actors/actor.h/.cpp` |
| Skeletal model loader (bones, clips, ComputeBones) | `client/src/renderer/model.h/.cpp` |
| Particle system (GL_POINTS, additive blend) | `client/src/renderer/particles.h/.cpp` |
| Procedural skybox + sun | `client/src/renderer/skybox.h/.cpp` |
| Audio system (miniaudio, pimpl) | `client/src/audio/audio.h/.cpp` |
| UI texture cache (PNG/BMP → GL texture) | `client/src/ui/ui_texture.h` |
| GLFW window wrapper | `client/src/core/window.h` |
| Particle shaders | `dist/client/shaders/particle.vert/.frag` |

### GUE (Grand Unified Editor) — `tools/gue/`

Standalone ImGui executable. Opens `rco.db` **directly** (no server required). On startup auto-detects the DB walking up directories; if not found shows a path dialog. Saves edits immediately to SQLite; restart the server to reload.

| File | Purpose |
|---|---|
| `tools/gue/src/main.cpp` | Window, DB open/path dialog, tab bar |
| `tools/gue/src/tabs/items.h/.cpp` | Items tab — CRUD on `item_templates` |
| `tools/gue/src/tabs/spells.h/.cpp` | Spells tab — CRUD on `spell_templates` incl. AoE fields |
| `tools/gue/src/tabs/actors.h/.cpp` | Actors tab — CRUD on `npc_spawns` (aggressiveness, ranges, respawn) |
| `tools/gue/src/tabs/areas.h/.cpp` | Areas tab — CRUD on `area_config` (music, fog) + `area_portals` (trigger + destination) |
| `tools/gue/src/tabs/media.h/.cpp` | Media tab — registry of models, materials, anim clips, and composed actor defs |
| `tools/gue/CMakeLists.txt` | Build: glfw + glad + imgui + sqlite3 (vcpkg) |

### Terrain Editor — `tools/terrain-editor/`

Standalone OpenGL tool for sculpting heightmaps and painting material splatmaps. Completely independent — no server, no DB required. Outputs `heightmap.bin` + `splatmap.bin` which the client loads via `terrain.LoadFromEditor()`.

| File | Purpose |
|---|---|
| `tools/terrain-editor/src/main.cpp` | Main loop, input, ImGui panels, save/load |
| `tools/terrain-editor/src/heightmap.h/.cpp` | Height data (`float[]`), Save/Load to `.bin`, `SampleHeight()` |
| `tools/terrain-editor/src/splatmap.h` | Per-pixel material weights (RGBA), GPU upload |
| `tools/terrain-editor/src/brush.h` | `ApplyBrush()` (Raise/Lower/Smooth/Flatten) and `PaintSplatmap()` |
| `tools/terrain-editor/src/material.h` | Material descriptor: albedo/normal/roughness PNG paths, `ScanMaterials()` |
| `tools/terrain-editor/src/terrain_renderer.h/.cpp` | Chunked mesh rebuild from heightmap, dirty-region updates |
| `tools/terrain-editor/src/camera.h` | Fly-camera (RMB look, WASD, scroll) |
| `tools/terrain-editor/CMakeLists.txt` | Build: glfw + glad + imgui + glm + stb (vcpkg) |

**Editor modes**: Sculpt (Raise / Lower / Smooth / Flatten) and Paint (per-material splatmap layer).  
**Terrain sizes**: 512×512, 768×768, 1024×1024 (resizable in-editor).  
**Materials**: scanned from `dist/client/data/terrain/materials/` — each subfolder = one material (albedo.png, normal.png, roughness.png). Up to 4 materials blended via UE5 LB_HeightBlend formula.  
**Save path**: `dist/client/data/areas/<area>/heightmap.bin` + `splatmap.bin`.

### Lua Scripts (server-side)

| Script | Purpose |
|---|---|
| `dist/server/scripts/spells/fireball.lua` | Fireball spell (id=1, 20-35 damage) |
| `dist/server/scripts/spells/heal.lua` | Heal spell (id=2, self-cast) |
| `dist/server/scripts/spells/lightning_bolt.lua` | Lightning Bolt (id=3, scales with HP%) |
| `dist/server/scripts/npcs/dialog.lua` | NPC dialog trees (Guard, Merchant, Innkeeper) |
| `dist/server/scripts/events/player_events.lua` | Player event hooks (stub) |

### DB Schema — key tables

| Table | Key columns | Notes |
|---|---|---|
| `accounts` | id, username, password_hash, is_banned | bcrypt passwords |
| `characters` | id, account_id, slot, name, race, class, level, xp, gold, area_name, x/y/z/yaw, health/healthMax, energy/energyMax | gold default=100 |
| `item_templates` | id, name, item_type, slot_type, weapon_damage, armor_level, max_stack, item_value, stackable | item_type: 0=weapon 1=armor 2=consumable 3=misc |
| `character_items` | character_id, slot (0-45), item_id, quantity, durability | slots 0-13 equip, 14-45 backpack |
| `spell_templates` | id, name, spell_type, damage_min/max, ep_cost, cooldown_ms, range, icon, aoe_type, aoe_radius | spell_type: 0=damage 1=heal 2=buff 3=debuff; aoe_type: 0=single 1=around_target 2=ground |
| `character_known_spells` | character_id, spell_id | all spells granted by default |
| `npc_spawns` | id, name, race, class, level, area_name, x/y/z/yaw, aggressiveness, aggressive_range, attack_range, respawn_delay_ms, **actor_def_id** | managed via GUE Actors tab; loaded at server startup. `actor_def_id` → `media_actor_defs.id` |
| `area_config` | name (PK), music_track, fog_density | managed via GUE Areas tab; loaded at server startup via `db.LoadAreaConfigs()` |
| `area_portals` | id, area_name, x/z/radius, target_area, dest_x/y/z/yaw | managed via GUE Areas tab; loaded at server startup via `db.LoadAreaPortals()` |
| `media_models` | id, name, file_path, scale | mesh file registry — GUE Media tab |
| `media_materials` | id, name, albedo/normal/orm paths, albedo_r/g/b, roughness, metallic | PBR material registry — GUE Media tab |
| `media_anim_clips` | id, name, source_path, clip_override | animation clip registry (source_path empty = embedded in model) |
| `media_actor_defs` | id, name | composed actor appearance template |
| `media_actor_meshes` | id, actor_def_id, slot, model_id, material_id | per-slot mesh assignments (0=Body 1=Hair 2=Helm 3=Chest 4=Hands 5=Belt 6=Legs 7=Feet 8=Weapon 9=Shield 10=Attachment) |
| `media_actor_anims` | id, actor_def_id, action, clip_id | action→clip mapping ("Idle", "Walk", "Attack", "Death"…) |

---

## Current State

### Phase 1 — DONE ✓
- Go server boots, accepts QUIC connections (quic-go)
- Protocol: binary framing `[u16 type][u32 len][payload]`
- Accounts + DB: SQLite default (zero-install), PostgreSQL optional via config
- Client: GLFW window, OpenGL 4.6, Dear ImGui
- Login screen + character select (9 slots, create/delete)
- Player enters world, position synced server↔client at 10Hz

### Phase 2 — DONE ✓
- Chunked terrain (64×64 verts/chunk, seamless borders, LOD, triplanar shading, UE5 LB_HeightBlend material blending)
- Terrain loaded from `heightmap.bin` + `splatmap.bin` produced by terrain editor; ray-terrain intersection for click-to-move
- Procedural skybox with sun disc + gradient
- Actor renderer via Assimp (.glb) — falls back to placeholder box
- WoW-style third-person orbital camera (RMB drag = mouse-look + cursor lock, scroll zoom, A/D turn, RMB+A/D strafe)
- WASD movement; player always faces camera forward direction
- NPC spawning on server, broadcast via PNewActor to all clients

### Phase 3 — DONE ✓

| Feature | Status | Notes |
|---|---|---|
| Chat | ✓ | PChatMessage, area broadcast, ImGui fade UI |
| Inventory + Items | ✓ | 46 slots, drag-drop, equip validation, durability, tooltips, item stacking |
| Basic combat (melee) | ✓ | PAttackActor/Dead/StatUpdate/FloatingNumber, 800ms cooldown, aggro AI |
| Player death + respawn | ✓ | death overlay, PRespawnPlayer=108, spawn teleport with full HP |
| XP + level up | ✓ | PXPUpdate=32, world/progression.go, stat restore on level |
| Lua scripting | ✓ | gopher-lua, Spell/Combat/Actor/Event/Dialog API, scripts auto-loaded |
| Dialogs (NPC) | ✓ | PDialog=21, handleRightClick, Lua dialog trees, choice handler |
| Portals / ChangeArea | ✓ | PChangeArea=9, 3s cooldown, actor broadcast on both areas |
| Spells | ✓ | PKnownSpells=26, PCastSpell=110, 3 spells via Lua; AoE + Debuff system |
| Mob drop system | ✓ | PWorldItem=111, PPickupItem=113, PRemoveWorldItem=114, F-key pickup, 60s despawn |
| NPC Shop | ✓ | POpenShop=115, PShopAction=116, buy/sell, PGoldChange=24, Dialog.open_shop() |
| Audio | ✓ | miniaudio integrated; SFX on combat/death/pickup/shop; music per area |
| Particles | ✓ | GL_POINTS system; server triggers PCreateEmitter=28; Explosion/Portal/Heal/Fire/Blood/Smoke types |
| Quests | ✗ Pending | PQuestLog=23 — no code at all |
| Factions | ✗ Pending | no code at all |
| Player-to-player trading | ✗ Pending | POpenTrading=35 — no code at all |
| Animations | ✗ Pending | PAnimateActor=30 — no packet, no system |

### Phase 4 — DONE ✓

| Tool / Feature | Status | Notes |
|---|---|---|
| GUE — Items tab | ✓ | Full CRUD, SQLite direct, no server needed |
| GUE — Spells tab | ✓ | Full CRUD + AoE mode + Debuff type |
| GUE — Actors tab | ✓ | CRUD on `npc_spawns`; NPCs loaded from DB at server startup (no hardcoding) |
| Terrain editor | ✓ | `tools/terrain-editor/` — sculpt + paint, save/load `.bin`, 4-material UE5 blend |
| NPC movement + leash | ✓ | NPCs chase, move toward player at 5 u/s, leash back to spawn at 2.5× AggressiveRange |
| Two-range NPC AI | ✓ | AggressiveRange = detection/chase trigger; AttackRange = melee swing (default 2.0) |
| Bag window (I) | ✓ | Standalone 4×8 backpack grid + gold; toggled with I |
| Character sheet (C) | ✓ | WoW-style: equip slots flanking silhouette, HP/EP/XP bars, Attack/Armor/Gold stats; toggled with C |
| Item stacking | ✓ | `AddStackableItem()` merges onto partial stacks before using free slots; used by pickup and shop buy |
| Spell range system | ✓ | Range stored in `spell_templates`, sent in PKnownSpells; client enforces + shows range/AoE circles |
| WoW camera + controls | ✓ | RMB mouse-look, cursor lock, A/D turn, RMB+A/D strafe, tab-target, target ring |

### Phase 5 — PARTIAL

| Feature | Priority | Notes |
|---|---|---|
| **Animations** | ✓ Done | PAnimateActor=30; skeletal skinning in model.cpp (Assimp bones, 64-bone limit, 4-weights/vertex); string-based clip names (any arbitrary string); `Actor.play_anim(rid, name)` Lua binding; walk auto-detected from player movement |
| **GUE — Areas tab** | ✓ Done | `area_config` (music, fog) + `area_portals` (trigger + dest); server loads from DB at startup |
| **Media system** | ✓ Done | GUE Media tab with 4 sub-tabs (Models, Materials, Anim Clips, Actor Defs); 6 DB tables (`media_*`); `npc_spawns.actor_def_id`; server resolves at spawn via `buildAppearance()` → `world.Appearance`; `PNewActor` carries meshes + anim bindings; client instantiates per-actor `rco::renderer::Actor` with its own Model when appearance data is present, else falls back to shared `player_actor` |
| **Quests** | Medium | PQuestLog=23 — no code at all |
| Factions | Low | no code at all |
| Player-to-player trading | Low | POpenTrading=35 — no code at all |
| Multi-mesh rendering | Low | Appearance carries all mesh slots; currently only slot 0 (Body) is drawn client-side. Hair/Helm/Weapon attachments need bone-attachment rendering |
| Material override at runtime | Low | Server ships albedo/normal/ORM paths + PBR factors, but client currently uses the model's embedded material. Needs refactor of `SubMesh` to accept external texture IDs |
| Preview 3D no GUE | Low | FBO viewport + Assimp in GUE; requires adding Assimp to `tools/gue/CMakeLists.txt` |

---

## Architectural Decisions

- **Server-authoritative**: all game logic runs on the server. Client sends input, receives state.
- **Protocol defined first**: always define packet IDs in `server/internal/protocol/packets.go` AND `client/src/net/protocol.h` before implementing either side. They must match exactly.
- **Chunked terrain**: no vertex count cap — terrain uses chunked meshes with LOD.
- **Triplanar mapping**: GLSL shader eliminates texture stretching on slopes.
- **Lua scripting**: gopher-lua on server only. Drop tables and shop inventories are registered at startup via `setupDropsAndShops()` in main.go using item templates loaded from the DB.
- **Dropped items are in-memory only**: `DroppedItem` structs live in `Area.droppedItems` and are not persisted. They despawn after 60 seconds. RuntimeIDs use the high bit (0x80000000) to avoid collision with actor RuntimeIDs.
- **Gold is persisted**: `characters.gold` column, updated via `db.UpdateGold()`. Loaded into `actor.Gold` at game start.
- **Scripting package never imports net/db**: world package has no DB imports. All DB access happens in the `net` package (client.go) which orchestrates between world, db, and scripting.
- **XP/progression in Go, not Lua**: damage formulas and leveling curves are engine invariants in `world/progression.go`. When Lua scripting matures, progression is the first candidate to be ported to editable scripts.
- **QUIC only**: no legacy protocol support. Both client and server speak QUIC exclusively.
- **SQLite default**: the DB driver defaults to SQLite for zero-install local development. Switch to PostgreSQL via `dist/server/config.toml`.
- **GUE edits DB directly**: the GUE opens `rco.db` with SQLite (no server needed). Changes take effect on next server restart. WAL mode is enabled so server and GUE can coexist on the same file safely.
- **AoE is engine-level, not Lua-level**: `aoe_type` and `aoe_radius` are stored in `spell_templates` and overlay Lua `SpellDef` at startup via `PatchAoEFromDB()`. Lua scripts call `Combat.deal_damage(caster, target, amount)` normally — the Go engine spreads damage to nearby actors automatically when `aoe_type > 0`. Scripts do not need to change.
- **Audio degrades gracefully**: missing `.wav`/`.ogg` files are silently skipped. Sound files live in `data/audio/sfx/` and `data/audio/music/`.
- **DB-driven NPC spawning**: NPCs are stored in `npc_spawns` and loaded at startup via `db.LoadNPCSpawns()`. Do not hardcode NPC lists in `main.go` — use the GUE Actors tab instead.
- **Item stacking via AddStackableItem**: always call `db.AddStackableItem()` (not `FindFreeBackpackSlot + AddItemToSlot`) when giving items to a player. Pass `maxStack=0` to auto-look it up from `item_templates`.
- **Inventory UI split into two windows**: `bag_visible` (I key) = backpack only; `char_visible` (C key) = equipment + stats. Both are fields on the `Inventory` class. Stats (HP/EP/XP/level) must be kept in sync by writing to `inventory.stat_*` fields in every relevant packet handler.
- **ImGui fixed-size windows**: never use `ImGuiWindowFlags_AlwaysAutoResize` combined with a child whose height is computed from `GetContentRegionAvail()` — causes a feedback resize loop. Always use `SetNextWindowSize(..., ImGuiCond_Always)` with a pre-computed fixed height.
- **Terrain chunk seams**: `kChunkSize = (TerrainChunk::kSize - 1) * kCellSize` — the `(kSize-1)` is intentional so adjacent chunks share their border vertex. Sampling uses `gx = cx * stride + x` where `stride = kSize - 1`. Do not change this to `kSize * kCellSize`.
- **Spell range in PKnownSpells**: `range(f32)` is written **before** `icon(u8)` in the packet. Both sides must agree on this order — check `sendKnownSpells()` in `client.go` and the `kPKnownSpells` handler in `main.cpp`.
- **WoW camera yaw convention**: camera sits at `+yaw` from player (at yaw=0 camera is at +Z). Player forward = `{-sin(yr), -cos(yr)}` on XZ. A key **increases** yaw (turns left), D key **decreases** yaw (turns right). Counter-intuitive but correct.
- **Media system composition, not file references**: actor appearance is never hardcoded and never referenced by raw file path from other tables. Everything routes through the Media registry (`media_models`, `media_materials`, `media_anim_clips`, `media_actor_defs` + its two pivot tables). `npc_spawns.actor_def_id` → `db.LoadActorDef(id)` returns the composed definition; `main.go:buildAppearance()` resolves each slot's model+material and each anim mapping's clip into concrete `world.Appearance` at NPC spawn. This is what lets a single "Human Male Body" model be reused across 50 NPCs with different armor + animations without duplicating assets.
- **Appearance travels in PNewActor**: client never queries the DB. The full resolved `Appearance` (meshes with all paths + PBR factors + anim bindings) is serialized in `PNewActor` via `frame.go:newActorPayload()`. Client consumes it in `kPNewActor` and instantiates `std::unique_ptr<rco::renderer::Actor>` per NPC. Appearance nil / `num_meshes == 0` → client falls back to the shared `player_actor` model.
- **WorldActorEntry is move-only**: the client's `world_actors` map holds `WorldActorEntry` with a `std::unique_ptr<Actor>` inside. Copy ctor/assign are deleted; only move allowed. Never use `world_actors[rid] = WorldActorEntry{...}` — write directly through the reference returned by `operator[]`.
- **Source vs runtime split**: `client/`, `server/`, `tools/` at repo root hold ONLY source code (.cpp/.h/.go/CMakeLists/go.mod). Everything runtime lives in `dist/`. Never commit binaries, DBs, or copies of assets back into the source folders. Build outputs → `dist/` only.
- **Tools suppress imgui.ini**: GUE and terrain editor set `io.IniFilename = nullptr` so they don't litter the cwd with ImGui window-state files. The client already does this. If you add a new ImGui-based tool, do the same.
- **Every exe chdirs to its own dir on startup**: client uses `rco::SetCwdToExeDir()` (in `client/src/core/paths.h/.cpp`); GUE and terrain editor have inline copies of the same helper in their `main.cpp`; Go server calls `anchorCwdToExeDir()`. After this single call, every relative path (`shaders/`, `assets/`, `data/`, `scripts/`, `../server/rco.db`, etc.) resolves from the install tree regardless of launcher cwd. This is what makes the `dist/` split work.

---

## Spell AoE System

AoE behavior is configured per-spell in the GUE (Spells tab) or directly in `spell_templates`:

| aoe_type | Behavior | Client UX |
|---|---|---|
| 0 | Single target | Tab-target required; fires immediately |
| 1 | AoE around target | Tab-target required; engine auto-hits all actors within `aoe_radius` of target |
| 2 | Ground-targeted AoE | Press hotkey → slot pulses yellow → reticle circle on terrain → LMB to cast; ESC to cancel |

`aoe_radius` = world units. `PCastSpell` always sends 12 bytes: `spell_id(u16) + target_rid(u32) + ground_x(f32) + ground_z(f32)`. For types 0/1, ground coords are 0.

---

## Media Registry (GUE Media tab)

Central asset registry driven by four SQLite tables. The GUE Media tab exposes four sub-tabs:

| Sub-tab | Table | Purpose |
|---|---|---|
| Models | `media_models` | Mesh file entries: name + file_path (`.glb`/`.fbx`/`.obj`/`.dae`/`.b3d`) + scale |
| Materials | `media_materials` | PBR material bundle: albedo + normal + ORM paths, plus color/roughness/metallic factors |
| Anim Clips | `media_anim_clips` | Named animation clips. `source_path=""` → embedded in the model; else points to a separate FBX/GLB. `clip_override` lets you pick a specific clip name inside the source |
| Actor Defs | `media_actor_defs` + `media_actor_meshes` + `media_actor_anims` | Composes an actor's appearance: any number of mesh slots (Body/Hair/Helm/Chest/Hands/Belt/Legs/Feet/Weapon/Shield/Attachment) each with its own model+material, plus a mapping of action names ("Idle", "Walk", "Attack", "Death"…) to animation clips |

**Path fields have a `[...]` button** (`tools/gue/src/file_import.h`) that opens a native Win32 file dialog (`GetOpenFileNameW`). Picking a file that's already under `dist/client/assets/` references it directly; picking anything else copies it into the appropriate subfolder (`assets/models/`, `assets/textures/`, `assets/anims/`) before storing the relative path. No manual copy/rename needed.

**Resolution flow:**
1. GUE author registers a model in Media → Models
2. Registers a material in Media → Materials (or reuses the model's embedded one by leaving material unset)
3. Registers animation clips in Media → Anim Clips (one row per clip — can share a source file between rows via different `clip_override`s)
4. Composes an Actor Def: adds mesh slots and anim mappings
5. GUE Actors tab → picks the Actor Def for each NPC spawn via `actor_def_id`
6. Server at startup reads `npc_spawns.actor_def_id` → `db.LoadActorDef(id)` → resolves all paths → includes them in `PNewActor` to the client
7. Client receives paths in `PNewActor` and instantiates a per-actor model

Server-side accessors (read-only) in `server/internal/db/db.go`:
- `db.LoadActorDef(ctx, id)` → returns `*ActorDef` with its `Meshes[]` and `Anims[]`
- `db.GetMediaModel(ctx, id)`, `GetMediaMaterial(ctx, id)`, `GetMediaAnimClip(ctx, id)` — individual lookups

**Tables created by `migrateV7`** (runs on every startup, creates tables if missing, adds `npc_spawns.actor_def_id` if absent). Safe on both SQLite and Postgres.

**Client-side composition:**
- `WorldActorEntry.actor` holds `std::unique_ptr<rco::renderer::Actor>` per NPC when the server sends `num_meshes > 0` in `PNewActor`.
- Primary renderable = slot 0 (Body). Other slots are received and kept in the payload but not yet rendered (pending multi-mesh work).
- Separate-file animations (`source_path` non-empty) are loaded via `Actor::LoadAnim(path, action_name)` right after `Init`, so the clip is retrievable by its action name.
- Embedded clips keep their file-resident name and still resolve via `PlayAnim("Walk"/"Attack"/…)`.

**Material overrides** travel in `PNewActor` but the client currently ignores them (uses the GLB's embedded material). This is a known gap; enabling it requires exposing external texture IDs on `SubMesh`.

---

## NPC Setup Pattern

NPCs are defined in the `npc_spawns` DB table and edited via the **GUE Actors tab**. At server startup, `main.go` loads all rows via `db.LoadNPCSpawns()` and calls `gameWorld.SpawnNPC(...)` for each. To add a new NPC, use the GUE — no code changes needed.

Fields editable in GUE:
- `aggressiveness`: 0=passive, 1=defensive, 2=aggressive, 3=dialog-only
- `aggressive_range`: detection radius — NPC starts chasing when player enters this distance
- `attack_range`: swing distance (melee ≈ 2, ranged ≈ 15–25)
- `respawn_delay_ms`: 0 = permanent death; 30000 = 30 sec
- `actor_def_id`: FK to `media_actor_defs` — drives model/material/animation composition. Managed in the Media tab. `0` = use default player model fallback.

To give an NPC a drop table or shop, add entries in `setupDropsAndShops()` in `main.go`:
```go
world.RegisterDropTable("Name", []world.DropEntry{
    entry("Health Potion", 0.5, 1, 2),
})
world.RegisterShop("Name", []world.ShopItem{
    shopItem("Health Potion", 15),
})
```

Then add a Lua dialog in `dist/server/scripts/npcs/dialog.lua` with `Dialog.open_shop()` on the relevant choice.

---

## NPC AI — Two-Range System

NPCs have two independent ranges stored in `actor.go`:

| Field | Purpose | Default |
|---|---|---|
| `AggressiveRange` | Detection radius — NPC enters chase when player steps inside | 8.0 |
| `AttackRange` | Melee swing radius — NPC attacks when within this distance | 2.0 |

AI tick runs every 500 ms in `area.go`:
1. **AIWait** → scan for players within `AggressiveRange` → switch to **AIChase**
2. **AIChase** → if within `AttackRange`, attack; else call `moveNPCToward()` and broadcast `PStandardUpdate`
3. **Leash** → if NPC has travelled more than `AggressiveRange × 2.5` from spawn, teleport back and reset to AIWait

---

## Inventory Slot Layout

```
Slots  0-13  = Equipment (14 slots)
  0=Weapon  1=Shield  2=Hat  3=Chest  4=Hands  5=Belt
  6=Legs  7=Feet  8-11=Ring (x4)  12-13=Amulet (x2)

Slots 14-45 = Backpack (32 slots, 4×8 grid)
```

### Bag window (I)
Standalone window, right side of screen. 4×8 backpack grid + slot counter + gold.

### Character sheet (C)
WoW-style window, positioned left of the bag when both are open.

| Left column (top→bottom) | Right column (top→bottom) |
|---|---|
| Hat (2) | Ring 1 (8) |
| Amulet 1 (12) | Ring 2 (9) |
| Amulet 2 (13) | Ring 3 (10) |
| Chest (3) | Ring 4 (11) |
| Belt (5) | Feet (7) |
| Hands (4) | Weapon (0) |
| Legs (6) | Shield (1) |

Center: ImDrawList silhouette (head/torso/arms/legs). Below columns: HP bar (red), EP bar (blue), Attack/Armor/Gold attributes, XP bar (green).

Empty equip slots render their icon from `assets/ui/gui/<SlotName>.bmp` via `UITextureCache::GUI()`.

### Item stacking
`db.AddStackableItem(ctx, charID, itemID, qty, maxStack=0, dur)` — pass `maxStack=0` to look up from `item_templates.max_stack`. Merges onto existing partial stacks first, then uses a free slot. Always use this instead of `FindFreeBackpackSlot + AddItemToSlot` when giving items to players.

---

## UI Texture System

`UITextureCache` (`client/src/ui/ui_texture.h`) — singleton `g_tex`. Loads PNG/BMP into OpenGL textures, cached by path.

```cpp
ImTextureID id = g_tex.GUI("Hat.bmp");      // loads assets/ui/gui/Hat.bmp
ImTextureID id = g_tex.Menu("logo.png");    // loads assets/ui/menu/logo.png
ImTextureID id = g_tex.Root("frame.png");   // loads assets/ui/frame.png
```

Equip slot icons live in `assets/ui/gui/`: `Weapon.bmp`, `Shield.bmp`, `Hat.bmp`, `Chest.bmp`, `Hand.bmp`, `Belt.bmp`, `Legs.bmp`, `Feet.bmp`, `Ring.bmp`, `Amulet.bmp`, `EmptySlot.bmp`.

---

## Known Gaps / Pending Work

### Inventory parity (RC backlog)
| Gap | Notes |
|---|---|
| Stack split (move X of Y) | RC: `"A" + SlotFrom + SlotTo + Amount` |
| Item thumbnail/icon in bag slots | Needs per-item texture IDs in `item_templates` |
| Race/class restriction per item | RC: ExclusiveRace/ExclusiveClass |
| Item attributes (40 values) | RC: Attributes.Value[0..39] bonuses on equip |
| P_InventoryUpdate "O" for others | Broadcast equip appearance change to nearby players |

### Combat parity
| Feature | RC Packet | Notes |
|---|---|---|
| Animation blend / transitions | PAnimateActor=30 | System done; blend trees and transition curves not yet implemented |
| AICallForHelp | — | NPCs near a fight join in |

---

## RC 1.26 Reference — What NOT to Replicate

| RC Limitation | RCO Solution |
|---|---|
| 250×250 vertex terrain cap (DirectX 8) | Chunked terrain with LOD |
| Texture stretching on slopes | Triplanar mapping in GLSL |
| No shader support | GLSL shaders for everything |
| Binary config files (.dat) | TOML |
| Hardcoded 1024×768 in editors | Resolution-independent ImGui UI |
| BriskVM proprietary scripting | gopher-lua (Lua 5.1) on server |
| ENet/RottNet UDP | QUIC (encrypted, multiplexed, modern) |
| GUE requires MySQL server running | GUE opens SQLite directly, no server needed |
| Hardcoded NPC lists in server code | DB-driven via `npc_spawns` + GUE Actors tab |
