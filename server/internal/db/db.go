package db

import (
	"context"
	"database/sql"
	"fmt"
	"strings"
	"time"

	"github.com/google/uuid"
	_ "github.com/jackc/pgx/v5/stdlib" // PostgreSQL driver for database/sql
	_ "modernc.org/sqlite"             // SQLite — pure Go, no CGo needed
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
	WeaponType   uint8 // 1=one-hand 2=two-hand 3=ranged
	MaxStack     uint8
	ItemValue    int32
	Stackable    bool
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
}

// Character mirrors the characters table.
type Character struct {
	ID         string
	AccountID  string
	Slot       int
	Name       string
	Race       string
	Class      string
	Gender     int
	Level      int
	XP         int64
	Gold       int64
	AreaName   string
	X, Y, Z    float32
	Yaw        float32
	FaceTex    int
	Hair       int
	Beard      int
	BodyTex    int
	Health     int32
	HealthMax  int32
	Energy     int32
	EnergyMax  int32
	ActorDefID int
}

// PlayableDef is a minimal view of media_actor_defs used for the character-
// creation dropdown.
type PlayableDef struct {
	ID           int
	Name         string
	DefaultRace  string
	DefaultClass string
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
//	driver = "sqlite"   → dsn is a file path, e.g. "./rco.db"
//	driver = "postgres" → dsn is a postgres:// URL
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

	return d, nil
}

// Close releases the underlying connection pool.
func (d *DB) Close() {
	_ = d.db.Close()
}

// ---------------------------------------------------------------------------
// Placeholder translation
// q() converts ? placeholders to $1, $2, … for PostgreSQL.
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
		          health, health_max, energy, energy_max, actor_def_id
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
			&c.Health, &c.HealthMax, &c.Energy, &c.EnergyMax, &c.ActorDefID,
		); err != nil {
			return nil, fmt.Errorf("db: ListCharacters scan: %w", err)
		}
		chars = append(chars, c)
	}
	return chars, rows.Err()
}

// CreateCharacter inserts a new character into the specified slot.
// Initial x/z put the character at the center of the default Starter Zone
// (1024×1024 world units → center is 512). Column defaults can't be relied
// on across SQLite/Postgres for this — so we set them explicitly.
func (d *DB) CreateCharacter(ctx context.Context, accountID string, slot int, name, race, className string, gender, faceTex, hair, beard, bodyTex, actorDefID int) (*Character, error) {
	id := uuid.New().String()
	_, err := d.db.ExecContext(ctx,
		d.q(`INSERT INTO characters
		       (id, account_id, slot, name, race, class, gender, face_tex, hair, beard, body_tex, actor_def_id, created_at,
		        x, z)
		     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 512, 512)`),
		id, accountID, slot, name, race, className, gender, faceTex, hair, beard, bodyTex, actorDefID, now(),
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
		          health, health_max, energy, energy_max, actor_def_id
		     FROM characters WHERE account_id = ? AND slot = ?`),
		accountID, slot,
	).Scan(
		&c.ID, &c.AccountID, &c.Slot, &c.Name, &c.Race, &c.Class, &c.Gender,
		&c.Level, &c.XP, &c.Gold, &c.AreaName, &c.X, &c.Y, &c.Z, &c.Yaw,
		&c.FaceTex, &c.Hair, &c.Beard, &c.BodyTex,
		&c.Health, &c.HealthMax, &c.Energy, &c.EnergyMax, &c.ActorDefID,
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
		name         string
		itemType     int
		slotType     int
		weaponDamage int
		armorLevel   int
		weaponType   int
		maxStack     int
		itemValue    int
		stackable    int
	}{
		{"Rusty Sword", 0, 0, 15, 0, 1, 1, 10, 0},
		{"Old Shield", 1, 1, 0, 5, 0, 1, 8, 0},
		{"Leather Tunic", 1, 3, 0, 8, 0, 1, 15, 0},
		{"Leather Hat", 1, 2, 0, 3, 0, 1, 5, 0},
		{"Leather Gloves", 1, 4, 0, 2, 0, 1, 4, 0},
		{"Leather Belt", 1, 5, 0, 2, 0, 1, 3, 0},
		{"Leather Leggings", 1, 6, 0, 5, 0, 1, 10, 0},
		{"Traveller's Boots", 1, 7, 0, 3, 0, 1, 6, 0},
		{"Health Potion", 2, 255, 0, 0, 0, 10, 50, 1},
		{"Iron Ring", 3, 8, 0, 0, 0, 1, 20, 0},
	}
	for _, it := range items {
		_, err := d.db.ExecContext(ctx,
			d.q(`INSERT INTO item_templates
			       (name, item_type, slot_type, weapon_damage, armor_level, weapon_type, max_stack, item_value, stackable)
			     SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?
			     WHERE NOT EXISTS (SELECT 1 FROM item_templates WHERE name = ?)`),
			it.name, it.itemType, it.slotType, it.weaponDamage, it.armorLevel,
			it.weaponType, it.maxStack, it.itemValue, it.stackable, it.name,
		)
		if err != nil {
			return fmt.Errorf("db: SeedDefaultItems %q: %w", it.name, err)
		}
	}
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
			continue // item template not seeded yet — skip
		}
		insertSQL := `INSERT OR IGNORE INTO character_items (character_id, slot, item_id, quantity, durability) VALUES (?, ?, ?, ?, 100)`
		if d.driver == "postgres" {
			insertSQL = `INSERT INTO character_items (character_id, slot, item_id, quantity, durability) VALUES ($1, $2, $3, $4, 100) ON CONFLICT DO NOTHING`
		}
		_, _ = d.db.ExecContext(ctx, insertSQL, charID, s.slot, itemID, s.qty)
	}
	return nil
}

// GetEquippedStats returns the weapon damage of the highest-damage weapon
// and the sum of armor levels from all equipment slots (0-13).
func (d *DB) GetEquippedStats(ctx context.Context, charID string) (weaponDamage, armorLevel int32, err error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT it.weapon_damage, it.armor_level
		     FROM character_items ci
		     JOIN item_templates it ON it.id = ci.item_id
		     WHERE ci.character_id = ? AND ci.slot < 14`),
		charID,
	)
	if err != nil {
		return 0, 0, fmt.Errorf("db: GetEquippedStats: %w", err)
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
	return weaponDamage, armorLevel, rows.Err()
}

// GetInventory returns all items for a character.
func (d *DB) GetInventory(ctx context.Context, charID string) ([]*CharacterItem, error) {
	rows, err := d.db.QueryContext(ctx,
		d.q(`SELECT ci.slot, ci.item_id, ci.quantity, ci.durability,
		          it.name, it.item_type, it.slot_type, it.weapon_damage, it.armor_level
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

// SaveXP persists updated XP, level, and (on level-up) new stat maxima.
func (d *DB) SaveXP(ctx context.Context, charID string, xp int64, level int, hpMax, epMax int32) error {
	_, err := d.db.ExecContext(ctx,
		d.q(`UPDATE characters SET xp = ?, level = ?, health_max = ?, energy_max = ? WHERE id = ?`),
		xp, level, hpMax, epMax, charID)
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
	case res.ItemType == 2: // consumable — reduce stack
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

	case slotType < 10 && slot >= 14: // equippable from bag — auto-equip
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
	ID         uint16
	Name       string
	SpellType  int
	DamageMin  int32
	DamageMax  int32
	EPCost     int32
	CooldownMs int64
	Range      float32
	Icon       uint8
	AoEType    uint8   // 0=single 1=around_target 2=ground_target
	AoERadius  float32 // world units; 0 = not AoE
}

// LoadSpells returns all spell templates.
func (d *DB) LoadSpells(ctx context.Context) ([]SpellRow, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, name, spell_type, damage_min, damage_max, ep_cost, cooldown_ms, range, icon, aoe_type, aoe_radius
		 FROM spell_templates ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []SpellRow
	for rows.Next() {
		var r SpellRow
		if err := rows.Scan(&r.ID, &r.Name, &r.SpellType, &r.DamageMin, &r.DamageMax,
			&r.EPCost, &r.CooldownMs, &r.Range, &r.Icon, &r.AoEType, &r.AoERadius); err != nil {
			return nil, err
		}
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
	ActorDefID       int // FK → media_actor_defs.id (0 = unset)
	StartWaypointID  int // FK → area_waypoints.id (0 = no patrol)
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
	// Starter Zone = 1024×1024 → center is (512, 512); seeds cluster there.
	seeds := []seed{
		// Starter Zone — dialog NPCs (aggressiveness=3)
		{"Guard", "Human", "Warrior", 5, "Starter Zone", 517, 0, 512, 0, 3, 8, 2, 30000},
		{"Merchant", "Elf", "Mage", 3, "Starter Zone", 504, 0, 515, 180, 3, 8, 2, 30000},
		{"Innkeeper", "Dwarf", "Warrior", 10, "Starter Zone", 524, 0, 507, 270, 3, 8, 2, 30000},
		// Starter Zone — combat mobs (aggressiveness=2)
		{"Goblin", "Beast", "Warrior", 2, "Starter Zone", 527, 0, 520, 0, 2, 8, 2, 30000},
		{"Goblin", "Beast", "Warrior", 2, "Starter Zone", 532, 0, 506, 90, 2, 8, 2, 30000},
		{"Goblin Scout", "Beast", "Rogue", 3, "Starter Zone", 522, 0, 530, 180, 2, 8, 2, 30000},
		{"Slime", "Beast", "Beast", 1, "Starter Zone", 497, 0, 522, 0, 2, 8, 2, 30000},
		{"Slime", "Beast", "Beast", 1, "Starter Zone", 494, 0, 508, 0, 2, 8, 2, 30000},
		// Forest — combat mobs
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
// them to resolve a spawn's ActorDefID → model_path + material_paths + animation
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
	// a flat "k1=v1;k2=v2" string keyed by the model's ai-material name → the
	// media_materials.name to use. Mirrors the GUE-side migration.
	if d.driver == "postgres" {
		exec(`ALTER TABLE media_models ADD COLUMN IF NOT EXISTS material_map TEXT NOT NULL DEFAULT ''`)
	} else {
		exec(`ALTER TABLE media_models ADD COLUMN material_map TEXT NOT NULL DEFAULT ''`)
	}

	// Normal map intensity per terrain material — compensates for whiteout triplanar
	// blend softening on top-facing surfaces. 2.5 matches the shader's previous global default.
	if d.driver == "postgres" {
		exec(`ALTER TABLE media_materials ADD COLUMN IF NOT EXISTS normal_strength REAL NOT NULL DEFAULT 2.5`)
	} else {
		exec(`ALTER TABLE media_materials ADD COLUMN normal_strength REAL NOT NULL DEFAULT 2.5`)
	}

	// migrateV8 — extend media_actor_defs with gameplay defaults so an actor
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

// migrateV9 — coordinate-system alignment: previously the client terrain
// was centered on the origin while the GUE stored positions in [0, W*cs].
// Shift rows that still use the old "centered on origin" values into the
// new 0-indexed convention (Starter Zone is 1024×1024 → center = 512,512).
// Heuristic: positions inside the small ±100 window around the old origin
// are assumed legacy and get bumped. Positions already in the new range
// (hundreds of units) are left alone.
// Safe to run multiple times: once a row's been shifted it falls outside
// the window and no further migration is applied.
func (d *DB) migrateV9(ctx context.Context) {
	exec := func(sql string) { _, _ = d.db.ExecContext(ctx, sql) }
	// Characters still at (0, 0, 0) — the old map center — land at the
	// new center of Starter Zone so they don't spawn at the corner.
	exec(`UPDATE characters SET x = 512, z = 512
	      WHERE x = 0 AND z = 0 AND area_name = 'Starter Zone'`)
	// Seed NPCs in Starter Zone (coords around old origin) → shift by the
	// old map half-extent.
	exec(`UPDATE npc_spawns SET x = x + 512, z = z + 512
	      WHERE area_name = 'Starter Zone'
	        AND x BETWEEN -100 AND 100 AND z BETWEEN -100 AND 100`)
	exec(`UPDATE npc_spawns SET x = x + 512, z = z + 512
	      WHERE area_name = 'Forest'
	        AND x BETWEEN -100 AND 100 AND z BETWEEN -100 AND 100`)
}

// migrateV10 — adds actor_def_id to characters so player appearance is driven
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

// migrateV12 — adds yaw_offset and y_offset to media_actor_defs so GUE authors
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
// Media registry — read-only accessors used by the server to resolve an
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

	// Gameplay defaults — applied to freshly placed npc_spawns so users
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
		        default_respawn_ms,
		        is_playable, is_mountable, is_interactive
		 FROM media_actor_defs WHERE id = ?`), id).Scan(
		&out.Name, &out.Scale, &out.YawOffset, &out.YOffset,
		&out.DefaultName, &out.DefaultRace, &out.DefaultClass,
		&out.DefaultLevel, &out.DefaultHP, &out.DefaultEP,
		&out.DefaultAggressiveness, &out.DefaultAggroRange, &out.DefaultAttackRange,
		&out.DefaultRespawnMs,
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
// material_map (aiMaterial → media material name) into concrete PBR paths.
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
		          it.name, it.item_type, it.slot_type, it.weapon_damage, it.armor_level, it.item_value
		     FROM character_items ci
		     JOIN item_templates it ON it.id = ci.item_id
		     WHERE ci.character_id = ? AND ci.slot = ?`),
		charID, slot,
	).Scan(&ci.Slot, &ci.ItemID, &ci.Quantity, &ci.Durability,
		&ci.Name, &ci.ItemType, &ci.SlotType, &ci.WeaponDamage, &ci.ArmorLevel, &ci.ItemValue)
	if err != nil {
		return nil, fmt.Errorf("db: GetItemAtSlot: %w", err)
	}
	return ci, nil
}

// LoadAllItemTemplates returns all item templates keyed by name.
func (d *DB) LoadAllItemTemplates(ctx context.Context) (map[string]*ItemTemplate, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, name, item_type, slot_type, weapon_damage, armor_level, weapon_type, max_stack, item_value, stackable
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
			&t.ArmorLevel, &t.WeaponType, &t.MaxStack, &t.ItemValue, &stackable); err != nil {
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
		`SELECT id, name, item_type, slot_type, weapon_damage, armor_level, weapon_type, max_stack, item_value, stackable
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
			&t.ArmorLevel, &t.WeaponType, &t.MaxStack, &t.ItemValue, &stackable); err != nil {
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
		`INSERT INTO item_templates (name, item_type, slot_type, weapon_damage, armor_level, weapon_type, max_stack, item_value, stackable)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		t.Name, t.ItemType, t.SlotType, t.WeaponDamage, t.ArmorLevel, t.WeaponType, t.MaxStack, t.ItemValue, stackable)
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
		 SET name=?, item_type=?, slot_type=?, weapon_damage=?, armor_level=?, weapon_type=?, max_stack=?, item_value=?, stackable=?
		 WHERE id=?`,
		t.Name, t.ItemType, t.SlotType, t.WeaponDamage, t.ArmorLevel, t.WeaponType, t.MaxStack, t.ItemValue, stackable, t.ID)
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
	_, err := d.db.ExecContext(ctx, `DELETE FROM item_templates WHERE id = ?`, id)
	if err != nil {
		return fmt.Errorf("db: DeleteItemTemplate: %w", err)
	}
	return nil
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
	FogR, FogG, FogB             float32 // 0.0–1.0
	AmbientR, AmbientG, AmbientB uint8
	Gravity                      float32 // 1.0 = normal

	// Gameplay
	PvPEnabled  bool
	EntryScript string
	ExitScript  string

	// Weather probabilities (0–100 %)
	WeatherRain  uint8
	WeatherSnow  uint8
	WeatherFog   uint8
	WeatherStorm uint8
	WeatherWind  uint8

	// Skybox — filename relative to assets/ibl/ (e.g. "forest.hdr"). Empty = use default.hdr.
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

	// Migrate: add new columns if they don't exist yet (errors are ignored — column may already exist).
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
// Spawn Points — authored in GUE, loaded read-only by the server at startup.
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
