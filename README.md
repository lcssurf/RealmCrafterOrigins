# RealmCrafter: Origins

RealmCrafter: Origins (RCO) is a modern MMORPG engine project built from scratch, with gameplay and systems inspired by the original RealmCrafter reference behavior.

This repository is structured as a source tree plus a separate runtime output tree (`dist/`), so the code here stays mostly buildable and reviewable in a public environment.

---

## Quick overview

- Client: C++ (OpenGL 4.6), GLFW + GLAD, Dear ImGui, Assimp, STB/miniaudio
- Server: Go 1.22+, QUIC (`quic-go`), gopher-lua scripting
- Data: SQLite for development, PostgreSQL optional for production
- Rendering/Gameplay targets: chunked terrain, skeletal actor rendering, particles, audio, combat, spells, quests, portals, inventory, NPC AI and editor tooling

---

## Repository layout

```text
RealmCrafterOrigins/
  client/      # C++ source (client)
  server/      # Go source (server)
  tools/       # GUE and related editor code
  dist/        # Runtime/output tree (built binaries, shaders, assets, data)
```

### What lives in `dist/`

- `dist/client/` — shipped game client package
- `dist/server/` — server runtime files (kept for local dev/runtime, not shipped to players)
- `dist/tools/` — editor tooling runtime (dev only)

All executables are expected to run with working directory anchored to their own folder, so relative asset/script/db paths resolve from the install location.

---

## Features (implemented)

- Login + character creation/selection
- World movement + network sync
- Inventory and equipped character sheet
- NPC spawn/AI, melee combat, spells, quests, portals
- Lua-based server game logic (damage, events, dialogs, etc.)
- Floating damage numbers / combat FX / particles / audio cues
- GUE editor tabs for items, spells, actors, areas, media and terrain editing

---

## Prerequisites

### Required

- Go 1.22+
- CMake 3.20+
- Visual Studio 2022 (or a compatible MSVC toolchain on Windows)
- vcpkg (used by project builds)

### Suggested packages

```bash
vcpkg install glfw3 glad glm imgui[glfw-binding,opengl3-binding] assimp stb msquic miniaudio sqlite3 --triplet x64-windows
```

---

## Build

### From helper scripts (recommended)

From repo root:

```bash
build-server.bat
build-client.bat
build-gue.bat
```

### Manual build

```bash
# Server (Go)
cd server
go build -o ../dist/server/server.exe ./cmd/server

# Client (C++)
cd ../client
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

# GUE (editor) — when needed
cd ../tools/gue
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

By convention, build outputs are placed in `dist/`.

---

## Run

For local development:

```bash
cd dist/server  && ./server.exe
cd dist/client  && ./rco_client.exe
cd dist/tools   && ./rco_gue.exe   # optional editor
```

The game server will create/initialize runtime DB state as configured.

---

## Configuration

Server configuration lives in `dist/server/config.toml`.

```toml
[server]
listen_addr = "0.0.0.0:7777"
cert_file   = ""
key_file    = ""

[database]
driver = "sqlite"
dsn    = "./rco.db"
# driver = "postgres"
# dsn = "postgres://rco:rco@localhost:5432/rco?sslmode=disable"

[game]
login_message = "Welcome to RealmCrafter: Origins"
max_players   = 500
```

If `cert_file` and `key_file` are empty, a TLS cert is generated automatically for local development.

---

## Packaging

Client-only delivery:

```bash
7z a rco-client-v0.1.zip dist/client/
```

Do not ship `dist/server/` or `dist/tools/` in public gameplay artifacts.

---

## Notes for public repo

- Keep runtime secrets and machine-specific paths out of version control.
- Do not commit generated/local runtime files:
  - DB files and WAL/SHM shards
  - Executables and DLLs
  - Local editor/runtime folders like `.claude/` and `graphify-out/`
- If you have local debug tooling folders, add them to `.gitignore` before push.

---

## Development references

Refer to the repository docs for full protocol, schema and architecture details.

---

## License

Choose a license for the repository (MIT, Apache-2.0, etc.) before a public launch.

