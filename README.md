# RealmCrafter: Origins

Engine MMORPG moderna construída do zero, usando RealmCrafter Standard 1.26 como referência funcional.

**Stack:** C++20 · OpenGL 4.6 · GLFW · Dear ImGui · msquic (cliente) | Go 1.22 · QUIC · SQLite/PostgreSQL · Lua 5.4 (servidor)

---

## Phase 1 — "Enter the World" (implementado)

| Componente | Detalhes |
|---|---|
| **Protocolo binário** | 53 IDs do RC 1.26 + extensões RCO (100–105), framing `[u16 type][u32 len][payload]` |
| **Servidor QUIC** | TLS self-signed gerado automaticamente, porta 7777 |
| **Accounts** | Registro, login, bcrypt, controle de sessão online |
| **Characters** | 9 slots por conta (igual RC), criar/deletar, posição persistida ao desconectar |
| **World** | Zones, broadcast de posição, runtime IDs únicos por sessão |
| **Banco de dados** | SQLite embutido (dev, zero instalação) ou PostgreSQL (produção) |
| **Cliente — janela** | OpenGL 4.6 Core, céu azul, Dear ImGui |
| **Cliente — rede** | Conexão QUIC via msquic, codec binário little-endian |
| **Cliente — login** | Tela com abas Login / Criar Conta |
| **Cliente — personagens** | Grade de 9 slots, criar (nome/raça/classe/gênero), deletar com confirmação |
| **Cliente — mundo** | Enter world, HUD com área/HP/EP/posição |

**Phase 2 (próximo):** terrain renderer, skybox CSM, actor 3D via Assimp, câmera third-person.

---

## Banco de dados — SQLite ou PostgreSQL

Assim como o RC 1.26 funcionava sem MySQL (arquivos `.dat` locais), o RCO usa **SQLite por padrão** — sem instalar nada. Para produção, troque para PostgreSQL em dois campos do `config.toml`.

| Modo | Quando usar |
|---|---|
| `driver = "sqlite"` + `dsn = "./rco.db"` | Desenvolvimento / testes — **padrão** |
| `driver = "postgres"` + `dsn = "postgres://..."` | Produção, múltiplos servidores |

O arquivo `rco.db` é criado automaticamente e está no `.gitignore`.

---

## Pré-requisitos

### Servidor
- **[Go 1.22+](https://go.dev/dl/)** — único requisito. SQLite é embutido (pure Go, sem CGo).

### Cliente
- **[CMake 3.20+](https://cmake.org/download/)**
- **[MSVC 2022](https://visualstudio.microsoft.com/)** com suporte a C++20 (ou GCC 13+)
- **[vcpkg](https://github.com/microsoft/vcpkg)** — gerencia as dependências C++

---

## 1 · Servidor

```bash
cd server

# Primeira vez: baixa dependências (inclui SQLite pure Go)
go mod tidy

# Compilar
go build -o server.exe ./cmd/server

# Rodar — cria rco.db automaticamente
./server.exe
```

**Saída esperada:**
```
main: database connected [sqlite] ./rco.db
server: using auto-generated self-signed TLS certificate
server: listening on 0.0.0.0:7777
main: RCO Server started on 0.0.0.0:7777
main: Welcome to RealmCrafter: Origins
```

Para resetar tudo: `del server\rco.db` (recria na próxima vez que rodar).

---

## 2 · config.toml

```toml
[server]
listen_addr = "0.0.0.0:7777"
cert_file   = ""   # vazio = TLS self-signed gerado automaticamente
key_file    = ""

[database]
driver = "sqlite"       # "sqlite" (padrão) ou "postgres"
dsn    = "./rco.db"     # SQLite: caminho do arquivo

# Para PostgreSQL:
# driver = "postgres"
# dsn    = "postgres://rco:rco@localhost:5432/rco?sslmode=disable"

[game]
login_message = "Welcome to RealmCrafter: Origins"
max_players   = 500
```

### Configurar PostgreSQL (opcional)

```sql
-- No psql:
CREATE USER rco WITH PASSWORD 'rco';
CREATE DATABASE rco OWNER rco;
```

Troque o `config.toml` para `driver = "postgres"`. As tabelas são criadas automaticamente.

---

## 3 · Cliente C++

### 3.1 · Dependências via vcpkg

```bash
# Instalar vcpkg (se ainda não tiver):
git clone https://github.com/microsoft/vcpkg C:/vcpkg
C:/vcpkg/bootstrap-vcpkg.bat

# Dependências da Phase 1:
C:/vcpkg/vcpkg install glfw3 glad glm imgui[glfw-binding,opengl3-binding] msquic --triplet x64-windows
```

> `assimp` (modelos 3D) só será necessário na Phase 2. O build da Phase 1 funciona sem ele.

### 3.2 · Compilar

```bash
cd client

cmake -B build ^
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

### 3.3 · Rodar

```bash
build\Release\rco_client.exe
```

---

## 4 · Fluxo de teste completo

Com o servidor rodando:

1. Abrir o cliente → tela de login
2. Aba **"Create Account"** → usuário / senha / email → **Register**
3. Aba **"Login"** → entrar com as mesmas credenciais
4. Tela de personagens → clicar em **[Empty]** no slot 0
5. Preencher nome (mín. 3 chars) + raça + classe + gênero → **Create**
6. Clicar no personagem → **Enter World**
7. HUD: área `Starter Zone`, HP/EP, posição `0 0 0`

---

## 5 · Testes unitários (servidor)

```bash
cd server
go test ./...
```

---

## 6 · Estrutura do repositório

```
RealmCrafterOrigins/
├── client/                     # C++ client + rendering engine
│   ├── src/
│   │   ├── core/               # Janela GLFW, loop principal (main.cpp)
│   │   ├── net/                # Conexão QUIC (msquic), codec binário
│   │   └── ui/                 # Telas ImGui: login, char select, HUD
│   └── CMakeLists.txt          # C++20, vcpkg, dependências Phase 1
│
├── server/                     # Go authoritative game server
│   ├── cmd/server/main.go      # Boot, config TOML, graceful shutdown
│   ├── config.toml             # SQLite por padrão
│   ├── rco.db                  # Banco SQLite (gerado em runtime, gitignored)
│   ├── go.mod
│   └── internal/
│       ├── accounts/           # Registro, login, bcrypt, sessão online
│       ├── db/                 # database/sql — suporta SQLite e PostgreSQL
│       ├── net/                # QUIC listener, state machine por cliente
│       ├── protocol/           # IDs dos packets (espelho do RC Packets.bb)
│       └── world/              # Zones, actors, broadcast de posição
│
├── shared/
│   └── protocol/               # Fonte da verdade: packets.go + packets.h
│
├── scripts/server/             # Lua 5.4 — game logic (Phase 3)
└── tools/                      # GUE + terrain editor (Phase 4)
```

---

## 7 · Roadmap

| Phase | Objetivo | Status |
|---|---|---|
| **1 — Enter the World** | Login, char select, entrar no mundo | ✅ Completo |
| **2 — See the World** | Terrain chunks, skybox, actor 3D, câmera | 🔜 Próximo |
| **3 — Interact** | NPCs, Lua scripting, inventário, combate básico | ⏳ |
| **4 — Content** | GUE, terrain editor, spells, quests, portais | ⏳ |

---

## Referência RC 1.26

Código-fonte original: `D:/Github/RealmCrafter-Standard-1.26/Engine Source/`  
Executáveis para observar comportamento: `D:/Github/RealmCrafter-Standard-1.26/(Build) Game files/`

Antes de implementar qualquer sistema, consulte o módulo correspondente no RC (ex: `ServerNet.bb`, `AccountsServer.bp`, `Packets.bb`) e rode o RC original para ver o comportamento esperado.
