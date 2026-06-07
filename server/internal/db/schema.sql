-- RCO PostgreSQL 16 schema
-- Each table is annotated with the RC 1.26 system it replaces.
-- Run this file once against a fresh database:
--   psql -U postgres -d rco -f schema.sql

-- ---------------------------------------------------------------------------
-- accounts
-- Replaces: RC AccountsServer — the Account type stored in Accounts.rcd
-- Stores login credentials, GM flag, and ban state.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS accounts (
    id            UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    username      VARCHAR(32)  UNIQUE NOT NULL,
    -- bcrypt hash, always 60 chars
    password_hash VARCHAR(60)  NOT NULL,
    email         VARCHAR(256) NOT NULL,
    is_gm         BOOLEAN      NOT NULL DEFAULT FALSE,
    is_banned     BOOLEAN      NOT NULL DEFAULT FALSE,
    -- NULL when not banned
    ban_reason    TEXT,
    created_at    TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    -- NULL until first login
    last_login    TIMESTAMPTZ
);

-- ---------------------------------------------------------------------------
-- characters
-- Replaces: RC ActorInstance — persistent player-character data saved in
--           server actor files and the AccountsServer character slots.
-- Each account may hold up to 9 character slots (0-8), matching RC's limit.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS characters (
    id           UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    account_id   UUID        NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,

    -- Character slot index within the account (0-8, matching RC's 9-slot limit)
    slot         SMALLINT    NOT NULL CHECK (slot BETWEEN 0 AND 8),

    -- Display / in-world name; must be globally unique
    name         VARCHAR(32) NOT NULL,

    -- RC ActorDef fields
    race         VARCHAR(32) NOT NULL DEFAULT 'Human',
    class        VARCHAR(32) NOT NULL DEFAULT 'Warrior',
    -- 0 = male, 1 = female (matches RC ActorInstance.Gender)
    gender       SMALLINT    NOT NULL DEFAULT 0,

    -- Progression
    level        SMALLINT    NOT NULL DEFAULT 1,
    xp           BIGINT      NOT NULL DEFAULT 0,
    gold         BIGINT      NOT NULL DEFAULT 100,

    -- World position (replaces RC's Area + X/Y/Z actor fields)
    area_name    VARCHAR(64) NOT NULL DEFAULT 'Starter Zone',
    x            REAL        NOT NULL DEFAULT 0,
    y            REAL        NOT NULL DEFAULT 0,
    z            REAL        NOT NULL DEFAULT 0,
    yaw          REAL        NOT NULL DEFAULT 0,

    -- Appearance (replaces RC ActorInstance appearance indices)
    face_tex     SMALLINT    NOT NULL DEFAULT 0,
    hair         SMALLINT    NOT NULL DEFAULT 0,
    beard        SMALLINT    NOT NULL DEFAULT 0,
    body_tex     SMALLINT    NOT NULL DEFAULT 0,

    -- Vital stats (replaces RC ActorInstance HP / Energy)
    health       INTEGER     NOT NULL DEFAULT 100,
    health_max   INTEGER     NOT NULL DEFAULT 100,
    energy       INTEGER     NOT NULL DEFAULT 100,
    energy_max   INTEGER     NOT NULL DEFAULT 100,

    created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    last_seen    TIMESTAMPTZ NOT NULL DEFAULT NOW(),

    -- One character per slot per account
    UNIQUE (account_id, slot),
    -- Names are globally unique across all accounts
    UNIQUE (name)
);

-- ---------------------------------------------------------------------------
-- areas
-- Replaces: RC Area / ServerArea — area definitions stored in AreaData.rcd
--           and loaded by the server on startup.
-- Terrain geometry will reference area rows in a future phase.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS areas (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    name        VARCHAR(64) UNIQUE NOT NULL,
    description TEXT,

    -- Default spawn coordinates for players entering this area
    spawn_x     REAL        NOT NULL DEFAULT 0,
    spawn_y     REAL        NOT NULL DEFAULT 0,
    spawn_z     REAL        NOT NULL DEFAULT 0,

    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- ---------------------------------------------------------------------------
-- chat_log
-- Replaces: RC file-based chat logs written by GameServer.
-- Optional — disable inserts if persistence is not required.
-- channel values mirror RC chat channel IDs:
--   0 = normal, 1 = party, 2 = guild, 3 = GM, 4 = system
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS chat_log (
    id           BIGSERIAL   PRIMARY KEY,
    channel      SMALLINT    NOT NULL,
    -- NULL for system messages
    sender_name  VARCHAR(32),
    -- NULL for global/non-area messages
    area_name    VARCHAR(64),
    message      TEXT        NOT NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- ---------------------------------------------------------------------------
-- Indexes
-- ---------------------------------------------------------------------------

CREATE INDEX IF NOT EXISTS idx_accounts_username
    ON accounts (username);

CREATE INDEX IF NOT EXISTS idx_characters_account_id
    ON characters (account_id);

CREATE INDEX IF NOT EXISTS idx_characters_name
    ON characters (name);

CREATE INDEX IF NOT EXISTS idx_chat_log_created_at
    ON chat_log (created_at);

-- ---------------------------------------------------------------------------
-- Seed data
-- ---------------------------------------------------------------------------

-- Default starting area — referenced by characters.area_name DEFAULT value.
INSERT INTO areas (name, description, spawn_x, spawn_y, spawn_z)
VALUES ('Starter Zone', 'The beginning area for new adventurers.', 0, 0, 0)
ON CONFLICT (name) DO NOTHING;

-- ---------------------------------------------------------------------------
-- quests (Phase 3 / Etapa B)
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS quest_defs (
    id                    SERIAL       PRIMARY KEY,
    code                  VARCHAR(64)  NOT NULL UNIQUE,
    title                 VARCHAR(128) NOT NULL DEFAULT '',
    description           TEXT         NOT NULL DEFAULT '',
    min_level             INTEGER      NOT NULL DEFAULT 1,
    repeatable            BOOLEAN      NOT NULL DEFAULT FALSE,
    auto_accept           BOOLEAN      NOT NULL DEFAULT FALSE,
    prerequisite_quest_id INTEGER      NOT NULL DEFAULT 0,
    is_active             BOOLEAN      NOT NULL DEFAULT TRUE
);

CREATE TABLE IF NOT EXISTS quest_objective_defs (
    id               SERIAL       PRIMARY KEY,
    quest_id         INTEGER      NOT NULL DEFAULT 0,
    objective_order  INTEGER      NOT NULL DEFAULT 0,
    objective_type   SMALLINT     NOT NULL DEFAULT 1,
    description      TEXT         NOT NULL DEFAULT '',
    target_npc_name  VARCHAR(128) NOT NULL DEFAULT '',
    target_item_id   INTEGER      NOT NULL DEFAULT 0,
    target_area_name VARCHAR(128) NOT NULL DEFAULT '',
    target_count     INTEGER      NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS quest_reward_defs (
    id          SERIAL PRIMARY KEY,
    quest_id    INTEGER NOT NULL DEFAULT 0,
    xp_reward   INTEGER NOT NULL DEFAULT 0,
    gold_reward INTEGER NOT NULL DEFAULT 0,
    item_id     INTEGER NOT NULL DEFAULT 0,
    item_qty    INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS character_quests (
    id           SERIAL       PRIMARY KEY,
    character_id TEXT         NOT NULL,
    quest_id     INTEGER      NOT NULL DEFAULT 0,
    state        VARCHAR(32)  NOT NULL DEFAULT 'active',
    accepted_at  TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    completed_at TIMESTAMPTZ,
    turned_in_at TIMESTAMPTZ,
    updated_at   TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    UNIQUE (character_id, quest_id)
);

CREATE TABLE IF NOT EXISTS character_quest_progress (
    character_id  TEXT        NOT NULL,
    quest_id      INTEGER     NOT NULL DEFAULT 0,
    objective_id  INTEGER     NOT NULL DEFAULT 0,
    current_count INTEGER     NOT NULL DEFAULT 0,
    target_count  INTEGER     NOT NULL DEFAULT 1,
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (character_id, quest_id, objective_id)
);

CREATE INDEX IF NOT EXISTS idx_character_quests_character
    ON character_quests (character_id);

CREATE INDEX IF NOT EXISTS idx_character_quests_state
    ON character_quests (state);

CREATE INDEX IF NOT EXISTS idx_character_quest_progress_char
    ON character_quest_progress (character_id);

-- ---------------------------------------------------------------------------
-- combat ability runtime (AAA data-driven foundation)
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS ability_templates (
    id                        SERIAL       PRIMARY KEY,
    name                      VARCHAR(96)  NOT NULL UNIQUE,
    family                    VARCHAR(32)  NOT NULL DEFAULT 'melee_special',
    resource_type             VARCHAR(16)  NOT NULL DEFAULT 'none',
    resource_cost             INTEGER      NOT NULL DEFAULT 0,
    cooldown_ms               INTEGER      NOT NULL DEFAULT 2000,
    range_min                 REAL         NOT NULL DEFAULT 0,
    range_max                 REAL         NOT NULL DEFAULT 2.5,
    windup_ms                 INTEGER      NOT NULL DEFAULT 700,
    impact_delay_ms           INTEGER      NOT NULL DEFAULT 0,
    recover_ms                INTEGER      NOT NULL DEFAULT 400,
    parry_window_ms           INTEGER      NOT NULL DEFAULT 200,
    interruptible             BOOLEAN      NOT NULL DEFAULT TRUE,
    base_damage_min           INTEGER      NOT NULL DEFAULT 0,
    base_damage_max           INTEGER      NOT NULL DEFAULT 0,
    damage_stat_scale_json    TEXT         NOT NULL DEFAULT '',
    armor_pierce_pct          REAL         NOT NULL DEFAULT 0,
    crit_policy_json          TEXT         NOT NULL DEFAULT '',
    telegraph_type            VARCHAR(32)  NOT NULL DEFAULT 'ring_close',
    telegraph_radius          REAL         NOT NULL DEFAULT 2.5,
    telegraph_color_rgba      VARCHAR(32)  NOT NULL DEFAULT '1,0.2,0.2,0.75',
    action_windup             VARCHAR(64)  NOT NULL DEFAULT 'Attack',
    action_impact             VARCHAR(64)  NOT NULL DEFAULT 'Attack',
    action_recover            VARCHAR(64)  NOT NULL DEFAULT 'Idle',
    allow_action_override     BOOLEAN      NOT NULL DEFAULT FALSE,
    allowed_action_tags_json  TEXT         NOT NULL DEFAULT '',
    vfx_id_windup             INTEGER      NOT NULL DEFAULT 0,
    vfx_id_impact             INTEGER      NOT NULL DEFAULT 0,
    sfx_id_windup             INTEGER      NOT NULL DEFAULT 0,
    sfx_id_impact             INTEGER      NOT NULL DEFAULT 0,
    vfx_path_windup           TEXT         NOT NULL DEFAULT '',
    vfx_path_impact           TEXT         NOT NULL DEFAULT '',
    sfx_path_windup           TEXT         NOT NULL DEFAULT '',
    sfx_path_impact           TEXT         NOT NULL DEFAULT '',
    enabled                   BOOLEAN      NOT NULL DEFAULT TRUE
);

CREATE TABLE IF NOT EXISTS fx_templates (
    id                  SERIAL       PRIMARY KEY,
    fx_key              TEXT         NOT NULL UNIQUE,
    display_name        TEXT         NOT NULL DEFAULT '',

    -- Emission
    burst_count         INTEGER      NOT NULL DEFAULT 0,        -- 0=stream, >0=burst all at once
    stream_interval     REAL         NOT NULL DEFAULT 0.04,     -- seconds between stream particles (used if burst_count=0)
    lifetime_seconds    REAL         NOT NULL DEFAULT 1.0,      -- particle lifetime

    -- Velocity
    speed_min           REAL         NOT NULL DEFAULT 1.0,
    speed_max           REAL         NOT NULL DEFAULT 3.0,
    velocity_bias_x     REAL         NOT NULL DEFAULT 0.0,
    velocity_bias_y     REAL         NOT NULL DEFAULT 2.0,
    velocity_bias_z     REAL         NOT NULL DEFAULT 0.0,
    velocity_spread     REAL         NOT NULL DEFAULT 0.5,      -- random spread radius

    -- Visual
    color_start_r       REAL         NOT NULL DEFAULT 1.0,
    color_start_g       REAL         NOT NULL DEFAULT 0.5,
    color_start_b       REAL         NOT NULL DEFAULT 0.0,
    color_start_a       REAL         NOT NULL DEFAULT 1.0,
    color_end_r         REAL         NOT NULL DEFAULT 1.0,
    color_end_g         REAL         NOT NULL DEFAULT 0.0,
    color_end_b         REAL         NOT NULL DEFAULT 0.0,
    color_end_a         REAL         NOT NULL DEFAULT 0.0,
    size_start          REAL         NOT NULL DEFAULT 8.0,      -- pixels or world units (matches kCfg[] values)
    size_end            REAL         NOT NULL DEFAULT 2.0,
    texture_path        TEXT         NOT NULL DEFAULT '',       -- empty = solid colored quad

    enabled             INTEGER      NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS npc_ability_loadouts (
    id                 SERIAL       PRIMARY KEY,
    npc_spawn_id       INTEGER      NOT NULL DEFAULT 0,
    actor_def_id       INTEGER      NOT NULL DEFAULT 0,
    ability_id         INTEGER      NOT NULL DEFAULT 0,
    priority           INTEGER      NOT NULL DEFAULT 100,
    weight             INTEGER      NOT NULL DEFAULT 100,
    min_distance       REAL         NOT NULL DEFAULT 0,
    max_distance       REAL         NOT NULL DEFAULT 0,
    min_target_hp_pct  REAL         NOT NULL DEFAULT 0,
    max_target_hp_pct  REAL         NOT NULL DEFAULT 100,
    phase_tag          VARCHAR(32)  NOT NULL DEFAULT '',
    condition_lua      TEXT         NOT NULL DEFAULT '',
    enabled            BOOLEAN      NOT NULL DEFAULT TRUE
);

CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_spawn
    ON npc_ability_loadouts (npc_spawn_id);

CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_actor
    ON npc_ability_loadouts (actor_def_id);

CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_ability
    ON npc_ability_loadouts (ability_id);

CREATE TABLE IF NOT EXISTS npc_combat_profiles (
    id                        SERIAL       PRIMARY KEY,
    name                      VARCHAR(64)  NOT NULL UNIQUE,
    global_gcd_ms             INTEGER      NOT NULL DEFAULT 450,
    decision_tick_ms          INTEGER      NOT NULL DEFAULT 250,
    aggro_style               VARCHAR(32)  NOT NULL DEFAULT 'default',
    allow_chain_cast          BOOLEAN      NOT NULL DEFAULT FALSE,
    max_consecutive_specials  INTEGER      NOT NULL DEFAULT 1,
    enabled                   BOOLEAN      NOT NULL DEFAULT TRUE
);

CREATE TABLE IF NOT EXISTS npc_profile_bindings (
    id           SERIAL PRIMARY KEY,
    npc_spawn_id INTEGER NOT NULL DEFAULT 0,
    actor_def_id INTEGER NOT NULL DEFAULT 0,
    profile_id   INTEGER NOT NULL DEFAULT 0,
    enabled      BOOLEAN NOT NULL DEFAULT TRUE
);

CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_spawn
    ON npc_profile_bindings (npc_spawn_id);

CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_actor
    ON npc_profile_bindings (actor_def_id);

CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_profile
    ON npc_profile_bindings (profile_id);
