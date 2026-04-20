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
func (d *DB) CreateCharacter(ctx context.Context, accountID string, slot int, name, race, className string, gender, faceTex, hair, beard, bodyTex int) (*Character, error) {
	id := uuid.New().String()
	_, err := d.db.ExecContext(ctx,
		d.q(`INSERT INTO characters
		       (id, account_id, slot, name, race, class, gender, face_tex, hair, beard, body_tex, created_at)
		     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`),
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
}

// LoadSpells returns all spell templates.
func (d *DB) LoadSpells(ctx context.Context) ([]SpellRow, error) {
	rows, err := d.db.QueryContext(ctx,
		`SELECT id, name, spell_type, damage_min, damage_max, ep_cost, cooldown_ms, range, icon
		 FROM spell_templates ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []SpellRow
	for rows.Next() {
		var r SpellRow
		if err := rows.Scan(&r.ID, &r.Name, &r.SpellType, &r.DamageMin, &r.DamageMax,
			&r.EPCost, &r.CooldownMs, &r.Range, &r.Icon); err != nil {
			return nil, err
		}
		out = append(out, r)
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
