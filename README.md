# RealmCrafter: Origins

Engine MMORPG moderna construída do zero, usando RealmCrafter Standard 1.26 como referência funcional.

**Stack:** C++20 · OpenGL 4.6 · GLFW · Dear ImGui · Assimp · msquic (cliente) │ Go 1.22 · quic-go · SQLite/PostgreSQL · gopher-lua (servidor)

---

## Layout do projeto

```
RealmCrafterOrigins/
├── client/     # C++ source (src/, CMakeLists.txt)
├── server/     # Go source (cmd/, internal/, go.mod)
├── tools/      # GUE + terrain editor (C++ source)
│
└── dist/                     # TUDO que roda vive aqui
    ├── client/               # ←─ isto é o que o jogador recebe (zip)
    │   ├── rco_client.exe
    │   ├── *.dll             # assimp, glfw, msquic (deployed pelo vcpkg)
    │   ├── shaders/
    │   ├── assets/
    │   └── data/
    │
    ├── server/               # privado — nunca ships
    │   ├── server.exe
    │   ├── config.toml
    │   ├── rco.db            # SQLite — stats, NPCs, spells
    │   └── scripts/          # Lua server logic (secret)
    │
    └── tools/                # dev only — nunca ships
        ├── rco_gue.exe
        └── rco_terrain.exe
```

Cada exe faz `chdir` para o próprio diretório ao iniciar — todos os paths relativos resolvem a partir da pasta de instalação do exe, não da cwd do launcher.

---

## Pré-requisitos

- **[Go 1.22+](https://go.dev/dl/)** — servidor (SQLite embutido pure-Go, zero CGo)
- **[CMake 3.20+](https://cmake.org/download/)** e **MSVC 2022** (C++20)
- **[vcpkg](https://github.com/microsoft/vcpkg)** em `C:/vcpkg/`

```bash
C:/vcpkg/vcpkg install glfw3 glad glm imgui[glfw-binding,opengl3-binding] \
    assimp stb msquic miniaudio sqlite3 --triplet x64-windows
```

---

## Build

Helpers `.bat` no repo root (cada um gera o exe direto em `dist/...`):

```cmd
build-server.bat
build-client.bat
build-gue.bat
build-terrain-editor.bat
```

Ou manual:

```bash
# Server
cd server && go build -o ../dist/server/server.exe ./cmd/server

# Client / GUE / terrain — todos o mesmo padrão
cd client        # ou tools/gue, tools/terrain-editor
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

---

## Rodar

```bash
cd dist/server  && ./server.exe        # terminal 1
cd dist/client  && ./rco_client.exe    # terminal 2
cd dist/tools   && ./rco_gue.exe       # edita dist/server/rco.db
cd dist/tools   && ./rco_terrain.exe   # edita dist/client/data/areas/
```

Saída esperada do server:
```
main: database connected [sqlite] ./rco.db
main: scripting ready — 3 spells registered (from "scripts")
main: spawned 12 NPCs from database
server: listening on 0.0.0.0:7777
```

Para resetar tudo: `del dist\server\rco.db` — é recriado no próximo boot com seeds default.

---

## Config (dist/server/config.toml)

```toml
[server]
listen_addr = "0.0.0.0:7777"
cert_file   = ""   # vazio = TLS self-signed auto
key_file    = ""

[database]
driver = "sqlite"
dsn    = "./rco.db"
# driver = "postgres"
# dsn    = "postgres://rco:rco@localhost:5432/rco?sslmode=disable"

[game]
login_message = "Welcome to RealmCrafter: Origins"
max_players   = 500
```

---

## Distribuir

```bash
7z a rco-client-v0.1.zip dist/client/
```

Nada de `dist/server/` ou `dist/tools/` vai para o jogador — scripts Lua, DB, tools e cert ficam privados.

---

## Testes (servidor)

```bash
cd server && go test ./...
```

---

## Documentação arquitetural

Veja **`CLAUDE.md`** — mapa completo de cada sistema, protocolo, schema do DB, arquitetura do Media System, convenções de path, etc.

---

## Referência RC 1.26

- Código-fonte original: `D:/Github/RealmCrafter-Standard-1.26/Engine Source/`
- Executáveis para observar comportamento: `D:/Github/RealmCrafter-Standard-1.26/(Build) Game files/`

Antes de implementar qualquer sistema, consulte o módulo correspondente no RC (ex: `ServerNet.bb`, `AccountsServer.bp`, `Packets.bb`) e rode o RC original para ver o comportamento esperado.
