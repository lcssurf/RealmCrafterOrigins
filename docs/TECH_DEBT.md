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
