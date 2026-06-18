package db

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"log"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/google/uuid"
	_ "github.com/jackc/pgx/v5/stdlib" // PostgreSQL driver for database/sql
	_ "modernc.org/sqlite"             // SQLite - pure Go, no CGo needed
	"realm-crafter/server/internal/world"
)

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// Account mirrors the accounts table.
type Account struct {
	ID           string
	Username     string
	PasswordHash string
	Email        string
	IsGM         bool
	IsBanned     bool
}

// ItemTemplate mirrors the item_templates table.
type ItemTemplate struct {
	ID           int
	Name         string
	ItemType     uint8 // 0=weapon 1=armor 2=consumable 3=misc
	SlotType     uint8 // equip slot: 0=weapon 1=shield 2=hat 3=chest 4=hands 5=belt 6=legs 7=feet 8=ring 9=amulet 255=backpack-only
	WeaponDamage int16
	ArmorLevel   int16
	WeaponDimension int32  // 0=melee, 1=ranged, 2=magic (matches world.CombatDimension)
	WeaponHands     int32  // 1=one-hand, 2=two-hand (no mechanical effect yet)
	WeaponRange     float32 // explicit attack range (world units); 0 = use dimension default
	WeaponKit    string // kit_key reference in weapon_kits, "" if item is not a kit-providing weapon
	MaxStack     uint8
	ItemValue    int32
	Stackable    bool
	ModelPath    string
	ModelScale   float32
	SocketName   string
}

// ItemAttribute holds one bonus row for an item.
type ItemAttribute struct {
	ID           int
	ItemID       int
	AttributeKey string
	Value        float64
}

// WeaponKit defines a kit of skills granted by a weapon type (sword, bow, etc).
type WeaponKit struct {
	ID          int
	KitKey      string // unique identifier ("sword", "bow", "staff", ...)
	DisplayName string // user-facing name ("Sword", "Bow")
	Description string
	Enabled     bool
}

// LootTable groups one drop table definition.
type LootTable struct {
	ID      int
	Name    string
	Enabled bool
}

// LootEntry defines an item row in a loot table.
type LootEntry struct {
	ID          int
	LootTableID int
	ItemID      int
	Chance      float64
	MinQty      int
	MaxQty      int
}

// WeaponKitAbility links an ability_template to a weapon_kit at a specific slot.
type WeaponKitAbility struct {
	ID        int
	KitID     int // FK lógica para weapon_kits.id
	AbilityID int // FK lógica para ability_templates.id
	SlotIndex int // posição no hotbar (0 = primeiro)
	Enabled   bool
}

// EquipmentSlotConfig defines whether an equipment slot grants a skill kit,
// and how many of those skills can be in the player's active loadout.
type EquipmentSlotConfig struct {
	SlotID             int // 0=weapon, 1=shield, etc. Canonical from item_templates.slot_type.
	GivesKit           bool
	HotbarSlotsGranted int // number of player hotbar slots this equipment slot grants.
	Enabled            bool
}

// CharacterSkillLoadout is one ability assigned to a slot in a character's kit loadout.
type CharacterSkillLoadout struct {
	ID          int
	CharacterID string // UUID
	KitID       int    // weapon_kits.id
	SlotIndex   int    // 0..N-1 within this kit's hotbar contribution
	AbilityID   int    // ability_templates.id
}

// CharacterSkillProgress tracks per-character per-ability XP and level.
type CharacterSkillProgress struct {
	ID          int
	CharacterID string
	AbilityID   int
	XP          int
	Level       int
}

// SkillProgressionConfig holds engine-wide rules for skill leveling.
// Singleton: there's only ever one row (id=1).
type SkillProgressionConfig struct {
	ID                    int
	XPPerUse              int
	MaxLevel              int
	XPCurveType           string // "irregular", "linear", "quadratic", or "exponential"
	XPCurveBase           int
	XPCurveExponent       float64
	XPIrregularity        float64
	DamageBonusPerLevel   float64
	CooldownReduxPerLevel float64
}

// CharacterProgressionConfig defines global character level progression rules.
// Singleton: there should only be one row (id=1).
type CharacterProgressionConfig struct {
	ID                   int
	MaxLevel             int
	XPCurveType          string
	XPCurveBase          int
	XPCurveFactor        float64
	XPCurveExponent      float64
	XPIrregularity       float64
	StatPointsPerLevel   int
	InitialStatValue     int
	RespecFreeUntilLevel int
	RespecCostGold       int
}

// CharacterPrimaryStatsLevel stores the primary stats granted at a given level.
type CharacterPrimaryStatsLevel struct {
	Level        int
	Strength     int32
	Dexterity    int32
	Intelligence int32
	Wisdom       int32
	Perception   int32
}

// KillXPScalingConfig defines kill XP scaling knobs.
// Singleton: there should only be one row (id=1).
type KillXPScalingConfig struct {
	ID                   int
	BaseXPPerNPCLevel    int
	LevelDiffCoefficient float64
	MultiplierMin        float64
	MultiplierMax        float64
	MasteryXPPerMobLevel int
	MasteryKillingBlow   float64
	MasteryWindowTimeout int
}

// FXTemplate defines one visual effect template keyed by path-like string.
type FXTemplate struct {
	ID          int
	Key         string
	DisplayName string

	BurstCount      int
	StreamInterval  float32
	LifetimeSeconds float32

	SpeedMin       float32
	SpeedMax       float32
	VelocityBiasX  float32
	VelocityBiasY  float32
	VelocityBiasZ  float32
	VelocitySpread float32

	ColorStartR float32
	ColorStartG float32
	ColorStartB float32
	ColorStartA float32
	ColorEndR   float32
	ColorEndG   float32
	ColorEndB   float32
	ColorEndA   float32
	SizeStart   float32
	SizeEnd     float32
	TexturePath string

	Enabled bool
}

// PlayerKitAbilityEntry is one ability slot in an active kit.
type PlayerKitAbilityEntry struct {
	SlotIndex   int
	AbilityID   int
	AbilityName string // for logging/debug
	CooldownMs  int64
}

// KitPoolAbilityEntry is one enabled ability available in a kit pool.
type KitPoolAbilityEntry struct {
	AbilityID   int
	AbilityName string
	CooldownMs  int64
}

// PlayerKitResolution holds the active weapon kit for a player and its abilities.
// If no kit is active, HasKit is false and other fields are zero-values.
type PlayerKitResolution struct {
	HasKit         bool
	KitID          int
	KitKey         string
	KitDisplayName string
	Abilities      []PlayerKitAbilityEntry // ordered by slot_index
}

// CharacterItem mirrors the character_items table.
type CharacterItem struct {
	Slot         uint8
	ItemID       uint16
	Quantity     uint8
	Durability   uint8
	Name         string
	ItemType     uint8
	SlotType     uint8
	WeaponDamage int16
	ArmorLevel   int16
	ItemValue    int32
	ModelPath    string
	ModelScale   float32
	SocketName   string
}

// Character mirrors the characters table.
type Character struct {
	ID                  string
	AccountID           string
	Slot                int
	Name                string
	Race                string
	Class               string
	Gender              int
	Level               int
	XP                  int64
	Gold                int64
	AreaName            string
	X, Y, Z             float32
	Yaw                 float32
	FaceTex             int
	Hair                int
	Beard               int
	BodyTex             int
	Health              int32
	HealthMax           int32
	Energy              int32
	EnergyMax           int32
	PrimaryStrength     int32
	PrimaryDexterity    int32
	PrimaryIntelligence int32
	PrimaryWisdom       int32
	PrimaryPerception   int32
	UnspentStatPoints   int32
	FreeRespecsUsed     int32
	ActorDefID          int
}

// PlayableDef is a minimal view of media_actor_defs used for the character-
// creation dropdown.
type PlayableDef struct {
	ID           int
	Name         string
	DefaultRace  string
	DefaultClass string
}

// Quest objective type constants.
const (
	QuestObjectiveKill     uint8 = 1
	QuestObjectiveCollect  uint8 = 2
	QuestObjectiveTalk     uint8 = 3
	QuestObjectiveExplore  uint8 = 4
	QuestObjectiveInteract uint8 = 5
)

// Quest runtime state constants.
const (
	QuestStateActive uint8 = iota + 1
	QuestStateCompleted
	QuestStateTurnedIn
	QuestStateFailed
	QuestStateAbandoned
)

const (
	questStateTextActive    = "active"
	questStateTextCompleted = "completed"
	questStateTextTurnedIn  = "turned_in"
	questStateTextFailed    = "failed"
	questStateTextAbandoned = "abandoned"
)

// QuestLogEntry is one quest row sent to the client quest log.
type QuestLogEntry struct {
	QuestID     int
	Code        string
	Title       string
	Description string
	State       uint8
	Objectives  []QuestLogObjective
}

// QuestAvailableEntry is one quest that can be accepted by the character.
type QuestAvailableEntry struct {
	QuestID     int
	Code        string
	Title       string
	Description string
	MinLevel    int
	Repeatable  bool
}

// QuestLogObjective is one objective row inside a quest log entry.
type QuestLogObjective struct {
	ObjectiveID   int
	ObjectiveType uint8
	Description   string
	CurrentCount  int
	TargetCount   int
	TargetNPCName string
	TargetItemID  int
	TargetArea    string
}

// QuestRewardEntry describes one reward row attached to a quest turn-in.
type QuestRewardEntry struct {
	XPReward   int64
	GoldReward int64
	ItemID     uint16
	ItemQty    uint8
}

// QuestTurnInResult contains the final state updates produced by a successful
// turn-in transaction (quest state + rewards applied atomically).
type QuestTurnInResult struct {
	Rewards      []QuestRewardEntry
	NewGold      int64
	NewXP        int64
	NewLevel     int
	Leveled      bool
	GoldChanged  bool
	XPChanged    bool
	ItemsChanged bool
}

// QuestProgressEvent is emitted by gameplay systems (kill/pickup/dialog/etc.)
// and consumed by DB quest progression.
type QuestProgressEvent struct {
	ObjectiveType uint8
	TargetNPCName string
	TargetItemID  uint16
	TargetArea    string
	Delta         int
}

// ---------------------------------------------------------------------------
// DB
// ---------------------------------------------------------------------------

// DB wraps a database/sql pool. Supports SQLite (dev) and PostgreSQL (prod).
type DB struct {
	db     *sql.DB
	driver string // "sqlite" or "postgres"
}

// Open creates a DB connection, runs migrations, and returns a ready DB.
//
//	driver = "sqlite"   Ã¢â€ â€™ dsn is a file path, e.g. "./rco.db"
//	driver = "postgres" Ã¢â€ â€™ dsn is a postgres:// URL
func Open(ctx context.Context, driver, dsn string) (*DB, error) {
	var sqlDriver string
	switch driver {
	case "sqlite":
		sqlDriver = "sqlite"
	case "postgres", "postgresql":
		sqlDriver = "pgx"
	default:
		return nil, fmt.Errorf("db: unknown driver %q (use \"sqlite\" or \"postgres\")", driver)
	}

	raw, err := sql.Open(sqlDriver, dsn)
	if err != nil {
		return nil, fmt.Errorf("db: open: %w", err)
	}

	if err := raw.PingContext(ctx); err != nil {
		_ = raw.Close()
		return nil, fmt.Errorf("db: ping: %w", err)
	}

	d := &DB{db: raw, driver: driver}

	if driver == "sqlite" {
		// Foreign keys are disabled by default in SQLite.
		if _, err := raw.ExecContext(ctx, "PRAGMA foreign_keys = ON;"); err != nil {
			_ = raw.Close()
			return nil, fmt.Errorf("db: sqlite pragma: %w", err)
		}
		// Improve writer concurrency and busy handling for live gameplay writes.
		if _, err := raw.ExecContext(ctx, "PRAGMA journal_mode = WAL;"); err != nil {
			_ = raw.Close()
			return nil, fmt.Errorf("db: sqlite pragma journal_mode: %w", err)
		}
		if _, err := raw.ExecContext(ctx, "PRAGMA busy_timeout = 5000;"); err != nil {
			_ = raw.Close()
			return nil, fmt.Errorf("db: sqlite pragma busy_timeout: %w", err)
		}
		if _, err := raw.ExecContext(ctx, "PRAGMA synchronous = NORMAL;"); err != nil {
			_ = raw.Close()
			return nil, fmt.Errorf("db: sqlite pragma synchronous: %w", err)
		}
	}

	if err := d.migrate(ctx); err != nil {
		_ = raw.Close()
		return nil, fmt.Errorf("db: migrate: %w", err)
	}

	d.migrateV2(ctx)
	d.migrateV3(ctx)
	d.migrateV4(ctx)
	d.migrateV5(ctx)
	d.migrateV6(ctx)
	d.migrateV7(ctx)
	d.migrateV9(ctx)
	d.migrateV10(ctx)
	d.migrateV11(ctx)
	d.migrateV12(ctx)
	d.migrateV13(ctx)
	d.migrateV14(ctx)
	d.migrateV15(ctx)
	d.migrateV16(ctx)
	d.migrateV17(ctx)
	d.migrateV18(ctx)
	d.migrateV19(ctx)
	d.migrateV20(ctx)
	d.migrateV21(ctx)
	d.migrateV22(ctx)
	d.migrateV23(ctx)
	d.migrateV24(ctx)
	d.migrateV25(ctx)
	d.migrateV26(ctx)
	d.migrateV27(ctx)
	d.migrateV28(ctx)
	d.migrateV29(ctx)
	d.migrateV30(ctx)
	d.migrateV31(ctx)
	d.migrateV32(ctx)
	d.migrateV33(ctx)
	d.migrateV34(ctx)
	d.migrateV35(ctx)
	d.migrateV36(ctx)
	d.migrateV37(ctx)
	d.migrateV38(ctx)
	d.migrateV39(ctx)
	d.migrateV40(ctx)
	d.migrateV41(ctx)
	d.migrateV42(ctx)
	d.migrateV43(ctx)
	d.migrateV44(ctx)
	d.migrateV45(ctx)
	d.migrateV46(ctx)
	d.migrateV47(ctx)
	d.migrateV48(ctx)
	d.migrateV49(ctx)

	return d, nil
}

// ItemSocketOverride holds one optional override row for rendering item meshes
// on a specific actor_def/socket combination.
type ItemSocketOverride struct {
	ID              int
	ItemTemplateID  int
	ActorDefID      int
	OffsetPosX      float64
	OffsetPosY      float64
	OffsetPosZ      float64
	OffsetRotX      float64
	OffsetRotY      float64
	OffsetRotZ      float64
	OffsetScale     float64
}

// Close releases the underlying connection pool.
func (d *DB) Close() {
	_ = d.db.Close()
}

// ---------------------------------------------------------------------------
// Placeholder translation
// q() converts ? placeholders to $1, $2, Ã¢â‚¬Â¦ for PostgreSQL.
// SQLite and PostgreSQL are the only two drivers; SQLite uses ?.
// ---------------------------------------------------------------------------

func (d *DB) q(query string) string {
	if d.driver != "postgres" {
		return query
	}
	n := 0
	var b strings.Builder
	b.Grow(len(query) + 8)
	for _, c := range query {
		if c == '?' {
			n++
			b.WriteString(fmt.Sprintf("$%d", n))
		} else {
			b.WriteRune(c)
		}
	}
	return b.String()
}

// now returns the current UTC time in a format both drivers accept.
func now() string {
	return time.Now().UTC().Format(time.RFC3339)
}

func boolFromDB(v interface{}) bool {
	switch b := v.(type) {
	case bool:
		return b
	case int:
		return b != 0
	case int32:
		return b != 0
	case int64:
		return b != 0
	case uint:
		return b != 0
	case uint32:
		return b != 0
	case uint64:
		return b != 0
	case []byte:
		s := strings.TrimSpace(string(b))
		if s == "" {
			return false
		}
		if n, err := strconv.ParseInt(s, 10, 64); err == nil {
			return n != 0
		}
		return strings.EqualFold(s, "true") || strings.EqualFold(s, "t")
	case string:
		s := strings.TrimSpace(b)
		if s == "" {
			return false
		}
		if n, err := strconv.ParseInt(s, 10, 64); err == nil {
			return n != 0
		}
		return strings.EqualFold(s, "true") || strings.EqualFold(s, "t")
	default:
		return false
	}
}

func questStateToCode(state string) uint8 {
	switch state {
	case questStateTextActive:
		return QuestStateActive
	case questStateTextCompleted:
		return QuestStateCompleted
	case questStateTextTurnedIn:
		return QuestStateTurnedIn
	case questStateTextFailed:
		return QuestStateFailed
	case questStateTextAbandoned:
		return QuestStateAbandoned
	default:
		return 0
	}
}

// ---------------------------------------------------------------------------
// Migrations
// ---------------------------------------------------------------------------

// sqliteMigration uses types native to SQLite.
const sqliteMigration = `
CREATE TABLE IF NOT EXISTS accounts (
	id            TEXT PRIMARY KEY,
	username      TEXT NOT NULL UNIQUE,
	password_hash TEXT NOT NULL,
	email         TEXT NOT NULL DEFAULT '',
	is_gm         INTEGER NOT NULL DEFAULT 0,
	is_banned     INTEGER NOT NULL DEFAULT 0,
	created_at    TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS characters (
	id          TEXT PRIMARY KEY,
	account_id  TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
	slot        INTEGER NOT NULL CHECK (slot >= 0 AND slot <= 8),
	name        TEXT NOT NULL UNIQUE,
	race        TEXT NOT NULL DEFAULT 'Human',
	class       TEXT NOT NULL DEFAULT 'Warrior',
	gender      INTEGER NOT NULL DEFAULT 0,
	level       INTEGER NOT NULL DEFAULT 1,
	xp          INTEGER NOT NULL DEFAULT 0,
	gold        INTEGER NOT NULL DEFAULT 100,
	area_name   TEXT NOT NULL DEFAULT 'Starter Zone',
	x           REAL NOT NULL DEFAULT 0,
	y           REAL NOT NULL DEFAULT 0,
	z           REAL NOT NULL DEFAULT 0,
	yaw         REAL NOT NULL DEFAULT 0,
	face_tex    INTEGER NOT NULL DEFAULT 0,
	hair        INTEGER NOT NULL DEFAULT 0,
	beard       INTEGER NOT NULL DEFAULT 0,
	body_tex    INTEGER NOT NULL DEFAULT 0,
	health      INTEGER NOT NULL DEFAULT 100,
	health_max  INTEGER NOT NULL DEFAULT 100,
	energy      INTEGER NOT NULL DEFAULT 100,
	energy_max  INTEGER NOT NULL DEFAULT 100,
	created_at  TEXT NOT NULL,
	UNIQUE(account_id, slot)
);

CREATE TABLE IF NOT EXISTS item_templates (
	id        INTEGER PRIMARY KEY AUTOINCREMENT,
	name      TEXT NOT NULL UNIQUE,
	item_type INTEGER NOT NULL DEFAULT 3,
	max_stack INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS character_items (
	character_id TEXT NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
	slot         INTEGER NOT NULL CHECK (slot >= 0 AND slot <= 44),
	item_id      INTEGER NOT NULL REFERENCES item_templates(id),
	quantity     INTEGER NOT NULL DEFAULT 1,
	durability   INTEGER NOT NULL DEFAULT 100,
	PRIMARY KEY (character_id, slot)
);
`

// postgresMigration uses PostgreSQL-native types.
const postgresMigration = `
CREATE TABLE IF NOT EXISTS accounts (
	id            TEXT PRIMARY KEY,
	username      VARCHAR(32) NOT NULL UNIQUE,
	password_hash VARCHAR(60) NOT NULL,
	email         VARCHAR(256) NOT NULL DEFAULT '',
	is_gm         BOOLEAN NOT NULL DEFAULT FALSE,
	is_banned     BOOLEAN NOT NULL DEFAULT FALSE,
	created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS characters (
	id          TEXT PRIMARY KEY,
	account_id  TEXT NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
	slot        SMALLINT NOT NULL CHECK (slot >= 0 AND slot <= 8),
	name        VARCHAR(32) NOT NULL UNIQUE,
	race        VARCHAR(32) NOT NULL DEFAULT 'Human',
	class       VARCHAR(32) NOT NULL DEFAULT 'Warrior',
	gender      SMALLINT NOT NULL DEFAULT 0,
	level       SMALLINT NOT NULL DEFAULT 1,
	xp          BIGINT NOT NULL DEFAULT 0,
	gold        BIGINT NOT NULL DEFAULT 100,
	area_name   VARCHAR(64) NOT NULL DEFAULT 'Starter Zone',
	x           REAL NOT NULL DEFAULT 0,
	y           REAL NOT NULL DEFAULT 0,
	z           REAL NOT NULL DEFAULT 0,
	yaw         REAL NOT NULL DEFAULT 0,
	face_tex    SMALLINT NOT NULL DEFAULT 0,
	hair        SMALLINT NOT NULL DEFAULT 0,
	beard       SMALLINT NOT NULL DEFAULT 0,
	body_tex    SMALLINT NOT NULL DEFAULT 0,
	health      INTEGER NOT NULL DEFAULT 100,
	health_max  INTEGER NOT NULL DEFAULT 100,
	energy      INTEGER NOT NULL DEFAULT 100,
	energy_max  INTEGER NOT NULL DEFAULT 100,
	created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
	UNIQUE(account_id, slot)
);

CREATE TABLE IF NOT EXISTS item_templates (
	id        SERIAL PRIMARY KEY,
	name      VARCHAR(64) NOT NULL UNIQUE,
	item_type SMALLINT NOT NULL DEFAULT 3,
	max_stack SMALLINT NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS character_items (
	character_id TEXT NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
	slot         SMALLINT NOT NULL CHECK (slot >= 0 AND slot <= 44),
	item_id      INTEGER NOT NULL REFERENCES item_templates(id),
	quantity     SMALLINT NOT NULL DEFAULT 1,
	durability   SMALLINT NOT NULL DEFAULT 100,
	PRIMARY KEY (character_id, slot)
);
`

func (d *DB) migrate(ctx context.Context) error {
	schema := sqliteMigration
	if d.driver == "postgres" {
		schema = postgresMigration
	}
	_, err := d.db.ExecContext(ctx, schema)
	return err
}

// migrateV2 adds new columns to existing tables. Errors are ignored so it's
// safe to run against a DB that already has some or all of these columns.
func (d *DB) migrateV2(ctx context.Context) {
	var alters []string
	if d.driver == "postgres" {
		alters = []string{
			"ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS slot_type     SMALLINT NOT NULL DEFAULT 255",
			"ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS weapon_damage SMALLINT NOT NULL DEFAULT 0",
			"ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS armor_level   SMALLINT NOT NULL DEFAULT 0",
			"ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS weapon_type   SMALLINT NOT NULL DEFAULT 0",
			"ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS item_value    INTEGER  NOT NULL DEFAULT 0",
			"ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS stackable     BOOLEAN  NOT NULL DEFAULT FALSE",
		}
	} else {
		alters = []string{
			"ALTER TABLE item_templates ADD COLUMN slot_type     INTEGER NOT NULL DEFAULT 255",
			"ALTER TABLE item_templates ADD COLUMN weapon_damage INTEGER NOT NULL DEFAULT 0",
			"ALTER TABLE item_templates ADD COLUMN armor_level   INTEGER NOT NULL DEFAULT 0",
			"ALTER TABLE item_templates ADD COLUMN weapon_type   INTEGER NOT NULL DEFAULT 0",
			"ALTER TABLE item_templates ADD COLUMN item_value    INTEGER NOT NULL DEFAULT 0",
			"ALTER TABLE item_templates ADD COLUMN stackable     INTEGER NOT NULL DEFAULT 0",
		}
	}
	for _, sql := range alters {
		_, _ = d.db.ExecContext(ctx, sql)
	}
}

// migrateV3 fixes data values that changed after initial seeding.
func (d *DB) migrateV3(ctx context.Context) {
	_, _ = d.db.ExecContext(ctx,
		`UPDATE item_templates SET item_value = 50 WHERE name = 'Health Potion' AND item_type = 2 AND item_value < 50`)
}

// migrateV4 creates spell tables and seeds default spells.
func (d *DB) migrateV4(ctx context.Context) {
	// Create tables (idempotent).
	if d.driver == "postgres" {
		_, _ = d.db.ExecContext(ctx, `
			CREATE TABLE IF NOT EXISTS spell_templates (
				id           SERIAL PRIMARY KEY,
				name         VARCHAR(64) NOT NULL UNIQUE,
				spell_type   SMALLINT NOT NULL DEFAULT 0,
				damage_min   INTEGER NOT NULL DEFAULT 0,
				damage_max   INTEGER NOT NULL DEFAULT 0,
				ep_cost      INTEGER NOT NULL DEFAULT 10,
				cooldown_ms  INTEGER NOT NULL DEFAULT 2000,
				range        REAL NOT NULL DEFAULT 20.0,
				icon         SMALLINT NOT NULL DEFAULT 0
			);
			CREATE TABLE IF NOT EXISTS character_known_spells (
				character_id TEXT NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
				spell_id     INTEGER NOT NULL REFERENCES spell_templates(id),
				PRIMARY KEY (character_id, spell_id)
			);`)
	} else {
		_, _ = d.db.ExecContext(ctx, `
			CREATE TABLE IF NOT EXISTS spell_templates (
				id           INTEGER PRIMARY KEY AUTOINCREMENT,
				name         TEXT NOT NULL UNIQUE,
				spell_type   INTEGER NOT NULL DEFAULT 0,
				damage_min   INTEGER NOT NULL DEFAULT 0,
				damage_max   INTEGER NOT NULL DEFAULT 0,
				ep_cost      INTEGER NOT NULL DEFAULT 10,
				cooldown_ms  INTEGER NOT NULL DEFAULT 2000,
				range        REAL NOT NULL DEFAULT 20.0,
				icon         INTEGER NOT NULL DEFAULT 0
			);
			CREATE TABLE IF NOT EXISTS character_known_spells (
				character_id TEXT NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
				spell_id     INTEGER NOT NULL REFERENCES spell_templates(id),
				PRIMARY KEY (character_id, spell_id)
			);`)
	}

	// Seed default spells (ignore errors if they already exist).
	seeds := []struct {
		name                      string
		stype, dmin, dmax, ep, cd int
		rng                       float64
	}{
		{"Fireball", 0, 20, 35, 20, 2000, 25.0},
		{"Heal", 1, 30, 50, 15, 3000, 0.0},
		{"Lightning Bolt", 0, 30, 50, 30, 3000, 30.0},
	}
	for _, s := range seeds {
		_, _ = d.db.ExecContext(ctx,
			d.q(`INSERT INTO spell_templates (name,spell_type,damage_min,damage_max,ep_cost,cooldown_ms,range)
			     VALUES (?,?,?,?,?,?,?) ON CONFLICT DO NOTHING`),
			s.name, s.stype, s.dmin, s.dmax, s.ep, s.cd, s.rng)
	}
}

// ---------------------------------------------------------------------------
// Account methods
// ---------------------------------------------------------------------------

// CreateAccount inserts a new account and returns its UUID.
func (d *DB) CreateAccount(ctx context.Context, username, passwordHash, email string) (string, error) {
	id := uuid.New().String()
	_, err := d.db.ExecContext(ctx,
		d.q(`INSERT INTO accounts (id, username, password_hash, email, created_at)
		     VALUES (?, ?, ?, ?, ?)`),
		id, username, passwordHash, email, now(),
	)
	if err != nil {
		return "", fmt.Errorf("db: CreateAccount: %w", err)
	}
	return id, nil
}

// GetAccountByUsername fetches an account by username.
func (d *DB) GetAccountByUsername(ctx context.Context, username string) (*Account, error) {
	a := &Account{}
	var isGM, isBanned int
	err := d.db.QueryRowContext(ctx,
		d.q(`SELECT id, username, password_hash, email, is_gm, is_banned
		     FROM accounts WHERE username = ?`),
		username,
	).Scan(&a.ID, &a.Username, &a.PasswordHash, &a.Email, &isGM, &isBanned)
	if err != nil {
		return nil, fmt.Errorf("db: GetAccountByUsername: %w", err)
	}
	a.IsGM = isGM != 0
	a.IsBanned = isBanned != 0
	return a, nil
}

// ---------------------------------------------------------------------------
// Character methods
// ---------------------------------------------------------------------------

// ListCharacters returns all characters for an account ordered by slot.
func (d *DB) ListCharacters(ctx context.Context, accountID string) ([]*Character, error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT id, account_id, slot, name, race, class, gender, level, xp, gold,
		            area_name, x, y, z, yaw, face_tex, hair, beard, body_tex,
		            health, health_max, energy, energy_max,
		            primary_strength, primary_dexterity, primary_intelligence, primary_wisdom, primary_perception,
		            unspent_stat_points, free_respecs_used,
		            actor_def_id
		     FROM characters WHERE account_id = ? ORDER BY slot`),
		accountID,
	)
	if err != nil {
		return nil, fmt.Errorf("db: ListCharacters: %w", err)
	}
	defer rows.Close()

	var chars []*Character
	for rows.Next() {
		c := &Character{}
		if err := rows.Scan(
			&c.ID, &c.AccountID, &c.Slot, &c.Name, &c.Race, &c.Class, &c.Gender,
			&c.Level, &c.XP, &c.Gold, &c.AreaName, &c.X, &c.Y, &c.Z, &c.Yaw,
			&c.FaceTex, &c.Hair, &c.Beard, &c.BodyTex,
			&c.Health, &c.HealthMax, &c.Energy, &c.EnergyMax,
			&c.PrimaryStrength, &c.PrimaryDexterity, &c.PrimaryIntelligence, &c.PrimaryWisdom, &c.PrimaryPerception,
			&c.UnspentStatPoints, &c.FreeRespecsUsed,
			&c.ActorDefID,
		); err != nil {
			return nil, fmt.Errorf("db: ListCharacters scan: %w", err)
		}
		chars = append(chars, c)
	}
	return chars, rows.Err()
}

// CreateCharacter inserts a new character into the specified slot.
// Initial x/z put the character at the center of the default Starter Zone
// (1024Ãƒâ€”1024 world units Ã¢â€ â€™ center is 512). Column defaults can't be relied
// on across SQLite/Postgres for this Ã¢â‚¬â€ so we set them explicitly.
func (d *DB) CreateCharacter(ctx context.Context, accountID string, slot int, name, race, className string, gender, faceTex, hair, beard, bodyTex, actorDefID int) (*Character, error) {
	id := uuid.New().String()
	cfg := world.GetCachedCharProgressionConfig()
	initialStatValue := cfg.InitialStatValue
	if initialStatValue < 1 {
		initialStatValue = 1
	}
	_, err := d.db.ExecContext(ctx,
		d.q(`INSERT INTO characters
		       (id, account_id, slot, name, race, class, gender, face_tex, hair, beard, body_tex, actor_def_id, created_at,
		        primary_strength, primary_dexterity, primary_intelligence, primary_wisdom, primary_perception,
		        unspent_stat_points, free_respecs_used,
		        x, z)
		     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0, 512, 512)`),
		id, accountID, slot, name, race, className, gender, faceTex, hair, beard, bodyTex, actorDefID, now(),
		initialStatValue, initialStatValue, initialStatValue, initialStatValue, initialStatValue,
	)
	if err != nil {
		return nil, fmt.Errorf("db: CreateCharacter: %w", err)
	}
	return d.GetCharacterBySlot(ctx, accountID, slot)
}

// DeleteCharacter removes a character by account + slot.
func (d *DB) DeleteCharacter(ctx context.Context, accountID string, slot int) error {
	res, err := d.db.ExecContext(ctx,
		d.q(`DELETE FROM characters WHERE account_id = ? AND slot = ?`),
		accountID, slot,
	)
	if err != nil {
		return fmt.Errorf("db: DeleteCharacter: %w", err)
	}
	n, _ := res.RowsAffected()
	if n == 0 {
		return fmt.Errorf("db: DeleteCharacter: no character in slot %d", slot)
	}
	return nil
}

// GetCharacterBySlot fetches a single character by account + slot.
func (d *DB) GetCharacterBySlot(ctx context.Context, accountID string, slot int) (*Character, error) {
	c := &Character{}
	err := d.db.QueryRowContext(ctx,
		d.q(`SELECT id, account_id, slot, name, race, class, gender, level, xp, gold,
		          area_name, x, y, z, yaw, face_tex, hair, beard, body_tex,
		          health, health_max, energy, energy_max,
		          primary_strength, primary_dexterity, primary_intelligence, primary_wisdom, primary_perception,
		          unspent_stat_points, free_respecs_used,
		          actor_def_id
		     FROM characters WHERE account_id = ? AND slot = ?`),
		accountID, slot,
	).Scan(
		&c.ID, &c.AccountID, &c.Slot, &c.Name, &c.Race, &c.Class, &c.Gender,
		&c.Level, &c.XP, &c.Gold, &c.AreaName, &c.X, &c.Y, &c.Z, &c.Yaw,
		&c.FaceTex, &c.Hair, &c.Beard, &c.BodyTex,
		&c.Health, &c.HealthMax, &c.Energy, &c.EnergyMax,
		&c.PrimaryStrength, &c.PrimaryDexterity, &c.PrimaryIntelligence, &c.PrimaryWisdom, &c.PrimaryPerception,
		&c.UnspentStatPoints, &c.FreeRespecsUsed,
		&c.ActorDefID,
	)
	if err != nil {
		return nil, fmt.Errorf("db: GetCharacterBySlot: %w", err)
	}
	return c, nil
}

// SaveCharacterPosition persists the player's current position.
func (d *DB) SaveCharacterPosition(ctx context.Context, charID, areaName string, x, y, z, yaw float32) error {
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE characters SET area_name=?, x=?, y=?, z=?, yaw=? WHERE id=?`),
		areaName, x, y, z, yaw, charID,
	)
	if err != nil {
		return fmt.Errorf("db: SaveCharacterPosition: %w", err)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Inventory methods
// ---------------------------------------------------------------------------

// SeedDefaultItems inserts starter item templates if they don't exist yet.
func (d *DB) SeedDefaultItems(ctx context.Context) error {
	// item_type: 0=weapon 1=armor 2=consumable 3=misc
	// slot_type: 0=weapon 1=shield 2=hat 3=chest 4=hands 5=belt 6=legs 7=feet 8=ring 9=amulet 255=bag-only
	items := []struct {
		name            string
		itemType        int
		slotType        int
		weaponDamage    int
		armorLevel      int
		weaponDimension int
		weaponHands     int
		maxStack        int
		itemValue       int
		stackable       int
	}{
		{"Rusty Sword", 0, 0, 15, 0, 0, 1, 1, 10, 0},
		{"Old Shield", 1, 1, 0, 5, 0, 1, 1, 8, 0},
		{"Leather Tunic", 1, 3, 0, 8, 0, 1, 1, 15, 0},
		{"Leather Hat", 1, 2, 0, 3, 0, 1, 1, 5, 0},
		{"Leather Gloves", 1, 4, 0, 2, 0, 1, 1, 4, 0},
		{"Leather Belt", 1, 5, 0, 2, 0, 1, 1, 3, 0},
		{"Leather Leggings", 1, 6, 0, 5, 0, 1, 1, 10, 0},
		{"Traveller's Boots", 1, 7, 0, 3, 0, 1, 1, 6, 0},
		{"Health Potion", 2, 255, 0, 0, 0, 1, 10, 50, 1},
		{"Iron Ring", 3, 8, 0, 0, 0, 1, 1, 20, 0},
	}
	for _, it := range items {
		_, err := d.db.ExecContext(ctx,
			d.q(`INSERT INTO item_templates
			       (name, item_type, slot_type, weapon_damage, armor_level, weapon_dimension, weapon_hands, max_stack, item_value, stackable, weapon_kit)
			     SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?
			     WHERE NOT EXISTS (SELECT 1 FROM item_templates WHERE name = ?)`),
			it.name, it.itemType, it.slotType, it.weaponDamage, it.armorLevel,
			it.weaponDimension, it.weaponHands, it.maxStack, it.itemValue, it.stackable, "", it.name,
		)
		if err != nil {
			return fmt.Errorf("db: SeedDefaultItems %q: %w", it.name, err)
		}
	}
	return nil
}

// SeedDefaultWeaponKits creates the default weapon kits (sword, bow) with their abilities.
// Idempotent: base entries are inserted only when missing by unique key (name/kit_key),
// and kit ability slots are reset to canonical seeded mappings each run.
func (d *DB) SeedDefaultWeaponKits(ctx context.Context) error {
	type abilitySeed struct {
		Name               string
		Family             string
		ResourceType       string
		ResourceCost       int
		CooldownMs         int
		RangeMin           float64
		RangeMax           float64
		WindupMs           int
		ImpactDelayMs      int
		RecoverMs          int
		ParryWindowMs      int
		Interruptible      bool
		BaseDamageMin      int
		BaseDamageMax      int
		TelegraphType      string
		TelegraphRadius    float64
		TelegraphColorRGBA string
		ActionWindup       string
		ActionImpact       string
		ActionRecover      string
		Enabled            bool
	}
	type kitSeed struct {
		KitKey      string
		DisplayName string
		Description string
		Enabled     bool
	}
	type kitAbilitySeed struct {
		KitKey      string
		AbilityName string
		SlotIndex   int
		Enabled     bool
	}

	abilities := []abilitySeed{
		{
			Name:               "sword_slash",
			Family:             "melee_basic",
			ResourceType:       "stamina",
			ResourceCost:       0,
			CooldownMs:         600,
			RangeMin:           0.0,
			RangeMax:           2.5,
			WindupMs:           200,
			ImpactDelayMs:      0,
			RecoverMs:          300,
			ParryWindowMs:      0,
			Interruptible:      true,
			BaseDamageMin:      8,
			BaseDamageMax:      14,
			TelegraphType:      "none",
			TelegraphRadius:    0.0,
			TelegraphColorRGBA: "1,0.2,0.2,0.75",
			ActionWindup:       "AttackBasic",
			ActionImpact:       "AttackBasic",
			ActionRecover:      "Idle",
			Enabled:            true,
		},
		{
			Name:               "sword_cleave",
			Family:             "melee_special",
			ResourceType:       "stamina",
			ResourceCost:       20,
			CooldownMs:         5000,
			RangeMin:           0.0,
			RangeMax:           3.0,
			WindupMs:           700,
			ImpactDelayMs:      50,
			RecoverMs:          500,
			ParryWindowMs:      250,
			Interruptible:      true,
			BaseDamageMin:      18,
			BaseDamageMax:      28,
			TelegraphType:      "cone",
			TelegraphRadius:    3.0,
			TelegraphColorRGBA: "1,0.4,0.2,0.75",
			ActionWindup:       "AttackHeavyWindup",
			ActionImpact:       "AttackHeavyImpact",
			ActionRecover:      "Recover",
			Enabled:            true,
		},
		{
			Name:               "bow_quickshot",
			Family:             "ranged_basic",
			ResourceType:       "stamina",
			ResourceCost:       5,
			CooldownMs:         800,
			RangeMin:           5.0,
			RangeMax:           30.0,
			WindupMs:           150,
			ImpactDelayMs:      0,
			RecoverMs:          250,
			ParryWindowMs:      0,
			Interruptible:      true,
			BaseDamageMin:      6,
			BaseDamageMax:      10,
			TelegraphType:      "none",
			TelegraphRadius:    0.0,
			TelegraphColorRGBA: "1,0.2,0.2,0.75",
			ActionWindup:       "BowDraw",
			ActionImpact:       "BowRelease",
			ActionRecover:      "Idle",
			Enabled:            true,
		},
		{
			Name:               "bow_aimedshot",
			Family:             "ranged_special",
			ResourceType:       "stamina",
			ResourceCost:       25,
			CooldownMs:         6000,
			RangeMin:           8.0,
			RangeMax:           40.0,
			WindupMs:           1200,
			ImpactDelayMs:      0,
			RecoverMs:          500,
			ParryWindowMs:      300,
			Interruptible:      true,
			BaseDamageMin:      30,
			BaseDamageMax:      45,
			TelegraphType:      "line",
			TelegraphRadius:    1.0,
			TelegraphColorRGBA: "1,0.2,0.2,0.75",
			ActionWindup:       "BowAimWindup",
			ActionImpact:       "BowAimRelease",
			ActionRecover:      "Recover",
			Enabled:            true,
		},
	}

	kits := []kitSeed{
		{
			KitKey:      "sword",
			DisplayName: "Sword",
			Description: "One-handed sword combat. Fast strikes with a heavy cleave special.",
			Enabled:     true,
		},
		{
			KitKey:      "bow",
			DisplayName: "Bow",
			Description: "Ranged combat. Quick volleys with a charged aimed shot.",
			Enabled:     true,
		},
	}

	kitAbilities := []kitAbilitySeed{
		{KitKey: "sword", AbilityName: "sword_slash", SlotIndex: 0, Enabled: true},
		{KitKey: "sword", AbilityName: "sword_cleave", SlotIndex: 1, Enabled: true},
		{KitKey: "bow", AbilityName: "bow_quickshot", SlotIndex: 0, Enabled: true},
		{KitKey: "bow", AbilityName: "bow_aimedshot", SlotIndex: 1, Enabled: true},
	}

	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("db: SeedDefaultWeaponKits begin: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	insertAbilitySQL := d.q(`
		INSERT INTO ability_templates (
			name, family, resource_type, resource_cost, cooldown_ms,
			range_min, range_max, windup_ms, impact_delay_ms, recover_ms,
			parry_window_ms, interruptible, base_damage_min, base_damage_max,
			damage_stat_scale_json, armor_pierce_pct, crit_policy_json,
			telegraph_type, telegraph_radius, telegraph_color_rgba,
			action_windup, action_impact, action_recover,
			allow_action_override, allowed_action_tags_json,
			vfx_id_windup, vfx_id_impact, sfx_id_windup, sfx_id_impact,
			vfx_path_windup, vfx_path_impact, sfx_path_windup, sfx_path_impact,
			enabled
		)
		SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?
		WHERE NOT EXISTS (SELECT 1 FROM ability_templates WHERE name = ?)
	`)
	for _, a := range abilities {
		interruptible := 0
		if a.Interruptible {
			interruptible = 1
		}
		enabled := 0
		if a.Enabled {
			enabled = 1
		}
		if _, err := tx.ExecContext(ctx, insertAbilitySQL,
			a.Name, a.Family, a.ResourceType, a.ResourceCost, a.CooldownMs,
			a.RangeMin, a.RangeMax, a.WindupMs, a.ImpactDelayMs, a.RecoverMs,
			a.ParryWindowMs, interruptible, a.BaseDamageMin, a.BaseDamageMax,
			"", 0.0, "",
			a.TelegraphType, a.TelegraphRadius, a.TelegraphColorRGBA,
			a.ActionWindup, a.ActionImpact, a.ActionRecover,
			0, "",
			0, 0, 0, 0,
			"", "", "", "",
			enabled,
			a.Name,
		); err != nil {
			return fmt.Errorf("db: SeedDefaultWeaponKits insert ability %q: %w", a.Name, err)
		}
	}

	abilityIDs := make(map[string]int, len(abilities))
	for _, a := range abilities {
		var id int
		if err := tx.QueryRowContext(ctx, d.q(`SELECT id FROM ability_templates WHERE name = ?`), a.Name).Scan(&id); err != nil {
			return fmt.Errorf("db: SeedDefaultWeaponKits lookup ability %q: %w", a.Name, err)
		}
		abilityIDs[a.Name] = id
	}

	insertKitSQL := d.q(`
		INSERT INTO weapon_kits (kit_key, display_name, description, enabled)
		SELECT ?, ?, ?, ?
		WHERE NOT EXISTS (SELECT 1 FROM weapon_kits WHERE kit_key = ?)
	`)
	for _, k := range kits {
		enabled := 0
		if k.Enabled {
			enabled = 1
		}
		if _, err := tx.ExecContext(ctx, insertKitSQL, k.KitKey, k.DisplayName, k.Description, enabled, k.KitKey); err != nil {
			return fmt.Errorf("db: SeedDefaultWeaponKits insert kit %q: %w", k.KitKey, err)
		}
	}

	kitIDs := make(map[string]int, len(kits))
	for _, k := range kits {
		var id int
		if err := tx.QueryRowContext(ctx, d.q(`SELECT id FROM weapon_kits WHERE kit_key = ?`), k.KitKey).Scan(&id); err != nil {
			return fmt.Errorf("db: SeedDefaultWeaponKits lookup kit %q: %w", k.KitKey, err)
		}
		kitIDs[k.KitKey] = id
	}

	for _, k := range kits {
		kitID := kitIDs[k.KitKey]
		if _, err := tx.ExecContext(ctx, d.q(`DELETE FROM weapon_kit_abilities WHERE kit_id = ?`), kitID); err != nil {
			return fmt.Errorf("db: SeedDefaultWeaponKits clear kit %q abilities: %w", k.KitKey, err)
		}
	}

	insertKitAbilitySQL := d.q(`INSERT INTO weapon_kit_abilities (kit_id, ability_id, slot_index, enabled) VALUES (?, ?, ?, ?)`)
	for _, entry := range kitAbilities {
		kitID, ok := kitIDs[entry.KitKey]
		if !ok || kitID <= 0 {
			return fmt.Errorf("db: SeedDefaultWeaponKits missing kit ID for %q", entry.KitKey)
		}
		abilityID, ok := abilityIDs[entry.AbilityName]
		if !ok || abilityID <= 0 {
			return fmt.Errorf("db: SeedDefaultWeaponKits missing ability ID for %q", entry.AbilityName)
		}
		enabled := 0
		if entry.Enabled {
			enabled = 1
		}
		if _, err := tx.ExecContext(ctx, insertKitAbilitySQL, kitID, abilityID, entry.SlotIndex, enabled); err != nil {
			return fmt.Errorf("db: SeedDefaultWeaponKits insert kit %q slot %d ability %q: %w",
				entry.KitKey, entry.SlotIndex, entry.AbilityName, err)
		}
	}

	if err := tx.Commit(); err != nil {
		return fmt.Errorf("db: SeedDefaultWeaponKits commit: %w", err)
	}
	log.Printf("seed: weapon kits seeded")
	return nil
}

// SeedDefaultEquipmentSlotConfig populates equipment_slot_config with default
// values for a new installation. Each row is inserted only if not already
// present, preserving admin customizations done via GUE.
//
// Defaults (engine baseline):
//   - slot 0 (weapon): gives_kit=true, hotbar_slots_granted=4
//   - slot 3 (chest): gives_kit=true, hotbar_slots_granted=1
//   - slot 7 (feet): gives_kit=true, hotbar_slots_granted=1
//   - other slots: not seeded (caller via GUE can enable)
func (d *DB) SeedDefaultEquipmentSlotConfig(ctx context.Context) error {
	type defaultEntry struct {
		slotID             int
		givesKit           bool
		hotbarSlotsGranted int
	}
	defaults := []defaultEntry{
		{slotID: 0, givesKit: true, hotbarSlotsGranted: 4}, // Weapon
		{slotID: 3, givesKit: true, hotbarSlotsGranted: 1}, // Chest
		{slotID: 7, givesKit: true, hotbarSlotsGranted: 1}, // Feet
	}

	for _, entry := range defaults {
		var exists int
		if err := d.db.QueryRowContext(ctx, d.q(`
			SELECT COUNT(*)
			  FROM equipment_slot_config
			 WHERE slot_id = ?`), entry.slotID).Scan(&exists); err != nil {
			return fmt.Errorf("db: SeedDefaultEquipmentSlotConfig check slot %d: %w", entry.slotID, err)
		}
		if exists > 0 {
			continue
		}

		givesKit := 0
		if entry.givesKit {
			givesKit = 1
		}
		if _, err := d.db.ExecContext(ctx, d.q(`
			INSERT INTO equipment_slot_config (slot_id, gives_kit, hotbar_slots_granted, enabled)
			VALUES (?, ?, ?, ?)`),
			entry.slotID, givesKit, entry.hotbarSlotsGranted, 1); err != nil {
			return fmt.Errorf("db: SeedDefaultEquipmentSlotConfig insert slot %d: %w", entry.slotID, err)
		}
	}

	log.Printf("seed: equipment slot config seeded (admin customizations preserved)")
	return nil
}

// SeedDefaultSkillProgressionConfig inserts default mastery progression config
// if no row exists yet. Existing admin-edited config is preserved.
func (d *DB) SeedDefaultSkillProgressionConfig(ctx context.Context) error {
	var count int
	if err := d.db.QueryRowContext(ctx, d.q(`
		SELECT COUNT(*)
		  FROM skill_progression_config`)).Scan(&count); err != nil {
		return fmt.Errorf("seed skill_progression_config check: %w", err)
	}
	if count > 0 {
		return nil
	}

	if _, err := d.db.ExecContext(ctx, d.q(`
		INSERT INTO skill_progression_config
		(id, xp_per_use, max_level, xp_curve_type, xp_curve_base, xp_curve_exponent, xp_irregularity,
		 damage_bonus_per_level, cooldown_redux_per_level)
		VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?)`),
		10, 10, "irregular", 40, 2.0, 0.5, 0.03, 0.01); err != nil {
		return fmt.Errorf("seed skill_progression_config insert: %w", err)
	}
	log.Printf("seed: skill progression config seeded (defaults)")
	return nil
}

// SeedDefaultCharacterProgressionConfig inserts default character progression
// config if none exists yet. Existing admin-edited config is preserved.
func (d *DB) SeedDefaultCharacterProgressionConfig(ctx context.Context) error {
	var count int
	if err := d.db.QueryRowContext(ctx, d.q(`
		SELECT COUNT(*)
		  FROM character_progression_config`)).Scan(&count); err != nil {
		return fmt.Errorf("seed character_progression_config check: %w", err)
	}
	if count > 0 {
		return nil
	}

	if _, err := d.db.ExecContext(ctx, d.q(`
		INSERT INTO character_progression_config
		(id, max_level, xp_curve_type, xp_curve_base, xp_curve_factor, xp_curve_exponent, xp_irregularity,
		 stat_points_per_level, initial_stat_value, respec_free_until_level, respec_cost_gold)
		VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`),
		60, "irregular", 60, 1.3, 2.5, 0.4, 5, 5, 10, 1000); err != nil {
		return fmt.Errorf("seed character_progression_config insert: %w", err)
	}
	log.Printf("seed: character progression config seeded (defaults)")
	return nil
}

// SeedDefaultCharacterPrimaryStatsPerLevel inserts default primary stat rows
// (levels 1..60) if table is empty. Existing rows are preserved.
func (d *DB) SeedDefaultCharacterPrimaryStatsPerLevel(ctx context.Context) error {
	var count int
	if err := d.db.QueryRowContext(ctx, d.q(`
		SELECT COUNT(*)
		  FROM character_primary_stats_per_level`)).Scan(&count); err != nil {
		return fmt.Errorf("seed character_primary_stats_per_level check: %w", err)
	}
	if count > 0 {
		return nil
	}

	for level := 1; level <= 60; level++ {
		stats := &CharacterPrimaryStatsLevel{
			Level:        level,
			Strength:     int32(level * 3),
			Dexterity:    int32(level * 3),
			Intelligence: int32(level * 3),
			Wisdom:       int32(level * 3),
			Perception:   int32(level * 3),
		}
		if err := d.UpsertCharacterPrimaryStatsLevel(ctx, stats); err != nil {
			return fmt.Errorf("seed character_primary_stats_per_level row %d: %w", level, err)
		}
	}

	log.Printf("seed: character primary stats per level seeded (defaults)")
	return nil
}

// SeedDefaultKillXPScalingConfig inserts default kill XP scaling config if
// none exists yet. Existing admin-edited config is preserved.
func (d *DB) SeedDefaultKillXPScalingConfig(ctx context.Context) error {
	var count int
	if err := d.db.QueryRowContext(ctx, d.q(`
		SELECT COUNT(*)
		  FROM kill_xp_scaling_config`)).Scan(&count); err != nil {
		return fmt.Errorf("seed kill_xp_scaling_config check: %w", err)
	}
	if count > 0 {
		return nil
	}

	if _, err := d.db.ExecContext(ctx, d.q(`
		INSERT INTO kill_xp_scaling_config
		(id, base_xp_per_npc_level, level_diff_coefficient, multiplier_min, multiplier_max,
		 mastery_xp_per_mob_level, mastery_killing_blow_mult, mastery_window_timeout_ms)
		VALUES (1, ?, ?, ?, ?, ?, ?, ?)`),
		25, 0.1, 0.1, 1.5, 10, 1.5, 10000); err != nil {
		return fmt.Errorf("seed kill_xp_scaling_config insert: %w", err)
	}
	log.Printf("seed: kill xp scaling config seeded (defaults)")
	return nil
}

// GiveStarterItems gives a newly created character their starting items if
// they have no items yet.
func (d *DB) GiveStarterItems(ctx context.Context, charID string) error {
	var count int
	if err := d.db.QueryRowContext(ctx,
		d.q(`SELECT COUNT(*) FROM character_items WHERE character_id = ?`), charID,
	).Scan(&count); err != nil {
		return fmt.Errorf("db: GiveStarterItems count: %w", err)
	}
	if count > 0 {
		return nil // already has items
	}

	// Slot 0 = weapon, slot 15 = first backpack slot (consumable)
	starters := []struct {
		slot     int
		itemName string
		qty      int
	}{
		{0, "Rusty Sword", 1},
		{15, "Health Potion", 3},
	}
	for _, s := range starters {
		var itemID int
		if err := d.db.QueryRowContext(ctx,
			d.q(`SELECT id FROM item_templates WHERE name = ?`), s.itemName,
		).Scan(&itemID); err != nil {
			continue // item template not seeded yet Ã¢â‚¬â€ skip
		}
		insertSQL := `INSERT OR IGNORE INTO character_items (character_id, slot, item_id, quantity, durability) VALUES (?, ?, ?, ?, 100)`
		if d.driver == "postgres" {
			insertSQL = `INSERT INTO character_items (character_id, slot, item_id, quantity, durability) VALUES ($1, $2, $3, $4, 100) ON CONFLICT DO NOTHING`
		}
		_, _ = d.db.ExecContext(ctx, insertSQL, charID, s.slot, itemID, s.qty)
	}
	return nil
}

// GetEquippedStats returns the weapon damage of the highest-damage weapon,
// the sum of armor levels from all equipment slots (0-13), and the
// weapon_dimension/weapon_range of the item equipped in the weapon slot (slot 0).
// weaponDimension is a world.CombatDimension value (0=melee, 1=ranged,
// 2=magic) and is 0 (melee) if no weapon is equipped in slot 0.
// weaponRange is the weapon's explicit attack range (0 if unset or no weapon
// equipped); callers should resolve it via world.ResolveAttackRange.
func (d *DB) GetEquippedStats(ctx context.Context, charID string) (weaponDamage, armorLevel, weaponDimension int32, weaponRange float32, err error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT it.weapon_damage, it.armor_level
		     FROM character_items ci
		     JOIN item_templates it ON it.id = ci.item_id
		     WHERE ci.character_id = ? AND ci.slot < 14`),
		charID,
	)
	if err != nil {
		return 0, 0, 0, 0, fmt.Errorf("db: GetEquippedStats: %w", err)
	}
	defer rows.Close()
	for rows.Next() {
		var wd, al int32
		if err := rows.Scan(&wd, &al); err != nil {
			continue
		}
		if wd > weaponDamage {
			weaponDamage = wd
		}
		armorLevel += al
	}
	if err := rows.Err(); err != nil {
		return weaponDamage, armorLevel, 0, 0, err
	}

	wdRow := d.db.QueryRowContext(ctx,
		d.q(`SELECT it.weapon_dimension, it.weapon_range
		     FROM character_items ci
		     JOIN item_templates it ON it.id = ci.item_id
		     WHERE ci.character_id = ? AND ci.slot = 0`),
		charID,
	)
	if scanErr := wdRow.Scan(&weaponDimension, &weaponRange); scanErr != nil {
		weaponDimension = 0
		weaponRange = 0
	}

	return weaponDamage, armorLevel, weaponDimension, weaponRange, nil
}

// GetEquippedAttributes aggregates item_attributes from all equipped items
// (slot 0-13) into a single bonus map, summing values when multiple equipped
// items grant the same attribute key (e.g. two rings each granting +str).
// Returns an empty (non-nil) map if no equipped item has attributes.
func (d *DB) GetEquippedAttributes(ctx context.Context, charID string) (map[string]float64, error) {
	bonuses := make(map[string]float64)

	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT ia.attribute_key, ia.value
		     FROM character_items ci
		     JOIN item_attributes ia ON ia.item_id = ci.item_id
		     WHERE ci.character_id = ? AND ci.slot < 14`),
		charID,
	)
	if err != nil {
		return nil, fmt.Errorf("db: GetEquippedAttributes: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var key string
		var value float64
		if err := rows.Scan(&key, &value); err != nil {
			continue
		}
		bonuses[key] += value
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("db: GetEquippedAttributes: %w", err)
	}

	return bonuses, nil
}

// resolveSlotAbilities returns abilities for one equipped kit contribution.
// It first applies the character's personal loadout for the kit; if there is
// no personal loadout yet, it falls back to the kit default pool (first N by
// weapon_kit_abilities.slot_index). Disabled/missing abilities in an existing
// personal loadout are skipped silently.
func (d *DB) resolveSlotAbilities(ctx context.Context, charID string, kitID, limit int) ([]PlayerKitAbilityEntry, error) {
	if limit <= 0 {
		return nil, nil
	}

	loadout, err := d.ListLoadoutForCharKit(ctx, charID, kitID)
	if err != nil {
		return nil, fmt.Errorf("db: resolveSlotAbilities loadout: %w", err)
	}

	if len(loadout) > 0 {
		out := make([]PlayerKitAbilityEntry, 0, limit)
		for _, entry := range loadout {
			if entry.SlotIndex < 0 {
				continue
			}
			if entry.SlotIndex >= limit {
				break
			}

			var (
				name      string
				cooldown  int64
				isEnabled int
			)
			err := d.db.QueryRowContext(ctx, d.q(`
				SELECT name, cooldown_ms, CASE WHEN enabled THEN 1 ELSE 0 END AS enabled
				  FROM ability_templates
				 WHERE id = ?`), entry.AbilityID).Scan(&name, &cooldown, &isEnabled)
			if err == sql.ErrNoRows {
				continue
			}
			if err != nil {
				return nil, fmt.Errorf("db: resolveSlotAbilities ability lookup id=%d: %w", entry.AbilityID, err)
			}
			if isEnabled == 0 {
				continue
			}

			out = append(out, PlayerKitAbilityEntry{
				SlotIndex:   entry.SlotIndex,
				AbilityID:   entry.AbilityID,
				AbilityName: name,
				CooldownMs:  cooldown,
			})
		}
		return out, nil
	}

	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT wka.slot_index, wka.ability_id, at.name, at.cooldown_ms
		  FROM weapon_kit_abilities wka
		  JOIN ability_templates at ON at.id = wka.ability_id
		 WHERE wka.kit_id = ?
		   AND wka.enabled = 1
		   AND at.enabled = 1
		 ORDER BY wka.slot_index
		 LIMIT ?`), kitID, limit)
	if err != nil {
		return nil, fmt.Errorf("db: resolveSlotAbilities default pool: %w", err)
	}
	defer rows.Close()

	out := make([]PlayerKitAbilityEntry, 0, limit)
	for rows.Next() {
		var entry PlayerKitAbilityEntry
		if err := rows.Scan(&entry.SlotIndex, &entry.AbilityID, &entry.AbilityName, &entry.CooldownMs); err != nil {
			return nil, fmt.Errorf("db: resolveSlotAbilities default pool scan: %w", err)
		}
		out = append(out, entry)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("db: resolveSlotAbilities default pool rows: %w", err)
	}
	return out, nil
}

// ResolveActivePlayerKit resolves the player's active kit identity and aggregates
// abilities from all equipped slots that are configured to grant kits.
//
// Identity (KitKey, KitDisplayName) comes from slot 0 (weapon) when available;
// otherwise it falls back to the first contributing slot by slot_id order.
//
// Abilities are aggregated by slot order (weapon first, then chest/feet/etc),
// respecting each slot's configured hotbar_slots_granted. Ability slot indexes
// are reindexed globally for the player's hotbar (0..N-1).
//
// For each contributing kit, this function first applies the character's
// personal loadout (if present) and falls back to default pool ordering
// (first N abilities) when personal loadout does not exist yet.
//
// Returns zero-value (HasKit=false) when no active kit applies.
func (d *DB) ResolveActivePlayerKit(ctx context.Context, charID string) (PlayerKitResolution, error) {
	var out PlayerKitResolution
	charID = strings.TrimSpace(charID)
	if charID == "" {
		return out, nil
	}

	type slotGrantConfig struct {
		SlotID             int
		HotbarSlotsGranted int
	}
	slotRows, err := d.db.QueryContext(ctx, d.q(`
		SELECT slot_id, hotbar_slots_granted
		  FROM equipment_slot_config
		 WHERE gives_kit = 1 AND enabled = 1
		 ORDER BY slot_id`))
	if err != nil {
		return out, fmt.Errorf("db: ResolveActivePlayerKit slot config: %w", err)
	}
	defer slotRows.Close()

	var slotConfigs []slotGrantConfig
	for slotRows.Next() {
		var cfg slotGrantConfig
		if err := slotRows.Scan(&cfg.SlotID, &cfg.HotbarSlotsGranted); err != nil {
			return out, fmt.Errorf("db: ResolveActivePlayerKit scan slot config: %w", err)
		}
		slotConfigs = append(slotConfigs, cfg)
	}
	if err := slotRows.Err(); err != nil {
		return out, fmt.Errorf("db: ResolveActivePlayerKit slot config rows: %w", err)
	}
	if len(slotConfigs) == 0 {
		return out, nil
	}

	type equippedKitRow struct {
		SlotID         int
		ItemKitKey     string
		KitID          int
		KitKey         string
		KitDisplayName string
		ValidKit       bool
	}
	equippedBySlot := map[int]equippedKitRow{}
	equippedRows, err := d.db.QueryContext(ctx, d.q(`
		SELECT ci.slot,
		       TRIM(it.weapon_kit) AS item_kit_key,
		       wk.id,
		       wk.kit_key,
		       wk.display_name
		  FROM character_items ci
		  JOIN item_templates it ON it.id = ci.item_id
		  LEFT JOIN weapon_kits wk
		    ON wk.kit_key = TRIM(it.weapon_kit)
		   AND wk.enabled = 1
		 WHERE ci.character_id = ?
		   AND ci.slot < 14
		   AND TRIM(it.weapon_kit) <> ''`), charID)
	if err != nil {
		return out, fmt.Errorf("db: ResolveActivePlayerKit equipped query: %w", err)
	}
	defer equippedRows.Close()

	for equippedRows.Next() {
		var row equippedKitRow
		var kitID sql.NullInt64
		var kitKey sql.NullString
		var kitDisplayName sql.NullString
		if err := equippedRows.Scan(&row.SlotID, &row.ItemKitKey, &kitID, &kitKey, &kitDisplayName); err != nil {
			return out, fmt.Errorf("db: ResolveActivePlayerKit scan equipped: %w", err)
		}
		row.ItemKitKey = strings.TrimSpace(row.ItemKitKey)
		if kitID.Valid {
			row.ValidKit = true
			row.KitID = int(kitID.Int64)
			if kitKey.Valid {
				row.KitKey = kitKey.String
			}
			if kitDisplayName.Valid {
				row.KitDisplayName = kitDisplayName.String
			}
		}
		equippedBySlot[row.SlotID] = row
	}
	if err := equippedRows.Err(); err != nil {
		return out, fmt.Errorf("db: ResolveActivePlayerKit equipped rows: %w", err)
	}

	var (
		identitySet         bool
		weaponIdentity      equippedKitRow
		fallbackIdentity    equippedKitRow
		hasFallbackIdentity bool
		globalSlotIndex     int
	)

	for _, cfg := range slotConfigs {
		eq, ok := equippedBySlot[cfg.SlotID]
		if !ok {
			continue
		}
		if !eq.ValidKit {
			log.Printf("warn: character %s has item in slot %d with weapon_kit=%q but kit not found or disabled", charID, cfg.SlotID, eq.ItemKitKey)
			continue
		}

		if cfg.SlotID == 0 {
			weaponIdentity = eq
			identitySet = true
		} else if !hasFallbackIdentity {
			fallbackIdentity = eq
			hasFallbackIdentity = true
		}

		if cfg.HotbarSlotsGranted <= 0 {
			continue
		}

		slotAbilities, err := d.resolveSlotAbilities(ctx, charID, eq.KitID, cfg.HotbarSlotsGranted)
		if err != nil {
			return out, fmt.Errorf("db: ResolveActivePlayerKit abilities for slot %d: %w", cfg.SlotID, err)
		}

		for _, entry := range slotAbilities {
			entry.SlotIndex = globalSlotIndex
			globalSlotIndex++
			out.Abilities = append(out.Abilities, entry)
		}
	}

	switch {
	case identitySet:
		out.HasKit = true
		out.KitID = weaponIdentity.KitID
		out.KitKey = weaponIdentity.KitKey
		out.KitDisplayName = weaponIdentity.KitDisplayName
	case hasFallbackIdentity:
		out.HasKit = true
		out.KitID = fallbackIdentity.KitID
		out.KitKey = fallbackIdentity.KitKey
		out.KitDisplayName = fallbackIdentity.KitDisplayName
	}

	return out, nil
}

// ResolveActivePlayerAbilities returns active ability IDs ordered by slot_index.
// Returns empty slice when the character has no active kit.
func (d *DB) ResolveActivePlayerAbilities(ctx context.Context, charID string) ([]int, error) {
	res, err := d.ResolveActivePlayerKit(ctx, charID)
	if err != nil {
		return nil, err
	}
	if !res.HasKit || len(res.Abilities) == 0 {
		return []int{}, nil
	}
	out := make([]int, 0, len(res.Abilities))
	for _, e := range res.Abilities {
		out = append(out, e.AbilityID)
	}
	return out, nil
}

// GetInventory returns all items for a character.
func (d *DB) GetInventory(ctx context.Context, charID string) ([]*CharacterItem, error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT ci.slot, ci.item_id, ci.quantity, ci.durability,
		          it.name, it.item_type, it.slot_type, it.weapon_damage, it.armor_level, it.model_path, it.model_scale, it.socket_name
		     FROM character_items ci
		     JOIN item_templates it ON it.id = ci.item_id
		     WHERE ci.character_id = ?
		     ORDER BY ci.slot`),
		charID,
	)
	if err != nil {
		return nil, fmt.Errorf("db: GetInventory: %w", err)
	}
	defer rows.Close()

	var items []*CharacterItem
	for rows.Next() {
		ci := &CharacterItem{}
		if err := rows.Scan(
			&ci.Slot, &ci.ItemID, &ci.Quantity, &ci.Durability,
			&ci.Name, &ci.ItemType, &ci.SlotType, &ci.WeaponDamage, &ci.ArmorLevel,
			&ci.ModelPath, &ci.ModelScale, &ci.SocketName,
		); err != nil {
			return nil, fmt.Errorf("db: GetInventory scan: %w", err)
		}
		items = append(items, ci)
	}
	return items, rows.Err()
}

// slotTypeMatches returns true if an item with the given slotType can be placed
// in the given equipment slot index (0-13).
func slotTypeMatches(equipSlot uint8, itemSlotType uint8) bool {
	switch itemSlotType {
	case 0, 1, 2, 3, 4, 5, 6, 7:
		return equipSlot == itemSlotType
	case 8:
		return equipSlot >= 8 && equipSlot <= 11
	case 9:
		return equipSlot == 12 || equipSlot == 13
	}
	return false
}

// SwapInventorySlots swaps two inventory slots for a character.
// Validates slot-type compatibility when moving into an equipment slot.
func (d *DB) SwapInventorySlots(ctx context.Context, charID string, slotA, slotB uint8) error {
	const maxSlot = 45
	if slotA > maxSlot || slotB > maxSlot || slotA == slotB {
		return fmt.Errorf("db: SwapInventorySlots: invalid slots %d/%d", slotA, slotB)
	}

	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("db: SwapInventorySlots begin tx: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	type row struct {
		itemID     int
		quantity   int
		durability int
		slotType   int
	}
	readSlot := func(slot uint8) (*row, error) {
		var r row
		err := tx.QueryRowContext(ctx,
			d.q(`SELECT ci.item_id, ci.quantity, ci.durability, it.slot_type
			     FROM character_items ci
			     JOIN item_templates it ON it.id = ci.item_id
			     WHERE ci.character_id = ? AND ci.slot = ?`),
			charID, slot,
		).Scan(&r.itemID, &r.quantity, &r.durability, &r.slotType)
		if err == sql.ErrNoRows {
			return nil, nil
		}
		return &r, err
	}

	itemA, err := readSlot(slotA)
	if err != nil {
		return fmt.Errorf("db: SwapInventorySlots read A: %w", err)
	}
	itemB, err := readSlot(slotB)
	if err != nil {
		return fmt.Errorf("db: SwapInventorySlots read B: %w", err)
	}

	// Validate equip-slot compatibility.
	if slotA < 14 && itemB != nil && !slotTypeMatches(slotA, uint8(itemB.slotType)) {
		return fmt.Errorf("db: SwapInventorySlots: item doesn't fit slot %d", slotA)
	}
	if slotB < 14 && itemA != nil && !slotTypeMatches(slotB, uint8(itemA.slotType)) {
		return fmt.Errorf("db: SwapInventorySlots: item doesn't fit slot %d", slotB)
	}

	del := d.q(`DELETE FROM character_items WHERE character_id = ? AND slot = ?`)
	ins := d.q(`INSERT INTO character_items (character_id, slot, item_id, quantity, durability) VALUES (?, ?, ?, ?, ?)`)

	if _, err := tx.ExecContext(ctx, del, charID, slotA); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx, del, charID, slotB); err != nil {
		return err
	}
	if itemB != nil {
		if _, err := tx.ExecContext(ctx, ins, charID, slotA, itemB.itemID, itemB.quantity, itemB.durability); err != nil {
			return err
		}
	}
	if itemA != nil {
		if _, err := tx.ExecContext(ctx, ins, charID, slotB, itemA.itemID, itemA.quantity, itemA.durability); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// SaveXP persists updated XP and level.
func (d *DB) SaveXP(ctx context.Context, charID string, xp int64, level int) error {
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE characters SET xp = ?, level = ? WHERE id = ?`),
		xp, level, charID)
	return err
}

// UseItemResult is the outcome of a UseItem call.
type UseItemResult struct {
	ItemType  uint8
	HealAmt   int32 // > 0 when a consumable was used
	EquipSlot uint8 // 0xFF when no equip change happened
}

// UseItem processes a right-click "use" on the given inventory slot:
//   - Consumable (item_type == 2): removes one from stack, returns heal amount.
//   - Equippable from bag (slot_type < 10, slot >= 14): moves to best equip slot.
func (d *DB) UseItem(ctx context.Context, charID string, slot uint8) (*UseItemResult, error) {
	var quantity int
	res := &UseItemResult{EquipSlot: 0xFF}
	var slotType uint8
	err := d.db.QueryRowContext(ctx,
		d.q(`SELECT ci.quantity, it.item_type, it.slot_type, it.item_value
		     FROM character_items ci
		     JOIN item_templates it ON it.id = ci.item_id
		     WHERE ci.character_id = ? AND ci.slot = ?`),
		charID, slot,
	).Scan(&quantity, &res.ItemType, &slotType, &res.HealAmt)
	if err == sql.ErrNoRows {
		return nil, fmt.Errorf("db: UseItem: no item at slot %d", slot)
	}
	if err != nil {
		return nil, fmt.Errorf("db: UseItem: %w", err)
	}

	switch {
	case res.ItemType == 2: // consumable Ã¢â‚¬â€ reduce stack
		if quantity > 1 {
			_, err = d.db.ExecContext(ctx,
				d.q(`UPDATE character_items SET quantity = quantity - 1
				     WHERE character_id = ? AND slot = ?`),
				charID, slot)
		} else {
			_, err = d.db.ExecContext(ctx,
				d.q(`DELETE FROM character_items WHERE character_id = ? AND slot = ?`),
				charID, slot)
		}
		if err != nil {
			return nil, fmt.Errorf("db: UseItem consume: %w", err)
		}

	case slotType < 10 && slot >= 14: // equippable from bag Ã¢â‚¬â€ auto-equip
		target := d.findEquipSlotFor(ctx, charID, slotType)
		if err := d.SwapInventorySlots(ctx, charID, slot, target); err != nil {
			return nil, err
		}
		res.EquipSlot = target
		res.HealAmt = 0

	default:
		return nil, fmt.Errorf("db: UseItem: slot %d is not usable", slot)
	}

	return res, nil
}

// ---------------------------------------------------------------------------
// Spell methods
// ---------------------------------------------------------------------------

// SpellRow holds one row from spell_templates.
type SpellRow struct {
	ID               uint16
	Name             string
	SpellType        int
	DamageMin        int32
	DamageMax        int32
	EPCost           int32
	CooldownMs       int64
	Range            float32
	Icon             uint8
	AoEType          uint8   // 0=single 1=around_target 2=ground_target
	AoERadius        float32 // world units; 0 = not AoE
	RuntimeAbilityID int     // 0 = legacy script-spell path, >0 = cast_intent ability ID
}

// AbilityTemplateRow mirrors one row in ability_templates.
type AbilityTemplateRow struct {
	ID                         int
	Name                       string
	Description                string
	Family                     string
	Category                   string
	Dimension                  string
	ResourceType               string
	ResourceCost               int32
	CooldownMs                 int64
	RangeMin                   float32
	RangeMax                   float32
	WindupMs                   int64
	ImpactDelayMs              int64
	RecoverMs                  int64
	ParryWindowMs              int64
	Interruptible              bool
	BaseDamageMin              int32
	BaseDamageMax              int32
	DamageStatScaleJSON        string
	ArmorPiercePct             float32
	CritPolicyJSON             string
	TelegraphType              string
	TelegraphRadius            float32
	TelegraphColorRGBA         string
	ActionWindup               string
	ActionImpact               string
	ActionRecover              string
	AllowActionOverride        bool
	AllowedActionTagsJSON      string
	VFXIDWindup                int
	VFXIDImpact                int
	SFXIDWindup                int
	SFXIDImpact                int
	VFXPathWindup              string
	VFXPathImpact              string
	SFXPathWindup              string
	SFXPathImpact              string
	MasteryXPPerUse            int
	MasteryMaxLevel            int
	MasteryXPCurveType         string
	MasteryXPCurveBase         int
	MasteryXPCurveExponent     float64
	MasteryXPIrregularity      float64
	MasteryPrimaryBonusPerLvl  float64
	MasteryCooldownReduxPerLvl float64
	Enabled                    bool
}

// NPCAbilityLoadoutRow mirrors one row in npc_ability_loadouts.
type NPCAbilityLoadoutRow struct {
	ID             int
	NPCSpawnID     int
	ActorDefID     int
	AbilityID      int
	Priority       int
	Weight         int
	MinDistance    float32
	MaxDistance    float32
	MinTargetHPPct float32
	MaxTargetHPPct float32
	PhaseTag       string
	ConditionLua   string
	Enabled        bool
}

// NPCCombatProfileRow mirrors one row in npc_combat_profiles.
type NPCCombatProfileRow struct {
	ID                     int
	Name                   string
	GlobalGCDMs            int64
	DecisionTickMs         int64
	AggroStyle             string
	AllowChainCast         bool
	MaxConsecutiveSpecials int
	Enabled                bool
}

// NPCProfileBindingRow mirrors one row in npc_profile_bindings.
type NPCProfileBindingRow struct {
	ID         int
	NPCSpawnID int
	ActorDefID int
	ProfileID  int
	Enabled    bool
}

// LoadSpells returns all spell templates.
func (d *DB) LoadSpells(ctx context.Context) ([]SpellRow, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, name, spell_type, damage_min, damage_max, ep_cost, cooldown_ms, range, icon, aoe_type, aoe_radius, runtime_ability_id
		 FROM spell_templates ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []SpellRow
	for rows.Next() {
		var r SpellRow
		if err := rows.Scan(&r.ID, &r.Name, &r.SpellType, &r.DamageMin, &r.DamageMax,
			&r.EPCost, &r.CooldownMs, &r.Range, &r.Icon, &r.AoEType, &r.AoERadius, &r.RuntimeAbilityID); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

// LoadAbilityTemplates returns all ability templates ordered by id.
func (d *DB) LoadAbilityTemplates(ctx context.Context) ([]AbilityTemplateRow, error) {
	rows, err := d.db.QueryContext(ctx, `
		SELECT id, name, description, family, category, dimension, resource_type, resource_cost, cooldown_ms,
		       range_min, range_max, windup_ms, impact_delay_ms, recover_ms,
		       parry_window_ms, interruptible, base_damage_min, base_damage_max,
		       damage_stat_scale_json, armor_pierce_pct, crit_policy_json,
		       telegraph_type, telegraph_radius, telegraph_color_rgba,
		       action_windup, action_impact, action_recover,
		       allow_action_override, allowed_action_tags_json,
		       vfx_id_windup, vfx_id_impact, sfx_id_windup, sfx_id_impact,
		       vfx_path_windup, vfx_path_impact, sfx_path_windup, sfx_path_impact,
		       mastery_xp_per_use, mastery_max_level, mastery_xp_curve_type,
		       mastery_xp_curve_base, mastery_xp_curve_exponent, mastery_xp_irregularity,
		       mastery_primary_bonus_per_lvl,
		       mastery_cooldown_redux_per_lvl, enabled
		  FROM ability_templates
		 ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("db: LoadAbilityTemplates: %w", err)
	}
	defer rows.Close()

	var out []AbilityTemplateRow
	for rows.Next() {
		var r AbilityTemplateRow
		var interruptibleRaw interface{}
		var allowOverrideRaw interface{}
		var enabledRaw interface{}
		if err := rows.Scan(
			&r.ID, &r.Name, &r.Description, &r.Family, &r.Category, &r.Dimension, &r.ResourceType, &r.ResourceCost, &r.CooldownMs,
			&r.RangeMin, &r.RangeMax, &r.WindupMs, &r.ImpactDelayMs, &r.RecoverMs,
			&r.ParryWindowMs, &interruptibleRaw, &r.BaseDamageMin, &r.BaseDamageMax,
			&r.DamageStatScaleJSON, &r.ArmorPiercePct, &r.CritPolicyJSON,
			&r.TelegraphType, &r.TelegraphRadius, &r.TelegraphColorRGBA,
			&r.ActionWindup, &r.ActionImpact, &r.ActionRecover,
			&allowOverrideRaw, &r.AllowedActionTagsJSON,
			&r.VFXIDWindup, &r.VFXIDImpact, &r.SFXIDWindup, &r.SFXIDImpact,
			&r.VFXPathWindup, &r.VFXPathImpact, &r.SFXPathWindup, &r.SFXPathImpact,
			&r.MasteryXPPerUse, &r.MasteryMaxLevel, &r.MasteryXPCurveType,
			&r.MasteryXPCurveBase, &r.MasteryXPCurveExponent, &r.MasteryXPIrregularity,
			&r.MasteryPrimaryBonusPerLvl,
			&r.MasteryCooldownReduxPerLvl,
			&enabledRaw,
		); err != nil {
			return nil, fmt.Errorf("db: LoadAbilityTemplates scan: %w", err)
		}
		r.Interruptible = boolFromDB(interruptibleRaw)
		r.AllowActionOverride = boolFromDB(allowOverrideRaw)
		r.Enabled = boolFromDB(enabledRaw)
		out = append(out, r)
	}
	return out, rows.Err()
}

// LoadNPCAbilityLoadouts returns enabled/disabled NPC ability loadout rows.
func (d *DB) LoadNPCAbilityLoadouts(ctx context.Context) ([]NPCAbilityLoadoutRow, error) {
	rows, err := d.db.QueryContext(ctx, `
		SELECT id, npc_spawn_id, actor_def_id, ability_id, priority, weight,
		       min_distance, max_distance, min_target_hp_pct, max_target_hp_pct,
		       phase_tag, condition_lua, enabled
		  FROM npc_ability_loadouts
		 ORDER BY priority DESC, id`)
	if err != nil {
		return nil, fmt.Errorf("db: LoadNPCAbilityLoadouts: %w", err)
	}
	defer rows.Close()

	var out []NPCAbilityLoadoutRow
	for rows.Next() {
		var r NPCAbilityLoadoutRow
		var enabledRaw interface{}
		if err := rows.Scan(
			&r.ID, &r.NPCSpawnID, &r.ActorDefID, &r.AbilityID, &r.Priority, &r.Weight,
			&r.MinDistance, &r.MaxDistance, &r.MinTargetHPPct, &r.MaxTargetHPPct,
			&r.PhaseTag, &r.ConditionLua, &enabledRaw,
		); err != nil {
			return nil, fmt.Errorf("db: LoadNPCAbilityLoadouts scan: %w", err)
		}
		r.Enabled = boolFromDB(enabledRaw)
		out = append(out, r)
	}
	return out, rows.Err()
}

// LoadNPCCombatProfiles returns enabled/disabled NPC combat profile rows.
func (d *DB) LoadNPCCombatProfiles(ctx context.Context) ([]NPCCombatProfileRow, error) {
	rows, err := d.db.QueryContext(ctx, `
		SELECT id, name, global_gcd_ms, decision_tick_ms, aggro_style,
		       allow_chain_cast, max_consecutive_specials, enabled
		  FROM npc_combat_profiles
		 ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("db: LoadNPCCombatProfiles: %w", err)
	}
	defer rows.Close()

	var out []NPCCombatProfileRow
	for rows.Next() {
		var r NPCCombatProfileRow
		var allowChainRaw interface{}
		var enabledRaw interface{}
		if err := rows.Scan(
			&r.ID, &r.Name, &r.GlobalGCDMs, &r.DecisionTickMs, &r.AggroStyle,
			&allowChainRaw, &r.MaxConsecutiveSpecials, &enabledRaw,
		); err != nil {
			return nil, fmt.Errorf("db: LoadNPCCombatProfiles scan: %w", err)
		}
		r.AllowChainCast = boolFromDB(allowChainRaw)
		r.Enabled = boolFromDB(enabledRaw)
		out = append(out, r)
	}
	return out, rows.Err()
}

// LoadNPCProfileBindings returns enabled/disabled profile binding rows.
func (d *DB) LoadNPCProfileBindings(ctx context.Context) ([]NPCProfileBindingRow, error) {
	rows, err := d.db.QueryContext(ctx, `
		SELECT id, npc_spawn_id, actor_def_id, profile_id, enabled
		  FROM npc_profile_bindings
		 ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("db: LoadNPCProfileBindings: %w", err)
	}
	defer rows.Close()

	var out []NPCProfileBindingRow
	for rows.Next() {
		var r NPCProfileBindingRow
		var enabledRaw interface{}
		if err := rows.Scan(
			&r.ID, &r.NPCSpawnID, &r.ActorDefID, &r.ProfileID, &enabledRaw,
		); err != nil {
			return nil, fmt.Errorf("db: LoadNPCProfileBindings scan: %w", err)
		}
		r.Enabled = boolFromDB(enabledRaw)
		out = append(out, r)
	}
	return out, rows.Err()
}

// NpcSpawn mirrors one row in npc_spawns.
type NpcSpawn struct {
	ID               int
	Name             string
	Race             string
	Class            string
	Level            int
	AreaName         string
	X, Y, Z, Yaw     float32
	Aggressiveness   int
	AggressiveRange  float32
	AttackRange      float32
	RespawnDelayMs   int64
	ActorDefID       int // FK Ã¢â€ â€™ media_actor_defs.id (0 = unset)
	StartWaypointID  int // FK Ã¢â€ â€™ area_waypoints.id (0 = no patrol)
	WanderRadius     float32
	WanderPauseMinMs int
	WanderPauseMaxMs int
}

// WorldObject is one placed static model instance in a zone.
// ModelPath is the resolved file_path from media_models (relative to dist/client/).
type WorldObject struct {
	ID        int
	AreaName  string
	ModelPath string
	Scale     float32
	X, Y, Z   float32
	Yaw       float32
}

// Waypoint mirrors one row in area_waypoints.
type Waypoint struct {
	ID       int
	AreaName string
	X, Y, Z  float32
	NextA    int // ID of next waypoint (0 = end of path)
	NextB    int // ID of alternate branch (0 = no branch)
	PauseMs  int // ms to pause at this node before moving on
}

// migrateV5 adds AoE columns to spell_templates.
func (d *DB) migrateV5(ctx context.Context) {
	if d.driver == "postgres" {
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE spell_templates ADD COLUMN IF NOT EXISTS aoe_type   SMALLINT NOT NULL DEFAULT 0`)
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE spell_templates ADD COLUMN IF NOT EXISTS aoe_radius REAL    NOT NULL DEFAULT 0`)
	} else {
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE spell_templates ADD COLUMN aoe_type   INTEGER NOT NULL DEFAULT 0`)
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE spell_templates ADD COLUMN aoe_radius REAL    NOT NULL DEFAULT 0`)
	}
}

// migrateV6 creates the npc_spawns table and seeds default NPC spawn points.
func (d *DB) migrateV6(ctx context.Context) {
	if d.driver == "postgres" {
		_, _ = d.db.ExecContext(ctx, `
			CREATE TABLE IF NOT EXISTS npc_spawns (
				id               SERIAL PRIMARY KEY,
				name             VARCHAR(64)  NOT NULL DEFAULT 'NPC',
				race             VARCHAR(32)  NOT NULL DEFAULT 'Human',
				class            VARCHAR(32)  NOT NULL DEFAULT 'Warrior',
				level            SMALLINT     NOT NULL DEFAULT 1,
				area_name        VARCHAR(64)  NOT NULL DEFAULT 'Starter Zone',
				x                REAL         NOT NULL DEFAULT 0,
				y                REAL         NOT NULL DEFAULT 0,
				z                REAL         NOT NULL DEFAULT 0,
				yaw              REAL         NOT NULL DEFAULT 0,
				aggressiveness   SMALLINT     NOT NULL DEFAULT 0,
				aggressive_range REAL         NOT NULL DEFAULT 8.0,
				attack_range     REAL         NOT NULL DEFAULT 2.0,
				respawn_delay_ms BIGINT       NOT NULL DEFAULT 30000
			)`)
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE npc_spawns ADD COLUMN IF NOT EXISTS attack_range REAL NOT NULL DEFAULT 2.0`)
	} else {
		_, _ = d.db.ExecContext(ctx, `
			CREATE TABLE IF NOT EXISTS npc_spawns (
				id               INTEGER PRIMARY KEY AUTOINCREMENT,
				name             TEXT    NOT NULL DEFAULT 'NPC',
				race             TEXT    NOT NULL DEFAULT 'Human',
				class            TEXT    NOT NULL DEFAULT 'Warrior',
				level            INTEGER NOT NULL DEFAULT 1,
				area_name        TEXT    NOT NULL DEFAULT 'Starter Zone',
				x                REAL    NOT NULL DEFAULT 0,
				y                REAL    NOT NULL DEFAULT 0,
				z                REAL    NOT NULL DEFAULT 0,
				yaw              REAL    NOT NULL DEFAULT 0,
				aggressiveness   INTEGER NOT NULL DEFAULT 0,
				aggressive_range REAL    NOT NULL DEFAULT 8.0,
				attack_range     REAL    NOT NULL DEFAULT 2.0,
				respawn_delay_ms INTEGER NOT NULL DEFAULT 30000
			)`)
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE npc_spawns ADD COLUMN attack_range REAL NOT NULL DEFAULT 2.0`)
	}

	// Seed defaults only if the table is empty.
	var count int
	_ = d.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM npc_spawns`).Scan(&count)
	if count > 0 {
		return
	}

	type seed struct {
		name                  string
		race, class           string
		level                 int
		area                  string
		x, y, z, yaw          float64
		agg                   int
		aggRange, attackRange float64
		respawnMs             int64
	}
	// Coords are 0-indexed to match the GUE / client (terrain [0, W*cs]).
	// Starter Zone = 1024Ãƒâ€”1024 Ã¢â€ â€™ center is (512, 512); seeds cluster there.
	seeds := []seed{
		// Starter Zone Ã¢â‚¬â€ dialog NPCs (aggressiveness=3)
		{"Guard", "Human", "Warrior", 5, "Starter Zone", 517, 0, 512, 0, 3, 8, 2, 30000},
		{"Merchant", "Elf", "Mage", 3, "Starter Zone", 504, 0, 515, 180, 3, 8, 2, 30000},
		{"Innkeeper", "Dwarf", "Warrior", 10, "Starter Zone", 524, 0, 507, 270, 3, 8, 2, 30000},
		// Starter Zone Ã¢â‚¬â€ combat mobs (aggressiveness=2)
		{"Goblin", "Beast", "Warrior", 2, "Starter Zone", 527, 0, 520, 0, 2, 8, 2, 30000},
		{"Goblin", "Beast", "Warrior", 2, "Starter Zone", 532, 0, 506, 90, 2, 8, 2, 30000},
		{"Goblin Scout", "Beast", "Rogue", 3, "Starter Zone", 522, 0, 530, 180, 2, 8, 2, 30000},
		{"Slime", "Beast", "Beast", 1, "Starter Zone", 497, 0, 522, 0, 2, 8, 2, 30000},
		{"Slime", "Beast", "Beast", 1, "Starter Zone", 494, 0, 508, 0, 2, 8, 2, 30000},
		// Forest Ã¢â‚¬â€ combat mobs
		{"Wolf", "Beast", "Beast", 4, "Forest", 520, 0, 524, 0, 2, 10, 2, 30000},
		{"Wolf", "Beast", "Beast", 4, "Forest", 526, 0, 518, 90, 2, 10, 2, 30000},
		{"Forest Troll", "Beast", "Beast", 8, "Forest", 507, 0, 520, 90, 2, 10, 2, 30000},
		{"Forest Troll", "Beast", "Beast", 8, "Forest", 502, 0, 528, 270, 2, 10, 2, 30000},
	}
	ins := d.q(`INSERT INTO npc_spawns
		(name,race,class,level,area_name,x,y,z,yaw,aggressiveness,aggressive_range,attack_range,respawn_delay_ms)
		VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)`)
	for _, s := range seeds {
		_, _ = d.db.ExecContext(ctx, ins,
			s.name, s.race, s.class, s.level, s.area,
			s.x, s.y, s.z, s.yaw,
			s.agg, s.aggRange, s.attackRange, s.respawnMs)
	}
}

// LoadNPCSpawns returns all rows from npc_spawns ordered by id.
func (d *DB) LoadNPCSpawns(ctx context.Context) ([]*NpcSpawn, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, name, race, class, level, area_name, x, y, z, yaw,
		        aggressiveness, aggressive_range, attack_range, respawn_delay_ms,
		        actor_def_id, start_waypoint_id,
		        wander_radius, wander_pause_min_ms, wander_pause_max_ms
		 FROM npc_spawns ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("db: LoadNPCSpawns: %w", err)
	}
	defer rows.Close()
	var out []*NpcSpawn
	for rows.Next() {
		s := &NpcSpawn{}
		if err := rows.Scan(
			&s.ID, &s.Name, &s.Race, &s.Class, &s.Level, &s.AreaName,
			&s.X, &s.Y, &s.Z, &s.Yaw,
			&s.Aggressiveness, &s.AggressiveRange, &s.AttackRange, &s.RespawnDelayMs,
			&s.ActorDefID, &s.StartWaypointID,
			&s.WanderRadius, &s.WanderPauseMinMs, &s.WanderPauseMaxMs,
		); err != nil {
			return nil, fmt.Errorf("db: LoadNPCSpawns scan: %w", err)
		}
		out = append(out, s)
	}
	return out, rows.Err()
}

// migrateV7 creates the Media registry tables and adds actor_def_id to npc_spawns.
//
// These tables are primarily written by the GUE (Media tab). The server reads
// them to resolve a spawn's ActorDefID Ã¢â€ â€™ model_path + material_paths + animation
// clips before broadcasting PNewActor to clients.
func (d *DB) migrateV7(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }

	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS media_models (
			id        SERIAL PRIMARY KEY,
			name      VARCHAR(64)  NOT NULL DEFAULT 'New Model',
			file_path VARCHAR(256) NOT NULL DEFAULT '',
			scale     REAL         NOT NULL DEFAULT 1.0
		)`)
		exec(`CREATE TABLE IF NOT EXISTS media_materials (
			id          SERIAL PRIMARY KEY,
			name        VARCHAR(64)  NOT NULL DEFAULT 'New Material',
			albedo_path VARCHAR(256) NOT NULL DEFAULT '',
			normal_path VARCHAR(256) NOT NULL DEFAULT '',
			orm_path    VARCHAR(256) NOT NULL DEFAULT '',
			albedo_r    REAL NOT NULL DEFAULT 0.72,
			albedo_g    REAL NOT NULL DEFAULT 0.68,
			albedo_b    REAL NOT NULL DEFAULT 0.60,
			roughness   REAL NOT NULL DEFAULT 0.5,
			metallic    REAL NOT NULL DEFAULT 0.0
		)`)
		exec(`CREATE TABLE IF NOT EXISTS media_anim_clips (
			id            SERIAL PRIMARY KEY,
			name          VARCHAR(64)  NOT NULL DEFAULT 'New Clip',
			source_path   VARCHAR(256) NOT NULL DEFAULT '',
			clip_override VARCHAR(64)  NOT NULL DEFAULT ''
		)`)
		exec(`CREATE TABLE IF NOT EXISTS media_actor_defs (
			id   SERIAL PRIMARY KEY,
			name VARCHAR(64) NOT NULL DEFAULT 'New Actor'
		)`)
		exec(`CREATE TABLE IF NOT EXISTS media_actor_meshes (
			id           SERIAL PRIMARY KEY,
			actor_def_id INTEGER  NOT NULL,
			slot         SMALLINT NOT NULL DEFAULT 0,
			model_id     INTEGER  NOT NULL DEFAULT 0,
			material_id  INTEGER  NOT NULL DEFAULT 0
		)`)
		exec(`CREATE TABLE IF NOT EXISTS media_actor_anims (
			id           SERIAL PRIMARY KEY,
			actor_def_id INTEGER NOT NULL,
			action       VARCHAR(64) NOT NULL DEFAULT 'Idle',
			clip_id      INTEGER NOT NULL DEFAULT 0
		)`)
		exec(`ALTER TABLE npc_spawns ADD COLUMN IF NOT EXISTS actor_def_id INTEGER NOT NULL DEFAULT 0`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS media_models (
			id        INTEGER PRIMARY KEY AUTOINCREMENT,
			name      TEXT    NOT NULL DEFAULT 'New Model',
			file_path TEXT    NOT NULL DEFAULT '',
			scale     REAL    NOT NULL DEFAULT 1.0
		)`)
		exec(`CREATE TABLE IF NOT EXISTS media_materials (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			name        TEXT    NOT NULL DEFAULT 'New Material',
			albedo_path TEXT    NOT NULL DEFAULT '',
			normal_path TEXT    NOT NULL DEFAULT '',
			orm_path    TEXT    NOT NULL DEFAULT '',
			albedo_r    REAL    NOT NULL DEFAULT 0.72,
			albedo_g    REAL    NOT NULL DEFAULT 0.68,
			albedo_b    REAL    NOT NULL DEFAULT 0.60,
			roughness   REAL    NOT NULL DEFAULT 0.5,
			metallic    REAL    NOT NULL DEFAULT 0.0
		)`)
		exec(`CREATE TABLE IF NOT EXISTS media_anim_clips (
			id            INTEGER PRIMARY KEY AUTOINCREMENT,
			name          TEXT    NOT NULL DEFAULT 'New Clip',
			source_path   TEXT    NOT NULL DEFAULT '',
			clip_override TEXT    NOT NULL DEFAULT ''
		)`)
		exec(`CREATE TABLE IF NOT EXISTS media_actor_defs (
			id   INTEGER PRIMARY KEY AUTOINCREMENT,
			name TEXT    NOT NULL DEFAULT 'New Actor'
		)`)
		exec(`CREATE TABLE IF NOT EXISTS media_actor_meshes (
			id           INTEGER PRIMARY KEY AUTOINCREMENT,
			actor_def_id INTEGER NOT NULL,
			slot         INTEGER NOT NULL DEFAULT 0,
			model_id     INTEGER NOT NULL DEFAULT 0,
			material_id  INTEGER NOT NULL DEFAULT 0
		)`)
		exec(`CREATE TABLE IF NOT EXISTS media_actor_anims (
			id           INTEGER PRIMARY KEY AUTOINCREMENT,
			actor_def_id INTEGER NOT NULL,
			action       TEXT    NOT NULL DEFAULT 'Idle',
			clip_id      INTEGER NOT NULL DEFAULT 0
		)`)
		// SQLite: ADD COLUMN fails silently if already present (we don't use IF NOT EXISTS).
		exec(`ALTER TABLE npc_spawns ADD COLUMN actor_def_id INTEGER NOT NULL DEFAULT 0`)
	}

	// Per-aiMaterial mapping for multi-material models (e.g. Substance imports
	// where every submesh names a different "blinn"/"ID" material). Stored as
	// a flat "k1=v1;k2=v2" string keyed by the model's ai-material name Ã¢â€ â€™ the
	// media_materials.name to use. Mirrors the GUE-side migration.
	if d.driver == "postgres" {
		exec(`ALTER TABLE media_models ADD COLUMN IF NOT EXISTS material_map TEXT NOT NULL DEFAULT ''`)
	} else {
		exec(`ALTER TABLE media_models ADD COLUMN material_map TEXT NOT NULL DEFAULT ''`)
	}

	// Normal map intensity per terrain material Ã¢â‚¬â€ compensates for whiteout triplanar
	// blend softening on top-facing surfaces. 2.5 matches the shader's previous global default.
	if d.driver == "postgres" {
		exec(`ALTER TABLE media_materials ADD COLUMN IF NOT EXISTS normal_strength REAL NOT NULL DEFAULT 2.5`)
	} else {
		exec(`ALTER TABLE media_materials ADD COLUMN normal_strength REAL NOT NULL DEFAULT 2.5`)
	}

	// migrateV8 Ã¢â‚¬â€ extend media_actor_defs with gameplay defaults so an actor
	// def carries everything needed to spawn a creature (appearance + stats +
	// AI tuning). Zone placement copies these into npc_spawns at insert time.
	if d.driver == "postgres" {
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS scale                  REAL NOT NULL DEFAULT 1.0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS default_name           VARCHAR(64)  NOT NULL DEFAULT ''`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS default_race           VARCHAR(32)  NOT NULL DEFAULT ''`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS default_class          VARCHAR(32)  NOT NULL DEFAULT ''`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS default_level          INTEGER NOT NULL DEFAULT 1`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS default_hp             INTEGER NOT NULL DEFAULT 100`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS default_ep             INTEGER NOT NULL DEFAULT 100`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS default_aggressiveness INTEGER NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS default_aggro_range    REAL NOT NULL DEFAULT 8.0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS default_attack_range   REAL NOT NULL DEFAULT 2.0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS default_respawn_ms     INTEGER NOT NULL DEFAULT 30000`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS is_playable            INTEGER NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS is_mountable           INTEGER NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS is_interactive         INTEGER NOT NULL DEFAULT 0`)
	} else {
		// SQLite: these fail silently if the column is already present.
		exec(`ALTER TABLE media_actor_defs ADD COLUMN scale                  REAL    NOT NULL DEFAULT 1.0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN default_name           TEXT    NOT NULL DEFAULT ''`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN default_race           TEXT    NOT NULL DEFAULT ''`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN default_class          TEXT    NOT NULL DEFAULT ''`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN default_level          INTEGER NOT NULL DEFAULT 1`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN default_hp             INTEGER NOT NULL DEFAULT 100`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN default_ep             INTEGER NOT NULL DEFAULT 100`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN default_aggressiveness INTEGER NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN default_aggro_range    REAL    NOT NULL DEFAULT 8.0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN default_attack_range   REAL    NOT NULL DEFAULT 2.0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN default_respawn_ms     INTEGER NOT NULL DEFAULT 30000`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN is_playable            INTEGER NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN is_mountable           INTEGER NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN is_interactive         INTEGER NOT NULL DEFAULT 0`)
	}
}

// migrateV9 Ã¢â‚¬â€ coordinate-system alignment: previously the client terrain
// was centered on the origin while the GUE stored positions in [0, W*cs].
// Shift rows that still use the old "centered on origin" values into the
// new 0-indexed convention (Starter Zone is 1024Ãƒâ€”1024 Ã¢â€ â€™ center = 512,512).
// Heuristic: positions inside the small Ã‚Â±100 window around the old origin
// are assumed legacy and get bumped. Positions already in the new range
// (hundreds of units) are left alone.
// Safe to run multiple times: once a row's been shifted it falls outside
// the window and no further migration is applied.
func (d *DB) migrateV9(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	// Characters still at (0, 0, 0) Ã¢â‚¬â€ the old map center Ã¢â‚¬â€ land at the
	// new center of Starter Zone so they don't spawn at the corner.
	exec(`UPDATE characters SET x = 512, z = 512
	      WHERE x = 0 AND z = 0 AND area_name = 'Starter Zone'`)
	// Seed NPCs in Starter Zone (coords around old origin) Ã¢â€ â€™ shift by the
	// old map half-extent.
	exec(`UPDATE npc_spawns SET x = x + 512, z = z + 512
	      WHERE area_name = 'Starter Zone'
	        AND x BETWEEN -100 AND 100 AND z BETWEEN -100 AND 100`)
	exec(`UPDATE npc_spawns SET x = x + 512, z = z + 512
	      WHERE area_name = 'Forest'
	        AND x BETWEEN -100 AND 100 AND z BETWEEN -100 AND 100`)
}

// migrateV10 Ã¢â‚¬â€ adds actor_def_id to characters so player appearance is driven
// by the Media registry (same path as NPCs).
func (d *DB) migrateV10(ctx context.Context) {
	if d.driver == "postgres" {
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE characters ADD COLUMN IF NOT EXISTS actor_def_id INTEGER NOT NULL DEFAULT 0`)
	} else {
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE characters ADD COLUMN actor_def_id INTEGER NOT NULL DEFAULT 0`)
	}
}

// migrateV11 extends the animation + input tables with playback metadata,
// animation events, and input mapping tables.
//
// Idempotent: ADD COLUMN fails silently on SQLite (column already present)
// and uses IF NOT EXISTS on Postgres. CREATE TABLE IF NOT EXISTS is safe to
// re-run in all cases.
func (d *DB) migrateV11(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }

	if d.driver == "postgres" {
		// Extend media_anim_clips
		exec(`ALTER TABLE media_anim_clips ADD COLUMN IF NOT EXISTS start_frame INTEGER NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE media_anim_clips ADD COLUMN IF NOT EXISTS end_frame   INTEGER NOT NULL DEFAULT -1`)
		exec(`ALTER TABLE media_anim_clips ADD COLUMN IF NOT EXISTS fps         REAL    NOT NULL DEFAULT 30.0`)

		// Extend media_actor_anims
		exec(`ALTER TABLE media_actor_anims ADD COLUMN IF NOT EXISTS loop      INTEGER NOT NULL DEFAULT 1`)
		exec(`ALTER TABLE media_actor_anims ADD COLUMN IF NOT EXISTS speed     REAL    NOT NULL DEFAULT 1.0`)
		exec(`ALTER TABLE media_actor_anims ADD COLUMN IF NOT EXISTS blend_in  REAL    NOT NULL DEFAULT 0.15`)
		exec(`ALTER TABLE media_actor_anims ADD COLUMN IF NOT EXISTS return_to TEXT    NOT NULL DEFAULT ''`)
		exec(`ALTER TABLE media_actor_anims ADD COLUMN IF NOT EXISTS priority  INTEGER NOT NULL DEFAULT 0`)

		// Animation events
		exec(`CREATE TABLE IF NOT EXISTS media_anim_events (
			id         SERIAL PRIMARY KEY,
			clip_id    INTEGER     NOT NULL,
			frame      INTEGER     NOT NULL,
			event_type VARCHAR(32) NOT NULL DEFAULT 'sfx',
			payload    TEXT        NOT NULL DEFAULT ''
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_anim_events_clip ON media_anim_events(clip_id)`)

		// Input presets
		exec(`CREATE TABLE IF NOT EXISTS media_input_presets (
			id          SERIAL PRIMARY KEY,
			name        VARCHAR(64) NOT NULL UNIQUE,
			description TEXT        NOT NULL DEFAULT '',
			is_default  INTEGER     NOT NULL DEFAULT 0,
			created_at  INTEGER     NOT NULL DEFAULT 0
		)`)

		// Input maps
		exec(`CREATE TABLE IF NOT EXISTS media_input_maps (
			id           SERIAL PRIMARY KEY,
			preset_id    INTEGER     NOT NULL DEFAULT 1,
			context      TEXT        NOT NULL DEFAULT 'gameplay',
			key          TEXT        NOT NULL DEFAULT '',
			modifier     TEXT        NOT NULL DEFAULT '',
			trigger_type TEXT        NOT NULL DEFAULT 'press',
			action       TEXT        NOT NULL DEFAULT '',
			axis_value   REAL        NOT NULL DEFAULT 1.0,
			enabled      INTEGER     NOT NULL DEFAULT 1,
			remappable   INTEGER     NOT NULL DEFAULT 1,
			UNIQUE (preset_id, context, key, modifier, trigger_type)
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_input_maps ON media_input_maps(preset_id, context, enabled)`)

		// Seed Default preset
		exec(`INSERT INTO media_input_presets (id, name, description, is_default, created_at)
		      VALUES (1, 'Default', 'Default keyboard layout', 1, 0)
		      ON CONFLICT DO NOTHING`)

		// Seed default input mappings
		seeds := [][9]string{
			{"1", "gameplay", "W", "", "axis", "MoveForward", "1.0", "1", "1"},
			{"1", "gameplay", "S", "", "axis", "MoveBack", "-1.0", "1", "1"},
			{"1", "gameplay", "A", "", "axis", "MoveLeft", "-1.0", "1", "1"},
			{"1", "gameplay", "D", "", "axis", "MoveRight", "1.0", "1", "1"},
			{"1", "gameplay", "Space", "", "press", "Jump", "1.0", "1", "1"},
			{"1", "gameplay", "Mouse1", "", "press", "Attack", "1.0", "1", "1"},
			{"1", "gameplay", "Mouse2", "", "hold", "Block", "1.0", "1", "1"},
			{"1", "gameplay", "F", "", "press", "Interact", "1.0", "1", "0"},
			{"1", "gameplay", "I", "", "press", "OpenInventory", "1.0", "1", "0"},
			{"1", "gameplay", "C", "", "press", "OpenCharacter", "1.0", "1", "0"},
			{"1", "gameplay", "Escape", "", "press", "CloseUI", "1.0", "1", "0"},
			{"1", "gameplay", "1", "", "press", "UseSpell1", "1.0", "1", "1"},
			{"1", "gameplay", "2", "", "press", "UseSpell2", "1.0", "1", "1"},
			{"1", "gameplay", "3", "", "press", "UseSpell3", "1.0", "1", "1"},
		}
		for _, s := range seeds {
			_, _ = d.db.ExecContext(ctx,
				`INSERT INTO media_input_maps
				   (preset_id, context, key, modifier, trigger_type, action, axis_value, enabled, remappable)
				 VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)
				 ON CONFLICT DO NOTHING`,
				s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8])
		}
	} else {
		// SQLite: ADD COLUMN fails silently if already present.
		// Extend media_anim_clips
		exec(`ALTER TABLE media_anim_clips ADD COLUMN start_frame INTEGER NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE media_anim_clips ADD COLUMN end_frame   INTEGER NOT NULL DEFAULT -1`)
		exec(`ALTER TABLE media_anim_clips ADD COLUMN fps         REAL    NOT NULL DEFAULT 30.0`)

		// Extend media_actor_anims
		exec(`ALTER TABLE media_actor_anims ADD COLUMN loop      INTEGER NOT NULL DEFAULT 1`)
		exec(`ALTER TABLE media_actor_anims ADD COLUMN speed     REAL    NOT NULL DEFAULT 1.0`)
		exec(`ALTER TABLE media_actor_anims ADD COLUMN blend_in  REAL    NOT NULL DEFAULT 0.15`)
		exec(`ALTER TABLE media_actor_anims ADD COLUMN return_to TEXT    NOT NULL DEFAULT ''`)
		exec(`ALTER TABLE media_actor_anims ADD COLUMN priority  INTEGER NOT NULL DEFAULT 0`)

		// Animation events
		exec(`CREATE TABLE IF NOT EXISTS media_anim_events (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			clip_id    INTEGER NOT NULL,
			frame      INTEGER NOT NULL,
			event_type TEXT    NOT NULL DEFAULT 'sfx',
			payload    TEXT    NOT NULL DEFAULT ''
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_anim_events_clip ON media_anim_events(clip_id)`)

		// Input presets
		exec(`CREATE TABLE IF NOT EXISTS media_input_presets (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			name        TEXT    NOT NULL UNIQUE,
			description TEXT    NOT NULL DEFAULT '',
			is_default  INTEGER NOT NULL DEFAULT 0,
			created_at  INTEGER NOT NULL DEFAULT 0
		)`)

		// Input maps
		exec(`CREATE TABLE IF NOT EXISTS media_input_maps (
			id           INTEGER PRIMARY KEY AUTOINCREMENT,
			preset_id    INTEGER NOT NULL DEFAULT 1,
			context      TEXT    NOT NULL DEFAULT 'gameplay',
			key          TEXT    NOT NULL DEFAULT '',
			modifier     TEXT    NOT NULL DEFAULT '',
			trigger_type TEXT    NOT NULL DEFAULT 'press',
			action       TEXT    NOT NULL DEFAULT '',
			axis_value   REAL    NOT NULL DEFAULT 1.0,
			enabled      INTEGER NOT NULL DEFAULT 1,
			remappable   INTEGER NOT NULL DEFAULT 1,
			UNIQUE (preset_id, context, key, modifier, trigger_type)
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_input_maps ON media_input_maps(preset_id, context, enabled)`)

		// Seed Default preset
		exec(`INSERT OR IGNORE INTO media_input_presets (id, name, description, is_default, created_at)
		      VALUES (1, 'Default', 'Default keyboard layout', 1, 0)`)

		// Seed default input mappings
		seeds := [][8]string{
			{"gameplay", "W", "", "axis", "MoveForward", "1.0", "1", "1"},
			{"gameplay", "S", "", "axis", "MoveBack", "-1.0", "1", "1"},
			{"gameplay", "A", "", "axis", "MoveLeft", "-1.0", "1", "1"},
			{"gameplay", "D", "", "axis", "MoveRight", "1.0", "1", "1"},
			{"gameplay", "Space", "", "press", "Jump", "1.0", "1", "1"},
			{"gameplay", "Mouse1", "", "press", "Attack", "1.0", "1", "1"},
			{"gameplay", "Mouse2", "", "hold", "Block", "1.0", "1", "1"},
			{"gameplay", "F", "", "press", "Interact", "1.0", "1", "0"},
			{"gameplay", "I", "", "press", "OpenInventory", "1.0", "1", "0"},
			{"gameplay", "C", "", "press", "OpenCharacter", "1.0", "1", "0"},
			{"gameplay", "Escape", "", "press", "CloseUI", "1.0", "1", "0"},
			{"gameplay", "1", "", "press", "UseSpell1", "1.0", "1", "1"},
			{"gameplay", "2", "", "press", "UseSpell2", "1.0", "1", "1"},
			{"gameplay", "3", "", "press", "UseSpell3", "1.0", "1", "1"},
		}
		for _, s := range seeds {
			_, _ = d.db.ExecContext(ctx,
				`INSERT OR IGNORE INTO media_input_maps
				   (preset_id, context, key, modifier, trigger_type, action, axis_value, enabled, remappable)
				 VALUES (1,?,?,?,?,?,?,?,?)`,
				s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7])
		}
	}
}

// migrateV12 Ã¢â‚¬â€ adds yaw_offset and y_offset to media_actor_defs so GUE authors
// can correct backwards-facing or ground-sunken models without touching assets.
func (d *DB) migrateV12(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	if d.driver == "postgres" {
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS yaw_offset REAL NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN IF NOT EXISTS y_offset   REAL NOT NULL DEFAULT 0`)
	} else {
		exec(`ALTER TABLE media_actor_defs ADD COLUMN yaw_offset REAL NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE media_actor_defs ADD COLUMN y_offset   REAL NOT NULL DEFAULT 0`)
	}
}

// ---------------------------------------------------------------------------
// Media registry Ã¢â‚¬â€ read-only accessors used by the server to resolve an
// NpcSpawn.ActorDefID into concrete asset paths for PNewActor.
// ---------------------------------------------------------------------------

// MediaModel mirrors one row in media_models.
type MediaModel struct {
	ID       int
	Name     string
	FilePath string
	Scale    float32

	// MaterialMap maps an aiMaterial name (as it appears in the mesh file)
	// to a media_materials.name. Built from the "k1=v1;k2=v2" persisted in
	// the material_map column. Empty when the model has no overrides.
	MaterialMap map[string]string
}

// MediaMaterial mirrors one row in media_materials.
type MediaMaterial struct {
	ID         int
	Name       string
	AlbedoPath string
	NormalPath string
	ORMPath    string
	AlbedoR    float32
	AlbedoG    float32
	AlbedoB    float32
	Roughness  float32
	Metallic   float32
}

// MediaAnimClip mirrors one row in media_anim_clips.
type MediaAnimClip struct {
	ID           int
	Name         string
	SourcePath   string
	ClipOverride string
	StartFrame   int32
	EndFrame     int32
	FPS          float32
}

// MediaAnimEvent mirrors one row in media_anim_events.
type MediaAnimEvent struct {
	ID        int
	ClipID    int
	Frame     int32
	EventType string
	Payload   string
}

// InputPreset mirrors one row in media_input_presets.
type InputPreset struct {
	ID          int
	Name        string
	Description string
	IsDefault   bool
}

// InputBinding mirrors one row in media_input_maps.
type InputBinding struct {
	ID          int
	PresetID    int
	Context     string
	Key         string
	Modifier    string
	TriggerType string
	Action      string
	AxisValue   float32
	Enabled     bool
	Remappable  bool
}

// ActorDefMesh mirrors one row in media_actor_meshes.
type ActorDefMesh struct {
	ID         int
	ActorDefID int
	Slot       int
	ModelID    int
	MaterialID int
}

// ActorDefAnim mirrors one row in media_actor_anims.
type ActorDefAnim struct {
	ID         int
	ActorDefID int
	Action     string
	ClipID     int
	Loop       bool
	Speed      float32
	BlendIn    float32
	ReturnTo   string
	Priority   uint8
}

// ActorDef bundles an actor definition with its meshes + animation map +
// gameplay defaults. Zone placement copies the defaults into npc_spawns.
type ActorDef struct {
	ID        int
	Name      string
	Scale     float32 // multiplies each mesh slot's model scale (1.0 = natural size)
	YawOffset float64 // model-space Y rotation (degrees) applied before world yaw
	YOffset   float64 // vertical offset (world units) added to position at render time

	// Gameplay defaults Ã¢â‚¬â€ applied to freshly placed npc_spawns so users
	// don't have to retype identical fields for every copy of the same
	// creature. Empty strings / zero values mean "no default".
	DefaultName           string
	DefaultRace           string
	DefaultClass          string
	DefaultLevel          int
	DefaultHP             int
	DefaultEP             int
	DefaultAggressiveness int
	DefaultAggroRange     float32
	DefaultAttackRange    float32
	DefaultRespawnMs      int
	LootTableID           int
	IsPlayable            bool
	IsMountable           bool
	IsInteractive         bool

	Meshes []ActorDefMesh
	Anims  []ActorDefAnim
}

// LoadActorDef resolves a single actor def (id > 0) with its meshes + anims.
// Returns nil if the def does not exist.
func (d *DB) LoadActorDef(ctx context.Context, id int) (*ActorDef, error) {
	if id <= 0 {
		return nil, nil
	}
	out := &ActorDef{ID: id}
	var playable, mountable, interactive int
	err := d.db.QueryRowContext(ctx, d.q(
		`SELECT name, scale, yaw_offset, y_offset,
		        default_name, default_race, default_class,
		        default_level, default_hp, default_ep,
		        default_aggressiveness, default_aggro_range, default_attack_range,
		        default_respawn_ms, loot_table_id,
		        is_playable, is_mountable, is_interactive
		 FROM media_actor_defs WHERE id = ?`), id).Scan(
		&out.Name, &out.Scale, &out.YawOffset, &out.YOffset,
		&out.DefaultName, &out.DefaultRace, &out.DefaultClass,
		&out.DefaultLevel, &out.DefaultHP, &out.DefaultEP,
		&out.DefaultAggressiveness, &out.DefaultAggroRange, &out.DefaultAttackRange,
		&out.DefaultRespawnMs, &out.LootTableID,
		&playable, &mountable, &interactive,
	)
	if err != nil {
		return nil, nil // missing / not found
	}
	out.IsPlayable = playable != 0
	out.IsMountable = mountable != 0
	out.IsInteractive = interactive != 0
	if out.Scale <= 0 {
		out.Scale = 1.0 // guard against rows from before this migration
	}

	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT id, actor_def_id, slot, model_id, material_id
		     FROM media_actor_meshes WHERE actor_def_id = ? ORDER BY slot`), id)
	if err == nil {
		defer rows.Close()
		for rows.Next() {
			var m ActorDefMesh
			if err := rows.Scan(&m.ID, &m.ActorDefID, &m.Slot, &m.ModelID, &m.MaterialID); err == nil {
				out.Meshes = append(out.Meshes, m)
			}
		}
	}

	arows, err := d.db.QueryContext(ctx,
		d.q(`SELECT id, actor_def_id, action, clip_id, loop, speed, blend_in, return_to, priority
		     FROM media_actor_anims WHERE actor_def_id = ?`), id)
	if err == nil {
		defer arows.Close()
		for arows.Next() {
			var a ActorDefAnim
			var loopInt, priInt int
			if err := arows.Scan(&a.ID, &a.ActorDefID, &a.Action, &a.ClipID,
				&loopInt, &a.Speed, &a.BlendIn, &a.ReturnTo, &priInt); err == nil {
				a.Loop = loopInt != 0
				a.Priority = uint8(priInt)
				out.Anims = append(out.Anims, a)
			}
		}
	}
	return out, nil
}

// GetMediaModel fetches one row by id. Returns nil if missing.
func (d *DB) GetMediaModel(ctx context.Context, id int) (*MediaModel, error) {
	if id <= 0 {
		return nil, nil
	}
	m := &MediaModel{ID: id}
	var matMap string
	err := d.db.QueryRowContext(ctx,
		d.q(`SELECT name, file_path, scale, material_map FROM media_models WHERE id = ?`), id,
	).Scan(&m.Name, &m.FilePath, &m.Scale, &matMap)
	if err != nil {
		return nil, nil
	}
	m.MaterialMap = parseMaterialMap(matMap)
	return m, nil
}

// ListMediaMaterials returns every row in media_materials, keyed by name.
// Used by the spawn-time appearance builder to resolve a model's
// material_map (aiMaterial Ã¢â€ â€™ media material name) into concrete PBR paths.
func (d *DB) ListMediaMaterials(ctx context.Context) (map[string]*MediaMaterial, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, name, albedo_path, normal_path, orm_path,
		        albedo_r, albedo_g, albedo_b, roughness, metallic
		   FROM media_materials`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := make(map[string]*MediaMaterial)
	for rows.Next() {
		var m MediaMaterial
		if err := rows.Scan(&m.ID, &m.Name, &m.AlbedoPath, &m.NormalPath,
			&m.ORMPath, &m.AlbedoR, &m.AlbedoG, &m.AlbedoB,
			&m.Roughness, &m.Metallic); err == nil {
			mm := m
			out[mm.Name] = &mm
		}
	}
	return out, nil
}

// ListPlayableDefs returns all actor defs marked is_playable=1.
// Used by the server to send available character options to the client.
func (d *DB) ListPlayableDefs(ctx context.Context) ([]PlayableDef, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, name, default_race, default_class
		 FROM media_actor_defs WHERE is_playable = 1 ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("db: ListPlayableDefs: %w", err)
	}
	defer rows.Close()
	var out []PlayableDef
	for rows.Next() {
		var p PlayableDef
		if err := rows.Scan(&p.ID, &p.Name, &p.DefaultRace, &p.DefaultClass); err != nil {
			return nil, fmt.Errorf("db: ListPlayableDefs scan: %w", err)
		}
		out = append(out, p)
	}
	return out, rows.Err()
}

// parseMaterialMap deserialises the "k1=v1;k2=v2" format used by
// media_models.material_map. Empty / malformed rows return an empty map.
func parseMaterialMap(s string) map[string]string {
	if s == "" {
		return nil
	}
	out := make(map[string]string)
	for _, pair := range strings.Split(s, ";") {
		eq := strings.IndexByte(pair, '=')
		if eq <= 0 {
			continue
		}
		k := strings.TrimSpace(pair[:eq])
		v := strings.TrimSpace(pair[eq+1:])
		if k != "" && v != "" {
			out[k] = v
		}
	}
	return out
}

// GetMediaMaterial fetches one row by id. Returns nil if missing.
func (d *DB) GetMediaMaterial(ctx context.Context, id int) (*MediaMaterial, error) {
	if id <= 0 {
		return nil, nil
	}
	m := &MediaMaterial{ID: id}
	err := d.db.QueryRowContext(ctx,
		d.q(`SELECT name, albedo_path, normal_path, orm_path,
		            albedo_r, albedo_g, albedo_b, roughness, metallic
		     FROM media_materials WHERE id = ?`), id,
	).Scan(&m.Name, &m.AlbedoPath, &m.NormalPath, &m.ORMPath,
		&m.AlbedoR, &m.AlbedoG, &m.AlbedoB, &m.Roughness, &m.Metallic)
	if err != nil {
		return nil, nil
	}
	return m, nil
}

// GetMediaAnimClip fetches one row by id. Returns nil if missing.
func (d *DB) GetMediaAnimClip(ctx context.Context, id int) (*MediaAnimClip, error) {
	if id <= 0 {
		return nil, nil
	}
	c := &MediaAnimClip{ID: id}
	err := d.db.QueryRowContext(ctx,
		d.q(`SELECT name, source_path, clip_override, start_frame, end_frame, fps
		     FROM media_anim_clips WHERE id = ?`), id,
	).Scan(&c.Name, &c.SourcePath, &c.ClipOverride, &c.StartFrame, &c.EndFrame, &c.FPS)
	if err != nil {
		return nil, nil
	}
	return c, nil
}

// LoadAnimEvents returns all animation events for a clip, ordered by frame.
func (d *DB) LoadAnimEvents(ctx context.Context, clipID int) ([]MediaAnimEvent, error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT id, clip_id, frame, event_type, payload
		     FROM media_anim_events WHERE clip_id = ? ORDER BY frame`), clipID)
	if err != nil {
		return nil, fmt.Errorf("db: LoadAnimEvents: %w", err)
	}
	defer rows.Close()
	var out []MediaAnimEvent
	for rows.Next() {
		var e MediaAnimEvent
		if err := rows.Scan(&e.ID, &e.ClipID, &e.Frame, &e.EventType, &e.Payload); err != nil {
			return nil, fmt.Errorf("db: LoadAnimEvents scan: %w", err)
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

// LoadInputBindings returns all input bindings for a preset, ordered by id.
func (d *DB) LoadInputBindings(ctx context.Context, presetID int) ([]InputBinding, error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT id, preset_id, context, key, modifier, trigger_type, action, axis_value, enabled, remappable
		     FROM media_input_maps WHERE preset_id = ? ORDER BY id`), presetID)
	if err != nil {
		return nil, fmt.Errorf("db: LoadInputBindings: %w", err)
	}
	defer rows.Close()
	var out []InputBinding
	for rows.Next() {
		var b InputBinding
		var enabled, remappable int
		if err := rows.Scan(&b.ID, &b.PresetID, &b.Context, &b.Key, &b.Modifier,
			&b.TriggerType, &b.Action, &b.AxisValue, &enabled, &remappable); err != nil {
			return nil, fmt.Errorf("db: LoadInputBindings scan: %w", err)
		}
		b.Enabled = enabled != 0
		b.Remappable = remappable != 0
		out = append(out, b)
	}
	return out, rows.Err()
}

// EnsureKnownSpells grants all spells to a new character if it has none yet.
func (d *DB) EnsureKnownSpells(ctx context.Context, charID string) error {
	var count int
	_ = d.db.QueryRowContext(ctx,
		d.q(`SELECT COUNT(*) FROM character_known_spells WHERE character_id = ?`), charID,
	).Scan(&count)
	if count > 0 {
		return nil
	}
	rows, err := d.db.QueryContext(ctx, `SELECT id FROM spell_templates`)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		var id int
		if err := rows.Scan(&id); err != nil {
			return err
		}
		_, _ = d.db.ExecContext(ctx,
			d.q(`INSERT INTO character_known_spells (character_id, spell_id) VALUES (?, ?) ON CONFLICT DO NOTHING`),
			charID, id)
	}
	return rows.Err()
}

// GetKnownSpellIDs returns the spell IDs known by a character.
func (d *DB) GetKnownSpellIDs(ctx context.Context, charID string) ([]uint16, error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT spell_id FROM character_known_spells WHERE character_id = ? ORDER BY spell_id`),
		charID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []uint16
	for rows.Next() {
		var id uint16
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		out = append(out, id)
	}
	return out, rows.Err()
}

// ---------------------------------------------------------------------------
// Quest methods
// ---------------------------------------------------------------------------

// ListQuestLog returns all visible quest states for a character.
func (d *DB) ListQuestLog(ctx context.Context, charID string) ([]*QuestLogEntry, error) {
	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT q.id, q.code, q.title, q.description, cq.state
		  FROM character_quests cq
		  JOIN quest_defs q ON q.id = cq.quest_id
		 WHERE cq.character_id = ?
		   AND cq.state IN ('active', 'completed', 'turned_in')
		 ORDER BY q.id`), charID)
	if err != nil {
		return nil, fmt.Errorf("db: ListQuestLog: %w", err)
	}
	defer rows.Close()

	var out []*QuestLogEntry
	for rows.Next() {
		e := &QuestLogEntry{}
		var stateText string
		if err := rows.Scan(&e.QuestID, &e.Code, &e.Title, &e.Description, &stateText); err != nil {
			return nil, fmt.Errorf("db: ListQuestLog scan quest: %w", err)
		}
		e.State = questStateToCode(stateText)

		objRows, err := d.db.QueryContext(ctx, d.q(`
			SELECT od.id, od.objective_type, od.description,
			       COALESCE(cp.current_count, 0),
			       CASE
			         WHEN COALESCE(cp.target_count, od.target_count, 1) < 1 THEN 1
			         ELSE COALESCE(cp.target_count, od.target_count, 1)
			       END AS target_count,
			       od.target_npc_name,
			       od.target_item_id,
			       od.target_area_name
			  FROM quest_objective_defs od
			  LEFT JOIN character_quest_progress cp
			         ON cp.character_id = ? AND cp.quest_id = od.quest_id AND cp.objective_id = od.id
			 WHERE od.quest_id = ?
			 ORDER BY od.objective_order, od.id`), charID, e.QuestID)
		if err != nil {
			return nil, fmt.Errorf("db: ListQuestLog objectives: %w", err)
		}
		for objRows.Next() {
			var obj QuestLogObjective
			if err := objRows.Scan(
				&obj.ObjectiveID,
				&obj.ObjectiveType,
				&obj.Description,
				&obj.CurrentCount,
				&obj.TargetCount,
				&obj.TargetNPCName,
				&obj.TargetItemID,
				&obj.TargetArea,
			); err != nil {
				objRows.Close()
				return nil, fmt.Errorf("db: ListQuestLog scan objective: %w", err)
			}
			e.Objectives = append(e.Objectives, obj)
		}
		if err := objRows.Err(); err != nil {
			objRows.Close()
			return nil, fmt.Errorf("db: ListQuestLog objective rows: %w", err)
		}
		objRows.Close()
		out = append(out, e)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("db: ListQuestLog rows: %w", err)
	}
	return out, nil
}

// ListAvailableQuests returns quests currently available to accept.
func (d *DB) ListAvailableQuests(ctx context.Context, charID string) ([]*QuestAvailableEntry, error) {
	var charLevel int
	if err := d.db.QueryRowContext(ctx, d.q(`SELECT level FROM characters WHERE id = ?`), charID).Scan(&charLevel); err != nil {
		return nil, fmt.Errorf("db: ListAvailableQuests character level: %w", err)
	}

	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT q.id,
		       q.code,
		       q.title,
		       q.description,
		       q.min_level,
		       CASE WHEN q.repeatable THEN 1 ELSE 0 END AS repeatable_int,
		       q.prerequisite_quest_id,
		       COALESCE(cq.state, '') AS current_state
		  FROM quest_defs q
		  LEFT JOIN character_quests cq
		         ON cq.character_id = ? AND cq.quest_id = q.id
		 WHERE CASE WHEN q.is_active THEN 1 ELSE 0 END = 1
		 ORDER BY q.id`), charID)
	if err != nil {
		return nil, fmt.Errorf("db: ListAvailableQuests query: %w", err)
	}
	defer rows.Close()

	var out []*QuestAvailableEntry
	for rows.Next() {
		var (
			e              QuestAvailableEntry
			repeatableInt  int
			prerequisiteID int
			currentState   string
		)
		if err := rows.Scan(
			&e.QuestID,
			&e.Code,
			&e.Title,
			&e.Description,
			&e.MinLevel,
			&repeatableInt,
			&prerequisiteID,
			&currentState,
		); err != nil {
			return nil, fmt.Errorf("db: ListAvailableQuests scan: %w", err)
		}
		e.Repeatable = repeatableInt != 0

		if charLevel < e.MinLevel {
			continue
		}

		switch currentState {
		case questStateTextActive, questStateTextCompleted:
			continue
		case questStateTextTurnedIn:
			if !e.Repeatable {
				continue
			}
		}

		if prerequisiteID > 0 {
			var prereqTurnedIn int
			if err := d.db.QueryRowContext(ctx, d.q(`
				SELECT COUNT(1)
				  FROM character_quests
				 WHERE character_id = ? AND quest_id = ? AND state = ?`),
				charID, prerequisiteID, questStateTextTurnedIn).Scan(&prereqTurnedIn); err != nil {
				return nil, fmt.Errorf("db: ListAvailableQuests prerequisite: %w", err)
			}
			if prereqTurnedIn == 0 {
				continue
			}
		}

		out = append(out, &e)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("db: ListAvailableQuests rows: %w", err)
	}
	return out, nil
}

// AcceptQuest starts a quest for the character (idempotent for already-active).
// Returns true when state changed.
func (d *DB) AcceptQuest(ctx context.Context, charID string, questID int) (bool, error) {
	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer tx.Rollback()

	var (
		minLevel       int
		repeatableInt  int
		prerequisiteID int
		isActiveInt    int
		charLevel      int
		existingState  string
		nowStamp       = now()
	)
	err = tx.QueryRowContext(ctx, d.q(`
		SELECT min_level,
		       CASE WHEN repeatable THEN 1 ELSE 0 END,
		       prerequisite_quest_id,
		       CASE WHEN is_active THEN 1 ELSE 0 END
		  FROM quest_defs
		 WHERE id = ?`), questID).Scan(&minLevel, &repeatableInt, &prerequisiteID, &isActiveInt)
	if err == sql.ErrNoRows {
		return false, fmt.Errorf("db: AcceptQuest: quest %d not found", questID)
	}
	if err != nil {
		return false, fmt.Errorf("db: AcceptQuest quest row: %w", err)
	}
	if isActiveInt == 0 {
		return false, fmt.Errorf("db: AcceptQuest: quest %d is inactive", questID)
	}

	if err := tx.QueryRowContext(ctx, d.q(`SELECT level FROM characters WHERE id = ?`), charID).Scan(&charLevel); err != nil {
		return false, fmt.Errorf("db: AcceptQuest character level: %w", err)
	}
	if charLevel < minLevel {
		return false, fmt.Errorf("db: AcceptQuest: level %d required", minLevel)
	}

	if prerequisiteID > 0 {
		var prereqTurnedIn int
		if err := tx.QueryRowContext(ctx, d.q(`
			SELECT COUNT(1)
			  FROM character_quests
			 WHERE character_id = ? AND quest_id = ? AND state = ?`),
			charID, prerequisiteID, questStateTextTurnedIn).Scan(&prereqTurnedIn); err != nil {
			return false, fmt.Errorf("db: AcceptQuest prerequisite: %w", err)
		}
		if prereqTurnedIn == 0 {
			return false, fmt.Errorf("db: AcceptQuest: prerequisite quest %d not turned in", prerequisiteID)
		}
	}

	err = tx.QueryRowContext(ctx, d.q(`
		SELECT state
		  FROM character_quests
		 WHERE character_id = ? AND quest_id = ?`), charID, questID).Scan(&existingState)
	switch err {
	case nil:
		if existingState == questStateTextActive {
			if err := tx.Commit(); err != nil {
				return false, err
			}
			return false, nil
		}
		if repeatableInt == 0 && (existingState == questStateTextCompleted || existingState == questStateTextTurnedIn) {
			return false, fmt.Errorf("db: AcceptQuest: quest %d is not repeatable", questID)
		}
		if _, err := tx.ExecContext(ctx, d.q(`
			UPDATE character_quests
			   SET state = ?, accepted_at = ?, completed_at = NULL, turned_in_at = NULL, updated_at = ?
			 WHERE character_id = ? AND quest_id = ?`),
			questStateTextActive, nowStamp, nowStamp, charID, questID); err != nil {
			return false, fmt.Errorf("db: AcceptQuest update: %w", err)
		}
	case sql.ErrNoRows:
		if _, err := tx.ExecContext(ctx, d.q(`
			INSERT INTO character_quests (character_id, quest_id, state, accepted_at, updated_at)
			VALUES (?, ?, ?, ?, ?)`),
			charID, questID, questStateTextActive, nowStamp, nowStamp); err != nil {
			return false, fmt.Errorf("db: AcceptQuest insert: %w", err)
		}
	default:
		return false, fmt.Errorf("db: AcceptQuest query existing: %w", err)
	}

	if _, err := tx.ExecContext(ctx, d.q(`
		DELETE FROM character_quest_progress
		 WHERE character_id = ? AND quest_id = ?`), charID, questID); err != nil {
		return false, fmt.Errorf("db: AcceptQuest clear progress: %w", err)
	}

	if _, err := tx.ExecContext(ctx, d.q(`
		INSERT INTO character_quest_progress (character_id, quest_id, objective_id, current_count, target_count, updated_at)
		SELECT ?, ?, od.id, 0,
		       CASE WHEN od.target_count < 1 THEN 1 ELSE od.target_count END,
		       ?
		  FROM quest_objective_defs od
		 WHERE od.quest_id = ?`),
		charID, questID, nowStamp, questID); err != nil {
		return false, fmt.Errorf("db: AcceptQuest seed progress: %w", err)
	}

	var objectiveCount int
	if err := tx.QueryRowContext(ctx, d.q(`SELECT COUNT(1) FROM quest_objective_defs WHERE quest_id = ?`), questID).Scan(&objectiveCount); err != nil {
		return false, fmt.Errorf("db: AcceptQuest objective count: %w", err)
	}
	if objectiveCount == 0 {
		if _, err := tx.ExecContext(ctx, d.q(`
			UPDATE character_quests
			   SET state = ?, completed_at = ?, updated_at = ?
			 WHERE character_id = ? AND quest_id = ? AND state = ?`),
			questStateTextCompleted, nowStamp, nowStamp, charID, questID, questStateTextActive); err != nil {
			return false, fmt.Errorf("db: AcceptQuest auto-complete: %w", err)
		}
	}

	if err := tx.Commit(); err != nil {
		return false, err
	}
	return true, nil
}

// AbandonQuest marks an active quest as abandoned and clears progress.
// Returns true when state changed.
func (d *DB) AbandonQuest(ctx context.Context, charID string, questID int) (bool, error) {
	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		return false, err
	}
	defer tx.Rollback()

	var state string
	err = tx.QueryRowContext(ctx, d.q(`
		SELECT state FROM character_quests
		 WHERE character_id = ? AND quest_id = ?`), charID, questID).Scan(&state)
	if err == sql.ErrNoRows {
		if err := tx.Commit(); err != nil {
			return false, err
		}
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("db: AbandonQuest query: %w", err)
	}
	if state != questStateTextActive {
		if err := tx.Commit(); err != nil {
			return false, err
		}
		return false, nil
	}

	nowStamp := now()
	if _, err := tx.ExecContext(ctx, d.q(`
		UPDATE character_quests
		   SET state = ?, updated_at = ?
		 WHERE character_id = ? AND quest_id = ? AND state = ?`),
		questStateTextAbandoned, nowStamp, charID, questID, questStateTextActive); err != nil {
		return false, fmt.Errorf("db: AbandonQuest update: %w", err)
	}
	if _, err := tx.ExecContext(ctx, d.q(`
		DELETE FROM character_quest_progress
		 WHERE character_id = ? AND quest_id = ?`), charID, questID); err != nil {
		return false, fmt.Errorf("db: AbandonQuest clear progress: %w", err)
	}
	if err := tx.Commit(); err != nil {
		return false, err
	}
	return true, nil
}

// TurnInQuest atomically marks a completed quest as turned-in and applies all
// rewards (items/gold/xp) in the same DB transaction.
//
// Returns changed=false when the quest was already turned in (idempotent).
func (d *DB) TurnInQuest(ctx context.Context, charID string, questID int) (*QuestTurnInResult, bool, error) {
	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, false, err
	}
	defer tx.Rollback()

	var state string
	err = tx.QueryRowContext(ctx, d.q(`
		SELECT state FROM character_quests
		 WHERE character_id = ? AND quest_id = ?`), charID, questID).Scan(&state)
	if err == sql.ErrNoRows {
		return nil, false, fmt.Errorf("db: TurnInQuest: quest %d not accepted", questID)
	}
	if err != nil {
		return nil, false, fmt.Errorf("db: TurnInQuest query state: %w", err)
	}
	if state == questStateTextTurnedIn {
		if err := tx.Commit(); err != nil {
			return nil, false, err
		}
		return nil, false, nil
	}
	if state != questStateTextCompleted {
		return nil, false, fmt.Errorf("db: TurnInQuest: quest %d is not completed", questID)
	}

	rows, err := tx.QueryContext(ctx, d.q(`
		SELECT xp_reward, gold_reward, item_id, item_qty
		  FROM quest_reward_defs
		 WHERE quest_id = ?
		 ORDER BY id`), questID)
	if err != nil {
		return nil, false, fmt.Errorf("db: TurnInQuest rewards query: %w", err)
	}

	var (
		rewards      []QuestRewardEntry
		totalXP      int64
		totalGold    int64
		itemsChanged bool
	)
	for rows.Next() {
		var (
			r       QuestRewardEntry
			itemID  int
			itemQty int
		)
		if err := rows.Scan(&r.XPReward, &r.GoldReward, &itemID, &itemQty); err != nil {
			rows.Close()
			return nil, false, fmt.Errorf("db: TurnInQuest rewards scan: %w", err)
		}
		if itemID < 0 || itemQty < 0 {
			rows.Close()
			return nil, false, fmt.Errorf("db: TurnInQuest reward row invalid: negative item fields")
		}
		if itemID == 0 && itemQty > 0 {
			rows.Close()
			return nil, false, fmt.Errorf("db: TurnInQuest reward row invalid: item_qty without item_id")
		}
		if itemID > 0 && itemQty == 0 {
			rows.Close()
			return nil, false, fmt.Errorf("db: TurnInQuest reward row invalid: item_id without item_qty")
		}
		if itemID > math.MaxUint16 {
			rows.Close()
			return nil, false, fmt.Errorf("db: TurnInQuest reward row invalid: item_id %d exceeds uint16", itemID)
		}
		if itemQty > math.MaxUint8 {
			rows.Close()
			return nil, false, fmt.Errorf("db: TurnInQuest reward row invalid: item_qty %d exceeds uint8", itemQty)
		}

		if itemID > 0 {
			r.ItemID = uint16(itemID)
			r.ItemQty = uint8(itemQty)
		}
		rewards = append(rewards, r)
		totalXP += r.XPReward
		totalGold += r.GoldReward
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, false, fmt.Errorf("db: TurnInQuest rewards rows: %w", err)
	}
	rows.Close()

	var (
		curXP    int64
		curLevel int
		curGold  int64
	)
	if err := tx.QueryRowContext(ctx, d.q(`
		SELECT xp, level, gold
		  FROM characters
		 WHERE id = ?`), charID).Scan(&curXP, &curLevel, &curGold); err != nil {
		return nil, false, fmt.Errorf("db: TurnInQuest character state: %w", err)
	}

	newXP := curXP
	newLevel := curLevel
	leveled := false
	if totalXP > 0 {
		newXP, newLevel, leveled = world.ProcessXPCumulative(curXP, curLevel, totalXP)
	}

	for _, reward := range rewards {
		if reward.ItemID == 0 || reward.ItemQty == 0 {
			continue
		}
		if _, err := d.addStackableItemTx(ctx, tx, charID, reward.ItemID, reward.ItemQty, 0, 100); err != nil {
			return nil, false, fmt.Errorf("db: TurnInQuest add item (%d x%d): %w", reward.ItemID, reward.ItemQty, err)
		}
		itemsChanged = true
	}

	newGold := curGold
	if totalGold != 0 {
		if _, err := tx.ExecContext(ctx,
			d.q(`UPDATE characters SET gold = gold + ? WHERE id = ?`),
			totalGold, charID); err != nil {
			return nil, false, fmt.Errorf("db: TurnInQuest update gold: %w", err)
		}
		if err := tx.QueryRowContext(ctx,
			d.q(`SELECT gold FROM characters WHERE id = ?`),
			charID).Scan(&newGold); err != nil {
			return nil, false, fmt.Errorf("db: TurnInQuest read gold: %w", err)
		}
	}

	if totalXP > 0 {
		if _, err := tx.ExecContext(ctx, d.q(`
			UPDATE characters
			   SET xp = ?, level = ?
			 WHERE id = ?`),
			newXP, newLevel, charID); err != nil {
			return nil, false, fmt.Errorf("db: TurnInQuest update xp: %w", err)
		}
	}

	nowStamp := now()
	if _, err := tx.ExecContext(ctx, d.q(`
		UPDATE character_quests
		   SET state = ?, turned_in_at = ?, updated_at = ?
		 WHERE character_id = ? AND quest_id = ? AND state = ?`),
		questStateTextTurnedIn, nowStamp, nowStamp, charID, questID, questStateTextCompleted); err != nil {
		return nil, false, fmt.Errorf("db: TurnInQuest update state: %w", err)
	}

	if err := tx.Commit(); err != nil {
		return nil, false, err
	}

	result := &QuestTurnInResult{
		Rewards:      rewards,
		NewGold:      newGold,
		NewXP:        newXP,
		NewLevel:     newLevel,
		Leveled:      leveled,
		GoldChanged:  totalGold != 0,
		XPChanged:    totalXP > 0,
		ItemsChanged: itemsChanged,
	}
	return result, true, nil
}

func (d *DB) findFreeBackpackSlotTx(ctx context.Context, tx *sql.Tx, charID string) (uint8, bool, error) {
	rows, err := tx.QueryContext(ctx,
		d.q(`SELECT slot FROM character_items WHERE character_id = ? AND slot >= 14 ORDER BY slot`),
		charID)
	if err != nil {
		return 0, false, fmt.Errorf("db: findFreeBackpackSlotTx: %w", err)
	}
	defer rows.Close()

	used := make(map[uint8]bool)
	for rows.Next() {
		var slot uint8
		if err := rows.Scan(&slot); err != nil {
			return 0, false, err
		}
		used[slot] = true
	}
	if err := rows.Err(); err != nil {
		return 0, false, err
	}
	for slot := uint8(14); slot <= 44; slot++ {
		if !used[slot] {
			return slot, true, nil
		}
	}
	return 0, false, nil
}

func (d *DB) addItemToSlotTx(ctx context.Context, tx *sql.Tx, charID string, slot uint8, itemID uint16, qty, dur uint8) error {
	_, err := tx.ExecContext(ctx,
		d.q(`INSERT INTO character_items (character_id, slot, item_id, quantity, durability)
		     VALUES (?, ?, ?, ?, ?)`),
		charID, slot, itemID, qty, dur)
	if err != nil {
		return fmt.Errorf("db: addItemToSlotTx: %w", err)
	}
	return nil
}

func (d *DB) addStackableItemTx(ctx context.Context, tx *sql.Tx, charID string, itemID uint16, qty uint8, maxStack uint8, dur uint8) (uint8, error) {
	if qty == 0 {
		return 0, nil
	}

	if maxStack == 0 {
		var ms int
		err := tx.QueryRowContext(ctx,
			d.q(`SELECT max_stack FROM item_templates WHERE id = ?`),
			itemID).Scan(&ms)
		if err != nil {
			if err == sql.ErrNoRows {
				return 0, fmt.Errorf("item template %d not found", itemID)
			}
			return 0, fmt.Errorf("db: addStackableItemTx max_stack: %w", err)
		}
		if ms > 0 {
			maxStack = uint8(ms)
		}
	}
	if maxStack < 1 {
		maxStack = 1
	}

	rows, err := tx.QueryContext(ctx,
		d.q(`SELECT slot, quantity FROM character_items
		     WHERE character_id = ? AND item_id = ? AND slot >= 14 AND quantity < ?
		     ORDER BY slot`),
		charID, itemID, maxStack)
	if err != nil {
		return 0, fmt.Errorf("db: addStackableItemTx query: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var (
			slot     uint8
			existing uint8
		)
		if err := rows.Scan(&slot, &existing); err != nil {
			return 0, fmt.Errorf("db: addStackableItemTx scan: %w", err)
		}
		room := maxStack - existing
		add := qty
		if add > room {
			add = room
		}
		if _, err := tx.ExecContext(ctx,
			d.q(`UPDATE character_items SET quantity = quantity + ? WHERE character_id = ? AND slot = ?`),
			add, charID, slot); err != nil {
			return 0, fmt.Errorf("db: addStackableItemTx update: %w", err)
		}
		qty -= add
		if qty == 0 {
			return slot, nil
		}
	}
	if err := rows.Err(); err != nil {
		return 0, fmt.Errorf("db: addStackableItemTx rows: %w", err)
	}

	freeSlot, found, err := d.findFreeBackpackSlotTx(ctx, tx, charID)
	if err != nil {
		return 0, err
	}
	if !found {
		return 0, fmt.Errorf("inventory full")
	}
	if err := d.addItemToSlotTx(ctx, tx, charID, freeSlot, itemID, qty, dur); err != nil {
		return 0, err
	}
	return freeSlot, nil
}

// ApplyQuestProgressEvent advances active objectives matching the event and
// returns the list of quest IDs that changed progress/state.
func (d *DB) ApplyQuestProgressEvent(ctx context.Context, charID string, event QuestProgressEvent) ([]int, error) {
	if event.Delta <= 0 {
		event.Delta = 1
	}

	query := `
		SELECT cq.quest_id, cp.objective_id, cp.current_count, cp.target_count
		  FROM character_quests cq
		  JOIN character_quest_progress cp
		    ON cp.character_id = cq.character_id AND cp.quest_id = cq.quest_id
		  JOIN quest_objective_defs od
		    ON od.id = cp.objective_id
		 WHERE cq.character_id = ? AND cq.state = ? AND od.objective_type = ?`
	args := []interface{}{charID, questStateTextActive, event.ObjectiveType}

	switch event.ObjectiveType {
	case QuestObjectiveKill, QuestObjectiveTalk, QuestObjectiveInteract:
		if event.TargetNPCName == "" {
			return nil, nil
		}
		query += ` AND od.target_npc_name = ?`
		args = append(args, event.TargetNPCName)
	case QuestObjectiveCollect:
		if event.TargetItemID == 0 {
			return nil, nil
		}
		query += ` AND od.target_item_id = ?`
		args = append(args, int(event.TargetItemID))
	case QuestObjectiveExplore:
		if event.TargetArea == "" {
			return nil, nil
		}
		query += ` AND od.target_area_name = ?`
		args = append(args, event.TargetArea)
	default:
		return nil, nil
	}

	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()

	rows, err := tx.QueryContext(ctx, d.q(query), args...)
	if err != nil {
		return nil, fmt.Errorf("db: ApplyQuestProgressEvent query: %w", err)
	}
	type rowData struct {
		questID     int
		objectiveID int
		current     int
		target      int
	}
	var matches []rowData
	for rows.Next() {
		var row rowData
		if err := rows.Scan(&row.questID, &row.objectiveID, &row.current, &row.target); err != nil {
			rows.Close()
			return nil, fmt.Errorf("db: ApplyQuestProgressEvent scan: %w", err)
		}
		matches = append(matches, row)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, fmt.Errorf("db: ApplyQuestProgressEvent rows: %w", err)
	}
	rows.Close()
	if len(matches) == 0 {
		if err := tx.Commit(); err != nil {
			return nil, err
		}
		return nil, nil
	}

	changedQuestIDs := make(map[int]struct{})
	nowStamp := now()

	for _, m := range matches {
		if m.target < 1 {
			m.target = 1
		}
		if m.current >= m.target {
			continue
		}
		next := m.current + event.Delta
		if next > m.target {
			next = m.target
		}
		if _, err := tx.ExecContext(ctx, d.q(`
			UPDATE character_quest_progress
			   SET current_count = ?, updated_at = ?
			 WHERE character_id = ? AND quest_id = ? AND objective_id = ?`),
			next, nowStamp, charID, m.questID, m.objectiveID); err != nil {
			return nil, fmt.Errorf("db: ApplyQuestProgressEvent update objective: %w", err)
		}
		changedQuestIDs[m.questID] = struct{}{}
	}

	var changed []int
	for questID := range changedQuestIDs {
		var pending int
		if err := tx.QueryRowContext(ctx, d.q(`
			SELECT COUNT(1)
			  FROM character_quest_progress
			 WHERE character_id = ? AND quest_id = ? AND current_count < target_count`),
			charID, questID).Scan(&pending); err != nil {
			return nil, fmt.Errorf("db: ApplyQuestProgressEvent pending count: %w", err)
		}
		if pending == 0 {
			if _, err := tx.ExecContext(ctx, d.q(`
				UPDATE character_quests
				   SET state = ?, completed_at = ?, updated_at = ?
				 WHERE character_id = ? AND quest_id = ? AND state = ?`),
				questStateTextCompleted, nowStamp, nowStamp, charID, questID, questStateTextActive); err != nil {
				return nil, fmt.Errorf("db: ApplyQuestProgressEvent complete quest: %w", err)
			}
		}
		changed = append(changed, questID)
	}
	sort.Ints(changed)

	if err := tx.Commit(); err != nil {
		return nil, err
	}
	return changed, nil
}

// ---------------------------------------------------------------------------
// Gold methods
// ---------------------------------------------------------------------------

// UpdateGold adds delta (positive or negative) to a character's gold.
// Returns the new gold total.
func (d *DB) UpdateGold(ctx context.Context, charID string, delta int64) (int64, error) {
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE characters SET gold = gold + ? WHERE id = ?`),
		delta, charID)
	if err != nil {
		return 0, fmt.Errorf("db: UpdateGold: %w", err)
	}
	var gold int64
	err = d.db.QueryRowContext(ctx,
		d.q(`SELECT gold FROM characters WHERE id = ?`), charID,
	).Scan(&gold)
	return gold, err
}

// UpdateCharacterGold sets an absolute gold value for the character.
func (d *DB) UpdateCharacterGold(ctx context.Context, charID string, gold int64) error {
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE characters SET gold = ? WHERE id = ?`),
		gold, charID)
	if err != nil {
		return fmt.Errorf("db: UpdateCharacterGold: %w", err)
	}
	return nil
}

// UpdateCharacterPrimaryStats persists per-character primary stats and unspent points.
func (d *DB) UpdateCharacterPrimaryStats(ctx context.Context, charID string, primary world.PrimaryStats, unspent int32) error {
	_, err := d.db.ExecContext(ctx, d.q(`
		UPDATE characters
		   SET primary_strength = ?,
		       primary_dexterity = ?,
		       primary_intelligence = ?,
		       primary_wisdom = ?,
		       primary_perception = ?,
		       unspent_stat_points = ?
		 WHERE id = ?`),
		primary.STR, primary.DEX, primary.INT, primary.WIS, primary.PER, unspent, charID)
	if err != nil {
		return fmt.Errorf("db: UpdateCharacterPrimaryStats: %w", err)
	}
	return nil
}

// UpdateCharacterUnspentStatPoints persists the current unspent stat point pool.
func (d *DB) UpdateCharacterUnspentStatPoints(ctx context.Context, charID string, unspent int32) error {
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE characters SET unspent_stat_points = ? WHERE id = ?`),
		unspent, charID)
	if err != nil {
		return fmt.Errorf("db: UpdateCharacterUnspentStatPoints: %w", err)
	}
	return nil
}

// UpdateCharacterFreeRespecsUsed persists the number of free respecs consumed.
func (d *DB) UpdateCharacterFreeRespecsUsed(ctx context.Context, charID string, count int32) error {
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE characters SET free_respecs_used = ? WHERE id = ?`),
		count, charID)
	if err != nil {
		return fmt.Errorf("db: UpdateCharacterFreeRespecsUsed: %w", err)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Inventory helpers for drops and shop
// ---------------------------------------------------------------------------

// FindFreeBackpackSlot finds the first unused backpack slot (14-44).
// Returns (slot, true) or (0, false) if the backpack is full.
func (d *DB) FindFreeBackpackSlot(ctx context.Context, charID string) (uint8, bool, error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT slot FROM character_items WHERE character_id = ? AND slot >= 14 ORDER BY slot`),
		charID)
	if err != nil {
		return 0, false, fmt.Errorf("db: FindFreeBackpackSlot: %w", err)
	}
	defer rows.Close()
	used := make(map[uint8]bool)
	for rows.Next() {
		var s uint8
		if err := rows.Scan(&s); err != nil {
			return 0, false, err
		}
		used[s] = true
	}
	if err := rows.Err(); err != nil {
		return 0, false, err
	}
	for s := uint8(14); s <= 44; s++ {
		if !used[s] {
			return s, true, nil
		}
	}
	return 0, false, nil
}

// AddItemToSlot inserts an item into the given inventory slot.
func (d *DB) AddItemToSlot(ctx context.Context, charID string, slot uint8, itemID uint16, qty, dur uint8) error {
	_, err := d.db.ExecContext(ctx,
		d.q(`INSERT INTO character_items (character_id, slot, item_id, quantity, durability)
		     VALUES (?, ?, ?, ?, ?)`),
		charID, slot, itemID, qty, dur)
	if err != nil {
		return fmt.Errorf("db: AddItemToSlot: %w", err)
	}
	return nil
}

// AddStackableItem tries to stack qty onto an existing backpack slot that has
// the same itemID and room. maxStack=0 means look it up from item_templates.
// Returns (slot, error). Returns (0, err) wrapping "inventory full" if no space.
func (d *DB) AddStackableItem(ctx context.Context, charID string, itemID uint16, qty uint8, maxStack uint8, dur uint8) (uint8, error) {
	if maxStack == 0 {
		var ms int
		if err := d.db.QueryRowContext(ctx,
			d.q(`SELECT max_stack FROM item_templates WHERE id = ?`), itemID,
		).Scan(&ms); err == nil && ms > 0 {
			maxStack = uint8(ms)
		}
	}
	if maxStack < 1 {
		maxStack = 1
	}

	// Look for an existing partial stack in backpack (slots 14-44)
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT slot, quantity FROM character_items
		     WHERE character_id = ? AND item_id = ? AND slot >= 14 AND quantity < ?
		     ORDER BY slot`),
		charID, itemID, maxStack)
	if err != nil {
		return 0, fmt.Errorf("db: AddStackableItem query: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var slot uint8
		var existing uint8
		if err := rows.Scan(&slot, &existing); err != nil {
			continue
		}
		room := maxStack - existing
		add := qty
		if add > room {
			add = room
		}
		_, err := d.db.ExecContext(ctx,
			d.q(`UPDATE character_items SET quantity = quantity + ? WHERE character_id = ? AND slot = ?`),
			add, charID, slot)
		if err != nil {
			return 0, fmt.Errorf("db: AddStackableItem update: %w", err)
		}
		qty -= add
		if qty == 0 {
			return slot, nil
		}
	}
	rows.Close()

	if qty == 0 {
		return 0, nil
	}

	// Need a free slot for the remainder
	freeSlot, found, err := d.FindFreeBackpackSlot(ctx, charID)
	if err != nil {
		return 0, err
	}
	if !found {
		return 0, fmt.Errorf("inventory full")
	}
	if err := d.AddItemToSlot(ctx, charID, freeSlot, itemID, qty, dur); err != nil {
		return 0, err
	}
	return freeSlot, nil
}

// RemoveItemAtSlot deletes the item at the given inventory slot.
func (d *DB) RemoveItemAtSlot(ctx context.Context, charID string, slot uint8) error {
	_, err := d.db.ExecContext(ctx,
		d.q(`DELETE FROM character_items WHERE character_id = ? AND slot = ?`),
		charID, slot)
	if err != nil {
		return fmt.Errorf("db: RemoveItemAtSlot: %w", err)
	}
	return nil
}

// GetItemAtSlot returns the item at the given inventory slot, including item_value.
func (d *DB) GetItemAtSlot(ctx context.Context, charID string, slot uint8) (*CharacterItem, error) {
	ci := &CharacterItem{}
	err := d.db.QueryRowContext(ctx,
		d.q(`SELECT ci.slot, ci.item_id, ci.quantity, ci.durability,
		          it.name, it.item_type, it.slot_type, it.weapon_damage, it.armor_level, it.item_value,
		          it.model_path, it.model_scale, it.socket_name
		     FROM character_items ci
		     JOIN item_templates it ON it.id = ci.item_id
		     WHERE ci.character_id = ? AND ci.slot = ?`),
		charID, slot,
	).Scan(&ci.Slot, &ci.ItemID, &ci.Quantity, &ci.Durability,
		&ci.Name, &ci.ItemType, &ci.SlotType, &ci.WeaponDamage, &ci.ArmorLevel, &ci.ItemValue,
		&ci.ModelPath, &ci.ModelScale, &ci.SocketName)
	if err != nil {
		return nil, fmt.Errorf("db: GetItemAtSlot: %w", err)
	}
	return ci, nil
}

// LoadAllItemTemplates returns all item templates keyed by name.
func (d *DB) LoadAllItemTemplates(ctx context.Context) (map[string]*ItemTemplate, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, name, item_type, slot_type, weapon_damage, armor_level, weapon_dimension, weapon_hands, weapon_range, max_stack, item_value, stackable, weapon_kit, model_path, model_scale, socket_name
		 FROM item_templates ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("db: LoadAllItemTemplates: %w", err)
	}
	defer rows.Close()
	out := make(map[string]*ItemTemplate)
	for rows.Next() {
		t := &ItemTemplate{}
		var stackable int
		if err := rows.Scan(&t.ID, &t.Name, &t.ItemType, &t.SlotType, &t.WeaponDamage,
			&t.ArmorLevel, &t.WeaponDimension, &t.WeaponHands, &t.WeaponRange, &t.MaxStack, &t.ItemValue, &stackable, &t.WeaponKit, &t.ModelPath, &t.ModelScale, &t.SocketName); err != nil {
			return nil, err
		}
		t.Stackable = stackable != 0
		out[t.Name] = t
	}
	return out, rows.Err()
}

// ListItemTemplates returns all item templates as a slice ordered by id.
func (d *DB) ListItemTemplates(ctx context.Context) ([]*ItemTemplate, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, name, item_type, slot_type, weapon_damage, armor_level, weapon_dimension, weapon_hands, weapon_range, max_stack, item_value, stackable, weapon_kit, model_path, model_scale, socket_name
		 FROM item_templates ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("db: ListItemTemplates: %w", err)
	}
	defer rows.Close()
	var out []*ItemTemplate
	for rows.Next() {
		t := &ItemTemplate{}
		var stackable int
		if err := rows.Scan(&t.ID, &t.Name, &t.ItemType, &t.SlotType, &t.WeaponDamage,
			&t.ArmorLevel, &t.WeaponDimension, &t.WeaponHands, &t.WeaponRange, &t.MaxStack, &t.ItemValue, &stackable, &t.WeaponKit, &t.ModelPath, &t.ModelScale, &t.SocketName); err != nil {
			return nil, err
		}
		t.Stackable = stackable != 0
		out = append(out, t)
	}
	return out, rows.Err()
}

// CreateItemTemplate inserts a new item template and returns the assigned id.
func (d *DB) CreateItemTemplate(ctx context.Context, t *ItemTemplate) (int, error) {
	stackable := 0
	if t.Stackable {
		stackable = 1
	}
	res, err := d.db.ExecContext(ctx,
		`INSERT INTO item_templates (name, item_type, slot_type, weapon_damage, armor_level, weapon_dimension, weapon_hands, weapon_range, max_stack, item_value, stackable, weapon_kit, model_path, model_scale, socket_name)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		t.Name, t.ItemType, t.SlotType, t.WeaponDamage, t.ArmorLevel, t.WeaponDimension, t.WeaponHands, t.WeaponRange, t.MaxStack, t.ItemValue, stackable, t.WeaponKit, t.ModelPath, t.ModelScale, t.SocketName)
	if err != nil {
		return 0, fmt.Errorf("db: CreateItemTemplate: %w", err)
	}
	id, _ := res.LastInsertId()
	return int(id), nil
}

// UpdateItemTemplate updates all mutable fields of an existing item template.
func (d *DB) UpdateItemTemplate(ctx context.Context, t *ItemTemplate) error {
	stackable := 0
	if t.Stackable {
		stackable = 1
	}
	_, err := d.db.ExecContext(ctx,
		`UPDATE item_templates
		 SET name=?, item_type=?, slot_type=?, weapon_damage=?, armor_level=?, weapon_dimension=?, weapon_hands=?, weapon_range=?, max_stack=?, item_value=?, stackable=?, weapon_kit=?, model_path=?, model_scale=?, socket_name=?
		 WHERE id=?`,
		t.Name, t.ItemType, t.SlotType, t.WeaponDamage, t.ArmorLevel, t.WeaponDimension, t.WeaponHands, t.WeaponRange, t.MaxStack, t.ItemValue, stackable, t.WeaponKit, t.ModelPath, t.ModelScale, t.SocketName, t.ID)
	if err != nil {
		return fmt.Errorf("db: UpdateItemTemplate: %w", err)
	}
	return nil
}

// DeleteItemTemplate removes an item template by id.
// Returns an error if the item is referenced by any character_items row.
func (d *DB) DeleteItemTemplate(ctx context.Context, id int) error {
	var count int
	_ = d.db.QueryRowContext(ctx,
		d.q(`SELECT COUNT(*) FROM character_items WHERE item_id = ?`), id).Scan(&count)
	if count > 0 {
		return fmt.Errorf("db: DeleteItemTemplate: item %d is in use by %d character(s)", id, count)
	}
	_, err := d.db.ExecContext(ctx, `DELETE FROM item_socket_overrides WHERE item_template_id = ?`, id)
	if err != nil {
		return fmt.Errorf("db: DeleteItemTemplate: clearing overrides for item %d failed: %w", id, err)
	}
	_, err = d.db.ExecContext(ctx, `DELETE FROM item_attributes WHERE item_id = ?`, id)
	if err != nil {
		return fmt.Errorf("db: DeleteItemTemplate: clearing attributes for item %d failed: %w", id, err)
	}
	_, err = d.db.ExecContext(ctx, `DELETE FROM item_templates WHERE id = ?`, id)
	if err != nil {
		return fmt.Errorf("db: DeleteItemTemplate: %w", err)
	}
	return nil
}

// ListItemAttributes returns all bonus attributes for an item template.
func (d *DB) ListItemAttributes(ctx context.Context, itemID int) ([]ItemAttribute, error) {
	if itemID <= 0 {
		return nil, fmt.Errorf("db: ListItemAttributes: itemID must be > 0")
	}

	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT id, item_id, attribute_key, value
		  FROM item_attributes
		 WHERE item_id = ?
		 ORDER BY id`), itemID)
	if err != nil {
		return nil, fmt.Errorf("db: ListItemAttributes: %w", err)
	}
	defer rows.Close()

	var out []ItemAttribute
	for rows.Next() {
		var a ItemAttribute
		if err := rows.Scan(&a.ID, &a.ItemID, &a.AttributeKey, &a.Value); err != nil {
			return nil, fmt.Errorf("db: ListItemAttributes scan: %w", err)
		}
		out = append(out, a)
	}
	return out, rows.Err()
}

// SetItemAttributes replaces all bonus attributes for an item template.
// This runs as a transaction and first clears existing rows before inserting the new set.
func (d *DB) SetItemAttributes(ctx context.Context, itemID int, attrs []ItemAttribute) error {
	if itemID <= 0 {
		return fmt.Errorf("db: SetItemAttributes: itemID must be > 0")
	}

	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("db: SetItemAttributes begin: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	if _, err := tx.ExecContext(ctx, d.q(`DELETE FROM item_attributes WHERE item_id = ?`), itemID); err != nil {
		return fmt.Errorf("db: SetItemAttributes clear: %w", err)
	}

	for i, attr := range attrs {
		key := strings.TrimSpace(attr.AttributeKey)
		if key == "" {
			continue
		}
		if _, ok := world.AttributeByKey(key); !ok {
			return fmt.Errorf("db: SetItemAttributes: unknown attribute key %q at index %d", key, i)
		}
		if _, err := tx.ExecContext(ctx,
			d.q(`INSERT INTO item_attributes (item_id, attribute_key, value) VALUES (?, ?, ?)`),
			itemID, key, attr.Value,
		); err != nil {
			return fmt.Errorf("db: SetItemAttributes insert key %q at index %d: %w", key, i, err)
		}
	}

	if err := tx.Commit(); err != nil {
		return fmt.Errorf("db: SetItemAttributes commit: %w", err)
	}
	return nil
}

// ListLootTables returns all loot tables ordered by id.
func (d *DB) ListLootTables(ctx context.Context) ([]*LootTable, error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT id, name, CASE WHEN enabled THEN 1 ELSE 0 END AS enabled FROM loot_tables ORDER BY id`))
	if err != nil {
		return nil, fmt.Errorf("db: ListLootTables: %w", err)
	}
	defer rows.Close()
	var out []*LootTable
	for rows.Next() {
		t := &LootTable{}
		var enabled int
		if err := rows.Scan(&t.ID, &t.Name, &enabled); err != nil {
			return nil, fmt.Errorf("db: ListLootTables scan: %w", err)
		}
		t.Enabled = enabled != 0
		out = append(out, t)
	}
	return out, rows.Err()
}

// GetLootTable returns a single loot table by id, or nil if not found.
func (d *DB) GetLootTable(ctx context.Context, id int) (*LootTable, error) {
	if id <= 0 {
		return nil, fmt.Errorf("db: GetLootTable: id must be > 0")
	}
	t := &LootTable{}
	var enabled int
	err := d.db.QueryRowContext(ctx,
		d.q(`SELECT id, name, CASE WHEN enabled THEN 1 ELSE 0 END AS enabled
		       FROM loot_tables
		      WHERE id = ?`), id).Scan(&t.ID, &t.Name, &enabled)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("db: GetLootTable: %w", err)
	}
	t.Enabled = enabled != 0
	return t, nil
}

// CreateLootTable inserts a new loot table and returns the new id.
func (d *DB) CreateLootTable(ctx context.Context, name string) (int, error) {
	name = strings.TrimSpace(name)
	if name == "" {
		return 0, fmt.Errorf("db: CreateLootTable: name is required")
	}
	res, err := d.db.ExecContext(ctx,
		d.q(`INSERT INTO loot_tables (name, enabled) VALUES (?, 1)`),
		name)
	if err != nil {
		return 0, fmt.Errorf("db: CreateLootTable: %w", err)
	}
	id, _ := res.LastInsertId()
	return int(id), nil
}

// UpdateLootTable updates an existing loot table.
func (d *DB) UpdateLootTable(ctx context.Context, t LootTable) error {
	if t.ID <= 0 {
		return fmt.Errorf("db: UpdateLootTable: id must be > 0")
	}
	t.Name = strings.TrimSpace(t.Name)
	if t.Name == "" {
		return fmt.Errorf("db: UpdateLootTable: name is required")
	}
	enabled := 0
	if t.Enabled {
		enabled = 1
	}
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE loot_tables SET name = ?, enabled = ? WHERE id = ?`),
		t.Name, enabled, t.ID)
	if err != nil {
		return fmt.Errorf("db: UpdateLootTable: %w", err)
	}
	return nil
}

// DeleteLootTable soft-deletes a loot table by setting enabled = 0.
func (d *DB) DeleteLootTable(ctx context.Context, id int) error {
	if id <= 0 {
		return fmt.Errorf("db: DeleteLootTable: id must be > 0")
	}
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE loot_tables SET enabled = 0 WHERE id = ?`), id)
	if err != nil {
		return fmt.Errorf("db: DeleteLootTable: %w", err)
	}
	return nil
}

// ListLootEntries returns all loot entries for a given table id.
func (d *DB) ListLootEntries(ctx context.Context, lootTableID int) ([]LootEntry, error) {
	if lootTableID <= 0 {
		return nil, fmt.Errorf("db: ListLootEntries: lootTableID must be > 0")
	}
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT id, loot_table_id, item_id, chance, min_qty, max_qty
		       FROM loot_entries
		      WHERE loot_table_id = ?
		      ORDER BY id`),
		lootTableID)
	if err != nil {
		return nil, fmt.Errorf("db: ListLootEntries: %w", err)
	}
	defer rows.Close()

	var out []LootEntry
	for rows.Next() {
		var e LootEntry
		if err := rows.Scan(&e.ID, &e.LootTableID, &e.ItemID, &e.Chance, &e.MinQty, &e.MaxQty); err != nil {
			return nil, fmt.Errorf("db: ListLootEntries scan: %w", err)
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

// CreateLootEntry inserts a new loot entry and returns the new id.
func (d *DB) CreateLootEntry(ctx context.Context, e LootEntry) (int, error) {
	if e.LootTableID <= 0 {
		return 0, fmt.Errorf("db: CreateLootEntry: lootTableID must be > 0")
	}
	if e.ItemID <= 0 {
		return 0, fmt.Errorf("db: CreateLootEntry: itemID must be > 0")
	}
	res, err := d.db.ExecContext(ctx,
		d.q(`INSERT INTO loot_entries (loot_table_id, item_id, chance, min_qty, max_qty)
		      VALUES (?, ?, ?, ?, ?)`),
		e.LootTableID, e.ItemID, e.Chance, e.MinQty, e.MaxQty)
	if err != nil {
		return 0, fmt.Errorf("db: CreateLootEntry: %w", err)
	}
	id, _ := res.LastInsertId()
	return int(id), nil
}

// UpdateLootEntry updates an existing loot entry.
func (d *DB) UpdateLootEntry(ctx context.Context, e LootEntry) error {
	if e.ID <= 0 {
		return fmt.Errorf("db: UpdateLootEntry: id must be > 0")
	}
	if e.LootTableID <= 0 {
		return fmt.Errorf("db: UpdateLootEntry: lootTableID must be > 0")
	}
	if e.ItemID <= 0 {
		return fmt.Errorf("db: UpdateLootEntry: itemID must be > 0")
	}
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE loot_entries
		        SET loot_table_id = ?, item_id = ?, chance = ?, min_qty = ?, max_qty = ?
		      WHERE id = ?`),
		e.LootTableID, e.ItemID, e.Chance, e.MinQty, e.MaxQty, e.ID)
	if err != nil {
		return fmt.Errorf("db: UpdateLootEntry: %w", err)
	}
	return nil
}

// DeleteLootEntry removes a loot entry.
func (d *DB) DeleteLootEntry(ctx context.Context, id int) error {
	if id <= 0 {
		return fmt.Errorf("db: DeleteLootEntry: id must be > 0")
	}
	_, err := d.db.ExecContext(ctx,
		d.q(`DELETE FROM loot_entries WHERE id = ?`), id)
	if err != nil {
		return fmt.Errorf("db: DeleteLootEntry: %w", err)
	}
	return nil
}

// GetSetting returns value for a key in game_settings, or "" if key does not exist.
func (d *DB) GetSetting(ctx context.Context, key string) (string, error) {
	key = strings.TrimSpace(key)
	if key == "" {
		return "", fmt.Errorf("db: GetSetting: key is required")
	}

	var value string
	err := d.db.QueryRowContext(ctx,
		d.q(`SELECT value FROM game_settings WHERE key = ?`), key).
		Scan(&value)
	if err == sql.ErrNoRows {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("db: GetSetting: key=%q: %w", key, err)
	}
	return value, nil
}

// SetSetting upserts a key/value pair in game_settings.
func (d *DB) SetSetting(ctx context.Context, key string, value string) error {
	key = strings.TrimSpace(key)
	if key == "" {
		return fmt.Errorf("db: SetSetting: key is required")
	}
	_, err := d.db.ExecContext(ctx,
		d.q(`INSERT INTO game_settings (key, value)
		 VALUES (?, ?)
		 ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		key, value)
	if err != nil {
		return fmt.Errorf("db: SetSetting: key=%q: %w", key, err)
	}
	return nil
}

// ListSettings returns all rows from game_settings as a key->value map.
func (d *DB) ListSettings(ctx context.Context) (map[string]string, error) {
	rows, err := d.db.QueryContext(ctx, d.q(`SELECT key, value FROM game_settings ORDER BY key`))
	if err != nil {
		return nil, fmt.Errorf("db: ListSettings: %w", err)
	}
	defer rows.Close()

	out := make(map[string]string)
	for rows.Next() {
		var key, value string
		if err := rows.Scan(&key, &value); err != nil {
			return nil, fmt.Errorf("db: ListSettings scan: %w", err)
		}
		key = strings.TrimSpace(key)
		if key == "" {
			continue
		}
		out[key] = value
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("db: ListSettings rows: %w", err)
	}
	return out, nil
}

// ListWeaponKits returns all weapon kits ordered by kit_key.
func (d *DB) ListWeaponKits(ctx context.Context) ([]*WeaponKit, error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT id, kit_key, display_name, description,
		            CASE WHEN enabled THEN 1 ELSE 0 END AS enabled
		       FROM weapon_kits
		      ORDER BY kit_key`))
	if err != nil {
		return nil, fmt.Errorf("db: ListWeaponKits: %w", err)
	}
	defer rows.Close()
	var out []*WeaponKit
	for rows.Next() {
		k := &WeaponKit{}
		var enabled int
		if err := rows.Scan(&k.ID, &k.KitKey, &k.DisplayName, &k.Description, &enabled); err != nil {
			return nil, fmt.Errorf("db: ListWeaponKits scan: %w", err)
		}
		k.Enabled = enabled != 0
		out = append(out, k)
	}
	return out, rows.Err()
}

// GetWeaponKit returns a single weapon kit by kit_key, or nil if not found.
func (d *DB) GetWeaponKit(ctx context.Context, kitKey string) (*WeaponKit, error) {
	if strings.TrimSpace(kitKey) == "" {
		return nil, fmt.Errorf("db: GetWeaponKit: kit_key is required")
	}
	k := &WeaponKit{}
	var enabled int
	err := d.db.QueryRowContext(ctx, d.q(`SELECT id, kit_key, display_name, description,
	                                             CASE WHEN enabled THEN 1 ELSE 0 END AS enabled
	                                        FROM weapon_kits
	                                       WHERE kit_key = ?`), kitKey).
		Scan(&k.ID, &k.KitKey, &k.DisplayName, &k.Description, &enabled)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("db: GetWeaponKit: %w", err)
	}
	k.Enabled = enabled != 0
	return k, nil
}

// GetWeaponKitByID returns a single weapon kit by ID, or nil if not found.
func (d *DB) GetWeaponKitByID(ctx context.Context, kitID int) (*WeaponKit, error) {
	if kitID <= 0 {
		return nil, fmt.Errorf("db: GetWeaponKitByID: kitID must be > 0")
	}
	k := &WeaponKit{}
	var enabled int
	err := d.db.QueryRowContext(ctx, d.q(`SELECT id, kit_key, display_name, description,
	                                             CASE WHEN enabled THEN 1 ELSE 0 END AS enabled
	                                        FROM weapon_kits
	                                       WHERE id = ?`), kitID).
		Scan(&k.ID, &k.KitKey, &k.DisplayName, &k.Description, &enabled)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("db: GetWeaponKitByID: %w", err)
	}
	k.Enabled = enabled != 0
	return k, nil
}

// IsAbilityInKitPool reports whether the ability belongs to the enabled pool
// of a given kit (weapon_kit_abilities.enabled=1 and ability_templates.enabled=1).
func (d *DB) IsAbilityInKitPool(ctx context.Context, kitID, abilityID int) (bool, error) {
	if kitID <= 0 {
		return false, fmt.Errorf("db: IsAbilityInKitPool: kitID must be > 0")
	}
	if abilityID <= 0 {
		return false, fmt.Errorf("db: IsAbilityInKitPool: abilityID must be > 0")
	}
	var count int
	err := d.db.QueryRowContext(ctx, d.q(`
		SELECT COUNT(*)
		  FROM weapon_kit_abilities wka
		  JOIN ability_templates at ON at.id = wka.ability_id
		 WHERE wka.kit_id = ?
		   AND wka.ability_id = ?
		   AND wka.enabled = 1
		   AND at.enabled = 1`), kitID, abilityID).Scan(&count)
	if err != nil {
		return false, fmt.Errorf("db: IsAbilityInKitPool: %w", err)
	}
	return count > 0, nil
}

// ListEnabledKitPoolAbilities returns enabled abilities for a kit pool ordered by slot_index.
// Only entries with weapon_kit_abilities.enabled=true and ability_templates.enabled=true are returned.
func (d *DB) ListEnabledKitPoolAbilities(ctx context.Context, kitID int) ([]KitPoolAbilityEntry, error) {
	if kitID <= 0 {
		return nil, fmt.Errorf("db: ListEnabledKitPoolAbilities: kitID must be > 0")
	}
	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT wka.ability_id, at.name, at.cooldown_ms
		  FROM weapon_kit_abilities wka
		  JOIN ability_templates at ON at.id = wka.ability_id
		 WHERE wka.kit_id = ?
		   AND wka.enabled = 1
		   AND at.enabled = 1
		 ORDER BY wka.slot_index`), kitID)
	if err != nil {
		return nil, fmt.Errorf("db: ListEnabledKitPoolAbilities: %w", err)
	}
	defer rows.Close()

	var out []KitPoolAbilityEntry
	for rows.Next() {
		var entry KitPoolAbilityEntry
		if err := rows.Scan(&entry.AbilityID, &entry.AbilityName, &entry.CooldownMs); err != nil {
			return nil, fmt.Errorf("db: ListEnabledKitPoolAbilities scan: %w", err)
		}
		out = append(out, entry)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("db: ListEnabledKitPoolAbilities rows: %w", err)
	}
	return out, nil
}

// CreateWeaponKit inserts a new weapon kit and returns its ID.
func (d *DB) CreateWeaponKit(ctx context.Context, k *WeaponKit) (int, error) {
	if k == nil {
		return 0, fmt.Errorf("db: CreateWeaponKit: kit is nil")
	}
	kitKey := strings.TrimSpace(k.KitKey)
	if kitKey == "" {
		return 0, fmt.Errorf("db: CreateWeaponKit: kit_key is required")
	}
	enabled := 0
	if k.Enabled {
		enabled = 1
	}
	res, err := d.db.ExecContext(ctx,
		d.q(`INSERT INTO weapon_kits (kit_key, display_name, description, enabled)
		     VALUES (?, ?, ?, ?)`),
		kitKey, k.DisplayName, k.Description, enabled)
	if err != nil {
		msg := strings.ToLower(err.Error())
		if strings.Contains(msg, "weapon_kits.kit_key") &&
			(strings.Contains(msg, "unique") || strings.Contains(msg, "duplicate")) {
			return 0, fmt.Errorf("db: CreateWeaponKit: kit_key %q already exists", kitKey)
		}
		return 0, fmt.Errorf("db: CreateWeaponKit: %w", err)
	}
	id, _ := res.LastInsertId()
	return int(id), nil
}

// UpdateWeaponKit updates an existing weapon kit by ID.
func (d *DB) UpdateWeaponKit(ctx context.Context, k *WeaponKit) error {
	if k == nil {
		return fmt.Errorf("db: UpdateWeaponKit: kit is nil")
	}
	if strings.TrimSpace(k.KitKey) == "" {
		return fmt.Errorf("db: UpdateWeaponKit: kit_key is required")
	}
	enabled := 0
	if k.Enabled {
		enabled = 1
	}
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE weapon_kits
		        SET display_name = ?, description = ?, enabled = ?
		      WHERE id = ?`),
		k.DisplayName, k.Description, enabled, k.ID)
	if err != nil {
		return fmt.Errorf("db: UpdateWeaponKit: %w", err)
	}
	return nil
}

// DeleteWeaponKit soft-deletes a weapon kit by setting enabled=false.
func (d *DB) DeleteWeaponKit(ctx context.Context, id int) error {
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE weapon_kits SET enabled = ? WHERE id = ?`),
		0, id)
	if err != nil {
		return fmt.Errorf("db: DeleteWeaponKit: %w", err)
	}
	return nil
}

// ListAbilitiesForKit returns all ability entries for a kit, ordered by slot_index.
// Includes entries with enabled=false (caller decides whether to filter).
func (d *DB) ListAbilitiesForKit(ctx context.Context, kitID int) ([]*WeaponKitAbility, error) {
	if kitID <= 0 {
		return nil, fmt.Errorf("db: ListAbilitiesForKit: kitID must be > 0")
	}
	rows, err := d.db.QueryContext(ctx, d.q(`SELECT id, kit_id, ability_id, slot_index,
	                                                CASE WHEN enabled THEN 1 ELSE 0 END AS enabled
	                                           FROM weapon_kit_abilities
	                                          WHERE kit_id = ?
	                                          ORDER BY slot_index`), kitID)
	if err != nil {
		return nil, fmt.Errorf("db: ListAbilitiesForKit: %w", err)
	}
	defer rows.Close()
	var out []*WeaponKitAbility
	for rows.Next() {
		entry := &WeaponKitAbility{}
		var enabled int
		if err := rows.Scan(&entry.ID, &entry.KitID, &entry.AbilityID, &entry.SlotIndex, &enabled); err != nil {
			return nil, fmt.Errorf("db: ListAbilitiesForKit scan: %w", err)
		}
		entry.Enabled = enabled != 0
		out = append(out, entry)
	}
	return out, rows.Err()
}

// SetKitAbilities atomically replaces all ability entries for a kit.
// Operates within a single transaction: other readers see the previous state
// until commit, then the new state. No partial state is visible.
func (d *DB) SetKitAbilities(ctx context.Context, kitID int, abilities []*WeaponKitAbility) error {
	if kitID <= 0 {
		return fmt.Errorf("db: SetKitAbilities: kitID must be > 0")
	}
	usedSlots := make(map[int]struct{}, len(abilities))
	for i, entry := range abilities {
		if entry == nil {
			return fmt.Errorf("db: SetKitAbilities: ability entry at index %d is nil", i)
		}
		if entry.AbilityID <= 0 {
			return fmt.Errorf("db: SetKitAbilities: ability_id must be > 0 at index %d", i)
		}
		if entry.SlotIndex < 0 {
			return fmt.Errorf("db: SetKitAbilities: slot_index must be >= 0 at index %d", i)
		}
		if _, exists := usedSlots[entry.SlotIndex]; exists {
			return fmt.Errorf("db: SetKitAbilities: duplicate slot_index %d", entry.SlotIndex)
		}
		usedSlots[entry.SlotIndex] = struct{}{}
	}

	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("db: SetKitAbilities begin: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	if _, err := tx.ExecContext(ctx, d.q(`DELETE FROM weapon_kit_abilities WHERE kit_id = ?`), kitID); err != nil {
		return fmt.Errorf("db: SetKitAbilities clear: %w", err)
	}

	for _, entry := range abilities {
		enabled := 0
		if entry.Enabled {
			enabled = 1
		}
		if _, err := tx.ExecContext(ctx, d.q(`INSERT INTO weapon_kit_abilities (kit_id, ability_id, slot_index, enabled)
		                                      VALUES (?, ?, ?, ?)`),
			kitID, entry.AbilityID, entry.SlotIndex, enabled); err != nil {
			return fmt.Errorf("db: SetKitAbilities insert slot %d ability %d: %w", entry.SlotIndex, entry.AbilityID, err)
		}
	}

	if err := tx.Commit(); err != nil {
		return fmt.Errorf("db: SetKitAbilities commit: %w", err)
	}
	return nil
}

// ClearKitAbilities removes all ability entries for a kit.
// Useful when permanently removing a kit (caller calls this before DeleteWeaponKit).
func (d *DB) ClearKitAbilities(ctx context.Context, kitID int) error {
	if kitID <= 0 {
		return fmt.Errorf("db: ClearKitAbilities: kitID must be > 0")
	}
	if _, err := d.db.ExecContext(ctx, d.q(`DELETE FROM weapon_kit_abilities WHERE kit_id = ?`), kitID); err != nil {
		return fmt.Errorf("db: ClearKitAbilities: %w", err)
	}
	return nil
}

// ListEquipmentSlotConfigs returns all slot configs ordered by slot_id.
// Returns empty slice if table is empty (no seeds yet).
// hotbar_slots_granted is how many hotbar slots this equipment slot grants.
func (d *DB) ListEquipmentSlotConfigs(ctx context.Context) ([]*EquipmentSlotConfig, error) {
	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT slot_id,
		       CASE WHEN gives_kit THEN 1 ELSE 0 END AS gives_kit,
		       hotbar_slots_granted,
		       CASE WHEN enabled THEN 1 ELSE 0 END AS enabled
		  FROM equipment_slot_config
		 ORDER BY slot_id`))
	if err != nil {
		return nil, fmt.Errorf("db: ListEquipmentSlotConfigs: %w", err)
	}
	defer rows.Close()

	var out []*EquipmentSlotConfig
	for rows.Next() {
		c := &EquipmentSlotConfig{}
		var givesKit, enabled int
		if err := rows.Scan(&c.SlotID, &givesKit, &c.HotbarSlotsGranted, &enabled); err != nil {
			return nil, fmt.Errorf("db: ListEquipmentSlotConfigs scan: %w", err)
		}
		c.GivesKit = givesKit != 0
		c.Enabled = enabled != 0
		out = append(out, c)
	}
	return out, rows.Err()
}

// GetEquipmentSlotConfig returns config for a specific slot, or nil if not found.
// "Not found" means the slot has no config row.
// hotbar_slots_granted is how many hotbar slots this equipment slot grants.
func (d *DB) GetEquipmentSlotConfig(ctx context.Context, slotID int) (*EquipmentSlotConfig, error) {
	if slotID < 0 {
		return nil, fmt.Errorf("db: GetEquipmentSlotConfig: slotID must be >= 0")
	}
	c := &EquipmentSlotConfig{}
	var givesKit, enabled int
	err := d.db.QueryRowContext(ctx, d.q(`
		SELECT slot_id,
		       CASE WHEN gives_kit THEN 1 ELSE 0 END AS gives_kit,
		       hotbar_slots_granted,
		       CASE WHEN enabled THEN 1 ELSE 0 END AS enabled
		  FROM equipment_slot_config
		 WHERE slot_id = ?`), slotID).
		Scan(&c.SlotID, &givesKit, &c.HotbarSlotsGranted, &enabled)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("db: GetEquipmentSlotConfig: %w", err)
	}
	c.GivesKit = givesKit != 0
	c.Enabled = enabled != 0
	return c, nil
}

// UpdateEquipmentSlotConfig upserts the config for a slot.
func (d *DB) UpdateEquipmentSlotConfig(ctx context.Context, c *EquipmentSlotConfig) error {
	if c == nil {
		return fmt.Errorf("db: UpdateEquipmentSlotConfig: config is nil")
	}
	if c.SlotID < 0 {
		return fmt.Errorf("db: UpdateEquipmentSlotConfig: slotID must be >= 0")
	}
	if c.HotbarSlotsGranted < 0 {
		return fmt.Errorf("db: UpdateEquipmentSlotConfig: hotbar_slots_granted must be >= 0")
	}

	givesKit := 0
	if c.GivesKit {
		givesKit = 1
	}
	enabled := 0
	if c.Enabled {
		enabled = 1
	}

	_, err := d.db.ExecContext(ctx, d.q(`
		INSERT INTO equipment_slot_config (slot_id, gives_kit, hotbar_slots_granted, enabled)
		VALUES (?, ?, ?, ?)
		ON CONFLICT(slot_id) DO UPDATE SET
		    gives_kit = excluded.gives_kit,
		    hotbar_slots_granted = excluded.hotbar_slots_granted,
		    enabled = excluded.enabled`),
		c.SlotID, givesKit, c.HotbarSlotsGranted, enabled)
	if err != nil {
		return fmt.Errorf("db: UpdateEquipmentSlotConfig: %w", err)
	}
	return nil
}

// ListLoadoutForCharKit returns the loadout entries for a character+kit,
// ordered by slot_index. Returns empty slice if loadout doesn't exist yet.
func (d *DB) ListLoadoutForCharKit(ctx context.Context, charID string, kitID int) ([]*CharacterSkillLoadout, error) {
	if strings.TrimSpace(charID) == "" {
		return nil, fmt.Errorf("db: ListLoadoutForCharKit: characterID is required")
	}
	if kitID <= 0 {
		return nil, fmt.Errorf("db: ListLoadoutForCharKit: kitID must be > 0")
	}
	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT id, character_id, kit_id, slot_index, ability_id
		  FROM character_skill_loadouts
		 WHERE character_id = ? AND kit_id = ?
		 ORDER BY slot_index`), charID, kitID)
	if err != nil {
		return nil, fmt.Errorf("db: ListLoadoutForCharKit: %w", err)
	}
	defer rows.Close()

	var out []*CharacterSkillLoadout
	for rows.Next() {
		entry := &CharacterSkillLoadout{}
		if err := rows.Scan(&entry.ID, &entry.CharacterID, &entry.KitID, &entry.SlotIndex, &entry.AbilityID); err != nil {
			return nil, fmt.Errorf("db: ListLoadoutForCharKit scan: %w", err)
		}
		out = append(out, entry)
	}
	return out, rows.Err()
}

// SetLoadoutForCharKit atomically replaces the loadout for a character+kit.
// Operates within a single transaction: readers see the previous state until
// commit, then the new state. No partial state is visible.
func (d *DB) SetLoadoutForCharKit(ctx context.Context, charID string, kitID int, entries []*CharacterSkillLoadout) error {
	if strings.TrimSpace(charID) == "" {
		return fmt.Errorf("db: SetLoadoutForCharKit: characterID is required")
	}
	if kitID <= 0 {
		return fmt.Errorf("db: SetLoadoutForCharKit: kitID must be > 0")
	}

	usedSlots := make(map[int]struct{}, len(entries))
	for i, entry := range entries {
		if entry == nil {
			return fmt.Errorf("db: SetLoadoutForCharKit: entry at index %d is nil", i)
		}
		if entry.AbilityID <= 0 {
			return fmt.Errorf("db: SetLoadoutForCharKit: abilityID must be > 0 at index %d", i)
		}
		if entry.SlotIndex < 0 {
			return fmt.Errorf("db: SetLoadoutForCharKit: slot_index must be >= 0 at index %d", i)
		}
		if _, exists := usedSlots[entry.SlotIndex]; exists {
			return fmt.Errorf("db: SetLoadoutForCharKit: duplicate slot_index %d", entry.SlotIndex)
		}
		usedSlots[entry.SlotIndex] = struct{}{}
	}

	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("db: SetLoadoutForCharKit begin: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck

	if _, err := tx.ExecContext(ctx, d.q(`DELETE FROM character_skill_loadouts WHERE character_id = ? AND kit_id = ?`), charID, kitID); err != nil {
		return fmt.Errorf("db: SetLoadoutForCharKit clear: %w", err)
	}

	for _, entry := range entries {
		if _, err := tx.ExecContext(ctx, d.q(`
			INSERT INTO character_skill_loadouts (character_id, kit_id, slot_index, ability_id)
			VALUES (?, ?, ?, ?)`),
			charID, kitID, entry.SlotIndex, entry.AbilityID); err != nil {
			return fmt.Errorf("db: SetLoadoutForCharKit insert slot %d ability %d: %w", entry.SlotIndex, entry.AbilityID, err)
		}
	}

	if err := tx.Commit(); err != nil {
		return fmt.Errorf("db: SetLoadoutForCharKit commit: %w", err)
	}
	return nil
}

// ClearLoadoutForCharKit removes all loadout entries for a character+kit.
func (d *DB) ClearLoadoutForCharKit(ctx context.Context, charID string, kitID int) error {
	if strings.TrimSpace(charID) == "" {
		return fmt.Errorf("db: ClearLoadoutForCharKit: characterID is required")
	}
	if kitID <= 0 {
		return fmt.Errorf("db: ClearLoadoutForCharKit: kitID must be > 0")
	}
	if _, err := d.db.ExecContext(ctx, d.q(`
		DELETE FROM character_skill_loadouts
		 WHERE character_id = ? AND kit_id = ?`), charID, kitID); err != nil {
		return fmt.Errorf("db: ClearLoadoutForCharKit: %w", err)
	}
	return nil
}

// DeleteAllLoadoutsForCharacter removes all loadout entries for a character.
func (d *DB) DeleteAllLoadoutsForCharacter(ctx context.Context, charID string) error {
	if strings.TrimSpace(charID) == "" {
		return fmt.Errorf("db: DeleteAllLoadoutsForCharacter: characterID is required")
	}
	if _, err := d.db.ExecContext(ctx, d.q(`
		DELETE FROM character_skill_loadouts
		 WHERE character_id = ?`), charID); err != nil {
		return fmt.Errorf("db: DeleteAllLoadoutsForCharacter: %w", err)
	}
	return nil
}

// GetCharacterSkillProgress returns the progress row for a (char, ability).
// Returns nil if no row exists (player never used this skill).
func (d *DB) GetCharacterSkillProgress(ctx context.Context, charID string, abilityID int) (*CharacterSkillProgress, error) {
	if strings.TrimSpace(charID) == "" {
		return nil, fmt.Errorf("db: GetCharacterSkillProgress: characterID is required")
	}
	if abilityID <= 0 {
		return nil, fmt.Errorf("db: GetCharacterSkillProgress: abilityID must be > 0")
	}

	row := &CharacterSkillProgress{}
	err := d.db.QueryRowContext(ctx, d.q(`
		SELECT id, character_id, ability_id, xp, level
		  FROM character_skill_progress
		 WHERE character_id = ? AND ability_id = ?`), charID, abilityID).
		Scan(&row.ID, &row.CharacterID, &row.AbilityID, &row.XP, &row.Level)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("db: GetCharacterSkillProgress: %w", err)
	}
	return row, nil
}

// ListCharacterSkillProgress returns all progress rows for a character.
// Ordered by ability_id. Empty slice if player has no progress yet.
func (d *DB) ListCharacterSkillProgress(ctx context.Context, charID string) ([]*CharacterSkillProgress, error) {
	if strings.TrimSpace(charID) == "" {
		return nil, fmt.Errorf("db: ListCharacterSkillProgress: characterID is required")
	}

	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT id, character_id, ability_id, xp, level
		  FROM character_skill_progress
		 WHERE character_id = ?
		 ORDER BY ability_id`), charID)
	if err != nil {
		return nil, fmt.Errorf("db: ListCharacterSkillProgress: %w", err)
	}
	defer rows.Close()

	var out []*CharacterSkillProgress
	for rows.Next() {
		entry := &CharacterSkillProgress{}
		if err := rows.Scan(&entry.ID, &entry.CharacterID, &entry.AbilityID, &entry.XP, &entry.Level); err != nil {
			return nil, fmt.Errorf("db: ListCharacterSkillProgress scan: %w", err)
		}
		out = append(out, entry)
	}
	return out, rows.Err()
}

// UpsertCharacterSkillProgress creates or updates a progress row.
func (d *DB) UpsertCharacterSkillProgress(ctx context.Context, p *CharacterSkillProgress) error {
	if p == nil {
		return fmt.Errorf("db: UpsertCharacterSkillProgress: progress is nil")
	}
	if strings.TrimSpace(p.CharacterID) == "" {
		return fmt.Errorf("db: UpsertCharacterSkillProgress: characterID is required")
	}
	if p.AbilityID <= 0 {
		return fmt.Errorf("db: UpsertCharacterSkillProgress: abilityID must be > 0")
	}
	if p.XP < 0 {
		return fmt.Errorf("db: UpsertCharacterSkillProgress: xp must be >= 0")
	}
	if p.Level < 1 {
		return fmt.Errorf("db: UpsertCharacterSkillProgress: level must be >= 1")
	}

	const maxAttempts = 3
	backoffs := [...]time.Duration{
		10 * time.Millisecond,
		50 * time.Millisecond,
		200 * time.Millisecond,
	}

	var lastErr error
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		_, err := d.db.ExecContext(ctx, d.q(`
			INSERT INTO character_skill_progress (character_id, ability_id, xp, level)
			VALUES (?, ?, ?, ?)
			ON CONFLICT(character_id, ability_id) DO UPDATE SET
			    xp = excluded.xp,
			    level = excluded.level`),
			p.CharacterID, p.AbilityID, p.XP, p.Level)
		if err == nil {
			if attempt > 1 {
				log.Printf("db: UpsertCharacterSkillProgress succeeded after retry attempts=%d char=%s ability=%d",
					attempt-1, p.CharacterID, p.AbilityID)
			}
			return nil
		}

		if !isSQLiteBusyError(err) {
			return fmt.Errorf("db: UpsertCharacterSkillProgress: %w", err)
		}

		lastErr = err
		if attempt == maxAttempts {
			break
		}

		wait := backoffs[attempt-1]
		log.Printf("db: UpsertCharacterSkillProgress busy retry attempt=%d/%d char=%s ability=%d wait=%s err=%v",
			attempt, maxAttempts, p.CharacterID, p.AbilityID, wait, err)

		timer := time.NewTimer(wait)
		select {
		case <-ctx.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return fmt.Errorf("db: UpsertCharacterSkillProgress canceled while retrying: %w", ctx.Err())
		case <-timer.C:
		}
	}

	if lastErr != nil {
		return fmt.Errorf("db: UpsertCharacterSkillProgress: sqlite busy/locked after %d attempts: %w", maxAttempts, lastErr)
	}
	return fmt.Errorf("db: UpsertCharacterSkillProgress: failed after %d attempts", maxAttempts)
}

func isSQLiteBusyError(err error) bool {
	if err == nil {
		return false
	}

	// modernc.org/sqlite exposes errors with Code() int; extended result codes
	// keep the primary code in the low byte.
	var sqliteCoder interface{ Code() int }
	if errors.As(err, &sqliteCoder) {
		primaryCode := sqliteCoder.Code() & 0xff
		if primaryCode == 5 || primaryCode == 6 { // SQLITE_BUSY / SQLITE_LOCKED
			return true
		}
	}

	msgUpper := strings.ToUpper(err.Error())
	msgLower := strings.ToLower(err.Error())
	return strings.Contains(msgUpper, "SQLITE_BUSY") ||
		strings.Contains(msgUpper, "SQLITE_LOCKED") ||
		strings.Contains(msgLower, "database is locked")
}

// GetCharacterProgressionConfig returns the singleton character progression
// config. If none exists, seeds defaults and returns the new row.
func (d *DB) GetCharacterProgressionConfig(ctx context.Context) (*CharacterProgressionConfig, error) {
	load := func() (*CharacterProgressionConfig, error) {
		cfg := &CharacterProgressionConfig{}
		err := d.db.QueryRowContext(ctx, d.q(`
			SELECT id, max_level, xp_curve_type, xp_curve_base, xp_curve_factor,
			       xp_curve_exponent, xp_irregularity,
			       stat_points_per_level, initial_stat_value, respec_free_until_level, respec_cost_gold
			  FROM character_progression_config
			 ORDER BY id
			 LIMIT 1`)).
			Scan(&cfg.ID, &cfg.MaxLevel, &cfg.XPCurveType, &cfg.XPCurveBase, &cfg.XPCurveFactor,
				&cfg.XPCurveExponent, &cfg.XPIrregularity,
				&cfg.StatPointsPerLevel, &cfg.InitialStatValue, &cfg.RespecFreeUntilLevel, &cfg.RespecCostGold)
		if err == sql.ErrNoRows {
			return nil, nil
		}
		if err != nil {
			return nil, err
		}
		return cfg, nil
	}

	cfg, err := load()
	if err != nil {
		return nil, fmt.Errorf("db: GetCharacterProgressionConfig: %w", err)
	}
	if cfg != nil {
		return cfg, nil
	}

	if err := d.SeedDefaultCharacterProgressionConfig(ctx); err != nil {
		return nil, err
	}
	cfg, err = load()
	if err != nil {
		return nil, fmt.Errorf("db: GetCharacterProgressionConfig: %w", err)
	}
	if cfg == nil {
		return nil, fmt.Errorf("db: GetCharacterProgressionConfig: config row missing after seed")
	}
	return cfg, nil
}

// UpdateCharacterProgressionConfig updates the singleton character progression
// config (id=1).
func (d *DB) UpdateCharacterProgressionConfig(ctx context.Context, c *CharacterProgressionConfig) error {
	if c == nil {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: config is nil")
	}
	if c.MaxLevel < 1 {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: max_level must be >= 1")
	}
	if c.XPCurveBase <= 0 {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: xp_curve_base must be > 0")
	}
	if c.XPCurveFactor <= 0 {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: xp_curve_factor must be > 0")
	}
	if c.XPCurveExponent <= 0 {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: xp_curve_exponent must be > 0")
	}
	if c.XPIrregularity < 0 || c.XPIrregularity > 1 {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: xp_irregularity must be between 0 and 1")
	}
	if c.StatPointsPerLevel < 0 {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: stat_points_per_level must be >= 0")
	}
	if c.InitialStatValue < 1 {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: initial_stat_value must be >= 1")
	}
	if c.RespecFreeUntilLevel < 0 {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: respec_free_until_level must be >= 0")
	}
	if c.RespecCostGold < 0 {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: respec_cost_gold must be >= 0")
	}

	curveType := strings.ToLower(strings.TrimSpace(c.XPCurveType))
	if curveType == "" {
		curveType = "irregular"
	}
	switch curveType {
	case "irregular", "quadratic", "linear", "exponential":
	default:
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: unsupported xp_curve_type %q", c.XPCurveType)
	}

	_, err := d.db.ExecContext(ctx, d.q(`
		INSERT INTO character_progression_config
		    (id, max_level, xp_curve_type, xp_curve_base, xp_curve_factor, xp_curve_exponent, xp_irregularity,
		     stat_points_per_level, initial_stat_value, respec_free_until_level, respec_cost_gold)
		VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
		    max_level = excluded.max_level,
		    xp_curve_type = excluded.xp_curve_type,
		    xp_curve_base = excluded.xp_curve_base,
		    xp_curve_factor = excluded.xp_curve_factor,
		    xp_curve_exponent = excluded.xp_curve_exponent,
		    xp_irregularity = excluded.xp_irregularity,
		    stat_points_per_level = excluded.stat_points_per_level,
		    initial_stat_value = excluded.initial_stat_value,
		    respec_free_until_level = excluded.respec_free_until_level,
		    respec_cost_gold = excluded.respec_cost_gold`),
		c.MaxLevel, curveType, c.XPCurveBase, c.XPCurveFactor, c.XPCurveExponent, c.XPIrregularity,
		c.StatPointsPerLevel, c.InitialStatValue, c.RespecFreeUntilLevel, c.RespecCostGold)
	if err != nil {
		return fmt.Errorf("db: UpdateCharacterProgressionConfig: %w", err)
	}
	return nil
}

// GetCharacterPrimaryStatsForLevel returns primary stat values for one level.
// Returns nil when no row exists for that level.
func (d *DB) GetCharacterPrimaryStatsForLevel(ctx context.Context, level int) (*CharacterPrimaryStatsLevel, error) {
	if level < 1 {
		return nil, fmt.Errorf("db: GetCharacterPrimaryStatsForLevel: level must be >= 1")
	}

	row := &CharacterPrimaryStatsLevel{}
	err := d.db.QueryRowContext(ctx, d.q(`
		SELECT level, strength, dexterity, intelligence, wisdom, perception
		  FROM character_primary_stats_per_level
		 WHERE level = ?`), level).
		Scan(&row.Level, &row.Strength, &row.Dexterity, &row.Intelligence, &row.Wisdom, &row.Perception)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("db: GetCharacterPrimaryStatsForLevel: %w", err)
	}
	return row, nil
}

// ListCharacterPrimaryStatsPerLevel returns all configured level rows ordered
// by level ascending.
func (d *DB) ListCharacterPrimaryStatsPerLevel(ctx context.Context) ([]*CharacterPrimaryStatsLevel, error) {
	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT level, strength, dexterity, intelligence, wisdom, perception
		  FROM character_primary_stats_per_level
		 ORDER BY level`))
	if err != nil {
		return nil, fmt.Errorf("db: ListCharacterPrimaryStatsPerLevel: %w", err)
	}
	defer rows.Close()

	var out []*CharacterPrimaryStatsLevel
	for rows.Next() {
		entry := &CharacterPrimaryStatsLevel{}
		if err := rows.Scan(
			&entry.Level, &entry.Strength, &entry.Dexterity,
			&entry.Intelligence, &entry.Wisdom, &entry.Perception,
		); err != nil {
			return nil, fmt.Errorf("db: ListCharacterPrimaryStatsPerLevel scan: %w", err)
		}
		out = append(out, entry)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("db: ListCharacterPrimaryStatsPerLevel rows: %w", err)
	}
	return out, nil
}

// UpsertCharacterPrimaryStatsLevel inserts or updates one level row.
func (d *DB) UpsertCharacterPrimaryStatsLevel(ctx context.Context, s *CharacterPrimaryStatsLevel) error {
	if s == nil {
		return fmt.Errorf("db: UpsertCharacterPrimaryStatsLevel: stats is nil")
	}
	if s.Level < 1 {
		return fmt.Errorf("db: UpsertCharacterPrimaryStatsLevel: level must be >= 1")
	}
	if s.Strength < 0 || s.Dexterity < 0 || s.Intelligence < 0 || s.Wisdom < 0 || s.Perception < 0 {
		return fmt.Errorf("db: UpsertCharacterPrimaryStatsLevel: stats must be >= 0")
	}

	_, err := d.db.ExecContext(ctx, d.q(`
		INSERT INTO character_primary_stats_per_level
		    (level, strength, dexterity, intelligence, wisdom, perception)
		VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT(level) DO UPDATE SET
		    strength = excluded.strength,
		    dexterity = excluded.dexterity,
		    intelligence = excluded.intelligence,
		    wisdom = excluded.wisdom,
		    perception = excluded.perception`),
		s.Level, s.Strength, s.Dexterity, s.Intelligence, s.Wisdom, s.Perception)
	if err != nil {
		return fmt.Errorf("db: UpsertCharacterPrimaryStatsLevel: %w", err)
	}
	return nil
}

// GetKillXPScalingConfig returns the singleton kill XP scaling config.
// If none exists, seeds defaults and returns the new row.
func (d *DB) GetKillXPScalingConfig(ctx context.Context) (*KillXPScalingConfig, error) {
	load := func() (*KillXPScalingConfig, error) {
		cfg := &KillXPScalingConfig{}
		err := d.db.QueryRowContext(ctx, d.q(`
			SELECT id, base_xp_per_npc_level, level_diff_coefficient, multiplier_min, multiplier_max,
			       mastery_xp_per_mob_level, mastery_killing_blow_mult, mastery_window_timeout_ms
			  FROM kill_xp_scaling_config
			 ORDER BY id
			 LIMIT 1`)).
			Scan(&cfg.ID, &cfg.BaseXPPerNPCLevel, &cfg.LevelDiffCoefficient, &cfg.MultiplierMin, &cfg.MultiplierMax,
				&cfg.MasteryXPPerMobLevel, &cfg.MasteryKillingBlow, &cfg.MasteryWindowTimeout)
		if err == sql.ErrNoRows {
			return nil, nil
		}
		if err != nil {
			return nil, err
		}
		return cfg, nil
	}

	cfg, err := load()
	if err != nil {
		return nil, fmt.Errorf("db: GetKillXPScalingConfig: %w", err)
	}
	if cfg != nil {
		return cfg, nil
	}

	if err := d.SeedDefaultKillXPScalingConfig(ctx); err != nil {
		return nil, err
	}
	cfg, err = load()
	if err != nil {
		return nil, fmt.Errorf("db: GetKillXPScalingConfig: %w", err)
	}
	if cfg == nil {
		return nil, fmt.Errorf("db: GetKillXPScalingConfig: config row missing after seed")
	}
	return cfg, nil
}

// UpdateKillXPScalingConfig updates the singleton kill XP scaling config
// (id=1).
func (d *DB) UpdateKillXPScalingConfig(ctx context.Context, c *KillXPScalingConfig) error {
	if c == nil {
		return fmt.Errorf("db: UpdateKillXPScalingConfig: config is nil")
	}
	if c.BaseXPPerNPCLevel < 0 {
		return fmt.Errorf("db: UpdateKillXPScalingConfig: base_xp_per_npc_level must be >= 0")
	}
	if c.MultiplierMin < 0 {
		return fmt.Errorf("db: UpdateKillXPScalingConfig: multiplier_min must be >= 0")
	}
	if c.MultiplierMax < c.MultiplierMin {
		return fmt.Errorf("db: UpdateKillXPScalingConfig: multiplier_max must be >= multiplier_min")
	}
	if c.MasteryXPPerMobLevel < 1 {
		return fmt.Errorf("db: UpdateKillXPScalingConfig: mastery_xp_per_mob_level must be >= 1")
	}
	if c.MasteryKillingBlow < 1.0 {
		return fmt.Errorf("db: UpdateKillXPScalingConfig: mastery_killing_blow_mult must be >= 1.0")
	}
	if c.MasteryWindowTimeout < 1000 {
		return fmt.Errorf("db: UpdateKillXPScalingConfig: mastery_window_timeout_ms must be >= 1000")
	}

	_, err := d.db.ExecContext(ctx, d.q(`
		INSERT INTO kill_xp_scaling_config
		    (id, base_xp_per_npc_level, level_diff_coefficient, multiplier_min, multiplier_max,
		     mastery_xp_per_mob_level, mastery_killing_blow_mult, mastery_window_timeout_ms)
		VALUES (1, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
		    base_xp_per_npc_level = excluded.base_xp_per_npc_level,
		    level_diff_coefficient = excluded.level_diff_coefficient,
		    multiplier_min = excluded.multiplier_min,
		    multiplier_max = excluded.multiplier_max,
		    mastery_xp_per_mob_level = excluded.mastery_xp_per_mob_level,
		    mastery_killing_blow_mult = excluded.mastery_killing_blow_mult,
		    mastery_window_timeout_ms = excluded.mastery_window_timeout_ms`),
		c.BaseXPPerNPCLevel, c.LevelDiffCoefficient, c.MultiplierMin, c.MultiplierMax,
		c.MasteryXPPerMobLevel, c.MasteryKillingBlow, c.MasteryWindowTimeout)
	if err != nil {
		return fmt.Errorf("db: UpdateKillXPScalingConfig: %w", err)
	}
	return nil
}

// ListFXTemplates returns enabled FX templates ordered by id.
func (d *DB) ListFXTemplates(ctx context.Context) ([]FXTemplate, error) {
	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT id, fx_key, display_name,
		       burst_count, stream_interval, lifetime_seconds,
		       speed_min, speed_max, velocity_bias_x, velocity_bias_y, velocity_bias_z, velocity_spread,
		       color_start_r, color_start_g, color_start_b, color_start_a,
		       color_end_r, color_end_g, color_end_b, color_end_a,
		       size_start, size_end, texture_path, enabled
		  FROM fx_templates
 		 WHERE enabled = 1
		 ORDER BY id`))
	if err != nil {
		return nil, fmt.Errorf("db: ListFXTemplates: %w", err)
	}
	defer rows.Close()

	var out []FXTemplate
	for rows.Next() {
		var t FXTemplate
		var enabledRaw interface{}
		if err := rows.Scan(
			&t.ID, &t.Key, &t.DisplayName,
			&t.BurstCount, &t.StreamInterval, &t.LifetimeSeconds,
			&t.SpeedMin, &t.SpeedMax, &t.VelocityBiasX, &t.VelocityBiasY, &t.VelocityBiasZ, &t.VelocitySpread,
			&t.ColorStartR, &t.ColorStartG, &t.ColorStartB, &t.ColorStartA,
			&t.ColorEndR, &t.ColorEndG, &t.ColorEndB, &t.ColorEndA,
			&t.SizeStart, &t.SizeEnd, &t.TexturePath, &enabledRaw,
		); err != nil {
			return nil, fmt.Errorf("db: ListFXTemplates scan: %w", err)
		}
		t.Enabled = boolFromDB(enabledRaw)
		out = append(out, t)
	}
	return out, rows.Err()
}

// GetFXTemplateByID returns one FX template by id, or nil if it doesn't exist.
func (d *DB) GetFXTemplateByID(ctx context.Context, id int) (*FXTemplate, error) {
	if id <= 0 {
		return nil, fmt.Errorf("db: GetFXTemplateByID: id must be > 0")
	}
	var t FXTemplate
	var enabledRaw interface{}
	err := d.db.QueryRowContext(ctx, d.q(`
		SELECT id, fx_key, display_name,
		       burst_count, stream_interval, lifetime_seconds,
		       speed_min, speed_max, velocity_bias_x, velocity_bias_y, velocity_bias_z, velocity_spread,
		       color_start_r, color_start_g, color_start_b, color_start_a,
		       color_end_r, color_end_g, color_end_b, color_end_a,
		       size_start, size_end, texture_path, enabled
		  FROM fx_templates
		 WHERE id = ?`), id).
		Scan(
			&t.ID, &t.Key, &t.DisplayName,
			&t.BurstCount, &t.StreamInterval, &t.LifetimeSeconds,
			&t.SpeedMin, &t.SpeedMax, &t.VelocityBiasX, &t.VelocityBiasY, &t.VelocityBiasZ, &t.VelocitySpread,
			&t.ColorStartR, &t.ColorStartG, &t.ColorStartB, &t.ColorStartA,
			&t.ColorEndR, &t.ColorEndG, &t.ColorEndB, &t.ColorEndA,
			&t.SizeStart, &t.SizeEnd, &t.TexturePath, &enabledRaw,
		)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("db: GetFXTemplateByID: %w", err)
	}
	t.Enabled = boolFromDB(enabledRaw)
	return &t, nil
}

// GetFXTemplateByKey returns one FX template by key, or nil if it doesn't exist.
func (d *DB) GetFXTemplateByKey(ctx context.Context, key string) (*FXTemplate, error) {
	key = strings.TrimSpace(key)
	if key == "" {
		return nil, fmt.Errorf("db: GetFXTemplateByKey: key is required")
	}
	var t FXTemplate
	var enabledRaw interface{}
	err := d.db.QueryRowContext(ctx, d.q(`
		SELECT id, fx_key, display_name,
		       burst_count, stream_interval, lifetime_seconds,
		       speed_min, speed_max, velocity_bias_x, velocity_bias_y, velocity_bias_z, velocity_spread,
		       color_start_r, color_start_g, color_start_b, color_start_a,
		       color_end_r, color_end_g, color_end_b, color_end_a,
		       size_start, size_end, texture_path, enabled
		  FROM fx_templates
		 WHERE fx_key = ?`), key).
		Scan(
			&t.ID, &t.Key, &t.DisplayName,
			&t.BurstCount, &t.StreamInterval, &t.LifetimeSeconds,
			&t.SpeedMin, &t.SpeedMax, &t.VelocityBiasX, &t.VelocityBiasY, &t.VelocityBiasZ, &t.VelocitySpread,
			&t.ColorStartR, &t.ColorStartG, &t.ColorStartB, &t.ColorStartA,
			&t.ColorEndR, &t.ColorEndG, &t.ColorEndB, &t.ColorEndA,
			&t.SizeStart, &t.SizeEnd, &t.TexturePath, &enabledRaw,
		)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("db: GetFXTemplateByKey: %w", err)
	}
	t.Enabled = boolFromDB(enabledRaw)
	return &t, nil
}

// CreateFXTemplate inserts a new FX template and returns its id.
func (d *DB) CreateFXTemplate(ctx context.Context, t *FXTemplate) (int, error) {
	if t == nil {
		return 0, fmt.Errorf("db: CreateFXTemplate: template is nil")
	}
	key := strings.TrimSpace(t.Key)
	if key == "" {
		return 0, fmt.Errorf("db: CreateFXTemplate: fx_key is required")
	}
	enabled := 0
	if t.Enabled {
		enabled = 1
	}

	if d.driver == "postgres" {
		var id int
		err := d.db.QueryRowContext(ctx, d.q(`
			INSERT INTO fx_templates (
				fx_key, display_name,
				burst_count, stream_interval, lifetime_seconds,
				speed_min, speed_max, velocity_bias_x, velocity_bias_y, velocity_bias_z, velocity_spread,
				color_start_r, color_start_g, color_start_b, color_start_a,
				color_end_r, color_end_g, color_end_b, color_end_a,
				size_start, size_end, texture_path, enabled
			) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
			RETURNING id`),
			key, t.DisplayName,
			t.BurstCount, t.StreamInterval, t.LifetimeSeconds,
			t.SpeedMin, t.SpeedMax, t.VelocityBiasX, t.VelocityBiasY, t.VelocityBiasZ, t.VelocitySpread,
			t.ColorStartR, t.ColorStartG, t.ColorStartB, t.ColorStartA,
			t.ColorEndR, t.ColorEndG, t.ColorEndB, t.ColorEndA,
			t.SizeStart, t.SizeEnd, t.TexturePath, enabled,
		).Scan(&id)
		if err != nil {
			return 0, fmt.Errorf("db: CreateFXTemplate: %w", err)
		}
		return id, nil
	}

	res, err := d.db.ExecContext(ctx, d.q(`
		INSERT INTO fx_templates (
			fx_key, display_name,
			burst_count, stream_interval, lifetime_seconds,
			speed_min, speed_max, velocity_bias_x, velocity_bias_y, velocity_bias_z, velocity_spread,
			color_start_r, color_start_g, color_start_b, color_start_a,
			color_end_r, color_end_g, color_end_b, color_end_a,
			size_start, size_end, texture_path, enabled
		) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`),
		key, t.DisplayName,
		t.BurstCount, t.StreamInterval, t.LifetimeSeconds,
		t.SpeedMin, t.SpeedMax, t.VelocityBiasX, t.VelocityBiasY, t.VelocityBiasZ, t.VelocitySpread,
		t.ColorStartR, t.ColorStartG, t.ColorStartB, t.ColorStartA,
		t.ColorEndR, t.ColorEndG, t.ColorEndB, t.ColorEndA,
		t.SizeStart, t.SizeEnd, t.TexturePath, enabled,
	)
	if err != nil {
		return 0, fmt.Errorf("db: CreateFXTemplate: %w", err)
	}
	id, _ := res.LastInsertId()
	return int(id), nil
}

// UpdateFXTemplate updates an existing FX template by id.
func (d *DB) UpdateFXTemplate(ctx context.Context, t *FXTemplate) error {
	if t == nil {
		return fmt.Errorf("db: UpdateFXTemplate: template is nil")
	}
	if t.ID <= 0 {
		return fmt.Errorf("db: UpdateFXTemplate: id must be > 0")
	}
	key := strings.TrimSpace(t.Key)
	if key == "" {
		return fmt.Errorf("db: UpdateFXTemplate: fx_key is required")
	}
	enabled := 0
	if t.Enabled {
		enabled = 1
	}
	_, err := d.db.ExecContext(ctx, d.q(`
		UPDATE fx_templates
		   SET fx_key = ?, display_name = ?,
		       burst_count = ?, stream_interval = ?, lifetime_seconds = ?,
		       speed_min = ?, speed_max = ?, velocity_bias_x = ?, velocity_bias_y = ?, velocity_bias_z = ?, velocity_spread = ?,
		       color_start_r = ?, color_start_g = ?, color_start_b = ?, color_start_a = ?,
		       color_end_r = ?, color_end_g = ?, color_end_b = ?, color_end_a = ?,
		       size_start = ?, size_end = ?, texture_path = ?, enabled = ?
		 WHERE id = ?`),
		key, t.DisplayName,
		t.BurstCount, t.StreamInterval, t.LifetimeSeconds,
		t.SpeedMin, t.SpeedMax, t.VelocityBiasX, t.VelocityBiasY, t.VelocityBiasZ, t.VelocitySpread,
		t.ColorStartR, t.ColorStartG, t.ColorStartB, t.ColorStartA,
		t.ColorEndR, t.ColorEndG, t.ColorEndB, t.ColorEndA,
		t.SizeStart, t.SizeEnd, t.TexturePath, enabled, t.ID,
	)
	if err != nil {
		return fmt.Errorf("db: UpdateFXTemplate: %w", err)
	}
	return nil
}

// UpsertFXTemplate inserts or updates one FX template by key.
func (d *DB) UpsertFXTemplate(ctx context.Context, t *FXTemplate) error {
	if t == nil {
		return fmt.Errorf("db: UpsertFXTemplate: template is nil")
	}
	key := strings.TrimSpace(t.Key)
	if key == "" {
		return fmt.Errorf("db: UpsertFXTemplate: fx_key is required")
	}
	enabled := 0
	if t.Enabled {
		enabled = 1
	}
	_, err := d.db.ExecContext(ctx, d.q(`
		INSERT INTO fx_templates (
			fx_key, display_name,
			burst_count, stream_interval, lifetime_seconds,
			speed_min, speed_max, velocity_bias_x, velocity_bias_y, velocity_bias_z, velocity_spread,
			color_start_r, color_start_g, color_start_b, color_start_a,
			color_end_r, color_end_g, color_end_b, color_end_a,
			size_start, size_end, texture_path, enabled
		) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(fx_key) DO UPDATE SET
			display_name = excluded.display_name,
			burst_count = excluded.burst_count,
			stream_interval = excluded.stream_interval,
			lifetime_seconds = excluded.lifetime_seconds,
			speed_min = excluded.speed_min,
			speed_max = excluded.speed_max,
			velocity_bias_x = excluded.velocity_bias_x,
			velocity_bias_y = excluded.velocity_bias_y,
			velocity_bias_z = excluded.velocity_bias_z,
			velocity_spread = excluded.velocity_spread,
			color_start_r = excluded.color_start_r,
			color_start_g = excluded.color_start_g,
			color_start_b = excluded.color_start_b,
			color_start_a = excluded.color_start_a,
			color_end_r = excluded.color_end_r,
			color_end_g = excluded.color_end_g,
			color_end_b = excluded.color_end_b,
			color_end_a = excluded.color_end_a,
			size_start = excluded.size_start,
			size_end = excluded.size_end,
			texture_path = excluded.texture_path,
			enabled = excluded.enabled`),
		key, t.DisplayName,
		t.BurstCount, t.StreamInterval, t.LifetimeSeconds,
		t.SpeedMin, t.SpeedMax, t.VelocityBiasX, t.VelocityBiasY, t.VelocityBiasZ, t.VelocitySpread,
		t.ColorStartR, t.ColorStartG, t.ColorStartB, t.ColorStartA,
		t.ColorEndR, t.ColorEndG, t.ColorEndB, t.ColorEndA,
		t.SizeStart, t.SizeEnd, t.TexturePath, enabled,
	)
	if err != nil {
		return fmt.Errorf("db: UpsertFXTemplate: %w", err)
	}
	return nil
}

// DeleteFXTemplate performs a soft delete by disabling the template.
func (d *DB) DeleteFXTemplate(ctx context.Context, id int) error {
	if id <= 0 {
		return fmt.Errorf("db: DeleteFXTemplate: id must be > 0")
	}
	_, err := d.db.ExecContext(ctx, d.q(`UPDATE fx_templates SET enabled = ? WHERE id = ?`), 0, id)
	if err != nil {
		return fmt.Errorf("db: DeleteFXTemplate: %w", err)
	}
	return nil
}

// GetSkillProgressionConfig returns the singleton config.
// If none exists, creates default and returns it.
func (d *DB) GetSkillProgressionConfig(ctx context.Context) (*SkillProgressionConfig, error) {
	load := func() (*SkillProgressionConfig, error) {
		cfg := &SkillProgressionConfig{}
		err := d.db.QueryRowContext(ctx, d.q(`
			SELECT id, xp_per_use, max_level, xp_curve_type, xp_curve_base,
			       xp_curve_exponent, xp_irregularity,
			       damage_bonus_per_level, cooldown_redux_per_level
			  FROM skill_progression_config
			 ORDER BY id
			 LIMIT 1`)).
			Scan(&cfg.ID, &cfg.XPPerUse, &cfg.MaxLevel, &cfg.XPCurveType, &cfg.XPCurveBase,
				&cfg.XPCurveExponent, &cfg.XPIrregularity,
				&cfg.DamageBonusPerLevel, &cfg.CooldownReduxPerLevel)
		if err == sql.ErrNoRows {
			return nil, nil
		}
		if err != nil {
			return nil, err
		}
		return cfg, nil
	}

	cfg, err := load()
	if err != nil {
		return nil, fmt.Errorf("db: GetSkillProgressionConfig: %w", err)
	}
	if cfg != nil {
		return cfg, nil
	}

	if err := d.SeedDefaultSkillProgressionConfig(ctx); err != nil {
		return nil, err
	}
	cfg, err = load()
	if err != nil {
		return nil, fmt.Errorf("db: GetSkillProgressionConfig: %w", err)
	}
	if cfg == nil {
		return nil, fmt.Errorf("db: GetSkillProgressionConfig: config row missing after seed")
	}
	return cfg, nil
}

// UpdateSkillProgressionConfig updates the singleton config (id=1).
func (d *DB) UpdateSkillProgressionConfig(ctx context.Context, c *SkillProgressionConfig) error {
	if c == nil {
		return fmt.Errorf("db: UpdateSkillProgressionConfig: config is nil")
	}
	if c.XPPerUse <= 0 {
		return fmt.Errorf("db: UpdateSkillProgressionConfig: xp_per_use must be > 0")
	}
	if c.MaxLevel < 1 {
		return fmt.Errorf("db: UpdateSkillProgressionConfig: max_level must be >= 1")
	}
	if c.XPCurveBase <= 0 {
		return fmt.Errorf("db: UpdateSkillProgressionConfig: xp_curve_base must be > 0")
	}
	if c.XPCurveExponent <= 0 {
		return fmt.Errorf("db: UpdateSkillProgressionConfig: xp_curve_exponent must be > 0")
	}
	if c.XPIrregularity < 0 || c.XPIrregularity > 1 {
		return fmt.Errorf("db: UpdateSkillProgressionConfig: xp_irregularity must be between 0 and 1")
	}
	curveType := strings.ToLower(strings.TrimSpace(c.XPCurveType))
	if curveType == "" {
		curveType = "irregular"
	}
	if curveType != "irregular" && curveType != "linear" && curveType != "quadratic" && curveType != "exponential" {
		return fmt.Errorf("db: UpdateSkillProgressionConfig: unsupported xp_curve_type %q", c.XPCurveType)
	}

	_, err := d.db.ExecContext(ctx, d.q(`
		INSERT INTO skill_progression_config
		    (id, xp_per_use, max_level, xp_curve_type, xp_curve_base, xp_curve_exponent, xp_irregularity,
		     damage_bonus_per_level, cooldown_redux_per_level)
		VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
		    xp_per_use = excluded.xp_per_use,
		    max_level = excluded.max_level,
		    xp_curve_type = excluded.xp_curve_type,
		    xp_curve_base = excluded.xp_curve_base,
		    xp_curve_exponent = excluded.xp_curve_exponent,
		    xp_irregularity = excluded.xp_irregularity,
		    damage_bonus_per_level = excluded.damage_bonus_per_level,
		    cooldown_redux_per_level = excluded.cooldown_redux_per_level`),
		c.XPPerUse, c.MaxLevel, curveType, c.XPCurveBase, c.XPCurveExponent, c.XPIrregularity,
		c.DamageBonusPerLevel, c.CooldownReduxPerLevel)
	if err != nil {
		return fmt.Errorf("db: UpdateSkillProgressionConfig: %w", err)
	}
	return nil
}

// XPRequiredForLevel returns the total XP required to reach a given level.
// Level 1 requires 0 XP.
func XPRequiredForLevel(level int, cfg *SkillProgressionConfig) int {
	if level <= 1 || cfg == nil {
		return 0
	}

	base := cfg.XPCurveBase
	if base <= 0 {
		base = 40
	}
	exponent := cfg.XPCurveExponent
	if exponent <= 0 {
		exponent = 2.0
	}
	return int(world.ComputeXPThreshold(level, cfg.XPCurveType, base, exponent, cfg.XPIrregularity))
}

// XPRequiredForLevelFromAbility returns the total XP required to reach a level
// using the mastery curve configured on the ability template itself.
// Level 1 requires 0 XP.
func XPRequiredForLevelFromAbility(level int, ability *world.AbilityTemplate) int {
	if level <= 1 || ability == nil {
		return 0
	}

	base := ability.MasteryXPCurveBase
	if base <= 0 {
		base = 40
	}
	exponent := ability.MasteryXPCurveExponent
	if exponent <= 0 {
		exponent = 2.0
	}
	return int(world.ComputeXPThreshold(level, ability.MasteryXPCurveType, base, exponent, ability.MasteryXPIrregularity))
}

// ProcessMasteryXPCumulative adds gain to cumulative mastery XP and updates
// level if absolute thresholds are crossed.
func ProcessMasteryXPCumulative(currentXP int, currentLevel int, gain int, ability *world.AbilityTemplate) (newXP int, newLevel int, leveled bool) {
	newXP = currentXP + gain
	if newXP < 0 {
		newXP = 0
	}
	newLevel = currentLevel
	if newLevel < 1 {
		newLevel = 1
	}

	maxLevel := 10
	if ability != nil && ability.MasteryMaxLevel > 0 {
		maxLevel = ability.MasteryMaxLevel
	}
	if maxLevel < 1 {
		maxLevel = 1
	}

	for newLevel < maxLevel {
		nextThreshold := XPRequiredForLevelFromAbility(newLevel+1, ability)
		if newXP < nextThreshold {
			break
		}
		newLevel++
		leveled = true
	}
	return
}

// Deprecated: use ProcessMasteryXPCumulative. Kept for old save-data
// conversion paths.
// ProcessMasteryXPSinceLevel applies XP gain when mastery XP is stored as
// "since last level". Returns (newXP, newLevel, leveled).
func ProcessMasteryXPSinceLevel(currentXP int, currentLevel int, gain int, ability *world.AbilityTemplate) (newXP int, newLevel int, leveled bool) {
	if currentLevel < 1 {
		currentLevel = 1
	}
	if currentXP < 0 {
		currentXP = 0
	}
	if gain <= 0 {
		return currentXP, currentLevel, false
	}

	maxLevel := 10
	if ability != nil && ability.MasteryMaxLevel > 0 {
		maxLevel = ability.MasteryMaxLevel
	}
	if maxLevel < 1 {
		maxLevel = 1
	}

	newXP = currentXP + gain
	newLevel = currentLevel

	for newLevel < maxLevel {
		curThreshold := XPRequiredForLevelFromAbility(newLevel, ability)
		nextThreshold := XPRequiredForLevelFromAbility(newLevel+1, ability)
		delta := nextThreshold - curThreshold
		if delta <= 0 {
			break
		}
		if newXP < delta {
			break
		}
		newXP -= delta
		newLevel++
		leveled = true
	}

	if newXP < 0 {
		newXP = 0
	}
	return
}

// CalculateLevelFromXP returns the mastery level for the provided cumulative XP.
// The result is capped to cfg.MaxLevel and never less than level 1.
func CalculateLevelFromXP(xp int, cfg *SkillProgressionConfig) int {
	if cfg == nil {
		return 1
	}
	if xp < 0 {
		xp = 0
	}

	maxLevel := cfg.MaxLevel
	if maxLevel < 1 {
		maxLevel = 1
	}

	level := 1
	for next := 2; next <= maxLevel; next++ {
		if xp < XPRequiredForLevel(next, cfg) {
			break
		}
		level = next
	}
	return level
}

// findEquipSlotFor returns the best equip slot index for the given slotType.
// Prefers empty slots; falls back to the first valid slot if all occupied.
func (d *DB) findEquipSlotFor(ctx context.Context, charID string, slotType uint8) uint8 {
	rows, _ := d.db.QueryContext(ctx,
		d.q(`SELECT slot FROM character_items WHERE character_id = ? AND slot < 14`),
		charID)
	occupied := make(map[uint8]bool)
	if rows != nil {
		for rows.Next() {
			var s uint8
			_ = rows.Scan(&s)
			occupied[s] = true
		}
		rows.Close()
	}
	first := uint8(0xFF)
	for eq := uint8(0); eq < 14; eq++ {
		if slotTypeMatches(eq, slotType) {
			if first == 0xFF {
				first = eq
			}
			if !occupied[eq] {
				return eq
			}
		}
	}
	if first != 0xFF {
		return first
	}
	return 0
}

// AreaConfig holds per-area settings loaded from area_config.
type AreaConfig struct {
	Name       string
	MusicTrack uint8
	FogDensity float32

	// Environment
	IsOutdoor                    bool
	FogNear                      float32
	FogFar                       float32
	FogR, FogG, FogB             float32 // 0.0Ã¢â‚¬â€œ1.0
	AmbientR, AmbientG, AmbientB uint8
	Gravity                      float32 // 1.0 = normal

	// Gameplay
	PvPEnabled  bool
	EntryScript string
	ExitScript  string

	// Weather probabilities (0Ã¢â‚¬â€œ100 %)
	WeatherRain  uint8
	WeatherSnow  uint8
	WeatherFog   uint8
	WeatherStorm uint8
	WeatherWind  uint8

	// Skybox Ã¢â‚¬â€ filename relative to assets/ibl/ (e.g. "forest.hdr"). Empty = use default.hdr.
	SkyboxHdr string

	// Authoritative render tuning (sent to clients through PAreaConfig).
	SunDirX, SunDirY, SunDirZ       float32
	SunColorR, SunColorG, SunColorB float32
	SunIntensityMul                 float32
	SkyIntensityMul                 float32
	FogDensityMul                   float32
	Volumetrics                     bool

	// Character readability tuning.
	CharShadowLift   float32
	CharRimStrength  float32
	CharRimExponent  float32
	CharMinNdotL     float32
	CharAmbientBoost float32

	// Scene look tuning.
	SceneIblIntensity       float32
	SceneSkyIntensity       float32
	SceneWorldShadowLift    float32
	SceneDirectScale        float32
	SceneAmbientScale       float32
	SceneFlatAmbient        float32
	SceneWorldMinNdotL      float32
	SceneAlbedoMinLuma      float32
	SceneAlbedoLiftStrength float32
	SceneSpecularScale      float32
	SceneExposureFactor     float32
	SceneSunIntensity       float32

	// Post tonemap color grading.
	ColorContrast         float32
	ColorSaturation       float32
	ColorVibrance         float32
	ColorBlackPoint       float32
	ColorVignetteStrength float32
	ColorVignetteSoftness float32

	// Terrain shading tuning (authoritative).
	TerrainTilingMul        float32
	TerrainMacroStrengthMul float32
	TerrainHeightBlendSlop  float32
}

// AreaTrigger is a script-trigger volume in an area (XZ cylinder).
type AreaTrigger struct {
	ID          int
	AreaName    string
	X, Z        float32
	Radius      float32
	Script      string
	Func        string
	TriggerOnce bool
}

// AreaSoundZone is an ambient audio sphere in an area.
type AreaSoundZone struct {
	ID             int
	AreaName       string
	X, Z           float32
	Radius         float32
	SoundName      string
	Volume         int
	LoopIntervalMs int
}

// AreaPortal holds a portal definition loaded from area_portals.
type AreaPortal struct {
	AreaName                     string
	X, Z                         float32
	Radius                       float32
	TargetArea                   string
	DestX, DestY, DestZ, DestYaw float32
}

// LoadAreaConfigs returns all rows from area_config; creates/migrates table if needed.
func (d *DB) LoadAreaConfigs(ctx context.Context) ([]*AreaConfig, error) {
	// Base table (idempotent).
	d.db.ExecContext(ctx, d.q(
		`CREATE TABLE IF NOT EXISTS area_config (
			name        TEXT PRIMARY KEY,
			music_track INTEGER NOT NULL DEFAULT 1,
			fog_density REAL    NOT NULL DEFAULT 0.0
		)`))

	// Migrate: add new columns if they don't exist yet (errors are ignored Ã¢â‚¬â€ column may already exist).
	migs := []string{
		`ALTER TABLE area_config ADD COLUMN is_outdoor    INTEGER NOT NULL DEFAULT 1`,
		`ALTER TABLE area_config ADD COLUMN pvp_enabled   INTEGER NOT NULL DEFAULT 0`,
		`ALTER TABLE area_config ADD COLUMN fog_near      REAL    NOT NULL DEFAULT 300.0`,
		`ALTER TABLE area_config ADD COLUMN fog_far       REAL    NOT NULL DEFAULT 600.0`,
		`ALTER TABLE area_config ADD COLUMN fog_r         REAL    NOT NULL DEFAULT 0.7`,
		`ALTER TABLE area_config ADD COLUMN fog_g         REAL    NOT NULL DEFAULT 0.75`,
		`ALTER TABLE area_config ADD COLUMN fog_b         REAL    NOT NULL DEFAULT 0.8`,
		`ALTER TABLE area_config ADD COLUMN ambient_r     INTEGER NOT NULL DEFAULT 80`,
		`ALTER TABLE area_config ADD COLUMN ambient_g     INTEGER NOT NULL DEFAULT 80`,
		`ALTER TABLE area_config ADD COLUMN ambient_b     INTEGER NOT NULL DEFAULT 90`,
		`ALTER TABLE area_config ADD COLUMN gravity       REAL    NOT NULL DEFAULT 1.0`,
		`ALTER TABLE area_config ADD COLUMN entry_script  TEXT    NOT NULL DEFAULT ''`,
		`ALTER TABLE area_config ADD COLUMN exit_script   TEXT    NOT NULL DEFAULT ''`,
		`ALTER TABLE area_config ADD COLUMN weather_rain  INTEGER NOT NULL DEFAULT 0`,
		`ALTER TABLE area_config ADD COLUMN weather_snow  INTEGER NOT NULL DEFAULT 0`,
		`ALTER TABLE area_config ADD COLUMN weather_fog   INTEGER NOT NULL DEFAULT 0`,
		`ALTER TABLE area_config ADD COLUMN weather_storm INTEGER NOT NULL DEFAULT 0`,
		`ALTER TABLE area_config ADD COLUMN weather_wind  INTEGER NOT NULL DEFAULT 0`,
		`ALTER TABLE area_config ADD COLUMN skybox_hdr    TEXT    NOT NULL DEFAULT ''`,
		`ALTER TABLE area_config ADD COLUMN sun_dir_x      REAL    NOT NULL DEFAULT 0.18`,
		`ALTER TABLE area_config ADD COLUMN sun_dir_y      REAL    NOT NULL DEFAULT 0.96`,
		`ALTER TABLE area_config ADD COLUMN sun_dir_z      REAL    NOT NULL DEFAULT 0.20`,
		`ALTER TABLE area_config ADD COLUMN sun_color_r    REAL    NOT NULL DEFAULT 1.14`,
		`ALTER TABLE area_config ADD COLUMN sun_color_g    REAL    NOT NULL DEFAULT 1.12`,
		`ALTER TABLE area_config ADD COLUMN sun_color_b    REAL    NOT NULL DEFAULT 1.05`,
		`ALTER TABLE area_config ADD COLUMN sun_intensity_mul REAL NOT NULL DEFAULT 1.00`,
		`ALTER TABLE area_config ADD COLUMN sky_intensity_mul REAL NOT NULL DEFAULT 1.00`,
		`ALTER TABLE area_config ADD COLUMN fog_density_mul REAL NOT NULL DEFAULT 0.92`,
		`ALTER TABLE area_config ADD COLUMN volumetrics     INTEGER NOT NULL DEFAULT 1`,
		`ALTER TABLE area_config ADD COLUMN char_shadow_lift   REAL NOT NULL DEFAULT 0.30`,
		`ALTER TABLE area_config ADD COLUMN char_rim_strength  REAL NOT NULL DEFAULT 0.18`,
		`ALTER TABLE area_config ADD COLUMN char_rim_exponent  REAL NOT NULL DEFAULT 2.40`,
		`ALTER TABLE area_config ADD COLUMN char_min_ndotl     REAL NOT NULL DEFAULT 0.10`,
		`ALTER TABLE area_config ADD COLUMN char_ambient_boost REAL NOT NULL DEFAULT 0.12`,
		`ALTER TABLE area_config ADD COLUMN scene_ibl_intensity        REAL NOT NULL DEFAULT 1.00`,
		`ALTER TABLE area_config ADD COLUMN scene_sky_intensity        REAL NOT NULL DEFAULT 1.16`,
		`ALTER TABLE area_config ADD COLUMN scene_world_shadow_lift    REAL NOT NULL DEFAULT 0.10`,
		`ALTER TABLE area_config ADD COLUMN scene_direct_scale         REAL NOT NULL DEFAULT 1.32`,
		`ALTER TABLE area_config ADD COLUMN scene_ambient_scale        REAL NOT NULL DEFAULT 0.88`,
		`ALTER TABLE area_config ADD COLUMN scene_flat_ambient         REAL NOT NULL DEFAULT 0.03`,
		`ALTER TABLE area_config ADD COLUMN scene_world_min_ndotl      REAL NOT NULL DEFAULT 0.05`,
		`ALTER TABLE area_config ADD COLUMN scene_albedo_min_luma      REAL NOT NULL DEFAULT 0.18`,
		`ALTER TABLE area_config ADD COLUMN scene_albedo_lift_strength REAL NOT NULL DEFAULT 0.00`,
		`ALTER TABLE area_config ADD COLUMN scene_specular_scale       REAL NOT NULL DEFAULT 0.88`,
		`ALTER TABLE area_config ADD COLUMN scene_exposure_factor      REAL NOT NULL DEFAULT 1.10`,
		`ALTER TABLE area_config ADD COLUMN scene_sun_intensity        REAL NOT NULL DEFAULT 1.36`,
		`ALTER TABLE area_config ADD COLUMN color_contrast          REAL NOT NULL DEFAULT 1.08`,
		`ALTER TABLE area_config ADD COLUMN color_saturation        REAL NOT NULL DEFAULT 1.08`,
		`ALTER TABLE area_config ADD COLUMN color_vibrance          REAL NOT NULL DEFAULT 0.20`,
		`ALTER TABLE area_config ADD COLUMN color_black_point       REAL NOT NULL DEFAULT 0.010`,
		`ALTER TABLE area_config ADD COLUMN color_vignette_strength REAL NOT NULL DEFAULT 0.04`,
		`ALTER TABLE area_config ADD COLUMN color_vignette_softness REAL NOT NULL DEFAULT 0.55`,
		`ALTER TABLE area_config ADD COLUMN terrain_tiling_mul         REAL NOT NULL DEFAULT 1.00`,
		`ALTER TABLE area_config ADD COLUMN terrain_macro_strength_mul REAL NOT NULL DEFAULT 1.00`,
		`ALTER TABLE area_config ADD COLUMN terrain_height_blend_slop  REAL NOT NULL DEFAULT 0.20`,
	}
	for _, m := range migs {
		d.db.ExecContext(ctx, d.q(m))
	}

	// Seed authoritative render/environment presets for core areas.
	// INSERT OR IGNORE preserves designer-authored rows if they already exist.
	type areaCfgSeed struct {
		name                                                                                                         string
		musicTrack                                                                                                   int
		fogDensity                                                                                                   float32
		isOutdoor, pvpEnabled                                                                                        int
		fogNear, fogFar                                                                                              float32
		fogR, fogG, fogB                                                                                             float32
		ambientR, ambientG, ambientB                                                                                 int
		gravity                                                                                                      float32
		weatherRain, weatherSnow                                                                                     int
		weatherFog, weatherStorm, weatherWind                                                                        int
		skyboxHdr                                                                                                    string
		sunDirX, sunDirY, sunDirZ                                                                                    float32
		sunColorR, sunColorG, sunColorB                                                                              float32
		sunIntensityMul, skyIntensityMul                                                                             float32
		fogDensityMul                                                                                                float32
		volumetrics                                                                                                  int
		charShadowLift, charRimStrength, charRimExponent, charMinNdotL, charAmbientBoost                             float32
		sceneIblIntensity, sceneSkyIntensity, sceneWorldShadowLift, sceneDirectScale, sceneAmbientScale              float32
		sceneFlatAmbient, sceneWorldMinNdotL, sceneAlbedoMinLuma, sceneAlbedoLiftStrength                            float32
		sceneSpecularScale, sceneExposureFactor, sceneSunIntensity                                                   float32
		colorContrast, colorSaturation, colorVibrance, colorBlackPoint, colorVignetteStrength, colorVignetteSoftness float32
		terrainTilingMul, terrainMacroStrengthMul, terrainHeightBlendSlop                                            float32
	}
	seeds := []areaCfgSeed{
		{
			name:       "Training Camp",
			musicTrack: 1,
			fogDensity: 0.0,
			isOutdoor:  1, pvpEnabled: 0,
			fogNear: 260.0, fogFar: 680.0,
			fogR: 0.70, fogG: 0.80, fogB: 0.93,
			ambientR: 96, ambientG: 102, ambientB: 114,
			gravity:     1.0,
			weatherRain: 0, weatherSnow: 0, weatherFog: 0, weatherStorm: 0, weatherWind: 0,
			skyboxHdr: "default.hdr",
			sunDirX:   0.18, sunDirY: 0.96, sunDirZ: 0.20,
			sunColorR: 1.14, sunColorG: 1.12, sunColorB: 1.05,
			sunIntensityMul: 1.00, skyIntensityMul: 1.00, fogDensityMul: 0.92,
			volumetrics:    1,
			charShadowLift: 0.30, charRimStrength: 0.18, charRimExponent: 2.40, charMinNdotL: 0.10, charAmbientBoost: 0.12,
			sceneIblIntensity: 1.00, sceneSkyIntensity: 1.16, sceneWorldShadowLift: 0.10, sceneDirectScale: 1.32, sceneAmbientScale: 0.88,
			sceneFlatAmbient: 0.03, sceneWorldMinNdotL: 0.05, sceneAlbedoMinLuma: 0.18, sceneAlbedoLiftStrength: 0.00,
			sceneSpecularScale: 0.88, sceneExposureFactor: 1.10, sceneSunIntensity: 1.36,
			colorContrast: 1.08, colorSaturation: 1.08, colorVibrance: 0.20, colorBlackPoint: 0.010, colorVignetteStrength: 0.04, colorVignetteSoftness: 0.55,
			terrainTilingMul: 1.10, terrainMacroStrengthMul: 1.20, terrainHeightBlendSlop: 0.24,
		},
		{
			name:       "Starter Zone",
			musicTrack: 1,
			fogDensity: 0.0,
			isOutdoor:  1, pvpEnabled: 0,
			fogNear: 240.0, fogFar: 620.0,
			fogR: 0.72, fogG: 0.80, fogB: 0.90,
			ambientR: 90, ambientG: 96, ambientB: 108,
			gravity:     1.0,
			weatherRain: 0, weatherSnow: 0, weatherFog: 0, weatherStorm: 0, weatherWind: 0,
			skyboxHdr: "default.hdr",
			sunDirX:   0.20, sunDirY: 0.95, sunDirZ: 0.23,
			sunColorR: 1.08, sunColorG: 1.08, sunColorB: 1.04,
			sunIntensityMul: 0.95, skyIntensityMul: 1.00, fogDensityMul: 1.00,
			volumetrics:    1,
			charShadowLift: 0.30, charRimStrength: 0.18, charRimExponent: 2.40, charMinNdotL: 0.10, charAmbientBoost: 0.12,
			sceneIblIntensity: 1.00, sceneSkyIntensity: 1.16, sceneWorldShadowLift: 0.10, sceneDirectScale: 1.32, sceneAmbientScale: 0.88,
			sceneFlatAmbient: 0.03, sceneWorldMinNdotL: 0.05, sceneAlbedoMinLuma: 0.18, sceneAlbedoLiftStrength: 0.00,
			sceneSpecularScale: 0.88, sceneExposureFactor: 1.10, sceneSunIntensity: 1.36,
			colorContrast: 1.08, colorSaturation: 1.08, colorVibrance: 0.20, colorBlackPoint: 0.010, colorVignetteStrength: 0.04, colorVignetteSoftness: 0.55,
			terrainTilingMul: 1.00, terrainMacroStrengthMul: 1.00, terrainHeightBlendSlop: 0.20,
		},
	}
	for _, s := range seeds {
		_, _ = d.db.ExecContext(ctx, d.q(`
			INSERT OR IGNORE INTO area_config (
				name, music_track, fog_density,
				is_outdoor, pvp_enabled,
				fog_near, fog_far, fog_r, fog_g, fog_b,
				ambient_r, ambient_g, ambient_b,
				gravity, entry_script, exit_script,
				weather_rain, weather_snow, weather_fog, weather_storm, weather_wind,
				skybox_hdr,
				sun_dir_x, sun_dir_y, sun_dir_z,
				sun_color_r, sun_color_g, sun_color_b,
				sun_intensity_mul, sky_intensity_mul, fog_density_mul,
				volumetrics,
				char_shadow_lift, char_rim_strength, char_rim_exponent, char_min_ndotl, char_ambient_boost,
				scene_ibl_intensity, scene_sky_intensity, scene_world_shadow_lift, scene_direct_scale, scene_ambient_scale,
				scene_flat_ambient, scene_world_min_ndotl, scene_albedo_min_luma, scene_albedo_lift_strength,
				scene_specular_scale, scene_exposure_factor, scene_sun_intensity,
				color_contrast, color_saturation, color_vibrance, color_black_point, color_vignette_strength, color_vignette_softness,
				terrain_tiling_mul, terrain_macro_strength_mul, terrain_height_blend_slop
			)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '', '', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		`),
			s.name, s.musicTrack, s.fogDensity,
			s.isOutdoor, s.pvpEnabled,
			s.fogNear, s.fogFar, s.fogR, s.fogG, s.fogB,
			s.ambientR, s.ambientG, s.ambientB,
			s.gravity,
			s.weatherRain, s.weatherSnow, s.weatherFog, s.weatherStorm, s.weatherWind,
			s.skyboxHdr,
			s.sunDirX, s.sunDirY, s.sunDirZ,
			s.sunColorR, s.sunColorG, s.sunColorB,
			s.sunIntensityMul, s.skyIntensityMul, s.fogDensityMul,
			s.volumetrics,
			s.charShadowLift, s.charRimStrength, s.charRimExponent, s.charMinNdotL, s.charAmbientBoost,
			s.sceneIblIntensity, s.sceneSkyIntensity, s.sceneWorldShadowLift, s.sceneDirectScale, s.sceneAmbientScale,
			s.sceneFlatAmbient, s.sceneWorldMinNdotL, s.sceneAlbedoMinLuma, s.sceneAlbedoLiftStrength,
			s.sceneSpecularScale, s.sceneExposureFactor, s.sceneSunIntensity,
			s.colorContrast, s.colorSaturation, s.colorVibrance, s.colorBlackPoint, s.colorVignetteStrength, s.colorVignetteSoftness,
			s.terrainTilingMul, s.terrainMacroStrengthMul, s.terrainHeightBlendSlop,
		)

		// Apply canonical render/environment preset to existing rows too.
		// Keeps gameplay/script fields untouched.
		_, _ = d.db.ExecContext(ctx, d.q(`
			UPDATE area_config
			   SET music_track = ?,
			       fog_density = ?,
			       is_outdoor = ?,
			       fog_near = ?, fog_far = ?,
			       fog_r = ?, fog_g = ?, fog_b = ?,
			       ambient_r = ?, ambient_g = ?, ambient_b = ?,
			       skybox_hdr = ?,
			       sun_dir_x = ?, sun_dir_y = ?, sun_dir_z = ?,
			       sun_color_r = ?, sun_color_g = ?, sun_color_b = ?,
			       sun_intensity_mul = ?, sky_intensity_mul = ?, fog_density_mul = ?,
			       volumetrics = ?,
			       char_shadow_lift = ?, char_rim_strength = ?, char_rim_exponent = ?, char_min_ndotl = ?, char_ambient_boost = ?,
			       scene_ibl_intensity = ?, scene_sky_intensity = ?, scene_world_shadow_lift = ?, scene_direct_scale = ?, scene_ambient_scale = ?,
			       scene_flat_ambient = ?, scene_world_min_ndotl = ?, scene_albedo_min_luma = ?, scene_albedo_lift_strength = ?,
			       scene_specular_scale = ?, scene_exposure_factor = ?, scene_sun_intensity = ?,
			       color_contrast = ?, color_saturation = ?, color_vibrance = ?, color_black_point = ?, color_vignette_strength = ?, color_vignette_softness = ?,
			       terrain_tiling_mul = ?, terrain_macro_strength_mul = ?, terrain_height_blend_slop = ?
			 WHERE name = ?
		`),
			s.musicTrack,
			s.fogDensity,
			s.isOutdoor,
			s.fogNear, s.fogFar,
			s.fogR, s.fogG, s.fogB,
			s.ambientR, s.ambientG, s.ambientB,
			s.skyboxHdr,
			s.sunDirX, s.sunDirY, s.sunDirZ,
			s.sunColorR, s.sunColorG, s.sunColorB,
			s.sunIntensityMul, s.skyIntensityMul, s.fogDensityMul,
			s.volumetrics,
			s.charShadowLift, s.charRimStrength, s.charRimExponent, s.charMinNdotL, s.charAmbientBoost,
			s.sceneIblIntensity, s.sceneSkyIntensity, s.sceneWorldShadowLift, s.sceneDirectScale, s.sceneAmbientScale,
			s.sceneFlatAmbient, s.sceneWorldMinNdotL, s.sceneAlbedoMinLuma, s.sceneAlbedoLiftStrength,
			s.sceneSpecularScale, s.sceneExposureFactor, s.sceneSunIntensity,
			s.colorContrast, s.colorSaturation, s.colorVibrance, s.colorBlackPoint, s.colorVignetteStrength, s.colorVignetteSoftness,
			s.terrainTilingMul, s.terrainMacroStrengthMul, s.terrainHeightBlendSlop,
			s.name,
		)
	}

	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT name, music_track, fog_density,
		       is_outdoor, pvp_enabled,
		       fog_near, fog_far, fog_r, fog_g, fog_b,
		       ambient_r, ambient_g, ambient_b,
		       gravity, entry_script, exit_script,
		       weather_rain, weather_snow, weather_fog, weather_storm, weather_wind,
		       skybox_hdr,
		       sun_dir_x, sun_dir_y, sun_dir_z,
		       sun_color_r, sun_color_g, sun_color_b,
		       sun_intensity_mul, sky_intensity_mul, fog_density_mul,
		       volumetrics,
		       char_shadow_lift, char_rim_strength, char_rim_exponent, char_min_ndotl, char_ambient_boost,
		       scene_ibl_intensity, scene_sky_intensity, scene_world_shadow_lift, scene_direct_scale, scene_ambient_scale,
		       scene_flat_ambient, scene_world_min_ndotl, scene_albedo_min_luma, scene_albedo_lift_strength,
		       scene_specular_scale, scene_exposure_factor, scene_sun_intensity,
		       color_contrast, color_saturation, color_vibrance, color_black_point, color_vignette_strength, color_vignette_softness,
		       terrain_tiling_mul, terrain_macro_strength_mul, terrain_height_blend_slop
		FROM area_config ORDER BY name`))
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []*AreaConfig
	for rows.Next() {
		c := &AreaConfig{}
		var (
			track, outdoor, pvp             int
			ambR, ambG, ambB                int
			wRain, wSnow, wFog, wStr, wWind int
			volumetrics                     int
		)
		_ = rows.Scan(
			&c.Name, &track, &c.FogDensity,
			&outdoor, &pvp,
			&c.FogNear, &c.FogFar, &c.FogR, &c.FogG, &c.FogB,
			&ambR, &ambG, &ambB,
			&c.Gravity, &c.EntryScript, &c.ExitScript,
			&wRain, &wSnow, &wFog, &wStr, &wWind,
			&c.SkyboxHdr,
			&c.SunDirX, &c.SunDirY, &c.SunDirZ,
			&c.SunColorR, &c.SunColorG, &c.SunColorB,
			&c.SunIntensityMul, &c.SkyIntensityMul, &c.FogDensityMul,
			&volumetrics,
			&c.CharShadowLift, &c.CharRimStrength, &c.CharRimExponent, &c.CharMinNdotL, &c.CharAmbientBoost,
			&c.SceneIblIntensity, &c.SceneSkyIntensity, &c.SceneWorldShadowLift, &c.SceneDirectScale, &c.SceneAmbientScale,
			&c.SceneFlatAmbient, &c.SceneWorldMinNdotL, &c.SceneAlbedoMinLuma, &c.SceneAlbedoLiftStrength,
			&c.SceneSpecularScale, &c.SceneExposureFactor, &c.SceneSunIntensity,
			&c.ColorContrast, &c.ColorSaturation, &c.ColorVibrance, &c.ColorBlackPoint, &c.ColorVignetteStrength, &c.ColorVignetteSoftness,
			&c.TerrainTilingMul, &c.TerrainMacroStrengthMul, &c.TerrainHeightBlendSlop,
		)
		c.MusicTrack = uint8(track)
		c.IsOutdoor = outdoor != 0
		c.PvPEnabled = pvp != 0
		c.AmbientR, c.AmbientG, c.AmbientB = uint8(ambR), uint8(ambG), uint8(ambB)
		c.WeatherRain, c.WeatherSnow = uint8(wRain), uint8(wSnow)
		c.WeatherFog, c.WeatherStorm, c.WeatherWind = uint8(wFog), uint8(wStr), uint8(wWind)
		c.Volumetrics = volumetrics != 0
		out = append(out, c)
	}
	return out, rows.Err()
}

// LoadAreaTriggers returns all trigger volumes; creates table if absent.
func (d *DB) LoadAreaTriggers(ctx context.Context) ([]*AreaTrigger, error) {
	d.db.ExecContext(ctx, d.q(
		`CREATE TABLE IF NOT EXISTS area_triggers (
			id           INTEGER PRIMARY KEY AUTOINCREMENT,
			area_name    TEXT    NOT NULL DEFAULT '',
			x            REAL    NOT NULL DEFAULT 0,
			z            REAL    NOT NULL DEFAULT 0,
			radius       REAL    NOT NULL DEFAULT 5,
			script       TEXT    NOT NULL DEFAULT '',
			func         TEXT    NOT NULL DEFAULT '',
			trigger_once INTEGER NOT NULL DEFAULT 0
		)`))

	rows, err := d.db.QueryContext(ctx, d.q(
		`SELECT id, area_name, x, z, radius, script, func, trigger_once
		 FROM area_triggers ORDER BY area_name, id`))
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []*AreaTrigger
	for rows.Next() {
		t := &AreaTrigger{}
		var once int
		var x, z, r float64
		_ = rows.Scan(&t.ID, &t.AreaName, &x, &z, &r, &t.Script, &t.Func, &once)
		t.X, t.Z, t.Radius = float32(x), float32(z), float32(r)
		t.TriggerOnce = once != 0
		out = append(out, t)
	}
	return out, rows.Err()
}

// LoadAreaSoundZones returns all sound zones; creates table if absent.
func (d *DB) LoadAreaSoundZones(ctx context.Context) ([]*AreaSoundZone, error) {
	d.db.ExecContext(ctx, d.q(
		`CREATE TABLE IF NOT EXISTS area_sound_zones (
			id               INTEGER PRIMARY KEY AUTOINCREMENT,
			area_name        TEXT    NOT NULL DEFAULT '',
			x                REAL    NOT NULL DEFAULT 0,
			z                REAL    NOT NULL DEFAULT 0,
			radius           REAL    NOT NULL DEFAULT 15,
			sound_name       TEXT    NOT NULL DEFAULT '',
			volume           INTEGER NOT NULL DEFAULT 100,
			loop_interval_ms INTEGER NOT NULL DEFAULT 0
		)`))

	rows, err := d.db.QueryContext(ctx, d.q(
		`SELECT id, area_name, x, z, radius, sound_name, volume, loop_interval_ms
		 FROM area_sound_zones ORDER BY area_name, id`))
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []*AreaSoundZone
	for rows.Next() {
		s := &AreaSoundZone{}
		var x, z, r float64
		_ = rows.Scan(&s.ID, &s.AreaName, &x, &z, &r, &s.SoundName, &s.Volume, &s.LoopIntervalMs)
		s.X, s.Z, s.Radius = float32(x), float32(z), float32(r)
		out = append(out, s)
	}
	return out, rows.Err()
}

// LoadAreaPortals returns all portal definitions; creates the table if absent.
func (d *DB) LoadAreaPortals(ctx context.Context) ([]*AreaPortal, error) {
	d.db.ExecContext(ctx, d.q(
		`CREATE TABLE IF NOT EXISTS area_portals (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			area_name   TEXT    NOT NULL DEFAULT '',
			x           REAL    NOT NULL DEFAULT 0,
			z           REAL    NOT NULL DEFAULT 0,
			radius      REAL    NOT NULL DEFAULT 3,
			target_area TEXT    NOT NULL DEFAULT '',
			dest_x      REAL    NOT NULL DEFAULT 0,
			dest_y      REAL    NOT NULL DEFAULT 0,
			dest_z      REAL    NOT NULL DEFAULT 0,
			dest_yaw    REAL    NOT NULL DEFAULT 0
		)`))

	rows, err := d.db.QueryContext(ctx, d.q(
		`SELECT area_name, x, z, radius, target_area, dest_x, dest_y, dest_z, dest_yaw
		 FROM area_portals ORDER BY area_name, id`))
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []*AreaPortal
	for rows.Next() {
		p := &AreaPortal{}
		var x, z, r, dx, dy, dz, dyaw float64
		_ = rows.Scan(&p.AreaName, &x, &z, &r, &p.TargetArea, &dx, &dy, &dz, &dyaw)
		p.X, p.Z, p.Radius = float32(x), float32(z), float32(r)
		p.DestX, p.DestY, p.DestZ, p.DestYaw = float32(dx), float32(dy), float32(dz), float32(dyaw)
		out = append(out, p)
	}
	return out, rows.Err()
}

// ---------------------------------------------------------------------------
// Spawn Points Ã¢â‚¬â€ authored in GUE, loaded read-only by the server at startup.
// ---------------------------------------------------------------------------

// SpawnPoint mirrors one row in spawn_points.
type SpawnPoint struct {
	ID       int
	Name     string
	AreaName string
	X, Y, Z  float64
	Radius   float64
}

// SpawnPointMob mirrors one row in spawn_point_mobs.
type SpawnPointMob struct {
	ID              int
	SpawnPointID    int
	ActorDefID      int
	Count           int
	Name            string
	Race            string
	Class           string
	Level           int
	Aggressiveness  int
	AggressiveRange float64
	AttackRange     float64
	RespawnDelayMs  int64
}

// migrateV13 creates the spawn_points and spawn_point_mobs tables.
func (d *DB) migrateV13(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS spawn_points (
			id        SERIAL PRIMARY KEY,
			name      VARCHAR(128) NOT NULL DEFAULT 'Spawn Point',
			area_name VARCHAR(128) NOT NULL DEFAULT '',
			x         REAL NOT NULL DEFAULT 0,
			y         REAL NOT NULL DEFAULT 0,
			z         REAL NOT NULL DEFAULT 0,
			radius    REAL NOT NULL DEFAULT 5
		)`)
		exec(`CREATE TABLE IF NOT EXISTS spawn_point_mobs (
			id               SERIAL PRIMARY KEY,
			spawn_point_id   INTEGER NOT NULL DEFAULT 0,
			actor_def_id     INTEGER NOT NULL DEFAULT 0,
			mob_count        INTEGER NOT NULL DEFAULT 1,
			name             VARCHAR(64) NOT NULL DEFAULT 'NPC',
			race             VARCHAR(32) NOT NULL DEFAULT 'Human',
			class            VARCHAR(32) NOT NULL DEFAULT 'Warrior',
			level            INTEGER NOT NULL DEFAULT 1,
			aggressiveness   INTEGER NOT NULL DEFAULT 2,
			aggressive_range REAL NOT NULL DEFAULT 8.0,
			attack_range     REAL NOT NULL DEFAULT 2.0,
			respawn_delay_ms INTEGER NOT NULL DEFAULT 30000
		)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS spawn_points (
			id        INTEGER PRIMARY KEY AUTOINCREMENT,
			name      TEXT NOT NULL DEFAULT 'Spawn Point',
			area_name TEXT NOT NULL DEFAULT '',
			x         REAL NOT NULL DEFAULT 0,
			y         REAL NOT NULL DEFAULT 0,
			z         REAL NOT NULL DEFAULT 0,
			radius    REAL NOT NULL DEFAULT 5
		)`)
		exec(`CREATE TABLE IF NOT EXISTS spawn_point_mobs (
			id               INTEGER PRIMARY KEY AUTOINCREMENT,
			spawn_point_id   INTEGER NOT NULL DEFAULT 0,
			actor_def_id     INTEGER NOT NULL DEFAULT 0,
			mob_count        INTEGER NOT NULL DEFAULT 1,
			name             TEXT NOT NULL DEFAULT 'NPC',
			race             TEXT NOT NULL DEFAULT 'Human',
			class             TEXT NOT NULL DEFAULT 'Warrior',
			level            INTEGER NOT NULL DEFAULT 1,
			aggressiveness   INTEGER NOT NULL DEFAULT 2,
			aggressive_range REAL NOT NULL DEFAULT 8.0,
			attack_range     REAL NOT NULL DEFAULT 2.0,
			respawn_delay_ms INTEGER NOT NULL DEFAULT 30000
		)`)
	}
}

// LoadSpawnPoints returns all spawn_points rows ordered by id.
func (d *DB) LoadSpawnPoints(ctx context.Context) ([]*SpawnPoint, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, name, area_name, x, y, z, radius FROM spawn_points ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("db: LoadSpawnPoints: %w", err)
	}
	defer rows.Close()
	var out []*SpawnPoint
	for rows.Next() {
		sp := &SpawnPoint{}
		if err := rows.Scan(&sp.ID, &sp.Name, &sp.AreaName, &sp.X, &sp.Y, &sp.Z, &sp.Radius); err != nil {
			return nil, fmt.Errorf("db: LoadSpawnPoints scan: %w", err)
		}
		out = append(out, sp)
	}
	return out, rows.Err()
}

// migrateV14 creates area_waypoints and adds start_waypoint_id to npc_spawns.
func (d *DB) migrateV14(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS area_waypoints (
			id        SERIAL PRIMARY KEY,
			area_name VARCHAR(128) NOT NULL DEFAULT '',
			x         REAL NOT NULL DEFAULT 0,
			y         REAL NOT NULL DEFAULT 0,
			z         REAL NOT NULL DEFAULT 0,
			next_a    INTEGER NOT NULL DEFAULT 0,
			next_b    INTEGER NOT NULL DEFAULT 0,
			pause_ms  INTEGER NOT NULL DEFAULT 0
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_waypoints_area ON area_waypoints(area_name)`)
		exec(`ALTER TABLE npc_spawns ADD COLUMN IF NOT EXISTS start_waypoint_id INTEGER NOT NULL DEFAULT 0`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS area_waypoints (
			id        INTEGER PRIMARY KEY AUTOINCREMENT,
			area_name TEXT NOT NULL DEFAULT '',
			x         REAL NOT NULL DEFAULT 0,
			y         REAL NOT NULL DEFAULT 0,
			z         REAL NOT NULL DEFAULT 0,
			next_a    INTEGER NOT NULL DEFAULT 0,
			next_b    INTEGER NOT NULL DEFAULT 0,
			pause_ms  INTEGER NOT NULL DEFAULT 0
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_waypoints_area ON area_waypoints(area_name)`)
		exec(`ALTER TABLE npc_spawns ADD COLUMN start_waypoint_id INTEGER NOT NULL DEFAULT 0`)
	}
}

// LoadWaypoints returns all rows from area_waypoints keyed by waypoint ID.
func (d *DB) LoadWaypoints(ctx context.Context) (map[int]*Waypoint, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, area_name, x, y, z, next_a, next_b, pause_ms FROM area_waypoints ORDER BY id`)
	if err != nil {
		return nil, fmt.Errorf("db: LoadWaypoints: %w", err)
	}
	defer rows.Close()
	out := make(map[int]*Waypoint)
	for rows.Next() {
		w := &Waypoint{}
		if err := rows.Scan(&w.ID, &w.AreaName, &w.X, &w.Y, &w.Z,
			&w.NextA, &w.NextB, &w.PauseMs); err != nil {
			return nil, fmt.Errorf("db: LoadWaypoints scan: %w", err)
		}
		out[w.ID] = w
	}
	return out, rows.Err()
}

// migrateV16 creates media_model_shapes for per-model collision primitives.
func (d *DB) migrateV16(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS media_model_shapes (
			id       SERIAL PRIMARY KEY,
			model_id INTEGER NOT NULL DEFAULT 0,
			type     SMALLINT NOT NULL DEFAULT 0,
			offset_x REAL NOT NULL DEFAULT 0,
			offset_y REAL NOT NULL DEFAULT 0,
			offset_z REAL NOT NULL DEFAULT 0,
			size_x   REAL NOT NULL DEFAULT 1,
			size_y   REAL NOT NULL DEFAULT 1,
			size_z   REAL NOT NULL DEFAULT 1
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_model_shapes ON media_model_shapes(model_id)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS media_model_shapes (
			id       INTEGER PRIMARY KEY AUTOINCREMENT,
			model_id INTEGER NOT NULL DEFAULT 0,
			type     INTEGER NOT NULL DEFAULT 0,
			offset_x REAL NOT NULL DEFAULT 0,
			offset_y REAL NOT NULL DEFAULT 0,
			offset_z REAL NOT NULL DEFAULT 0,
			size_x   REAL NOT NULL DEFAULT 1,
			size_y   REAL NOT NULL DEFAULT 1,
			size_z   REAL NOT NULL DEFAULT 1
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_model_shapes ON media_model_shapes(model_id)`)
	}
}

// MediaModelShape mirrors one row in media_model_shapes.
// Type: 0 = box (size_x/y/z = full W/H/D), 1 = sphere (size_x = radius).
type MediaModelShape struct {
	ID                        int
	ModelID                   int
	Type                      int
	OffsetX, OffsetY, OffsetZ float32
	SizeX, SizeY, SizeZ       float32
}

// LoadModelShapes returns all collision shapes for the given model ID.
func (d *DB) LoadModelShapes(ctx context.Context, modelID int) ([]MediaModelShape, error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT id, model_id, type, offset_x, offset_y, offset_z, size_x, size_y, size_z
		     FROM media_model_shapes WHERE model_id = ? ORDER BY id`), modelID)
	if err != nil {
		return nil, fmt.Errorf("db: LoadModelShapes: %w", err)
	}
	defer rows.Close()
	var out []MediaModelShape
	for rows.Next() {
		var s MediaModelShape
		if err := rows.Scan(&s.ID, &s.ModelID, &s.Type,
			&s.OffsetX, &s.OffsetY, &s.OffsetZ,
			&s.SizeX, &s.SizeY, &s.SizeZ); err != nil {
			return nil, fmt.Errorf("db: LoadModelShapes scan: %w", err)
		}
		out = append(out, s)
	}
	return out, rows.Err()
}

// migrateV17 creates the world_objects table for placed static model instances.
func (d *DB) migrateV17(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS world_objects (
			id        SERIAL PRIMARY KEY,
			area_name VARCHAR(128) NOT NULL DEFAULT '',
			model_id  INTEGER NOT NULL DEFAULT 0,
			x         REAL NOT NULL DEFAULT 0,
			y         REAL NOT NULL DEFAULT 0,
			z         REAL NOT NULL DEFAULT 0,
			yaw       REAL NOT NULL DEFAULT 0,
			scale     REAL NOT NULL DEFAULT 1
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_world_objects_area ON world_objects(area_name)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS world_objects (
			id        INTEGER PRIMARY KEY AUTOINCREMENT,
			area_name TEXT NOT NULL DEFAULT '',
			model_id  INTEGER NOT NULL DEFAULT 0,
			x         REAL NOT NULL DEFAULT 0,
			y         REAL NOT NULL DEFAULT 0,
			z         REAL NOT NULL DEFAULT 0,
			yaw       REAL NOT NULL DEFAULT 0,
			scale     REAL NOT NULL DEFAULT 1
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_world_objects_area ON world_objects(area_name)`)
	}
}

// migrateV18 creates quest definition/progress tables and seeds one baseline
// quest for the Training Camp loop.
func (d *DB) migrateV18(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }

	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS quest_defs (
			id                   SERIAL PRIMARY KEY,
			code                 VARCHAR(64) NOT NULL UNIQUE,
			title                VARCHAR(128) NOT NULL DEFAULT '',
			description          TEXT NOT NULL DEFAULT '',
			min_level            INTEGER NOT NULL DEFAULT 1,
			repeatable           BOOLEAN NOT NULL DEFAULT FALSE,
			auto_accept          BOOLEAN NOT NULL DEFAULT FALSE,
			prerequisite_quest_id INTEGER NOT NULL DEFAULT 0,
			is_active            BOOLEAN NOT NULL DEFAULT TRUE
		)`)
		exec(`CREATE TABLE IF NOT EXISTS quest_objective_defs (
			id               SERIAL PRIMARY KEY,
			quest_id         INTEGER NOT NULL DEFAULT 0,
			objective_order  INTEGER NOT NULL DEFAULT 0,
			objective_type   SMALLINT NOT NULL DEFAULT 1,
			description      TEXT NOT NULL DEFAULT '',
			target_npc_name  VARCHAR(128) NOT NULL DEFAULT '',
			target_item_id   INTEGER NOT NULL DEFAULT 0,
			target_area_name VARCHAR(128) NOT NULL DEFAULT '',
			target_count     INTEGER NOT NULL DEFAULT 1
		)`)
		exec(`CREATE TABLE IF NOT EXISTS quest_reward_defs (
			id          SERIAL PRIMARY KEY,
			quest_id    INTEGER NOT NULL DEFAULT 0,
			xp_reward   INTEGER NOT NULL DEFAULT 0,
			gold_reward INTEGER NOT NULL DEFAULT 0,
			item_id     INTEGER NOT NULL DEFAULT 0,
			item_qty    INTEGER NOT NULL DEFAULT 0
		)`)
		exec(`CREATE TABLE IF NOT EXISTS character_quests (
			id           SERIAL PRIMARY KEY,
			character_id TEXT NOT NULL,
			quest_id     INTEGER NOT NULL DEFAULT 0,
			state        VARCHAR(32) NOT NULL DEFAULT 'active',
			accepted_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
			completed_at TIMESTAMPTZ,
			turned_in_at TIMESTAMPTZ,
			updated_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
			UNIQUE(character_id, quest_id)
		)`)
		exec(`CREATE TABLE IF NOT EXISTS character_quest_progress (
			character_id TEXT NOT NULL,
			quest_id     INTEGER NOT NULL DEFAULT 0,
			objective_id INTEGER NOT NULL DEFAULT 0,
			current_count INTEGER NOT NULL DEFAULT 0,
			target_count  INTEGER NOT NULL DEFAULT 1,
			updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
			PRIMARY KEY(character_id, quest_id, objective_id)
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_character_quests_character ON character_quests(character_id)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_character_quests_state ON character_quests(state)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_character_quest_progress_char ON character_quest_progress(character_id)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS quest_defs (
			id                    INTEGER PRIMARY KEY AUTOINCREMENT,
			code                  TEXT NOT NULL UNIQUE,
			title                 TEXT NOT NULL DEFAULT '',
			description           TEXT NOT NULL DEFAULT '',
			min_level             INTEGER NOT NULL DEFAULT 1,
			repeatable            INTEGER NOT NULL DEFAULT 0,
			auto_accept           INTEGER NOT NULL DEFAULT 0,
			prerequisite_quest_id INTEGER NOT NULL DEFAULT 0,
			is_active             INTEGER NOT NULL DEFAULT 1
		)`)
		exec(`CREATE TABLE IF NOT EXISTS quest_objective_defs (
			id               INTEGER PRIMARY KEY AUTOINCREMENT,
			quest_id         INTEGER NOT NULL DEFAULT 0,
			objective_order  INTEGER NOT NULL DEFAULT 0,
			objective_type   INTEGER NOT NULL DEFAULT 1,
			description      TEXT NOT NULL DEFAULT '',
			target_npc_name  TEXT NOT NULL DEFAULT '',
			target_item_id   INTEGER NOT NULL DEFAULT 0,
			target_area_name TEXT NOT NULL DEFAULT '',
			target_count     INTEGER NOT NULL DEFAULT 1
		)`)
		exec(`CREATE TABLE IF NOT EXISTS quest_reward_defs (
			id          INTEGER PRIMARY KEY AUTOINCREMENT,
			quest_id    INTEGER NOT NULL DEFAULT 0,
			xp_reward   INTEGER NOT NULL DEFAULT 0,
			gold_reward INTEGER NOT NULL DEFAULT 0,
			item_id     INTEGER NOT NULL DEFAULT 0,
			item_qty    INTEGER NOT NULL DEFAULT 0
		)`)
		exec(`CREATE TABLE IF NOT EXISTS character_quests (
			id           INTEGER PRIMARY KEY AUTOINCREMENT,
			character_id TEXT NOT NULL,
			quest_id     INTEGER NOT NULL DEFAULT 0,
			state        TEXT NOT NULL DEFAULT 'active',
			accepted_at  TEXT NOT NULL DEFAULT '',
			completed_at TEXT NOT NULL DEFAULT '',
			turned_in_at TEXT NOT NULL DEFAULT '',
			updated_at   TEXT NOT NULL DEFAULT '',
			UNIQUE(character_id, quest_id)
		)`)
		exec(`CREATE TABLE IF NOT EXISTS character_quest_progress (
			character_id  TEXT NOT NULL,
			quest_id      INTEGER NOT NULL DEFAULT 0,
			objective_id  INTEGER NOT NULL DEFAULT 0,
			current_count INTEGER NOT NULL DEFAULT 0,
			target_count  INTEGER NOT NULL DEFAULT 1,
			updated_at    TEXT NOT NULL DEFAULT '',
			PRIMARY KEY(character_id, quest_id, objective_id)
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_character_quests_character ON character_quests(character_id)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_character_quests_state ON character_quests(state)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_character_quest_progress_char ON character_quest_progress(character_id)`)
	}

	// Seed a baseline quest for local tests and quick GUE parity checks.
	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO quest_defs (code, title, description, min_level, repeatable, auto_accept, prerequisite_quest_id, is_active)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(code) DO NOTHING`),
		"training_camp_cleanup",
		"Training Camp Cleanup",
		"Defeat 5 Goblins around the training camp.",
		1, 0, 0, 0, 1,
	)
	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO quest_objective_defs
			(quest_id, objective_order, objective_type, description, target_npc_name, target_item_id, target_area_name, target_count)
		SELECT q.id, 1, ?, ?, ?, 0, '', 5
		  FROM quest_defs q
		 WHERE q.code = ?
		   AND NOT EXISTS (
		       SELECT 1 FROM quest_objective_defs o
		        WHERE o.quest_id = q.id AND o.objective_order = 1
		   )`),
		QuestObjectiveKill,
		"Defeat Goblins (0/5)",
		"Goblin",
		"training_camp_cleanup",
	)
	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO quest_reward_defs
			(quest_id, xp_reward, gold_reward, item_id, item_qty)
		SELECT q.id, 150, 35, 0, 0
		  FROM quest_defs q
		 WHERE q.code = ?
		   AND NOT EXISTS (SELECT 1 FROM quest_reward_defs r WHERE r.quest_id = q.id)`),
		"training_camp_cleanup",
	)
}

// migrateV19 creates data-driven combat ability runtime tables.
func (d *DB) migrateV19(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }

	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS ability_templates (
			id                      SERIAL PRIMARY KEY,
			name                    VARCHAR(96) NOT NULL UNIQUE,
			family                  VARCHAR(32) NOT NULL DEFAULT 'melee_special',
			resource_type           VARCHAR(16) NOT NULL DEFAULT 'none',
			resource_cost           INTEGER NOT NULL DEFAULT 0,
			cooldown_ms             INTEGER NOT NULL DEFAULT 2000,
			range_min               REAL NOT NULL DEFAULT 0,
			range_max               REAL NOT NULL DEFAULT 2.5,
			windup_ms               INTEGER NOT NULL DEFAULT 700,
			impact_delay_ms         INTEGER NOT NULL DEFAULT 0,
			recover_ms              INTEGER NOT NULL DEFAULT 400,
			parry_window_ms         INTEGER NOT NULL DEFAULT 200,
			interruptible           BOOLEAN NOT NULL DEFAULT TRUE,
			base_damage_min         INTEGER NOT NULL DEFAULT 0,
			base_damage_max         INTEGER NOT NULL DEFAULT 0,
			damage_stat_scale_json  TEXT NOT NULL DEFAULT '',
			armor_pierce_pct        REAL NOT NULL DEFAULT 0,
			crit_policy_json        TEXT NOT NULL DEFAULT '',
			telegraph_type          VARCHAR(32) NOT NULL DEFAULT 'ring_close',
			telegraph_radius        REAL NOT NULL DEFAULT 2.5,
			telegraph_color_rgba    VARCHAR(32) NOT NULL DEFAULT '1,0.2,0.2,0.75',
			action_windup           VARCHAR(64) NOT NULL DEFAULT 'Attack',
			action_impact           VARCHAR(64) NOT NULL DEFAULT 'Attack',
			action_recover          VARCHAR(64) NOT NULL DEFAULT 'Idle',
			allow_action_override   BOOLEAN NOT NULL DEFAULT FALSE,
			allowed_action_tags_json TEXT NOT NULL DEFAULT '',
			vfx_id_windup           INTEGER NOT NULL DEFAULT 0,
			vfx_id_impact           INTEGER NOT NULL DEFAULT 0,
			sfx_id_windup           INTEGER NOT NULL DEFAULT 0,
			sfx_id_impact           INTEGER NOT NULL DEFAULT 0,
			vfx_path_windup         TEXT NOT NULL DEFAULT '',
			vfx_path_impact         TEXT NOT NULL DEFAULT '',
			sfx_path_windup         TEXT NOT NULL DEFAULT '',
			sfx_path_impact         TEXT NOT NULL DEFAULT '',
			enabled                 BOOLEAN NOT NULL DEFAULT TRUE
		)`)
		exec(`CREATE TABLE IF NOT EXISTS npc_ability_loadouts (
			id                 SERIAL PRIMARY KEY,
			npc_spawn_id       INTEGER NOT NULL DEFAULT 0,
			actor_def_id       INTEGER NOT NULL DEFAULT 0,
			ability_id         INTEGER NOT NULL DEFAULT 0,
			priority           INTEGER NOT NULL DEFAULT 100,
			weight             INTEGER NOT NULL DEFAULT 100,
			min_distance       REAL NOT NULL DEFAULT 0,
			max_distance       REAL NOT NULL DEFAULT 0,
			min_target_hp_pct  REAL NOT NULL DEFAULT 0,
			max_target_hp_pct  REAL NOT NULL DEFAULT 100,
			phase_tag          VARCHAR(32) NOT NULL DEFAULT '',
			condition_lua      TEXT NOT NULL DEFAULT '',
			enabled            BOOLEAN NOT NULL DEFAULT TRUE
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_spawn ON npc_ability_loadouts(npc_spawn_id)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_actor ON npc_ability_loadouts(actor_def_id)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_ability ON npc_ability_loadouts(ability_id)`)
		exec(`CREATE TABLE IF NOT EXISTS npc_combat_profiles (
			id                        SERIAL PRIMARY KEY,
			name                      VARCHAR(64) NOT NULL UNIQUE,
			global_gcd_ms             INTEGER NOT NULL DEFAULT 450,
			decision_tick_ms          INTEGER NOT NULL DEFAULT 250,
			aggro_style               VARCHAR(32) NOT NULL DEFAULT 'default',
			allow_chain_cast          BOOLEAN NOT NULL DEFAULT FALSE,
			max_consecutive_specials  INTEGER NOT NULL DEFAULT 1,
			enabled                   BOOLEAN NOT NULL DEFAULT TRUE
		)`)
		exec(`CREATE TABLE IF NOT EXISTS npc_profile_bindings (
			id           SERIAL PRIMARY KEY,
			npc_spawn_id INTEGER NOT NULL DEFAULT 0,
			actor_def_id INTEGER NOT NULL DEFAULT 0,
			profile_id   INTEGER NOT NULL DEFAULT 0,
			enabled      BOOLEAN NOT NULL DEFAULT TRUE
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_spawn ON npc_profile_bindings(npc_spawn_id)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_actor ON npc_profile_bindings(actor_def_id)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_profile ON npc_profile_bindings(profile_id)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS ability_templates (
			id                       INTEGER PRIMARY KEY AUTOINCREMENT,
			name                     TEXT NOT NULL UNIQUE,
			family                   TEXT NOT NULL DEFAULT 'melee_special',
			resource_type            TEXT NOT NULL DEFAULT 'none',
			resource_cost            INTEGER NOT NULL DEFAULT 0,
			cooldown_ms              INTEGER NOT NULL DEFAULT 2000,
			range_min                REAL NOT NULL DEFAULT 0,
			range_max                REAL NOT NULL DEFAULT 2.5,
			windup_ms                INTEGER NOT NULL DEFAULT 700,
			impact_delay_ms          INTEGER NOT NULL DEFAULT 0,
			recover_ms               INTEGER NOT NULL DEFAULT 400,
			parry_window_ms          INTEGER NOT NULL DEFAULT 200,
			interruptible            INTEGER NOT NULL DEFAULT 1,
			base_damage_min          INTEGER NOT NULL DEFAULT 0,
			base_damage_max          INTEGER NOT NULL DEFAULT 0,
			damage_stat_scale_json   TEXT NOT NULL DEFAULT '',
			armor_pierce_pct         REAL NOT NULL DEFAULT 0,
			crit_policy_json         TEXT NOT NULL DEFAULT '',
			telegraph_type           TEXT NOT NULL DEFAULT 'ring_close',
			telegraph_radius         REAL NOT NULL DEFAULT 2.5,
			telegraph_color_rgba     TEXT NOT NULL DEFAULT '1,0.2,0.2,0.75',
			action_windup            TEXT NOT NULL DEFAULT 'Attack',
			action_impact            TEXT NOT NULL DEFAULT 'Attack',
			action_recover           TEXT NOT NULL DEFAULT 'Idle',
			allow_action_override    INTEGER NOT NULL DEFAULT 0,
			allowed_action_tags_json TEXT NOT NULL DEFAULT '',
			vfx_id_windup            INTEGER NOT NULL DEFAULT 0,
			vfx_id_impact            INTEGER NOT NULL DEFAULT 0,
			sfx_id_windup            INTEGER NOT NULL DEFAULT 0,
			sfx_id_impact            INTEGER NOT NULL DEFAULT 0,
			vfx_path_windup          TEXT NOT NULL DEFAULT '',
			vfx_path_impact          TEXT NOT NULL DEFAULT '',
			sfx_path_windup          TEXT NOT NULL DEFAULT '',
			sfx_path_impact          TEXT NOT NULL DEFAULT '',
			enabled                  INTEGER NOT NULL DEFAULT 1
		)`)
		exec(`CREATE TABLE IF NOT EXISTS npc_ability_loadouts (
			id                INTEGER PRIMARY KEY AUTOINCREMENT,
			npc_spawn_id      INTEGER NOT NULL DEFAULT 0,
			actor_def_id      INTEGER NOT NULL DEFAULT 0,
			ability_id        INTEGER NOT NULL DEFAULT 0,
			priority          INTEGER NOT NULL DEFAULT 100,
			weight            INTEGER NOT NULL DEFAULT 100,
			min_distance      REAL NOT NULL DEFAULT 0,
			max_distance      REAL NOT NULL DEFAULT 0,
			min_target_hp_pct REAL NOT NULL DEFAULT 0,
			max_target_hp_pct REAL NOT NULL DEFAULT 100,
			phase_tag         TEXT NOT NULL DEFAULT '',
			condition_lua     TEXT NOT NULL DEFAULT '',
			enabled           INTEGER NOT NULL DEFAULT 1
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_spawn ON npc_ability_loadouts(npc_spawn_id)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_actor ON npc_ability_loadouts(actor_def_id)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_ability_loadouts_ability ON npc_ability_loadouts(ability_id)`)
		exec(`CREATE TABLE IF NOT EXISTS npc_combat_profiles (
			id                       INTEGER PRIMARY KEY AUTOINCREMENT,
			name                     TEXT NOT NULL UNIQUE,
			global_gcd_ms            INTEGER NOT NULL DEFAULT 450,
			decision_tick_ms         INTEGER NOT NULL DEFAULT 250,
			aggro_style              TEXT NOT NULL DEFAULT 'default',
			allow_chain_cast         INTEGER NOT NULL DEFAULT 0,
			max_consecutive_specials INTEGER NOT NULL DEFAULT 1,
			enabled                  INTEGER NOT NULL DEFAULT 1
		)`)
		exec(`CREATE TABLE IF NOT EXISTS npc_profile_bindings (
			id           INTEGER PRIMARY KEY AUTOINCREMENT,
			npc_spawn_id INTEGER NOT NULL DEFAULT 0,
			actor_def_id INTEGER NOT NULL DEFAULT 0,
			profile_id   INTEGER NOT NULL DEFAULT 0,
			enabled      INTEGER NOT NULL DEFAULT 1
		)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_spawn ON npc_profile_bindings(npc_spawn_id)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_actor ON npc_profile_bindings(actor_def_id)`)
		exec(`CREATE INDEX IF NOT EXISTS idx_npc_profile_bindings_profile ON npc_profile_bindings(profile_id)`)
	}

	// Seed baseline definitions so runtime has a canonical starting point.
	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO ability_templates (
			name, family, resource_type, resource_cost, cooldown_ms,
			range_min, range_max, windup_ms, impact_delay_ms, recover_ms,
			parry_window_ms, interruptible, base_damage_min, base_damage_max,
			damage_stat_scale_json, armor_pierce_pct, crit_policy_json,
			telegraph_type, telegraph_radius, telegraph_color_rgba,
			action_windup, action_impact, action_recover,
			allow_action_override, allowed_action_tags_json,
			vfx_id_windup, vfx_id_impact, sfx_id_windup, sfx_id_impact,
			vfx_path_windup, vfx_path_impact, sfx_path_windup, sfx_path_impact,
			enabled
		)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(name) DO NOTHING`),
		"legacy_special_v1", "melee_special", "none", 0, 6500,
		0.0, 2.5, 1300, 0, 350,
		220, true, 0, 0,
		"", 50.0, "",
		"ring_close", 2.5, "1,0.2,0.2,0.75",
		"Attack", "Attack", "Idle",
		false, "",
		0, 0, 0, 0,
		"", "", "", "",
		true,
	)
	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO npc_combat_profiles (
			name, global_gcd_ms, decision_tick_ms, aggro_style, allow_chain_cast, max_consecutive_specials, enabled
		)
		VALUES (?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(name) DO NOTHING`),
		"default_profile", 450, 250, "default", false, 1, true,
	)
}

// migrateV20 adds optional mapping from spell_templates -> ability_templates.
// runtime_ability_id = 0 keeps legacy script spell execution.
func (d *DB) migrateV20(ctx context.Context) {
	if d.driver == "postgres" {
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE spell_templates ADD COLUMN IF NOT EXISTS runtime_ability_id INTEGER NOT NULL DEFAULT 0`)
	} else {
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE spell_templates ADD COLUMN runtime_ability_id INTEGER NOT NULL DEFAULT 0`)
	}
}

// migrateV21 seeds one scripted phased-boss encounter baseline for Forest Trolls
// (abilities + profile + loadouts + profile bindings).
func (d *DB) migrateV21(ctx context.Context) {
	exec := func(sql string, args ...any) {
		_, _ = d.db.ExecContext(ctx, d.q(sql), args...)
	}

	insertAbility := func(
		name string,
		cooldownMs int64,
		rangeMax float64,
		windupMs int64,
		recoverMs int64,
		parryWindowMs int64,
		baseMin int32,
		baseMax int32,
		armorPiercePct float64,
		telegraphRadius float64,
		telegraphColor string,
		allowedTags string,
	) {
		exec(`
			INSERT INTO ability_templates (
				name, family, resource_type, resource_cost, cooldown_ms,
				range_min, range_max, windup_ms, impact_delay_ms, recover_ms,
				parry_window_ms, interruptible, base_damage_min, base_damage_max,
				damage_stat_scale_json, armor_pierce_pct, crit_policy_json,
				telegraph_type, telegraph_radius, telegraph_color_rgba,
				action_windup, action_impact, action_recover,
				allow_action_override, allowed_action_tags_json,
				vfx_id_windup, vfx_id_impact, sfx_id_windup, sfx_id_impact,
				vfx_path_windup, vfx_path_impact, sfx_path_windup, sfx_path_impact,
				enabled
			)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
			ON CONFLICT(name) DO NOTHING
		`,
			name, "melee_special", "none", 0, cooldownMs,
			0.0, rangeMax, windupMs, 0, recoverMs,
			parryWindowMs, true, baseMin, baseMax,
			"", armorPiercePct, "",
			"ring_close", telegraphRadius, telegraphColor,
			"Attack", "Attack", "Idle",
			true, allowedTags,
			0, 0, 0, 0,
			"", "", "", "",
			true,
		)
	}

	insertAbility("forest_troll_crushing_blow_v1", 6000, 2.8, 1100, 450, 240, 25, 40, 20, 2.8, "1,0.2,0.2,0.75", `["heavy","slam"]`)
	insertAbility("forest_troll_brutal_slam_v1", 8500, 3.0, 1400, 500, 260, 38, 58, 35, 3.2, "1,0.45,0.15,0.75", `["heavy","slam"]`)
	insertAbility("forest_troll_enrage_cleave_v1", 5000, 3.2, 850, 350, 180, 50, 75, 45, 3.4, "1,0.1,0.1,0.85", `["heavy","enrage"]`)

	exec(`
		INSERT INTO npc_combat_profiles (
			name, global_gcd_ms, decision_tick_ms, aggro_style, allow_chain_cast, max_consecutive_specials, enabled
		)
		VALUES (?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(name) DO NOTHING
	`, "forest_troll_boss_profile_v1", 550, 180, "default", true, 2, true)

	// Bind the boss profile to all Forest Troll spawn rows (idempotent).
	exec(`
		INSERT INTO npc_profile_bindings (npc_spawn_id, actor_def_id, profile_id, enabled)
		SELECT ns.id, 0, p.id, 1
		  FROM npc_spawns ns
		  JOIN npc_combat_profiles p ON p.name = ?
		 WHERE ns.name = ?
		   AND NOT EXISTS (
			   SELECT 1
			     FROM npc_profile_bindings b
			    WHERE b.npc_spawn_id = ns.id
			      AND b.profile_id = p.id
		   )
	`, "forest_troll_boss_profile_v1", "Forest Troll")

	// Also bind by actor_def_id for Hunger (spawn point mobs in Training Camp).
	// This makes the runtime profile work even when NPCs are spawned from
	// spawn_point_mobs (which do not map to npc_spawns.id at runtime).
	exec(`
		INSERT INTO npc_profile_bindings (npc_spawn_id, actor_def_id, profile_id, enabled)
		SELECT 0, ad.actor_def_id, p.id, 1
		  FROM (
			   SELECT DISTINCT actor_def_id
			     FROM spawn_point_mobs
			    WHERE name = ?
			      AND actor_def_id > 0
			   UNION
			   SELECT DISTINCT actor_def_id
			     FROM npc_spawns
			    WHERE name = ?
			      AND actor_def_id > 0
		  ) ad
		  JOIN npc_combat_profiles p ON p.name = ?
		 WHERE NOT EXISTS (
			 SELECT 1
			   FROM npc_profile_bindings b
			  WHERE b.actor_def_id = ad.actor_def_id
			    AND b.profile_id = p.id
		 )
	`, "Hunger", "Hunger", "forest_troll_boss_profile_v1")

	insertLoadout := func(
		abilityName string,
		priority int,
		weight int,
		minDistance float64,
		maxDistance float64,
		minTargetHPPct float64,
		maxTargetHPPct float64,
		phaseTag string,
		conditionLua string,
	) {
		exec(`
			INSERT INTO npc_ability_loadouts (
				npc_spawn_id, actor_def_id, ability_id, priority, weight,
				min_distance, max_distance, min_target_hp_pct, max_target_hp_pct,
				phase_tag, condition_lua, enabled
			)
			SELECT ns.id, 0, a.id, ?, ?, ?, ?, ?, ?, ?, ?, 1
			  FROM npc_spawns ns
			  JOIN ability_templates a ON a.name = ?
			 WHERE ns.name = ?
			   AND NOT EXISTS (
				   SELECT 1
				     FROM npc_ability_loadouts l
				    WHERE l.npc_spawn_id = ns.id
				      AND l.ability_id = a.id
				      AND l.phase_tag = ?
				      AND l.priority = ?
			   )
		`,
			priority, weight, minDistance, maxDistance, minTargetHPPct, maxTargetHPPct, phaseTag, conditionLua,
			abilityName, "Forest Troll", phaseTag, priority,
		)
	}

	insertLoadoutForActorDefByName := func(
		npcName string,
		abilityName string,
		priority int,
		weight int,
		minDistance float64,
		maxDistance float64,
		minTargetHPPct float64,
		maxTargetHPPct float64,
		phaseTag string,
		conditionLua string,
	) {
		exec(`
			INSERT INTO npc_ability_loadouts (
				npc_spawn_id, actor_def_id, ability_id, priority, weight,
				min_distance, max_distance, min_target_hp_pct, max_target_hp_pct,
				phase_tag, condition_lua, enabled
			)
			SELECT 0, ad.actor_def_id, a.id, ?, ?, ?, ?, ?, ?, ?, ?, 1
			  FROM (
				   SELECT DISTINCT actor_def_id
				     FROM spawn_point_mobs
				    WHERE name = ?
				      AND actor_def_id > 0
				   UNION
				   SELECT DISTINCT actor_def_id
				     FROM npc_spawns
				    WHERE name = ?
				      AND actor_def_id > 0
			  ) ad
			  JOIN ability_templates a ON a.name = ?
			 WHERE NOT EXISTS (
				 SELECT 1
				   FROM npc_ability_loadouts l
				  WHERE l.actor_def_id = ad.actor_def_id
				    AND l.ability_id = a.id
				    AND l.phase_tag = ?
				    AND l.priority = ?
			 )
		`,
			priority, weight, minDistance, maxDistance, minTargetHPPct, maxTargetHPPct, phaseTag, conditionLua,
			npcName, npcName, abilityName, phaseTag, priority,
		)
	}

	insertLoadout("forest_troll_crushing_blow_v1", 300, 100, 0.0, 2.8, 0, 100, "phase_1", "distance <= 2.8 and npc_hp_pct > 66")
	insertLoadout("forest_troll_brutal_slam_v1", 320, 100, 0.0, 3.0, 0, 100, "phase_2", "distance <= 3.0 and npc_hp_pct <= 66 and npc_hp_pct > 33")
	insertLoadout("forest_troll_enrage_cleave_v1", 340, 100, 0.0, 3.2, 0, 100, "phase_3,enrage", "distance <= 3.2 and npc_hp_pct <= 33")

	// Hunger encounter in Training Camp (spawn point mobs) via actor_def_id.
	insertLoadoutForActorDefByName("Hunger", "forest_troll_crushing_blow_v1", 300, 100, 0.0, 2.8, 0, 100, "phase_1", "distance <= 2.8 and npc_hp_pct > 66")
	insertLoadoutForActorDefByName("Hunger", "forest_troll_brutal_slam_v1", 320, 100, 0.0, 3.0, 0, 100, "phase_2", "distance <= 3.0 and npc_hp_pct <= 66 and npc_hp_pct > 33")
	insertLoadoutForActorDefByName("Hunger", "forest_troll_enrage_cleave_v1", 340, 100, 0.0, 3.2, 0, 100, "phase_3,enrage", "distance <= 3.2 and npc_hp_pct <= 33")
}

// migrateV22 creates weapon_kits table for weapon-based player skill kits.
func (d *DB) migrateV22(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }

	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS weapon_kits (
			id           SERIAL PRIMARY KEY,
			kit_key      VARCHAR(32) NOT NULL UNIQUE,
			display_name VARCHAR(64) NOT NULL DEFAULT '',
			description  TEXT NOT NULL DEFAULT '',
			enabled      BOOLEAN NOT NULL DEFAULT TRUE
		)`)

		exec(`DO $$
		BEGIN
			IF EXISTS (
				SELECT 1
				  FROM information_schema.columns
				 WHERE table_name = 'weapon_kits'
				   AND column_name = 'weapon_type'
			) AND NOT EXISTS (
				SELECT 1
				  FROM information_schema.columns
				 WHERE table_name = 'weapon_kits'
				   AND column_name = 'kit_key'
			) THEN
				ALTER TABLE weapon_kits RENAME COLUMN weapon_type TO kit_key;
			END IF;
		END $$;`)

		exec(`DO $$
		BEGIN
			IF EXISTS (
				SELECT 1
				  FROM pg_class
				 WHERE relkind = 'i'
				   AND relname = 'idx_weapon_kits_weapon_type'
			) AND NOT EXISTS (
				SELECT 1
				  FROM pg_class
				 WHERE relkind = 'i'
				   AND relname = 'idx_weapon_kits_kit_key'
			) THEN
				ALTER INDEX idx_weapon_kits_weapon_type RENAME TO idx_weapon_kits_kit_key;
			END IF;
		END $$;`)

		exec(`CREATE INDEX IF NOT EXISTS idx_weapon_kits_kit_key ON weapon_kits(kit_key)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS weapon_kits (
			id           INTEGER PRIMARY KEY AUTOINCREMENT,
			kit_key      TEXT NOT NULL UNIQUE,
			display_name TEXT NOT NULL DEFAULT '',
			description  TEXT NOT NULL DEFAULT '',
			enabled      INTEGER NOT NULL DEFAULT 1
		)`)

		// SQLite has no IF EXISTS for RENAME COLUMN; we attempt and ignore errors.
		exec(`ALTER TABLE weapon_kits RENAME COLUMN weapon_type TO kit_key`)
		// SQLite has no index rename statement; recreate under the new name.
		exec(`DROP INDEX IF EXISTS idx_weapon_kits_weapon_type`)
		exec(`CREATE INDEX IF NOT EXISTS idx_weapon_kits_kit_key ON weapon_kits(kit_key)`)
	}
}

// migrateV23 adds weapon_kit to item_templates for weapon-skill kit mapping.
func (d *DB) migrateV23(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	if d.driver == "postgres" {
		exec(`ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS weapon_kit VARCHAR(32) NOT NULL DEFAULT ''`)
	} else {
		exec(`ALTER TABLE item_templates ADD COLUMN weapon_kit TEXT NOT NULL DEFAULT ''`)
	}
}

// migrateV24 creates weapon_kit_abilities junction rows (kit -> ability per slot).
func (d *DB) migrateV24(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }

	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS weapon_kit_abilities (
			id         SERIAL PRIMARY KEY,
			kit_id     INTEGER NOT NULL DEFAULT 0,
			ability_id INTEGER NOT NULL DEFAULT 0,
			slot_index INTEGER NOT NULL DEFAULT 0,
			enabled    BOOLEAN NOT NULL DEFAULT TRUE
		)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS weapon_kit_abilities (
			id         INTEGER PRIMARY KEY AUTOINCREMENT,
			kit_id     INTEGER NOT NULL DEFAULT 0,
			ability_id INTEGER NOT NULL DEFAULT 0,
			slot_index INTEGER NOT NULL DEFAULT 0,
			enabled    INTEGER NOT NULL DEFAULT 1
		)`)
	}

	exec(`CREATE INDEX IF NOT EXISTS idx_wka_kit_id ON weapon_kit_abilities(kit_id)`)
	exec(`CREATE INDEX IF NOT EXISTS idx_wka_ability_id ON weapon_kit_abilities(ability_id)`)
	exec(`CREATE UNIQUE INDEX IF NOT EXISTS idx_wka_kit_slot ON weapon_kit_abilities(kit_id, slot_index)`)
}

// migrateV25 repairs legacy SQLite weapon_kits schema where weapon_type was not
// renamed to kit_key on existing databases.
func (d *DB) migrateV25(ctx context.Context) {
	if d.driver != "sqlite" {
		return
	}

	hasOldColumn := false
	hasNewColumn := false
	rows, err := d.db.QueryContext(ctx, `PRAGMA table_info(weapon_kits)`)
	if err != nil {
		log.Printf("migrateV25: PRAGMA table_info(weapon_kits) failed: %v", err)
		return
	}
	defer rows.Close()

	for rows.Next() {
		var cid int
		var name, ctype string
		var notnull, pk int
		var dflt sql.NullString
		if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
			log.Printf("migrateV25: scan table_info row failed: %v", err)
			continue
		}
		if name == "weapon_type" {
			hasOldColumn = true
		}
		if name == "kit_key" {
			hasNewColumn = true
		}
	}
	if err := rows.Err(); err != nil {
		log.Printf("migrateV25: table_info rows error: %v", err)
	}

	if !hasOldColumn || hasNewColumn {
		return
	}

	exec := func(sql string) error {
		_, err := d.db.ExecContext(ctx, sql)
		if err != nil {
			log.Printf("migrateV25: %s -> %v", sql, err)
		}
		return err
	}

	log.Printf("migrateV25: recreating weapon_kits table (had old 'weapon_type' column)")
	_ = exec(`DROP INDEX IF EXISTS idx_weapon_kits_weapon_type`)
	_ = exec(`DROP TABLE IF EXISTS weapon_kits`)
	_ = exec(`CREATE TABLE IF NOT EXISTS weapon_kits (
		id           INTEGER PRIMARY KEY AUTOINCREMENT,
		kit_key      TEXT NOT NULL UNIQUE,
		display_name TEXT NOT NULL DEFAULT '',
		description  TEXT NOT NULL DEFAULT '',
		enabled      INTEGER NOT NULL DEFAULT 1
	)`)
	_ = exec(`CREATE INDEX IF NOT EXISTS idx_weapon_kits_kit_key ON weapon_kits(kit_key)`)
}

// migrateV26 creates equipment_slot_config for data-driven slot->kit/loadout limits.
func (d *DB) migrateV26(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS equipment_slot_config (
			slot_id               INTEGER PRIMARY KEY,
			gives_kit             BOOLEAN NOT NULL DEFAULT FALSE,
			max_skills_in_loadout INTEGER NOT NULL DEFAULT 0,
			enabled               BOOLEAN NOT NULL DEFAULT TRUE
		)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS equipment_slot_config (
			slot_id               INTEGER PRIMARY KEY,
			gives_kit             INTEGER NOT NULL DEFAULT 0,
			max_skills_in_loadout INTEGER NOT NULL DEFAULT 0,
			enabled               INTEGER NOT NULL DEFAULT 1
		)`)
	}
}

// migrateV27 renames equipment_slot_config.max_skills_in_loadout to hotbar_slots_granted.
// Idempotent: applies only when old exists and new does not.
func (d *DB) migrateV27(ctx context.Context) {
	if d.driver == "postgres" {
		_, _ = d.db.ExecContext(ctx, `
DO $$
BEGIN
	IF EXISTS (
		SELECT 1
		  FROM information_schema.columns
		 WHERE table_name = 'equipment_slot_config'
		   AND column_name = 'max_skills_in_loadout'
	) AND NOT EXISTS (
		SELECT 1
		  FROM information_schema.columns
		 WHERE table_name = 'equipment_slot_config'
		   AND column_name = 'hotbar_slots_granted'
	) THEN
		ALTER TABLE equipment_slot_config
		RENAME COLUMN max_skills_in_loadout TO hotbar_slots_granted;
	END IF;
END $$;`)
		return
	}

	hasOld := false
	hasNew := false
	rows, err := d.db.QueryContext(ctx, `PRAGMA table_info(equipment_slot_config)`)
	if err != nil {
		return
	}
	defer rows.Close()

	for rows.Next() {
		var cid int
		var name, ctype string
		var notnull, pk int
		var dflt sql.NullString
		if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
			continue
		}
		if name == "max_skills_in_loadout" {
			hasOld = true
		}
		if name == "hotbar_slots_granted" {
			hasNew = true
		}
	}

	if hasOld && !hasNew {
		log.Printf("migrateV27: renaming max_skills_in_loadout -> hotbar_slots_granted")
		_, _ = d.db.ExecContext(ctx, `ALTER TABLE equipment_slot_config RENAME COLUMN max_skills_in_loadout TO hotbar_slots_granted`)
	}
}

// migrateV28 creates character_skill_loadouts for per-character per-kit loadouts.
func (d *DB) migrateV28(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS character_skill_loadouts (
			id           SERIAL PRIMARY KEY,
			character_id TEXT NOT NULL,
			kit_id       INTEGER NOT NULL DEFAULT 0,
			slot_index   INTEGER NOT NULL DEFAULT 0,
			ability_id   INTEGER NOT NULL DEFAULT 0
		)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS character_skill_loadouts (
			id           INTEGER PRIMARY KEY AUTOINCREMENT,
			character_id TEXT NOT NULL,
			kit_id       INTEGER NOT NULL DEFAULT 0,
			slot_index   INTEGER NOT NULL DEFAULT 0,
			ability_id   INTEGER NOT NULL DEFAULT 0
		)`)
	}
	exec(`CREATE INDEX IF NOT EXISTS idx_csl_char_kit ON character_skill_loadouts(character_id, kit_id)`)
	exec(`CREATE UNIQUE INDEX IF NOT EXISTS idx_csl_char_kit_slot ON character_skill_loadouts(character_id, kit_id, slot_index)`)
}

// migrateV29 creates mastery progression tables:
// - character_skill_progress (per-character, per-ability xp/level)
// - skill_progression_config (singleton tuning row)
func (d *DB) migrateV29(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS character_skill_progress (
			id           SERIAL PRIMARY KEY,
			character_id TEXT    NOT NULL,
			ability_id   INTEGER NOT NULL DEFAULT 0,
			xp           INTEGER NOT NULL DEFAULT 0,
			level        INTEGER NOT NULL DEFAULT 1
		)`)
		exec(`CREATE TABLE IF NOT EXISTS skill_progression_config (
			id                       SERIAL PRIMARY KEY,
			xp_per_use               INTEGER NOT NULL DEFAULT 10,
			max_level                INTEGER NOT NULL DEFAULT 10,
			xp_curve_type            TEXT    NOT NULL DEFAULT 'linear',
			xp_curve_base            INTEGER NOT NULL DEFAULT 100,
			damage_bonus_per_level   REAL    NOT NULL DEFAULT 0.03,
			cooldown_redux_per_level REAL    NOT NULL DEFAULT 0.01
		)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS character_skill_progress (
			id           INTEGER PRIMARY KEY AUTOINCREMENT,
			character_id TEXT    NOT NULL,
			ability_id   INTEGER NOT NULL DEFAULT 0,
			xp           INTEGER NOT NULL DEFAULT 0,
			level        INTEGER NOT NULL DEFAULT 1
		)`)
		exec(`CREATE TABLE IF NOT EXISTS skill_progression_config (
			id                       INTEGER PRIMARY KEY AUTOINCREMENT,
			xp_per_use               INTEGER NOT NULL DEFAULT 10,
			max_level                INTEGER NOT NULL DEFAULT 10,
			xp_curve_type            TEXT    NOT NULL DEFAULT 'linear',
			xp_curve_base            INTEGER NOT NULL DEFAULT 100,
			damage_bonus_per_level   REAL    NOT NULL DEFAULT 0.03,
			cooldown_redux_per_level REAL    NOT NULL DEFAULT 0.01
		)`)
	}
	exec(`CREATE INDEX IF NOT EXISTS idx_csp_char ON character_skill_progress(character_id)`)
	exec(`CREATE UNIQUE INDEX IF NOT EXISTS idx_csp_char_ability ON character_skill_progress(character_id, ability_id)`)
}

// migrateV30 adds per-skill mastery config columns to ability_templates.
// It backfills existing rows from the global skill_progression_config template
// only when mastery columns are first introduced, preserving later manual edits.
func (d *DB) migrateV30(ctx context.Context) {
	type progressionDefaults struct {
		xpPerUse      int
		maxLevel      int
		curveType     string
		curveBase     int
		damageBonus   float64
		cooldownRedux float64
	}

	defaults := progressionDefaults{
		xpPerUse:      10,
		maxLevel:      10,
		curveType:     "linear",
		curveBase:     100,
		damageBonus:   0.03,
		cooldownRedux: 0.01,
	}

	// Global config is now a template source for new per-skill mastery columns.
	var curveTypeRaw string
	err := d.db.QueryRowContext(ctx, d.q(`
		SELECT xp_per_use, max_level, xp_curve_type, xp_curve_base, damage_bonus_per_level, cooldown_redux_per_level
		  FROM skill_progression_config
		 ORDER BY id
		 LIMIT 1`),
	).Scan(
		&defaults.xpPerUse,
		&defaults.maxLevel,
		&curveTypeRaw,
		&defaults.curveBase,
		&defaults.damageBonus,
		&defaults.cooldownRedux,
	)
	if err != nil && err != sql.ErrNoRows {
		log.Printf("migrateV30: load skill_progression_config template failed: %v", err)
	}

	curveTypeRaw = strings.ToLower(strings.TrimSpace(curveTypeRaw))
	if curveTypeRaw == "linear" || curveTypeRaw == "exponential" {
		defaults.curveType = curveTypeRaw
	}
	if defaults.xpPerUse <= 0 {
		defaults.xpPerUse = 10
	}
	if defaults.maxLevel <= 0 {
		defaults.maxLevel = 10
	}
	if defaults.curveBase <= 0 {
		defaults.curveBase = 100
	}
	if defaults.damageBonus < 0 {
		defaults.damageBonus = 0
	}
	if defaults.cooldownRedux < 0 {
		defaults.cooldownRedux = 0
	}

	hasCol := map[string]bool{}
	if d.driver == "postgres" {
		rows, err := d.db.QueryContext(ctx, `
			SELECT column_name
			  FROM information_schema.columns
			 WHERE table_name = 'ability_templates'`)
		if err != nil {
			log.Printf("migrateV30: information_schema.columns failed: %v", err)
			return
		}
		for rows.Next() {
			var name string
			if err := rows.Scan(&name); err != nil {
				continue
			}
			hasCol[strings.ToLower(strings.TrimSpace(name))] = true
		}
		_ = rows.Close()
	} else {
		rows, err := d.db.QueryContext(ctx, `PRAGMA table_info(ability_templates)`)
		if err != nil {
			log.Printf("migrateV30: PRAGMA table_info(ability_templates) failed: %v", err)
			return
		}
		for rows.Next() {
			var cid int
			var name, ctype string
			var notnull, pk int
			var dflt sql.NullString
			if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
				continue
			}
			hasCol[strings.ToLower(strings.TrimSpace(name))] = true
		}
		_ = rows.Close()
	}

	addColumn := func(columnName, pgSQL, sqliteSQL string) {
		key := strings.ToLower(strings.TrimSpace(columnName))
		if hasCol[key] {
			return
		}
		sqlStmt := sqliteSQL
		if d.driver == "postgres" {
			sqlStmt = pgSQL
		}
		if _, err := d.db.ExecContext(ctx, sqlStmt); err != nil {
			log.Printf("migrateV30: add column %s failed: %v", columnName, err)
			return
		}
		hasCol[key] = true
	}

	addColumn(
		"category",
		`ALTER TABLE ability_templates ADD COLUMN category TEXT NOT NULL DEFAULT 'damage'`,
		`ALTER TABLE ability_templates ADD COLUMN category TEXT NOT NULL DEFAULT 'damage'`,
	)
	addColumn(
		"mastery_xp_per_use",
		`ALTER TABLE ability_templates ADD COLUMN mastery_xp_per_use INTEGER NOT NULL DEFAULT 10`,
		`ALTER TABLE ability_templates ADD COLUMN mastery_xp_per_use INTEGER NOT NULL DEFAULT 10`,
	)
	addColumn(
		"mastery_max_level",
		`ALTER TABLE ability_templates ADD COLUMN mastery_max_level INTEGER NOT NULL DEFAULT 10`,
		`ALTER TABLE ability_templates ADD COLUMN mastery_max_level INTEGER NOT NULL DEFAULT 10`,
	)
	addColumn(
		"mastery_xp_curve_type",
		`ALTER TABLE ability_templates ADD COLUMN mastery_xp_curve_type TEXT NOT NULL DEFAULT 'linear'`,
		`ALTER TABLE ability_templates ADD COLUMN mastery_xp_curve_type TEXT NOT NULL DEFAULT 'linear'`,
	)
	addColumn(
		"mastery_xp_curve_base",
		`ALTER TABLE ability_templates ADD COLUMN mastery_xp_curve_base INTEGER NOT NULL DEFAULT 100`,
		`ALTER TABLE ability_templates ADD COLUMN mastery_xp_curve_base INTEGER NOT NULL DEFAULT 100`,
	)
	addColumn(
		"mastery_primary_bonus_per_lvl",
		`ALTER TABLE ability_templates ADD COLUMN mastery_primary_bonus_per_lvl DOUBLE PRECISION NOT NULL DEFAULT 0.03`,
		`ALTER TABLE ability_templates ADD COLUMN mastery_primary_bonus_per_lvl REAL NOT NULL DEFAULT 0.03`,
	)
	addColumn(
		"mastery_cooldown_redux_per_lvl",
		`ALTER TABLE ability_templates ADD COLUMN mastery_cooldown_redux_per_lvl DOUBLE PRECISION NOT NULL DEFAULT 0.01`,
		`ALTER TABLE ability_templates ADD COLUMN mastery_cooldown_redux_per_lvl REAL NOT NULL DEFAULT 0.01`,
	)

	// Normalize/backfill existing rows (idempotent) in case legacy rows were left
	// with NULL/empty/zero values after column add behavior differences.
	if _, err := d.db.ExecContext(ctx, `
		UPDATE ability_templates
		   SET category = 'damage'
		 WHERE category IS NULL OR TRIM(category) = ''`); err != nil {
		log.Printf("migrateV30: normalize category failed: %v", err)
	}

	if _, err := d.db.ExecContext(ctx, d.q(`
		UPDATE ability_templates
		   SET mastery_xp_per_use = ?
		 WHERE mastery_xp_per_use IS NULL OR mastery_xp_per_use <= 0`), defaults.xpPerUse); err != nil {
		log.Printf("migrateV30: normalize mastery_xp_per_use failed: %v", err)
	}

	if _, err := d.db.ExecContext(ctx, d.q(`
		UPDATE ability_templates
		   SET mastery_max_level = ?
		 WHERE mastery_max_level IS NULL OR mastery_max_level <= 0`), defaults.maxLevel); err != nil {
		log.Printf("migrateV30: normalize mastery_max_level failed: %v", err)
	}

	if _, err := d.db.ExecContext(ctx, d.q(`
		UPDATE ability_templates
		   SET mastery_xp_curve_type = ?
		 WHERE mastery_xp_curve_type IS NULL OR TRIM(mastery_xp_curve_type) = ''`), defaults.curveType); err != nil {
		log.Printf("migrateV30: normalize mastery_xp_curve_type failed: %v", err)
	}

	if _, err := d.db.ExecContext(ctx, d.q(`
		UPDATE ability_templates
		   SET mastery_xp_curve_base = ?
		 WHERE mastery_xp_curve_base IS NULL OR mastery_xp_curve_base <= 0`), defaults.curveBase); err != nil {
		log.Printf("migrateV30: normalize mastery_xp_curve_base failed: %v", err)
	}

	if _, err := d.db.ExecContext(ctx, d.q(`
		UPDATE ability_templates
		   SET mastery_primary_bonus_per_lvl = ?
		 WHERE mastery_primary_bonus_per_lvl IS NULL OR mastery_primary_bonus_per_lvl = 0`), defaults.damageBonus); err != nil {
		log.Printf("migrateV30: normalize mastery_primary_bonus_per_lvl failed: %v", err)
	}

	if _, err := d.db.ExecContext(ctx, d.q(`
		UPDATE ability_templates
		   SET mastery_cooldown_redux_per_lvl = ?
		 WHERE mastery_cooldown_redux_per_lvl IS NULL OR mastery_cooldown_redux_per_lvl = 0`), defaults.cooldownRedux); err != nil {
		log.Printf("migrateV30: normalize mastery_cooldown_redux_per_lvl failed: %v", err)
	}

	// Defensive bootstrap: legacy attack abilities should remain damage category.
	if _, err := d.db.ExecContext(ctx,
		`UPDATE ability_templates
		    SET category = 'damage'
		  WHERE base_damage_min > 0`); err != nil {
		log.Printf("migrateV30: bootstrap category='damage' failed: %v", err)
	}
}

// migrateV31 adds an optional gameplay-facing description field to
// ability_templates so clients can show "what this skill does" in tooltips.
func (d *DB) migrateV31(ctx context.Context) {
	hasDescription := false
	if d.driver == "postgres" {
		rows, err := d.db.QueryContext(ctx, `
			SELECT column_name
			  FROM information_schema.columns
			 WHERE table_name = 'ability_templates'`)
		if err != nil {
			log.Printf("migrateV31: information_schema.columns failed: %v", err)
			return
		}
		for rows.Next() {
			var name string
			if err := rows.Scan(&name); err != nil {
				continue
			}
			if strings.EqualFold(strings.TrimSpace(name), "description") {
				hasDescription = true
				break
			}
		}
		_ = rows.Close()
	} else {
		rows, err := d.db.QueryContext(ctx, `PRAGMA table_info(ability_templates)`)
		if err != nil {
			log.Printf("migrateV31: PRAGMA table_info(ability_templates) failed: %v", err)
			return
		}
		for rows.Next() {
			var cid int
			var name, ctype string
			var notnull, pk int
			var dflt sql.NullString
			if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
				continue
			}
			if strings.EqualFold(strings.TrimSpace(name), "description") {
				hasDescription = true
				break
			}
		}
		_ = rows.Close()
	}

	if !hasDescription {
		stmt := `ALTER TABLE ability_templates ADD COLUMN description TEXT NOT NULL DEFAULT ''`
		if d.driver == "postgres" {
			stmt = `ALTER TABLE ability_templates ADD COLUMN IF NOT EXISTS description TEXT NOT NULL DEFAULT ''`
		}
		if _, err := d.db.ExecContext(ctx, stmt); err != nil {
			log.Printf("migrateV31: add description failed: %v", err)
		}
	}

	// Defensive normalization for legacy rows.
	if _, err := d.db.ExecContext(ctx, `
		UPDATE ability_templates
		   SET description = ''
		 WHERE description IS NULL`); err != nil {
		log.Printf("migrateV31: normalize description failed: %v", err)
	}
}

// migrateV32 creates character progression/stats/xp scaling config tables.
func (d *DB) migrateV32(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }

	if d.driver == "postgres" {
		exec(`CREATE TABLE IF NOT EXISTS character_progression_config (
			id              SERIAL PRIMARY KEY,
			max_level       INTEGER NOT NULL DEFAULT 60,
			xp_curve_type   TEXT    NOT NULL DEFAULT 'quadratic',
			xp_curve_base   INTEGER NOT NULL DEFAULT 100,
			xp_curve_factor REAL    NOT NULL DEFAULT 1.3
		)`)
		exec(`CREATE TABLE IF NOT EXISTS character_primary_stats_per_level (
			level        INTEGER PRIMARY KEY,
			strength     INTEGER NOT NULL,
			dexterity    INTEGER NOT NULL,
			intelligence INTEGER NOT NULL,
			wisdom       INTEGER NOT NULL,
			perception   INTEGER NOT NULL
		)`)
		exec(`CREATE TABLE IF NOT EXISTS kill_xp_scaling_config (
			id                     SERIAL PRIMARY KEY,
			base_xp_per_npc_level  INTEGER NOT NULL DEFAULT 25,
			level_diff_coefficient REAL    NOT NULL DEFAULT 0.1,
			multiplier_min         REAL    NOT NULL DEFAULT 0.1,
			multiplier_max         REAL    NOT NULL DEFAULT 1.5
		)`)
	} else {
		exec(`CREATE TABLE IF NOT EXISTS character_progression_config (
			id              INTEGER PRIMARY KEY AUTOINCREMENT,
			max_level       INTEGER NOT NULL DEFAULT 60,
			xp_curve_type   TEXT    NOT NULL DEFAULT 'quadratic',
			xp_curve_base   INTEGER NOT NULL DEFAULT 100,
			xp_curve_factor REAL    NOT NULL DEFAULT 1.3
		)`)
		exec(`CREATE TABLE IF NOT EXISTS character_primary_stats_per_level (
			level        INTEGER PRIMARY KEY,
			strength     INTEGER NOT NULL,
			dexterity    INTEGER NOT NULL,
			intelligence INTEGER NOT NULL,
			wisdom       INTEGER NOT NULL,
			perception   INTEGER NOT NULL
		)`)
		exec(`CREATE TABLE IF NOT EXISTS kill_xp_scaling_config (
			id                     INTEGER PRIMARY KEY AUTOINCREMENT,
			base_xp_per_npc_level  INTEGER NOT NULL DEFAULT 25,
			level_diff_coefficient REAL    NOT NULL DEFAULT 0.1,
			multiplier_min         REAL    NOT NULL DEFAULT 0.1,
			multiplier_max         REAL    NOT NULL DEFAULT 1.5
		)`)
	}
}

func legacyCharacterXPThreshold(level int) int64 {
	if level <= 1 {
		return 0
	}
	n := int64(level - 1)
	return n * n * 100
}

// migrateV33 converts character and mastery XP storage from cumulative totals
// to "XP since last level" and marks completion in meta.
func (d *DB) migrateV33(ctx context.Context) {
	const conversionDoneKey = "migration_v33_xp_since_level_done"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV33: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr != nil && doneErr != sql.ErrNoRows {
		log.Printf("migrateV33: read meta flag failed: %v", doneErr)
		return
	}
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		log.Printf("migrateV33: begin tx failed: %v", err)
		return
	}
	defer func() { _ = tx.Rollback() }()

	charUpdates := 0
	charRows, err := tx.QueryContext(ctx, d.q(`SELECT id, xp, level FROM characters`))
	if err != nil {
		log.Printf("migrateV33: query characters failed: %v", err)
		return
	}
	for charRows.Next() {
		var (
			charID string
			xp     int64
			level  int
		)
		if err := charRows.Scan(&charID, &xp, &level); err != nil {
			_ = charRows.Close()
			log.Printf("migrateV33: scan character row failed: %v", err)
			return
		}
		if level < 1 {
			level = 1
		}
		threshold := legacyCharacterXPThreshold(level)
		xpSinceLevel := xp - threshold
		if xpSinceLevel < 0 {
			xpSinceLevel = 0
		}
		if xpSinceLevel == xp {
			continue
		}
		if _, err := tx.ExecContext(ctx, d.q(`UPDATE characters SET xp = ? WHERE id = ?`), xpSinceLevel, charID); err != nil {
			_ = charRows.Close()
			log.Printf("migrateV33: update characters xp failed (char=%s): %v", charID, err)
			return
		}
		charUpdates++
	}
	if err := charRows.Err(); err != nil {
		_ = charRows.Close()
		log.Printf("migrateV33: iterate characters failed: %v", err)
		return
	}
	_ = charRows.Close()

	masteryUpdates := 0
	masteryRows, err := tx.QueryContext(ctx, d.q(`
		SELECT csp.id, csp.xp, csp.level,
		       at.mastery_xp_curve_type,
		       at.mastery_xp_curve_base
		  FROM character_skill_progress csp
		  LEFT JOIN ability_templates at ON at.id = csp.ability_id`))
	if err != nil {
		log.Printf("migrateV33: query character_skill_progress failed: %v", err)
		return
	}
	for masteryRows.Next() {
		var (
			rowID     int
			xp        int64
			level     int
			curveType sql.NullString
			curveBase sql.NullInt64
		)
		if err := masteryRows.Scan(&rowID, &xp, &level, &curveType, &curveBase); err != nil {
			_ = masteryRows.Close()
			log.Printf("migrateV33: scan mastery row failed: %v", err)
			return
		}
		if level < 1 {
			level = 1
		}
		ability := &world.AbilityTemplate{
			MasteryXPCurveType: "linear",
			MasteryXPCurveBase: 100,
		}
		if curveType.Valid {
			s := strings.ToLower(strings.TrimSpace(curveType.String))
			if s != "" {
				ability.MasteryXPCurveType = s
			}
		}
		if curveBase.Valid && curveBase.Int64 > 0 {
			ability.MasteryXPCurveBase = int(curveBase.Int64)
		}

		threshold := int64(XPRequiredForLevelFromAbility(level, ability))
		xpSinceLevel := xp - threshold
		if xpSinceLevel < 0 {
			xpSinceLevel = 0
		}
		if xpSinceLevel == xp {
			continue
		}
		if _, err := tx.ExecContext(ctx, d.q(`UPDATE character_skill_progress SET xp = ? WHERE id = ?`), xpSinceLevel, rowID); err != nil {
			_ = masteryRows.Close()
			log.Printf("migrateV33: update mastery xp failed (row=%d): %v", rowID, err)
			return
		}
		masteryUpdates++
	}
	if err := masteryRows.Err(); err != nil {
		_ = masteryRows.Close()
		log.Printf("migrateV33: iterate character_skill_progress failed: %v", err)
		return
	}
	_ = masteryRows.Close()

	if _, err := tx.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1"); err != nil {
		log.Printf("migrateV33: write meta flag failed: %v", err)
		return
	}

	if err := tx.Commit(); err != nil {
		log.Printf("migrateV33: commit failed: %v", err)
		return
	}

	log.Printf("migrateV33: converted XP storage to since-level (characters=%d mastery_rows=%d)", charUpdates, masteryUpdates)
}

// migrateV34 moves XP back to cumulative totals, adds irregular curve knobs,
// and switches default character/mastery curves to the Diablo-style model.
func (d *DB) migrateV34(ctx context.Context) {
	const conversionDoneKey = "migration_v34_xp_cumulative_irregular_done"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV34: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr != nil && doneErr != sql.ErrNoRows {
		log.Printf("migrateV34: read meta flag failed: %v", doneErr)
		return
	}
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	type charUpdate struct {
		id string
		xp int64
	}
	type masteryUpdate struct {
		id int
		xp int64
	}

	oldCharCurveType := "quadratic"
	oldCharBase := 100
	if err := d.db.QueryRowContext(ctx, d.q(`
		SELECT xp_curve_type, xp_curve_base
		  FROM character_progression_config
		 ORDER BY id
		 LIMIT 1`)).Scan(&oldCharCurveType, &oldCharBase); err != nil && err != sql.ErrNoRows {
		log.Printf("migrateV34: read old character progression config failed: %v", err)
		return
	}
	if oldCharBase <= 0 {
		oldCharBase = 100
	}

	var charUpdates []charUpdate
	charRows, err := d.db.QueryContext(ctx, d.q(`SELECT id, xp, level FROM characters`))
	if err != nil {
		log.Printf("migrateV34: query characters failed: %v", err)
		return
	}
	for charRows.Next() {
		var (
			charID string
			xp     int64
			level  int
		)
		if err := charRows.Scan(&charID, &xp, &level); err != nil {
			_ = charRows.Close()
			log.Printf("migrateV34: scan character failed: %v", err)
			return
		}
		if level < 1 {
			level = 1
		}
		oldThreshold := world.ComputeXPThreshold(level, oldCharCurveType, oldCharBase, 2.0, 0)
		newThreshold := world.ComputeXPThreshold(level, "irregular", 60, 2.5, 0.4)
		xpCumulative := xp + oldThreshold
		if xpCumulative < newThreshold {
			xpCumulative = newThreshold
		}
		charUpdates = append(charUpdates, charUpdate{id: charID, xp: xpCumulative})
	}
	if err := charRows.Err(); err != nil {
		_ = charRows.Close()
		log.Printf("migrateV34: iterate characters failed: %v", err)
		return
	}
	_ = charRows.Close()

	var masteryUpdates []masteryUpdate
	masteryRows, err := d.db.QueryContext(ctx, d.q(`
		SELECT csp.id, csp.xp, csp.level,
		       COALESCE(at.mastery_xp_curve_type, 'linear'),
		       COALESCE(at.mastery_xp_curve_base, 100)
		  FROM character_skill_progress csp
		  LEFT JOIN ability_templates at ON at.id = csp.ability_id`))
	if err != nil {
		log.Printf("migrateV34: query character_skill_progress failed: %v", err)
		return
	}
	for masteryRows.Next() {
		var (
			rowID     int
			xp        int64
			level     int
			curveType string
			curveBase int
		)
		if err := masteryRows.Scan(&rowID, &xp, &level, &curveType, &curveBase); err != nil {
			_ = masteryRows.Close()
			log.Printf("migrateV34: scan mastery failed: %v", err)
			return
		}
		if level < 1 {
			level = 1
		}
		if curveBase <= 0 {
			curveBase = 100
		}
		oldThreshold := world.ComputeXPThreshold(level, curveType, curveBase, 2.0, 0)
		newThreshold := world.ComputeXPThreshold(level, "irregular", 40, 2.0, 0.5)
		xpCumulative := xp + oldThreshold
		if xpCumulative < newThreshold {
			xpCumulative = newThreshold
		}
		masteryUpdates = append(masteryUpdates, masteryUpdate{id: rowID, xp: xpCumulative})
	}
	if err := masteryRows.Err(); err != nil {
		_ = masteryRows.Close()
		log.Printf("migrateV34: iterate character_skill_progress failed: %v", err)
		return
	}
	_ = masteryRows.Close()

	hasColumn := func(table, column string) bool {
		if d.driver == "postgres" {
			var exists bool
			err := d.db.QueryRowContext(ctx, `
				SELECT EXISTS (
					SELECT 1 FROM information_schema.columns
					WHERE table_name = $1 AND column_name = $2
				)`, table, column).Scan(&exists)
			return err == nil && exists
		}

		rows, err := d.db.QueryContext(ctx, fmt.Sprintf(`PRAGMA table_info(%s)`, table))
		if err != nil {
			return false
		}
		defer rows.Close()
		for rows.Next() {
			var cid int
			var name, ctype string
			var notnull, pk int
			var dflt sql.NullString
			if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
				continue
			}
			if strings.EqualFold(strings.TrimSpace(name), column) {
				return true
			}
		}
		return false
	}

	addColumn := func(table, column, pgSQL, sqliteSQL string) bool {
		if hasColumn(table, column) {
			return true
		}
		stmt := sqliteSQL
		if d.driver == "postgres" {
			stmt = pgSQL
		}
		if _, err := d.db.ExecContext(ctx, stmt); err != nil {
			log.Printf("migrateV34: add column %s.%s failed: %v", table, column, err)
			return false
		}
		return true
	}

	if !addColumn("character_progression_config", "xp_curve_exponent",
		`ALTER TABLE character_progression_config ADD COLUMN xp_curve_exponent DOUBLE PRECISION NOT NULL DEFAULT 2.5`,
		`ALTER TABLE character_progression_config ADD COLUMN xp_curve_exponent REAL NOT NULL DEFAULT 2.5`) {
		return
	}
	if !addColumn("character_progression_config", "xp_irregularity",
		`ALTER TABLE character_progression_config ADD COLUMN xp_irregularity DOUBLE PRECISION NOT NULL DEFAULT 0.4`,
		`ALTER TABLE character_progression_config ADD COLUMN xp_irregularity REAL NOT NULL DEFAULT 0.4`) {
		return
	}
	if !addColumn("skill_progression_config", "xp_curve_exponent",
		`ALTER TABLE skill_progression_config ADD COLUMN xp_curve_exponent DOUBLE PRECISION NOT NULL DEFAULT 2.0`,
		`ALTER TABLE skill_progression_config ADD COLUMN xp_curve_exponent REAL NOT NULL DEFAULT 2.0`) {
		return
	}
	if !addColumn("skill_progression_config", "xp_irregularity",
		`ALTER TABLE skill_progression_config ADD COLUMN xp_irregularity DOUBLE PRECISION NOT NULL DEFAULT 0.5`,
		`ALTER TABLE skill_progression_config ADD COLUMN xp_irregularity REAL NOT NULL DEFAULT 0.5`) {
		return
	}
	if !addColumn("ability_templates", "mastery_xp_curve_exponent",
		`ALTER TABLE ability_templates ADD COLUMN mastery_xp_curve_exponent DOUBLE PRECISION NOT NULL DEFAULT 2.0`,
		`ALTER TABLE ability_templates ADD COLUMN mastery_xp_curve_exponent REAL NOT NULL DEFAULT 2.0`) {
		return
	}
	if !addColumn("ability_templates", "mastery_xp_irregularity",
		`ALTER TABLE ability_templates ADD COLUMN mastery_xp_irregularity DOUBLE PRECISION NOT NULL DEFAULT 0.5`,
		`ALTER TABLE ability_templates ADD COLUMN mastery_xp_irregularity REAL NOT NULL DEFAULT 0.5`) {
		return
	}

	tx, err := d.db.BeginTx(ctx, nil)
	if err != nil {
		log.Printf("migrateV34: begin tx failed: %v", err)
		return
	}
	defer func() { _ = tx.Rollback() }()

	if _, err := tx.ExecContext(ctx, d.q(`
		UPDATE character_progression_config
		   SET xp_curve_type = 'irregular',
		       xp_curve_base = 60,
		       xp_curve_exponent = 2.5,
		       xp_irregularity = 0.4
		 WHERE id = 1`)); err != nil {
		log.Printf("migrateV34: update character progression config failed: %v", err)
		return
	}
	if _, err := tx.ExecContext(ctx, d.q(`
		UPDATE skill_progression_config
		   SET xp_curve_type = 'irregular',
		       xp_curve_base = 40,
		       xp_curve_exponent = 2.0,
		       xp_irregularity = 0.5
		 WHERE id = 1`)); err != nil {
		log.Printf("migrateV34: update skill progression config failed: %v", err)
		return
	}
	if _, err := tx.ExecContext(ctx, d.q(`
		UPDATE ability_templates
		   SET mastery_xp_curve_type = 'irregular',
		       mastery_xp_curve_base = 40,
		       mastery_xp_curve_exponent = 2.0,
		       mastery_xp_irregularity = 0.5
		 WHERE LOWER(TRIM(mastery_xp_curve_type)) IN ('linear', 'quadratic', 'exponential')`)); err != nil {
		log.Printf("migrateV34: update ability mastery curves failed: %v", err)
		return
	}

	for _, u := range charUpdates {
		if _, err := tx.ExecContext(ctx, d.q(`UPDATE characters SET xp = ? WHERE id = ?`), u.xp, u.id); err != nil {
			log.Printf("migrateV34: update character xp failed (char=%s): %v", u.id, err)
			return
		}
	}
	for _, u := range masteryUpdates {
		if _, err := tx.ExecContext(ctx, d.q(`UPDATE character_skill_progress SET xp = ? WHERE id = ?`), u.xp, u.id); err != nil {
			log.Printf("migrateV34: update mastery xp failed (row=%d): %v", u.id, err)
			return
		}
	}

	if _, err := tx.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1"); err != nil {
		log.Printf("migrateV34: write meta flag failed: %v", err)
		return
	}

	if err := tx.Commit(); err != nil {
		log.Printf("migrateV34: commit failed: %v", err)
		return
	}

	log.Printf("migrateV34: converted XP back to cumulative irregular curves (characters=%d mastery_rows=%d)",
		len(charUpdates), len(masteryUpdates))
}

func (d *DB) migrateV35(ctx context.Context) {
	const conversionDoneKey = "migration_v35_stat_distribution_done"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV35: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	addCol := func(table, col, def string) {
		sql := fmt.Sprintf("ALTER TABLE %s ADD COLUMN %s %s", table, col, def)
		_, _ = d.db.ExecContext(ctx, sql)
	}

	addCol("characters", "primary_strength", "INTEGER NOT NULL DEFAULT 5")
	addCol("characters", "primary_dexterity", "INTEGER NOT NULL DEFAULT 5")
	addCol("characters", "primary_intelligence", "INTEGER NOT NULL DEFAULT 5")
	addCol("characters", "primary_wisdom", "INTEGER NOT NULL DEFAULT 5")
	addCol("characters", "primary_perception", "INTEGER NOT NULL DEFAULT 5")
	addCol("characters", "unspent_stat_points", "INTEGER NOT NULL DEFAULT 0")
	addCol("characters", "free_respecs_used", "INTEGER NOT NULL DEFAULT 0")

	addCol("character_progression_config", "stat_points_per_level", "INTEGER NOT NULL DEFAULT 5")
	addCol("character_progression_config", "initial_stat_value", "INTEGER NOT NULL DEFAULT 5")
	addCol("character_progression_config", "respec_free_until_level", "INTEGER NOT NULL DEFAULT 10")
	addCol("character_progression_config", "respec_cost_gold", "INTEGER NOT NULL DEFAULT 1000")

	_, _ = d.db.ExecContext(ctx, d.q(`
		UPDATE characters SET unspent_stat_points = (level - 1) * 5
		WHERE unspent_stat_points = 0 AND level > 1
	`))

	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1")
}

func (d *DB) migrateV36(ctx context.Context) {
	const conversionDoneKey = "migration_v36_xp_scaling_defaults_done"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV36: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	_, _ = d.db.ExecContext(ctx, d.q(`
		UPDATE kill_xp_scaling_config
		SET multiplier_min = 0.1, multiplier_max = 1.5
		WHERE id = 1 AND multiplier_min = 0.0 AND multiplier_max = 2.0
	`))

	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1")
}

func (d *DB) migrateV37(ctx context.Context) {
	const conversionDoneKey = "migration_v37_mastery_window_done"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV37: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	addCol := func(table, col, def string) {
		sql := fmt.Sprintf("ALTER TABLE %s ADD COLUMN %s %s", table, col, def)
		_, _ = d.db.ExecContext(ctx, sql)
	}
	addCol("kill_xp_scaling_config", "mastery_xp_per_mob_level", "INTEGER NOT NULL DEFAULT 10")
	addCol("kill_xp_scaling_config", "mastery_killing_blow_mult", "REAL NOT NULL DEFAULT 1.5")
	addCol("kill_xp_scaling_config", "mastery_window_timeout_ms", "INTEGER NOT NULL DEFAULT 10000")

	_, _ = d.db.ExecContext(ctx, d.q(`
		UPDATE kill_xp_scaling_config
		SET mastery_xp_per_mob_level = 10
		WHERE mastery_xp_per_mob_level IS NULL OR mastery_xp_per_mob_level <= 0
	`))
	_, _ = d.db.ExecContext(ctx, d.q(`
		UPDATE kill_xp_scaling_config
		SET mastery_killing_blow_mult = 1.5
		WHERE mastery_killing_blow_mult IS NULL OR mastery_killing_blow_mult < 1.0
	`))
	_, _ = d.db.ExecContext(ctx, d.q(`
		UPDATE kill_xp_scaling_config
		SET mastery_window_timeout_ms = 10000
		WHERE mastery_window_timeout_ms IS NULL OR mastery_window_timeout_ms < 1000
	`))

	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1")
}

func (d *DB) addColumnIfMissing(ctx context.Context, table, column, def string) {
	hasColumn := func() bool {
		if d.driver == "postgres" {
			var exists bool
			err := d.db.QueryRowContext(ctx, `
				SELECT EXISTS (
					SELECT 1 FROM information_schema.columns
					WHERE table_name = $1 AND column_name = $2
				)`, table, column).Scan(&exists)
			return err == nil && exists
		}
		rows, err := d.db.QueryContext(ctx, fmt.Sprintf(`PRAGMA table_info(%s)`, table))
		if err != nil {
			return false
		}
		defer rows.Close()
		for rows.Next() {
			var cid int
			var name, ctype string
			var notnull, pk int
			var dflt sql.NullString
			if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
				continue
			}
			if strings.EqualFold(strings.TrimSpace(name), column) {
				return true
			}
		}
		return false
	}

	if hasColumn() {
		return
	}
	sqlStmt := fmt.Sprintf("ALTER TABLE %s ADD COLUMN %s %s", table, column, def)
	_, _ = d.db.ExecContext(ctx, sqlStmt)
}

func (d *DB) migrateV38(ctx context.Context) {
	const conversionDoneKey = "migration_v38_vfx_paths_done"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV38: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	d.addColumnIfMissing(ctx, "ability_templates", "vfx_path_windup", "TEXT NOT NULL DEFAULT ''")
	d.addColumnIfMissing(ctx, "ability_templates", "vfx_path_impact", "TEXT NOT NULL DEFAULT ''")
	d.addColumnIfMissing(ctx, "ability_templates", "sfx_path_windup", "TEXT NOT NULL DEFAULT ''")
	d.addColumnIfMissing(ctx, "ability_templates", "sfx_path_impact", "TEXT NOT NULL DEFAULT ''")

	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1")
}

func (d *DB) migrateV39(ctx context.Context) {
	const conversionDoneKey = "migration_v39_fx_templates_done"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV39: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	createTableSQL := `
		CREATE TABLE IF NOT EXISTS fx_templates (
			id                  INTEGER PRIMARY KEY AUTOINCREMENT,
			fx_key              TEXT    NOT NULL UNIQUE,
			display_name        TEXT    NOT NULL DEFAULT '',

			-- Emission
			burst_count         INTEGER NOT NULL DEFAULT 0,
			stream_interval     REAL    NOT NULL DEFAULT 0.04,
			lifetime_seconds    REAL    NOT NULL DEFAULT 1.0,

			-- Velocity
			speed_min           REAL    NOT NULL DEFAULT 1.0,
			speed_max           REAL    NOT NULL DEFAULT 3.0,
			velocity_bias_x     REAL    NOT NULL DEFAULT 0.0,
			velocity_bias_y     REAL    NOT NULL DEFAULT 2.0,
			velocity_bias_z     REAL    NOT NULL DEFAULT 0.0,
			velocity_spread     REAL    NOT NULL DEFAULT 0.5,

			-- Visual
			color_start_r       REAL    NOT NULL DEFAULT 1.0,
			color_start_g       REAL    NOT NULL DEFAULT 0.5,
			color_start_b       REAL    NOT NULL DEFAULT 0.0,
			color_start_a       REAL    NOT NULL DEFAULT 1.0,
			color_end_r         REAL    NOT NULL DEFAULT 1.0,
			color_end_g         REAL    NOT NULL DEFAULT 0.0,
			color_end_b         REAL    NOT NULL DEFAULT 0.0,
			color_end_a         REAL    NOT NULL DEFAULT 0.0,
			size_start          REAL    NOT NULL DEFAULT 8.0,
			size_end            REAL    NOT NULL DEFAULT 2.0,
			texture_path        TEXT    NOT NULL DEFAULT '',

			enabled             INTEGER NOT NULL DEFAULT 1
		)`
	if d.driver == "postgres" {
		createTableSQL = `
			CREATE TABLE IF NOT EXISTS fx_templates (
				id                  SERIAL PRIMARY KEY,
				fx_key              TEXT    NOT NULL UNIQUE,
				display_name        TEXT    NOT NULL DEFAULT '',

				-- Emission
				burst_count         INTEGER NOT NULL DEFAULT 0,
				stream_interval     REAL    NOT NULL DEFAULT 0.04,
				lifetime_seconds    REAL    NOT NULL DEFAULT 1.0,

				-- Velocity
				speed_min           REAL    NOT NULL DEFAULT 1.0,
				speed_max           REAL    NOT NULL DEFAULT 3.0,
				velocity_bias_x     REAL    NOT NULL DEFAULT 0.0,
				velocity_bias_y     REAL    NOT NULL DEFAULT 2.0,
				velocity_bias_z     REAL    NOT NULL DEFAULT 0.0,
				velocity_spread     REAL    NOT NULL DEFAULT 0.5,

				-- Visual
				color_start_r       REAL    NOT NULL DEFAULT 1.0,
				color_start_g       REAL    NOT NULL DEFAULT 0.5,
				color_start_b       REAL    NOT NULL DEFAULT 0.0,
				color_start_a       REAL    NOT NULL DEFAULT 1.0,
				color_end_r         REAL    NOT NULL DEFAULT 1.0,
				color_end_g         REAL    NOT NULL DEFAULT 0.0,
				color_end_b         REAL    NOT NULL DEFAULT 0.0,
				color_end_a         REAL    NOT NULL DEFAULT 0.0,
				size_start          REAL    NOT NULL DEFAULT 8.0,
				size_end            REAL    NOT NULL DEFAULT 2.0,
				texture_path        TEXT    NOT NULL DEFAULT '',

				enabled             INTEGER NOT NULL DEFAULT 1
			)`
	}
	if _, err := d.db.ExecContext(ctx, createTableSQL); err != nil {
		log.Printf("migrateV39: create fx_templates failed: %v", err)
		return
	}

	var count int
	if err := d.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM fx_templates`).Scan(&count); err != nil {
		log.Printf("migrateV39: count fx_templates failed: %v", err)
		return
	}
	if count == 0 {
		d.seedDefaultFXTemplates(ctx)
	}

	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1")
}

func (d *DB) migrateV40(ctx context.Context) {
	const conversionDoneKey = "schema_v40_loot_tables"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV40: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	createLootTablesSQL := `
		CREATE TABLE IF NOT EXISTS loot_tables (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			name TEXT NOT NULL,
			enabled INTEGER NOT NULL DEFAULT 1
		)`
	createLootEntriesSQL := `
		CREATE TABLE IF NOT EXISTS loot_entries (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			loot_table_id INTEGER NOT NULL,
			item_id INTEGER NOT NULL,
			chance REAL NOT NULL DEFAULT 0,
			min_qty INTEGER NOT NULL DEFAULT 1,
			max_qty INTEGER NOT NULL DEFAULT 1
		)`
	if d.driver == "postgres" {
		createLootTablesSQL = `
			CREATE TABLE IF NOT EXISTS loot_tables (
				id SERIAL PRIMARY KEY,
				name TEXT NOT NULL,
				enabled INTEGER NOT NULL DEFAULT 1
			)`
		createLootEntriesSQL = `
			CREATE TABLE IF NOT EXISTS loot_entries (
				id SERIAL PRIMARY KEY,
				loot_table_id INTEGER NOT NULL,
				item_id INTEGER NOT NULL,
				chance REAL NOT NULL DEFAULT 0,
				min_qty INTEGER NOT NULL DEFAULT 1,
				max_qty INTEGER NOT NULL DEFAULT 1
			)`
	}
	if _, err := d.db.ExecContext(ctx, createLootTablesSQL); err != nil {
		log.Printf("migrateV40: create loot_tables failed: %v", err)
		return
	}
	if _, err := d.db.ExecContext(ctx, createLootEntriesSQL); err != nil {
		log.Printf("migrateV40: create loot_entries failed: %v", err)
		return
	}

	d.addColumnIfMissing(ctx, "media_actor_defs", "loot_table_id", "INTEGER NOT NULL DEFAULT 0")

	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1")
}

func (d *DB) migrateV41(ctx context.Context) {
	const conversionDoneKey = "schema_v41_game_settings"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV41: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	if _, err := d.db.ExecContext(ctx,
		`CREATE TABLE IF NOT EXISTS game_settings (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)` ); err != nil {
		log.Printf("migrateV41: create game_settings failed: %v", err)
		return
	}

	_, _ = d.db.ExecContext(ctx,
		`INSERT INTO game_settings (key, value)
		 VALUES ('default_drop_model_id', '')
		 ON CONFLICT(key) DO NOTHING`)

	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1")
}

func (d *DB) migrateV42(ctx context.Context) {
	const conversionDoneKey = "schema_v42_item_attributes"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV42: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	createItemAttributesSQL := `
		CREATE TABLE IF NOT EXISTS item_attributes (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			item_id INTEGER NOT NULL,
			attribute_key TEXT NOT NULL,
			value REAL NOT NULL DEFAULT 0
		)`
	createItemAttributeIndexesSQL := `CREATE INDEX IF NOT EXISTS idx_item_attributes_item ON item_attributes(item_id)`
	if d.driver == "postgres" {
		createItemAttributesSQL = `
			CREATE TABLE IF NOT EXISTS item_attributes (
				id SERIAL PRIMARY KEY,
				item_id INTEGER NOT NULL,
				attribute_key TEXT NOT NULL,
				value REAL NOT NULL DEFAULT 0
			)`
	}
	if _, err := d.db.ExecContext(ctx, createItemAttributesSQL); err != nil {
		log.Printf("migrateV42: create item_attributes failed: %v", err)
		return
	}
	if _, err := d.db.ExecContext(ctx, createItemAttributeIndexesSQL); err != nil {
		log.Printf("migrateV42: create item_attributes index failed: %v", err)
		return
	}

	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1")
}

// columnExists reports whether the given table has a column with the given
// name, on both the sqlite and postgres drivers.
func (d *DB) columnExists(ctx context.Context, table, column string) bool {
	if d.driver == "postgres" {
		var exists int
		err := d.db.QueryRowContext(ctx,
			`SELECT 1 FROM information_schema.columns WHERE table_name = $1 AND column_name = $2`,
			table, column,
		).Scan(&exists)
		return err == nil
	}

	rows, err := d.db.QueryContext(ctx, `PRAGMA table_info(`+table+`)`)
	if err != nil {
		log.Printf("columnExists: PRAGMA table_info(%s) failed: %v", table, err)
		return false
	}
	defer rows.Close()

	for rows.Next() {
		var cid int
		var name, ctype string
		var notnull, pk int
		var dflt sql.NullString
		if err := rows.Scan(&cid, &name, &ctype, &notnull, &dflt, &pk); err != nil {
			continue
		}
		if name == column {
			return true
		}
	}
	return false
}

// migrateV43 splits the legacy item_templates.weapon_type column (which mixed
// hand-grip and combat dimension) into two clean columns:
//   - weapon_dimension: 0=melee, 1=ranged, 2=magic (matches world.CombatDimension)
//   - weapon_hands: 1=one-hand, 2=two-hand (no mechanical effect yet)
//
// It migrates existing data from weapon_type, then drops the legacy column.
func (d *DB) migrateV43(ctx context.Context) {
	const conversionDoneKey = "schema_v43_weapon_dimension"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV43: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	var addCols []string
	if d.driver == "postgres" {
		addCols = []string{
			"ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS weapon_dimension INTEGER NOT NULL DEFAULT 0",
			"ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS weapon_hands     INTEGER NOT NULL DEFAULT 1",
		}
	} else {
		addCols = []string{
			"ALTER TABLE item_templates ADD COLUMN weapon_dimension INTEGER NOT NULL DEFAULT 0",
			"ALTER TABLE item_templates ADD COLUMN weapon_hands     INTEGER NOT NULL DEFAULT 1",
		}
	}
	for _, sql := range addCols {
		// SQLite has no "ADD COLUMN IF NOT EXISTS"; ignore errors if the
		// column already exists from a partially-applied migration.
		if _, err := d.db.ExecContext(ctx, sql); err != nil {
			log.Printf("migrateV43: add column (may already exist): %v", err)
		}
	}

	// Migrate data from the legacy weapon_type column before dropping it.
	// If a previous run already dropped weapon_type but died before
	// recording the meta-key, skip straight to marking done.
	if d.columnExists(ctx, "item_templates", "weapon_type") {
		if _, err := d.db.ExecContext(ctx, `
			UPDATE item_templates SET
				weapon_dimension = CASE weapon_type
					WHEN 3 THEN 1 -- ranged
					WHEN 4 THEN 2 -- magic
					ELSE 0        -- 0,1,2 -> melee
				END,
				weapon_hands = CASE weapon_type
					WHEN 2 THEN 2 -- two-hand
					WHEN 3 THEN 2 -- ranged: conventionally two-handed
					ELSE 1        -- 0,1,4 -> one-hand (default)
				END
			WHERE weapon_type IS NOT NULL`); err != nil {
			log.Printf("migrateV43: backfill weapon_dimension/weapon_hands failed: %v", err)
			return
		}

		if _, err := d.db.ExecContext(ctx, `ALTER TABLE item_templates DROP COLUMN weapon_type`); err != nil {
			log.Printf("migrateV43: drop weapon_type column failed: %v", err)
			return
		}
	}

	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1")
}

// migrateV44 adds item_templates.weapon_range, the explicit attack range for a
// weapon (world units). A value of 0 means "no explicit range" — the basic
// attack falls back to world.DefaultRangeForDimension(weapon_dimension).
// Purely additive; no data migration or column drop needed.
func (d *DB) migrateV44(ctx context.Context) {
	const conversionDoneKey = "schema_v44_weapon_range"

	if _, err := d.db.ExecContext(ctx, `
		CREATE TABLE IF NOT EXISTS meta (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		)`); err != nil {
		log.Printf("migrateV44: ensure meta table failed: %v", err)
		return
	}

	var doneValue string
	doneErr := d.db.QueryRowContext(ctx, d.q(`SELECT value FROM meta WHERE key = ?`), conversionDoneKey).Scan(&doneValue)
	if doneErr == nil && strings.TrimSpace(doneValue) == "1" {
		return
	}

	var addCol string
	if d.driver == "postgres" {
		addCol = "ALTER TABLE item_templates ADD COLUMN IF NOT EXISTS weapon_range REAL NOT NULL DEFAULT 0"
	} else {
		addCol = "ALTER TABLE item_templates ADD COLUMN weapon_range REAL NOT NULL DEFAULT 0"
	}
	// SQLite has no "ADD COLUMN IF NOT EXISTS"; ignore errors if the column
	// already exists from a partially-applied migration.
	if _, err := d.db.ExecContext(ctx, addCol); err != nil {
		log.Printf("migrateV44: add column (may already exist): %v", err)
	}

	_, _ = d.db.ExecContext(ctx, d.q(`
		INSERT INTO meta (key, value)
		VALUES (?, ?)
		ON CONFLICT(key) DO UPDATE SET value = excluded.value`),
		conversionDoneKey, "1")
}

// migrateV45 adds the optional combat-dimension override column to
// ability_templates. Empty string means "inherit the attacker's basic-attack
// dimension" (C3a foundation; the runtime does not read this column yet).
func (d *DB) migrateV45(ctx context.Context) {
	d.addColumnIfMissing(ctx, "ability_templates", "dimension", "TEXT NOT NULL DEFAULT ''")
}

// migrateV46 creates anim_vocabulary, a tree of animation action names
// (parent_id = fallback target when an actor has no binding for a given
// action; parent_id = 0 means root / no fallback). Phase A.1 foundation:
// media_actor_anims and ability_templates remain free-string and untouched —
// this table only seeds an editable vocabulary for the GUE and a fallback map
// the server can load (SetAnimVocabulary/AnimFallbackParent), not yet
// consulted by BroadcastAnimate.
func (d *DB) migrateV46(ctx context.Context) {
	if d.driver == "postgres" {
		if _, err := d.db.ExecContext(ctx, `
			CREATE TABLE IF NOT EXISTS anim_vocabulary (
				id        SERIAL PRIMARY KEY,
				name      VARCHAR(64) NOT NULL UNIQUE,
				parent_id INTEGER NOT NULL DEFAULT 0
			)`); err != nil {
			log.Printf("migrateV46: create anim_vocabulary failed: %v", err)
			return
		}
	} else {
		if _, err := d.db.ExecContext(ctx, `
			CREATE TABLE IF NOT EXISTS anim_vocabulary (
				id        INTEGER PRIMARY KEY AUTOINCREMENT,
				name      TEXT NOT NULL UNIQUE,
				parent_id INTEGER NOT NULL DEFAULT 0
			)`); err != nil {
			log.Printf("migrateV46: create anim_vocabulary failed: %v", err)
			return
		}
	}

	d.seedAnimVocabulary(ctx)
}

// seedAnimVocabulary populates anim_vocabulary with the default action tree,
// plus any action strings already in use (ability_templates.action_windup/
// action_impact/action_recover, media_actor_anims.action) that aren't part of
// the default tree, as extra roots. Only runs when the table is empty.
func (d *DB) seedAnimVocabulary(ctx context.Context) {
	var count int
	if err := d.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM anim_vocabulary`).Scan(&count); err != nil {
		log.Printf("migrateV46: count anim_vocabulary failed: %v", err)
		return
	}
	if count > 0 {
		return
	}

	insert := func(name, parentName string) {
		var parentID int64 = 0
		if parentName != "" {
			if err := d.db.QueryRowContext(ctx, d.q(`SELECT id FROM anim_vocabulary WHERE name = ?`), parentName).Scan(&parentID); err != nil {
				log.Printf("migrateV46: seed %q: parent %q not found: %v", name, parentName, err)
				return
			}
		}
		if _, err := d.db.ExecContext(ctx, d.q(`INSERT INTO anim_vocabulary (name, parent_id) VALUES (?, ?)`), name, parentID); err != nil {
			log.Printf("migrateV46: seed insert %q failed: %v", name, err)
		}
	}

	// Roots
	for _, name := range []string{"Idle", "Locomotion", "Jump", "Attack", "Cast", "Defend", "Hit", "Death", "Emote"} {
		insert(name, "")
	}

	// Children of roots (topological order: parents must exist already)
	type child struct{ name, parent string }
	for _, c := range []child{
		{"Walk", "Locomotion"}, {"Run", "Locomotion"}, {"Sprint", "Locomotion"},
		{"Slash", "Attack"}, {"Thrust", "Attack"}, {"Bash", "Attack"}, {"Shoot", "Attack"}, {"Unarmed", "Attack"},
		{"CastUp", "Cast"}, {"CastForward", "Cast"}, {"CastSelf", "Cast"},
		{"Block", "Defend"}, {"Dodge", "Defend"}, {"Roll", "Defend"}, {"Parry", "Defend"},
		{"HitHeavy", "Hit"},
		{"Wave", "Emote"}, {"Dance", "Emote"}, {"Sit", "Emote"},
	} {
		insert(c.name, c.parent)
	}

	// Grandchildren: BowDraw/BowRelease are children of Shoot (itself a child of Attack).
	for _, c := range []child{
		{"BowDraw", "Shoot"}, {"BowRelease", "Shoot"},
	} {
		insert(c.name, c.parent)
	}

	d.seedExtraAnimActions(ctx)
}

// seedExtraAnimActions scans ability_templates' action_windup/action_impact/
// action_recover columns and media_actor_anims.action for non-empty action
// strings already in use that aren't covered by the default vocabulary tree,
// and inserts them as extra roots so they don't disappear from GUE dropdowns.
func (d *DB) seedExtraAnimActions(ctx context.Context) {
	extras := map[string]bool{}

	if rows, err := d.db.QueryContext(ctx, `SELECT action_windup, action_impact, action_recover FROM ability_templates`); err == nil {
		for rows.Next() {
			var w, i, r string
			if scanErr := rows.Scan(&w, &i, &r); scanErr == nil {
				for _, a := range []string{w, i, r} {
					a = strings.TrimSpace(a)
					if a != "" {
						extras[a] = true
					}
				}
			}
		}
		rows.Close()
	}

	if rows, err := d.db.QueryContext(ctx, `SELECT DISTINCT action FROM media_actor_anims`); err == nil {
		for rows.Next() {
			var a string
			if scanErr := rows.Scan(&a); scanErr == nil {
				a = strings.TrimSpace(a)
				if a != "" {
					extras[a] = true
				}
			}
		}
		rows.Close()
	}

	for name := range extras {
		var existingID int64
		if err := d.db.QueryRowContext(ctx, d.q(`SELECT id FROM anim_vocabulary WHERE name = ?`), name).Scan(&existingID); err == nil {
			continue // already in the vocabulary (default tree)
		}
		if _, err := d.db.ExecContext(ctx, d.q(`INSERT INTO anim_vocabulary (name, parent_id) VALUES (?, 0)`), name); err != nil {
			log.Printf("migrateV46: seed extra action %q failed: %v", name, err)
			continue
		}
		log.Printf("migrateV46: seeded extra action %q as root (found in ability_templates/media_actor_anims)", name)
	}
}

// migrateV47 creates socket_vocabulary, a flat list of attachment socket names
// (WeaponHand, OffHand, Head, …). No tree — each socket is independent.
// Arco B / B2. Server loading deferred to B3 when actor defs consume them.
func (d *DB) migrateV47(ctx context.Context) {
	if d.driver == "postgres" {
		if _, err := d.db.ExecContext(ctx, `
			CREATE TABLE IF NOT EXISTS socket_vocabulary (
				id   SERIAL      PRIMARY KEY,
				name VARCHAR(64) NOT NULL UNIQUE
			)`); err != nil {
			log.Printf("migrateV47: create socket_vocabulary failed: %v", err)
			return
		}
	} else {
		if _, err := d.db.ExecContext(ctx, `
			CREATE TABLE IF NOT EXISTS socket_vocabulary (
				id   INTEGER PRIMARY KEY AUTOINCREMENT,
				name TEXT    NOT NULL UNIQUE
			)`); err != nil {
			log.Printf("migrateV47: create socket_vocabulary failed: %v", err)
			return
		}
	}

	d.seedSocketVocabulary(ctx)
}

func (d *DB) seedSocketVocabulary(ctx context.Context) {
	var count int
	if err := d.db.QueryRowContext(ctx, `SELECT COUNT(*) FROM socket_vocabulary`).Scan(&count); err != nil {
		log.Printf("migrateV47: count socket_vocabulary failed: %v", err)
		return
	}
	if count > 0 {
		return
	}

	defaults := []string{"WeaponHand", "OffHand", "Head", "Back", "Hip", "Shoulder_R", "Shoulder_L", "Chest"}
	for _, name := range defaults {
		if _, err := d.db.ExecContext(ctx, d.q(`INSERT INTO socket_vocabulary (name) VALUES (?)`), name); err != nil {
			log.Printf("migrateV47: seed insert %q failed: %v", name, err)
		}
	}
}

// SocketVocabEntry mirrors one row in socket_vocabulary.
type SocketVocabEntry struct {
	ID   int
	Name string
}

// LoadSocketVocabulary returns all socket names ordered alphabetically.
// Deferred to B3 consumer; exposed here so the server can load them when needed.
func (d *DB) LoadSocketVocabulary(ctx context.Context) ([]SocketVocabEntry, error) {
	rows, err := d.db.QueryContext(ctx, `SELECT id, name FROM socket_vocabulary ORDER BY name`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []SocketVocabEntry
	for rows.Next() {
		var e SocketVocabEntry
		if err2 := rows.Scan(&e.ID, &e.Name); err2 != nil {
			return nil, err2
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

// migrateV48 creates actor_def_sockets, which maps each actor def's attachment
// sockets to a bone name + pos/rot/scale offset. Flat rows, no cascade delete —
// orphaned rows are harmless if an actor def is deleted. Arco B / B3a.
func (d *DB) migrateV48(ctx context.Context) {
	if d.driver == "postgres" {
		if _, err := d.db.ExecContext(ctx, `
			CREATE TABLE IF NOT EXISTS actor_def_sockets (
				id           SERIAL       PRIMARY KEY,
				actor_def_id INTEGER      NOT NULL,
				socket_name  VARCHAR(64)  NOT NULL DEFAULT '',
				bone_name    VARCHAR(128) NOT NULL DEFAULT '',
				offset_pos_x REAL         NOT NULL DEFAULT 0.0,
				offset_pos_y REAL         NOT NULL DEFAULT 0.0,
				offset_pos_z REAL         NOT NULL DEFAULT 0.0,
				offset_rot_x REAL         NOT NULL DEFAULT 0.0,
				offset_rot_y REAL         NOT NULL DEFAULT 0.0,
				offset_rot_z REAL         NOT NULL DEFAULT 0.0,
				offset_scale REAL         NOT NULL DEFAULT 1.0
			)`); err != nil {
			log.Printf("migrateV48: create actor_def_sockets failed: %v", err)
		}
	} else {
		if _, err := d.db.ExecContext(ctx, `
			CREATE TABLE IF NOT EXISTS actor_def_sockets (
				id           INTEGER PRIMARY KEY AUTOINCREMENT,
				actor_def_id INTEGER NOT NULL,
				socket_name  TEXT    NOT NULL DEFAULT '',
				bone_name    TEXT    NOT NULL DEFAULT '',
				offset_pos_x REAL   NOT NULL DEFAULT 0.0,
				offset_pos_y REAL   NOT NULL DEFAULT 0.0,
				offset_pos_z REAL   NOT NULL DEFAULT 0.0,
				offset_rot_x REAL   NOT NULL DEFAULT 0.0,
				offset_rot_y REAL   NOT NULL DEFAULT 0.0,
				offset_rot_z REAL   NOT NULL DEFAULT 0.0,
				offset_scale REAL   NOT NULL DEFAULT 1.0
			)`); err != nil {
			log.Printf("migrateV48: create actor_def_sockets failed: %v", err)
		}
	}
}

// migrateV49 adds item rendering metadata for attached item meshes:
// - item_templates.model_path / model_scale / socket_name
// - item_socket_overrides: optional per (item_template_id, actor_def_id) transform overrides
func (d *DB) migrateV49(ctx context.Context) {
	if d.driver == "postgres" {
		d.addColumnIfMissing(ctx, "item_templates", "model_path", "VARCHAR(512) NOT NULL DEFAULT ''")
		d.addColumnIfMissing(ctx, "item_templates", "model_scale", "REAL NOT NULL DEFAULT 1.0")
		d.addColumnIfMissing(ctx, "item_templates", "socket_name", "VARCHAR(64) NOT NULL DEFAULT ''")
	} else {
		d.addColumnIfMissing(ctx, "item_templates", "model_path", "TEXT NOT NULL DEFAULT ''")
		d.addColumnIfMissing(ctx, "item_templates", "model_scale", "REAL NOT NULL DEFAULT 1.0")
		d.addColumnIfMissing(ctx, "item_templates", "socket_name", "TEXT NOT NULL DEFAULT ''")
	}

	var createTable string
	if d.driver == "postgres" {
		createTable = `
			CREATE TABLE IF NOT EXISTS item_socket_overrides (
				id               SERIAL       PRIMARY KEY,
				item_template_id INTEGER      NOT NULL,
				actor_def_id     INTEGER      NOT NULL,
				offset_pos_x     REAL         NOT NULL DEFAULT 0.0,
				offset_pos_y     REAL         NOT NULL DEFAULT 0.0,
				offset_pos_z     REAL         NOT NULL DEFAULT 0.0,
				offset_rot_x     REAL         NOT NULL DEFAULT 0.0,
				offset_rot_y     REAL         NOT NULL DEFAULT 0.0,
				offset_rot_z     REAL         NOT NULL DEFAULT 0.0,
				offset_scale     REAL         NOT NULL DEFAULT 1.0
			)`
	} else {
		createTable = `
			CREATE TABLE IF NOT EXISTS item_socket_overrides (
				id               INTEGER PRIMARY KEY AUTOINCREMENT,
				item_template_id INTEGER      NOT NULL,
				actor_def_id     INTEGER      NOT NULL,
				offset_pos_x     REAL         NOT NULL DEFAULT 0.0,
				offset_pos_y     REAL         NOT NULL DEFAULT 0.0,
				offset_pos_z     REAL         NOT NULL DEFAULT 0.0,
				offset_rot_x     REAL         NOT NULL DEFAULT 0.0,
				offset_rot_y     REAL         NOT NULL DEFAULT 0.0,
				offset_rot_z     REAL         NOT NULL DEFAULT 0.0,
				offset_scale     REAL         NOT NULL DEFAULT 1.0
			)`
	}

	if _, err := d.db.ExecContext(ctx, createTable); err != nil {
		log.Printf("migrateV49: create item_socket_overrides failed: %v", err)
	}
}

// ActorDefSocket mirrors one row in actor_def_sockets.
type ActorDefSocket struct {
	ID          int
	ActorDefID  int
	SocketName  string
	BoneName    string
	OffsetPosX  float64
	OffsetPosY  float64
	OffsetPosZ  float64
	OffsetRotX  float64
	OffsetRotY  float64
	OffsetRotZ  float64
	OffsetScale float64
}

// LoadActorDefSockets returns all socket bindings for the given actor def.
func (d *DB) LoadActorDefSockets(ctx context.Context, actorDefID int) ([]ActorDefSocket, error) {
	rows, err := d.db.QueryContext(ctx, d.q(
		`SELECT id, actor_def_id, socket_name, bone_name,
		        offset_pos_x, offset_pos_y, offset_pos_z,
		        offset_rot_x, offset_rot_y, offset_rot_z, offset_scale
		 FROM actor_def_sockets WHERE actor_def_id = ? ORDER BY id`), actorDefID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ActorDefSocket
	for rows.Next() {
		var s ActorDefSocket
		if err2 := rows.Scan(
			&s.ID, &s.ActorDefID, &s.SocketName, &s.BoneName,
			&s.OffsetPosX, &s.OffsetPosY, &s.OffsetPosZ,
			&s.OffsetRotX, &s.OffsetRotY, &s.OffsetRotZ, &s.OffsetScale,
		); err2 != nil {
			return nil, err2
		}
		out = append(out, s)
	}
	return out, rows.Err()
}

// LoadItemSocketOverride resolves a (item_template_id, actor_def_id) render
// override. Returns (override, found, error).
func (d *DB) LoadItemSocketOverride(ctx context.Context, itemTemplateID, actorDefID int) (*ItemSocketOverride, bool, error) {
	if itemTemplateID <= 0 || actorDefID <= 0 {
		return nil, false, nil
	}

	row := d.db.QueryRowContext(ctx, d.q(
		`SELECT id, item_template_id, actor_def_id,
		          offset_pos_x, offset_pos_y, offset_pos_z,
		          offset_rot_x, offset_rot_y, offset_rot_z, offset_scale
		   FROM item_socket_overrides
		   WHERE item_template_id = ? AND actor_def_id = ?
		   LIMIT 1`),
		itemTemplateID, actorDefID)

	var o ItemSocketOverride
	if err := row.Scan(
		&o.ID, &o.ItemTemplateID, &o.ActorDefID,
		&o.OffsetPosX, &o.OffsetPosY, &o.OffsetPosZ,
		&o.OffsetRotX, &o.OffsetRotY, &o.OffsetRotZ, &o.OffsetScale,
	); err != nil {
		if err == sql.ErrNoRows {
			return nil, false, nil
		}
		return nil, false, fmt.Errorf("db: LoadItemSocketOverride: %w", err)
	}
	return &o, true, nil
}

// AnimVocabNode mirrors one row in anim_vocabulary.
type AnimVocabNode struct {
	ID       int
	Name     string
	ParentID int
}

// LoadAnimVocabulary returns the full animation vocabulary tree.
func (d *DB) LoadAnimVocabulary(ctx context.Context) ([]AnimVocabNode, error) {
	rows, err := d.db.QueryContext(ctx, `SELECT id, name, parent_id FROM anim_vocabulary`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var out []AnimVocabNode
	for rows.Next() {
		var n AnimVocabNode
		if err := rows.Scan(&n.ID, &n.Name, &n.ParentID); err != nil {
			return nil, err
		}
		out = append(out, n)
	}
	return out, rows.Err()
}

func (d *DB) seedDefaultFXTemplates(ctx context.Context) {
	defaults := []FXTemplate{
		{
			Key:             "vfx:fire",
			DisplayName:     "Fire Burst",
			BurstCount:      0,
			StreamInterval:  0.04,
			LifetimeSeconds: 1.2,
			SpeedMin:        1.5,
			SpeedMax:        3.0,
			VelocityBiasX:   0.0,
			VelocityBiasY:   2.5,
			VelocityBiasZ:   0.0,
			VelocitySpread:  0.5,
			ColorStartR:     1.0,
			ColorStartG:     0.55,
			ColorStartB:     0.05,
			ColorStartA:     0.9,
			ColorEndR:       0.8,
			ColorEndG:       0.1,
			ColorEndB:       0.0,
			ColorEndA:       0.0,
			SizeStart:       8.0,
			SizeEnd:         2.0,
			TexturePath:     "",
			Enabled:         true,
		},
		{
			Key:             "vfx:explosion",
			DisplayName:     "Explosion",
			BurstCount:      40,
			StreamInterval:  0.0,
			LifetimeSeconds: 0.8,
			SpeedMin:        3.0,
			SpeedMax:        7.0,
			VelocityBiasX:   0.0,
			VelocityBiasY:   1.0,
			VelocityBiasZ:   0.0,
			VelocitySpread:  3.14159,
			ColorStartR:     1.0,
			ColorStartG:     0.6,
			ColorStartB:     0.0,
			ColorStartA:     1.0,
			ColorEndR:       0.3,
			ColorEndG:       0.0,
			ColorEndB:       0.0,
			ColorEndA:       0.0,
			SizeStart:       12.0,
			SizeEnd:         2.0,
			TexturePath:     "",
			Enabled:         true,
		},
		{
			Key:             "vfx:heal",
			DisplayName:     "Healing Light",
			BurstCount:      20,
			StreamInterval:  0.0,
			LifetimeSeconds: 1.4,
			SpeedMin:        1.0,
			SpeedMax:        2.5,
			VelocityBiasX:   0.0,
			VelocityBiasY:   2.0,
			VelocityBiasZ:   0.0,
			VelocitySpread:  0.7,
			ColorStartR:     0.2,
			ColorStartG:     1.0,
			ColorStartB:     0.4,
			ColorStartA:     0.9,
			ColorEndR:       0.0,
			ColorEndG:       0.5,
			ColorEndB:       0.1,
			ColorEndA:       0.0,
			SizeStart:       6.0,
			SizeEnd:         2.0,
			TexturePath:     "",
			Enabled:         true,
		},
		{
			Key:             "vfx:portal",
			DisplayName:     "Portal Swirl",
			BurstCount:      0,
			StreamInterval:  0.06,
			LifetimeSeconds: 1.8,
			SpeedMin:        1.5,
			SpeedMax:        3.0,
			VelocityBiasX:   0.0,
			VelocityBiasY:   0.5,
			VelocityBiasZ:   0.0,
			VelocitySpread:  3.14159,
			ColorStartR:     0.0,
			ColorStartG:     0.8,
			ColorStartB:     1.0,
			ColorStartA:     0.8,
			ColorEndR:       0.0,
			ColorEndG:       0.2,
			ColorEndB:       0.6,
			ColorEndA:       0.0,
			SizeStart:       5.0,
			SizeEnd:         1.5,
			TexturePath:     "",
			Enabled:         true,
		},
		{
			Key:             "vfx:blood",
			DisplayName:     "Blood Splat",
			BurstCount:      15,
			StreamInterval:  0.0,
			LifetimeSeconds: 0.4,
			SpeedMin:        2.0,
			SpeedMax:        5.0,
			VelocityBiasX:   0.0,
			VelocityBiasY:   -1.5,
			VelocityBiasZ:   0.0,
			VelocitySpread:  1.8,
			ColorStartR:     0.9,
			ColorStartG:     0.0,
			ColorStartB:     0.0,
			ColorStartA:     1.0,
			ColorEndR:       0.4,
			ColorEndG:       0.0,
			ColorEndB:       0.0,
			ColorEndA:       0.0,
			SizeStart:       5.0,
			SizeEnd:         1.0,
			TexturePath:     "",
			Enabled:         true,
		},
		{
			Key:             "vfx:smoke",
			DisplayName:     "Smoke",
			BurstCount:      0,
			StreamInterval:  0.12,
			LifetimeSeconds: 2.0,
			SpeedMin:        0.3,
			SpeedMax:        0.8,
			VelocityBiasX:   0.0,
			VelocityBiasY:   1.2,
			VelocityBiasZ:   0.0,
			VelocitySpread:  0.4,
			ColorStartR:     0.5,
			ColorStartG:     0.5,
			ColorStartB:     0.5,
			ColorStartA:     0.5,
			ColorEndR:       0.3,
			ColorEndG:       0.3,
			ColorEndB:       0.3,
			ColorEndA:       0.0,
			SizeStart:       10.0,
			SizeEnd:         6.0,
			TexturePath:     "",
			Enabled:         true,
		},
	}

	for i := range defaults {
		t := defaults[i]
		if err := d.UpsertFXTemplate(ctx, &t); err != nil {
			log.Printf("seedDefaultFXTemplates: insert %s failed: %v", t.Key, err)
		}
	}
	log.Printf("seedDefaultFXTemplates: seeded %d defaults", len(defaults))
}

// LoadWorldObjects returns all placed static world objects from zone_scenery with resolved model paths.
func (d *DB) LoadWorldObjects(ctx context.Context) ([]*WorldObject, error) {
	rows, err := d.db.QueryContext(ctx, d.q(
		`SELECT zs.id, zs.area_name, COALESCE(mm.file_path,''), zs.sx, zs.x, zs.y, zs.z, zs.yaw
		 FROM zone_scenery zs
		 LEFT JOIN media_models mm ON mm.id = zs.model_id
		 ORDER BY zs.area_name, zs.id`))
	if err != nil {
		return nil, fmt.Errorf("db: LoadWorldObjects: %w", err)
	}
	defer rows.Close()
	var out []*WorldObject
	for rows.Next() {
		w := &WorldObject{}
		var scale, x, y, z, yaw float64
		if err := rows.Scan(&w.ID, &w.AreaName, &w.ModelPath,
			&scale, &x, &y, &z, &yaw); err != nil {
			return nil, fmt.Errorf("db: LoadWorldObjects scan: %w", err)
		}
		w.Scale = float32(scale)
		w.X, w.Y, w.Z, w.Yaw = float32(x), float32(y), float32(z), float32(yaw)
		out = append(out, w)
	}
	return out, rows.Err()
}

// migrateV15 adds random wander fields to npc_spawns.
func (d *DB) migrateV15(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	if d.driver == "postgres" {
		exec(`ALTER TABLE npc_spawns ADD COLUMN IF NOT EXISTS wander_radius       REAL    NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE npc_spawns ADD COLUMN IF NOT EXISTS wander_pause_min_ms INTEGER NOT NULL DEFAULT 2000`)
		exec(`ALTER TABLE npc_spawns ADD COLUMN IF NOT EXISTS wander_pause_max_ms INTEGER NOT NULL DEFAULT 5000`)
	} else {
		exec(`ALTER TABLE npc_spawns ADD COLUMN wander_radius       REAL    NOT NULL DEFAULT 0`)
		exec(`ALTER TABLE npc_spawns ADD COLUMN wander_pause_min_ms INTEGER NOT NULL DEFAULT 2000`)
		exec(`ALTER TABLE npc_spawns ADD COLUMN wander_pause_max_ms INTEGER NOT NULL DEFAULT 5000`)
	}
}

// LoadSpawnPointMobs returns all mob entries for a given spawn point.
func (d *DB) LoadSpawnPointMobs(ctx context.Context, spawnPointID int) ([]*SpawnPointMob, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, spawn_point_id, actor_def_id, mob_count, name, race, class,
		        level, aggressiveness, aggressive_range, attack_range, respawn_delay_ms
		 FROM spawn_point_mobs WHERE spawn_point_id = ? ORDER BY id`,
		spawnPointID)
	if err != nil {
		return nil, fmt.Errorf("db: LoadSpawnPointMobs: %w", err)
	}
	defer rows.Close()
	var out []*SpawnPointMob
	for rows.Next() {
		m := &SpawnPointMob{}
		if err := rows.Scan(
			&m.ID, &m.SpawnPointID, &m.ActorDefID, &m.Count,
			&m.Name, &m.Race, &m.Class, &m.Level,
			&m.Aggressiveness, &m.AggressiveRange, &m.AttackRange, &m.RespawnDelayMs,
		); err != nil {
			return nil, fmt.Errorf("db: LoadSpawnPointMobs scan: %w", err)
		}
		out = append(out, m)
	}
	return out, rows.Err()
}
