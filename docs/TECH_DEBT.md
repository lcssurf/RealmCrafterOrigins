# Débito Técnico — RCO MMO Engine

Lista de pendências reconhecidas durante desenvolvimento. Cada item tem origem 
(quando virou débito) e quando vale atacar. Manter atualizado.

## 1. Split de `server/internal/db/db.go`

**Estado:** arquivo monolítico com ~4500 linhas em 2026-05-16.  
**Contém:** structs de domínio + 25+ migrations + funções CRUD de items, characters, 
spells, quests, party, abilities, weapon kits, etc — tudo misturado.

**Sintomas:**
- Cada mudança em DB toca o mesmo arquivo
- Cognitivo: difícil localizar funções
- Performance da IA: contexto longo reduz qualidade de edits

**Plano proposto:**
Split por domínio em `server/internal/db/`:
- `db.go` (Open, struct DB, helpers)
- `migrations.go` (todas as migrateV*)
- `items.go`, `characters.go`, `spells.go`, `parties.go`, `quests.go`, `weapons.go`, etc

**Quando atacar:** antes da próxima feature que não seja Weapon Kits.  
**Estimativa:** 6-8 commits, 2-3 dias.

## 2. Migração `spell_templates` → `ability_templates`

**Estado:** dois sistemas de skill coexistem.
- Legado: `spell_templates` + `character_known_spells` + `PCastSpell` + spellbar UI
- Novo: `ability_templates` + cast_intent runtime + Weapon Kits

**Bridge atual:** `spell_templates.runtime_ability_id` opcional faz spell legada 
disparar via runtime moderno. Funcional mas frágil.

**Plano proposto:**
- Após Weapon Kits funcional no client (Commits 10-17 da feature atual), avaliar
- Migrar player skills para `ability_templates` exclusivamente
- Aposentar `spell_templates`, `character_known_spells`, `PCastSpell`, aba Spells no GUE
- Hotbar legado removido

**Quando atacar:** após Commit 17 da feature Weapon Kits, ou quando spells virarem 
empecilho perceptível.  
**Estimativa:** 2 semanas, varia conforme spells/Lua em uso.

## 3. DBs duplicados no projeto

**Estado:** existem 3 cópias de `rco.db`:
- `dist/server/rco.db` — DB real, usado por server e GUE
- `server/rco.db` — vazio (0 bytes), legado de teste antigo
- `server/.gocache/.../rco.db` — DB temporário criado pelos `go test`

**Sintomas:**
- Confusão em validação ("IA validou em qual DB?")
- Risco de migrations rodarem em DB errado
- Já causou ~1 dia perdido no Commit 7 da feature Weapon Kits

**Plano proposto:**
- Adicionar regras explícitas em `.gitignore` para todos os `rco.db`
- Deletar `server/rco.db` vazio
- Documentar: "o DB autoritativo é `dist/server/rco.db`. Nenhum outro deve ser usado."

**Quando atacar:** trivial, qualquer hora. 15 minutos.

## 4. Processo: validação em DB real, não em cópia

**Estado:** durante Commits 1.5 / 7 da Weapon Kits, a IA executora reportou validação 
em cópia local porque o DB real estava readonly. Migrations não rodaram no DB real. 
Bug "Create não funciona" custou ~2h de diagnóstico.

**Regra para o futuro:**
- Validação de migration: sempre rodar no `dist/server/rco.db` que GUE/jogo realmente abrem
- Se ambiente bloquear (readonly, lock), parar e reportar — não substituir por cópia silenciosamente
- Aplicar a todo commit que toque schema

**Quando atacar:** já em vigor para próximos commits.

## 7. Lição: nomenclatura captura intent

Durante Commit 14 da feature Weapon Kits, criei coluna `max_skills_in_loadout`
em equipment_slot_config. Nome ambíguo: sugere "tamanho do pool de skills disponíveis"
ao mesmo tempo que "quantos slots no hotbar". Renomeei para `hotbar_slots_granted`
no Commit 15.5 quando a confusão ficou explícita.

Lição para próximos schemas: ao nomear campo, pergunte "este nome distingue de
todos os outros conceitos similares no sistema?". Se há ambiguidade conceitual,
o nome carrega o débito.

---

Reportar:
- Path completo do arquivo criado
- Confirmação que conteúdo bate exatamente com o pedido
- Sem modificar outros arquivos

## 11. Categorias de skill nao-damage sem runtime

Schema de `ability_templates` suporta categories: `damage`, `heal`, `buff`, `debuff`, `mobility`, `utility`, `summon`.
Mas o runtime de combate so processa `damage` hoje.

Quando a primeira skill de outra categoria for criada, o runtime precisa ser estendido:
- `heal`: aplicar cura no target (nao dano negativo)
- `buff`: aplicar status effect com duracao no caster/ally
- `debuff`: status effect no target
- `mobility`: movimento fisico (dash/charge) com regras de cancelamento
- `utility`: efeitos diversos (taunt, interrupt, etc)
- `summon`: spawn de pet/companion temporario

Mastery tambem precisa adaptar semantica de `mastery_primary_bonus_per_lvl` por categoria
(dano, cura, duracao/potencia, etc).

## 12. Ability templates editadas via GUE nao recarregam runtime

Hoje, mudancas em `ability_templates` via GUE exigem restart do server para recarregar
o catalogo runtime.

Quando edicao de skills em tempo de execucao virar frequente, precisamos de um mecanismo
seguro de reload (manual trigger, versionamento ou hot-reload controlado) para evitar
divergencia entre DB e runtime ativo.

## 13. Schema de ability_templates duplicado entre server e GUE

Server cria tabela via `migrateV19` + `migrateV30`. GUE tambem cria via
`EnsureTables` em `combat_abilities.cpp`. Quando schema mudar (novos campos),
precisa atualizar 2 lugares - risco de divergencia.

Decidir futuro:
- GUE deixa de criar tabela (assume que server ja criou) - mais simples
- GUE e server compartilham migrations via arquivo SQL unico - mais robusto
- Manter status quo, aceitar duplicacao consciente

## 14. Retry de SQLite BUSY aplicado apenas em UpsertCharacterSkillProgress

Hotfix 30.5 adicionou retry-with-backoff em `UpsertCharacterSkillProgress`
porque essa funcao e chamada em hot path (hook de combat) e perdia XP.

Outras funcoes de escrita podem ter o mesmo problema mas nao foram protegidas:
- AbilityCooldowns (in-memory, sem risco de SQLite)
- ApplyDamage (HP em memoria; se persistido futuramente, candidato)
- Updates em LastCombatAt (em memoria)

Padrao sugerido para futuro: criar helper `d.executeWithRetry(ctx, query, args...)`
que aplique retry automatico para `ExecContext`. Migrar funcoes criticas conforme
necessario.

Trigger: quando aparecer outro caso de "dado perdido por BUSY", migrar mais
funcoes para o helper.

## 15. Anim bindings faltando em combat

Configurar anim bindings `Hit`, `AttackBasic` e `AttackHeavyWindup` nos
Appearance dos NPCs e player. Hoje isso ainda gera warnings
`missing_action_binding` durante o combate.

## 16. Anim bindings Dodge/Guard/Parry ausentes (fallback Idle)

Adicionar bindings de animacao `Dodge`, `Guard` e `Parry` nos appearances de
personagens e NPCs. Hoje o runtime usa fallback `Idle`, o que evita travamento
de estado, mas nao reproduz anim visual especifica para a acao defensiva.

## 17. `character_primary_stats_per_level` hoje e referencia de NPC/fallback

Com primary stats persistidos por personagem, a tabela
`character_primary_stats_per_level` deixa de ser fonte principal para player e
fica como referencia de defaults por level (NPC/fallback).

Se isso virar confuso no fluxo de dados, considerar separar explicitamente em
uma tabela dedicada para NPCs ou renomear para refletir o novo papel.

## 18. UI de distribuicao de stats/respec pendente no cliente

Backend de distribuicao de pontos e respec foi preparado, mas a interface de
cliente (sheet/botoes/feedback) fica para o commit 38b.

Enquanto isso, os pacotes existem e o estado e persistido, mas o jogador final
nao tem fluxo visual para consumir a feature.

## 19. Cache de progression/primary stats exige restart

`CharacterProgressionRuntimeConfig` e cache de primary stats por level sao
carregados no startup do server.

Edicoes feitas via GUE/SQL em runtime nao refletem imediatamente sem restart.
Se isso virar gargalo, planejar mecanismo de reload controlado.

## 20. Formulas de derived stats duplicadas no cliente

Para suportar preview reativo de distribuicao de atributos, as formulas de
`server/internal/world/derived_stats.go` foram replicadas em
`client/src/core/derived_stats.h`.

Sempre que qualquer coeficiente ou formula mudar no server, o cliente precisa
ser atualizado de forma identica para nao mostrar preview divergente.

## 21. Config de respec hardcoded no cliente

`kRespecFreeUntilLevel`, `kRespecCostGold` e `kInitialStatValue` estao
hardcoded na UI do cliente.

Ideal: server enviar esses valores no login (ou pacote de config) para evitar
drift entre balance runtime e UX.

## 22. Parse defensivo de PStartGame sem Remaining()

`Reader` do cliente nao expoe `Remaining()`. O parse dos campos extras de stats
em `PStartGame` usa `Done()` + `ReadU16()` + `OK()`.

Funciona para compat retroativa, mas manteria o parser mais claro se existisse
helper explicito de bytes restantes.

## 23. sendVitalsUpdate centraliza PStatUpdate, mas sendXPUpdate ainda duplica

Foi adicionado helper `sendVitalsUpdate` para centralizar envio de
`PStatUpdate` (HP/EP/SP atuais e maximos), usado em fluxos de distribute/respec.

`sendXPUpdate` ainda contem a mesma logica inline. Futuro refactor: migrar
`sendXPUpdate` para chamar `sendVitalsUpdate` internamente e eliminar duplicacao.

## 24. Cache de kill_xp_scaling_config exige restart

`kill_xp_scaling_config` e carregada para cache runtime no startup do server.

Edicoes via GUE/SQL nao refletem em runtime ate reiniciar o processo.

## 25. XP scaling editado dentro de Progression Config tab

No GUE, os campos de kill XP scaling foram adicionados na mesma tab de
Progression Config.

Se o escopo dessa tab crescer demais, considerar separar em uma tab dedicada de
XP scaling para reduzir ruido operacional.

## 26. `awardXPGain` usa `scalingMobLevel=0` como sentinela

Para manter um unico caminho de aplicacao de XP, `awardXPGain` recebe
`scalingMobLevel` e interpreta `0` como "nao aplicar scaling de kill".

Funciona, mas a intencao ficaria mais explicita com split futuro entre
`awardKillXP` e `awardQuestXP`.

## 27. Mastery kill window nao cobre heal/buff/debuff/defense

O tracking de combat window para mastery XP por kill foi conectado apenas em
skills de dano no fluxo de special hit.

Skills de heal/buff/debuff/defense ainda nao entram na janela e nao recebem
mastery XP nesse modelo. Precisa de hooks dedicados por categoria.

## 28. Decisao de design: melee basico nao progride mastery

Por decisao de design, ataque basico (melee normal) nao trackeia combat window
e nao concede mastery XP.

Mastery e progressao de skills nomeadas (Slash, Cleave, etc). Para progredir
mastery, o player deve usar specials.

## 29. Legacy spell com runtime_ability_id=0 nao participa de mastery kill window

Quando spell legada nao mapeia para runtime ability (`runtime_ability_id=0`),
ela nao entra no tracking de skill usada para mastery kill XP.

Idealmente esse caminho some quando o fluxo legado de spells for migrado.

## 30. Combat windows sao runtime-only

As janelas de combate sao mantidas em memoria e nao persistem em crash/restart.
Se o server cair durante combate, estado de tracking e descartado.

## 31. Cleanup de janelas depende de tickRegen

Expiracao por timeout e executada no `tickRegen` (a cada ~3s). Isso pode atrasar
coleta de janelas expiradas em ate um tick.

## 32. Cache de mastery kill scaling exige restart

Os valores de mastery kill scaling carregam no startup e ficam em cache runtime.
Edicoes no GUE/SQL exigem restart para refletir no servidor ativo.

## 33. Heavy Attack stats removidos por decisao de design

Heavy Attack stats foram removidos porque o modelo T&L-style de proc 2x cria
stat dominante (builds convergem para o mesmo eixo). Critico ja cobre o papel
de proc multiplier sem introduzir esse acoplamento.

Se feature de "heavy attack" voltar no futuro, tratar como design novo
(ex.: charged attack Souls-style, stagger mechanic, ou outra direcao), sem
ressuscitar automaticamente os stats antigos.

As animacoes `AttackHeavyWindup`/`AttackHeavyImpact` foram mantidas, pois sao
nomes genericos usados por specials como Cleave e nao dependem desses stats.

## 34. DamageStatScale em runtime ainda limitado a primary/level

`damage_stat_scale_json` agora e aplicado no runtime moderno, mas os sources de
scaling suportados no momento sao apenas `STR`, `DEX`, `INT`, `WIS`, `PER` e
`level`.

Campos derived (ex.: `MeleeCritValue`, `MagicDmgMax`) ainda nao estao
disponiveis como source de scaling por skill.

## 35. CritPolicy em runtime depende de JSON valido com fallback silencioso

`crit_policy_json` agora substitui a formula global de crit quando parseia com
sucesso. Se vier vazio/invalido, runtime faz fallback para a formula global e
registra `WARN` no log.

## 36. GUE sem validador inline de JSON para scaling/crit

O GUE exibe apenas hints de exemplo para `damage_stat_scale_json` e
`crit_policy_json`. Ainda nao existe validacao inline; erros so aparecem no
runtime via log.

## 37. GUE sem "Insert Template" para JSON de ability

Ainda nao existe botao de preenchimento automatico de template JSON para os
campos de scaling/crit. O usuario continua escrevendo manualmente.

## 38. GUE sem auto-complete inline para JSON de ability

Os campos `damage_stat_scale_json` e `crit_policy_json` ainda nao tem
auto-complete inline. Hoje o fluxo usa templates por dropdown para cobrir os
casos mais comuns.

Quando essa UX virar prioridade, avaliar widget custom de edicao JSON
(ex.: ImGuiColorTextEdit ou equivalente).

## 39. Validacao semantica de JSON nao cobre interacoes logicas

A validacao atual cobre sintaxe JSON, nomes de stats e ranges de campos.
Ainda nao cobre interacoes logicas mais profundas (ex.: `scaling` vazio, que e
sintaticamente valido mas pode nao gerar efeito pratico no runtime).

## 40. Save no GUE permite JSON invalido por design (WIP-friendly)

O GUE permite salvar ability mesmo com JSON invalido em scaling/crit e apenas
emite `WARN` no console para nao bloquear iteracao.

No runtime do server, JSON invalido continua caindo em fallback da formula
global.

## 41. nlohmann/json foi adicionado apenas no GUE

`nlohmann/json.hpp` foi adicionado como header-only em
`tools/gue/third_party/nlohmann/`.

Existem outros pontos do projeto (especialmente no client) com parsing JSON
manual; avaliar consolidacao futura para reduzir codigo ad-hoc.

## 42. `resource_type=hp` usa clamp em 1 HP (nao auto-suicida)

No runtime de cast, custo de HP agora e consumido com clamp em 1 de vida para
evitar auto-kill acidental ao usar ability.

Se no futuro o design quiser permitir "blood magic" com auto-sacrificio,
remover esse clamp no `consumeCastResource`.

## 43. HP cost segue sem modificadores adicionais

Custo de HP por ability nao interage com mastery cooldown reduction nem com
outros modificadores de recurso, mantendo comportamento consistente com o fluxo
atual de MP/SP.

## 44. ability_templates: IDs legados e paths coexistem para FX

`vfx_id_*` / `sfx_id_*` (INTEGER) coexistem com `vfx_path_*` / `sfx_path_*`
(TEXT) em `ability_templates`.

Os IDs legados serao removidos em commit futuro, apos Q3.2/Q3.3 estarem
funcionando e a migracao de conteudo para paths estar concluida.

## 45. GUE Combat Abilities com campos duplicados (legado + novo)

A tab de Combat Abilities mantem os campos antigos `InputInt` (IDs) e os novos
`InputText` (paths) ao mesmo tempo para compatibilidade de transicao.

Apos a remocao dos IDs legados, simplificar UI removendo os campos inteiros.

## 46. Fase recover de FX ainda nao suportada em schema path

No modelo de paths, por enquanto so existem fases `windup` e `impact`.
`recover` ainda nao foi adicionado.

Se design futuro exigir, adicionar `vfx_path_recover` / `sfx_path_recover` em
migration dedicada.

## 47. VFX path mapping hardcoded para 6 chaves

No client, o mapeamento de `vfx_path` para `EmitterType` e hardcoded com as
chaves:
`vfx:fire`, `vfx:explosion`, `vfx:heal`, `vfx:portal`, `vfx:blood`,
`vfx:smoke`.

Quando o sistema de FX evoluir para templates/data-driven, esse mapeamento
deve migrar para lookup em catalogo configuravel.

## 48. SFX path recebido, mas audio ainda e ID-based

`PSound` agora pode carregar `sfx_path` no payload expandido, mas o client
ainda usa pipeline de audio por `sound_id`.

Quando `sfx_path` vier preenchido, o client apenas registra log e ignora
playback legado para esse evento.

## 49. Writer.Read/WriteString usa prefixo uint16

O formato de string no protocolo segue `uint16 length + bytes UTF-8` sem
terminador nulo.

Esse contrato deve continuar documentado em qualquer extensao futura de packet
para evitar divergencia entre parser Go/C++.

## 50. Campos opcionais em packet dependem de bytes restantes

No client, parsing dos campos ricos de `PCreateEmitter`/`PSound` depende de
checagem de bytes restantes (`!Reader.Done()`) para manter backward compat com
payload legado.

Padrao deve ser mantido enquanto coexistirem formatos antigo e expandido.

## 51. PCreateEmitter/PSound ainda carregam campos legados

Os packets `PCreateEmitter` e `PSound` foram expandidos com contexto rico,
mas mantem prefixo legado para compatibilidade.

Quando todos os clientes estiverem atualizados e o uso legado for removido,
simplificar payload e remover campos antigos.

## 52. fx_templates V0.1 ainda e single-emitter

`fx_templates` V0.1 suporta apenas 1 emitter por template.

Ainda nao existe suporte a sub-emitters, multi-stage emitters ou composicao de
efeitos mais complexos no mesmo template.

## 53. fx_templates sem curvas avancadas e GPU compute

Interpolacoes atuais sao lineares (`start -> end`) e nao ha curvas
customizadas por canal/tempo.

Tambem nao existe caminho para simulacao GPU/compute no pipeline atual.

## 54. fx_templates sem hot-reload

O catalogo de FX e carregado no startup do server e cacheado em memoria.

Edicoes futuras no GUE exigem restart para refletirem no runtime.

## 55. Campos de FX V0.1 ainda limitados

`fx_templates` V0.1 nao inclui multi-emitter, `particle_orientation`,
billboard mode, alpha curves, drag ou turbulence.

Esses campos ficam para evolucao V0.2+ quando o mini Niagara avancar.

## 56. fx_templates V0.1 fundação sem runtime

- `fx_templates` V0.1 usa 1 emitter por template, sem sub-emitters, sem curvas
  customizadas (interpolação linear start→end), sem GPU compute.
- `fx_templates` não tem hot-reload no runtime: edição no GUE (F2) exige restart
  do server para refletir no cache usado pelo gameplay (F4).
- `seed` de `fx_templates` replica os valores exatos de `kCfg[]` hardcoded.
  A remoção de `kCfg[]` do cliente só ocorrerá em F6.
- `fx_templates` V0.1 não inclui: multi-emitter, particle_orientation,
  billboard mode, alpha curves, drag, turbulence. Evoluir para V0.2+.
## 57. Refatorar part�culas para shared + API data-driven (F3.1)
- Partir de agora, `particle.h/.cpp` foi movido de `client/src/renderer/` para
  `shared/renderer/include/rco/renderer/particles.h` e `shared/renderer/src/particles.cpp`
  para compartilhar a implementa��o entre cliente e ferramentas.
- `FXParams` foi adicionado no cabe�alho shared e usado como fonte de verdade do
  spawn no runtime (`spawn`/simula��o).
- A API antiga foi preservada: `SpawnEmitter(EmitterType, ...)` segue funcionando.
- `kCfg[]` permanece hardcoded (sem remo��o nesta etapa) e foi convertido para
  `FXParams` no wrapper `SpawnEmitter`.
- `kGravity` segue hardcoded em `ParticleSystem::Update` como `-4.5f` (V0.2+).

## 58. FX Templates preview no GUE (F3.2)
- Added a dedicated 3D FX preview viewport to FX Templates tab using shared `ParticleSystem`.
- `FXPreviewViewport` now owns a private FBO and orbit controls; renders in-editor via `ImGui::Image`.
- Preview in `FXTemplatesTab` is updated on selection changes (`preview_for_id_`), with one-time reseed on entering `New FX Template`.
- Spawn loop is periodic and visual-only (does not replicate gameplay timing).
- `FXPreviewViewport` GL resources are reclaimed by the OS at process exit (no GL cleanup in destructor, since GUE tabs are destroyed after the GL context is gone � same constraint affects existing viewports).
- FX preview ao vivo (F3.3): edicao de campos atualiza params do preview; a mudanca aparece na proxima onda de respawn (nao limpa particulas ja vivas). Delay maximo = spawn_period_ (~1x lifetime). Opcao deliberada para evitar flicker durante drag de sliders.
## 59. PFXCatalog (F4.1): server envia catálogo de fx_templates no startgame
- PFXCatalog (138): server envia catálogo completo de fx_templates no startgame.
  Serializa os 24 campos por template. A ORDEM dos campos é contrato com o
  cliente (F4.2) — manter sincronizado.
- texture_path e display_name são enviados mas ainda não usados pelo cliente
  (à prova de futuro).
- sendFXCatalog roda uma vez por entrada no mundo. Sem hot-reload: editar
  fx_templates no GUE exige o cliente reconectar/reentrar pra receber catálogo
  novo (mesma limitação já documentada do catálogo server-side).

## 60. F4.2: cliente cacheia PFXCatalog e usa SpawnEmitterParams
- Cliente recebe PFXCatalog no login e armazena fx_key -> rco::renderer::FXParams em cache para templates com enabled==1.
- Em kPCreateEmitter, o client usa vfx_path para buscar no cache e usar SpawnEmitterParams em prioridade.
- Se nao encontrar no cache, ainda usa fallback ResolveVFXPathToType para manter compatibilidade com os 6 enums hardcoded.
- Campos V0.1 display_name, id, texture_path sao lidos do pacote, mas nao entram no FXParams.

## 61. F5+F6: removido fallback hardcoded e kCfg[] (FX 100% data-driven)
- Em `kPCreateEmitter`, o cliente passou a usar somente o `PFXCatalog` em cache (`fx_catalog`) e n�o usa mais `ResolveVFXPathToType`.
- Se `vfx_path` não estiver no cache, emite WARN.
- Emissão legacy por `type` sem path segue temporariamente mapeada para chaves de catálogo (`vfx:fire`, `vfx:explosion`, ...), enquanto houver emissões legadas no fluxo do server.
- Itens legados relacionados a `kCfg[]` foram resolvidos:
  - ## 47 (mapping hardcoded em VFX) [RESOLVIDO]
  - ## 56 (remoção de `kCfg[]` do cliente) [RESOLVIDO]
  - ## 57 (API antiga `SpawnEmitter(EmitterType, ...)`, `kCfg[]`, `ParamsFromTypeCfg`) [RESOLVIDO]
  - ## 60 (fallback `ResolveVFXPathToType` em F4.2) [RESOLVIDO]
- Removidos de `shared/renderer/include/rco/renderer/particles.h` / `shared/renderer/src/particles.cpp`:
  - `enum EmitterType`
  - `ResolveVFXPathToType`
  - `SpawnEmitter(EmitterType, ...)`
  - `kCfg[]`
  - `TypeCfg`
  - `ParamsFromTypeCfg`
- `SpawnEmitterParams` segue como API runtime viva para todos os spawns.

## 63. L1: loot_tables + loot_entries (schema + CRUD, data-driven loot)
- Adicionadas `loot_tables` (`id`, `name`, `enabled`) e `loot_entries` (`loot_table_id`, `item_id`, `chance`, `min_qty`, `max_qty`).
- Adicionada coluna `loot_table_id INTEGER DEFAULT 0` em `media_actor_defs`.
- Criada migration `V40` (`schema_v40_loot_tables`) com idempotência por meta-key, criação condicionada por DB e alteração de coluna via `addColumnIfMissing`.
- `List/Get/Create/Update/Delete` de `LootTable` e `List/Create/Update/Delete` de `LootEntry` implementados em `server/internal/db/db.go` para habilitar CRUD lato GUE.
- `enabled` em `loot_tables` é tratado com soft-delete (`DeleteLootTable` seta `enabled=0`); `loot_entries` não tem soft-delete e usa `DELETE`.
- Nenhum seed foi adicionado (tabelas iniciam vazias).

## 65. L3: runtime resolve drops por actor_def.loot_table_id (loot data-driven completo)
- `main.go` substituiu `setupDropsAndShops` por:
  - `setupLootCatalog` para carregar `loot_tables` + `loot_entries` em memória runtime.
  - `setupShops` para manter apenas o registro de inventário dos comerciantes.
- `setupLootCatalog` carrega `item_templates`, resolve cada `loot_entry.item_id` no DB e monta `world.DropEntry` completo (`ItemType`, `SlotType`, `WeaponDamage`, `ArmorLevel`, `ItemValue`, etc.) antes de popular o cache via `world.SetLootCatalog`.
- `world` ganhou catalogo runtime `map[int]*LootTableRuntime`, com:
  - `SetLootCatalog(...)`
  - `GetLootTable(id int) (*LootTableRuntime, bool)`
  - `RollDropsByTable(lootTableID, x, y, z float32) []*DroppedItem`
- O fluxo de morte do NPC agora usa `npc.LootTableID` para resolver drops; mob sem `loot_table_id` (0) não solta loot.
- Removido hardcoded runtime de drops (`setupDropsAndShops` drops, `npcDropTables`, `RegisterDropTable`, `RollDrops(npcName)`); `spawn` de NPCs passa `ActorDefID` -> `ActorDef` -> `LootTableID` e grava no runtime actor.

## 66. D1: game_settings (config global) + default_drop_model_id
- Adicionada tabela global `game_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL DEFAULT '')` para configs globais.
- Migration `V41` (`schema_v41_game_settings`) aplicada no `server/internal/db/db.go`, incluindo branch de idempotência via meta-flag.
- `game_settings` também incluída no `server/internal/db/schema.sql` para provisionamento de ambiente novo.
- CRUD genérico implementado em `server/internal/db/db.go`:
  - `GetSetting(ctx, key) (string, error)` retorna `""` quando não existe.
  - `SetSetting(ctx, key, value string) error` (UPSERT).
  - `ListSettings(ctx) (map[string]string, error)`.
- Padrão de configuração inicial `default_drop_model_id`:
  - Chave: `default_drop_model_id`.
  - Valor em TEXT (ex: `"5"`, vazio = sem mesh).
- Nova tab GUE "Settings" (nova aba no `tools/gue/src/main.cpp`) criada para editar:
  - `default_drop_model_id` com combo de `media_models` (label por `id` + `name`/`file_path`);
  - botão **Save** que faz UPSERT em `game_settings`.
- D2: consumo de `default_drop_model_id` no fluxo de render do `PWorldItem` para desenhar o mesh de chão.

## 67. D2: mesh girando/flutuando nos itens dropados
- Implementada a resolução de `default_drop_model_id` em `main.go`:
  - lê `game_settings.default_drop_model_id`,
  - converte para `media_models.id`,
  - resolve `file_path` via `db.GetMediaModel`,
  - publica no runtime com `world.SetDropModelPath`.
- Adicionado estado global de runtime `world.SetDropModelPath`/`GetDropModelPath` para o mesh padrão de drops:
  - vazio = sem mesh.
- `PWorldItem` passa a enviar `model_path` nos dois pontos:
  - `world.Area.AddDroppedItem` (drops ao nascer).
  - `sendWorldItems` (itens enviados na sincronização de área/reentrada).
- Cliente:
  - `WorldItemEntry` ganhou `model_path`, `actor` (`unique_ptr`), `spawn_time`, move-only.
  - `kPWorldItem` passou a ler `model_path` com `if (!r.Done()) wi.model_path = r.ReadString();`
    para compatibilidade retroativa.
  - Render loop cria `Actor` por item sob demanda e renderiza com:
    - flutuação (`sinf`) e deslocamento vertical,
    - rotação contínua por tempo,
    - `scale = 1.0f`.
- HUD 2D de pickup continua inalterado (rótulos/texto de interação ainda via `world_items`).

## 68. D3: escala configurável + correção de textura do mesh de drop
- Adicionada configuração global `default_drop_model_scale` em `game_settings` (TEXT), na mesma cadeia de configs já criada em D1.
- Leitura no boot:
  - `main.go` lê `default_drop_model_scale`,
  - converte de string para float (fallback `1.0` em erro/ausência),
  - publica com `world.SetDropModelScale`.
- `PWorldItem` passou a carregar escala (após `model_path`) em ambos os pontos de serialização:
  - `world.Area.AddDroppedItem` (spawn de novos itens);
  - `net.ClientConn.sendWorldItems` (sync de área/reentrada).
- Cliente:
  - `WorldItemEntry` recebeu `model_scale` (default `1.0f`);
  - parser `kPWorldItem` lê `model_scale` com `if (!r.Done()) wi.model_scale = r.ReadF32();` (compatível com payloads antigos);
  - render aplica `wi.actor->scale = wi.model_scale` quando carregado;
  - ao criar `Actor` de drop novo, marcação de materiais é feita com `engine.MarkMaterialsDirty()`, alinhado ao fluxo dos world objects.
- GUE/Settings:
  - `tools/gue/src/tabs/settings.cpp` agora edita e persiste:
    - `default_drop_model_id` (string) e
    - `default_drop_model_scale` (string numérica).
- `default_drop_model_scale` vazio/ausente => visual padrão sem alterar escala (1.0f), mantendo compatibilidade.

## 69. Centralizar combobox por id em helper reutilizável (início)
- GUE:
  - Adicionado `tools/gue/src/ui_widgets.h` com `gue::ui::SearchableComboId`.
  - O helper combina `BeginCombo`, `ImGuiTextFilter` e `Selectable`, com filtro por texto e estado persistente por combo (`ImGuiID`).
  - `tools/gue/src/tabs/settings.cpp` migra `Default Drop Model` para usar esse helper (removida cópia local de `ComboId`).

## 70. B2: blood-on-hit configurável (Settings)
- Configuração global:
  - `blood_fx_key` (string, em `game_settings`): `fx_key` do FX de sangue/impact. `""` desativa.
  - `blood_mode` (string, em `game_settings`): `"basic"` (padrão) ou `"all"`.
- GUE Settings:
  - Carrega `fx_key` de `fx_templates` para uma `SearchableComboString` nova em `tools/gue/src/ui_widgets.h`.
  - Salva/edita `blood_fx_key` e `blood_mode` (`basic`/`all`).
- Server bootstrap:
  - `main.go` lê `blood_fx_key` e `blood_mode` no startup e chama `world.SetBloodFX` / `world.SetBloodMode`.
  - Log informa `blood fx: key=%q mode=%q`.
- Hook de FX no mundo:
  - `world.SetBloodFXHook` + `world.BroadcastBloodFX`.
  - O net registra via `world.SetBloodFXHook(s.handleBloodFXBroadcast)` em `server.NewServer`.
  - `combat_fx_bridge.go` implementa `handleBloodFXBroadcast`, que reutiliza `broadcastEmitterRich` (padrão já usado por ability FX).
- Emissão de FX:
  - Auto-attack (`handleAttackActor`): se `dmg > 0`, `target.IsNPC` e `blood_fx_key != ""`, emite em `target.X/Y/Z` com `FXPhaseImpact` quando `blood_mode` é `"basic"` ou `"all"`.
  - Ability/path especial (`combat_special.go`): após `ApplyDamage`, quando `GetBloodMode()=="all"` e `dmg>0` em NPC, dispara `BroadcastBloodFX`.
  - Legacy Lua (`Combat.deal_damage`): após `ApplyDamage` no alvo principal e no splash AOE, quando `GetBloodMode()=="all"` e `amount>0`, dispara `BroadcastBloodFX`.
- Regras:
  - Não dispara em `dmg==-1`/miss (auto-attack usa `dmg > 0` no handler).
  - `blood_mode == "basic"` = somente auto-attack.
  - `blood_mode == "all"` = auto-attack + dano de ability.
  - Em `all`, não há deduplicação com `impact` de ability (efeitos somam).

## 71. B3: elevar emissões de impacto para altura de impacto do alvo (corpo/torso)
- Cliente:
  - Em `client/src/core/main.cpp` (`case rco::net::kPCreateEmitter`), o spawn de FX agora tenta usar `target_rid` (quando presente) para buscar `world_actors[target_rid].actor`.
  - Calcula `impact_y = target.Y + actor->ModelHeight() * 0.5f` e usa `{x, impact_y, z}` para `SpawnEmitterParams`.
  - Mantém fallback para `{x, target.Y, z}` quando o actor-alvo não existe em `world_actors` ou não tem `actor` válido.
- Server:
  - Mantido o fluxo de emissões (`broadcastEmitterRich` -> `kPCreateEmitter`) sem mudanças funcionais.
- Debug:
  - Removidos os logs de diagnóstico temporários usados na investigação de B3:
    - `blood-debug` em `server/internal/net/client.go`.
    - `HIT-DEBUG` em `server/internal/world/combat_melee.go`, `server/internal/world/combat_special.go`, `server/internal/world/spell.go`, `server/internal/scripting/api.go`.
    - `HIT-DEBUG` no parser de `kPCreateEmitter` em `client/src/core/main.cpp`.

## 73. I2b: editor de atributos por item na Items tab (GUE)
- Implementar seção "Attributes" na `tools/gue/src/tabs/items.cpp` com lista por linha de pares `(atributo, valor)` e remoção (`PushID` por linha), seguindo padrão do editor de loot entries.
- Inclusão de `tools/gue/src/attribute_list.h` com a lista de 37 chaves canônicas usadas em `item_attributes` (keys/display + kind/isFloat) para o lado de edição do GUE.
- `Save` da `ItemsTab` passa a persistir atributos em `item_attributes` via transação:
  - `BEGIN`
  - `DELETE FROM item_attributes WHERE item_id=?`
  - `INSERT (...)` para cada atributo não-vazio
  - `COMMIT`
- Atributos agora são carregados **lazy** ao selecionar um item (uma consulta por seleção), não no fetch geral de `item_templates`.
- Pendência técnica (`TECH_DEBT`) permanece: é a 3ª cópia da lista canônica de atributos (`server/internal/world/attributes.go` + `client/src/core/derived_stats.h` + `tools/gue/src/attribute_list.h`) e precisa unificação futura.

## 74. C0: completa o trio de dano com RangedDmgMin/Max

- `DerivedStats` ganhou `RangedDmgMin` e `RangedDmgMax`; fórmula `RangedDamageRange`
  adicionada em `server/internal/world/derived_stats.go` (DEX no mínimo e DEX+PER no máximo),
  e a função é usada em `ComputeDerivedStats`.
- `MagicDamageRange` e `MeleeDamageRange` permanecem sem alteração.
- Registry +2 chaves (`ranged_dmg_min`, `ranged_dmg_max`) adicionadas nas 3 cópias:
  - `server/internal/world/attributes.go`
  - `tools/gue/src/attribute_list.h`
  - `client/src/core/derived_stats.h` (estrutural + constants/fórmula espelhada)
- Comportamento de combate não mudou: os novos campos ainda não são consumidos em
  resolução de dano (foco dessa etapa é estrutural e de dados).
- Estado: **Pendente para próxima etapa de combate (C1/C2)**.

## 76. C2: ataque básico roteia por dimensão da arma (+ weapon_type 4 magic)
- `WeaponTypeToDimension` (1/2→melee, 3→ranged, 4→magic) em `combat_dimension.go`.
- `Actor.BasicAttackDim`: dimensão do básico, setada no equip (start/swap/useItem)
  via `GetEquippedStats` estendido (agora retorna também o `weapon_type` da arma
  do slot 0).
- `ProcessAttack` usa `attacker.BasicAttackDim` (lido sob lock junto com
  `aDerived`) em vez de `DimMelee` fixo.
- NPCs: `BasicAttackDim = DimMelee` explícito em `SpawnNPC` (melee mantido).
- `weapon_type` 4 (magic) adicionado no GUE (`kWeaponTypes`, Combo "Weapon Type").
- Resultado: arco (weapon_type=3) usa stats ranged, cajado (weapon_type=4) usa
  stats magic, espada (1/2) e sem-arma (0) continuam melee. Ability/special
  attack NÃO afetada (C3).
- Lembrete: `handleInventorySwap` só recomputa/atualiza stats quando o slot 0
  (arma) é trocado — `BasicAttackDim` segue a mesma regra (atualizado só quando
  `weaponSlotAffected`). Bug de armadura-não-recomputa noutros slots fica pro
  arco de itens (I3).

## 77. C2.5: separa weapon_type em weapon_dimension + weapon_hands
- `weapon_type` (misturava empunhadura+dimensão) REMOVIDO. Migration V43 cria
  `weapon_dimension` (0/1/2=melee/ranged/magic) + `weapon_hands` (1/2), migra
  dados, dropa `weapon_type` (SQLite moderno via `modernc.org/sqlite` suporta
  `DROP COLUMN` diretamente — sem necessidade de recriar a tabela).
- `weapon_hands` SEM efeito mecânico ainda (rótulo, como era one/two-hand
  antes) — reservado pra futuro (two-hand bloquear escudo, dar dano, etc).
- `GetEquippedStats` retorna `weaponDimension` (valor direto de
  `world.CombatDimension`); `WeaponTypeToDimension` REMOVIDA (não converte
  mais — `BasicAttackDim = world.CombatDimension(wdim)`).
- GUE: dois combos ("Dimension": Melee/Ranged/Magic, "Hands": One-Hand/Two-Hand)
  no lugar do "Weapon Type" único.
- `weapon_kit` NÃO tocado (ortogonal, sistema de abilities). Possível
  inconsistência de dados (kit=bow + dimension=melee) é responsabilidade de
  autoria, não validada.

## 78. Fix: slot_type=255 em itens weapon/armor novos no GUE (não equipavam)
- Combo "Equip Slot" mostrava valor (fallback 0) sem sincronizar t.slot_type,
  deixando 255 (bag-only) → item não equipava. Agora sincroniza o fallback.
- Bug pré-existente, exposto pelos primeiros itens-arma criados via GUE (C2.5).
- Itens já quebrados (Fire Staff, Bow) corrigir via GUE re-selecionando o slot.

## 79. CB1: range por arma (+ fallback por dimensão)
- weapon_range no item (migration V44). Equip resolve: arma se >0, senão default
  da dimensão (melee 2 / ranged 15 / magic 12) via ResolveAttackRange.
- actor.AttackRange setado no equip (junto de BasicAttackDim). InAttackRange JÁ
  usava AttackRange — intocado.
- Arco/cajado agora atacam de longe. Desarmado = melee 2.
- NPCs intocados (já têm AttackRange do spawn).
- weapon_range por arma prepara terreno pra buffs de range futuros (RangeBonusPct órfão).
- NOTA: InMeleeRange tinha nome enganoso (serve todas dimensões). Renomeada para
  InAttackRange — ver item 91.

## 80. CB2: AttackSpeedMult liga ao delay do ataque básico
- ProcessAttack: delay efetivo = CombatDelay / AttackSpeedMult (era CombatDelay
  fixo 800ms). Mais DEX = ataque mais rápido. Faixa: 800ms (mult 1.0) → 533ms
  (mult 1.5, cap).
- AttackSpeedMult lido sob lock antes do check de cooldown.
- Afeta player E NPC (agnóstico a IsNPC, padrão do projeto). NPC com DEX alto
  ataca mais rápido.
- Guard atkSpeed<=0 → 1.0 (evita div-zero). Piso 1.0 = nunca mais lento que antes.
- Só ataque básico. Abilities têm cooldown próprio (CooldownSpeedPct órfão fica
  pra CB futuro).

## 81. CB3: regen real (misto HP max + atributo)
- HealthRegen/EnergyRegen agora MISTO: proporcional ao HealthMax/EnergyMax (justo
  pra tanks) + bônus do atributo (STR/WIS). Calculado no derived (por segundo).
- tickRegen usa HealthRegen*regenTickSeconds em vez de hpMax/20 hardcoded. Piso 1,
  clamp no max, só fora de combate (preservado).
- Constantes: healthRegenPctOfMax=0.01 (1%/s), energyRegenPctOfMax=0.02 (2%/s) +
  os perSTR/perWIS existentes.
- Cliente derived_stats.h espelhado.
- Stamina inalterada. NPCs incluídos (Derived deles).

## 83. AO-Go: consolida atributos num arquivo central (attributes.go)
- Tudo de atributos do lado Go agora em attributes.go: registry + structs
  (Primary/Derived) + constantes + ComputeDerivedStats + RecomputeDerivedStats +
  helpers. derived_stats.go removido.
- Organizado em seções (typing / constants / computation / recompute).
- Reorganização PURA, comportamento idêntico, mesmo package (zero mudança de import).
- Objetivo: UMA fonte de atributos por linguagem (menos lugares espalhados).
- GUE já organizado (attribute_list.h). Cliente (unificar PlayerState/PrimaryStats)
  fica pra um AO-Client futuro.
- Reforça contexto do TECH_DEBT #73 (3 cópias) — agora cada cópia é 1 arquivo central.

## 84. Renomeia combat_melee.go → combat_basic.go (nome legado)
- ProcessAttack trata as 3 dimensões desde C1/C2; o nome "melee" enganava.
- Só o ARQUIVO + comentários renomeados. Símbolos (InMeleeRange, etc) mantidos
  pra não afetar callers — renomear símbolos fica pra cleanup futuro (ver item 91).

## 91. Rename InMeleeRange → InAttackRange (símbolo)
- Função tratava todas dimensões desde CB1; nome "melee" era enganoso. Renomeada
  + todos os callers. Constante MeleeRange (fallback default) mantida (não enganosa).

## 85. I3: atributos de item aplicados no personagem
- GetEquippedAttributes (db.go) agrega item_attributes dos equipados (slot<14),
  somando valores repetidos (2 anéis +str = +2*str).
- RecomputeDerivedStats(actor, itemBonuses): primários somados em
  effectivePrimary (cópia local, actor.Primary base intocado) e propagados via
  ComputeDerivedStats; derivados aplicados via applyDerivedBonus como overlay
  pós-compute. Recalculado do zero a cada chamada (desequipar = bônus somem).
- applyDerivedBonus (32 cases, 1:1 com DerivedStats e AttributeRegistry) vive em
  attributes.go, seção ITEM BONUS (AO-Go consolidado).
- Helper c.recomputeStatsWithItemBonuses(ctx) no client.go busca os bônus e
  chama o recompute — usado por TODOS os callers (equip, distribute-stat, respec,
  level-up via party/quest XP) pra não esquecerem os bônus de item.
- Gap do swap CORRIGIDO: handleInventorySwap recomputa incondicionalmente em
  qualquer swap bem-sucedido; BasicAttackDim/AttackRange só mudam quando a arma
  (slot 0) é afetada.
- NPCs (world.go spawnNPC) passam nil — não usam item_attributes.

## 86. DS-min: vitals atualizam ao equipar (sendVitalsUpdate nos pontos de equip)
- SUPERSEDIDO pelo item 87 (PFullStats) — sendVitalsUpdate foi removido e o
  helper de recompute agora envia o snapshot completo via PFullStats.
- Limitação histórica: só os 6 vitals. Resolvido pelo PFullStats (34 derived).

## 87. SYNC-1: pacote PFullStats (sync completo de stats S->C)
- PFullStats (id 139): primários + unspent + 6 vitais + 34 derived (ordem da
  struct DerivedStats). Enviado via sendFullStats, embutido no helper
  recomputeStatsWithItemBonuses → dispara nos 7 pontos de "derived mudou"
  (handleStartGame, handleInventorySwap, handleUseItem, handleDistributeStatPoint,
  handleRespec, awardXPGain, applyQuestTurnInResult, awardXPAmount).
- handleStartGame dispara sendFullStats após o bootstrap PStartGame (Opção A).
- DS-min removido (sendVitalsUpdate no equip) — coberto pelo PFullStats.
- Senders removidos (órfãos após embutir sendFullStats no helper + startgame):
  sendVitalsUpdate (4 chamadas → 0), sendPrimaryStatsUpdate (3 chamadas → 0),
  sendStatPointsUpdate (4 chamadas → 0). Todas as 3 funções foram deletadas.
- Canal leve de vital atual (BroadcastHPUpdate/MPUpdate/SPUpdate + inlines de
  dano/cura/regen/respawn/consumível) INTOCADO.
- Cliente (receber PFullStats + PlayerState.derived + unificar primários) é o
  SYNC-2 — ainda não implementado. Até lá, o cliente não lê PFullStats; os
  campos que antes vinham por sendPrimaryStatsUpdate/sendStatPointsUpdate no
  startgame já chegavam duplicados pelo próprio payload do PStartGame (extras),
  então a remoção não regride o estado atual do cliente.

## 88. SYNC-2: cliente recebe PFullStats, UI usa stats reais
- PlayerState.primary (struct PrimaryStats, AO-Client) + PlayerState.derived (novo).
- Handler kPFullStats (139): lê primários+vitais+34 derived na ordem da struct
  DerivedStats (attributes.go); inteiros via ReadU32+cast (codec não tem ReadI32).
- Painel Derived Stats: confirmed vem do servidor (com gear); preview continua local.
- Handlers órfãos removidos: kPPrimaryStatsUpdate (135), kPStatPointsUpdate (134)
  — servidor não manda mais (SYNC-1). kPStatUpdate (22) FICA (HP leve).
- Primários unificados: PlayerState.primary substitui primary_strength/dexterity/
  intelligence/wisdom/perception nos 2 escritores (kPStartGame, kPFullStats) e
  2 leitores (inventory.cpp DrawStatRow + painel Derived Stats).
- LIMITAÇÃO: preview local sem gear diverge do confirmed (com gear). Preview
  gear-aware = futuro (mandar item bonuses pro cliente).

## 89. SYNC-3a: primário efetivo (base+bônus) no PFullStats
- actor.EffectivePrimary guardado no recompute (junto do Derived).
- sendFullStats manda base (5×u16) + unspent + efetivo (5×u16) + vitais + derived.
  Cliente mostra o total e o preview parte do efetivo (SYNC-3b).
- Posição no pacote: efetivo após unspent, antes dos vitais.
- PStartGame inalterado (efetivo vem pelo sendFullStats do login).

## 90. SYNC-3b: cliente lê efetivo, primário total, preview condicional
- CORRIGE o desalinhamento de 10 bytes do PFullStats (SYNC-3a mandou 5 efetivos
  que o cliente não lia → derived corrompidos). Handler agora lê o efetivo
  (offset 12-21), alinhamento restaurado.
- Vitais lidos com static_cast<int32_t>(ReadU32) (sender usa Int32).
- DrawStatRow mostra o total: "DEX: 15 (+10)"; preview de pontos parte do efetivo.
- Preview dos derivados só aparece com HasPreviewChanges() (antes aparecia sempre
  por causa da divergência confirmed-com-gear vs preview-sem-gear).
- LIMITAÇÃO mantida: preview dos derivados não inclui bônus derivados diretos nem
  weapon/armor (só aparece ao distribuir, mostra a direção).

## 92. CooldownSpeedPct ligado ao cooldown de ability
- effectiveCooldownMs reestruturado: mastery (player/damage/level>1) e
  CooldownSpeedPct (todas abilities, player+NPC) compõem multiplicativo. Piso 0.1
  depois do produto.
- CooldownSpeedPct vem de WIS (cap 30%). Antes órfão.
- Equivalência: cdSpeed=0 → comportamento idêntico ao anterior.
- Órfão removido da lista (CooldownSpeedPct agora consumido).

## 93. MovementSpeedMult ligado (cliente + servidor, player)
- Cliente: PlayerController multiplica a velocidade por player.derived.
  MovementSpeedMult (guard <=0→1.0).
- Servidor: handleStandardUpdate escala o maxAllowedStep pelo mult do actor (não
  rejeita player rápido legítimo; previne bug de item/buff futuro estourar o teto).
- MovementSpeedMult vem de DEX (cap +30%). Antes órfão.
- NPC FORA (add-on futuro: multiplicar npcMoveSpeed pelo mult nos 4 pontos do AI).
- Órfão removido da lista.

## 94. Balanceamento de MovementSpeedMult/CooldownSpeedPct adiado (tuning futuro)
- Os ligáveis (CB MovementSpeedMult de DEX, CooldownSpeedPct de WIS) estão
  FUNCIONANDO, mas os valores das constantes são placeholders:
  - moveSpeedPerDEX=0.002, cap 1.3 (DEX 40 → só +8%)
  - cooldownSpdPerWIS=0.002, cap 0.30
- Calibração adiada de propósito pro tuning de PvP. Cuidado específico:
  velocidade de movimento afeta o equilíbrio melee×ranged (ranged rápido demais
  nunca é alcançado pelo melee) — manter o ganho de velocidade CONTIDO é uma
  decisão de balance, não um bug. Não "fortalecer" sem considerar PvP.
- Ajustar quando o jogo estiver mais completo (abilities por dimensão, mais
  arquétipos testáveis).
