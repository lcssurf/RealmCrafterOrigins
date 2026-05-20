package main

import (
	"context"
	"log"
	"math"
	"math/rand"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"

	"realm-crafter/server/internal/accounts"
	"realm-crafter/server/internal/db"
	rconet "realm-crafter/server/internal/net"
	"realm-crafter/server/internal/scripting"
	"realm-crafter/server/internal/world"

	"github.com/BurntSushi/toml"
)

// anchorCwdToExeDir changes the process working directory to the directory
// holding server.exe. All relative paths (config.toml, rco.db, scripts/) then
// resolve from dist/server/ regardless of how the server was launched.
func anchorCwdToExeDir() {
	exe, err := os.Executable()
	if err != nil {
		return
	}
	_ = os.Chdir(filepath.Dir(exe))
}

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

type movementConfig struct {
	MinDeltaSec       float64 `toml:"min_delta_sec"`
	MaxDeltaSec       float64 `toml:"max_delta_sec"`
	BaseStepAllowance float64 `toml:"base_step_allowance"`
	MaxMoveSpeed      float64 `toml:"max_move_speed"`
	SpeedSlackMult    float64 `toml:"speed_slack_mult"`
	MaxBelowGround    float64 `toml:"max_below_ground"`
	MaxAboveGround    float64 `toml:"max_above_ground"`
	EnableTelemetry   bool    `toml:"enable_telemetry"`
	LogRejections     bool    `toml:"log_rejections"`
	TelemetrySampleMs int64   `toml:"telemetry_sample_ms"`
}

type featuresConfig struct {
	CombatAbilityRuntime bool `toml:"combat_ability_runtime"`
}

type config struct {
	Server   serverConfig   `toml:"server"`
	Database databaseConfig `toml:"database"`
	Game     gameConfig     `toml:"game"`
	Movement movementConfig `toml:"movement"`
	Features featuresConfig `toml:"features"`
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
		Movement: movementConfig{
			MinDeltaSec:       0.016,
			MaxDeltaSec:       1.0,
			BaseStepAllowance: 0.75,
			MaxMoveSpeed:      18.0,
			SpeedSlackMult:    1.25,
			MaxBelowGround:    1.0,
			MaxAboveGround:    12.0,
			EnableTelemetry:   false,
			LogRejections:     true,
			TelemetrySampleMs: 500,
		},
		Features: featuresConfig{
			CombatAbilityRuntime: true,
		},
	}
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

func main() {
	// Anchor all relative file lookups to the exe's install dir.
	anchorCwdToExeDir()

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
	world.SetAbilityRuntimeEnabled(cfg.Features.CombatAbilityRuntime)

	// Seed starter item templates (idempotent).
	if err := database.SeedDefaultItems(ctx); err != nil {
		log.Fatalf("main: seed items: %v", err)
	}
	if err := database.SeedDefaultWeaponKits(ctx); err != nil {
		log.Fatalf("main: seed weapon kits: %v", err)
	}
	if err := database.SeedDefaultEquipmentSlotConfig(ctx); err != nil {
		log.Fatalf("main: seed equipment slot config: %v", err)
	}
	if err := database.SeedDefaultSkillProgressionConfig(ctx); err != nil {
		log.Fatalf("main: seed skill progression config: %v", err)
	}
	if err := database.SeedDefaultCharacterProgressionConfig(ctx); err != nil {
		log.Fatalf("main: seed char progression config: %v", err)
	}
	if err := database.SeedDefaultCharacterPrimaryStatsPerLevel(ctx); err != nil {
		log.Fatalf("main: seed char primary stats per level: %v", err)
	}
	if err := database.SeedDefaultKillXPScalingConfig(ctx); err != nil {
		log.Fatalf("main: seed kill xp scaling config: %v", err)
	}
	charProgression, err := database.GetCharacterProgressionConfig(ctx)
	if err != nil {
		log.Fatalf("main: load char progression config: %v", err)
	}
	world.LoadAndCacheCharacterProgressionConfig(world.CharacterProgressionRuntimeConfig{
		MaxLevel:        charProgression.MaxLevel,
		XPCurveType:     charProgression.XPCurveType,
		XPCurveBase:     charProgression.XPCurveBase,
		XPCurveExponent: charProgression.XPCurveExponent,
		XPIrregularity:  charProgression.XPIrregularity,
	})

	// Create Lua scripting registry and load scripts.
	// Canonical path: dist/server/scripts/ (relative to exe, after anchor).
	scriptReg := scripting.New(gameWorld)
	world.SetNPCDecisionHook(scriptReg.DispatchNPCAIDecision)
	const scriptsDir = "scripts"
	if _, err := os.Stat(scriptsDir); err != nil {
		log.Printf("main: WARNING — scripts directory %q not found, scripting disabled", scriptsDir)
	} else if err := scriptReg.LoadDir(scriptsDir); err != nil {
		log.Printf("main: load scripts from %q: %v", scriptsDir, err)
	}
	log.Printf("main: scripting ready — %d spells registered (from %q)", len(scriptReg.AllSpells()), scriptsDir)

	// Overlay AoE config from DB onto in-memory SpellDefs so GUE edits take effect.
	if spellRows, err := database.LoadSpells(ctx); err == nil {
		var aoeRows []scripting.SpellAoERow
		var runtimeRows []scripting.SpellRuntimeAbilityRow
		for _, r := range spellRows {
			aoeRows = append(aoeRows, scripting.SpellAoERow{
				ID:        r.ID,
				AoEType:   r.AoEType,
				AoERadius: r.AoERadius,
			})
			runtimeRows = append(runtimeRows, scripting.SpellRuntimeAbilityRow{
				ID:               r.ID,
				RuntimeAbilityID: r.RuntimeAbilityID,
			})
		}
		scriptReg.PatchAoEFromDB(aoeRows)
		scriptReg.PatchRuntimeAbilityFromDB(runtimeRows)
		log.Printf("main: AoE config patched for %d spells from DB", len(aoeRows))
		log.Printf("main: runtime ability mapping patched for %d spells from DB", len(runtimeRows))
	}

	// Load item templates and register drop tables + shops.
	if err := setupDropsAndShops(ctx, database); err != nil {
		log.Printf("main: drops/shops: %v", err)
	}

	// Load data-driven combat ability runtime definitions.
	if cfg.Features.CombatAbilityRuntime {
		abilityRows, err := database.LoadAbilityTemplates(ctx)
		if err != nil {
			log.Printf("main: load ability_templates: %v", err)
		} else {
			abilities := make([]world.AbilityTemplate, 0, len(abilityRows))
			for _, row := range abilityRows {
				abilities = append(abilities, world.AbilityTemplate{
					ID:                         row.ID,
					Name:                       row.Name,
					Description:                row.Description,
					Family:                     row.Family,
					Category:                   row.Category,
					ResourceType:               row.ResourceType,
					ResourceCost:               row.ResourceCost,
					CooldownMs:                 row.CooldownMs,
					RangeMin:                   row.RangeMin,
					RangeMax:                   row.RangeMax,
					WindupMs:                   row.WindupMs,
					ImpactDelayMs:              row.ImpactDelayMs,
					RecoverMs:                  row.RecoverMs,
					ParryWindowMs:              row.ParryWindowMs,
					Interruptible:              row.Interruptible,
					BaseDamageMin:              row.BaseDamageMin,
					BaseDamageMax:              row.BaseDamageMax,
					DamageStatScaleJSON:        row.DamageStatScaleJSON,
					ArmorPiercePct:             row.ArmorPiercePct,
					CritPolicyJSON:             row.CritPolicyJSON,
					TelegraphType:              row.TelegraphType,
					TelegraphRadius:            row.TelegraphRadius,
					TelegraphColorRGBA:         row.TelegraphColorRGBA,
					ActionWindup:               row.ActionWindup,
					ActionImpact:               row.ActionImpact,
					ActionRecover:              row.ActionRecover,
					AllowActionOverride:        row.AllowActionOverride,
					AllowedActionTagsJSON:      row.AllowedActionTagsJSON,
					VFXIDWindup:                row.VFXIDWindup,
					VFXIDImpact:                row.VFXIDImpact,
					SFXIDWindup:                row.SFXIDWindup,
					SFXIDImpact:                row.SFXIDImpact,
					MasteryXPPerUse:            row.MasteryXPPerUse,
					MasteryMaxLevel:            row.MasteryMaxLevel,
					MasteryXPCurveType:         row.MasteryXPCurveType,
					MasteryXPCurveBase:         row.MasteryXPCurveBase,
					MasteryXPCurveExponent:     row.MasteryXPCurveExponent,
					MasteryXPIrregularity:      row.MasteryXPIrregularity,
					MasteryPrimaryBonusPerLvl:  row.MasteryPrimaryBonusPerLvl,
					MasteryCooldownReduxPerLvl: row.MasteryCooldownReduxPerLvl,
					Enabled:                    row.Enabled,
				})
			}
			world.SetAbilityCatalog(abilities)
			log.Printf("main: loaded %d ability templates", len(abilities))
		}

		loadoutRows, err := database.LoadNPCAbilityLoadouts(ctx)
		if err != nil {
			log.Printf("main: load npc_ability_loadouts: %v", err)
		} else {
			loadouts := make([]world.NPCAbilityLoadoutEntry, 0, len(loadoutRows))
			for _, row := range loadoutRows {
				loadouts = append(loadouts, world.NPCAbilityLoadoutEntry{
					ID:             row.ID,
					NPCSpawnID:     row.NPCSpawnID,
					ActorDefID:     row.ActorDefID,
					AbilityID:      row.AbilityID,
					Priority:       row.Priority,
					Weight:         row.Weight,
					MinDistance:    row.MinDistance,
					MaxDistance:    row.MaxDistance,
					MinTargetHPPct: row.MinTargetHPPct,
					MaxTargetHPPct: row.MaxTargetHPPct,
					PhaseTag:       row.PhaseTag,
					ConditionLua:   row.ConditionLua,
					Enabled:        row.Enabled,
				})
			}
			world.SetNPCAbilityLoadouts(loadouts)
			log.Printf("main: loaded %d npc ability loadout rows", len(loadouts))
		}

		profileRows, err := database.LoadNPCCombatProfiles(ctx)
		if err != nil {
			log.Printf("main: load npc_combat_profiles: %v", err)
		} else {
			profiles := make([]world.NPCCombatProfile, 0, len(profileRows))
			for _, row := range profileRows {
				profiles = append(profiles, world.NPCCombatProfile{
					ID:                     row.ID,
					Name:                   row.Name,
					GlobalGCDMs:            row.GlobalGCDMs,
					DecisionTickMs:         row.DecisionTickMs,
					AggroStyle:             row.AggroStyle,
					AllowChainCast:         row.AllowChainCast,
					MaxConsecutiveSpecials: row.MaxConsecutiveSpecials,
					Enabled:                row.Enabled,
				})
			}
			world.SetNPCCombatProfiles(profiles)
			log.Printf("main: loaded %d npc combat profile rows", len(profiles))
		}

		profileBindingRows, err := database.LoadNPCProfileBindings(ctx)
		if err != nil {
			log.Printf("main: load npc_profile_bindings: %v", err)
		} else {
			bindings := make([]world.NPCProfileBinding, 0, len(profileBindingRows))
			for _, row := range profileBindingRows {
				bindings = append(bindings, world.NPCProfileBinding{
					ID:         row.ID,
					NPCSpawnID: row.NPCSpawnID,
					ActorDefID: row.ActorDefID,
					ProfileID:  row.ProfileID,
					Enabled:    row.Enabled,
				})
			}
			world.SetNPCProfileBindings(bindings)
			log.Printf("main: loaded %d npc profile binding rows", len(bindings))
		}
	} else {
		world.SetAbilityCatalog(nil)
		world.SetNPCAbilityLoadouts(nil)
		world.SetNPCCombatProfiles(nil)
		world.SetNPCProfileBindings(nil)
		log.Printf("main: combat ability runtime disabled via config")
	}

	// Spawn NPCs from the database.
	npcSpawns, err := database.LoadNPCSpawns(ctx)
	if err != nil {
		log.Printf("main: load npc_spawns: %v — no NPCs will be spawned", err)
	}
	for _, s := range npcSpawns {
		area := gameWorld.GetOrCreateArea(s.AreaName)
		npc := gameWorld.SpawnNPC(area, s.Name, s.Race, s.Class, s.Level,
			s.X, s.Y, s.Z, s.Yaw)
		npc.Aggressiveness = s.Aggressiveness
		npc.AggressiveRange = s.AggressiveRange
		npc.AttackRange = s.AttackRange
		npc.RespawnDelay = s.RespawnDelayMs
		npc.SpawnID = s.ID
		npc.ActorDefID = s.ActorDefID
		npc.StartWaypointID = s.StartWaypointID
		npc.WanderRadius = s.WanderRadius
		npc.WanderPauseMinMs = s.WanderPauseMinMs
		npc.WanderPauseMaxMs = s.WanderPauseMaxMs

		// Resolve actor_def_id → full Appearance (meshes + anim bindings).
		if s.ActorDefID > 0 {
			if app := rconet.BuildAppearance(ctx, database, s.ActorDefID); app != nil {
				npc.Appearance = app
			}
		}
	}
	log.Printf("main: spawned %d NPCs from database", len(npcSpawns))

	// Load waypoints and distribute them to their respective areas.
	dbWaypoints, err := database.LoadWaypoints(ctx)
	if err != nil {
		log.Printf("main: load waypoints: %v", err)
	} else {
		for _, dw := range dbWaypoints {
			area := gameWorld.GetOrCreateArea(dw.AreaName)
			wp := &world.Waypoint{
				ID: dw.ID, X: dw.X, Y: dw.Y, Z: dw.Z,
				NextA: dw.NextA, NextB: dw.NextB, PauseMs: dw.PauseMs,
			}
			area.Mu.Lock()
			area.Waypoints[dw.ID] = wp
			area.Mu.Unlock()
		}
		log.Printf("main: loaded %d waypoints", len(dbWaypoints))
	}

	// Load world objects and distribute to areas.
	worldObjs, err := database.LoadWorldObjects(ctx)
	if err != nil {
		log.Printf("main: load world_objects: %v", err)
	} else {
		for _, wo := range worldObjs {
			if wo.ModelPath == "" {
				continue
			}
			area := gameWorld.GetOrCreateArea(wo.AreaName)
			area.Mu.Lock()
			area.Objects = append(area.Objects, world.WorldObject{
				ModelPath: wo.ModelPath,
				Scale:     wo.Scale,
				X:         wo.X, Y: wo.Y, Z: wo.Z,
				Yaw: wo.Yaw,
			})
			area.Mu.Unlock()
		}
		log.Printf("main: loaded %d world objects", len(worldObjs))
	}

	// Spawn NPCs from spawn points (scatter within radius).
	spawnPoints, err := database.LoadSpawnPoints(ctx)
	if err != nil {
		log.Printf("main: load spawn_points: %v", err)
	} else {
		totalFromGroups := 0
		for _, sp := range spawnPoints {
			mobs, mobErr := database.LoadSpawnPointMobs(ctx, sp.ID)
			if mobErr != nil {
				log.Printf("main: load spawn_point_mobs id=%d: %v", sp.ID, mobErr)
				continue
			}
			area := gameWorld.GetOrCreateArea(sp.AreaName)
			for _, m := range mobs {
				for i := 0; i < m.Count; i++ {
					angle := rand.Float64() * 2 * math.Pi
					r := math.Sqrt(rand.Float64()) * sp.Radius
					nx := sp.X + r*math.Cos(angle)
					nz := sp.Z + r*math.Sin(angle)
					npc := gameWorld.SpawnNPC(area, m.Name, m.Race, m.Class, m.Level,
						float32(nx), float32(sp.Y), float32(nz), 0)
					npc.Aggressiveness = m.Aggressiveness
					npc.AggressiveRange = float32(m.AggressiveRange)
					npc.AttackRange = float32(m.AttackRange)
					npc.RespawnDelay = m.RespawnDelayMs
					npc.ActorDefID = m.ActorDefID
					if m.ActorDefID > 0 {
						if app := rconet.BuildAppearance(ctx, database, m.ActorDefID); app != nil {
							npc.Appearance = app
						}
					}
					totalFromGroups++
				}
			}
		}
		log.Printf("main: spawned %d NPCs from %d spawn points", totalFromGroups, len(spawnPoints))
	}

	// Load area configs (music tracks) from DB.
	areaCfgs, err := database.LoadAreaConfigs(ctx)
	if err != nil {
		log.Printf("main: load area_config: %v", err)
	}
	areaMusicMap := make(map[string]uint8)
	areaConfigMap := make(map[string]*db.AreaConfig)
	for _, c := range areaCfgs {
		areaMusicMap[c.Name] = c.MusicTrack
		areaConfigMap[c.Name] = c
	}
	rconet.SetAreaMusicMap(areaMusicMap)
	rconet.SetAreaConfigMap(areaConfigMap)
	log.Printf("main: loaded %d area configs", len(areaCfgs))

	// Load portals from DB; fall back to hardcoded defaults if table is empty.
	areaPortals, err := database.LoadAreaPortals(ctx)
	if err != nil {
		log.Printf("main: load area_portals: %v", err)
	}
	if len(areaPortals) == 0 {
		log.Printf("main: no portals in DB — using built-in defaults")
		// 0-indexed coords matching the GUE / client terrain [0, W*cs].
		gameWorld.AddPortal("Starter Zone", world.Portal{
			X: 537, Z: 537, Radius: 3,
			TargetArea: "Forest",
			DestX:      512, DestY: 0, DestZ: 517, DestYaw: 180,
		})
		gameWorld.AddPortal("Forest", world.Portal{
			X: 512, Z: 512, Radius: 3,
			TargetArea: "Starter Zone",
			DestX:      534, DestY: 0, DestZ: 534, DestYaw: 0,
		})
	} else {
		for _, p := range areaPortals {
			gameWorld.AddPortal(p.AreaName, world.Portal{
				X: p.X, Z: p.Z, Radius: p.Radius,
				TargetArea: p.TargetArea,
				DestX:      p.DestX, DestY: p.DestY, DestZ: p.DestZ, DestYaw: p.DestYaw,
			})
		}
		log.Printf("main: loaded %d portals from database", len(areaPortals))
	}

	// Load heightmaps so NPCs follow terrain Y when moving.
	gameWorld.LoadHeightmaps("../client/data/areas")

	// Start NPC AI and regen goroutines.
	gameWorld.StartAI(ctx)
	gameWorld.StartRegen(ctx)

	// Create and start QUIC server.
	srv := rconet.NewServer(&rconet.Config{
		ListenAddr: cfg.Server.ListenAddr,
		CertFile:   cfg.Server.CertFile,
		KeyFile:    cfg.Server.KeyFile,
		Movement: rconet.MovementValidationConfig{
			MinDeltaSec:       cfg.Movement.MinDeltaSec,
			MaxDeltaSec:       cfg.Movement.MaxDeltaSec,
			BaseStepAllowance: cfg.Movement.BaseStepAllowance,
			MaxMoveSpeed:      cfg.Movement.MaxMoveSpeed,
			SpeedSlackMult:    cfg.Movement.SpeedSlackMult,
			MaxBelowGround:    cfg.Movement.MaxBelowGround,
			MaxAboveGround:    cfg.Movement.MaxAboveGround,
			EnableTelemetry:   cfg.Movement.EnableTelemetry,
			LogRejections:     cfg.Movement.LogRejections,
			TelemetrySampleMs: cfg.Movement.TelemetrySampleMs,
		},
	}, database, acctService, gameWorld, scriptReg)
	scriptReg.SetQuestBridge(srv)

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

// setupDropsAndShops loads all item templates from the DB and registers
// NPC drop tables and shop inventories.
func setupDropsAndShops(ctx context.Context, database *db.DB) error {
	templates, err := database.LoadAllItemTemplates(ctx)
	if err != nil {
		return err
	}

	entry := func(name string, chance float32, minQ, maxQ uint8) world.DropEntry {
		t := templates[name]
		if t == nil {
			log.Printf("main: drop entry %q not found in item_templates", name)
			return world.DropEntry{}
		}
		return world.DropEntry{
			ItemID:       uint16(t.ID),
			Name:         t.Name,
			ItemType:     t.ItemType,
			SlotType:     t.SlotType,
			WeaponDamage: t.WeaponDamage,
			ArmorLevel:   t.ArmorLevel,
			ItemValue:    t.ItemValue,
			Chance:       chance,
			MinQty:       minQ,
			MaxQty:       maxQ,
		}
	}

	shopItem := func(name string, buyPrice int32) world.ShopItem {
		t := templates[name]
		if t == nil {
			log.Printf("main: shop item %q not found in item_templates", name)
			return world.ShopItem{}
		}
		return world.ShopItem{
			ItemID:       uint16(t.ID),
			Name:         t.Name,
			ItemType:     t.ItemType,
			SlotType:     t.SlotType,
			WeaponDamage: t.WeaponDamage,
			ArmorLevel:   t.ArmorLevel,
			BuyPrice:     buyPrice,
			SellPrice:    buyPrice / 2,
		}
	}

	// Drop tables
	world.RegisterDropTable("Goblin", []world.DropEntry{
		entry("Health Potion", 0.65, 1, 2),
		entry("Rusty Sword", 0.12, 1, 1),
		entry("Iron Ring", 0.05, 1, 1),
	})
	world.RegisterDropTable("Goblin Scout", []world.DropEntry{
		entry("Health Potion", 0.55, 1, 1),
		entry("Leather Hat", 0.10, 1, 1),
	})
	world.RegisterDropTable("Slime", []world.DropEntry{
		entry("Health Potion", 0.40, 1, 1),
	})
	world.RegisterDropTable("Wolf", []world.DropEntry{
		entry("Health Potion", 0.50, 1, 2),
		entry("Leather Gloves", 0.10, 1, 1),
	})
	world.RegisterDropTable("Forest Troll", []world.DropEntry{
		entry("Health Potion", 0.80, 1, 3),
		entry("Leather Tunic", 0.15, 1, 1),
		entry("Old Shield", 0.10, 1, 1),
		entry("Iron Ring", 0.08, 1, 1),
	})

	// Shops
	world.RegisterShop("Merchant", []world.ShopItem{
		shopItem("Rusty Sword", 25),
		shopItem("Old Shield", 20),
		shopItem("Leather Hat", 12),
		shopItem("Leather Tunic", 30),
		shopItem("Leather Gloves", 10),
		shopItem("Leather Belt", 8),
		shopItem("Leather Leggings", 22),
		shopItem("Traveller's Boots", 14),
		shopItem("Health Potion", 15),
		shopItem("Iron Ring", 40),
	})
	world.RegisterShop("Innkeeper", []world.ShopItem{
		shopItem("Health Potion", 12),
	})

	log.Printf("main: drop tables and shops registered")
	return nil
}
