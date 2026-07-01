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

