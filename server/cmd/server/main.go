package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"syscall"

	"realm-crafter/server/internal/accounts"
	"realm-crafter/server/internal/db"
	rconet "realm-crafter/server/internal/net"
	"realm-crafter/server/internal/scripting"
	"realm-crafter/server/internal/world"

	"github.com/BurntSushi/toml"
)

// ---------------------------------------------------------------------------
// Config structs
// ---------------------------------------------------------------------------

type serverConfig struct {
	ListenAddr string `toml:"listen_addr"`
	CertFile   string `toml:"cert_file"`
	KeyFile    string `toml:"key_file"`
}

type databaseConfig struct {
	Driver string `toml:"driver"`
	DSN    string `toml:"dsn"`
}

type gameConfig struct {
	LoginMessage string `toml:"login_message"`
	MaxPlayers   int    `toml:"max_players"`
}

type config struct {
	Server   serverConfig   `toml:"server"`
	Database databaseConfig `toml:"database"`
	Game     gameConfig     `toml:"game"`
}

// defaultConfig returns sensible defaults for local development.
func defaultConfig() config {
	return config{
		Server: serverConfig{
			ListenAddr: "0.0.0.0:7777",
			CertFile:   "",
			KeyFile:    "",
		},
		Database: databaseConfig{
			Driver: "sqlite",
			DSN:    "./rco.db",
		},
		Game: gameConfig{
			LoginMessage: "Welcome to RealmCrafter: Origins",
			MaxPlayers:   500,
		},
	}
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

func main() {
	// Determine config file path.
	cfgPath := "config.toml"
	if len(os.Args) > 1 {
		cfgPath = os.Args[1]
	}

	cfg := defaultConfig()
	if _, err := toml.DecodeFile(cfgPath, &cfg); err != nil {
		if !os.IsNotExist(err) {
			log.Fatalf("main: load config %q: %v", cfgPath, err)
		}
		log.Printf("main: config file %q not found, using defaults", cfgPath)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Open database.
	database, err := db.Open(ctx, cfg.Database.Driver, cfg.Database.DSN)
	if err != nil {
		log.Fatalf("main: open db: %v", err)
	}
	defer database.Close()
	log.Printf("main: database connected [%s] %s", cfg.Database.Driver, cfg.Database.DSN)

	// Bootstrap services.
	acctService := accounts.New(database)
	gameWorld := world.New()

	// Seed starter item templates (idempotent).
	if err := database.SeedDefaultItems(ctx); err != nil {
		log.Fatalf("main: seed items: %v", err)
	}

	// Create Lua scripting registry and load scripts.
	// Try several paths in order so it works regardless of working directory.
	scriptReg := scripting.New(gameWorld)
	scriptPaths := []string{
		"../scripts/server",   // running from server/ (normal)
		"scripts/server",      // running from repo root
		"../../scripts/server", // running from server/cmd/server/
	}
	var loadedFrom string
	for _, p := range scriptPaths {
		if _, err := os.Stat(p); err == nil {
			loadedFrom = p
			break
		}
	}
	if loadedFrom == "" {
		log.Printf("main: WARNING — scripts directory not found (tried %v), scripting disabled", scriptPaths)
	} else {
		if err := scriptReg.LoadDir(loadedFrom); err != nil {
			log.Printf("main: load scripts from %q: %v", loadedFrom, err)
		}
	}
	log.Printf("main: scripting ready — %d spells registered (from %q)", len(scriptReg.AllSpells()), loadedFrom)

	// Spawn static NPCs in the default Starter Zone.
	starterZone := gameWorld.GetOrCreateArea("Starter Zone")
	for _, npc := range []*world.Actor{
		gameWorld.SpawnNPC(starterZone, "Guard",     "Human", "Warrior", 5,   5.0,  0.0,  0.0,  0.0),
		gameWorld.SpawnNPC(starterZone, "Merchant",  "Elf",   "Mage",    3,  -8.0,  0.0,  3.0, 180.0),
		gameWorld.SpawnNPC(starterZone, "Innkeeper", "Dwarf", "Warrior", 10, 12.0,  0.0, -5.0, 270.0),
	} {
		npc.Aggressiveness = 3 // dialog-only, cannot be attacked
	}

	// Combat mobs in Starter Zone (Aggressiveness=2: aggressive).
	for _, mob := range []*world.Actor{
		gameWorld.SpawnNPC(starterZone, "Goblin",       "Beast", "Warrior", 2,  15.0, 0.0,  8.0,   0.0),
		gameWorld.SpawnNPC(starterZone, "Goblin",       "Beast", "Warrior", 2,  20.0, 0.0, -6.0,  90.0),
		gameWorld.SpawnNPC(starterZone, "Goblin Scout", "Beast", "Rogue",   3,  10.0, 0.0, 18.0, 180.0),
		gameWorld.SpawnNPC(starterZone, "Slime",        "Beast", "Beast",   1, -15.0, 0.0, 10.0,   0.0),
		gameWorld.SpawnNPC(starterZone, "Slime",        "Beast", "Beast",   1, -18.0, 0.0, -4.0,   0.0),
	} {
		mob.Aggressiveness  = 2
		mob.AggressiveRange = 8.0
	}

	// Portals — Starter Zone ↔ Forest.
	gameWorld.AddPortal("Starter Zone", world.Portal{
		X: 25, Z: 25, Radius: 3,
		TargetArea: "Forest",
		DestX: 0, DestY: 0, DestZ: 5, DestYaw: 180,
	})
	gameWorld.AddPortal("Forest", world.Portal{
		X: 0, Z: 0, Radius: 3,
		TargetArea: "Starter Zone",
		DestX: 22, DestY: 0, DestZ: 22, DestYaw: 0,
	})

	// Forest mobs (tougher).
	forest := gameWorld.GetOrCreateArea("Forest")
	for _, mob := range []*world.Actor{
		gameWorld.SpawnNPC(forest, "Wolf",         "Beast", "Beast",   4,   8.0, 0.0, 12.0,   0.0),
		gameWorld.SpawnNPC(forest, "Wolf",         "Beast", "Beast",   4,  14.0, 0.0,  6.0,  90.0),
		gameWorld.SpawnNPC(forest, "Forest Troll", "Beast", "Beast",   8,  -5.0, 0.0,  8.0,  90.0),
		gameWorld.SpawnNPC(forest, "Forest Troll", "Beast", "Beast",   8, -10.0, 0.0, 16.0, 270.0),
	} {
		mob.Aggressiveness  = 2
		mob.AggressiveRange = 10.0
	}

	// Start NPC AI and regen goroutines.
	gameWorld.StartAI(ctx)
	gameWorld.StartRegen(ctx)

	// Create and start QUIC server.
	srv := rconet.NewServer(&rconet.Config{
		ListenAddr: cfg.Server.ListenAddr,
		CertFile:   cfg.Server.CertFile,
		KeyFile:    cfg.Server.KeyFile,
	}, database, acctService, gameWorld, scriptReg)

	// Run server in background goroutine so we can wait on OS signals.
	errCh := make(chan error, 1)
	go func() {
		errCh <- srv.Start(ctx)
	}()

	log.Printf("main: RCO Server started on %s", cfg.Server.ListenAddr)
	log.Printf("main: %s", cfg.Game.LoginMessage)

	// Wait for shutdown signal or server error.
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)

	select {
	case sig := <-sigCh:
		log.Printf("main: received signal %s, shutting down", sig)
		cancel()
		// Wait for server to stop.
		if err := <-errCh; err != nil {
			log.Printf("main: server stopped with error: %v", err)
		}
	case err := <-errCh:
		if err != nil {
			log.Fatalf("main: server error: %v", err)
		}
	}

	log.Println("main: shutdown complete")
}
