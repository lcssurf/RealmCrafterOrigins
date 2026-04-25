package db

import (
	"context"
	"database/sql"
	"fmt"
	"strings"
	"time"

	"github.com/google/uuid"
	_ "github.com/jackc/pgx/v5/stdlib" // PostgreSQL driver for database/sql
	_ "modernc.org/sqlite"              // SQLite — pure Go, no CGo needed
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
	ItemType     uint8  // 0=weapon 1=armor 2=consumable 3=misc
	SlotType     uint8  // equip slot: 0=weapon 1=shield 2=hat 3=chest 4=hands 5=belt 6=legs 7=feet 8=ring 9=amulet 255=backpack-only
	WeaponDamage int16
	ArmorLevel   int16
	WeaponType   uint8  // 1=one-hand 2=two-hand 3=ranged
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
	ID        string
	AccountID string
	Slot      int
	Name      string
	Race      string
	Class     string
	Gender    int
	Level     int
	XP        int64
	Gold      int64
	AreaName  string
	X, Y, Z  float32
	Yaw       float32
	FaceTex   int
	Hair      int
	Beard     int
	BodyTex   int
	Health    int32
	HealthMax int32
	Energy    int32
	EnergyMax int32
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
		name                         string
		stype, dmin, dmax, ep, cd    int
		rng                          float64
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
		          health, health_max, energy, energy_max
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
func (d *DB) CreateCharacter(ctx context.Context, accountID string, slot int, name, race, className string, gender, faceTex, hair, beard, bodyTex int) (*Character, error) {
	id := uuid.New().String()
	_, err := d.db.ExecContext(ctx,
		d.q(`INSERT INTO characters
		       (id, account_id, slot, name, race, class, gender, face_tex, hair, beard, body_tex, created_at,
		        x, z)
		     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 512, 512)`),
		id, accountID, slot, name, race, className, gender, faceTex, hair, beard, bodyTex, now(),
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
		          health, health_max, energy, energy_max
		     FROM characters WHERE account_id = ? AND slot = ?`),
		accountID, slot,
	).Scan(
		&c.ID, &c.AccountID, &c.Slot, &c.Name, &c.Race, &c.Class, &c.Gender,
		&c.Level, &c.XP, &c.Gold, &c.AreaName, &c.X, &c.Y, &c.Z, &c.Yaw,
		&c.FaceTex, &c.Hair, &c.Beard, &c.BodyTex,
		&c.Health, &c.HealthMax, &c.Energy, &c.EnergyMax,
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
		{"Rusty Sword",       0,   0, 15,  0, 1, 1, 10, 0},
		{"Old Shield",        1,   1,  0,  5, 0, 1, 8,  0},
		{"Leather Tunic",     1,   3,  0,  8, 0, 1, 15, 0},
		{"Leather Hat",       1,   2,  0,  3, 0, 1, 5,  0},
		{"Leather Gloves",    1,   4,  0,  2, 0, 1, 4,  0},
		{"Leather Belt",      1,   5,  0,  2, 0, 1, 3,  0},
		{"Leather Leggings",  1,   6,  0,  5, 0, 1, 10, 0},
		{"Traveller's Boots", 1,   7,  0,  3, 0, 1, 6,  0},
		{"Health Potion",     2, 255,  0,  0, 0, 10, 50, 1},
		{"Iron Ring",         3,   8,  0,  0, 0, 1, 20, 0},
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
	ID              int
	Name            string
	Race            string
	Class           string
	Level           int
	AreaName        string
	X, Y, Z, Yaw   float32
	Aggressiveness  int
	AggressiveRange float32
	AttackRange     float32
	RespawnDelayMs  int64
	ActorDefID      int // FK → media_actor_defs.id (0 = unset)
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
		name                    string
		race, class             string
		level                   int
		area                    string
		x, y, z, yaw            float64
		agg                     int
		aggRange, attackRange   float64
		respawnMs               int64
	}
	// Coords are 0-indexed to match the GUE / client (terrain [0, W*cs]).
	// Starter Zone = 1024×1024 → center is (512, 512); seeds cluster there.
	seeds := []seed{
		// Starter Zone — dialog NPCs (aggressiveness=3)
		{"Guard",     "Human", "Warrior",  5, "Starter Zone",  517, 0, 512,   0, 3,  8, 2, 30000},
		{"Merchant",  "Elf",   "Mage",     3, "Starter Zone",  504, 0, 515, 180, 3,  8, 2, 30000},
		{"Innkeeper", "Dwarf", "Warrior", 10, "Starter Zone",  524, 0, 507, 270, 3,  8, 2, 30000},
		// Starter Zone — combat mobs (aggressiveness=2)
		{"Goblin",       "Beast", "Warrior", 2, "Starter Zone", 527, 0, 520,   0, 2,  8, 2, 30000},
		{"Goblin",       "Beast", "Warrior", 2, "Starter Zone", 532, 0, 506,  90, 2,  8, 2, 30000},
		{"Goblin Scout", "Beast", "Rogue",   3, "Starter Zone", 522, 0, 530, 180, 2,  8, 2, 30000},
		{"Slime",        "Beast", "Beast",   1, "Starter Zone", 497, 0, 522,   0, 2,  8, 2, 30000},
		{"Slime",        "Beast", "Beast",   1, "Starter Zone", 494, 0, 508,   0, 2,  8, 2, 30000},
		// Forest — combat mobs
		{"Wolf",         "Beast", "Beast",  4, "Forest", 520, 0, 524,   0, 2, 10, 2, 30000},
		{"Wolf",         "Beast", "Beast",  4, "Forest", 526, 0, 518,  90, 2, 10, 2, 30000},
		{"Forest Troll", "Beast", "Beast",  8, "Forest", 507, 0, 520,  90, 2, 10, 2, 30000},
		{"Forest Troll", "Beast", "Beast",  8, "Forest", 502, 0, 528, 270, 2, 10, 2, 30000},
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
		        actor_def_id
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
			&s.ActorDefID,
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
}

// ActorDefMesh mirrors one row in media_actor_meshes.
type ActorDefMesh struct {
	ID          int
	ActorDefID  int
	Slot        int
	ModelID     int
	MaterialID  int
}

// ActorDefAnim mirrors one row in media_actor_anims.
type ActorDefAnim struct {
	ID         int
	ActorDefID int
	Action     string
	ClipID     int
}

// ActorDef bundles an actor definition with its meshes + animation map +
// gameplay defaults. Zone placement copies the defaults into npc_spawns.
type ActorDef struct {
	ID       int
	Name     string
	Scale    float32 // multiplies each mesh slot's model scale (1.0 = natural size)

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

	Meshes   []ActorDefMesh
	Anims    []ActorDefAnim
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
		`SELECT name, scale,
		        default_name, default_race, default_class,
		        default_level, default_hp, default_ep,
		        default_aggressiveness, default_aggro_range, default_attack_range,
		        default_respawn_ms,
		        is_playable, is_mountable, is_interactive
		 FROM media_actor_defs WHERE id = ?`), id).Scan(
		&out.Name, &out.Scale,
		&out.DefaultName, &out.DefaultRace, &out.DefaultClass,
		&out.DefaultLevel, &out.DefaultHP, &out.DefaultEP,
		&out.DefaultAggressiveness, &out.DefaultAggroRange, &out.DefaultAttackRange,
		&out.DefaultRespawnMs,
		&playable, &mountable, &interactive,
	)
	if err != nil {
		return nil, nil // missing / not found
	}
	out.IsPlayable    = playable    != 0
	out.IsMountable   = mountable   != 0
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
		d.q(`SELECT id, actor_def_id, action, clip_id
		     FROM media_actor_anims WHERE actor_def_id = ?`), id)
	if err == nil {
		defer arows.Close()
		for arows.Next() {
			var a ActorDefAnim
			if err := arows.Scan(&a.ID, &a.ActorDefID, &a.Action, &a.ClipID); err == nil {
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
		d.q(`SELECT name, source_path, clip_override FROM media_anim_clips WHERE id = ?`), id,
	).Scan(&c.Name, &c.SourcePath, &c.ClipOverride)
	if err != nil {
		return nil, nil
	}
	return c, nil
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
	IsOutdoor  bool
	FogNear    float32
	FogFar     float32
	FogR, FogG, FogB float32 // 0.0–1.0
	AmbientR, AmbientG, AmbientB uint8
	Gravity    float32 // 1.0 = normal

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
	AreaName   string
	X, Z       float32
	Radius     float32
	TargetArea string
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
	}
	for _, m := range migs {
		d.db.ExecContext(ctx, d.q(m))
	}

	rows, err := d.db.QueryContext(ctx, d.q(`
		SELECT name, music_track, fog_density,
		       is_outdoor, pvp_enabled,
		       fog_near, fog_far, fog_r, fog_g, fog_b,
		       ambient_r, ambient_g, ambient_b,
		       gravity, entry_script, exit_script,
		       weather_rain, weather_snow, weather_fog, weather_storm, weather_wind
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
		)
		_ = rows.Scan(
			&c.Name, &track, &c.FogDensity,
			&outdoor, &pvp,
			&c.FogNear, &c.FogFar, &c.FogR, &c.FogG, &c.FogB,
			&ambR, &ambG, &ambB,
			&c.Gravity, &c.EntryScript, &c.ExitScript,
			&wRain, &wSnow, &wFog, &wStr, &wWind,
		)
		c.MusicTrack = uint8(track)
		c.IsOutdoor = outdoor != 0
		c.PvPEnabled = pvp != 0
		c.AmbientR, c.AmbientG, c.AmbientB = uint8(ambR), uint8(ambG), uint8(ambB)
		c.WeatherRain, c.WeatherSnow = uint8(wRain), uint8(wSnow)
		c.WeatherFog, c.WeatherStorm, c.WeatherWind = uint8(wFog), uint8(wStr), uint8(wWind)
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
