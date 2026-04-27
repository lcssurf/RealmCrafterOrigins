# Animation System + Input Mapping — Implementation Spec

**Status:** Ready for execution
**Stack:** C++ / OpenGL (client) · Go (server) · Lua (gameplay) · GUE (editor + DB)
**Philosophy:** Simple enough for anyone to use, complete enough for any game.

---

## 0. TL;DR

**What it is:** a skeletal animation system + input mapping, fully data-driven through the GUE (visual editor), for an MMO engine in production.

**How it works in one paragraph:** In the GUE, the designer imports animation files (whole FBX or frame slices) as **clips**. Creates **Actor Defs** (warrior, vendor, dragon) and assigns arbitrary actions (`Idle`, `Walk`, `Attack`, `Wave`, `Hop`, any string) → clips. In the GUE, maps keys to actions (Space → "Jump"). At runtime, the server sends the actor's animation table to the client; the client receives player input, performs lookup → action → sends to server → server validates and propagates to clients. The client runs an `AnimController` that swaps clips with SLERP blend, fires animation events at specific frames (hitbox, footstep), and manages auto-locomotion (Idle/Walk/Run by velocity).

**Tables:** 5 — `media_anim_clips`, `media_actor_anims`, `media_anim_events`, `media_input_maps`, `media_input_presets`.
**New/modified messages:** 4 — `PNewActor` (extension), `PAnimateActor` (change), `PPlayerAction` (new), `PSetInputContext` (new).
**Client runtime components:** 2 — `AnimController` (per actor), `InputSystem` (singleton).

**What is NOT here:** upper/lower body layers, continuous blend tree, client prediction, hot reload, gamepad, IK, cinematics. See Section 13.

**How to read this document:** Sections 1-2 (principles and glossary) are the contract — read first. Sections 3-4 (schema and triggers) are the conceptual core. Sections 5-12 are specific details. **Section 15 is mandatory hardening** — fixes decisions that would normally be ambiguous. Section 16 gives implementation order.

---

## 1. Design Principles

Before any line of code, these principles guide every decision. If a future feature breaks any of these, the feature is wrong — not the principle.

**P1 — GUE is the single source of truth.**
Every `action → clip` mapping, every playback metadata field, every animation event, every input mapping lives in the database. Code has no hardcoded animation strings. Changing an animation or a keybind is an editor operation, not a deploy.

**P2 — Every Actor Def is autonomous.**
Each Actor Def declares its own actions and its own clips. No presets, no inheritance, no forced reuse. A vendor has `Idle, Wave, Talk`. A dragon has `Idle, Fly, Breath`. A slime has `Idle, Hop`. Each builds its own vocabulary.

**P3 — Actions are free strings, not enums.**
There is no fixed list of valid actions. If the designer wants an action `BackflipKick`, they create it. The engine only triggers by name. Exists? Plays. Doesn't exist? Silently ignored, gameplay continues.

**P4 — Server sends semantic events, never file names.**
The server says "this actor entered the Attack state". Never "play warrior_slash.fbx". Swapping the Attack clip in the GUE must not break any Lua script.

**P5 — Dumb client, smart server.**
The server resolves the actor's table and sends it to the client in `PNewActor`. The client never knows about the database's internal structure — it only receives `[action → clip + metadata]` ready to use.

**P6 — Two clip formats supported from day 1.**
Scenario A: whole file = 1 clip (`warrior_walk.fbx`).
Scenario B: file slice (`warrior.fbx`, frames 31-60 = Attack).
Modeling this later is painful refactoring. Modeling it now costs 2 extra columns.

**P7 — Engine does not validate skeleton compatibility.**
The user models, animates, and imports. The engine renders with the bones that exist in the clip and in the model. If the user assigns an incompatible animation, the result may look strange — but the engine does not crash or corrupt data. User's responsibility.

**P8 — Input is decoupled from animation.**
A key maps to an action (string). If the Actor Def has that action with a clip, the animation plays. If not, it just triggers gameplay logic. Allows UI/tool actions (`OpenInventory`, `Screenshot`) that don't need animation.

---

## 2. Glossary

The whole team uses these terms. Code review rejects variations.

| Term | Definition |
|---|---|
| **Clip** | Raw asset. A loadable skeletal animation segment. Can be a whole file (`warrior_walk.fbx`) or a file slice (`warrior.fbx` frames 31-60). |
| **Action** | Semantic string representing intent. E.g.: `Idle`, `Walk`, `Jump`, `Attack`, `Wave`. **Free vocabulary per Actor Def** — no fixed list. |
| **Actor Def** | Static definition of an actor type. Has mesh, materials, and its own list of actions with associated clips. |
| **AnimController** | Per-actor runtime instance, on the client. Manages current state, active blend, parameters, and event dispatch. |
| **Animation Event** | Marker at a specific frame of a clip. Triggers a gameplay callback (hitbox spawn, footstep SFX). |
| **Trigger** | Origin of an action call. Four types: direct input, auto-locomotion, Lua/AI, automatic events. |
| **Input Map** | Set of rules mapping key/button → action. Lives in the GUE, customizable in-game by the player. |
| **Context** | Active input mode. Only one at a time. E.g.: `gameplay`, `menu`, `vehicle`, `swimming`. |
| **Trigger Type** | How the key fires: `press`, `release`, `hold`, `double`, `axis`. |

---

## 3. Data Model

5 tables total: 3 for animation + 2 for input mapping.

### 3.1 Clips (animation assets)

```sql
CREATE TABLE media_anim_clips (
    id              INTEGER PRIMARY KEY,
    name            TEXT    NOT NULL UNIQUE,    -- "warrior_walk", "dragon_breath"
    source_path     TEXT    NOT NULL,           -- "media/anims/warrior.fbx"

    -- Supports both scenarios (P6):
    -- Scenario A (whole file): start_frame=0, end_frame=-1 (sentinel "until end")
    -- Scenario B (slice):       start_frame=31, end_frame=60
    start_frame     INTEGER NOT NULL DEFAULT 0,
    end_frame       INTEGER NOT NULL DEFAULT -1,
    fps             REAL    NOT NULL DEFAULT 30.0,

    created_at      INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL
);
```

**Why `end_frame = -1` instead of NULL:** simplifies binary serialization in the protocol. Field always present, known sentinel value.

### 3.2 Actor Animations (the actor's vocabulary)

```sql
CREATE TABLE media_actor_anims (
    id              INTEGER PRIMARY KEY,
    actor_def_id    INTEGER NOT NULL REFERENCES media_actor_defs(id) ON DELETE CASCADE,
    action          TEXT    NOT NULL,           -- "Idle", "Walk", "Jump", "Wave"...
    clip_id         INTEGER NOT NULL REFERENCES media_anim_clips(id),

    loop            INTEGER NOT NULL DEFAULT 0, -- 0/1
    speed           REAL    NOT NULL DEFAULT 1.0,
    blend_in        REAL    NOT NULL DEFAULT 0.15,  -- seconds of blend on entry
    return_to       TEXT    NOT NULL DEFAULT '',    -- '' = none; otherwise return action
    priority        INTEGER NOT NULL DEFAULT 0,     -- higher wins on conflict

    UNIQUE (actor_def_id, action)
);

CREATE INDEX idx_actor_anims ON media_actor_anims(actor_def_id);
```

**Vocabulary suggested by the GUE** (not enforced — just a UI shortcut). The GUE offers an "Add Standard Actions" button that creates these rows, but the designer can edit freely, create custom actions, or not use these at all:

| Action | Loop | Return_to | Priority | Typical use |
|---|---|---|---|---|
| `Idle` | true | — | 0 | Neutral state |
| `Walk` | true | — | 1 | Continuous movement |
| `Run` | true | — | 1 | Fast movement |
| `Jump` | false | `Idle` | 2 | One-shot |
| `Attack` | false | `Idle` | 2 | One-shot |
| `Cast` | false | `Idle` | 2 | One-shot |
| `Hit` | false | `Idle` | 3 | Interrupts Attack/Cast |
| `Death` | false | — | 99 | Does not return |

But a vendor can have only `Idle` + `Wave` + `Talk`. A slime only `Idle` + `Hop`. A dragon only `Idle` + `Fly` + `Roar` + `Breath`. **Each Actor Def is autonomous.**

### 3.3 Animation Events (frame markers)

```sql
CREATE TABLE media_anim_events (
    id          INTEGER PRIMARY KEY,
    clip_id     INTEGER NOT NULL REFERENCES media_anim_clips(id) ON DELETE CASCADE,
    frame       INTEGER NOT NULL,           -- relative to clip's start_frame
    event_type  TEXT    NOT NULL,           -- "hitbox", "footstep", "sfx", "vfx", custom
    payload     TEXT    NOT NULL DEFAULT '' -- free JSON interpreted by handler
);

CREATE INDEX idx_events_clip ON media_anim_events(clip_id);
```

**Important decision:** `event_type` is a free string. The client has registered handlers (`hitbox`, `footstep`, `sfx`, `vfx`) and silently ignores unknown types. Adding a new event type means registering a new handler in the client — zero schema changes.

**Payload example (hitbox):** `{"radius": 1.5, "damage_mult": 1.0, "shape": "cone"}`. The combat system parses it; the animation system just dispatches.

### 3.4 Input Maps (key → action mapping)

```sql
CREATE TABLE media_input_maps (
    id              INTEGER PRIMARY KEY,
    preset_id       INTEGER NOT NULL REFERENCES media_input_presets(id) ON DELETE CASCADE,
    context         TEXT    NOT NULL DEFAULT 'gameplay', -- "gameplay", "menu", "vehicle"...
    key             TEXT    NOT NULL,           -- "Space", "Mouse1", "F", "1", "W"
    modifier        TEXT    NOT NULL DEFAULT '',-- "" | "Shift" | "Ctrl" | "Alt" | "Shift+Ctrl"
    trigger_type    TEXT    NOT NULL DEFAULT 'press', -- "press"|"release"|"hold"|"double"|"axis"
    action          TEXT    NOT NULL,           -- "Jump", "Attack", "OpenInventory"...
    axis_value      REAL    NOT NULL DEFAULT 1.0, -- for trigger_type=axis: +1 or -1
    enabled         INTEGER NOT NULL DEFAULT 1, -- 0/1, to disable without deleting
    remappable      INTEGER NOT NULL DEFAULT 1, -- 0/1, can player remap in-game?

    -- Logical uniqueness: one (preset, context, key, modifier, trigger_type) → one action
    UNIQUE (preset_id, context, key, modifier, trigger_type)
);

CREATE INDEX idx_input_context ON media_input_maps(preset_id, context, enabled);
```

**Key name conventions** (free string, but with canonical list for the GUE):

- Letters: `A`–`Z`
- Numbers: `0`–`9`
- Function keys: `F1`–`F12`
- Special: `Space`, `Enter`, `Escape`, `Tab`, `Backspace`, `Delete`
- Arrows: `Up`, `Down`, `Left`, `Right`
- Mouse: `Mouse1` (left), `Mouse2` (right), `Mouse3` (middle), `MouseWheelUp`, `MouseWheelDown`, `Mouse4`, `Mouse5`

**Modifiers:** `""`, `"Shift"`, `"Ctrl"`, `"Alt"`, `"Shift+Ctrl"`, `"Shift+Alt"`, `"Ctrl+Alt"`, `"Shift+Ctrl+Alt"`. Always combined in alphabetical order to avoid duplicates.

**Trigger types in detail:**

| Type | Behavior | Example use |
|---|---|---|
| `press` | Fires once when the key is pressed | `Jump`, `Attack`, `Interact` |
| `release` | Fires when the key is released | `CancelCast`, `ReleaseArrow` |
| `hold` | Continuous state while held | `Block`, `AimBow`, `Sprint` |
| `double` | Two quick presses (< 300ms) | `Dash`, `Roll` |
| `axis` | Continuous movement | `MoveForward` (W=+1), `MoveBack` (S=-1) |

**For `hold`:** the client sends `PPlayerAction{action, state=start}` on press and `PPlayerAction{action, state=end}` on release. The server maintains state.

**For `axis`:** multiple keys can map to the same action with different `axis_value` (`W → MoveForward axis=+1`, `S → MoveForward axis=-1`). The client combines and sends the resulting value (-1, 0, +1) to the server.

### 3.5 Input Presets (saved schemes)

```sql
CREATE TABLE media_input_presets (
    id          INTEGER PRIMARY KEY,
    name        TEXT    NOT NULL UNIQUE,       -- "Default", "WASD_Classic", "MMO_Style"
    description TEXT    NOT NULL DEFAULT '',
    is_default  INTEGER NOT NULL DEFAULT 0,    -- 0/1, which preset the game starts with
    created_at  INTEGER NOT NULL
);
```

**How it works:**
- The dev defines `Default` in the GUE (and possibly `WASD_Classic`, `MMO_Style` as alternatives)
- Only one preset has `is_default = 1`
- Player can pick in-game which preset to use as base
- Player customizations live in a local file (`<game_dir>/users/<player_name>/input.json`), overriding the chosen preset

### 3.6 Server-side resolution (animations)

When the server builds `PNewActor` for an actor of def `D`:

```
1. table = []
2. For each row L in media_actor_anims WHERE actor_def_id = D.id:
     resolve clip → (source_path, start_frame, end_frame, fps)
     load clip's events
     append to table
3. Assign sequential action_id (uint8) in insertion order
4. Serialize and send to client
```

The client never executes this logic. It receives the table already resolved.

### 3.7 Client-side resolution (input)

The client loads input maps on boot or when the player switches preset:

```
1. Read chosen preset (default or player-customized)
2. Load media_input_maps WHERE preset_id = X AND enabled = 1
3. Apply overrides from local input.json (if exists)
4. Build in-memory table: (context, key, modifier, trigger_type) → action
5. On every key press, lookup in this table
```

When switching context (e.g., opening menu), the client just filters which subset of the table is active.

---

## 4. Action Triggers (where play_action calls come from)

Four origins. Each one lives in a specific layer.

### 4.1 Direct input (player's client)

Player presses key. The client looks up in the input map, finds the action, sends to the server. The server validates (has stamina? not stunned? on the ground for jumping?) and propagates.

```
Player's client: space key pressed
                 ↓
Player's client: lookup ("gameplay", "Space", "", "press") → "Jump"
                 ↓
Player's client: sends PPlayerAction{"Jump"} to server
                 ↓
Server: validates (player on ground? has stamina?)
                 ↓ (if valid)
Server: player.play_action("Jump")
                 ↓
Server: sends PAnimateActor{actor_id=42, action_id=N} to ALL clients in scope
                 ↓
All clients (including the player's own): AnimController.RequestState(N)
```

**Why not play locally immediately:** in multiplayer, what others see must match what you see. Playing locally before server confirmation can desync at high ping. (Client prediction is a possible future optimization, out of scope for this spec.)

**Actions without animation:** if the action is `OpenInventory`, the server might not even propagate — it just responds by opening the inventory for the player. Or it propagates and the player's Actor Def doesn't have that action, so `play_action` is a no-op in the animation system but the UI still opens.

### 4.2 Auto-locomotion (client, no network)

Idle/Walk/Run based on actor velocity. Each client detects on its own the velocity of visible actors and decides the animation. No network message. Works for all actors (own player, other players, NPCs). See Section 8.3 for full pseudocode with hysteresis and Section 15.3 for exact thresholds.

**Important:** auto-locomotion only acts if the actor has at least `Idle` declared. A slime doesn't have Walk/Run, only Hop — in this case, auto-locomotion doesn't interfere, and the server controls (via Lua) when the slime hops.

**Coexistence with high-priority actions:** if the server sent Attack (priority 2), auto-locomotion stops trying to switch (priority check in AnimController denies). When Attack ends and does return_to Idle, auto-locomotion takes over again.

### 4.3 Lua / AI (server)

NPC scripts, AI, quests, special events. All on the server.

```lua
-- Lua, server-side

function dragon_boss:on_aggro(target)
    self:play_action("Roar")
    self:set_target(target)
end

function dragon_boss:on_low_hp()
    self:play_action("Phase2_Transform")
end

function vendor:on_player_approach(player)
    self:face(player)
    self:play_action("Wave")
end

function archer:fire_arrow(target_pos)
    if self:has_action("Shoot") then
        self:play_action("Shoot")
    end
    spawn_arrow(self.position, target_pos)
end
```

**API:**
- `actor:play_action(name)` — plays the action. If it doesn't exist on the Actor Def, silently ignored.
- `actor:has_action(name)` — returns bool. Useful when gameplay needs to decide based on capability.
- `actor:current_action()` — returns the active action. Useful for AI to wait for it to finish before sending another.

### 4.4 Automatic events (gameplay engine)

The engine fires automatically when universal situations occur — **if the Actor Def has that action**:

| Situation | Action fired (if it exists) |
|---|---|
| Actor takes damage | `Hit` |
| Actor HP reaches 0 | `Death` |
| Actor falls from great height | `Land` |
| Actor enters water | `Swim` |

This is convention. If the Actor Def doesn't have `Hit`, nothing plays, and no error is generated. Vendors might not have `Death` — that's fine, they just don't play an animation on death (unlikely scenario, but the engine doesn't crash).

---

## 5. Input Contexts (input modes)

Only one context active at a time. Default is `gameplay`. Server or client-side script switches contexts as the situation requires.

**Common suggested contexts** (not enforced, dev creates whichever needed):

| Context | When active | Example mappings |
|---|---|---|
| `gameplay` | Default — player controlling character in the world | `Space=Jump`, `Mouse1=Attack`, `WASD=Move` |
| `menu` | Any fullscreen UI open (inventory, map, options) | `WASD=NavigateUI`, `Escape=Close`, `Enter=Confirm` |
| `vehicle` | Player in mount/vehicle | `Space=Brake`, `WASD=DriveControl` |
| `swimming` | Player in water | `Space=SwimUp`, `Ctrl=SwimDown` |
| `chat` | Text box open | (game input disabled, just text) |

**Context switch API:**

```lua
-- Lua, server-side
player:set_input_context("vehicle")   -- sends to client
player:set_input_context("gameplay")  -- back to normal
```

```cpp
// Client C++
input_system.SetContext("menu");      // local, on opening fullscreen UI
input_system.SetContext("gameplay");  // on closing
```

**Rule:** the server is authoritative for gameplay-dependent contexts (`vehicle`, `swimming`). Client-side UI controls its own contexts (`menu`, `chat`). Both can call `SetContext`.

---

## 6. Animation Events vs Triggers (don't confuse)

These are different things:

**Triggers (Section 4)** decide **which animation to play**.

**Animation Events** fire **during** an already-playing animation, at specific frames.

```
Trigger: player:play_action("Attack")
              ↓ (decides to play Attack)
Client plays warrior_slash.fbx
              ↓ (during the animation)
Frame 8:  "footstep" event → SFX fires
Frame 12: "hitbox" event   → damage spawn
Frame 18: "vfx" event      → sword spark
              ↓ (animation ends)
return_to "Idle" → AnimController switches to Idle
```

Animation Events are defined in the GUE per clip. Systems register handlers in the client:

```cpp
// client boot — once
combat_system.register_handler("hitbox", [](Actor& actor, const Event& e) {
    auto json = parse(e.payload);
    spawn_hitbox(actor, json["radius"], json["damage_mult"]);
});

audio_system.register_handler("footstep", [](Actor& actor, const Event& e) {
    play_sfx("footstep_" + ground_material(actor.position));
});

vfx_system.register_handler("vfx", [](Actor& actor, const Event& e) {
    auto json = parse(e.payload);
    spawn_vfx(actor, json["effect"]);
});
```

**Animator defines when, gameplay defines what happens.** Changing an attack's visual effect means editing the handler. Changing when the effect happens means editing the frame in the GUE. Independent.

**Cosmetic vs authoritative events:**

- **Cosmetic** (footstep SFX, sparkle VFX): client fires locally when the frame arrives. No network needed.
- **Authoritative** (hitbox that applies damage): server simulates the animation with the same timing and triggers the gameplay effect. Client receives the result (damage applied) via a separate message. Ensures ping doesn't desync damage from animation.

---

## 7. Network Protocol

### 7.1 PNewActor (extension to existing message)

When the client enters scope of an actor, it receives the full animation table. Logical layout below; actual wire format is in Section 15.1.

```cpp
struct AnimBinding {
    char     action[32];          // "Idle", "Walk", "Wave", "BackflipKick"
    char     source_path[256];    // "media/anims/warrior.fbx"
    int32_t  start_frame;         // 0 or offset
    int32_t  end_frame;           // -1 = until end
    float    fps;
    uint8_t  loop;
    float    speed;
    float    blend_in;
    char     return_to[32];       // "" if none
    uint8_t  priority;
    uint16_t event_count;
    AnimEvent events[];           // dynamic array — see 15.1 for serialization
};

struct AnimEvent {
    int32_t frame;
    char    event_type[32];
    char    payload[256];
};
```

**Typical size:** ~600 bytes per actor for 6 actions with 2 events each. Acceptable — `PNewActor` is rare (only when actor enters scope).

### 7.2 PAnimateActor (main state-change message)

```cpp
struct PAnimateActor {
    uint32_t actor_runtime_id;
    uint8_t  action_id;           // index in the table sent in PNewActor
};
```

**How the client resolves it:** when receiving `PNewActor`, it builds an `unordered_map<uint8_t, AnimBinding>` indexed by the order actions arrived. `PAnimateActor.action_id = 2` means "the third action that came in this actor's PNewActor".

**Why it's safe:** the server that sent `PNewActor` is the same one that sends `PAnimateActor`. It controls the order. If the actor is re-spawned, IDs are reassigned.

### 7.3 PPlayerAction (client → server, player intent)

```cpp
struct PPlayerAction {
    char    action[32];           // "Jump", "Attack", "Cast", "OpenInventory"
    uint8_t state;                // 0=instant/press, 1=hold_start, 2=hold_end
    float   axis_value;           // for axis: -1.0 to +1.0; otherwise 0
};
```

Client sends, server validates, and converts into propagated `PAnimateActor` (if the action generates animation).

### 7.4 PSetInputContext (server → client)

```cpp
struct PSetInputContext {
    char context[32];             // "gameplay", "vehicle", "swimming"
};
```

Direct message — the server only sends to the affected player's client, so no `target_player_id` needed. Client receives and calls `InputSystem::SetContext(context)` directly.

---

## 8. Client Runtime (C++)

### 8.1 Structures — Animation

```cpp
// src/renderer/anim_controller.h

namespace rco::anim {

struct Pose {
    std::vector<glm::mat4> bone_matrices;  // size = skeleton's bone_count
};

struct AnimEvent {
    int32_t     frame;
    std::string event_type;
    std::string payload;
};

struct Clip {
    std::string source_path;
    int32_t     start_frame;
    int32_t     end_frame;
    float       fps;
    bool        loop;
    std::vector<AnimEvent> events;
    std::shared_ptr<SkeletalAnim> data;  // FBX data (lazy loaded, cached globally)
};

struct AnimBinding {
    std::string action;
    Clip        clip;
    float       speed;
    float       blend_in;
    std::string return_to;
    uint8_t     priority;
    uint8_t     return_to_action_id;  // resolved in Bind() — 0xFF if ""
};

struct ActiveAnim {
    Clip*    clip;
    float    time_seconds;
    float    speed;
    bool     finished;
    int32_t  last_event_frame;
};

struct BlendState {
    Pose       from_pose;
    float      elapsed;
    float      duration;
    bool       active;
};

class AnimController {
public:
    // Called once per actor on PNewActor reception. After Bind, never modify
    // bindings_by_id_ — active_.clip points into it (see implementation note in 8.3).
    void Bind(const std::vector<AnimBinding>& bindings, const Skeleton& skel);

    bool RequestState(uint8_t action_id);
    bool RequestStateByName(const std::string& action);  // helper

    void Update(float dt, glm::vec3 actor_velocity);
    const Pose& GetCurrentPose() const { return current_pose_; }

    bool HasAction(const std::string& action) const;
    const std::string& CurrentAction() const;

    using EventHandler = std::function<void(const AnimEvent&)>;
    void OnEvent(const std::string& event_type, EventHandler h);

private:
    std::unordered_map<uint8_t, AnimBinding>    bindings_by_id_;
    std::unordered_map<std::string, uint8_t>    action_to_id_;
    uint8_t      current_action_id_ = 0;
    uint8_t      pending_return_id_ = 0xFF;
    ActiveAnim   active_;
    BlendState   blend_;
    Pose         current_pose_;
    std::unordered_map<std::string, std::vector<EventHandler>> handlers_;
};

}  // namespace rco::anim
```

### 8.2 Structures — Input

```cpp
// src/input/input_system.h

namespace rco::input {

enum class TriggerType : uint8_t {
    Press, Release, Hold, Double, Axis
};

struct InputBinding {
    std::string  context;        // "gameplay", "menu"...
    std::string  key;            // "Space", "Mouse1", "F"
    std::string  modifier;       // "" | "Shift" | "Ctrl+Alt"...
    TriggerType  trigger_type;
    std::string  action;         // "Jump", "Attack", "OpenInventory"
    float        axis_value;     // for axis: +1, -1
    bool         remappable;     // can the player change this in-game?
};

struct ChordKey {
    std::string key;
    std::string modifier;
    TriggerType trigger;
    bool operator==(const ChordKey&) const = default;
};
struct ChordKeyHash { size_t operator()(const ChordKey&) const noexcept; };

class InputSystem {
public:
    explicit InputSystem(NetworkClient* net) : network_(net) {}

    // Boot: load bindings from server + local overrides
    void LoadBindings(const std::vector<InputBinding>& bindings);
    void LoadLocalOverrides(const std::string& json_path);

    void SetContext(const std::string& context);
    const std::string& Context() const;

    // Called by the windowing system's event loop (GLFW/SDL)
    void OnKeyDown(const std::string& key, const std::string& modifier);
    void OnKeyUp(const std::string& key, const std::string& modifier);

    // Player remap in-game
    void Rebind(const ChordKey& chord, const std::string& new_action);
    void SaveLocalOverrides(const std::string& json_path);
    void ResetToDefault();

    // Callbacks gameplay/UI registers
    using ActionHandler = std::function<void(const std::string& action,
                                             uint8_t state,
                                             float axis_value)>;
    void OnAction(ActionHandler h);

private:
    void Dispatch(const std::string& action, uint8_t state, float axis_value);

    NetworkClient* network_;
    std::string current_context_ = "gameplay";
    // context → ((key, modifier, trigger) → binding)
    std::unordered_map<std::string,
        std::unordered_map<ChordKey, InputBinding, ChordKeyHash>> by_context_;
    std::unordered_set<std::string> hold_active_;  // actions in hold state
    std::unordered_map<std::string, double> last_press_time_;  // for double
    std::vector<ActionHandler> handlers_;
};

}  // namespace rco::input
```

### 8.3 Update loop (animation, pseudocode)

```cpp
void AnimController::Update(float dt, glm::vec3 vel) {
    // 1. Auto-locomotion with hysteresis (see 15.3 for thresholds)
    const std::string& current = bindings_by_id_[current_action_id_].action;
    if (current == "Idle" || current == "Walk" || current == "Run") {
        float speed = glm::length(vel);
        const char* target = current.c_str();
        if      (current == "Idle" && speed > 0.15f) target = "Walk";
        else if (current == "Walk" && speed < 0.10f) target = "Idle";
        else if (current == "Walk" && speed > 3.0f)  target = "Run";
        else if (current == "Run"  && speed < 2.7f)  target = "Walk";

        if (target != current && HasAction(target)) {
            RequestStateByName(target);
        }
    }

    // 2. Advance current animation
    active_.time_seconds += dt * active_.speed;
    int32_t current_frame = int32_t(active_.time_seconds * active_.clip->fps)
                          + active_.clip->start_frame;

    // 3. Event dispatch (between last_event_frame and current_frame)
    for (const auto& ev : active_.clip->events) {
        if (ev.frame > active_.last_event_frame && ev.frame <= current_frame) {
            DispatchEvent(ev);
        }
    }
    active_.last_event_frame = current_frame;

    // 4. Detect end of one-shot
    int32_t end = active_.clip->end_frame > 0
                ? active_.clip->end_frame
                : (active_.clip->start_frame + active_.clip->data->total_frames);
    if (current_frame >= end) {
        if (active_.clip->loop) {
            active_.time_seconds = 0;
            active_.last_event_frame = active_.clip->start_frame - 1;  // re-arm events
        } else if (pending_return_id_ != 0xFF) {
            uint8_t ret = pending_return_id_;
            pending_return_id_ = 0xFF;
            RequestState(ret);
        } else {
            active_.finished = true;
        }
    }

    // 5. Sample pose (with blend if active)
    Pose target_pose = SampleClip(active_);
    if (blend_.active) {
        blend_.elapsed += dt;
        float t = std::min(blend_.elapsed / blend_.duration, 1.0f);
        current_pose_ = LerpPoses(blend_.from_pose, target_pose, SmoothStep(t));
        if (t >= 1.0f) blend_.active = false;
    } else {
        current_pose_ = target_pose;
    }
}

bool AnimController::RequestState(uint8_t new_id) {
    if (new_id == current_action_id_) return true;  // already in this state, no-op (see 15.3)

    auto it = bindings_by_id_.find(new_id);
    if (it == bindings_by_id_.end()) return false;
    const auto& nb = it->second;
    const auto& cb = bindings_by_id_[current_action_id_];

    // Priority check
    if (nb.priority < cb.priority && !active_.finished) return false;

    // Snapshot current pose for the blend
    blend_.from_pose = current_pose_;
    blend_.duration = nb.blend_in;
    blend_.elapsed = 0;
    blend_.active = (nb.blend_in > 0);

    // Switch active clip (stable reference: points to Clip inside bindings_by_id_)
    active_.clip = &bindings_by_id_[new_id].clip;
    active_.time_seconds = 0;
    active_.speed = nb.speed;
    active_.finished = false;
    active_.last_event_frame = nb.clip.start_frame - 1;

    pending_return_id_ = nb.return_to_action_id;
    current_action_id_ = new_id;
    return true;
}
```

**Critical implementation notes:**
- `bindings_by_id_` must be stable after `Bind()` — never call `Bind` while `active_.clip` points inside it. The current implementation uses `std::unordered_map`, which keeps pointers stable on insert (but not on erase/rehash). After `Bind`, **never modify** the map — only read.
- The `RequestState` early-return (`new_id == current_action_id_`) is also described in Section 15.3 — applies to both call sources (server → client and auto-locomotion).

### 8.4 Update loop (input, pseudocode)

```cpp
void InputSystem::OnKeyDown(const std::string& key, const std::string& mod) {
    auto& ctx_map = by_context_[current_context_];

    // 1. Try press
    ChordKey ck_press{key, mod, TriggerType::Press};
    if (auto it = ctx_map.find(ck_press); it != ctx_map.end()) {
        Dispatch(it->second.action, /*state=*/0, /*axis=*/0);
    }

    // 2. Try hold (start)
    ChordKey ck_hold{key, mod, TriggerType::Hold};
    if (auto it = ctx_map.find(ck_hold); it != ctx_map.end()) {
        hold_active_.insert(it->second.action);
        Dispatch(it->second.action, /*state=hold_start*/1, 0);
    }

    // 3. Try double
    ChordKey ck_dbl{key, mod, TriggerType::Double};
    if (auto it = ctx_map.find(ck_dbl); it != ctx_map.end()) {
        double now = TimeNow();
        std::string lpkey = key + "+" + mod;
        if (last_press_time_.count(lpkey) && now - last_press_time_[lpkey] < 0.3) {
            Dispatch(it->second.action, 0, 0);
            last_press_time_.erase(lpkey);
        } else {
            last_press_time_[lpkey] = now;
        }
    }

    // 4. Axis
    ChordKey ck_axis{key, mod, TriggerType::Axis};
    if (auto it = ctx_map.find(ck_axis); it != ctx_map.end()) {
        Dispatch(it->second.action, 0, it->second.axis_value);
    }
}

void InputSystem::OnKeyUp(const std::string& key, const std::string& mod) {
    auto& ctx_map = by_context_[current_context_];

    // Release
    ChordKey ck_rel{key, mod, TriggerType::Release};
    if (auto it = ctx_map.find(ck_rel); it != ctx_map.end()) {
        Dispatch(it->second.action, 0, 0);
    }

    // Hold end
    ChordKey ck_hold{key, mod, TriggerType::Hold};
    if (auto it = ctx_map.find(ck_hold); it != ctx_map.end()) {
        hold_active_.erase(it->second.action);
        Dispatch(it->second.action, /*state=hold_end*/2, 0);
    }

    // Axis returns to zero
    ChordKey ck_axis{key, mod, TriggerType::Axis};
    if (auto it = ctx_map.find(ck_axis); it != ctx_map.end()) {
        Dispatch(it->second.action, 0, 0);
    }
}

void InputSystem::Dispatch(const std::string& action, uint8_t state, float axis) {
    // Send to server + call local handlers (UI, etc.)
    PPlayerAction msg;
    StrCopy(msg.action, action.c_str(), sizeof(msg.action));
    msg.state = state;
    msg.axis_value = axis;
    network_->Send(msg);

    for (auto& h : handlers_) h(action, state, axis);
}
```

### 8.5 Pose lerp (correct, non-trivial)

Linear interpolation of 4x4 matrices produces distortion (bones stretch). Decompose into TRS, interpolate with SLERP on rotation, recompose:

```cpp
Pose LerpPoses(const Pose& a, const Pose& b, float t) {
    Pose out;
    out.bone_matrices.resize(a.bone_matrices.size());
    for (size_t i = 0; i < a.bone_matrices.size(); ++i) {
        glm::vec3 ta, tb, sa, sb;
        glm::quat ra, rb;
        DecomposeTRS(a.bone_matrices[i], ta, ra, sa);
        DecomposeTRS(b.bone_matrices[i], tb, rb, sb);

        glm::vec3 t_lerp = glm::mix(ta, tb, t);
        glm::quat r_lerp = glm::slerp(ra, rb, t);   // SLERP, not LERP
        glm::vec3 s_lerp = glm::mix(sa, sb, t);

        out.bone_matrices[i] = ComposeTRS(t_lerp, r_lerp, s_lerp);
    }
    return out;
}
```

**Why this matters:** direct lerp of 4x4 matrices produces visible distortions. SLERP on quaternion eliminates the pop. This is the detail that separates "functional blend" from "AAA blend".

---

## 9. GUE UI

### 9.1 "Animations" tab (already exists — extensions)

- **Clips** subtab: list of clips. Drag-drop FBX creates a clip. Edit `start_frame`, `end_frame`, `fps`.
- **Events** subtab (inside the clip detail): timeline with markers. "+ Add Event" → frame, type, payload (JSON editor).
- In **Actor Defs → Animations tab:** table of actor's actions. "Add Standard Actions" button creates the 8 default rows. Designer can add custom (`Jump`, `Block`, `Wave`).

### 9.2 New "Input Maps" tab

Structure:
- **Left panel:** list of presets (`Default`, `WASD_Classic`...). Buttons "+ New Preset", "Set as Default", "Duplicate".
- **Right panel:** editable table of the selected preset's mappings.
  - Filter: Context dropdown (`gameplay`, `menu`, `vehicle`...) — shows only those of the selected context.
  - Columns: `Context`, `Key`, `Modifier`, `Trigger`, `Action`, `Axis Value`, `Enabled`, `Remappable`.
  - **"Key" column:** "Press a key..." button captures the pressed key (including mouse buttons).
  - **"Modifier" column:** dropdown (`""`, `"Shift"`, `"Ctrl"`, `"Alt"`, `"Shift+Ctrl"`, etc.).
  - **"Trigger" column:** dropdown (`press`, `release`, `hold`, `double`, `axis`).
  - **"Action" column:** dropdown suggesting all actions already existing in some Actor Def + universal actions hardcoded in the GUE (`OpenInventory`, `OpenMap`, `Screenshot`, `Interact`, `Chat`).
  - Validation: red warning if two rows share the same `(context, key, modifier, trigger)` — conflict.
- **"Test in viewport" button** — opens a window where you press keys and the GUE shows which action would fire (useful for debugging conflicts and modifiers).

---

## 10. GUE Designer Workflow

Complete flow for adding a new character to the game:

**1. Import the model:**
- Media tab → Models → drag `warrior.fbx`
- Create material(s), assign textures

**2. Import animations (pick one of two scenarios):**

*Scenario A — separate files:*
- Media tab → Animations → drag `warrior_idle.fbx` → creates clip `warrior_idle` (start=0, end=-1)
- Repeat for `warrior_walk.fbx`, `warrior_attack.fbx`, etc.

*Scenario B — single file with multiple anims:*
- Drag `warrior.fbx` → timeline preview
- "Add Slice" button → creates clip `warrior_idle` (start=0, end=30)
- Another slice → `warrior_walk` (start=31, end=60)
- Another → `warrior_attack` (start=61, end=90)

**3. Define Animation Events (optional, per clip):**
- Select clip `warrior_attack` → Events tab
- "+" button → frame 8, type=`footstep`, payload=`{}`
- "+" button → frame 12, type=`hitbox`, payload=`{"radius":1.5,"damage_mult":1.0}`
- "+" button → frame 18, type=`vfx`, payload=`{"effect":"sword_spark"}`

**4. Create Actor Def:**
- Actor Defs tab → New → name "Warrior"
- Assign mesh `warrior.fbx`
- Animations tab inside the Actor Def:
  - "Add Standard Actions" button → creates rows for Idle/Walk/Run/Attack/Hit/Death (empty)
  - Assign clip to each one via dropdown
  - Add custom action: `Jump` row → clip `warrior_jump`, return_to=`Idle`
  - Add custom action: `Block` row → clip `warrior_block`, loop=true

**5. Configure inputs:**
- Input Maps tab → `Default` preset
- Row: context=`gameplay`, key=`Space`, modifier=``, trigger=`press`, action=`Jump`
- Row: context=`gameplay`, key=`Mouse1`, modifier=``, trigger=`press`, action=`Attack`
- Row: context=`gameplay`, key=`Mouse2`, modifier=``, trigger=`hold`, action=`Block`
- Row: context=`gameplay`, key=`Q`, modifier=``, trigger=`double`, action=`Dash`
- Row: context=`gameplay`, key=`1`, modifier=`Shift`, trigger=`press`, action=`Emote_Wave`
- Save.

**6. Test.**

For a vendor (no input — AI-controlled):
- Mesh `vendor.fbx`
- Actor Def "Vendor" → only adds `Idle`, `Wave`, `Talk` (doesn't use Standard Actions)
- No input mappings — vendor is controlled by Lua

For a vehicle (own context):
- Create new context `mount` in Input Maps
- key=`Space` action=`Brake`, key=`W` action=`Accelerate` (in `mount` context)
- When player mounts: Lua calls `player:set_input_context("mount")`
- When dismounts: `player:set_input_context("gameplay")`

Each Actor Def is independent. Designer doesn't need to think about rig compatibility, presets, or inheritance.

---

## 11. Player In-Game Remap

### 11.1 In-game "Configure Controls" UI

Options screen with a table similar to the GUE's, but filtered and simplified:
- Shows only the `gameplay` context (and possibly `menu`, depending on what's allowed for player customization)
- For each listed action, "Click to rebind" button → captures the next key
- Buttons: "Reset to default" (back to chosen preset), "Save" (saves to local `input.json`)
- Only actions with `remappable=1` are shown

### 11.2 Local persistence

File: `<game_dir>/users/<player_name>/input.json`

```json
{
  "preset_base": "Default",
  "overrides": [
    { "context": "gameplay", "action": "Jump", "key": "F", "modifier": "", "trigger": "press" },
    { "context": "gameplay", "action": "Attack", "key": "Mouse1", "modifier": "Ctrl", "trigger": "press" }
  ]
}
```

**Load logic:**
1. Load base preset from DB (`media_input_maps WHERE preset_id = id_of("Default")`)
2. Apply `overrides` on top — for each override, find existing binding with same `(context, action, trigger)` and replace `key` + `modifier`
3. If override doesn't match any existing binding, **silently ignore it** (see Section 15.4 — security against players manually editing the file to create arbitrary actions)

### 11.3 Remap validation

- Conflict: if the key is already mapped to another action in the same context+trigger, the GUE/UI shows "Already used by `Block`. Replace?" — player chooses replace or cancel.
- Blocked keys: some keys must not be remappable (`Escape`, `~` if used as console). Hardcoded list in the client.

---

## 12. End-to-End Flow (example)

Player customized `Space` to `F` and presses `F` to jump:

```
[1] Player's client (boot):
    InputSystem.LoadBindings(from server)
    InputSystem.LoadLocalOverrides("<game_dir>/users/<player_name>/input.json")
    → in-memory table has ("gameplay", "F", "", press) → "Jump"

[2] Player presses F:
    InputSystem.OnKeyDown("F", "")
    → lookup finds "Jump"
    → Dispatch("Jump", state=0, axis=0)
    → Sends PPlayerAction{action:"Jump", state:0, axis:0} to server

[3] Server (Go):
    Receives PPlayerAction
    Validates: player on ground? Has stamina?
    Lookup actor_def → has action "Jump"? Yes, action_id = N
    player.play_action("Jump")
    Sends PAnimateActor{actor_id=42, action_id=N} to all clients in scope

[4] All clients (including the player's own):
    Receive PAnimateActor
    actors[42].anim_controller.RequestState(N)

[5] AnimController.RequestState(N):
    binding[N] = {clip: warrior_jump, blend_in: 0.10s, return_to: "Idle", priority: 2}
    Snapshot current pose → blend.from_pose
    active.clip = &warrior_jump
    pending_return_id = id_of("Idle")

[6] Update(dt) in loop, frame by frame:
    frame 0 → 3:   blend from Idle to warrior_jump (0.10s)
    frame 5:       "sfx" event fires → "jump_grunt.ogg"
    frame 24:      "footstep" event fires (landing)
    frame 30:      clip ends → RequestState(Idle) → blend back

[7] Renderer:
    GetCurrentPose() → upload bone_matrices as uniform
    glDrawElementsBaseVertex(...)
```

No `"warrior_jump"` string appears in gameplay code. The name only exists in the GUE and asset path. No `"F"` or `"Space"` string appears in code — input is fully data-driven via DB + local override.

---

## 13. What is NOT in this spec (deferred decisions)

Note these so they're not forgotten, but out of scope for v1:

- **Upper/lower body layers** — walking and attacking simultaneously. Structure prepared (priority, independent events) but not implemented.
- **Continuous locomotion blend tree** — Idle ↔ Walk ↔ Run with proportional transition to velocity instead of discrete with hysteresis.
- **Client prediction** — playing animation locally before server confirmation, for input responsiveness.
- **Hot reload** — explicitly out. Restart client/server to apply changes.
- **Animation compression** — when RAM gets tight, consider quat compression and keyframe reduction.
- **Clip streaming** — load on demand, unload when actor leaves scope for > 30s.
- **Foot IK on terrain** — procedural post-pose adjustment.
- **Procedural overlays** — breathing, eye tracking, head look-at.
- **Cinematics** — full-body clips with custom time curves for cutscenes.
- **Skeleton compatibility validation** — engine doesn't validate. User's responsibility (P7).
- **Gamepad support** — keyboard and mouse only in v1. Structure allows adding (`axis_value` already exists), but button/stick mapping deferred to v2.
- **Macros / input sequences** — combos like "↓→ + Mouse1 = Hadouken". Out of scope.
- **Multi-key chords** — only 1 main key + modifiers. We don't support `K+L` simultaneously.

---

## 14. What IS in v1 scope

To make deliverables clear:

**Animation:**
- Schema for the 3 tables (`media_anim_clips`, `media_actor_anims`, `media_anim_events`)
- GUE with clip import (whole file or slice), event editor, action editor per Actor Def
- Extended `PNewActor` protocol + `PAnimateActor` by action_id + `PPlayerAction`
- Server resolves table and propagates
- Client `AnimController` with:
  - Transition blend (SLERP on quaternion)
  - Priority check
  - Automatic return_to
  - Auto-locomotion with hysteresis
  - Animation Events dispatch
- Lua API: `actor:play_action(name)`, `actor:has_action(name)`, `actor:current_action()`
- Automatic events: `Hit` on damage, `Death` on dying (if action exists)

**Input:**
- Schema for the 2 tables (`media_input_maps`, `media_input_presets`)
- GUE with "Input Maps" tab — key capture, action dropdown, conflict validation
- Trigger types: `press`, `release`, `hold`, `double`, `axis`
- Modifiers: `Shift`, `Ctrl`, `Alt` and combinations
- Contexts: `gameplay` (default), switching via `set_input_context` (Lua server / client local)
- Client `InputSystem` with efficient lookup, server dispatch, local callbacks
- In-game "Configure Controls" UI with per-action remap and persistence in local `input.json`
- `PSetInputContext` message (server → client)

---

## 15. Implementation Details (hardening)

This section eliminates ambiguities so implementation is deterministic. Wherever two reasonable choices exist, this section pins down which one to follow.

### 15.1 Network message wire format

C++ `struct` with dynamic array is illegal. The `struct`s shown in Sections 7.x are **logical layout**, not actual C++ declaration. Implementation serializes/deserializes manually.

**Binary format (little-endian throughout the protocol):**

- Integers: fixed size as declared (`uint8_t`, `uint16_t`, `uint32_t`, `int32_t`)
- `float`: IEEE 754 32-bit
- Fixed-size strings (`char[32]`, `char[256]`): N raw bytes, padded with `\0` at end, null-terminated. Truncate silently if source > N-1 characters.
- Dynamic arrays: prefixed by `uint16_t` count, then N elements of declared type.

**`PNewActor` animation section** (added part, not the entire message):

```
uint16_t binding_count
[binding_count times]:
    char[32]  action
    char[256] source_path
    int32_t   start_frame
    int32_t   end_frame
    float     fps
    uint8_t   loop
    float     speed
    float     blend_in
    char[32]  return_to
    uint8_t   priority
    uint16_t  event_count
    [event_count times]:
        int32_t   frame
        char[32]  event_type
        char[256] payload
```

**`PAnimateActor`:**
```
uint32_t actor_runtime_id
uint8_t  action_id
```

**`PPlayerAction`:**
```
char[32] action
uint8_t  state         // 0=instant/press, 1=hold_start, 2=hold_end
float    axis_value
```

**`PSetInputContext`:**
```
char[32] context
```

**Implementation:** follow the pattern already used by other RCO messages (same encoding, same serialization helper). If there's inconsistency between this spec and the existing pattern, **the existing pattern wins** — adjust this spec, not the protocol.

### 15.2 `return_to` resolution during `Bind()`

```cpp
void AnimController::Bind(const std::vector<AnimBinding>& bindings, const Skeleton& skel) {
    bindings_by_id_.clear();
    action_to_id_.clear();

    // First pass: index all actions
    for (size_t i = 0; i < bindings.size(); ++i) {
        bindings_by_id_[uint8_t(i)] = bindings[i];
        action_to_id_[bindings[i].action] = uint8_t(i);
    }

    // Second pass: resolve return_to
    for (auto& [id, binding] : bindings_by_id_) {
        if (binding.return_to.empty()) {
            binding.return_to_action_id = 0xFF;
        } else {
            auto it = action_to_id_.find(binding.return_to);
            if (it != action_to_id_.end()) {
                binding.return_to_action_id = it->second;
            } else {
                // return_to points to an action that doesn't exist on this actor.
                // Log warning and treat as "no return".
                LogWarning("AnimController: return_to '" + binding.return_to +
                           "' from action '" + binding.action +
                           "' not found in actor's action list — ignoring");
                binding.return_to_action_id = 0xFF;
            }
        }
    }
}
```

**Rule:** invalid `return_to` never crashes. It becomes `0xFF` (no return), animation ends and stays on last frame until another state is requested.

### 15.3 Auto-locomotion vs server policy

Auto-locomotion runs in the client's `Update()` (see pseudocode 8.3). But the server can send `PAnimateActor` with any action. Deterministic rules to resolve conflicts:

1. **Auto-locomotion only acts if the current action is exactly one of:** `"Idle"`, `"Walk"`, `"Run"`. Any other (`"Attack"`, `"Cast"`, `"Sneaking"`, `"Hop"`, custom) — auto-locomotion **does not** try to switch.

2. **`RequestState(new_id)` is no-op if `new_id == current_action_id_`** (early return at the start). Ensures that the server sending `Walk` while already in `Walk` does not re-trigger blend or reset `time_seconds`.

3. **If the server sends a custom action (`"Sneaking"`):** auto-locomotion is automatically disabled (rule 1). When that action is one-shot and does return_to to `Idle`, auto-locomotion takes over again. If it's a loop without return_to, it stays in `Sneaking` indefinitely until another command.

4. **Exact hysteresis** — values hardcoded in `Update()`:
   - Leave `Idle` for `Walk`: `speed > 0.15f`
   - Leave `Walk` for `Idle`: `speed < 0.10f`
   - Leave `Walk` for `Run`: `speed > 3.0f`
   - Leave `Run` for `Walk`: `speed < 2.7f`

   The asymmetric pairs (0.15/0.10 and 3.0/2.7) are intentional — they eliminate flickering when velocity oscillates around the threshold.

### 15.4 Player remap restrictions

Player remap **CANNOT** create mappings for arbitrary actions. It can only move actions already present in the base preset.

**Validation when loading `<game_dir>/users/<player_name>/input.json`:**

```cpp
void InputSystem::LoadLocalOverrides(const std::string& path) {
    auto json = ReadJson(path);
    for (auto& override : json["overrides"]) {
        // Find existing binding with same (context, action, trigger)
        auto* existing = FindBinding(override.context, override.action, override.trigger);
        if (!existing) {
            LogWarning("Skipping override for unknown action '" + override.action + "'");
            continue;
        }
        // Replace only key and modifier
        existing->key = override.key;
        existing->modifier = override.modifier;
    }
    RebuildLookupTables();
}
```

**Result:** if a player edits `input.json` by hand to map `"AdminKickPlayer"`, the client silently ignores it because that action doesn't exist in the project's base preset.

**Blocked actions for remap:** the `remappable` column (default 1) in `media_input_maps`. The in-game UI only lists actions with `remappable=1`.

### 15.5 InputSystem threading

**Policy:** `InputSystem` is single-threaded. Runs entirely on the main thread.

**Guarantees:**
- `OnKeyDown/Up` are called by the windowing system (GLFW/SDL) on the main thread, before the AnimController's `Update()`.
- Handlers registered via `OnAction` run synchronously during `Dispatch`, on the main thread.
- `Send` to the network uses the existing network code's internal lock-free queue — not `InputSystem`'s responsibility.

**Don't use mutex in InputSystem.** If a race appears, it's an integration bug — not architecture.

### 15.6 External dependencies and types

**`Skeleton`:** already exists in the RCO renderer at `src/renderer/skeleton.h`. Expected minimal structure:
```cpp
struct Skeleton {
    std::vector<std::string> bone_names;
    std::vector<int>         bone_parents;
    std::vector<glm::mat4>   bind_pose_inverse;
    int bone_count() const { return bone_names.size(); }
};
```
If it doesn't exist yet, create it with this interface.

**`SkeletalAnim`:** loaded representation of a clip. Expected interface:
```cpp
struct SkeletalAnim {
    int total_frames;
    float fps;
    // bone_name → vector of keyframes (one matrix per frame)
    std::unordered_map<std::string, std::vector<glm::mat4>> tracks;
};
```

**FBX loading:** use **Assimp** (`assimp/scene.h`). It's the library already in use in the project. Global cache of `SkeletalAnim` by `source_path` — never reload the same file twice. The cache owns the data; `Clip::data` is `shared_ptr` pointing to the cache entry.

**Math:** GLM (already a project dependency). Use `glm::mat4`, `glm::vec3`, `glm::quat`.

**Time:**
```cpp
double TimeNow() {
    using clk = std::chrono::steady_clock;
    static auto start = clk::now();
    return std::chrono::duration<double>(clk::now() - start).count();
}
```
Single source of truth on the client — define once in `src/core/time.h`.

**JSON (for event payloads and `input.json`):** `nlohmann::json` (header-only, should already be available or trivial to add).

### 15.7 Math functions

**`SmoothStep`:**
```cpp
inline float SmoothStep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
```

**`DecomposeTRS` / `ComposeTRS`:**
```cpp
inline void DecomposeTRS(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    t = glm::vec3(m[3]);
    s.x = glm::length(glm::vec3(m[0]));
    s.y = glm::length(glm::vec3(m[1]));
    s.z = glm::length(glm::vec3(m[2]));
    glm::mat3 rot_mat;
    rot_mat[0] = glm::vec3(m[0]) / s.x;
    rot_mat[1] = glm::vec3(m[1]) / s.y;
    rot_mat[2] = glm::vec3(m[2]) / s.z;
    r = glm::quat_cast(rot_mat);
}

inline glm::mat4 ComposeTRS(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    glm::mat4 m = glm::mat4_cast(r);
    m[0] *= s.x;
    m[1] *= s.y;
    m[2] *= s.z;
    m[3] = glm::vec4(t, 1.0f);
    return m;
}
```

**Don't recreate alternative versions** — blend code depends on these exact ones for consistent results.

### 15.8 Automatic event detection

The spec mentions `Hit`, `Death`, `Land`, `Swim` fired automatically. Who detects each:

| Event | Who detects | Where |
|---|---|---|
| `Hit` | Combat system on damage | Server (Go), calls `actor:play_action("Hit")` in combat logic |
| `Death` | Combat system when HP hits 0 | Server (Go), calls `actor:play_action("Death")` |
| `Land` | Physics system on landing detection | Client (C++), purely cosmetic — calls locally without sending to server |
| `Swim` | Gameplay system on water entry | Server (Go), context switch + calls `actor:play_action("Swim")` |

`Land` is the only exception that fires local-only (no network). Justification: it's cosmetic, has no gameplay effect, and the server already validates gravity/height through other means.

If the action doesn't exist on the Actor Def, all these calls are silent no-ops.

### 15.9 Performance and limits

**Hard limits (validate in GUE, hard-fail):**
- Maximum 255 actions per Actor Def (`action_id` is `uint8_t` — 256 IDs, 0xFF reserved for "none")
- Maximum 65535 events per clip (`event_count` is `uint16_t`)
- `action`, `return_to`, `event_type`, `context`: ≤ 31 characters + null
- `source_path`, `payload`: ≤ 255 characters + null

**Performance targets (not enforced, but design goals):**
- 100 simultaneously visible actors: total AnimController < 3ms/frame
- Input lookup: O(1) via hash — < 0.1ms per key event
- `PNewActor` with 8 actions / 3 events each: ~3KB per message, acceptable

### 15.10 GUE Rebind UI details

**Key capture** ("Press a key..."):
- Button enters "listening" mode (visual: blinking border, label changes to "Press any key... (Esc to cancel)")
- Next key event **excluding Escape** captures. If Escape, cancels.
- Modifiers active at capture time are also recorded (pressing Shift+F captures `key=F, modifier=Shift`).
- Mouse buttons capture normally. Mouse wheel too (`MouseWheelUp`/`MouseWheelDown`).

**Visual conflict detection:**
- After editing a row, GUE re-checks uniqueness `(context, key, modifier, trigger_type)` in current preset.
- Conflicting rows get red border. Tooltip: "Conflicts with row #N (action: X)".
- Saving with conflicts is allowed — just a warning. At runtime, definition order wins (the first row from DB is the winner).

### 15.11 Consistency rules

- **Clip `name` is case-sensitive**: `warrior_walk` ≠ `Warrior_Walk`. Recommend snake_case by convention.
- **`action` is case-sensitive**: `Idle` ≠ `idle`. Recommend PascalCase by convention.
- **`context` is case-sensitive**: `gameplay` ≠ `Gameplay`. Recommend lowercase by convention.
- **`event_type` is case-sensitive**: `hitbox` ≠ `Hitbox`. Recommend lowercase by convention.

GUE doesn't auto-normalize — if the designer types `Idle` in one place and `idle` in another, it's a bug. Recommended that GUE shows a warning on detecting case variations of the same base string.

---

## 16. Suggested implementation order

Not mandatory, but reduces rework. Grouped by deliverable milestones — each milestone produces something testable in isolation.

### Milestone 1 — Basic animation working (no network, no GUE)

1. **SQL Schema** — create the 5 tables and migrations (including input ones, to avoid a second migration later).
2. **FBX Loader** — `SkeletalAnim` via Assimp, with global cache by `source_path`.
3. **Minimal `AnimController`** — `Bind`, `RequestState`, `Update` with pose sampling. No blend yet. Test with hardcoded bindings in code + a single actor in the scene.
4. **SLERP Blend** — add `LerpPoses`, `BlendState`, `SmoothStep`. Visually verify smooth transitions.
5. **Auto-locomotion with hysteresis** — hardcoded `Idle/Walk/Run` bindings, move actor with direct input and verify correct switching.

**Milestone 1 acceptance criteria:** an actor rendered on screen with hardcoded C++ bindings that switches between Idle/Walk/Run by velocity, with visually smooth blending.

### Milestone 2 — Animation Events and API

6. **Animation Events** — handler registration (`OnEvent`), dispatch in `Update` between `last_event_frame` and `current_frame`. Test with a handler that logs to console.
7. **Server: table loading** — in Go, `LoadActorAnims(actor_def_id)` returns the merged binding list.
8. **Network protocol** — `PNewActor` extended with bindings + events; `PAnimateActor` with `action_id` (uint8). Wire format per 15.1.
9. **Lua API** — `actor:play_action`, `actor:has_action`, `actor:current_action`. Test with script that fires animations.
10. **Automatic events** — `Hit`, `Death` on server; `Land` on client. Just no-op calls if action doesn't exist.

**Milestone 2 acceptance criteria:** an NPC controlled by Lua executes Idle/Walk/Attack/Hit/Death correctly, with hitbox spawning at the right frame via Animation Event.

### Milestone 3 — GUE for animations

11. **GUE — Clips subtab** — FBX drag-drop, edit start/end/fps.
12. **GUE — Events editor** — timeline with markers per clip.
13. **GUE — Animations tab in Actor Def** — actions table with clip dropdown, "Add Standard Actions".

**Milestone 3 acceptance criteria:** designer creates a new Actor Def in the GUE, saves, restarts the server, and the NPC appears with correct animations in the game.

### Milestone 4 — Basic input system

14. **`InputSystem`** with hardcoded bindings in code — key capture via GLFW/SDL, dispatch to local handlers. No network yet.
15. **`PPlayerAction`** + server processing inputs — basic validation (has stamina?), conversion into `play_action`.
16. **All trigger types** — `press`, `release`, `hold` (with start/end state), `double` (with 300ms window), `axis` (with axis_value).

**Milestone 4 acceptance criteria:** player presses Space, server-side validates, all clients see the Jump animation.

### Milestone 5 — GUE for input + customization

17. **GUE — Input Maps tab** — key capture, suggested actions dropdown, visual conflict validation.
18. **`PSetInputContext`** + Lua API `player:set_input_context(name)`.
19. **Loading bindings from DB** at client boot, replacing the hardcoded ones.
20. **In-game "Configure Controls" UI** + local persistence in `<game_dir>/users/<player_name>/input.json` with override logic from Section 15.4.

**Milestone 5 acceptance criteria:** designer edits keybinds in the GUE, player customizes in-game, restart preserves configuration.

---

**End of document. Ready for implementation when the team decides to start.**
