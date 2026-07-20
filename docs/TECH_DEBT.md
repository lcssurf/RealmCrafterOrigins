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

## 12. Per-actor material SSBO entries nunca são liberados

**Estado:** `Actor::OverrideMaterial(mm)` chama `mm->RegisterFromHandles("actor_override:<addr>#<i>", ...)`.
O entry é atualizado in-place se chamado novamente no mesmo ator, mas **nunca removido** de
`MaterialManager` quando o Actor é destruído (`Destroy()` limpa `mesh_material_overrides_` mas
não notifica o manager).

**Consequências:**
- SSBO cresce monotonicamente enquanto actors com override forem criados e destruídos.
- No GUE preview (um ator por preview, recriado ao trocar de actor def), o leak é 1 entry × 2
  meshes por troca de actor def — na prática desprezível.
- No client (atores em `unordered_map<uint32_t, Actor*>`, longos mapa de vida), o leak é
  1 slot por ator que teve override, nunca compactado.

**Plano proposto:**
1. Adicionar `void MaterialManager::FreeSlotsByPrefix(const std::string& prefix)` que remove
   todas as entries cujo nome começa com prefix da `insertionOrder_` e `nameToIndex_`.
2. `Actor::Destroy()` chama `mm_->FreeSlotsByPrefix("actor_override:" + addr)` se `mm_` for
   armazenado como membro (requer guardar ponteiro no actor).
3. Alternativa mais simples: substituir o key scheme por um `uint32_t actor_id` global
   (incrementado por Init) e permitir reset do manager em cenários de alta rotatividade.

**Quando atacar:** quando houver cenário de alta rotatividade (dungeon instances, etc).
Por ora o leak é aceitável.

## 13. Material por-submesh (por parte do modelo)

**Estado:** `actor_def_submesh_materials` guarda um material por `ai_material_name` por slot de
actor def. `BuildAppearance` lê dela com fallback para `model.MaterialMap` (actor defs sem
per-parte não regridem). Render via `Actor::OverrideMaterialsByName` usa `mesh_material_overrides_`
seletivo — partes sem override ficam com o `material_idx` do modelo.

**Tech debt herdado da entrada #12:** entradas SSBO per-actor também não são liberadas aqui.
Cada parte com override = uma entrada `"actor_override:<id>#<i>"` no `MaterialManager`, nunca
removida na destruição do actor.

**Limitação zone_renderer:** `ApplyMaterialsByName` (zona/cliente) ainda escreve no modelo
compartilhado (`material_idx`). Dois actor defs diferentes apontando para o mesmo arquivo de
modelo com per-submesh distintos se sobreporiam. Fixing isso requer `OverrideMaterialsByName`
com per-actor entries no zone_renderer (análogo ao que existe para o preview do GUE).

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
- **Corrigido (cliente)**: o timer de auto-attack do cliente (`main.cpp`, bloco
  "Auto-attack" perto do fim do loop principal) ignorava `AttackSpeedMult`
  inteiramente — usava um `kAutoAttackInterval` fixo de 0.85s, mesmo o
  servidor já modulando o delay real por DEX (acima). Personagens com DEX
  investido tinham direito a atacar mais rápido no servidor, mas o cliente só
  reenviava no ritmo fixo, gargalando o DPS real no timing de reenvio.
  Agora o cliente calcula `effective_interval_sec = (kCombatDelayMs/1000) /
  AttackSpeedMult` — MESMA fórmula do servidor — com `kCombatDelayMs = 800.0f`
  espelhando `combat.go:7` (mesmo contrato "must stay in sync" já usado em
  `derived_stats.h`). Não mexe no botão de ataque explícito (já disparava na
  hora, independente do timer).

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

## 95. C3a: campo de dimensão na ability (fundação)
- ability_templates.dimension (TEXT, ''=inherit). Migração addColumnIfMissing.
- Carga: Row→Template (db.go, main.go), struct AbilityTemplate.Dimension.
- helper resolveAbilityDimension (override ou herda BasicAttackDim) — DEFINIDO,
  ainda não usado (C3b liga no specialAttackDamage).
- GUE: combo dimension (inherit/melee/ranged/magic).
- Lua NÃO afetado (sistema de spell legado separado).
- Comportamento INALTERADO (runtime ainda usa melee fixo até o C3b).

## 96. C3b: abilities roteiam por dimensão + SkillDamageBoost/BonusDamageFlat
- specialAttackDamage usa resolveAbilityDimension + selectDimensionStats. Trocados:
  defesa (→stats.DefenseValue), crit sem CritPolicy (→stats.CritValue), fallback
  dmg (→stats.DmgMin/Max). Branch CritPolicy intocado.
- SkillDamageBoostPct (órfão→ligado): multiplicador ANTES do crit (decisão dev:
  amplifica o crit). BonusDamageFlat (órfão→ligado): no base, antes da defesa.
- Cobre todos os caminhos (specialAttackDamage é chamada única).
- Órfãos removidos da lista: SkillDamageBoostPct, BonusDamageFlat (no caminho de
  ability; BonusDamageFlat no ataque básico ainda pendente — TECH_DEBT separado).

NOTA: BonusDamageFlat no ATAQUE BÁSICO (ProcessAttack) ainda não ligado — fica
como item futuro (só o caminho de ability foi feito aqui).

## 97. SkillDamageBoostPct favorece builds mágicas (revisar no tuning)
- SkillDamageBoostPct vem de INT/WIS — builds físicas (rogue DEX/STR) ganham
  pouco, mesmo em abilities físicas. Conceitualmente estranho (dano de skill
  física escalar com INT).
- O DamageStatScale (JSON por ability) já permite escala por-stat correta por
  ability — é a ferramenta certa pra "como a ability escala". O SkillDamageBoostPct
  global (INT/WIS) é redundante e enviesado.
- Decisão adiada pro tuning: reavaliar se o SkillDamageBoostPct deve ser revertido
  (deixar o JSON fazer), mudar de fonte, ou virar explicitamente "magic skill
  boost". Não mexer agora — funciona, só é enviesado.

## 98. C-reações: dodge/guard nas abilities (antes só parry)
- resolveActorWindup agora respeita as 3 reações (dodge/parry/guard), como o
  ataque básico. Dodge (evita 100%) antes do parry; guard (reduz pra 40% + consome
  stamina) entre specialAttackDamage e ApplyDamage.
- Universal: sem condicional por dimensão (decisão dev; restrições temáticas tipo
  "parry não vale vs magia" ficam pra tuning futuro).
- Reutiliza eventos genéricos (HitDodged=8, HitGuarded=9, GuardEnded=4).
- FECHA o arco combate por dimensões (C0→C-reações).

NOTA FUTURA: condicionar reações por dimensão (ex parry/guard só físico) é um
ajuste de balance disponível (a dimensão está acessível via resolveAbilityDimension).

## 99. StaminaMax escala com nível (era fixo 100)
- StaminaMax = staminaBase(100) + level*staminaPerLevel(5), calculado no
  RecomputeDerivedStats (só nível, sem atributo — equilíbrio defensivo previsível).
- NÃO entra no DerivedStats struct (evita desalinhar o PFullStats; já trafega
  como vital spMax). Setado em actor.StaminaMax no recompute, como HealthMax/EnergyMax.
- client.go hardcoded 100 substituído. Level-up recalcula (via recompute existente).
- Corrige a assimetria: HP/MP escalavam, stamina não.
- Valores placeholder (base 100, +5/nível) — tuning futuro.
- NPC StaminaMax: world.go já chama RecomputeDerivedStats(npc, nil) no spawn, então
  npc.StaminaMax também passa a escalar com o nível do NPC. npc.Stamina (atual)
  continua não sendo inicializado (igual ao comportamento anterior) — NPCs não
  usam dodge/guard hoje, então não há impacto prático.

## 100. Fase A / Commit 1: vocabulário de animação em árvore (fundação)
- Tabela anim_vocabulary(id, name, parent_id). Árvore via parent_id (0=raiz). A
  árvore É o fallback (filho→pai). Seed com a árvore padrão + ações já em uso.
- Carga no server (LoadAnimVocabulary → SetAnimVocabulary), getter
  AnimFallbackParent pronto — mas NÃO usado no runtime ainda (commit do fallback).
- GUE: sub-tab Animation Vocabulary em Settings, árvore editável (add/rename/delete).
- String validada (Opção 2): media_actor_anims e ability_templates INTOCADOS.
- Próximos commits da Fase A: (2) atores selecionam do vocab, (3) fallback no
  runtime, (4) abilities selecionam do vocab.

## 101. Fase A / Commit 2: atores selecionam ação do vocabulário (combo estrito)
- media.cpp: editor de anims do ator usa SearchableComboString(anim_vocab_names_)
  na coluna Action, em vez de InputText livre. Estrito (só escolhe do vocab).
- Preserva actions legadas fora do vocab (aviso "(not in vocabulary)" passivo —
  não sobrescreve; troca é ação consciente do dev).
- MediaTab carrega anim_vocabulary independente (LoadAnimVocabNames em FetchAll).
- Server intocado (Opção 2: action continua string).

## 102. Fase A / Commit 3: fallback em cascata no runtime (o vocabulário AGE)
- BroadcastAnimate agora sobe a árvore do vocabulário (AnimFallbackParent) quando
  o ator não tem o slot pedido. CastForward→Cast→raiz. Guard maxFallbackDepth=8
  contra ciclo de config.
- Supressão de locomoção intocada (usa actionName original). action_id enviado vem
  do slot resolvido.
- Logs: "resolved via fallback to X" (sucesso via pai) / "missing_action_binding
  (fallback exhausted)" (falhou até a raiz).
- O vocabulário (Commits 1-2) agora REGE qual animação cada ator toca.

## 103. Fase A / Commit 4: abilities selecionam ação do vocabulário (FECHA Fase A)
- combat_abilities.cpp: ActionWindup/Impact/Recover usam SearchableComboString
  (anim_vocab_names_), estrito, em vez de InputText livre.
- Permite VAZIO via "(none)" (herança via resolveStageAction preservada).
- Preserva legado fora do vocab (aviso passivo). Carga independente do vocab.
- Server intocado (campos continuam string).
- ✅ FASE A COMPLETA: vocabulário em árvore (C1) + atores selecionam (C2) +
  fallback em cascata (C3) + abilities selecionam (C4). Pipeline de animação
  100% por seleção, com fallback hierárquico.

## 104. Hardcode "mixamorig:Hips" no ComputeBones é auto-condicional (não-travante)
- model.cpp:510/600/621 + Load:1123: correção de retargeting Mixamo (delta do Hips
  vs Y Bot 1.98m). Auto-condicional: só dispara se o nó "mixamorig:Hips" existe.
- Modelos custom (ex: Hunger) caem no else (bind-pose, só rotação anima) — correto
  pra animação in-place (RCO não usa root motion). Mixamo e custom convivem.
- NÃO é bloqueio pra modelos não-Mixamo. Único limite: root motion seria descartado
  (irrelevante pra RCO in-place).
- Saída futura SE precisar (modelo custom com retarget cross-rig): membro
  root_bone_name_ (default "mixamorig:Hips") + setter, troca as 4 ocorrências.
  Zero quebra. Não necessário agora.

## 105. Arco B / B1 COMPLETA: bone world transform exposta + fix SubmitWithMatrix
- Model: bone_world_transforms_ (fatoração não-quebradora em Compute*Bones) +
  GetBoneWorldTransform(name, out). Actor: delega.
- BUG CORRIGIDO: SubmitWithMatrix não setava tex_albedo (mesh sem textura) — agora
  com paridade total de material com Submit. Essencial pra B5 (attachment usa
  SubmitWithMatrix).
- bone_world_transforms_ vive no Model compartilhado (ModelCache). Dois atores
  com o mesmo model path na mesma frame sobrescrevem um ao outro. Aceitável
  enquanto só o player usa; B4 move isso pro Actor se necessário.
- Validada: cubo-marcador grudou na mão e seguiu a animação (andaime removido).
- Fundação do attachment pronta. Próximo: B2 (vocabulário de sockets).

## 107. Arco B / B3a COMPLETA: socket mapping do actor def (dados + GUE + wire)
- Tabela actor_def_sockets (actor_def_id, socket_name TEXT, bone_name TEXT,
  offset_pos x/y/z, offset_rot x/y/z euler graus, offset_scale uniforme).
  migrateV48, dual sqlite/postgres, sem seed — usuário cadastra no GUE.
- world.SocketBinding{SocketName, BoneName, OffsetPos [3]float32, OffsetRot
  [3]float32, OffsetScale float32}. Appearance.Sockets []SocketBinding.
- appearance.go: BuildAppearanceFromDef carrega actor_def_sockets e popula
  Appearance.Sockets (via LoadActorDefSockets).
- frame.go: appendSockets helper; NewActorPayload adiciona sockets ao FINAL
  (aditivo). AppearanceBytes NÃO inclui sockets — usa AppearanceSockets()
  separado (client.go appenda após extras/primary stats). Nil: socket_count=0.
- AJUSTE (fix): PStartGame tinha sockets ANTES dos primary stats com guard
  !r.Done() frágil (sempre true). Movidos pro FINAL: server usa AppearanceSockets
  após extras.Bytes(); cliente parseia sockets após primary stats. Alinha com
  PNewActor. Guard agora genuíno: servidor antigo → r.Done()=true, cliente pula.
- Model::BoneNames() público permanente (sorted alphabetical via bone_map_
  iteration). Actor::BoneNames() delega.
- GUE media.cpp: seção "Sockets" em DrawActorDefs (após Animations) — socket
  combo (socket_vocabulary) + bone combo (preview_.GetModel().BoneNames()) +
  DragFloat3 pos/rot + DragFloat scale + Save/Del por linha + "+ Add Socket".
  EnsureTables cria socket_vocabulary + actor_def_sockets localmente.
  SaveActorDefSocket/DeleteActorDefSocket CRUD. LoadSocketVocabNames.
- Cliente: WorldSocket struct, player_sockets + WorldActorEntry::sockets.
  PNewActor: sockets após y_offset (guard !r.Done()); PStartGame: sockets após
  primary stats (último campo, guard genuíno). Dados guardados, NÃO usados até B5.
- Rotação euler graus, escala uniforme, socket/bone como TEXT (não FK).
- Próximo: B3b (preview 3D + sliders ao vivo), B4 (item→socket), B5 (render).

## 106. Arco B / B2 COMPLETA: vocabulário de sockets (lista flat configurável via GUE)
- server: migrateV47 — socket_vocabulary(id, name) sem parent_id (flat, sem
  fallback); seed com 8 defaults: WeaponHand, OffHand, Head, Back, Hip,
  Shoulder_R, Shoulder_L, Chest. LoadSocketVocabulary exposta mas não consumida
  pelo servidor ainda (B3 faz o bind socket→bone por actor_def).
- schema.sql: tabela postgres equivalente adicionada.
- GUE: sub-tab "Sockets" em Settings (ao lado de "Animation Vocabulary") com
  lista plana add/rename/delete + EnsureTables para sqlite local.
- Próximo: B3 (actor_def mapeia socket→bone+offset).

## 110. GUE: link tabela↔preview de animação (dropdown de ações + scrubber + Set Start/End)

- **Dropdown de ações** (`preview_viewport.cpp`): substituiu o dropdown de clips brutos
  ("mixamo.com") por um dropdown que lista as AÇÕES configuradas do actor def (Idle,
  Walk, Attack…) passadas via `PreviewViewport::SetAnimActions()`.
- **Resolução ação→clip** (`PlayActionEntry_`): `source_path==""` → `PlayAnim(clip_override
  ?: action, loop)`. `source_path!=""` → `LoadAnim(resolved, clip_name)` (usa o cache de
  parse FBX em `AppendAnimationsFrom` — O(1) se já carregado) depois `PlayAnim(clip_name)`.
- **Scrubber** (`preview_viewport.cpp`): com `PlayActionEntry_` setando `cur_name_` para o
  nome real do clip, o loop `ClipName(i)==cur_name_` agora encontra `scrub_dur` e o scrubber
  aparece.
- **Botões Set Start / Set End**: escrevem o frame atual do scrubber diretamente em
  `ActorAnimMap.start_frame/end_frame` via ponteiros não-proprietários (`p_start`, `p_end`
  em `AnimActionEntry`). Sem auto-save — o dev salva via o botão Save da linha na tabela.
- **Fix `Actor::Init`** (`actor.cpp`): `PlayAnim("Idle")` hardcoded substituído por
  `PlayAnim(ClipName(0))` — `cur_name_` passa a ser honesto (nome real do clip). O jogo
  toca o mesmo índice 0 que antes; só o nome interno muda.
- **Acoplamento mínimo**: `AnimActionEntry` (struct leve em `preview_viewport.h`) carrega
  apenas o que o preview precisa. Tabela de animações fica intacta no lugar atual.

## 109. Feature: animação em timeline única (recorte por frames)

- **Renderer** (`anim_controller.cpp`): `Submit` agora passa
  `active_.time_sec + start_frame/fps` ao `SubmitAs`/`SubmitBlended`.
  Clips sem recorte (`start_frame=0`) recebem offset zero — comportamento idêntico ao
  anterior, nenhum modelo Mixamo atual é afetado.
- **GUE** (`media.cpp` `DrawActorDefs`): tabela de animações ganhou colunas
  "Start Fr" / "End Fr" (InputInt). `SaveAnimMap` persiste esses campos em
  `media_anim_clips.start_frame` / `end_frame`.
- **Preview scrubber** (`preview_viewport.cpp`): abaixo dos controles de clip,
  slider de tempo (pausa ao arrastar) + display "Frame N  (X fps)" para achar
  os pontos de corte visualmente.
- **`AnimClip.fps`** (`model.h`, `model.cpp`): campo adicionado ao struct e
  populado em `LoadAnimations` e `AppendAnimationsFrom` (tps do Assimp). Exposto
  via `Model::ClipFps(i)`.
- **Infra de dados/wire já existia**: `media_anim_clips.start_frame/end_frame/fps`,
  protocolo server→client (`frame.go`), `AnimBinding` no cliente. Só faltava o
  offset no render + a UI no GUE.
- Botões "Set Start"/"Set End" no preview não implementados — o preview não tem
  acesso ao row selecionado em DrawActorDefs (seria acoplamento cross-tab). O dev
  lê o frame no display e digita nos campos.

## 108. Project Manager (rco_project_manager) — Gerenciador de Projetos
- Renomeado de "launcher" → "project_manager" (tools/project_manager/,
  rco_project_manager.exe). "launcher" fica reservado para feature futura.
- Novo exe C++/ImGui (mesmo vcpkg/CMake do GUE, boilerplate GLFW+glad+ImGui).
  Saída: dist/tools/rco_project_manager.exe.
- ESCOPO: gerenciador de projetos apenas. NÃO inicia processos — a pessoa abre
  server.exe/rco_gue.exe/rco_client.exe diretamente na pasta do projeto.
- Cada projeto = cópia completa de dist/ em <projects_dir>/<nome>/.
  Criar = copiar dist/ sem *.db / *.log / *.py / thumbcache (servidor self-seeda
  o .db no primeiro start).
- Pasta de projetos: default dist/tools/projects/ (ao lado do exe — normal
  rebuilds não apagam subpastas, só sobrescrevem o exe). Configurável via
  pm_config.txt (texto, 1 linha com path absoluto). Botão "Change..." na UI abre
  SHBrowseForFolderW. Persiste entre execuções. Se o path salvo não existir mais,
  cai no default + avisa na UI (não crasha).
- GUE de cada projeto resolve seu .db via "../server/rco.db" sem argumento
  (SetCwdToExeDir ancora cwd em <projeto>/tools/).
- Renomear a pasta do projeto é seguro: todos os paths internos são relativos ao
  exe, o .db não armazena paths absolutos da pasta raiz.
- Ações de pasta: Open Folder (ShellExecuteW explore), Rename (fs::rename),
  Delete (fs::remove_all com confirmação obrigatória).
- Links: rco_renderer (glad), glfw, imgui, shell32, ole32 (COM para folder picker).

## 111. GUE: preview aplica recorte start_frame/end_frame e exibe scrubber corretamente

- **B1** (`preview_viewport.h`): `AnimActionEntry` ganhou `start_frame` e `end_frame`.
  `media.cpp` copia os valores de `anim_map[ai]` a cada frame, então edições na tabela
  chegam ao preview sem exigir re-seleção no dropdown.
- **A1** (`preview_viewport.cpp`): `PlayActionEntry_` resolve o índice real do clip
  (exact match → prefix case-insensitive → fallback 0, espelhando `Actor::FindClip`).
  `DrawImGui` usa `cur_clip_idx_` em vez de `ClipName(i) == cur_name_` para obter
  `scrub_dur`/`scrub_fps` — resolve o bug onde clips Mixamo têm nome diferente da ação.
- **A2** (`preview_viewport.cpp`): `controls_h` de 28 → 56 px (duas linhas); scrubber
  não é mais cortado pelo child window de visualização.
- **B2/B3** (`preview_viewport.cpp`): `PlayActionEntry_` calcula `clip_start_sec_` /
  `clip_end_sec_` em segundos. `RenderToEngineFrame_` avança `anim_t_` e faz
  `fmod(local, range)` para loopar dentro do intervalo; `SubmitAs` recebe `anim_t_`
  que já está relocalizado (sem offset adicional, pois `anim_t_` parte de `clip_start_sec_`).
- **B4** (`DrawImGui`, chamado cada frame): relê `ae.start_frame`/`ae.end_frame` do
  entry ativo e atualiza `clip_start_sec_`/`clip_end_sec_` ao vivo; reposiciona
  `anim_t_` para `clip_start_sec_` se o recorte foi apertado e o cursor ficou de fora.
- Sem quebra em atores sem recorte: `start_frame=0, end_frame=-1` → range = total clip.

## 115. Black Cutout — implementado (Fase 1 + Fase 2)

Fase 1: flag `black_cutout` no **material** → `albedoFactor.w` no SSBO → discard em
`gBufferBindless.fs` quando `lum < u_blackCutoutThreshold`. Limiar global via slider no GUE Settings,
persistido em `game_settings.black_cutout_threshold`.

Fase 2: flag `black_cutout` no **modelo** — implementada.
- `media_models.black_cutout` (DB), `MediaModel::black_cutout`, checkbox no editor de modelos.
- `Model::ApplyBlackCutout(bool, MaterialManager*)` re-registra todas as submeshes com
  `albedoFactor.w` atualizado; cobre statics no deferred gBuffer (via SSBO) sem shader extra.
- `preview_static.fs` recebe `u_blackCutout` + `u_blackCutoutThreshold`; `PreviewViewport`
  sincroniza via `SetStaticBlackCutout`.

**Pendência restante:**

1. **Derivação automática do canal alpha (B3D fx=16)**: quando Assimp reporta
   `aiTextureType_OPACITY == albedo` (B3D `fx & 16` = masked), derivar a opacidade
   automaticamente do canal alpha da textura albedo, sem exigir arquivo separado.
   Log `[alpha-flag] assimp_opacity=equals_albedo` já identifica esses casos.

## 113. GUE: reorganização da importação de texturas/materiais

- **Dois fluxos distintos**:
  - `"Import PBR Material (folder)..."` → `assets/materials/<subdir>/` (nova pasta). Escaneia
    a pasta por grupos PBR (Albedo+Normal+ORM), preserva estrutura interna de subpastas via
    `fs::relative(file_parent, source_root)`. Registra no DB como `MediaMaterial` completo.
  - `"Import Texture(s)..."` (novo, na aba Materials) → `assets/textures/<stem>/`. Aceita N
    imagens soltas; cria `MediaMaterial` com apenas `albedo_path` preenchido. Sem ORM, sem
    normal. Label clara: "simple texture (no normal/ORM)".
- **Modal de aviso** quando `ScanTextureFolder` retorna 0 grupos: popup `"PBR Scan Failed"`
  lista as keywords esperadas nos nomes de arquivo e sugere usar `"Import Texture(s)..."`.
- **Botões do topo** renomeados para tornar claro que são model-only:
  - `"Import files..."` → `"Import Model Files..."` (só modelos; extensões de imagem removidas
    do filtro do picker).
  - `"Import folder..."` → `"Import Models (folder)..."`.
- **`texture_importer.cpp:284`**: `"assets/textures/"` → `"assets/materials/"` + campo
  `source_root` em `TextureImportOptions` para preservação de estrutura.

## 112. GUE: redimensionamentos na aba Actor Defs + persistência de UI

- **Splitter centro↔preview** (`media.cpp`): `ad_preview_w_` virou membro de `MediaTab`
  (não local — local resetaria pra 40% a cada frame). `InvisibleButton("##vsplit", {8, -1})`
  entre `##ad_edit` e `##ad_preview`; `MouseDelta.x` ajusta `ad_preview_w_`; clamp
  `[280, total_w - 200]`. O `##ad_preview` usa `{0,0}` (fill), então o aspect ratio do
  viewport 3D acompanha automaticamente — o debounce de resize já existente absorve a
  mudança sem distorcer. `IsItemDeactivatedAfterEdit` escreve `gue_prefs.txt`.
- **Altura ajustável de Animations** (`media.cpp:2786`): `ad_anim_h_` membro, init 300 px.
  `InvisibleButton("##anim_hsplit", {-1, 6})` logo após o `EndChild("##anims")`;
  `MouseDelta.y` ajusta; clamp `[120, 800]`. `IsItemDeactivatedAfterEdit` escreve prefs.
- **Colunas resizable** (`media.cpp:2788`): `SizingStretchProp` trocado por `SizingFixedFit`
  (evita conflito com `Resizable`) + `ImGuiTableFlags_Resizable` adicionado. Larguras
  persistem via `gue.ini` automaticamente por ID `"##anim_tbl"` — zero código extra.
- **Persistência híbrida**:
  - `gue.ini` reabilitado (`main.cpp:117`). `##gue_root` e `"Database path"` blindados
    com `NoSavedSettings` — apenas as tabelas persistem colunas; janelas de layout fixo
    ficam imunes.
  - `gue_prefs.txt` (`dist/tools/`) lido uma vez no primeiro `DrawActorDefs` (via
    `LoadPrefs_`), escrito só ao soltar os splitters (`SavePrefs_`). Formato: `key float`,
    duas linhas. Clamp na leitura: `preview_w ∈ [280, 4000]`, `anim_h ∈ [120, 800]`.
    Arquivo corrompido é ignorado silenciosamente (defaults prevalecem).
- Aba Models intocada (`##mdl_*`). Nenhuma outra aba afetada.

## 114. GUE: exclusão em massa por pasta (models + materials)

- **Fluxo**: botão direito num nó de pasta na árvore → "Delete Folder" →
  modal lista assets, separa deletáveis vs bloqueados (em uso). Só apaga não-usados.
- **Referências checadas para models**: `media_actor_meshes.model_id` (actor defs),
  `game_settings.default_drop_model_id` (drop padrão, ligado por STRING do id),
  `item_templates.model_path` (itens, ligado por `file_path` STRING — não por id!).
  Também limpa `media_model_shapes` órfãs (shapes ficam sem pai se não forem apagadas junto).
- **Referências checadas para materials**: `media_actor_meshes.material_id` (única referência).
- **Apaga DB + arquivo físico** com guard de containment: `fs::canonical` verifica que o
  caminho resolvido está estritamente sob `dist/client/assets/` antes de `fs::remove`.
  Arquivo por arquivo (não `remove_all` cego) — respeita bloqueio parcial.
- **Arquivos envolvidos**: `media.cpp` (`DrawFolderList` + `OpenFolderDeleteModal_` +
  `DrawFolderDeleteModal_` + helpers `ModelUsages`/`MaterialUsages`/`SafeRemoveAsset`),
  `media.h` (`FolderDeleteEntry` + 2 membros + 2 declarações).
- **Call sites clips/actor_defs**: intocados (passam `on_folder_context = nullptr`).

## 115. Spawn Fase 2: safe zones + respawn contextual (pendente)

**Fase 1 implementada** (Fase 2 ainda não):
- Tabela `player_spawns` criada; `media_actor_defs.initial_spawn_id` adicionado.
- GUE Zone editor: Player Spawn como objeto plaçável (padrão zone_scenery).
- GUE Actor Def: combo "Initial Spawn" visível só quando `is_playable=true`.
- `CreateCharacter` usa o spawn configurado; fallback seguro para (512,0,512,'Starter Zone').
- Bug do respawn pra (0,0,0) corrigido: `handleStartGame` agora seta `actor.SpawnX/Y/Z/Yaw`.

**O que falta (Fase 2):**
- **Tabela `safe_zones`**: rect/volume 2D ou AABB por área — define onde o player pode
  respawnar com segurança (longe de mobs, fora de dungeons, etc.).
- **Tracking de `last_safe_pos`**: servidor persiste a última posição do player dentro de
  uma safe zone (coluna em `characters` ou tabela separada). Atualizado periodicamente
  enquanto o player está dentro de uma safe zone.
- **Respawn contextual**: `handleRespawnPlayer` escolhe ponto de respawn na seguinte ordem:
  1. `last_safe_pos` (se dentro do mesmo mapa e válido)
  2. `actor.SpawnX/Y/Z` (initial spawn configurado no actor def)
  3. Fallback hardcode (512, 0, 512, 'Starter Zone')
- **GUE Zone editor**: Safe Zone como objeto plaçável (AABB ou raio), seguindo o mesmo
  padrão de Player Spawn implementado na Fase 1.
- **Arquivos a tocar**: `zone_scene.h`, `zone_scene.cpp`, `zone_renderer.h/.cpp`,
  `zones.h`, `zones.cpp`, `zones_panels.cpp`, `zones_sidebar.cpp`, `zones_viewport.cpp`,
  `server/internal/db/db.go` (migrateV52+), `server/internal/net/client.go`.

## 116. Point Lights Fase 2: luz dinâmica de skill/FX + sombra de point light (pendente)

**Fase 1 implementada** (estática, torches/lanternas):
- Pipeline de point light já existia pronto e nunca era chamado (`Pipeline::AddPointLight`,
  `localLightsPass_`, shaders `lightGeom.vs`/`gPhongManyLocal.fs`) — Fase 1 só ligou o fio,
  não mudou nada do shader/SSBO.
- Tabela `zone_lights` (schema em `tools/gue/src/zone_scene.cpp` `EnsureTables`).
- GUE Zone editor: Point Light como objeto plaçável (padrão zone_scenery/zone_emitters),
  `ZLight` em `zone_scene.h`, `kSelLight`/`kModeLight`, marcador esfera colorida + anel de
  raio no viewport (`zone_renderer.cpp`).
- Servidor: `LoadZoneLights` (`db.go`), `world.Light`/`Area.Lights` (`area.go`),
  `LightsPayload` (`frame.go`), pacote `PZoneLights=140`, enviado em `sendZoneLights`
  (mesmos 2 call sites de `sendWorldObjects`: login e troca de área).
- Cliente: `rco::renderer::LightManager` (`shared/renderer/include/rco/renderer/
  light_manager.h` + `src/light_manager.cpp`) — guarda a lista da área atual e resubmete
  via `Pipeline::AddPointLight` TODO FRAME (o pipeline limpa `localLights_` em `Begin()`,
  não é "adiciona uma vez"). `light_manager.Clear()` chamado em `kPChangeArea`,
  `kPKickedPlayer` e `OnLogout`, espelhando `world_static_objects.clear()`.

**O que falta (Fase 2):**
- **Luz dinâmica de skill/FX**: anexar cor/raio/intensidade a
  `ParticleSystem::Emitter` (`shared/renderer/include/rco/renderer/particles.h`), que já
  tem posição + `startTime`/`duration` por instância ativa. Em `ParticleSystem::Update()`,
  chamar `Pipeline::AddPointLight()` pra cada emitter vivo com luz configurada — reaproveita
  100% do tracking de posição/vida útil existente, sem estrutura de dados nova.
  Precisa adicionar campos de luz a `FXParams` (particles.h) e ao pacote `kPFXCatalog`
  (main.cpp) se a cor/raio da luz do FX for autorada pelo servidor (não hardcoded).
- **Sombra de point light**: hoje só a luz direcional (sun) tem sombra (`shadowPass_`,
  PCF em `gPhongGlobal.fs`). Point lights renderizam sem sombra nenhuma — sombra
  omnidirecional (cubemap) ou aproximação é trabalho novo, não reaproveita a shadow pass
  direcional existente além do padrão geral de bias/PCF como referência.
- **Nice-to-have não crítico**: `Pipeline::localLightsPass_` recria o SSBO
  (`std::make_unique<StaticBuffer>`) do zero todo frame em vez de atualizar um buffer
  persistente — funciona, mas é uma alocação de GPU buffer nova por frame. Só vale otimizar
  se o número de luzes simultâneas crescer a ponto de aparecer em profiling.

**Quando atacar:** quando skills com efeito visual luminoso (bola de fogo, raio, etc.)
virarem prioridade, ou se o teste da Fase 1 revelar que point lights sem sombra ficam
visualmente estranhos em interiores.
**Estimativa:** luz dinâmica ~1 dia (reaproveita infra); sombra de point light ~3-5 dias
(trabalho novo de renderização).

## 117. Água — reflexo aproximado (IBL+Fresnel) feito, reflexo real (planar/SSR) + mecânica de nadar pendentes

**Fase 0 implementada** (plano estático texturizado):
- `zone_water` já existia (schema + struct `ZWater`) mas sem UI pra `tex_path`/`tex_scale`
  e sem consumidor real — o GUE desenhava uma caixa de cor sólida (`kPrimVS`/`kPrimFS`,
  sem UV) e o cliente do jogo nunca recebia água nenhuma (zero pacote, zero shader).
- GUE: `DrawPanelWater` (`zones_panels.cpp`) ganhou combo de textura (reaproveita
  `media->Materials()`, mesmo padrão do combo Model/Material da Scenery) + slider de
  Tex Scale. `zone_renderer.cpp` ganhou shader real (`InitWaterShader`/`DrawWater`),
  lendo `dist/client/shaders/water.vs`/`water.fs` do disco (UV real + `sampler2D`,
  diferente do `kPrimVS`/`kPrimFS` que continua existindo p/ outros overlays de debug).
- Shaders `water.vs`/`water.fs` (`dist/client/shaders/`): quad único, UV tileável por
  `tex_scale`, tint de cor + opacity — sem onda, sem profundidade, sem reflexo.
- Servidor: `LoadZoneWater` (`db.go`), `world.Water`/`Area.Water` (`area.go`),
  `WaterPayload` (`frame.go`), pacote `PZoneWater=141`, enviado em `sendZoneWater`
  (mesmos 2 call sites de `sendZoneLights`: login e troca de área).
- Cliente: `rco::renderer::WaterManager` (`shared/renderer/include/rco/renderer/
  water_manager.h` + `src/water_manager.cpp`) — desenha um quad texturizado por
  instância dentro do forward pass do `Pipeline::End()`, no mesmo callback de
  `particles.Render` (`main.cpp`, perto de `pipeline->End()`). Shader `"water"`
  registrado em `compile_shaders.cpp` junto dos demais. `water_manager.Clear()`
  chamado em `kPChangeArea`, `kPKickedPlayer` e `OnLogout`, espelhando
  `light_manager.Clear()`.
- **Primeira vez que água aparece no jogo de verdade** — antes desta fase, água era
  100% preview do editor GUE.
- **Iluminação sun-only (pós-Fase 0)**: `water.fs` nasceu 100% auto-iluminado
  (`texel*tint`, sem nenhum uniform de luz) — água ficava com o mesmo brilho de dia ou
  de noite, sob sol ou sombra. Corrigido: `water.fs` ganhou um termo difuso simples
  usando só o sol direcional (`N=(0,1,0)` fixo — água ainda é plana, sem onda — e
  `L=normalize(-u_sunDir)`, mesma convenção de `gPhongGlobal.fs:196`), com piso mínimo
  0.4 pra não ficar preto total à noite/sombra. `Pipeline` ganhou `SunDirection()`/
  `SunColor()` (getters, `pipeline.h`/`pipeline.cpp`) só pra repassar o `sun_` que já
  existe — não duplica a fonte da verdade. `WaterManager::Render` agora recebe
  `const Pipeline&` e `DrawWater` (GUE) lê `fullPipeline_->SunDirection()/SunColor()`
  diretamente (é membro da mesma classe). **Ainda sem IBL e sem sombra** — água não
  escurece dentro de sombra de árvore/prédio nem reflete a cor ambiente do céu; é
  puramente `NdotL` contra o sol. Ver pendência abaixo.

**Fase 1 implementada, REVISADA — Gerstner waves reais (deslocamento de vértice), não
mais só perturbação de normal:**
- **Histórico**: a primeira versão da Fase 1 só perturbava a normal de shading via
  sum-of-sines (superfície continuava um quad plano de 4 vértices — só a luz "cintilava",
  a geometria não se mexia). Substituída pela técnica padrão da indústria: **Gerstner
  waves** (GPU Gems, "Effective Water Simulation from Physical Models" — mesma técnica
  usada em Dishonored/War Thunder/tutoriais UE4/Unity/Godot), que desloca vértices de
  verdade.
- **Malha**: `BuildWaterQuadVAO` (`tools/gue/src/zone_renderer.cpp`) e `WaterManager::Init`
  (`shared/renderer/src/water_manager.cpp`) trocaram o quad de 4 vértices por uma grade
  NxN (`kWaterGridN=24` → 576 vértices / 1058 triângulos) — só uma grade subdividida
  consegue curvar de verdade; um quad plano não. Índices em `GL_UNSIGNED_SHORT` (576
  cabe folgado). De quebra, corrigido um leak pré-existente no GUE (`ebo` local nunca
  armazenado num membro) — agora `waterEBO_` é membro e é deletado no destrutor.
- **Vertex shader** (`water.vs`): `GerstnerWave(dir, steepness, k, speed, time, p, out
  dDispDx, out dDispDz)` — implementa a fórmula padrão `f=k*(dot(dir,p)-speed*time)`,
  `a=steepness/k`, `disp=(dir.x*a*cos(f), a*sin(f), dir.y*a*cos(f))`, e retorna também as
  derivadas parciais analíticas de `disp` em relação a x/z (regra da cadeia sobre a
  própria fórmula de deslocamento, derivada à mão — não uma fórmula de normal genérica
  "encaixada" por fora). A normal final vem de `normalize(cross(tangentZ, tangentX))`,
  onde `tangentX/Z = (1,0,0)/(0,0,1) + Σ derivadas` — no caso plano (steepness=0) isso
  colapsa exatamente pro antigo `N=(0,1,0)`.
- **Mapeamento dos campos existentes** (nenhum campo novo configurável adicionado):
  `wave_speed` → `speed` (m/s que a crista viaja ao longo de `dir`); `wave_dir` → `dir`
  (normalizado no shader); `wave_scale` → `k` (número de onda angular, rad/unidade-de-
  mundo, usado DIRETO — `wavelength=2*PI/wave_scale`; no default do GUE, 0.35, dá um
  "swell" de ~18 unidades; no máximo do slider, 2.0, dá um chop de ~3 unidades).
  Amplitude é uma constante fixa no shader (~0.12 unidades, "sutil" — mesmo alvo visual
  da versão anterior), com `steepness = clamp(amplitude*k, 0, 0.5)` — o clamp evita
  onda "looping"/auto-interseção em k alto, e é também fisicamente plausível (ondas
  curtas e íngremes arrebentam em vez de crescer). Onda secundária: mesmo padrão do
  sum-of-sines anterior — direção rotacionada 90°+misturada, `k*1.8`, `speed*1.3`,
  metade da amplitude — sem campo configurável novo.
- **Uniforms renomeados**: `u_mvp` (combinado) virou `u_viewProj` (só proj*view) + o
  `u_model` que já existia (da correção de especular) passou a ser usado pra calcular a
  posição de mundo ANTES da câmera — necessário porque a fase da onda é avaliada em
  metros de mundo, e um MVP pré-combinado não dá pra "desfazer" de volta.
- **Difuso + especular Blinn-Phong** (já implementados numa correção anterior) mantidos
  intactos — agora contra a normal Gerstner real, não a perturbação artificial. Resultado
  esperado: o brilho especular "quebra" fisicamente nas cristas conforme a câmera/sol se
  move, em vez de só cintilar no lugar.
- `ZWater`/`WaterEntry`/rede (`wave_speed`/`wave_dir_x`/`wave_dir_z`/`wave_scale`,
  `PZoneWater`) — **inalterados**, os mesmos 4 campos da Fase 1 original, só a semântica
  de `wave_scale` mudou (de "frequência em espaço de UV" pra "número de onda em espaço
  de mundo").

**Sub-fase 2a implementada** (transparência por profundidade, via `gDepth_`):
- `Pipeline::SceneDepthTexture()` (`pipeline.h`/`pipeline.cpp`) — getter novo, mesmo
  padrão de `SunDirection()`/`ViewPos()`, retorna `engine_->gDepth_` (`Pipeline` já é
  `friend class Pipeline` de `Engine`, `engine.h:82`, usado direto em 6+ lugares de
  `pipeline.cpp` — nada novo em termos de acesso, só exposto pra fora da classe).
- `water.fs` ganhou `#include "common.h"` e reusa `WorldPosFromDepth()` (já existente,
  usada por `gPhongManyLocal.fs:37`) pra reconstruir a posição de mundo da cena a partir
  de `gDepth_` — **não** reinventou linearização de depth com near/far. `u_invViewProj`
  é calculado no C++ chamador (`glm::inverse(vp)`, mesmo padrão de todo pass deferred,
  ex. `pipeline.cpp:642`), não um getter novo no `Pipeline`.
- Fórmula: `depthDiff = max(vWorldPos.y - sceneWorldPos.y, 0)`,
  `depthFactor = 1-exp(-depthDiff/depth_fade_distance)`,
  `waterColor = mix(shallow_color, deep_color, depthFactor)`.
- **Composição final decidida**: `waterColor` (o gradiente por profundidade) **substitui**
  o papel do `color`/`u_tint.rgb` antigo no cálculo de luz — `texel.rgb * waterColor *
  lightAmt * u_sunColor` (a textura de albedo, se houver, continua multiplicando pra
  detalhe tileável; `u_tint.a`/opacity continua controlando alpha). O campo `color` antigo
  do `ZWater` continua salvo/carregado (não removido do schema), só deixou de influenciar
  a cor da água — `shallow_color`/`deep_color` são estritamente mais expressivos.
- Campos novos em `ZWater`: `shallow_color`/`deep_color` (vec3, default
  `(0.3,0.7,0.6)`/`(0.02,0.10,0.20)`) e `depth_fade_distance` (float, default 2.5) —
  mesmo padrão de `wave_speed` (migração `ALTER TABLE ADD COLUMN` idempotente, sliders/
  color-pickers no `DrawPanelWater`, campos espelhados em `ZoneWater`(Go)/`world.Water`/
  `WaterPayload`/`WaterEntry`).
- **⚠️ Risco técnico sinalizado, não resolvido silenciosamente**: `gDepth_` é
  simultaneamente o depth ATTACHMENT ativo do `postprocessFbo_` durante o forward pass
  e a textura sendo lida por `u_sceneDepth` — um feedback loop pela spec estrita do GL.
  Esperado ser seguro (`glDepthMask(GL_FALSE)` durante o forward pass, `pipeline.cpp:971`
  — é a técnica padrão de "soft particles"), mas **não testado empiricamente nesta
  engine ainda**. Se aparecer flickering/profundidade com ruído/cor estranha na água ao
  testar, a correção é copiar `gDepth_` pra uma textura separada antes do forward pass
  (blit, custo extra) em vez de sample-enquanto-attachment direto.

**Anotado, não implementado**: normal map de TEXTURA complementar ao Gerstner procedural
— o Gerstner dá deslocamento real de baixa frequência (ondas grandes), mas rugas finas de
alta frequência (o "chapinhado" fino da superfície) tradicionalmente vêm de um normal map
sampleado e animado por cima, mais barato que aumentar a resolução da grade. Não pedido
ainda; ficaria como uma segunda fonte de perturbação de normal somada à do Gerstner.

**Sub-fase 2b implementada** (espuma de margem procedural):
- Reusa `depthDiff` de 2a **sem modificação** — nenhuma segunda amostragem de
  profundidade. `foamMask = 1.0 - smoothstep(0.0, foam_width, depthDiff)` (`water.fs`).
- Ruído procedural (não textura): `foamNoise = sin(dot(vWorldPos.xz,dirA)*freqA +
  u_time*speedA) * cos(dot(vWorldPos.xz,dirB)*freqB + u_time*speedB)`, com
  `freqA/freqB`/`speedA/speedB` derivados de `wave_scale`/`wave_speed` (4x/5.3x e
  2x/1.7x — múltiplos não-inteiros pra evitar repetição óbvia), não campos novos.
  `foam = foamMask * smoothstep(0.3, 0.9, foamNoise*0.5+0.5)`; composição final
  `mix(lit+specular, foam_color, foam)`.
- **Acompanha a onda + desliza sozinha**: usa `vWorldPos.xz` (posição JÁ deslocada pelo
  Gerstner em `water.vs`, não a posição de repouso) e `u_time` (mesmo relógio
  reaproveitado — `elapsed_time_` no GUE, `now` no cliente) — dois efeitos empilhados,
  não uma textura grudada numa posição fixa.
- **Função de ruído NÃO compartilhada entre vertex/fragment** — decisão deliberada:
  `GerstnerWave` (vertex) retorna deslocamento + derivadas parciais analíticas (uma
  assinatura pesada e especializada, necessária pra deslocar vértice + montar normal);
  a espuma (fragment) só precisa de um valor escalar de ruído. Forçar um compartilhamento
  significaria pagar por derivadas não usadas de um lado, ou uma variante "burra" do
  outro — a espuma ganhou uma implementação fragment-local nova, curta (4 linhas),
  reaproveitando o ESPÍRITO sum-of-sines, não o código.
- Campos novos em `ZWater`: `foam_width` (float, default 0.4) e `foam_color` (vec3,
  default branco `1,1,1`) — mesmo padrão de `shallow_color`/`deep_color` (migração
  `ALTER TABLE ADD COLUMN` idempotente, slider+color-picker no `DrawPanelWater`, campos
  espelhados em `ZoneWater`(Go)/`world.Water`/`WaterPayload`/`WaterEntry`).

**Sub-fase 2c confirmada por design** (espuma ao redor de personagem/objetos, sem código
novo): `foamMask` usa o MESMO `depthDiff` de 2a, que já compara a água contra QUALQUER
geometria opaca em `gDepth_` — terreno (`terrainPass_`) OU atores/props
(`gBufferPass_`), ambos desenhados antes do forward pass da água rodar. Um personagem
parcialmente submerso automaticamente produz um `depthDiff` pequeno no ponto de
interseção, disparando a mesma faixa de espuma que a margem do terreno — nenhuma
lógica de "personagem" foi ou precisa ser escrita.

**Fase 3 — reflexo APROXIMADO implementada** (IBL + Fresnel simplificado, sem pass novo):
- `water.fs` ganhou `R = reflect(-V, N)` (reusa o mesmo `N` Gerstner+ripple e `V` já
  calculados pro especular Blinn-Phong) amostrando `u_prefilterCube` — a MESMA cubemap
  especular pré-filtrada que `gPhongGlobal.fs` já usa pro IBL split-sum da cena inteira
  (`Engine::prefilterCube_`), exposta ao forward pass via novo getter
  `Pipeline::PrefilterCube()` (mesmo padrão de `SceneDepthTexture()`/`SunDirection()`).
  Nenhum bake novo, nenhum FBO novo, nenhum render-to-texture — só mais uma leitura de
  uma cubemap já pronta.
- LOD do prefilter: `kWaterRoughness=0.08` (fixo, baixo mas não-zero — mesma convenção
  de `gPhongGlobal.fs:162`'s `clamp(RMA.r, 0.04, 1.0)`) × `MAX_REFLECTION_LOD=4.0` (MESMO
  valor de `gPhongGlobal.fs:57`, derivado de `Engine::prefilterMipLevels_=5`).
  Fresnel: Schlick simplificado direto (`pow(1-NdotV, 5.0)`), sem `brdfLUT`/F0/metalness
  — a integração split-sum completa não se aplica aqui, é um efeito aproximado.
- `u_reflectionStrength=0.45` — constante GLOBAL fixa em C++ (não campo por-`ZWater`),
  setada nos dois call sites (`WaterManager::Render`/`ZoneRenderer::DrawWater`), mesmo
  padrão de `u_specPower`/`u_specIntensity`.
- Bind na texture unit 3 (0=albedo por-instância, 1=`gDepth_`, 2=ripple tex já ocupadas).

**O que falta (fases futuras, nenhuma implementada ainda):**
- **IBL/sombra difusa na água**: o reflexo especular acima cobre a "sensação de céu
  refletido", mas água ainda não recebe irradiância difusa ambiente nem sombra projetada
  (árvore/prédio na frente do sol não escurece a água debaixo). Sombra exigiria sample do
  shadow map do sol dentro do forward pass (hoje só a light pass deferred lê). Fica pra
  quando fizer sentido — não pedido nesta rodada.
- **Reflexo REAL (planar/SSR)**: o reflexo atual é uma APROXIMAÇÃO via cubemap de
  ambiente estática — não reflete objetos/personagens específicos da cena (só o
  "ambiente" capturado no bake do IBL). Reflexo de verdade exigiria render-to-texture de
  uma câmera espelhada (reflexo planar, mais barato) ou SSR (mais caro, reaproveitaria o
  `ssr.fs` já existente pro resto da cena). Exige FBO novo + pass novo — maior escopo e
  custo de performance real desta lista. Só vale a pena quando água virar um elemento de
  gameplay mais central (lagos grandes, combate aquático).
- **Mecânica de nadar** (separada da renderização): depende da água estar
  **networked** (feito na Fase 0 — `Area.Water`/`PZoneWater`). Falta o check em si:
  comparar `player.y + player_actor.ModelHeight()*0.6` contra o `Water.Y` mais próximo
  na área, rodando no servidor (autoridade de movimento), no mesmo ponto onde já roda
  o snap de terreno (`main.cpp` cliente ~4474 e a lógica equivalente de spawn/movimento
  NPC em `server/internal/world/area.go`). Servidor é o lugar certo porque movimento é
  autoritativo lá, igual ao snap de terreno.

**Estimativa (itens restantes):** IBL difuso/sombra ~1-2 dias (sun-only já existe, é
aditivo); reflexo planar ~4-6 dias ou mais pra SSR; nadar ~1-2 dias (assumindo networked,
que já está).

## 118. Ripple Sim (rastro de onda do jogador) — Fase (a)+(b) implementadas, só cliente

**Fase (a) implementada** (buffer Hugo Elias isolado, sem integração):
- `rco::renderer::RippleSim` (`shared/renderer/include/rco/renderer/ripple_sim.h` +
  `src/ripple_sim.cpp`) — 2 texturas `GL_R32F` ping-pong (128px default) + FBOs, molde do
  SSAO (`engine.cpp:323-332`). Update via fullscreen triangle (`ripple_update.fs`,
  `fullscreen_tri.vs` reaproveitado) rodando a recorrência clássica Hugo Elias/verlet:
  `novaAltura = (vizinhos_media_de_u_current)/2 - u_previous(mesmo texel)`, com damping.
  `GL_CLAMP_TO_EDGE` nas duas texturas evita vazamento de borda; `GL_NEAREST` garante
  amostragem exata texel-a-texel (crítico pro same-texel self-read do `u_previous`).
- **Same-texel self-read documentado, não escondido**: `u_previous` é sampleado do MESMO
  buffer que está sendo escrito nesse pass (só no texel exato, sem offset de vizinho) —
  é a técnica padrão de toda implementação real do algoritmo (GPU Gems também documenta
  isso), tecnicamente não coberta por `GL_ARB_texture_barrier`, mas universalmente aceita
  na prática pra esse padrão específico de acesso. Mesma classe de risco documentado do
  `gDepth_` na Fase 2a.
- Debug: `F9` liga/desliga um overlay pequeno no canto superior esquerdo
  (`RippleSim::DebugDraw`, reusa `Shader::shaders["fstexture"]`); `F10` (fase a) forçava
  um carimbo manual no centro do buffer.

**Fase (b) implementada** (janela dinâmica + carimbo automático + integração na água):
- `RippleSim` ganhou `windowCenter_`/`windowSize_` — o buffer não é mais um UV fixo, é uma
  janela de MUNDO (`windowSize_` unidades, default 20) que segue o jogador.
  `UpdateWindow(playerXZ)` recentra com **histerese** (só quando `dist > 0.25*windowSize_`,
  não todo frame) — decisão confirmada: sem shift shader, aceita o salto ao recentrar
  (mascarado pelo damping). `WorldToUV(worldPos)` faz a conversão mundo→UV da janela,
  reaproveitada IDENTICAMENTE em C++ (carimbo) e GLSL (`water.vs`, mesma fórmula).
- **Carimbo automático**: `main.cpp` chama `water_manager.PlayerContact(playerPos)` (AABB
  via `WaterEntry::Contains` + tolerância vertical de 1.5 unidades) ANTES de
  `ripple_sim.Update()` — se o jogador está na água E se moveu (usa o `last_player_pos` já
  rastreado, sem duplicar estado), carimba na posição real via `WorldToUV`. `F10`
  mantido como override manual (carimba no centro da janela, ignora o teste de água) —
  útil pra teste visual mesmo fora d'água.
- `WaterEntry::Contains(vec2 xz)` — AABB simples (`pos.xz ± scale/2`) — o método que
  faltava, apontado na investigação.
- `WaterManager::Render` ganhou parâmetros `playerPos`/`rippleSim`; por instância, roda o
  MESMO teste de `PlayerContact` (AABB + tolerância Y) — só a instância onde o jogador
  está recebe `u_hasRipple=1` + bind da textura de ripple + os uniforms de janela; as
  outras recebem `u_hasRipple=0` e o `water.vs` pula o bloco inteiro (nem amostra a
  textura) — custo zero pras águas sem jogador.
- `water.vs`: dentro de `if (u_hasRipple != 0)`, soma a altura do ripple a `worldPos.y`
  (SOMA, não substitui — depois do deslocamento Gerstner já computado) e soma o gradiente
  (diferenças de vizinhos, técnica padrão pra heightmap discreto — Gerstner é analítico,
  isso não é) aos MESMOS acumuladores de tangente que as 2 ondas Gerstner já usam, antes
  do `cross(tangentZ, tangentX)`/`normalize()` final — combina as 3 fontes de normal
  (wave1+wave2+ripple) com o mesmo padrão aditivo já estabelecido.
- **GUE intocado, confirmado por design**: `Shader::shaders` é um mapa global POR
  PROCESSO — GUE e cliente são processos separados, cada um com seu próprio contexto GL.
  O GUE nunca seta `u_hasRipple`/`u_rippleTex`/etc (`ZoneRenderer::DrawWater` não foi
  tocado), e uniforms não setados ficam zero-inicializados em GLSL — `u_hasRipple`
  permanece `0` no GUE automaticamente, sem precisar de nenhuma mudança lá.
- `kRippleHeightScale=0.5`/`kRippleNormalStrength=3.0` são constantes fixas no shader,
  estimadas por olho (mesmo espírito do antigo `kWaveStrength` do Gerstner antes de ser
  calibrado) — esperar precisar de um ajuste visual quando testável com jogador real em
  água de verdade.

**O que falta**: calibração visual dos 2 constantes acima (só possível com teste real em
jogo); um possível ajuste de `kSurfaceYTolerance` (1.5 unidades, também estimado) se o
carimbo disparar cedo/tarde demais na prática.

## 119. Movimento do player — WASD relativo à câmera + rotação suave (implementado)

**Problema anterior**: `player.yaw` era travado no yaw da câmera todo frame
(`main.cpp:4633`, removido: `player.yaw = camera.GetYaw();`) — o personagem sempre
encarava exatamente pra onde a câmera olhava, então W/A/S/D moviam nos eixos
`fdir`/`rdir` dessa direção travada: W/S andavam de frente/trás corretamente, mas A/D
deslizavam de lado sem o corpo virar (strafe puro, sem giro).

**Correção implementada**:
- `main.cpp`: removida a sincronização `player.yaw = camera.GetYaw()` (câmera e
  personagem agora desacoplados — a câmera orbita livre via `ApplyMouseDelta`, sem
  empurrar o yaw do personagem). `player_ctrl.Update(...)` passa a receber
  `camera.GetYaw()` como novo parâmetro `camera_yaw`.
- `player_controller.h`/`.cpp`: `PlayerController::Update()` ganhou o parâmetro
  `float camera_yaw` — a base `fdir`/`rdir` do WASD (`player_controller.cpp`, dentro de
  `Update()`) passou a usar `camera_yaw` em vez de `player.yaw`, tornando o input
  genuinamente relativo à câmera.
- Rotação suave: quando há input de movimento (`dir != 0`), calcula
  `target_yaw = atan2(dir.x, dir.y)` (mesma convenção já usada pelo click-to-move,
  `player_controller.cpp:~250`) e interpola `player.yaw` até `target_yaw` via
  shortest-arc lerp — MESMO padrão já ativo em produção pra suavizar o yaw de OUTROS
  jogadores via rede (`main.cpp:102-117`,
  `NormalizeYawDegrees`/`ShortestYawDeltaDegrees`/`SmoothLerpFactor`), duplicado como
  helpers locais em `player_controller.cpp` (não compartilhados via header — os
  originais em `main.cpp` são `static`/linkage interno). Sem input (parado), `player.yaw`
  não muda — não gira mais sozinho acompanhando o mouse.
- `PlayerController::Config::turn_rate` (`player_controller.h:20`) — campo que existia
  mas nunca era lido — agora está ATIVO como a taxa do lerp (`SmoothLerpFactor`).
  Default subiu de 150.0f (morto) pra 180.0f (mais rápido que os 20°/s usados pra
  suavizar correção de rede de outros jogadores, já que resposta a input local precisa
  ser mais ágil que uma correção gradual de rede).
- **Intocado, confirmado por design**: animação (`anim_controller.cpp`'s `Update(dt,
  speed)` só reage a velocidade escalar, nunca a yaw/direção — Walk/Run ficam coerentes
  automaticamente já que o personagem sempre vira de frente antes/durante o movimento);
  rede (`main.cpp:4706-4716` continua enviando `player.yaw`, mesmo campo, sem mudança).
- **Corrigido** (era o item pendente abaixo): os 3 call sites de dodge-roll
  (`resolve_dodge_dir` em `main.cpp:~4568`, dodge autoritativo em `main.cpp:~2587`,
  `/combat dodge` em `main.cpp:~6099`) liam `player.yaw`, que divergia de
  `camera.GetYaw()` a qualquer momento (já que o acoplamento câmera/corpo foi removido
  acima) — um dodge lateral puro ganhava componente de frente indesejada quando o corpo
  não estava alinhado com a câmera. Todos os 3 agora derivam a direção de
  `camera.GetYaw()`, mesma base do WASD, sem delay do lerp de giro do corpo.

**O que falta**: calibração visual do `turn_rate=180.0f` em jogo real (só uma estimativa
inicial, mesmo espírito de outras constantes desta lista).

## 120. Stats derivados — auditoria de uso real em combate (server) + 2 fixes

- Auditoria completa dos 34 campos de `DerivedStats` confirmou que hit/miss, crit,
  defesa, dano (min/max por dimensão), `DamageReductionFlat`, `AttackSpeedMult` e
  `CooldownSpeedPct` já estavam genuinamente lidos na resolução de combate
  (`combat_basic.go`, `combat_special.go`, `combat_cooldown.go`, `combat_dimension.go`).
- **Corrigido**: `RangeBonusPct` (PER-driven) nunca influenciava alcance —
  `ResolveAttackRange` (`combat_dimension.go`) ganhou um 3º parâmetro
  `rangeBonusPct` e aplica `final = base * (1 + rangeBonusPct)`. Afeta o alcance do
  ataque básico sempre (`InAttackRange` lê `actor.AttackRange` diretamente); afeta
  skills só quando a ability não define `RangeMax` próprio (fallback pra
  `npc.AttackRange` em `inSpecialRange`, `combat_special.go`). Bug de ordering achado
  de brinde: no login (`handleStartGame`, `client.go`), `AttackRange` era resolvido
  ANTES de `RecomputeDerivedStats` popular `Derived` — todo personagem entrava com
  bônus de alcance zerado independente do PER. Movido pra depois do recompute.
  Edge case conhecido e NÃO corrigido: nas trocas de equipamento
  (`handleInventorySwap`/`handleUseItem`), o `AttackRange` da troca usa o
  `Derived.RangeBonusPct` de ANTES do recompute dessa mesma troca — só importa se o
  item recém-equipado também conceder `range_bonus_pct` via `item_attributes` (bônus
  só aparece no próximo recompute, não no pacote imediato).
- **Corrigido**: `BonusDamageFlat` (nível-based) só era aplicado em dano de skill
  (`combat_special.go:415`), não no ataque básico. Adicionado em `combat_basic.go`'s
  `ProcessAttack`, na MESMA posição relativa que skills usam — logo após o roll de
  dano base, antes de defesa/armor e antes de crit — pra manter o bônus consistente
  entre básico e skill (reduzido por defesa, amplificado por crit em ambos).
- **PRÓXIMO ARCO GRANDE (não é bug, é feature a projetar)**: Sistema de Crowd Control
  (stun/root/silence) e Buffs/Debuffs com duração — não existe hoje no servidor.
  `CCChanceValue`/`CCResistanceValue`/`BuffDurationPct`/`DebuffDurationPct` já são
  calculados (fórmulas reais, PER-driven) e transmitidos ao cliente (`PFullStats`),
  prontos para consumo quando essas features forem implementadas. Requer: sistema de
  status effect/aplicação, resolução de resistência (chance vs resistência, mesmo
  padrão comparativo de `computeHitChance`), duração e ticking, sincronização de
  estado (visual + servidor). Planejar como sessão dedicada, não como fix pontual.

## 121. Mesh slots fixos com anexação rígida via bone (Gremlin Helm)

**Problema investigado**: o Actor Def "Gremlin" tem um slot Helm (`GremlinEye_01`,
mesh estática sem esqueleto — confirmado inspecionando o `.b3d`: 0 chunks `BONE`,
0 `KEYS`) que nunca aparecia nem no preview do GUE nem no jogo. Causa raiz: tanto o
preview do GUE (`media.cpp`'s `DrawActorDefs`) quanto o lazy-init de NPC/player no
cliente (`main.cpp`) só resolviam e desenhavam o slot 0 (Body) — todo outro
`mesh_slots` era armazenado mas nunca lido de novo. O mecanismo de anexação por bone
já existia (`Actor::GetBoneWorldTransform` + `Actor::SubmitWithMatrix`, usados pelo
attachment de item da aba Items/preview e pelo B5 de arma-na-mão), mas cabeado
exclusivamente ao fluxo de item equipado — nunca aos mesh slots do próprio Actor Def.

**Implementado**: mesh slots não-Body agora podem ser anexados rigidamente a um bone
do Body, fixo por design (configurado uma vez no GUE, não runtime/dinâmico):
- **Schema** (`migrateV52`, `db.go`): `media_actor_meshes` ganhou `bone_name` +
  `offset_pos_x/y/z` + `offset_rot_x/y/z` + `offset_scale` (mesmo padrão de campos já
  usado por `actor_def_sockets`, mas tabela e consumidor **separados**). `bone_name`
  vazio = comportamento legado (slot ignorado além do Body, igual antes).
- **Servidor**: `world.MeshSlot` (`actor.go`) ganhou os mesmos campos;
  `BuildAppearanceFromDef` (`appearance.go`) os copia; `AppearanceBytes` e
  `NewActorPayload` (`frame.go`) os serializam por mesh, logo após `black_cutout` e
  antes do material_map — aditivo, mesma posição nos dois pacotes (PStartGame e
  PNewActor).
- **GUE**: editor de mesh slot (`media.cpp`, popup "edit_slot") ganhou campo Bone
  (reaproveitando a MESMA lista de bones que a tabela de Sockets já usa, só como
  fonte de opções de UI) + offsets, visível apenas quando `slot != 0`. Preview
  (`PreviewViewport::SetMeshSlotAttachments`, `preview_viewport.h/cpp`) é um
  **terceiro consumidor, paralelo e independente** de
  `GetBoneWorldTransform`/`SubmitWithMatrix` — não lê nem escreve
  `attachment_`/`AttachmentSpec`/`has_attachment_` (que continuam exclusivos do
  attachment de item da aba Items).
- **Cliente do jogo** (`main.cpp`): `WorldMesh` ganhou os mesmos campos;
  `WorldMeshAttachment` + as lambdas `rebuild_extra_mesh_actors` (carrega um Actor
  extra por slot com `bone_name` preenchido, na lazy-init do NPC/player) e
  `submit_extra_mesh_actors` (reposiciona todo frame via bone atual do Body) também
  são um consumidor paralelo — não tocam em `equipped_item`, `player_sockets`,
  `player_weapon_actor` nem no bloco B5.
- **Labels do GUE** (`media.cpp`'s `kSlotNames`): slots 1-10 renomeados de
  "Hair/Helm/Chest/.../Attachment" para "Slot 1".."Slot 10" — só o texto exibido
  mudou (os valores numéricos do enum continuam 0-10, nenhuma migração de dado). O
  slot 0 continua "Body" (único com significado especial: a mesh primária/skinned
  que preview e client sempre resolvem). Motivo: os nomes antigos prometiam um
  sistema de guarda-roupa que não existe — "Helm"/"Hat" também é um enum
  **totalmente separado** do `slot_type` de equipamento de item (`ItemTemplate`,
  `kSlotTypes` em `items.cpp`), que só coincide em 3 valores (Chest/Hands/Belt/Legs/
  Feet) por acaso.

**Isolamento confirmado**: nenhuma linha tocada em `items.cpp`, no bloco B5
(`equipped_item`/`player_weapon_actor`, `main.cpp`), em
`SetAttachment`/`AttachmentSpec`/`attachment_` (`preview_viewport.h/cpp`), ou em
`WorldSocket`/`ActorDefSocket`/`SocketBinding`. Apenas os dois primitivos de baixo
nível do renderer compartilhado (`GetBoneWorldTransform`, `SubmitWithMatrix`) foram
reaproveitados, como já acontecia entre o preview de item do GUE e o B5 do cliente.

**Não testado em jogo ainda** (implementado nesta sessão, sem compilar/commit) —
falta: configurar `bone_name` do slot Helm do Gremlin no GUE e verificar visualmente
no preview e em jogo.

## 122. Ícones de UI para Items e Combat Abilities

**Problema**: a UI de inventário e a hotbar de combate já reservavam espaço visual pra
um ícone por item/ability, mas nenhum dos dois sistemas tinha campo de ícone no schema
— o slot ocupado sempre desenhava só texto (nome truncado) e o hotbar sempre desenhava
um retângulo colorido fixo. Existiam 174 PNGs prontos em
`dist/client/assets/models/Textures/Item Icons/` (20 subpastas), sobrando do
RealmCrafter original, não referenciados por nenhum código.

**Implementado**: campo `icon_path` (string, "" = comportamento legado) ponta a
ponta, mesma técnica pros dois sistemas:
- **Schema** (`migrateV53`, `db.go`): `icon_path TEXT NOT NULL DEFAULT ''` em
  `item_templates` e `ability_templates` (ADD COLUMN idempotente). Structs Go
  (`ItemTemplate`, `CharacterItem`, `AbilityTemplateRow`, `world.AbilityTemplate`)
  e todas as queries (Load/List/Create/Update de item; Load de ability;
  `GetInventory`'s join) ganharam o campo. `server/cmd/server/main.go` propaga
  `row.IconPath` do DB pro catálogo de abilities em runtime
  (`world.SetAbilityCatalog`).
- **Wire**: `PInventoryUpdate` (`sendInventory`, `client.go`) ganhou `icon_path`
  como campo aditivo no fim de cada item. `PSkillState` subiu pra versão 5 e
  `PKitPool` pra versão 2 — mesmo padrão de versionamento retrocompatível já usado
  nesses dois pacotes (campos novos só lidos/escritos quando a versão negociada
  permite; servidor e cliente antigos continuam se falando sem o campo).
- **Cliente**: `InventoryItem`/`SkillStateAbility`/`KitPoolAbility` ganharam
  `icon_path`. `inventory.cpp` desenha o ícone (equip slot e grade da bag) atrás do
  texto quando `icon_path` não é vazio — texto continua sendo desenhado por cima
  (nome/durabilidade), nada removido. `skill_hotbar.cpp`'s `RenderAbilitySlot`
  desenha o ícone sobre o retângulo colorido pelo mesmo critério. Ambos usam
  `UITextureCache::Load(path)` (`ui_texture.h`) — cache por path já existente no
  projeto, mesmo espírito do `GetOrLoadWaterTexture` — então reabrir a bag/hotbar
  não recarrega a textura do disco a cada frame.
- **GUE**: `items.cpp` e `combat_abilities.cpp` ganharam um combo "Icon"
  (`SearchableComboString`, mesmo padrão já usado pra Model/Socket/Bone) populado
  escaneando recursivamente a pasta "Item Icons" existente, mais um botão "Import
  Icon..." (`PickAndImportAsset`, mesmo fluxo de import já usado pra texturas de
  material) pra trazer ícones novos pra essa mesma pasta. `LoadIconOptions()` é
  duplicada (não compartilhada) entre os dois arquivos — mesmo padrão de
  duplicação de helper pequeno já usado no resto do GUE (ex. bone-name lists por
  aba), não uma abstração nova.
- **Intocado, de propósito**: o sistema legado de `spell_templates`/`spells.cpp`
  (campo `icon` numérico antigo) — sistema desconectado do fluxo atual de combate,
  fora de escopo.

**Não testado em jogo ainda** (implementado nesta sessão, sem compilar/commit) —
falta configurar `icon_path` em pelo menos um item/ability no GUE e confirmar
visualmente na bag e na hotbar.

## 123. Retargeting genérico de animação por bone slot canônico (Humanoid Rig)

**Problema**: aplicar uma animação externa (ex.: download do Mixamo, "Without Skin")
no esqueleto de um modelo do projeto exige que os NOMES de bone batam exatamente
(`Model::AppendAnimationsFrom`, `shared/renderer/src/model.cpp`, casa canais por
`node_map_.find(rc.name)` — nome exato, sem tradução). O Mixamo sempre renomeia
bones pra sua própria convenção (`mixamorig:Hips`, etc.), então um clip baixado de
lá nunca bate com a convenção `Bip01 X` usada pelos modelos do projeto
(confirmado inspecionando `Male_01.b3d`/`FemaleHuman.b3d`/`Gremlin.b3d` — Male e
Female compartilham a mesma convenção exata; o Gremlin já diverge em topologia).

**Implementado — sistema de retargeting por PAPEL SEMÂNTICO (inspirado no
Humanoid Rig/IK Rig da Unreal/Unity)**, em vez de uma tabela de tradução fixa em
código ou um alias direto por modelo:

- **Papel canônico**: lista fixa de 19 slots humanoides (`Hips, Spine, Chest,
  Neck, Head, Clavicle_L/R, UpperArm_L/R, Forearm_L/R, Hand_L/R, Thigh_L/R,
  Calf_L/R, Foot_L/R`) — constante compartilhada, duplicada por linguagem
  (`server/internal/db/db.go`'s `CanonicalBoneSlots`, `tools/gue/src/tabs/media.h`'s
  `kCanonicalBoneSlots`), mesmo padrão já usado pra `ActorSlot`/`kSlotTypes`.
- **Duas tabelas independentes** (`migrateV54`, `db.go`):
  - `model_bone_slots` (POR MODELO): qual bone REAL do modelo toca cada slot
    (ex.: model 7's slot "Chest" → bone interno "Bip01 Neck"). Um modelo só
    preenche os slots que fazem sentido pra ele.
  - `external_bone_conventions` (GLOBAL, não por modelo): como uma convenção de
    nomenclatura (ex. "Mixamo") rotula cada slot (ex. "Chest" → "mixamorig:Spine2").
    Seedada uma vez com os 19 pares padrão do Mixamo (idempotente,
    `INSERT OR IGNORE`/`ON CONFLICT DO NOTHING`) — editável, outras convenções
    podem ser adicionadas como dado, sem tocar código.
- **`BuildBoneAliasMap`** (`db.go`, espelhado em C++ como
  `MediaTab::BuildBoneAliasMap`, `media.cpp`) — o JOIN das duas tabelas por
  `slot`, pra um `(model_id, convention_name)`, produzindo o mapa achatado
  `external_name -> internal_bone_name` que o motor consome.
- **Motor** (`shared/renderer`): `Model::SetBoneAliases(unordered_map)`
  (`model.h`) injeta esse mapa; `AppendAnimationsFrom` (`model.cpp:~1561`) tenta
  `node_map_.find(rc.name)` primeiro (caminho normal, intocado) e só consulta
  `bone_aliases_` como fallback se a busca exata falhar — arquivos que já usam
  nomes compatíveis não notam nenhuma diferença. `shared/renderer` continua sem
  nenhuma dependência de SQLite; o mapa é sempre dado injetado, nunca uma query
  interna.
- **GUE**: duas UIs novas — "Bone Slots" dentro da aba Models (por modelo
  selecionado) e uma sub-aba nova e GLOBAL "Bone Conventions" (lista de
  convenções + tabela slot→external_name editável, com botão "Load Mixamo
  Names" — só pré-preenche o campo de texto com os nomes padrão do Mixamo, não
  é lógica de tradução). Um botão "Apply for Preview" injeta o mapa achatado
  direto no `preview_`'s Model (`SetBoneAliases`), pra testar o retargeting ao
  vivo sem depender de rede/servidor.
- **Configurar um modelo novo não exige reensinar nada sobre Mixamo** — a
  convenção já está pronta globalmente (seedada uma vez); só é preciso mapear os
  slots do modelo novo em "Bone Slots".
- **`aiProcess_OptimizeGraph` é INVARIANTE no carregamento principal do corpo**
  (`Model::Load`, `model.cpp:~1291`) — funde nós de transformação "redundantes"
  no pai, e o pipeline de skinning depende desse grafo já colapsado. **Nunca
  remover esse flag pra "resolver" um problema de listagem de nós** — ele
  colapsa folhas reais sem peso de vértice próprio (ex.: `L_Shin`/`R_Shin` do
  Male_01.b3d, filhos de `Bip01 L/R Calf`) antes de `BuildNodeTree` rodar,
  então elas nunca entram em `node_map_`/aparecem no combo de Bone Slots — mas
  a correção correta é uma leitura AUXILIAR e descartável do mesmo arquivo, sem
  esse flag, só pra extrair nomes (`Model::AllNodeNamesUnoptimized`, estático,
  `model.cpp`/`model.h`), nunca usada pra desenhar/animar. O combo de Bone
  Slots (`media.cpp`'s `DrawModels`) usa essa função, cacheada por
  `editModel_.id` (`bone_slot_node_names_cache_`) pra não reparsear o arquivo
  todo frame; o Model real usado pelo jogo/preview nunca é afetado.

- **Delta-retargeting (rotação, não só nome do canal)**: `AppendAnimationsFrom`
  não aplica mais a rotação bruta do canal externo direto como rotação final
  do bone de destino (isso presumia implicitamente que os dois rigs
  compartilham referencial de repouso — falso entre Mixamo e um rig Biped
  próprio). Em vez disso, calcula um delta relativo à bind pose de cada lado:
  `delta = inverse(bind_origem) * rotação_animada`, aplicado como
  `bind_destino * delta`.
  - ⚠️ **`bind_origem` usado NESSA fórmula precisa ser a rotação PRÓPRIA do nó
    nomeado, NÃO composta com ancestrais sintéticos** — `CollectBindRotations`
    (`model.cpp`) usa só `BindPoseRS(node->mTransformation)`, deliberadamente
    sem subir por `_$AssimpFbx$_PreRotation`/etc. Isso porque o valor ANIMADO
    do canal (`rc.rot`, vindo direto de `ch->mRotationKeys`) também NUNCA é
    composto com esses ancestrais — Assimp sempre expressa a animação
    relativa ao pai IMEDIATO do nó (o próprio nó sintético de PreRotation,
    quando existe), nunca composta com ele. Usar a versão COMPOSTA
    (`ComposedOriginBindRotation`, mantida no arquivo só como utilitário não
    usado — `[[maybe_unused]]`) nessa fórmula foi um bug real, encontrado via
    um invariante matemático simples: num clipe quase parado (Idle), o canal
    animado ≈ bind pose de origem, então `delta_origin_frame` deveria ficar
    ≈identidade — mas com a versão composta, isso só valia pro Hips (sem
    ancestral PreRotation); todo o resto do corpo (que TEM PreRotation)
    produzia uma rotação espúria grande mesmo parado, porque `bind_origem`
    composta e o canal animado (não composto) estavam em referenciais
    diferentes sendo subtraídos como se fossem o mesmo. O estimador de offset
    posicional (`EstimateRetargetOffsets`/`BindWorldPositionsUnoptimized`) não
    tem esse problema — comparação de POSIÇÃO já é sempre feita com a
    hierarquia inteira composta dos dois lados, sem essa armadilha de
    referencial.

- **Retarget Pose offset por bone (`model_retarget_offsets`)** — inspirado na
  `FIKRetargetPose::BoneRotationOffsets` da Unreal IK Retargeter (investigação
  no source em `D:\Github\UnrealEngine\Engine\Plugins\Animation\IKRig`,
  `FKChainsOp.cpp`). Mesmo com a bind pose de origem corretamente composta, o
  delta puramente LOCAL ainda erra quando os dois rigs têm CONVENÇÕES DE EIXO
  diferentes por bone (ex.: braço em repouso ao longo de eixos diferentes) — a
  Unreal resolve isso em espaço GLOBAL/componente, exigindo uma "Retarget
  Pose" pré-alinhada entre os dois esqueletos (ver `FKChainsOp.cpp:342-351`,
  `RetargetRotation`). **Adaptação deliberada pra nossa arquitetura**: em vez
  de migrar pra avaliação hierárquica GLOBAL completa (que exigiria reescrever
  `ComputeBones`/`ComputeBlendedBones` pra propagar rotação global pai→filho
  em tempo de execução, e reavaliar a hierarquia INTEIRA do arquivo de origem
  por keyframe — nosso `AppendAnimationsFrom` hoje processa canais de forma
  achatada/independente, sem essa dependência hierárquica), implementamos o
  offset como uma CONJUGAÇÃO no delta local já existente:
  ```
  delta_dest_frame = offset * (inverse(bind_origem) * rotação_animada) * inverse(offset)
  final = bind_destino * delta_dest_frame
  ```
  Conjugação é a forma padrão de reinterpretar uma rotação de uma base pra
  outra quando as duas se relacionam por uma rotação fixa (`offset`) — e com
  `offset = identidade`, `offset * X * inverse(offset) == X`, então a fórmula
  colapsa EXATAMENTE pra `bind_destino * delta` (o comportamento pré-existente,
  sem NENHUMA mudança) — invariante provado, não só testado.
  - Schema: `model_retarget_offsets(model_id, slot, convention_name,
    offset_x/y/z/w)` — offset por (model, slot, CONVENÇÃO), não só por slot,
    já que a correção de eixo é especificamente em relação a uma convenção
    externa (`server/internal/db/db.go`'s `migrateV55`, espelhado em
    `media.cpp`'s `EnsureTables`).
  - Estimativa automática: `MediaTab::EstimateRetargetOffsets` (`media.cpp`)
    compara a direção osso-pai→filho (`kSlotChain`, `media.h`) entre a bind
    pose do modelo (`Model::GetBindWorldTransform`, composição via
    `node_map_`/`anim_nodes_`) e a de um "Reference Pose File" da convenção
    (`Model::BindWorldPositionsUnoptimized`, leitura descartável — mesmo
    padrão de `AllNodeNamesUnoptimized`), via `glm::rotation(origem, destino)`
    (equivalente ao `FQuat::FindBetweenNormals` da Unreal). Fallback pra
    extremidades sem filho na cadeia (Head, Hand_L/R, Foot_L/R): usa a direção
    PAI→este-bone em vez de este-bone→filho.
  - GUE: sub-seção "Retarget Offsets" (aba Models, dentro de Bone Slots) —
    slider euler XYZ por slot, salva no DB e reinjeta no preview
    IMEDIATAMENTE a cada mudança (sem esperar reload/restart — mesmo padrão
    de `bone_slots_revision_` já usado pros bone_aliases_).
  - **Limitação conhecida, não resolvida nesta sessão**: `bone_retarget_offsets_`
    (`Model`) é uma única flat map por bone (nome interno), não escopada por
    convenção em tempo de execução — se DUAS convenções diferentes
    calibrarem offsets DIFERENTES pro MESMO bone interno e ambas forem
    injetadas na mesma sessão de preview, uma sobrescreve/perde a outra
    (mesma limitação já documentada pra `bone_aliases_`, mas lá é inofensiva
    porque os vocabulários de nome externo são disjuntos — aqui NÃO é, já
    que a chave é o nome INTERNO). Na prática afeta pouco: só um corpo/preview
    ativo por vez, tipicamente uma convenção só.

**Não implementado nesta sessão (fora do escopo pedido)**: entrega do mapa pro
CLIENTE do jogo via rede (o `BuildBoneAliasMap`/`SetBoneAliases` hoje só é
exercitado pelo preview do GUE, que tem acesso direto ao SQLite — o cliente do
jogo, sem banco, precisaria receber o mapa resolvido servidor→wire, análogo ao
que já foi feito pra `bone_name`/offsets do mesh slot rígido). Sem compilar,
sem commit.

## 124. Terrain multi-material authoring (Phase 1) — teto de 4 materiais removido na AUTORIA; runtime do jogo ainda travado em 4

**Estado:** o sistema de splatmap/terreno era hard-limitado a 4 materiais
(`kMaxMats=4`, RGBA de um único `Splatmap`, shader com uniforms nomeados
`u_mat0_*`.._mat3_*`). Investigação prévia (ver `git log` desta sessão)
mapeou o limite e recomendou generalizar a autoria antes de resolver o
runtime — este item é a Fase 1 dessa recomendação.

**O que foi feito (autoria no GUE, sem cap):**
- `tools/gue/src/terrain/splatmap.h`: `Splatmap` virou uma pilha de
  `SplatLayer` (cada um a antiga textura RGBA8 de 4 canais). Material `i`
  resolve pra `layer i/4, channel i%4` — sem teto superior. Persistência
  ganhou formato nativo `RSPN` (multi-layer), com fallback de leitura pros
  formatos legados `RSP2`/`RSPM` (single-layer). `PaintSplatmap` generalizado:
  redistribui peso perdido entre TODOS os slots ativos da pilha, não só os 4
  do mesmo grupo RGBA.
- `tools/gue/src/terrain/editable_terrain.h/.cpp`: `kMaxMats` removido;
  `materials_`, `matIds_`, `matAlbedoPaths_`, `matNormalPaths_`,
  `matOrmPaths_`, `matNormalStrengths_`, `tilings` viraram `std::vector`
  sem teto. Novos `AddMaterialSlot`/`RemoveMaterialSlot`. `materials.txt` lê
  quantas linhas houver (antes limitado a `kMaxMats`).
- UI (`tools/gue/src/tabs/zones_panels.cpp`): picker de pintura trocou os 4
  botões fixos por um combo com busca (`ui::SearchableComboId`) listando
  todos os materiais cadastrados; painel "Materials" ganhou botão
  "+ Add Material" e "x" por slot pra remover; sliders de tiling e combos de
  Auto-paint Slope (`Flat/Rock layer`) agora percorrem `terrain.NumMaterials()`.

**Preview do editor (shader) — usa texture arrays, não mais units por material:**
Extender o shader generalizando a técnica antiga (mais `u_matN_*` nomeados,
mais texture units) satura em ~5-6 materiais: `GL_MAX_TEXTURE_IMAGE_UNITS`
por estágio de fragment é tipicamente 32 em GPUs desktop e o path legado já
usa 23 unidades pra só 4 materiais. Em vez disso:
- `tools/gue/src/terrain/material.h`: `MaterialTextureArray` — um
  `GL_TEXTURE_2D_ARRAY` por role (albedo/normal/roughness/ao/height), 1
  camada por material, resolução fixa (`kRes=512`, reamostragem
  nearest-neighbour a partir da textura 2D já carregada via `glGetTexImage`).
  Custa 1 unidade de textura por role independente de N.
- `shared/renderer/include/rco/renderer/pipeline.h`: `TerrainChunkSubmission`
  ganhou campos `ext_*` (aditivos — os campos legados `mat_albedo[4]`,
  `tilings` (vec4), `splatmap` continuam intocados e são os únicos que o
  CLIENTE (`client/src/renderer/terrain/terrain.cpp`, não tocado nesta sessão)
  preenche). `num_materials` = 0 é o sinal de "usa o path legado".
- `shared/renderer/src/pipeline.cpp` (`terrainPass_`): AMBOS os conjuntos de
  uniforms (legado sampler2D units 0-20, ext sampler2DArray units 23-28) são
  sempre setados/bindados em TODO draw, independente de qual path está
  ativo — ver bug 124.1 abaixo. O conjunto inativo recebe uma textura dummy
  1x1 branca do tipo certo (`ensureTerrainDummyTextures_`), nunca um handle 0
  nem uma textura do tipo errado.
- `dist/client/shaders/terrainGBuffer.fs`: branch em tempo de execução no
  `main()` — `u_numMaterials>0` entra no path generalizado (`sampler2DArray`,
  loop até `MAX_EXT_MATERIALS=64`, height-blend UE5 generalizado pra N);
  `u_numMaterials==0` (sempre o caso pro cliente do jogo) cai no path legado
  EXATO de antes, sem nenhuma mudança de comportamento/resultado visual.

**Bug encontrado e corrigido nesta sessão:** a primeira versão de
`EditableTerrain::SubmitToPipeline` setava `num_materials = materials_.size()`
incondicionalmente — fazendo QUALQUER área, mesmo com ≤4 materiais (ex.
"Training Camp", formato legado), entrar no path novo (texture arrays) nunca
testado visualmente, resultando em terreno invisível no GUE (o cliente do
jogo não era afetado, pois nunca seta `num_materials`). Corrigido: o path
generalizado só ativa quando `materials_.size() > 4`; caso contrário
`SubmitToPipeline` preenche os campos legados (`splatmap`, `mat_albedo[4]`,
`tilings` vec4, `mat_normal_strength[4]`) exatamente como antes da Fase 1, e
`num_materials` fica em 0 (path legado). Logs de diagnóstico temporários
(`[terrain-dbg]`) permanecem em `editable_terrain.cpp`/`pipeline.cpp` até uma
área com >4 materiais de verdade ser testada visualmente com sucesso.

**Bug 124.1 encontrado e corrigido — `GL_INVALID_OPERATION` "program texture
usage" (terreno invisível mesmo no path legado, inclusive em mapa novo/vazio):**
Depois da correção acima, o path legado passou a ser o caso comum de novo —
e foi aí que apareceu um segundo bug, pré-existente desde que o path `ext_*`
foi introduzido: o programa `terrainGBuffer` compilado tem, ao MESMO TEMPO,
os uniforms sampler2D legados (`u_splatmap`, `u_mat0_albedo`, ...) E os
sampler2DArray novos (`u_splatmapArrayExt`, `u_matAlbedoArrayExt`, ...).
Valores de uniform persistem no programa entre draw calls — o branch que
NÃO roda numa dada chamada deixa seus samplers apontando pra alguma unidade
(0, se nunca setados desde o link; ou o que uma chamada anterior tiver
deixado). O código original reusava as unidades 0-5 tanto pro splatmap/mat0
legado (sampler2D) quanto pros arrays novos (sampler2DArray) — dois samplers
ativos de tipo incompatível referenciando a MESMA unidade é exatamente o que
gera esse erro no draw. Isso batia com "quebra em qualquer mapa": assim que
o path legado passou a ser usado (Bug 124 corrigido acima), as unidades 0-5
ficaram com sampler2D bindado enquanto os sampler2DArray (nunca setados no
branch legado) ainda apontavam — por default/estado anterior — pras mesmas
unidades 0-5.
- **Correção 1:** unidades disjuntas — legado continua 0-22 (0=splatmap,
  1-20=materiais, 21=macro, 22=heightmap), `ext_*` movido pra 23-28
  (`shared/renderer/src/pipeline.cpp`, `terrainPass_`). Nenhum tipo de
  sampler pode mais colidir numa mesma unidade.
- **Correção 2:** `Pipeline::ensureTerrainDummyTextures_()` cria duas
  texturas dummy 1x1 brancas (uma `GL_TEXTURE_2D`, uma `GL_TEXTURE_2D_ARRAY`)
  uma única vez. TODO draw agora seta E binda os DOIS conjuntos de uniforms,
  sempre — o conjunto ativo recebe os dados reais do chunk, o conjunto
  inativo recebe a dummy do tipo certo. Elimina tanto a colisão de tipo
  quanto qualquer unidade "vazia" (handle 0) que um sampler ainda declarado
  ativo no programa possa amostrar.
- Isso explica por que um mapa NOVO/vazio ("Test") também quebrava: mesmo
  sem nenhum material configurado, `SubmitToPipeline` já roda o path legado
  (≤4 materiais sempre, ver Bug 124) — o conflito de unidade acontecia
  independente de haver dado real ou não.

**Bug 124.2 encontrado e corrigido — 5º material adicionado ("+ Add Material")
aparece com peso 100% em todo o terreno, sem o dev ter pintado nada:**
`Splatmap::RebuildArrayStorage()` (`tools/gue/src/terrain/splatmap.h`) roda a
cada mudança de contagem de materiais (inclusive ao crescer de 4→5 via
`EnsureMaterialCount`) e SEMPRE destrói e recria `arrayTex` do zero via
`glTexImage3D(..., nullptr)` — que aloca memória de GPU SEM zerar
(conteúdo indefinido/lixo, não garantidamente zero). Isso dependia
inteiramente de uma chamada posterior a `UploadArray()` (via `Upload()`,
disparado no próximo `Update()`/frame) pra sobrescrever esse lixo antes de
qualquer sample no shader — sem nenhuma garantia explícita nesse meio-tempo.
Como o canal de peso do material novo cai exatamente nessa janela (o layer
INTEIRO é realocado do zero, não só o canal novo), um valor de lixo alto no
canal R/G/B/A correspondente ao material recém-adicionado se traduz
diretamente em "peso ~100% em todo lugar" após a normalização do shader
(`wsum`), afogando os pesos reais dos outros materiais.
- **Correção:** `RebuildArrayStorage()` agora chama `glClearTexImage(arrayTex,
  0, GL_RGBA, GL_UNSIGNED_BYTE, zero)` imediatamente após a alocação — zera
  explicitamente TODOS os layers na GPU antes de qualquer outra coisa rodar,
  eliminando a dependência de ordem entre alocação e upload. `SplatLayer`
  individual (a textura 2D por-grupo) já fazia upload explícito de dados
  zerados (`data.assign(...,0u)` + `glTexImage2D(..., data.data())`, nunca
  `nullptr`) — não precisou de correção.

**Bug 124.3 — path novo (>4 materiais) ainda quebra (cinza/preto); desativado
via fallback defensivo, causa raiz não isolada:** mesmo depois de confirmar
por leitura direta de GPU que (a) os pesos do splatmap estão corretos em
múltiplos pontos, (b) as texturas por-material são distintas, (c) toda a
cadeia de bind não gera nenhum `GL error`, e (d) o programa do shader
linka/valida (`GL_LINK_STATUS`/`GL_VALIDATE_STATUS` = 1) nesse estado — o
terreno ainda aparece cinza/preto assim que a 5ª textura é adicionada.
Revisão de todos os limiares `>4`/`<4` (C++ e GLSL) e de todo array de
tamanho fixo no caminho novo não encontrou nenhum operador errado nem
limite menor que `MAX_EXT_MATERIALS=64`. Causa raiz **não isolada** ainda.
- **Fallback aplicado (`editable_terrain.cpp`, `SubmitToPipeline`):**
  `constexpr bool kExtPathEnabled = false;` — o path novo (texture arrays)
  fica desativado; `SubmitToPipeline` SEMPRE usa o path legado exato-4,
  mostrando só os primeiros 4 materiais quando há mais. Autoria (catálogo,
  pintura do splatmap) continua funcionando pra N ilimitado — só o PREVIEW
  fica limitado a 4 até o bug ser isolado e corrigido (flip
  `kExtPathEnabled = true` quando resolvido).
- Nunca mostra terreno quebrado: pior caso agora é "preview mostra só os 4
  primeiros materiais", nunca cinza/preto.

**Hipótese testada e DESCARTADA — placeholder inválido (GL id 0) nos mapas
normal/roughness/ao/height de slots novos:** pista de outra engine (HTerrain)
sugeria que slots novos podiam ter esses mapas em 0 (nunca atribuídos),
corrompendo o `MaterialTextureArray` inteiro no upload. Revisão de código
(não só do 5º slot, de TODO caminho que constrói um `Material`) mostrou que
`EnsureSlotCapacity`, `SetMaterialSlot`, `ReloadMaterials` e o branch
`hasIds` de `LoadArea` sempre atribuem `defaultNormal_`/`defaultRoughness_`/
`defaultAO_`/`defaultHeight_` (texturas sólidas válidas) — nunca 0.
`MaterialTextureArray::SetLayerFromGLTexture` também já tinha uma guarda
explícita pra `srcTex==0` (preenche branco, não chama `glBindTexture`/
`glGetTexImage` nesse estado). Hipótese não confirmada como causa raiz.
Mesmo assim, adicionado um clamp defensivo em `RebuildMaterialArrays()`
(`editable_terrain.cpp`) — se qualquer material tiver um desses handles em 0
por algum caminho futuro que esqueça de setá-lo, cai pro `default*_`
correspondente antes do upload, nunca um handle 0 alcança o array.
`kExtPathEnabled` continua `false` — essa correção é defesa-em-profundidade,
não a causa raiz encontrada.

**Bug 124.4 encontrado e corrigido — CRÍTICO: qualquer área salva pelo GUE
depois da Fase 1 quebrava o carregamento de splatmap no CLIENTE (jogo),
mesmo com só 4 materiais:** `Splatmap::Save()` (`splatmap.h`) sempre grava o
magic `RSPN` (multi-layer, novo formato) — mesmo pra uma área de 4
materiais (1 layer). O parser de splatmap do cliente
(`client/src/renderer/terrain/terrain.cpp:381-396`, antes desta correção) só
reconhecia os magics legados `RSP2`/`RSPM` — um arquivo `RSPN` não batia com
nenhum dos dois, `ok` ficava `false`, e o splatmap simplesmente nunca era
enviado pra GPU (`splatmap_tex_` ficava no estado anterior/zero) — terreno
carregava sem NENHUMA pintura de material no jogo. Isso não dependia de ter
>4 materiais: bastava salvar QUALQUER área pelo GUE pós-Fase-1 pra quebrar
no client.
- **Correção:** `terrain.cpp` ganhou um novo branch `magic == 0x4E505352`
  (RSPN) — lê o `numLayers` extra do header e carrega **só o layer 0** (W×H×4
  bytes, materiais 0-3), ignorando silenciosamente quaisquer layers extras
  (materiais 4+) sem erro — consistente com o cliente ficar limitado a 4
  materiais nesta fase. `materials.txt` com 5+ linhas já era seguro (loop
  `while (... && materials_.size() < 4)` em `terrain.cpp:454` já parava
  sozinho nos 4 primeiros, sem erro) — não precisou de correção.

**Limitação conhecida, esperada e documentada — o RUNTIME do jogo continua em 4:**
Quando uma área tem >4 materiais, o preview do editor mistura corretamente
qualquer N via o path novo (atualmente desativado, ver Bug 124.3 acima). O cliente do jogo
(`client/src/renderer/terrain/terrain.cpp`) monta só os 4 slots legados a
partir de `materials.txt` (loop `< 4` em `terrain.cpp:454`, e agora lê só o
layer 0 do `RSPN`, ver Bug 124.4) — uma área salva com >4 materiais roda no
jogo usando só os primeiros 4 até a Fase 2. Isso é intencional: o objetivo
desta
fase era só destravar a AUTORIA; o runtime do jogo (que não pode pagar o
custo por-pixel do path generalizado nem, no médio prazo, N sets de texturas
inteiras carregadas) precisa do bake (Fase 2, ver item de investigação
"Terrain material bake / RVT" desta mesma sessão) antes de ganhar >4
materiais de verdade.

**Pendente (Fase 2 — bake pra runtime simplificado):**
- Assar splatmap + materiais autorados (N ilimitado) numa textura
  índice+peso compacta (Variante B da investigação prévia), consumida por um
  shader novo e simples no cliente (2-4 texture fetches por pixel,
  independente de N).
- Até lá, o cliente permanece limitado a 4 materiais por área — comportamento
  inalterado em relação a antes desta sessão.

**Quando atacar:** ao iniciar a Fase 2 (bake), ou se algum dev tentar
efetivamente configurar/pintar >4 materiais numa área que precisa aparecer
correta no jogo (não só no editor) antes disso.
**Estimativa:** Fase 2 é o grosso do trabalho restante — bake FBO offscreen +
shader runtime novo + fluxo de UI "Bake Terrain"; 3-5 dias.

**Bug 124.5 — redesign: separado em DOIS shaders de terreno independentes,
`kExtPathEnabled` reativado:** manter os dois caminhos (legado exato-4 e o
novo N-materiais) coexistindo num ÚNICO programa GLSL foi identificado como a
causa estrutural da fragilidade — não um bug pontual. Uniforms de sampler
persistem no programa entre draw calls; qualquer branch que não roda numa
chamada deixa seus samplers apontando pra alguma unidade, e os dois conjuntos
de samplers (sampler2D legado + sampler2DArray novo) competindo por unidades
no MESMO programa foi a causa raiz confirmada do bug `GL_INVALID_OPERATION`
anterior — além de pressão combinada de orçamento (29 de um total prático de
~32 `GL_MAX_TEXTURE_IMAGE_UNITS`) e da dificuldade de depuração que consumiu
várias rodadas de investigação.
- **`dist/client/shaders/terrainGBuffer.fs`**: revertido — é EXATAMENTE o
  arquivo de antes da Fase 1 (sem `u_numMaterials`, sem `sampler2DArray`, sem
  `MAX_EXT_MATERIALS`, sem as variantes `*Arr` das funções triplanares). Só o
  path de 4 materiais. Usado pelo cliente do jogo e pelo editor quando
  `materials_.size() <= 4`.
- **`dist/client/shaders/terrainGBufferExt.fs`** (novo arquivo): só o path
  generalizado (sampler2DArray + blend de N materiais). Duplica as funções
  auxiliares (`stochCoords`, `sampleStochArr`, `triplanarArr`/`RArr`/`GArr`/
  `NormalArr`, `octEncode`) — duplicação aceita conscientemente, é pouco
  código e raramente mexido. Unidades de textura reiniciam do 0 (0=splat
  array, 1-5=arrays de material, 6=macro, 7=heightmap) — não competem mais
  com o legado, cada programa tem seu próprio orçamento.
- **`shared/renderer/src/compile_shaders.cpp`**: registra
  `Shader::shaders["terrainGBufferExt"]` compilado com o mesmo
  `terrainGBuffer.vs` (estágio de vértice idêntico) + o novo `.fs`.
- **`shared/renderer/src/pipeline.cpp`** (`terrainPass_`): escolhe o
  programa por chunk — `Shader::shaders[c.num_materials>0 ?
  "terrainGBufferExt" : "terrainGBuffer"]` — e faz bind SÓ dos
  uniforms/texturas do programa escolhido. Removida toda a maquinaria de
  texturas dummy/unidades disjuntas (`ensureTerrainDummyTextures_`,
  `terrainDummyTex2D_`/`terrainDummyTexArray_` em `pipeline.h`) e o acessor
  temporário `Shader::DebugProgramID()` — não fazem mais sentido com
  programas separados.
- **`tools/gue/src/terrain/editable_terrain.cpp`** (`SubmitToPipeline`):
  `kExtPathEnabled` voltou a `true` — com os dois shaders isolados, o path
  novo não tem mais como contaminar o legado.
- Logs de diagnóstico `TerrainCheckGL_`/`[gl-err]` em `splatmap.h`/
  `material.h` foram mantidos (são checks de erro genéricos, silenciosos em
  caso de sucesso) — não fazem parte do mecanismo de coexistência removido.

**Bug 124.6 encontrado e corrigido — CAUSA RAIZ FINAL do cinza/NaN no shader
novo: `Set1FloatArray("u_tilingsExt", ...)` e `Set1FloatArray("u_matNormalStrengthExt", ...)`
faltavam o sufixo `"[0]"` no nome do uniform:** `Shader::initUniforms()`
(`shader.cpp:107-123`) enumera os uniforms ativos via `glGetActiveUniform` e
guarda no mapa `Uniforms` a chave EXATA que o driver retorna — pra um
uniform array, essa chave SEMPRE inclui `[0]` (ex: `"u_tilingsExt[0]"`,
nunca o nome nu `"u_tilingsExt"`; comportamento padrão de reflexão GLSL, não
peculiaridade de driver). `Shader::loc()` (`shader.cpp:138-143`) faz um
lookup exato no mapa e retorna `-1` silenciosamente em release quando não
acha (só dispara `assert` em debug). `glProgramUniform1fv(prog, -1, ...)`
com location `-1` é um no-op definido pelo spec — ou seja,
`u_tilingsExt`/`u_matNormalStrengthExt` **nunca eram de fato setados**,
ficando em `0.0` (default GLSL) em todos os elementos. Dividir posição de
mundo por um tiling `0.0` produz Inf/NaN — exatamente o que o marcador
magenta de NaN capturou, e a causa do cinza uniforme antes disso (UV
degenerada por divisão por zero).
- **Confirmado pelo próprio padrão já usado no arquivo**: `ssaoPass_` e
  `volumetricPass_` já chamam `Set1FloatArray("kernel[0]", ...)` e
  `Set1FloatArray("offsets[0]", ...)` — com o sufixo — precisamente por essa
  razão. As chamadas novas do path Ext (`u_tilingsExt`/`u_matNormalStrengthExt`)
  foram as únicas no arquivo que omitiram o sufixo.
- **Correção**: `pipeline.cpp` (`terrainPass_`, branch Ext) — `Set1FloatArray("u_tilingsExt[0]", ...)`
  e `Set1FloatArray("u_matNormalStrengthExt[0]", ...)`.
- Isso explica por que o teste de coordenada (`wp.xz / u_tilingsExt[0]`) deu
  magenta (divisão por zero) enquanto os testes anteriores sem essa divisão
  (RMA fixo, normal fixa, albedo puro sem tiling) davam cinza mas não
  disparavam o marcador — o cinza vinha de UVs degeneradas alimentando
  `texture()`/`textureGrad()` com coordenadas Inf, não de um NaN aritmético
  simples nos valores escalares testados antes.

**Item de colisão anotado, NÃO investigado (reportado pelo dev):** objetos
de "ponte" não são escaláveis/atravessáveis pelo jogador — suspeita de
interação entre a colisão do objeto (ponte) e algum ajuste anterior de
colisão de terreno/montanha, impedindo o jogador de subir na ponte. Ver item
125 abaixo.

## 125. COLISÃO: objetos de ponte não são escaláveis/atravessáveis pelo jogador

**Estado:** reportado pelo dev — jogador não consegue subir/atravessar
objetos de ponte no jogo.

**Suspeita (não confirmada):** interação entre a colisão do objeto (ponte) e
algum ajuste anterior de colisão de terreno/montanha, impedindo o jogador de
subir na ponte.

**NÃO investigado ainda** — só reportado. A investigar em sessão futura.

**Quando atacar:** próxima sessão dedicada a colisão/movimentação do
jogador.

