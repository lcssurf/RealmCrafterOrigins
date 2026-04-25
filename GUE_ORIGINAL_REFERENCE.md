# GUE (Grand Unified Editor) — Referência Completa do RC 1.26

Documentação detalhada do editor original do RealmCrafter Standard 1.26 (arquivo fonte `Engine Source/GUE/GUE.bb` + módulos em `Engine Source/GUE/Modules/*.bb`).

Criado por Rob Williams (Solstar Games, 2005–2007). Escrito em **Blitz3D** (`.bb`). Resolução fixa **1024×768** (tenta janela se o desktop for maior; caso contrário fullscreen; flag `-F` força fullscreen).

---

## Índice

- [0. Arquitetura geral](#0-arquitetura-geral)
- [1. Aba Project](#1-aba-project)
- [2. Aba Media](#2-aba-media)
- [3. Aba Particles](#3-aba-particles)
- [4. Aba Combat (Damage Types)](#4-aba-combat-damage-types)
- [5. Aba Projectiles](#5-aba-projectiles)
- [6. Aba Factions](#6-aba-factions)
- [7. Aba Animations](#7-aba-animations)
- [8. Aba Attributes](#8-aba-attributes)
- [9. Aba Actors](#9-aba-actors)
- [10. Aba Items](#10-aba-items)
- [11. Aba Days & Seasons](#11-aba-days--seasons)
- [12. Aba Zones](#12-aba-zones)
- [13. Aba Abilities (Spells)](#13-aba-abilities-spells)
- [14. Aba Interface](#14-aba-interface)
- [15. Aba Other](#15-aba-other)
- [16. Atalhos globais e diálogos](#16-atalhos-globais-e-di%C3%A1logos)
- [17. Arquivos gravados por aba](#17-arquivos-gravados-por-aba)

---

## 0. Arquitetura geral

### 0.1 Inicialização (`GUE.bb` linhas 1–300)

Sequência na inicialização (splash screen `Data\GUE\Loading.PNG` + mensagem em `y=730`):

1. **Graphics mode** — 1024×768. Se desktop > 1024×768 e sem `-F` → windowed mode 2, senão fullscreen.
2. **Media Dialogs** — `InitMediaDialogs()` (ver `Modules/MediaDialogs.bb`).
3. **Interface settings** — `LoadInterfaceSettings("Data\Game Data\Interface.dat")`.
4. **Main screen components** — cria quads para: 40 Attribute displays, Chat area, Chat entry, Buffs area, Radar, Compass.
5. **Inventory components** — 46 slots de inventário com texturas padrão (Weapon/Shield/Hat/Chest/Hand/Belt/Legs/Feet/Ring×4/Amulet×2/Backpack×32).
6. **Emitter configs** — varre `Data\Emitter Configs\*.*` e carrega via `RP_LoadEmitterConfig()`.
7. **Misc / hosts** — lê `Data\Game Data\Misc.dat` (nome do jogo), `Data\Game Data\Hosts.dat` (host do servidor + URL de updates).
8. **Dados do jogo** — carrega sequencialmente:
   - `Data\Server Data\Damage.dat`        → damage types
   - `Data\Server Data\Attributes.dat`    → attributes
   - `Data\Server Data\Factions.dat`      → factions
   - `Data\Game Data\Animations.dat`      → animation sets
   - `Data\Server Data\Projectiles.dat`   → projectiles
   - `Data\Server Data\Items.dat`         → items
   - `Data\Server Data\Actors.dat`        → actors
   - `Data\Server Data\Spells.dat`        → spells
9. **Zonas** — varre `Data\Server Data\Areas\*.*` e chama `ServerLoadArea()` para cada (só lado-servidor; o lado cliente é lazy loaded).
10. **Environment** — `LoadEnvironment()` + `LoadSuns()` (dias/estações/sóis).
11. **Borders** — cria 4 quads posicionados em `z=10` em volta da câmera principal com `Data\GUE\Top.PNG`, `Bottom.PNG`, `Left.PNG`, `Right.PNG` (moldura visual da janela principal).
12. **Main tab strip** — janela `WMain` 974×693 com tab `TabMain` contendo 15 abas (abaixo).
13. **Criação das abas** — cada aba é criada sequencialmente com mensagem no splash.
14. **Skysphere** — carrega `Data\Meshes\Sky Sphere.b3d`, normaliza escala, cria `CloudEN` (cópia para nuvens), `StarsEN`.
15. **Finalização** — `SetZoneDefaults()`, cria uma área padrão (`CurrentArea = ServerCreateArea()`), `UpdateZoneDisplay(1)`. O log é escrito em `GUE Log.txt` pela função `StartLog()`.

### 0.2 Layout geral da tela

```
┌────────────────────────────────────────────────────────────────┐
│  [Project] [Media] [Particles] [Combat] [Projectiles] ...      │ ← TabMain (tab strip)
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                                                          │  │
│  │   Conteúdo da aba selecionada                            │  │
│  │   (controles em posições absolutas x/y em pixels)        │  │
│  │                                                          │  │
│  └──────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────┘
```

Janela principal: `FUI_Window(25, 50, 974, 693, "", "", 0, 0)`. Todas as coordenadas dos controles estão em **pixels absolutos** dentro da aba (aba é `974×693`).

### 0.3 Lista completa das 15 abas principais

| # | Variável (`Global`) | Título visível       | Identificador interno |
|---|---------------------|----------------------|-----------------------|
| 1 | `TProject`          | Project              | build/deploy          |
| 2 | `TMedia`            | Media                | database de assets    |
| 3 | `TParticles`        | Particles            | editor de emitters    |
| 4 | `TDamageTypes`      | Combat               | damage types + combat |
| 5 | `TProjectiles`      | Projectiles          | flechas/balas mágicas |
| 6 | `TFactions`         | Factions             | até 100 facções       |
| 7 | `TAnimSets`         | Animations           | sets de animação      |
| 8 | `TAttributes`       | Attributes           | até 40 atributos      |
| 9 | `TActors`           | Actors               | NPCs / raças playable |
| 10| `TItems`            | Items                | banco de itens        |
| 11| `TSeasons`          | Days & seasons       | calendário / sóis     |
| 12| `TZones`            | Zones                | editor 3D de áreas    |
| 13| `TSpells`           | Abilities            | magias / skills       |
| 14| `TInterface`        | Interface            | layout da UI do jogo  |
| 15| `TOther`            | Other                | configs diversas      |

Troca de aba dispara evento `Case TabMain`. Código fecha entidades específicas da aba anterior e abre as da nova aba (`Case E\EventData` interno).

### 0.4 Sistema de flags "saved"

Variáveis globais booleanas (linha 61–63) controlam estado "modificado":

```
ItemsSaved, ActorsSaved, FactionsSaved, ParticlesSaved, DamageTypesSaved,
ZoneSaved, AnimsSaved, StatsSaved, SpellsSaved, InterfaceSaved,
ProjectilesSaved, EnvironmentSaved

ActorsChanged, AttributesChanged, FactionsChanged, AnimsChanged,
ProjectilesChanged, ParticlesChanged
```

Botões "Save X" setam flag para `True`. Flags `*Changed` obrigam refresh de combos em outras abas quando o usuário muda de aba.

### 0.5 FUI — framework de UI

`F-UI.bb` (24k linhas) é o framework proprietário (baseado em Dear ImGui-style?). Widgets oferecidos:

- `FUI_Window(x, y, w, h, title, ...)` — janela/container
- `FUI_Tab(parent, x, y, w, h)` + `FUI_TabPage(tab, "Name")` — tab strip
- `FUI_Button(parent, x, y, w, h, "Text", [iconPath, [style]])` — botão normal ou toggle (style=1) ou radio-group (style=2)
- `FUI_TextBox(parent, x, y, w, h, [maxLen])` — text input
- `FUI_Label(parent, x, y, "Text", [align])` — label estático
- `FUI_ComboBox(parent, x, y, w, h, [maxVisible])` + `FUI_ComboBoxItem(combo, "Text")` — dropdown
- `FUI_ListBox(parent, x, y, w, h)` + `FUI_ListBoxItem(list, "Text")` — lista vertical
- `FUI_CheckBox(parent, x, y, "Label")` — boolean
- `FUI_Radio(parent, x, y, "Label", initialValue, groupID)` — radio button
- `FUI_Spinner(parent, x, y, w, h, min, max, default, step, DTYPE_INT|FLOAT, [suffix])` — numeric input
- `FUI_Slider(parent, x, y, w, h, min, max, default)` — track bar
- `FUI_GroupBox(parent, x, y, w, h, "Title")` — frame com título
- `FUI_View(parent, x, y, w, h, 0, 0, 0)` — viewport 3D; obtém câmera com `FUI_SendMessage(view, M_GETCAMERA)`
- `FUI_ToolTip(widget, "Texto")` — tooltip
- `FUI_ShortCut("Ctrl", "D")` — testa combinação
- `FUI_CustomMessageBox("msg", "title", MB_OK|MB_YESNO)` — dialog modal

Mensagens (`FUI_SendMessage(widget, MSG, data)`):

- `M_GETINDEX/M_SETINDEX` — item selecionado em combo/list
- `M_GETCAPTION/M_SETCAPTION` — texto visível
- `M_GETTEXT/M_SETTEXT` — conteúdo de textbox
- `M_GETVALUE/M_SETVALUE` — spinner/slider
- `M_GETCHECKED/M_SETCHECKED` — checkbox/radio
- `M_GETDATA/M_SETDATA` — metadata arbitrária (armazena IDs)
- `M_RESET` — limpa combo/list
- `M_DELETEINDEX` — remove item
- `M_GETSELECTED` — true se item selecionado
- `M_GETCAMERA` — para FUI_View

### 0.6 Loop de eventos

```blitz
Repeat
    For E.Event = Each Event
        Select E\EventID
            Case TabMain   ; troca de aba
            Case BZoneSave ; botão de salvar zona
            ...
        End Select
    Next
    ; ... updates específicos da aba ativa (ex: zona atualiza posição da câmera)
    FUI_Update()
    Flip(0)
Until False
```

---

## 1. Aba Project

**Função:** ponto de entrada; construção de instaladores e pacotes de atualização.

### 1.1 Labels

- `"Welcome to Realm Crafter!"` — centralizado em `y=20`.
- `GameName$` — nome do jogo lido de `Misc.dat`, centralizado em `y=60`.

### 1.2 Botões (coluna única em x=10)

| Botão                      | Ação                                                                                        |
|----------------------------|---------------------------------------------------------------------------------------------|
| `BBuildFullInstall` (`y=100`) | `GenerateFullInstall()` — cria instalador completo do cliente |
| `BBuildInstaller` (`y=135`)   | Cria pasta `Game\` e copia apenas o mínimo para rodar: `GameName.exe`, `Game.exe`, `RCEnet.dll`, `Language.txt`, `libbz2w.dll`, `blitzsys.dll`, `rc64.dll`, `rc63.dll`, `QuickCrypt.dll`, `Data\Textures\Menu Logo.bmp`, `Data\Last Username.dat`, `Data\Options.dat`, `Data\Controls.dat`, `Data\Patch.exe`, árvores `Game Data`, `UI`, `Textures\Menu`. Reescreve `Game\Data\Game Data\Misc.dat` para modo "Normal" (não-debug). |
| `BBuildUpdates` (`y=170`)     | Mostra warning "Building game updates may take some time. Please be patient." e chama `GenerateGamePatch()` — gera diffs a partir de `UpdatesList$()` (Areas, Emitter Configs, Game Data, UI, Meshes, Music, Sounds, Textures) para o sistema de auto-update. |
| `BBuildServer` (`y=205`)      | Pergunta "Include dynamic data (e.g. accounts)?" (YES/NO) e "Build a MySQL server?" (YES/NO). Cria pasta `Server\` com: `RCEnet.dll`, `briskvm.dll`, `ggTray.dll`, `Server.exe` **ou** (se MySQL) `MySQL Server.exe + MySQL Configure.exe + libmySQL.dll + SQLDLL.dll + BlitzSQL.dll + MySql.Data.dll + rcsql.sql + rcsql_flat.sql + mini.exe`. Se não quer dynamic data, deleta `Accounts.dat`, `Dropped Items.dat`, `Superglobals.dat`, `Areas\Ownerships\`. |
| `BRestoreLanguageFile` (comentado) | Restaura `Data\Game Data\Language Restore.txt`. |

### 1.3 Comportamento

- Todos os botões mostram `FUI_CustomMessageBox` de sucesso ao completar ("Complete! Required files are in the \Game folder.").
- A aba Project é a única que não tem flag "saved" — toda ação é imediata.

---

## 2. Aba Media

**Função:** banco de assets (meshes, texturas, sons, músicas). Organiza arquivos em subpastas e permite visualização prévia. Outras abas referenciam assets por **ID numérico** em vez de caminho de arquivo.

### 2.1 Controles esquerdos (painel de seleção)

- **`CMediaType`** — `ComboBox(20, 20, 250, 20)` com 4 opções:
  - `"View 3D Meshes"` (índice 1)
  - `"View Textures"` (índice 2)
  - `"View Sounds"` (índice 3)
  - `"View Music"` (índice 4)
  Chama `SetMediaType(index)` ao mudar.
- **`BMediaAdd`** — botão `"Add New File"` (`20, 50, 100, 20`). Abre file dialog Win32 apropriado ao tipo.
- **`BMediaDelete`** — botão `"Remove File"` (`160, 50, 100, 20`). Chama `RemoveMeshFromDatabase(ID)` / `RemoveTextureFromDatabase(ID)` / `RemoveSoundFromDatabase(ID)` / `RemoveMusicFromDatabase(ID)`.
- **`LMediaFolder`** — `ListBox(20, 80, 250, 150)` com subpastas de `Data\Meshes\` ou `Data\Textures\` etc.
- **`LMedia`** — `ListBox(20, 235, 250, 425)` com os arquivos da pasta selecionada. Popula via `FillMeshesList(LMedia, "", MeshDialog_All)` no startup.

### 2.2 Painel de preview (direita)

- **`FUI_GroupBox(290, 20, 660, 640, "Media Preview")`** — moldura do preview.
- **`VMediaPreview`** — `FUI_View(300, 60, 640, 480, 0, 0, 0)` — viewport 3D para mesh/textura.
- **`LMediaPreview`** — label `"Media ID: 0"` (`620, 620`, centralizado).
- **`BMediaPreviewPlay`** — `"Play"` (`310, 50, 60, 20`) para sons/músicas.
- **`BMediaPreviewStop`** — `"Stop"` (`380, 50, 60, 20`).
- **`SMediaPreviewVolume`** — `Slider(360, 80, 350, 20, 0.0, 1.0, 0.75)`.
- **`MediaPreviewQuad`** — quad plano escalado 20×20 em `z=10` usado para exibir texturas 2D.
- **`MediaPreviewMesh`** — mesh 3D ativo (para meshes).

### 2.3 Quando o usuário adiciona arquivo (`BMediaAdd`)

- **Meshes:** chama `AddMeshToDatabase(filename, isAnim)`. Pergunta "Also add subfolders to database?" se for pasta.
- **Texturas:** chama `AddTextureToDatabase(filename, flags)`. Mesma pergunta recursiva.
- **Sons:** chama `AddSoundToDatabase(filename, is3D)`. `is3D` é pedido via checkbox em dialog.
- **Músicas:** chama `AddMusicToDatabase(filename)`.

### 2.4 IDs

Após adicionar, cada asset ganha um **ID inteiro** usado por todas as outras abas. IDs vão até 65535 (meshes) / 65535 (texturas). O label `LMediaPreview` mostra esse ID. `EditorMeshName$(ID)`, `EditorTexName$(ID)` são arrays globais consultados quando outra aba precisa resolver um ID para caminho.

---

## 3. Aba Particles

**Função:** editor completo de emitters de partículas (RottParticles — `Modules/RottParticles.bb` 1411 linhas). Configs ficam em `Data\Emitter Configs\*.dat`.

### 3.1 Viewport de preview

- **`View`** — `FUI_View(20, 20, 600, 450, 0, 0, 0)` — câmera em `ParticlesCam` (`CameraRange 0.5, 800.0`).
- **`LActiveParticles`** — label `"Active particles: N"` em `(20, 475)`.

### 3.2 Barra de gerenciamento (direita)

- **`CParticleConfigs`** — `ComboBox(630, 145, 290, 20, 20)` com a lista de `RP_EmitterConfig`.
- **`BParticlesNew`** — `"New emitter"` (`630, 170, 90, 20`) — cria config nova.
- **`BParticlesSave`** — `"Save emitters"` (`740, 170, 90, 20`).
- **`BParticlesDelete`** — `"Delete emitter"` (`850, 170, 90, 20`).

### 3.3 Controles de preview (cima direita)

- **`BParticlesTex`** — `"Preview texture"` (`650, 20, 100, 20`).
- **`BParticlesPreviewReset`** — `"Reset preview"` (`800, 20, 100, 20`).
- **Navegação da câmera de preview** (arrows):
  - `BParticlesPreviewL` (← `Data\GUE\L.png`)
  - `BParticlesPreviewR` (→ `Data\GUE\R.png`)
  - `BParticlesPreviewU` (↑ `Data\GUE\U.png`)
  - `BParticlesPreviewD` (↓ `Data\GUE\D.png`)
  - `BParticlesPreviewIn` (`Data\GUE\In.png`)
  - `BParticlesPreviewOut` (`Data\GUE\Out.png`)

Variáveis de órbita: `ParticlesPreviewPitch#`, `ParticlesPreviewYaw#`, `ParticlesPreviewDistance# = 20.0`.

### 3.4 Sub-aba "General settings" (`TParticlesGeneral`, 630/200, 330×270)

| Campo                | Tipo     | Range / Default      |
|----------------------|----------|----------------------|
| Maximum particles    | Spinner  | 1–10000 / 200        |
| Spawn rate           | Spinner  | 1–200 / 2            |
| Particle lifespan    | Spinner  | 1–1000 / 100         |
| Initial size         | Spinner  | 0.01–100.0 / 1.0     |
| Size change          | Spinner  | −10.0 a 10.0 / 0.0   |
| Blend mode           | Combo    | Normal / Multiply / Add |
| Initial alpha        | Spinner  | 0.0–1.0 / 1.0        |
| Alpha change         | Spinner  | −1.0 a 1.0 / 0.0     |

### 3.5 Sub-aba "Colouring" (`TParticlesColours`)

- Initial R/G/B (0–255) e R/G/B change (−50.0 a 50.0, step 0.5).

### 3.6 GroupBox "Animated texture options" (10, 495, 170, 170)

- Frames across (1–50), Frames down (1–50), Animation speed (0–100%), `BParticlesRandFrame` (checkbox "Start on random frame").

### 3.7 GroupBox "Shape options" (190, 495, 310, 170)

| Campo          | Controle                                                 |
|----------------|----------------------------------------------------------|
| Emitter shape  | Combo: Sphere / Cylinder / Box                           |
| Cylinder axis  | Combo: X / Y / Z axis                                    |
| Width, Height, Depth | Spinner 0.0–500.0 / 0.0                           |
| Inner radius   | Spinner 0.0–500.0 / 0.0                                  |
| Outer radius   | Spinner 0.1–500.0 / 10.0                                 |

### 3.8 Sub-aba "Velocity" (`TParticlesVelocity`, 510/480, 450×185)

- **Velocity shaping:** None / Shaped / Strictly shaped.
- **X/Y/Z velocity** e **X/Y/Z randomisation** (spinners −500.0 a 500.0, step 0.01).

### 3.9 Sub-aba "Forces" (`TParticlesForce`)

- **Force shaping:** Linear / Spherical.
- **X/Y/Z force** (−500.0 a 500.0, step 0.001).
- **X/Y/Z force modifier** (−500.0 a 500.0, step 0.001).

### 3.10 Comportamento

- Toda mudança em qualquer campo chama `UpdateParticlesPreview()`.
- Ao mudar de aba, `ParticlesChanged = True` obriga refresh de `CProjEmitter1/2` (aba Projectiles) e `LEmitter` (aba Zones).

---

## 4. Aba Combat (Damage Types)

**Função:** define até **20 damage types** (texto livre) + opções globais de combate.

### 4.1 Lista de damage types

- **`BDamageTypesSave`** — `"Save damage types"` (`20, 20, 110, 20`).
- **`TDamageType(0..19)`** — 20 textboxes em coluna (`20, 110 + i*25, 250, 20`). Strings livres (ex: "Slashing", "Fire", "Holy").
- Array `DamageTypes$(0..19)` é a fonte; salvo em `Data\Server Data\Damage.dat`.

### 4.2 GroupBox "Combat Options" (350, 70, 460, 220)

Dados lidos de `Data\Server Data\Misc.dat` (offsets 9+).

| Controle              | Valores                                                  |
|-----------------------|----------------------------------------------------------|
| `SCombatDelay`        | Spinner 0–20000 ms — intervalo entre golpes              |
| `BDamageWeapon`       | Checkbox "Damage weapons when used"                      |
| `BDamageArmour`       | Checkbox "Damage armour when hit"                        |
| `CCombatFormula`      | Combo: Normal / No strength bonus or penalty / High damage, high defence / Use the Attack script (Advanced) |
| `CCombatInfoStyle`    | Combo: None / Chat message / Floating number             |
| `SCombatRatingAdjust` | Spinner 0–5% — penalidade de reputação ao matar          |

---

## 5. Aba Projectiles

**Função:** edita projéteis (flechas, magias lançadas, etc). Arquivo: `Data\Server Data\Projectiles.dat`.

### 5.1 Toolbar

- `BProjNew` "New projectile"
- `BProjCopy` "Copy projectile"
- `BProjDelete` "Delete projectile"
- `BProjSave` "Save projectiles"

### 5.2 Seleção

- **`CProjSelected`** — combo (`120, 60, 330, 20, 10`) com todos os projetéis.
- **`BProjPrev`** "`<<`" e **`BProjNext`** "`>>`" para navegação sequencial.

### 5.3 GroupBox "Projectile properties" (20, 100, 610, 340)

| Campo               | Controle                                                      |
|---------------------|---------------------------------------------------------------|
| Projectile name     | TextBox `TProjName`                                            |
| Projectile mesh     | Label `LProjMesh` + botões `BProjMesh` ("Change") e `BProjMeshN` ("None") |
| Emitter 1           | ComboBox `CProjEmitter1` com configs do `RottParticles`        |
| Emitter 1 texture   | Label `LProjTex1` + `BProjTex1` "Change"                       |
| Emitter 2           | ComboBox `CProjEmitter2`                                       |
| Emitter 2 texture   | `LProjTex2` + `BProjTex2`                                      |
| `BProjHoming`       | Checkbox "Projectile homes in on target"                       |
| Chance to hit       | Spinner 1–100 %                                               |
| Damage              | Spinner 0–5000                                                 |
| Damage type         | Combo com DamageTypes$()                                       |
| Movement speed      | Spinner 1–100 %                                                |

---

## 6. Aba Factions

**Função:** define até **100 facções** e matriz de reputação entre elas. Arquivo: `Data\Server Data\Factions.dat`.

### 6.1 Controles principais

- **`BFactionSave`** — "Save factions".
- **`LFactions`** — ListBox (`20, 100, 250, 310`) com todas as facções.
- **`BFactionAdd`** — "New faction" (`20, 430`).
- **`BFactionDelete`** — "Remove faction" (`130, 430`).

### 6.2 Edição

- **`TFactionName`** — TextBox (`455, 110, 250, 20`, max 30) para renomear.
- **`CFactionRating`** — ComboBox (`510, 160, 250, 20`) — "Adjust this faction's rating with". Seleciona uma facção alvo.
- **`SFactionRating`** — Spinner −100 a 100 % — rating desta facção com a alvo.
- **`LFactionRatingInverse`** — label "That faction's rating with this faction is 0%" (assimétrico!).

Lógica: cada par (A, B) tem duas ratings independentes — rating de A→B pode ser diferente de B→A. Isso permite, por exemplo, "Guards odeiam bandidos (−90%) mas bandidos são neutros com guards (+0%)".

---

## 7. Aba Animations

**Função:** cria **sets de animação** (grupos de animações de um modelo, ex: set "Human Male", com "Idle"/"Walk"/"Attack"/etc). Arquivo: `Data\Game Data\Animations.dat`.

### 7.1 Lista de sets

- **`BAnimsSave`** "Save animation sets".
- **`LAnimSets`** — ListBox 250×310 em `(20, 100)`.
- **`BAnimSetAdd`** "New animation set".
- **`BAnimSetDelete`** "Remove animation set".
- **`BAnimSetCopy`** "Copy animation set".

### 7.2 Edição de set selecionado

- **`TAnimSetName`** — renomear set.
- **`LAnims`** — ListBox (`350, 160, 250, 250`) com animações dentro do set.
- **`BAnimAdd`** "Add animation".
- **`BAnimDelete`** "Remove animation".

### 7.3 Edição de animação selecionada

- **`TAnimName`** — TextBox (200×20, max 30) — nome arbitrário ("Idle", "Run", "Jump"…).
- **`TAnimStart`** — TextBox 90×20 — frame inicial.
- **`TAnimEnd`** — TextBox 90×20 — frame final.
- **`SAnimSpeed`** — Spinner 1–1000 % — velocidade.

Sets referenciados por actors em `CActorMAnim` (male) / `CActorFAnim` (female). Ao salvar, `AnimsChanged = True` invalida combos.

---

## 8. Aba Attributes

**Função:** define até **40 atributos/skills** (ex: Strength, Dexterity, Lockpicking). Arquivo: `Data\Server Data\Attributes.dat`.

### 8.1 Controles

- **`BAttributeSave`** "Save attributes".
- **`BSetFixedAttributes`** "Set fixed attributes" — atalho para marcar HP/Stamina/Mana como fixos.
- **`LAttribute`** — ListBox (`20, 100, 250, 310`) com os atributos.
- **`BAttributeAdd`** "New attribute".
- **`BAttributeDelete`** "Remove attribute".

### 8.2 Edição

- **`TAttributeName`** — TextBox 250×20 (max 20).
- **`BAttributeSkill`** — Checkbox "Attribute is a skill" (treina com uso).
- **`BAttributeHidden`** — Checkbox "Hide attribute from players".

### 8.3 Setup global

- **`SAttributeAssignment`** — Spinner 0–100 — "Assignable attribute points available at character creation".

Quando `AttributesChanged = True`, os listboxes `LItemAttributes` (Items) e `LActorAttributes` (Actors) são refeitos.

---

## 9. Aba Actors

**Função:** define **raças/classes** jogáveis e **NPCs**. Um "Actor" = combinação raça+classe (ex: "Human Warrior", "Orc Merchant"). Arquivo: `Data\Server Data\Actors.dat`.

### 9.1 Toolbar

- `BActorNew` "New actor"
- `BActorCopy` "Copy actor"
- `BActorDelete` "Delete actor"
- `BActorSave` "Save actors"
- `CActorSelected` — combo (`100, 60, 400, 20, 25`) mostra `"Race [Class]"`.
- `BActorPrev`/`BActorNext` navegação.

### 9.2 Sub-aba Description (`TActorsDescription`)

| Campo            | Controle                                                      |
|------------------|---------------------------------------------------------------|
| Actor ID         | Label `LActorID` "Actor ID: N"                                |
| Actor race       | TextBox 200×20 (max 35)                                       |
| Actor class      | TextBox 200×20 (max 35)                                       |
| Actor description| TextBox 600×20 (max 100) — blurb na char creation              |
| Genders          | Combo: Male and female / Male only / Female only / No gender  |
| Home faction     | Combo populado com FactionNames$()                            |

### 9.3 Sub-aba General (`TActorsGeneral`)

| Campo                  | Controle                                                                      |
|------------------------|-------------------------------------------------------------------------------|
| Aggressiveness         | Combo: Passive / Defensive / Always attacks / Non-combatant                   |
| Attack range           | Spinner 0–5000                                                                |
| Trade Mode             | Combo: No trading / Pack animal / Salesman                                    |
| Environment type       | Combo: Normal / Swimming only / Flying / Walking only                         |
| Start area             | Combo com todos `Area\Name$`                                                  |
| Start portal           | TextBox — nome do portal de spawn                                             |
| XP multiplier          | Spinner 1–1000                                                                |
| `BActorPlayable`       | Checkbox — raça selecionável no char creation                                 |
| `BActorRideable`       | Checkbox — pode ser montada                                                   |
| Male animation set     | Combo `CActorMAnim`                                                           |
| Female animation set   | Combo `CActorFAnim`                                                           |
| Male sounds (12 eventos)    | Combo `CActorMSpeech` + botões `BActorMSpeech` (Change) / `BActorMSpeechN` (None). Eventos: Greeting 1/2, Goodbye 1/2, Attack 1/2, Ouch 1/2, Help!, Death, Dry Footstep, Wet Footstep |
| Female sounds          | Igual, mas `CActorFSpeech` / `BActorFSpeech`                                  |
| Inventory slot allowed | Combo com slots (Weapon, Shield, Hat, Chest, Hand, Belt, Legs, Feet, Ring, Amulet, Backpack) + checkbox `BActorSlotAllowed` "Disabled" para banir esse slot |

### 9.4 Sub-aba Appearance (`TActorsAppearance`)

Coluna **Male** (esquerda) e **Female** (direita). Cada "slot" tem um combo de 5 variações ("1st"..."5th") + Label mostrando o mesh/textura atual + botão "Change" + botão "N" (None).

| Linha | Male                            | Female                          |
|-------|---------------------------------|---------------------------------|
| 22    | `LActorMBodyMesh` + `BActorMBodyMesh` | `LActorFBodyMesh` + `BActorFBodyMesh` |
| 52    | Hair (1–5) `CActorMHairMesh`    | Hair (1–5) `CActorFHairMesh`    |
| 82    | Face Texture (1–5)              | Face Texture (1–5)              |
| 112   | Body Texture (1–5)              | Body Texture (1–5)              |
| 142   | Beard (1–5) `CActorBeardMesh`   | —                               |
| 192   | Gubbin (1–6) `CActorGubbinMesh` — attachments gerais | — |
| 222   | `LActorBlood` + `BActorBlood`   | —                               |

Extra:
- **Actor scale** `SActorScale` — Spinner 1–1000 %.
- **`BActorPolyCollision`** — Checkbox "Use polygonal collision (warning: using this on moving actors will disable collisions entirely)".

**Gubbins** são attachments em bones nomeados via aba Other. Cada actor pode ter até 6.

### 9.5 Sub-aba Attributes (`TActorsAttributes`)

Duas colunas:

| Esquerda (Attributes)                            | Direita (Resistances)                             |
|--------------------------------------------------|---------------------------------------------------|
| `LActorAttributes` ListBox 200×450              | `LActorResistances` ListBox 200×450              |
| Attribute value Spinner 0–65535                  | Resistance value Spinner 0–65535                  |
| Attribute maximum Spinner 0–65535                | —                                                 |

Cada linha corresponde a um atributo (até 40) ou tipo de dano (até 20).

### 9.6 Sub-aba Preview (`TActorsPreview`)

- **`VActorPreview`** — `FUI_View(146, 30, 640, 480, 0, 0, 0)` — viewport 3D.
- Instancia `ActorPreview.ActorInstance` e executa animações em loop para inspeção.

### 9.7 Comportamento

- Salvar com `BActorSave` chama `SaveActors()` e atualiza `CSpawnActor` (Zones / waypoints).
- `ActorsChanged = True` refaz as listas de raça/classe para filtros exclusivos nos Items e Spells (via `UpdateRaceClassLists()`).

---

## 10. Aba Items

**Função:** banco de itens. Arquivo: `Data\Server Data\Items.dat`.

### 10.1 Toolbar

- `BItemNew`, `BItemDelete`, `BItemSave`.
- `CItemSelected` combo 400×20 com todos os itens.
- `BItemPrev` / `BItemNext`.

### 10.2 Sub-aba General (`TItemsGeneral`)

| Campo           | Controle                                                                                    |
|-----------------|---------------------------------------------------------------------------------------------|
| Item ID         | Label `LItemID`                                                                             |
| Item name       | TextBox 400×20                                                                              |
| Item type       | Combo: Weapon / Armour / Ring / Potion / Food / Image / Other                               |
| Inventory slot  | Combo `CSlotType` (populado dinamicamente conforme Item type)                              |
| Value           | Spinner 0–10000000                                                                          |
| Mass            | Spinner 0–100000 kg                                                                         |

### 10.3 Sub-aba Specific (`TItemsSpecific`)

**Os campos exibidos dependem do Item type** (os mesmos coordenadas de tela, mas visibilidade alternada):

**Quando Weapon:**
- `SItemWeaponDamage` Spinner 1–5000
- `CItemWeaponType` Combo: One Handed / Two Handed / Ranged
- `CItemDamageType` Combo com DamageTypes$
- Se ranged: `CItemRangedProjectile` (Combo com projetéis), `TItemRangedAnimation` (nome da animação), `SItemRange` Spinner 0.1–200.0

**Quando Armour:** `SItemArmourLevel` Spinner 0–5000.

**Quando Potion/Food:** `SItemEatEffects` Spinner 1–100000 seconds — duração do efeito.

**Quando Image:** `LItemImageID` + `BItemImageID` "Change" — escolhe imagem de display.

**Outros:** `TItemMiscData` TextBox 300×20 — string livre.

### 10.4 Sub-aba Appearance (`TItemsAppearance`)

| Item                     | Controles                                                    |
|--------------------------|--------------------------------------------------------------|
| Thumbnail texture        | `LItemThumb` + `BItemThumb` "Change"                         |
| Mesh (male)              | `LItemMeshM` + `BItemMeshM` "Change" + `BItemMeshMN` "None"  |
| Mesh (female)            | `LItemMeshF` + `BItemMeshF` "Change" + `BItemMeshFN` "None"  |
| Gubbin                   | `CItemGubbin` Combo (Gubbin 1–6) + `BItemGubbin` checkbox "Show this gubbin when item is equipped" |

### 10.5 Sub-aba Attributes (`TItemsAttributes`)

- `LItemAttributes` ListBox 300×450 com os atributos.
- `SItemAttribute` Spinner −5000 a 5000 — bônus/penalidade do atributo ao equipar.

### 10.6 Sub-aba Other (`TItemsOther`)

| Campo                           | Controle                                                       |
|---------------------------------|----------------------------------------------------------------|
| `BItemStackable`                | Checkbox "Item can be stacked"                                 |
| `BItemTakesDamage`              | Checkbox "Item takes damage"                                   |
| Exclusive to race               | Combo `CItemExclusiveRace` ("None" + todas as raças)           |
| Exclusive to class              | Combo `CItemExclusiveClass`                                    |
| Script on right-click           | Combo `CItemScript` (populado de `Data\Server Data\Scripts\*.rsl`) |
| Function to start script in     | Combo `CItemMethod`                                            |

---

## 11. Aba Days & Seasons

**Função:** calendário do jogo + sóis/luas. Arquivo: `Data\Server Data\Environment.dat` + `Data\Server Data\Suns.dat`.

### 11.1 GroupBox "General" (10, 60, 480, 80)

- **`BSeasonSave`** "Save settings".
- **`SYearLength`** Spinner 25–10000 dias.
- **`STimeFactor`** Spinner 1–255 — "Time compression" (multiplicador do tempo real).
- **`SYear`** Spinner −100000 a 1000000 — ano atual.
- **`SDay`** Spinner 1–10000 — dia atual.

### 11.2 GroupBox "Months" (10, 150, 480, 80)

Até **20 meses**.

- **`CMonthNum`** Combo 1st..20th — seleciona mês.
- **`TMonthName`** TextBox 190×20 (max 50) — nome do mês.
- **`SMonthStart`** Spinner 1–10000 dias — duração (dia final).

### 11.3 GroupBox "Seasons" (10, 240, 480, 140)

Até **12 estações**.

- **`CSeasonNum`** Combo 1st..12th.
- **`TSeasonName`** TextBox.
- **`SSeasonStart`** Spinner dias — duração.
- **`SDawn`** Spinner 0–23 — hora do amanhecer.
- **`SDusk`** Spinner 0–23 — hora do anoitecer.

### 11.4 GroupBox "Suns & Moons" (510, 60, 450, 360)

- **`BSunNew`** "New sun", **`BSunDelete`** "Delete sun".
- **`BSunPrev`** "<<", **`BSunNext`** ">>".
- **`LSunNumber`** label "Sun N of M".
- **`LSunTex`** + **`BSunTex`** "Change" — textura do sol.
- **`SSunSize`** Spinner 1–10.
- **`SSunAngle`** Spinner 0–360° — ângulo do trajeto no céu.

**Rise/set por estação:**
- **`CSunSeason`** Combo 1..12 seleciona estação.
- **`LSunSeasonName`** — nome da estação selecionada.
- **`SSunRiseH`** / **`SSunRiseM`** — hora/minuto de nascer.
- **`SSunSetH`** / **`SSunSetM`** — hora/minuto de se pôr.

**Cor da luz:**
- **`SLSunR`**, **`SLSunG`**, **`SLSunB`** Sliders 0–255.
- **`BSunShowFlares`** Checkbox "Show lens flare from this sun".

Múltiplos sóis/luas podem coexistir (cada um com sua cor, tamanho, trajeto, horários por estação).

---

## 12. Aba Zones

**Função:** editor 3D completo de áreas do jogo. **Mais complexa** do GUE (cerca de 900 linhas só de setup + mais mil de event handlers). Arquivos: `Data\Server Data\Areas\<Name>.dat` + sidecars cliente.

### 12.1 Layout

- **Toolbar superior** (y=20): botões de gerenciamento + toggles de modo.
- **Viewport 3D** `VZone` em `(20, 50, 773, 580)` com câmera `ZoneCam`.
- **Painel direito** `(800, 47, 165, 580)` — alterna conforme o modo (são vários GroupBoxes sobrepostos, mostrados/escondidos).
- **Barra inferior** (y=640): transform modes (Select/Move/Rotate/Scale) + Precise + Camera speed + position label.

### 12.2 Toolbar principal (y=20)

| Botão               | Posição | Função                                                                 |
|---------------------|---------|------------------------------------------------------------------------|
| `BZoneNew`          | 20      | Cria nova zona                                                         |
| `BZoneSave`         | 115     | Salva zona ativa                                                       |
| `BZoneCopy`         | 210     | Duplica zona                                                           |
| `BZoneDelete`       | 305     | Deleta zona                                                            |
| `BZoneUndo`         | 410     | Undo — ícone `Data\GUE\L.png`                                          |
| `BZoneScenery`      | 435     | Scenery mode — `Data\GUE\612-home.png`                                 |
| `BZoneTerrain`      | 460     | Terrain mode — `Data\GUE\128-status.png`                               |
| `BZoneEmitters`     | 485     | Emitters mode — `Data\GUE\212-thunderbolt.png`                         |
| `BZoneWater`        | 510     | Water mode — `Data\GUE\608-funnel.png`                                 |
| `BZoneColBox`       | 535     | Collision box mode — `Data\GUE\614-lock.png`                           |
| `BZoneSoundZone`    | 560     | Sound zone mode — `Data\GUE\084-music.png`                             |
| `BZoneTriggers`     | 585     | Trigger mode — `Data\GUE\609-gift.png`                                 |
| `BZoneWaypoints`    | 610     | Waypoint mode — `Data\GUE\409-left_right.png`                          |
| `BZonePortals`      | 635     | Portal mode — `Data\GUE\029-app.png`                                   |
| `BZoneEnviro`       | 660     | Environment options — `Data\GUE\089-star.png`                          |
| `BZoneOther`        | 685     | Other options — `Data\GUE\103-options2.png`                            |
| `CZone`             | 800     | Combo "Current zone"                                                    |

### 12.3 Transform modes (y=640)

- **`BZoneSelect`** — Select (atalho F1). `ZoneMode = 1`.
- **`BZoneMove`** — Move (F2). `ZoneMode = 2`.
- **`BZoneRotate`** — Rotate (F3). `ZoneMode = 3`.
- **`BZoneScale`** — Scale (F4). `ZoneMode = 4`.
- **`BZonePrecise`** — Precise transform (abre dialog com X/Y/Z numéricos).
- **`SCameraSpeed`** — Spinner 1–100 % (padrão 20%). `CamSpeed# = 2.5`.
- **`LZonePos`** — label "X/Y/Z position..." atualizado a cada frame.

### 12.4 Variável `ZoneView`

Inteiro que indica qual painel está ativo (ímpar = placement, par = options):

| Valor | Painel                         |
|-------|--------------------------------|
| 1     | Scenery placement              |
| 2     | Scenery options                |
| 3     | Terrain placement              |
| 4     | Terrain options                |
| 5     | Emitter placement              |
| 6     | Emitter options                |
| 7     | Water placement                |
| 8     | Water options                  |
| 9     | Collision box placement        |
| 10    | Collision box options          |
| 11    | Sound zone placement           |
| 12    | Sound zone options             |
| 13    | Trigger placement              |
| 14    | Trigger options                |
| 15    | Waypoint placement             |
| 16    | Waypoint options               |
| 17    | Portal placement               |
| 18    | Portal options                 |
| 19    | Environment options            |
| 20    | Other options                  |

### 12.5 Painel: Scenery placement (`GScenery`, ZoneView=1)

- **`BSceneryAlign`** — Checkbox "Align to ground" — alinha com `PickedNX/NY/NZ`.
- **`LScenery`** — label centralizado com nome do mesh ativo.
- **`BScenery`** "Change" — abre dialog de mesh. Define `ZoneSceneryMeshID`.

**Colocação:** right-click no 3D com mesh selecionado → cria `Scenery` com `LoadedMeshScales#(ID) * 0.05` de escala, posiciona no ponto do pick (com normal se align ativo). Hold Ctrl → cria à frente da câmera.

### 12.6 Painel: Scenery options (`GSceneryOptions`, ZoneView=2)

| Campo                   | Controle                                                        |
|-------------------------|-----------------------------------------------------------------|
| Animation mode          | Combo: None / Constant loop / Constant ping-pong / When selected |
| Inventory size          | Spinner 0–50 — se >0, scenery vira container com slots          |
| Collision mode          | Combo: None / Sphere / Box / Polygon                            |
| `BSceneryOwnable`       | Checkbox "Scenery can be owned" (housing)                       |
| `LSceneryID`            | Label "Ownership ID: N/A"                                       |
| `BSceneryCatchRain`     | Checkbox "Catch rain/snow particles"                            |
| `BSceneryDuplicate`     | Botão "Duplicate"                                               |
| `BSceneryLocked`        | Checkbox "Lock this object"                                     |

### 12.7 Painel: Terrain placement (`GTerrain`, ZoneView=3)

- **`LTerrain`** — "Current heightmap: None (flat terrain)".
- **`BTerrainHM`** "Change heightmap" — abre file dialog (`.bmp` ou `.png`).
- **`BTerrainFlat`** "Reset".
- Instruções: "Select a heightmap, then right click 3D view to place terrain".

**Colocação:** right-click cria terrain 256×256 (ou menor potência de dois do heightmap) lendo canal R do bitmap. Escala inicial X=1.0, Y=10.0, Z=1.0.

### 12.8 Painel: Terrain options (`GTerrainOptions`, ZoneView=4)

| Campo                    | Controle                                                     |
|--------------------------|--------------------------------------------------------------|
| `BTerrainCM`             | "Change colour map" — textura de cor                         |
| `BTerrainDM`             | "Change detail map" — textura de detalhe (tile)              |
| `STerrainDetailScale`    | Spinner 0.1–100.0 / 15.0                                     |
| `STerrainDetail`         | Spinner 500–50000 — máx. triângulos                          |
| `BTerrainMorph`          | Checkbox "Enable morphing"                                   |
| `BTerrainShade`          | Checkbox "Enable shading"                                    |

### 12.9 Painel: Emitter placement (`GEmitters`, ZoneView=5)

- **`LEmitter`** — ListBox (`5, 10, 155, 540`) com todos `RP_EmitterConfig`.

**Colocação:** right-click cria `Emitter` com cone visual semitransparente (escala 3×3×3) + `RP_CreateEmitter` anexado.

### 12.10 Painel: Emitter options (`GEmitterOptions`, ZoneView=6)

- **`BEmitterTex`** "Change texture".
- (Mais controles abaixo; ver código de `Modules/ServerAreas.bb` para campos completos.)

### 12.11 Painel: Water placement (`GWater`, ZoneView=7)

- Instruções: "Right click 3D view to place a water area".

**Colocação:** cria plano 16×16 subdividido com textura default da database + `W\Blue = 150`, `W\Opacity = 50`.

### 12.12 Painel: Water options (`GWaterOptions`, ZoneView=8)

| Campo              | Controle                                                        |
|--------------------|-----------------------------------------------------------------|
| Red / Green / Blue | Sliders 0–255                                                   |
| `BWaterTex`        | "Change surface texture"                                        |
| Texture scale      | Spinner 0.1–100.0 / 15.0                                        |
| Opacity            | Slider 1–100                                                    |
| Damage per second  | Spinner 0–1000 (ex: lava)                                       |
| Damage type        | Combo com DamageTypes$                                          |

### 12.13 Painel: Collision box placement (ZoneView=9)

Right-click cria cubo 5×2×5 semitransparente vermelho (`EntityAlpha 0.4`) como colisor invisível no jogo.

### 12.14 Painel: Sound zone placement (`GSound`, ZoneView=11)

- **`LSound`** — label do som/música selecionado.
- **`BZoneSound`** "Choose sound" — abre dialog de sons.
- **`BZoneMusic`** "Choose music" — abre dialog de músicas.

**Colocação:** right-click cria esfera amarela (`EntityColor 255, 255, 0`) de raio 15, `Volume = 100`.

### 12.15 Painel: Sound zone options (`GSoundOptions`, ZoneView=12)

| Campo         | Controle                                    |
|---------------|---------------------------------------------|
| `LSoundName`  | Label                                       |
| Repeat time   | Spinner −1 a 1000 (−1 = não repete)         |
| Volume        | Spinner 1–100                               |

### 12.16 Painel: Trigger placement (`GTrigger`, ZoneView=13)

- **`CZoneTriggerScript`** — Combo populado com `Data\Server Data\Scripts\*.rsl`.
- **`CZoneTriggerMethod`** — Combo com funções do script selecionado.

**Colocação:** right-click cria esfera azul (`EntityColor 0, 0, 255`) raio 5. Máximo 150 triggers por área (índice 0–149). Nomeadas `"T" + ID`.

### 12.17 Painel: Trigger options (`GTriggerOptions`, ZoneView=14)

Labels read-only: script, method, trigger ID.

### 12.18 Painel: Waypoint placement (`GWaypoint`, ZoneView=15)

- Instruções: "Right click 3D view to place a waypoint".
- Mesh: `Data\GUE\Waypoint.b3d`. Máximo 2000 waypoints. Nomeados `"W" + ID`.

### 12.19 Painel: Waypoint options (`GWaypointOptions`, ZoneView=16)

| Campo                     | Controle                                                          |
|---------------------------|-------------------------------------------------------------------|
| `BWaypointA`              | "Set next waypoint A" — entra em `ZoneMode=6` (link), click em WP |
| `BWaypointB`              | "Set next waypoint B" — entra em `ZoneMode=7`                     |
| `BWaypointAN`, `BWaypointBN` | "N" — limpa link                                               |
| `SWaypointPause`          | Spinner 0–120 seconds                                             |
| `CSpawnActor`             | Combo com actors (Race [Class]) + "None" — NPC a spawnar          |
| `CSpawnScript`            | Combo — script de spawn                                           |
| `CSpawnActorScript`       | Combo — script de right-click do actor spawnado                   |
| `CSpawnDeathScript`       | Combo — script de morte                                           |
| `SSpawnFrequency`         | Spinner 0–10000 seconds — delay entre spawns                      |
| `SSpawnMax`               | Spinner 1–100 — número máximo spawnado                            |
| `SSpawnRange`             | Spinner 0–10000 — raio de auto-movement                           |

Links A (azul) e B (laranja) são cubos esticados entre waypoints via `PointEntity`. `CurrentArea\NextWaypointA/B[ID]` aponta para o próximo. `CurrentArea\PrevWaypoint[ID]` para o anterior.

Esfera azul translúcida `WPRangeEN` aparece ao redor do waypoint selecionado mostrando o `SpawnSize#`.

### 12.20 Painel: Portal placement (`GPortal`, ZoneView=17)

- **`TPortalName`** TextBox — nome obrigatório (se vazio, exibe erro).
- **`CPortalLinkArea`** Combo — área destino.
- **`CPortalLinkName`** Combo — portal destino na área.

**Colocação:** right-click cria mesh `Data\GUE\Portal.b3d` (ou `.x`) alpha 0.5, raio 5. Máximo 100 portais. Nomeados `"P" + ID`.

### 12.21 Painel: Portal options (`GPortalOptions`, ZoneView=18)

- `LPortalName` — label.
- `CPortalLinkAreaO` / `CPortalLinkNameO` — edita vínculo do portal selecionado.

### 12.22 Painel: Environment options (`GEnviroOptions`, ZoneView=19)

Painel com sub-tab interno `TabEnviroOptions` em `(5, 5, 150, 555)` com 3 páginas:

**Sub-aba "FX" (`TEnviroVisual`):**

| Campo                 | Controle                                                                   |
|-----------------------|----------------------------------------------------------------------------|
| `BOutdoors`           | Checkbox "Zone is outdoors"                                                |
| `BSkyTex`             | "Sky" — textura do céu                                                     |
| `BStarTex`            | "Stars"                                                                    |
| `BCloudTex`           | "Clouds"                                                                   |
| `BStormCloudTex`      | "Stormclouds"                                                              |
| `SLFogNear`           | Slider min fog range                                                       |
| `SLFogFar`            | Slider max fog range (até `MaxFogFar#`)                                    |
| Fog color R/G/B       | Sliders 0–255                                                              |
| Ambient light R/G/B   | Sliders 0–255                                                              |
| Default light pitch   | Spinner −90 a 90                                                           |
| Default light yaw     | Spinner −180 a 180                                                         |

**Sub-aba "Weather" (`TEnviroWeather`):**

| Campo                 | Controle                                                 |
|-----------------------|----------------------------------------------------------|
| Rain %                | Spinner 0–100 (`SWeatherChance(W_Rain-1)`)               |
| Snow %                | Spinner 0–100                                            |
| Fog %                 | Spinner 0–100                                            |
| Storm %               | Spinner 0–100                                            |
| Wind %                | Spinner 0–100                                            |
| `CWeatherLink`        | Combo — sincroniza clima com outra área                  |

**Sub-aba "Other" (`TEnviroOther`):**

- **`SGravity`** Spinner −200 a 800 % (100% = normal).
- **`SSlopeRestrict`** Spinner 0.0–1.0 / 0.6 — limite de inclinação caminhável.

### 12.23 Painel: Other options (`GOtherOptions`, ZoneView=20)

| Campo                | Controle                                                       |
|----------------------|----------------------------------------------------------------|
| `CEntryScript`       | Combo — script ao entrar                                       |
| `CExitScript`        | Combo — script ao sair                                         |
| `BPvP`               | Checkbox "PvP enabled"                                         |
| `LLoadingTex` + `BLoadingTex` / `BLoadingTexN` | Imagem da loading screen                |
| `LLoadingMusic` + `BLoadingMusic` / `BLoadingMusicN` | Música da loading             |
| `LLargeMapTex` + `BLargeMapTex` | Textura do world map                                 |
| `BScaleEntireZone`   | Botão "Scale entire zone" — escala todos os objetos            |

### 12.24 Controles de câmera (3D view)

- **SPACE hold** + mouse → mouselook. `MouseXSpeed() / MouseYSpeed()` + pitch clamp ±89°.
- Enquanto mouselook: **LMB** = move frente (`CamSpeed`), **RMB** = move trás.
- Sem SPACE: mouse é cursor normal.
- **Numpad**: 8=frente, 2=trás, 4=esquerda, 6=direita, 9=cima, 7=baixo, 1=turn left, 3=turn right.
- **Tab** (scan code 15): cicla entre objetos do tipo ativo (Scenery → Scenery, Terrain → Terrain…).
- **Del** (scan code 211): deleta objeto selecionado (`ZoneDeleteEntity`).

### 12.25 Transformações em modo Move / Rotate / Scale

**Move (ZoneMode=2):**
- Setas do teclado ↑↓←→ (`KeyDown 200/208/203/205`): translada em passo `Delta#`.
- A (30) / Z (44): translada Y (±Delta#/2).
- Mouse LMB drag no 3D: re-posiciona no ponto do pick.
- Enter (28): reseta posição para `TFormPoint(0,0,10, ZoneCam, 0)` (10 à frente da câmera).

**Rotate (ZoneMode=3):**
- ←/→: yaw (global).
- ↑/↓: pitch (local).
- A/Z: roll (local).
- Mouse LMB drag: yaw pelo `MouseXSpeed()`.
- Enter: reseta rotação para 0,0,0.

**Scale (ZoneMode=4):**
- ←/→: XScale ×0.99 / ×1.01.
- ↑/↓: ZScale.
- A/Z: YScale.
- Mouse LMB drag: uniform scale = `1.0 + MouseXSpeed()/15.0`.
- Enter: reseta para escala padrão (Terrain→1,10,1; Water→16,_,16; ColBox→5,2,5; SoundZone→15; Trigger→5; Waypoint→5; Portal→5).

### 12.26 Undo system

Type `Undo` com `Action$`, `Info`, `Info2`, `InfoX/Y/Z#`, `WPS.UndoWaypointState`, `ExtraInfo`.

- `CreateUndo(Action, Entity, Info2, X, Y, Z)` — undo genérico.
- `CreateTransformUndo(Action, EN, X, Y, Z)` — para transform.
- **Ctrl+Z** (`FUI_ShortCut("Ctrl", "Z")`) → `PerformUndo()` restaura.
- **Ctrl+D** → `DuplicateSelected()`.

### 12.27 Seleção (ZoneMode=1)

- LMB no 3D: `CameraPick` → se acerta algo → `ZoneSelectEntity(EN)` que:
  - Cria caixa vermelha translúcida `SelectionBoxEN` ao redor.
  - Atualiza painel direito para refletir propriedades do objeto.
  - Se waypoint, atualiza `WPRangeEN` (esfera de range).
- RMB em ZoneView par (options): de-seleciona o objeto e volta para view ímpar (placement) do mesmo tipo.

### 12.28 Arquivos da zona

Salvamento grava `Data\Server Data\Areas\<Name>.dat` (server side) + sidecars `Data\Client Data\Areas\<Name>\*` (client side, cache de meshes/texturas otimizadas para o cliente).

---

## 13. Aba Abilities (Spells)

**Função:** cria magias/habilidades scriptáveis. Arquivo: `Data\Server Data\Spells.dat`.

### 13.1 Toolbar

- `BSpellNew`, `BSpellDelete`, `BSpellSave`.
- `CSpellSelected` combo 400×20 com abilities.
- `BSpellPrev` / `BSpellNext`.

### 13.2 Propriedades

| Campo                          | Controle                                                             |
|--------------------------------|----------------------------------------------------------------------|
| Ability name                   | TextBox `TSpellName` 250×20                                          |
| Ability description            | TextBox `TSpellDesc` 700×20                                          |
| Display icon                   | Label `LSpellImageID` + `BSpellImageID` "Change"                     |
| Recharge time                  | Spinner `SSpellCharge` 0–60 seconds                                  |
| Exclusive to race              | Combo `CSpellExclusiveRace`                                          |
| Exclusive to class             | Combo `CSpellExclusiveClass`                                         |
| Script                         | Combo `CSpellScript` (populado de `Data\Server Data\Scripts\*.rsl`)  |
| Function to start script in    | Combo `CSpellMethod`                                                 |

Scripts BriskVM (linguagem proprietária do RC, `.rsl` = RealmCrafter Script Language) implementam a lógica real. Função default: `Init`.

---

## 14. Aba Interface

**Função:** editor visual do HUD in-game. Arquivo: `Data\Game Data\Interface.dat`.

### 14.1 Layout

- **`VInterface`** — `FUI_View(10, 20, 640, 480, 0, 0, 0)` — preview do HUD.
- **`BInterfaceSave`** "Save interface layout" (`660, 20, 150, 20`).
- **Radio group** (y=90/120):
  - `RInterfaceMain` — "Game screen" (padrão).
  - `RInterfaceInventory` — "Inventory".
- **`LInterfaceComponents`** — ListBox (`800, 70, 150, 430`) com componentes editáveis.

### 14.2 GroupBox "Component settings" (10, 510, 640, 150)

Para o componente selecionado:

| Campo      | Controle                                |
|------------|-----------------------------------------|
| X position | Spinner 0–100 % (step 0.1)              |
| Y position | Spinner 0–100 %                         |
| Width      | Spinner 0–100 %                         |
| Height     | Spinner 0–100 %                         |
| Red        | Slider 0–255                            |
| Green      | Slider 0–255                            |
| Blue       | Slider 0–255                            |
| Alpha      | Slider 0–255                            |

### 14.3 Campo especial: textura do chat

- `LInterfaceChatTex` — label do nome da textura atual.
- `BInterfaceChatTex` "Change" / `BInterfaceChatTexN` "None".

### 14.4 Componentes editáveis

**Game screen:**
- 40 Attribute Displays (HP/EP/Mana bars), Chat area, Chat Entry, Buffs area, Radar, Compass.

**Inventory:**
- Inventory Window background, Drop button, Eat button, Gold display, 46 inventory slots (4 equip + 32 backpack + amulets + rings).

Cada componente é um quad no mundo 3D (`FUI_View` renderiza contra o background do HUD).

---

## 15. Aba Other

**Função:** configurações globais do projeto que não cabem em outras abas. Arquivos: `Data\Game Data\Hosts.dat`, `Data\Server Data\Misc.dat`, `Data\Game Data\Other.dat`, `Data\Game Data\Money.dat`, `Data\Game Data\Gubbins.dat`.

### 15.1 GroupBox "Hosts" (10, 20, 620, 200)

| Campo                               | Controle                                                                |
|-------------------------------------|-------------------------------------------------------------------------|
| URL for auto-update system          | TextBox `TUpdatesHost` 600×20                                           |
| Host name/IP for game server        | TextBox `TServerHost` 600×20 (tooltip: "Tip: Enter localhost...")       |
| Port number for game server         | TextBox `TServerPort` 600×20 (default 25000)                            |
| `BNewAccounts`                      | Checkbox "Allow account creation from game client"                      |
| Maximum characters per account      | Spinner 1–10 (`SMaxAccountChars`)                                       |

### 15.2 GroupBox "Game" (10, 230, 230, 350)

| Campo                      | Controle                                                                   |
|----------------------------|----------------------------------------------------------------------------|
| Initial player money       | Spinner 0–5000 (`SStartGold`)                                              |
| Initial player reputation  | Spinner 0–5000 (`SStartReputation`)                                        |
| `BForcePortals`            | Checkbox "Force portal transfers"                                          |
| Show nametags              | Combo: Always / Never / Only on selected (`CHideNametags`)                 |
| `BDisableCollisions`       | Checkbox "Disable actor -> actor collisions"                               |
| Valid view modes           | Combo: First person / Third person / Both (`CViewMode`)                    |
| `BRequireMemorise`         | Checkbox "Require abilities to be memorised"                               |
| Use chat bubbles           | Combo: Never / With text / Exclusively (`CUseBubbles`)                     |
| Chat bubble text color     | Sliders R/G/B 0–255 (`SBubblesR/G/B`)                                       |

### 15.3 GroupBox "Money" (250, 230, 380, 140)

4 tiers de moeda (ex: copper / silver / gold / platinum).

| Campo              | Controle                                                    |
|--------------------|-------------------------------------------------------------|
| Tier 1 name        | TextBox `TMoney1Name` (max 12)                              |
| Tier 2 name        | TextBox `TMoney2Name` + Spinner `SMoney2x` 2–10000 (conversão ×) |
| Tier 3 name        | TextBox `TMoney3Name` + Spinner `SMoney3x`                  |
| Tier 4 name        | TextBox `TMoney4Name` + Spinner `SMoney4x`                  |

### 15.4 GroupBox "Gubbin remapping" (250, 380, 380, 200)

Os 6 gubbins (attachments de actor) são presos a bones específicos. Este grupo mapeia cada gubbin ao nome do bone.

| Gubbin  | TextBox     | Default                                      |
|---------|-------------|----------------------------------------------|
| Gubbin 1| `TGubbin1`  | lido de `Data\Game Data\Gubbins.dat`         |
| Gubbin 2| `TGubbin2`  | idem                                         |
| Gubbin 3| `TGubbin3`  | idem                                         |
| Gubbin 4| `TGubbin4`  | idem                                         |
| Gubbin 5| `TGubbin5`  | idem                                         |
| Gubbin 6| `TGubbin6`  | idem                                         |

Mudar qualquer campo seta `GubbinNamesChanged = True` e regrava `Gubbins.dat`. Na próxima vez que Actors tab abrir, os previews são refeitos.

---

## 16. Atalhos globais e diálogos

### 16.1 Atalhos de teclado (escopo global do editor)

| Atalho        | Efeito                                                                  |
|---------------|-------------------------------------------------------------------------|
| F1            | Zone mode: Select                                                       |
| F2            | Zone mode: Move                                                         |
| F3            | Zone mode: Rotate                                                       |
| F4            | Zone mode: Scale                                                        |
| Ctrl+Z        | Undo (Zones tab)                                                        |
| Ctrl+D        | Duplicate selected (Zones tab)                                          |
| Del (211)     | Delete selected entity (Zones tab)                                      |
| Tab (15)      | Cycle through objects of current type (Zones tab)                       |
| Space (57)    | Hold para mouselook (Zones tab)                                         |
| Enter (28)    | Reset transform (Zones tab, Move/Rotate/Scale mode)                     |
| Numpad 8/2    | Cam forward/back                                                        |
| Numpad 4/6    | Cam strafe                                                              |
| Numpad 7/9    | Cam down/up                                                             |
| Numpad 1/3    | Cam turn left/right                                                     |
| Arrow keys    | Transform em 1 unidade (modo Move/Rotate/Scale)                         |
| A (30)        | Transform Y up                                                          |
| Z (44)        | Transform Y down                                                        |
| Ctrl (29)     | Ao criar scenery: cria à frente da câmera (skip CameraPick)             |

### 16.2 Media dialogs (MediaDialogs.bb)

Dialogs modais reutilizados em vários lugares para escolher asset. Cada um tem filtro e preview próprio.

- **`MeshDialog`** — escolher mesh. Flags: `MeshDialog_All`, `MeshDialog_Terrain`, etc.
- **`TextureDialog`** — escolher textura.
- **`SoundDialog`** — escolher som.
- **`MusicDialog`** — escolher música.
- **`ScriptDialog`** — escolher script .rsl.

Cada dialog tem:
- ListBox de pastas.
- ListBox de arquivos na pasta.
- Preview.
- Botões OK / Cancel / Add to database.

### 16.3 CustomMessageBox

Modal nativo estilizado. Flags:
- `MB_OK` — só "OK".
- `MB_YESNO` — "Yes" / "No", retorna `IDYES` / `IDNO`.

### 16.4 File dialogs Windows

Usa `GetOpenFileNameW` via DLL. Invocados por `BMediaAdd`, `BTerrainHM`, etc.

---

## 17. Arquivos gravados por aba

| Aba                 | Arquivos gravados                                                              |
|---------------------|--------------------------------------------------------------------------------|
| Project             | `Game\*` (build de cliente) ou `Server\*` (build de servidor)                 |
| Media               | Adiciona/remove registros dos bancos (sem arquivo único; o índice é gerado na hora) |
| Particles           | `Data\Emitter Configs\*.dat`                                                  |
| Combat              | `Data\Server Data\Damage.dat`, `Data\Server Data\Misc.dat` (offsets 9–14), `Data\Game Data\Combat.dat` |
| Projectiles         | `Data\Server Data\Projectiles.dat`                                            |
| Factions            | `Data\Server Data\Factions.dat`                                               |
| Animations          | `Data\Game Data\Animations.dat`                                               |
| Attributes          | `Data\Server Data\Attributes.dat`                                             |
| Actors              | `Data\Server Data\Actors.dat`                                                 |
| Items               | `Data\Server Data\Items.dat`                                                  |
| Days & seasons      | `Data\Server Data\Environment.dat`, `Data\Server Data\Suns.dat`               |
| Zones               | `Data\Server Data\Areas\<Name>.dat` (+ sidecars em `Data\Client Data\Areas\<Name>\`) |
| Abilities           | `Data\Server Data\Spells.dat`                                                 |
| Interface           | `Data\Game Data\Interface.dat`                                                |
| Other               | `Data\Game Data\Hosts.dat`, `Data\Server Data\Misc.dat`, `Data\Game Data\Other.dat`, `Data\Game Data\Money.dat`, `Data\Game Data\Gubbins.dat` |

Todos os arquivos `.dat` são **binários proprietários** com offsets fixos — cada rota de escrita faz `OpenFile` + `SeekFile F, <offset>` + `WriteByte/Short/Int/Float/String`.

---

## Apêndice A — Módulos `.bb` e responsabilidades

| Arquivo                           | Linhas | Responsabilidade                                                       |
|-----------------------------------|--------|------------------------------------------------------------------------|
| `GUE.bb`                          | 10025  | Main: tab strip, event loop, init, handlers de cada aba                |
| `Modules/F-UI.bb`                 | 24473  | Framework de UI completo (FUI_*)                                       |
| `Modules/Media.bb`                | 1054   | Database de meshes/texturas/sons/músicas + IDs                         |
| `Modules/MediaDialogs.bb`         | 725    | Dialogs de seleção de asset                                            |
| `Modules/ClientAreas.bb`          | 1153   | Estruturas Scenery/Terrain/Water/Emitter/ColBox/SoundZone (lado client)|
| `Modules/ServerAreas.bb`          | 467    | Carregamento/salvamento de áreas (server)                              |
| `Modules/Actors.bb`               | 766    | Type `Actor`, load/save de actors                                      |
| `Modules/Actors3D.bb`             | 703    | `ActorInstance`, preview 3D de actors                                  |
| `Modules/Items.bb`                | 399    | Type `Item`, load/save de items                                        |
| `Modules/Inventories.bb`          | 289    | Slots de inventário, Equip/Unequip                                     |
| `Modules/Spells.bb`               | 109    | Type `Spell`, load/save                                                |
| `Modules/Animations.bb`           | 199    | Type `AnimSet` + `Anim`, load/save                                     |
| `Modules/Projectiles.bb`          | 120    | Type `Projectile`, load/save                                           |
| `Modules/Environment.bb`          | 226    | Damage types, factions, attributes, seasons, months, suns             |
| `Modules/Interface.bb`            | 608    | Type `InterfaceComponent`, load/save                                  |
| `Modules/RCTrees.bb`              | 853    | Vegetação procedural (grama/árvores)                                  |
| `Modules/RottParticles.bb`        | 1411   | Engine de partículas                                                  |
| `Modules/RCEnet.bb`               | 110    | Wrapper de `RCEnet.dll` (netcode UDP do RC)                           |
| `Modules/Packets.bb`              | 79     | IDs de packets (compartilhado com client/server)                       |
| `Modules/Language.bb`             | 352    | i18n via `language.xml`                                               |
| `Modules/Logging.bb`              | 76     | `StartLog`, `WriteLog`, `CloseAllLogs`                                |
| `Modules/CharacterEditorLoader.bb`| 130    | Carregamento de char editor preview                                    |

---

## Apêndice B — Peculiaridades e armadilhas conhecidas

- **Texturas com prefixo `m_`** recebem mipmap (flag 1+4). Com `a_` recebem 1+2. Ver `TextureFilter` linha 294.
- **Borders fixos** na viewport da câmera principal em `z=10` (quads Top/Bottom/Left/Right) — é o que dá o visual "emoldurado" do editor.
- **Gadgets em `x-45, y-100`**: o `CameraPick` de zonas usa `MouseX()-45, MouseY()-100` porque o viewport `VZone` começa em `(20, 50)` mas o frame da janela + tab header ocupam ~25 + ~50 pixels. **Se você mover a viewport, tem que ajustar esses offsets.**
- **Gubbins**: são attachments genéricos em bones nomeados via Aba Other. Armor/weapons podem "mostrar" um gubbin quando equipados (aba Items > Appearance > `CItemGubbin` + `BItemGubbin`).
- **View modes codificação estranha**: em `CViewMode` o valor salvo é 1=1st person, 2=Both, 3=3rd person. O combo mostra "First person / Third person / Both" mas o índice 2 do combo = valor 3 no arquivo e vice-versa.
- **Weather link**: uma área pode "herdar" o clima de outra via `CWeatherLink`. Útil para sub-áreas de uma cidade compartilharem tempo.
- **Portal nomes**: servem de vínculo. Portal "EntradaCidade" em Area A pode apontar para portal "SaidaCidade" em Area B. Ambos precisam existir e ter os nomes corretos.
- **Trigger size** mínimo = 5 (resetado pelo Enter em Scale mode). Máximo: 150 triggers / 2000 waypoints / 100 portais por área.
- **"PO code"** (linha 1845): check de trial/licença baseado em mês+ano. Bloqueia o editor se `POYear < 2007 Or POYear > 2008` e chama `PO()`. É o DRM original da Solstar Games.
- **Waypoint IDs** usam sentinela `PrevWaypoint = 2005` para "slot vazio" e `2000` para "sem link" — offset mágico porque só há 2000 waypoints (0–1999).
- **Combat formula "Use the Attack script (Advanced)"** desabilita a fórmula built-in e invoca um script Lua (RSL) por golpe — performance crítica.

---

**Fim do arquivo.** Todas as informações acima vêm de leitura direta de `Engine Source/GUE/GUE.bb` + os módulos `.bb`. O código-fonte é a fonte da verdade; este documento descreve a layout e comportamento sob a ótica do usuário final do editor.
