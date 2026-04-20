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
