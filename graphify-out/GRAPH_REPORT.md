# Graph Report - D:/Github/RealmCrafterOrigins  (2026-05-03)

## Corpus Check
- 142 files · ~162,871 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1111 nodes · 1835 edges · 48 communities detected
- Extraction: 93% EXTRACTED · 7% INFERRED · 0% AMBIGUOUS · INFERRED: 126 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Community Hubs (Navigation)
- [[_COMMUNITY_Server Database Layer|Server Database Layer]]
- [[_COMMUNITY_Net Client & Packet Handling|Net Client & Packet Handling]]
- [[_COMMUNITY_Window & GL Context|Window & GL Context]]
- [[_COMMUNITY_World Systems & Scripting|World Systems & Scripting]]
- [[_COMMUNITY_GUE Media Registry|GUE Media Registry]]
- [[_COMMUNITY_GUE Zones Editor|GUE Zones Editor]]
- [[_COMMUNITY_Terrain Editing & Rendering|Terrain Editing & Rendering]]
- [[_COMMUNITY_Renderer Pipeline & Lighting|Renderer Pipeline & Lighting]]
- [[_COMMUNITY_External Dependencies|External Dependencies]]
- [[_COMMUNITY_Lua Scripting API|Lua Scripting API]]
- [[_COMMUNITY_DB Schema Structs|DB Schema Structs]]
- [[_COMMUNITY_Zone Renderer (GUE)|Zone Renderer (GUE)]]
- [[_COMMUNITY_Model & Mesh System|Model & Mesh System]]
- [[_COMMUNITY_Shader Compilation|Shader Compilation]]
- [[_COMMUNITY_Server Net & Config|Server Net & Config]]
- [[_COMMUNITY_Actor & Model Cache|Actor & Model Cache]]
- [[_COMMUNITY_GUE Areas Tab|GUE Areas Tab]]
- [[_COMMUNITY_Engine Init & Shaders|Engine Init & Shaders]]
- [[_COMMUNITY_Material System|Material System]]
- [[_COMMUNITY_Input System|Input System]]
- [[_COMMUNITY_Module 20|Module 20]]
- [[_COMMUNITY_Module 21|Module 21]]
- [[_COMMUNITY_Module 22|Module 22]]
- [[_COMMUNITY_Module 23|Module 23]]
- [[_COMMUNITY_Module 24|Module 24]]
- [[_COMMUNITY_Module 25|Module 25]]
- [[_COMMUNITY_Module 26|Module 26]]
- [[_COMMUNITY_Module 27|Module 27]]
- [[_COMMUNITY_Module 28|Module 28]]
- [[_COMMUNITY_Module 29|Module 29]]
- [[_COMMUNITY_Module 30|Module 30]]
- [[_COMMUNITY_Module 31|Module 31]]
- [[_COMMUNITY_Module 32|Module 32]]
- [[_COMMUNITY_Module 33|Module 33]]
- [[_COMMUNITY_Module 34|Module 34]]
- [[_COMMUNITY_Module 35|Module 35]]
- [[_COMMUNITY_Module 36|Module 36]]
- [[_COMMUNITY_Module 37|Module 37]]
- [[_COMMUNITY_Module 38|Module 38]]
- [[_COMMUNITY_Module 39|Module 39]]
- [[_COMMUNITY_Module 40|Module 40]]
- [[_COMMUNITY_Module 41|Module 41]]
- [[_COMMUNITY_Module 42|Module 42]]
- [[_COMMUNITY_Module 43|Module 43]]
- [[_COMMUNITY_Module 44|Module 44]]
- [[_COMMUNITY_Module 46|Module 46]]
- [[_COMMUNITY_Module 48|Module 48]]
- [[_COMMUNITY_Module 87|Module 87]]

## God Nodes (most connected - your core abstractions)
1. `DB` - 66 edges
2. `ClientConn` - 49 edges
3. `gladLoadGLLoader()` - 26 edges
4. `rco_renderer Static Library` - 24 edges
5. `Area` - 21 edges
6. `Registry` - 20 edges
7. `Open()` - 19 edges
8. `DrawViewport()` - 19 edges
9. `buildFramedPacket()` - 18 edges
10. `DrawActorDefs()` - 18 edges

## Surprising Connections (you probably didn't know these)
- `Window()` --calls--> `gladLoadGLLoader()`  [INFERRED]
  client/src/core/window.cpp → shared/renderer/src/glad.c
- `SetMaterialSlot()` --calls--> `LoadTex()`  [INFERRED]
  tools/gue/src/terrain/editable_terrain.cpp → shared/renderer/src/model.cpp
- `gue()` --calls--> `View()`  [INFERRED]
  tools/gue/src/zone_camera.h → client/src/renderer/camera.cpp
- `EnsureDefaultTextures()` --calls--> `MakeSolidTex()`  [INFERRED]
  tools/gue/src/terrain/editable_terrain.cpp → client/src/renderer/terrain/terrain.cpp
- `ReloadMaterials()` --calls--> `MakeSolidTex()`  [INFERRED]
  tools/gue/src/terrain/editable_terrain.cpp → client/src/renderer/terrain/terrain.cpp

## Hyperedges (group relationships)
- **rco_renderer shared by both rco_client and rco_gue** — target_rco_client, target_rco_gue, target_rco_renderer [EXTRACTED 1.00]
- **rco_client vcpkg dependencies** — target_rco_client, dep_glfw3, dep_glm, dep_imgui, dep_assimp, dep_stb, dep_msquic [EXTRACTED 1.00]
- **rco_gue vcpkg dependencies** — target_rco_gue, dep_glfw3, dep_glm, dep_imgui, dep_sqlite3, dep_stb [EXTRACTED 1.00]

## Communities (116 total, 8 thin omitted)

### Community 0 - "Server Database Layer"
Cohesion: 0.06
Nodes (6): DB, Open(), slotTypeMatches(), EnsureTables(), Exec(), LoadFromDB()

### Community 1 - "Net Client & Packet Handling"
Cohesion: 0.09
Nodes (12): buildFramedPacket(), isClosedErr(), musicForArea(), resultMessage(), ClientConn, NewReader(), ReadPacket(), NewActor() (+4 more)

### Community 2 - "Window & GL Context"
Cohesion: 0.06
Nodes (41): Window(), Allocate(), Clear(), DynamicBuffer(), Free(), FreeOldest(), maybeMerge(), stateChanged() (+33 more)

### Community 3 - "World Systems & Scripting"
Cohesion: 0.07
Nodes (31): now(), Area, broadcastNPCPosition(), endChase(), leashNPC(), moveNPCToward(), pickWanderTarget(), postChaseMode() (+23 more)

### Community 4 - "GUE Media Registry"
Cohesion: 0.11
Nodes (45): ActorSlotName(), ClassifyAsset(), colText(), ComboId(), DeleteActorDef(), DeleteAnimClip(), DeleteAnimEvent(), DeleteAnimMap() (+37 more)

### Community 5 - "GUE Zones Editor"
Cohesion: 0.12
Nodes (43): DeleteSelected(), Draw(), DrawFloatingToolbar(), DrawInspector(), DrawPanelColBox(), DrawPanelEmitters(), DrawPanelEnviro(), DrawPanelNPC() (+35 more)

### Community 6 - "Terrain Editing & Rendering"
Cohesion: 0.1
Nodes (31): ApplyBrush(), BuildChunk(), ClearMaterialSlot(), DestroyChunks(), DestroyMaterials(), ~EditableTerrain(), EnsureDefaultTextures(), GenerateMacro() (+23 more)

### Community 7 - "Renderer Pipeline & Lighting"
Cohesion: 0.08
Nodes (22): blurTextureBase(), blurTextureR32f(), blurTextureRG32f(), blurTextureRGBA16f(), blurTextureRGBA32f(), drawFSTexture(), MakeLightMatrix(), compositePass_() (+14 more)

### Community 8 - "External Dependencies"
Cohesion: 0.07
Nodes (35): assimp (vcpkg), GLAD (vendored, bindless, GL 4.6 + GL_ARB_bindless_texture), glfw3 (vcpkg), glm (vcpkg), imgui (vcpkg), miniaudio (header-only), msquic (vcpkg/raw lib), opengl32 (Windows GL) (+27 more)

### Community 9 - "Lua Scripting API"
Cohesion: 0.09
Nodes (10): luaIntField(), luaNumField(), luaStrField(), callCtx, DialogPending, Registry, nowMs(), SpellTypeIndex() (+2 more)

### Community 10 - "DB Schema Structs"
Cohesion: 0.07
Nodes (27): Account, ActorDef, ActorDefAnim, ActorDefMesh, AreaConfig, AreaPortal, AreaSoundZone, AreaTrigger (+19 more)

### Community 11 - "Zone Renderer (GUE)"
Cohesion: 0.15
Nodes (24): BuildFBO(), BuildPrimitiveVAOs(), CompileShader(), DestroyFBO(), DrawBox(), DrawCircleXZ(), DrawForwardOverlays_(), DrawLine() (+16 more)

### Community 12 - "Model & Mesh System"
Cohesion: 0.14
Nodes (23): ConsolidateMeshes(), AppendAnimationsFrom(), ApplyMaterialsByName(), BuildNodeTree(), ComputeBones(), Destroy(), EnsureDefaults(), FreeMesh() (+15 more)

### Community 13 - "Shader Compilation"
Cohesion: 0.14
Nodes (25): renderer(), checkLinkStatus(), compileShader(), initUniforms(), loadFile(), loc(), resolveIncludes(), Set1FloatArray() (+17 more)

### Community 14 - "Server Net & Config"
Cohesion: 0.1
Nodes (19): BuildAppearance(), SetAreaConfigMap(), SetAreaMusicMap(), Config, Server, generateSelfSignedCert(), NewServer(), config (+11 more)

### Community 15 - "Actor & Model Cache"
Cohesion: 0.17
Nodes (12): renderer(), ComposeTRS(), DecomposeTRS(), EnsureBoneSSBOs_(), FindClip(), Init(), PlayAnim(), SubmitAs() (+4 more)

### Community 16 - "GUE Areas Tab"
Cohesion: 0.22
Nodes (17): DeleteArea(), DeletePortal(), DeleteWaypoint(), DeleteWorldObject(), Draw(), DrawAreaFields(), DrawPortalFields(), DrawWaypointFields() (+9 more)

### Community 17 - "Engine Init & Shaders"
Cohesion: 0.17
Nodes (13): CompileAllShaders(), createFramebuffers_(), createVAO_(), destroyFramebuffers_(), EndStaticScene(), ~Engine(), Init(), installDebugCallback_() (+5 more)

### Community 18 - "Material System"
Cohesion: 0.15
Nodes (7): MaterialManager(), appendName_(), GetLinearBindless(), GetLinearMaterials(), GetMaterial(), MakeResidentHandle_(), RegisterFromHandles()

### Community 19 - "Input System"
Cohesion: 0.2
Nodes (10): Dispatch(), jsonEscape(), jsonGetString(), LoadBindings(), LoadLocalOverrides(), OnKeyDown(), OnKeyUp(), RebuildLookupTables() (+2 more)

### Community 20 - "Module 20"
Cohesion: 0.19
Nodes (3): NewArea(), World, New()

### Community 21 - "Module 21"
Cohesion: 0.46
Nodes (5): AppearanceBytes(), appendAnimBindings(), NewActorPayload(), WorldObjectsPayload(), pb

### Community 22 - "Module 22"
Cohesion: 0.29
Nodes (12): BuildORM(), CopyFileTo(), DetectRole(), ExtractPrefix(), FlipNormalGChannel(), IFindIn(), ImportTextureGroup(), IsImageExt() (+4 more)

### Community 23 - "Module 23"
Cohesion: 0.32
Nodes (9): canEquipInSlot(), drawEquipSlot(), drawEquipSlotContent(), drawSilhouette(), drawTooltip(), pushSlotColors(), Render(), RenderBag() (+1 more)

### Community 24 - "Module 24"
Cohesion: 0.17
Nodes (6): Actor, AiMaterial, AnimBinding, AnimEvent, Appearance, MeshSlot

### Community 25 - "Module 25"
Cohesion: 0.38
Nodes (11): BuildActionSuggestions(), colStr(), DeleteBinding(), DeletePreset(), Draw(), DrawKeyCapture(), EnsureTables(), FetchAll() (+3 more)

### Community 26 - "Module 26"
Cohesion: 0.27
Nodes (7): Connection(), Disconnect(), OnData(), ProcessFrames(), SendPacket(), SendRaw(), StreamCallback()

### Community 29 - "Module 29"
Cohesion: 0.2
Nodes (3): Get(), SampleWorld(), UploadRegion()

### Community 30 - "Module 30"
Cohesion: 0.31
Nodes (5): FindChar(), Render(), RenderCharDetail(), RenderCreateForm(), RenderSlotGrid()

### Community 31 - "Module 31"
Cohesion: 0.29
Nodes (5): DrawImGui(), FitCameraToModel(), Init(), LoadModel(), RenderToEngineFrame_()

### Community 32 - "Module 32"
Cohesion: 0.39
Nodes (6): Bind(), DispatchEvents(), RequestState(), RequestStateByName(), RequestStateImpl_(), Update()

### Community 33 - "Module 33"
Cohesion: 0.33
Nodes (5): frand(), SpawnEmitter(), spawnInterval(), spawnParticle(), Update()

### Community 34 - "Module 34"
Cohesion: 0.36
Nodes (4): corsMiddleware(), jsonError(), jsonOK(), Server

### Community 35 - "Module 35"
Cohesion: 0.39
Nodes (7): BuildFilter(), gue(), ImportAbsolutePath(), PickAndImportAsset(), PickFolder(), PickMultipleFiles(), Widen()

### Community 36 - "Module 36"
Cohesion: 0.36
Nodes (4): AudioSystem(), PlayMusic(), Shutdown(), StopMusic()

### Community 39 - "Module 39"
Cohesion: 0.46
Nodes (7): DeleteMob(), DeletePoint(), Draw(), EnsureTables(), FetchAll(), SaveMob(), SavePoint()

### Community 40 - "Module 40"
Cohesion: 0.52
Nodes (6): Delete(), Draw(), DrawFields(), EnsureTable(), Fetch(), Save()

### Community 43 - "Module 43"
Cohesion: 0.6
Nodes (5): Delete(), Draw(), DrawFields(), Fetch(), Save()

### Community 44 - "Module 44"
Cohesion: 0.6
Nodes (5): Delete(), Draw(), DrawFields(), Fetch(), Save()

### Community 48 - "Module 48"
Cohesion: 0.6
Nodes (3): Render(), withAlpha(), worldToScreen()

## Knowledge Gaps
- **72 isolated node(s):** `serverConfig`, `databaseConfig`, `gameConfig`, `config`, `Account` (+67 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **8 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `now()` connect `World Systems & Scripting` to `Server Database Layer`, `Net Client & Packet Handling`, `Lua Scripting API`, `DB Schema Structs`, `Server Net & Config`?**
  _High betweenness centrality (0.039) - this node is a cross-community bridge._
- **Why does `NewActor()` connect `Net Client & Packet Handling` to `Module 24`, `Module 20`?**
  _High betweenness centrality (0.021) - this node is a cross-community bridge._
- **Why does `DB` connect `Server Database Layer` to `DB Schema Structs`?**
  _High betweenness centrality (0.017) - this node is a cross-community bridge._
- **Are the 2 inferred relationships involving `gladLoadGLLoader()` (e.g. with `Window()` and `main()`) actually correct?**
  _`gladLoadGLLoader()` has 2 INFERRED edges - model-reasoned connections that need verification._
- **What connects `serverConfig`, `databaseConfig`, `gameConfig` to the rest of the system?**
  _72 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `Server Database Layer` be split into smaller, more focused modules?**
  _Cohesion score 0.06 - nodes in this community are weakly interconnected._
- **Should `Net Client & Packet Handling` be split into smaller, more focused modules?**
  _Cohesion score 0.09 - nodes in this community are weakly interconnected._