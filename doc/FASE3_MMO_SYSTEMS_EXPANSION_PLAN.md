# Fase 3 - MMO Systems Expansion (Plano Detalhado e Expandido)

Objetivo da fase: sair de um vertical slice forte para um loop MMO realmente jogavel em grupo, com base server-authoritative, modular, data-driven e pronta para escalar sem virar acoplamento no `main.cpp` e no `client.go`.

## 0) Resultado esperado da fase

No fim da Fase 3, o projeto deve ter:

1. Quests funcionais ponta a ponta (definicao, progresso, recompensas, UI, persistencia).
2. Party funcional (convite, aceitar/recusar, sair, kick, sync de membros e estados basicos).
3. Projectile runtime funcional e autoritativo (spawn, travel, impacto, dano, efeitos).
4. Weather dinamico por area com sincronizacao server -> client e transicao visual estavel.
5. Appearance update em runtime (equip/unequip e overrides visuais refletidos sem reconnect).
6. Observabilidade e testes minimos por sistema para impedir regressao silenciosa.
7. Sessao de combate moderna (estilo Throne and Liberty) com base autoritativa e data-driven.

## 1) Regras globais obrigatorias (reafirmadas)

Estas regras ja acordadas seguem valendo em 100% da fase:

1. Refatoracao continua e incremental.
- Toda mudanca relevante entrega feature + melhoria estrutural no trecho tocado.
- Sem big-bang rewrite.

2. Validacao e organizacao de codigo.
- Codigo centralizado por dominio, com ownership claro de modulo.
- Fluxo de validacao minimo por entrega: build + smoke + checklist funcional.

3. Server-authoritative por padrao.
- Cliente nunca e fonte final de verdade para estado de gameplay.
- Cliente e apresentacao + input; servidor valida e decide.

4. Evitar hardcode de gameplay/config.
- Regras e tuning vao para DB, config versionada ou scripts Lua.
- Hardcode permitido apenas para fallback tecnico seguro.

5. Graphify-first para descoberta e impacto.
- Antes de mexer, mapear pontos de impacto no grafo e confirmar limites.

6. Paridade obrigatoria de tooling (GUE-first para operacao).
- Toda nova feature da Fase 3 so fecha com fluxo correspondente no GUE.
- Nao vale "feature pronta so por codigo": precisa criar/editar/duplicar/validar no editor.
- Se uma feature tocar dados novos (DB/config), o painel e validacao GUE entram no mesmo ciclo.

## 2) Estado atual (baseline tecnico)

Resumo do estado atual do codigo:

1. `PAreaConfig` autoritativo esta ativo e estavel.
2. Weather existe no schema de `area_config` (probabilidades), mas sem runtime completo de clima dinamico durante jogo.
3. Pacotes alvo da Fase 3 ainda nao estao implementados no fluxo atual:
- `PWeatherChange` (17)
- `PQuestLog` (23)
- `PProjectile` (37)
- `PPartyUpdate` (38)
- `PAppearanceUpdate` (39)
4. Fluxo de rede em gameplay esta concentrado em `server/internal/net/client.go` e `client/src/core/main.cpp`.
5. Existe base Lua para spells/eventos NPC, que pode ser reaproveitada para quests e gatilhos de progresso.

## 3) Direcao de arquitetura da Fase 3

Meta arquitetural da fase:

1. Reduzir acoplamento dos hotspots atuais.
2. Encapsular cada sistema MMO em modulo proprio.
3. Definir contratos de protocolo e dados por sistema.
4. Criar base para evolucao futura sem inflar `main.cpp` e `client.go`.

### 3.1 Modulos alvo (server)

Criar/organizar modulos por dominio em `server/internal/mmo/`:

1. `quests/`
2. `party/`
3. `projectiles/`
4. `weather/`
5. `appearance/`
6. `combat/`
7. `skills/`

Cada modulo deve expor:

1. Service interface.
2. DTOs/eventos locais.
3. Adaptador de persistencia (via `db.DB`).
4. Serializacao de pacotes do dominio (ou helpers dedicados).

### 3.2 Modulos alvo (client)

Criar camadas em `client/src/` para tirar logica de gameplay de `core/main.cpp`:

1. `gameplay/quests/`
2. `gameplay/party/`
3. `gameplay/projectiles/`
4. `gameplay/weather/`
5. `gameplay/appearance/`
6. `gameplay/combat/`
7. `gameplay/skills/`
8. `ui/quests/` e `ui/party/` para HUDs dedicadas.
9. `ui/combat/` e `ui/skills/` para HUD e barras de combate modernas.

### 3.3 Contrato de protocolo

Pacotes alvo da fase:

1. `17  PWeatherChange`
2. `23  PQuestLog`
3. `37  PProjectile`
4. `38  PPartyUpdate`
5. `39  PAppearanceUpdate`

Pacotes novos de acao C->S (se necessarios, extensoes RCO):

1. `124 PQuestAction` (accept, abandon, turn-in request)
2. `125 PPartyAction` (invite, accept, decline, leave, kick, transfer lead)
3. `126 PCombatAction` (dodge, guard, parry_start/end, interrupt attempt)
4. `127 PSkillLoadoutAction` (equip/unequip skill, swap preset, specialize)

Pacotes novos S->C recomendados para C.5/C.6:

1. `128 PCombatEvent` (hit_type, blocked, parried, interrupted, resisted, break)
2. `129 PSkillState` (cooldowns, charges, chain windows, lockouts)
3. `130 PWeaponMasteryUpdate` (xp, level, points, unlocks)
4. `131 PStatusEffectDelta` (buff/debuff add/update/remove)

Regra: payload versionado e validado dos dois lados, com parsing robusto e fallback seguro.

## 4) Escopo e fora de escopo

### 4.1 Escopo principal

1. Quests, Party, Projectile, Weather runtime, Appearance update runtime.
2. Persistencia, protocolo, UI minima funcional e integracao Lua necessaria.
3. Combat session TL core (defesa ativa, interrupt, status effects, chain rules).
4. Skill system + HUD overhaul (classe+arma+EXP de arma, loadouts e legibilidade).
5. Observabilidade minima e testes de regressao por sistema.

### 4.2 Fora de escopo nesta fase

1. Guild completo.
2. Auction house/economia avancada.
3. Sistema completo de raids/dungeons com matchmaking.
4. Otimizacao final pesada de performance (fica como foco principal posterior).

### 4.3 Lacunas adicionais detectadas (analise expandida)

Mesmo com Quests/Party/Projectile/Weather/Appearance, ainda faltam blocos importantes para um MMO "liveable" mais completo.

#### P0 - Recomendado entrar na Fase 3 (alta prioridade)

1. Chat de grupo e social basico completo.
- Chat `party` real (nao apenas canal generico), whisper/tell e mute local.
- Moderacao minima server-side (flood/rate-limit por conta/sessao).

2. Trade player-to-player autoritativo.
- Convite de trade, lock de oferta, confirmacao em dois passos, anti-dupe.
- Logs de auditoria para rollback de abuso/exploit.

3. Regras de loot em grupo.
- Round-robin/need-greed/free-for-all configuravel por party.
- Protecao contra ninja-loot em interacoes concorrentes.

4. Sistema de status effects (buff/debuff/DoT/HoT/CC).
- Base de combate moderna exige aura/state machine alem de dano instantaneo.
- Deve ser data-driven (DB/Lua), com stacking e expiracao server-side.

5. Cast bar e interrupcao.
- Sem isso, projectile/skill feel fica incompleto para gameplay 2026.
- Integrar com movement, stun/silence, damage interruption rules.

6. Respawn/bindpoint e penalidade de morte configuravel.
- Fluxo completo de morte/retorno, com opcao de bind area/checkpoint.

#### P1 - Recomendado preparar no fim da Fase 3 (ou Fase 3.5)

1. Guild/friends/ignore.
- Estruturas sociais permanentes, convites, permissao e presencia.

2. Economia basica persistente.
- Banco/storage pessoal.
- Mail simples com anexos.
- Trade logs consultaveis por admin.

3. Progressao expandida.
- Talentos/passivas ou especializacoes (alem do level baseline).
- Achievements/metas de progressao horizontal.

4. Conteudo dinamico de mundo.
- Eventos por area/horario.
- Day-night baseline sincronizado server->client.

5. Mount/pet runtime.
- Ja existe sinal de suporte em dados (`is_mountable`), falta sistema online completo.

#### P2 - Fora da Fase 3 (planejar para fases seguintes)

1. Auction house/marketplace completo.
2. Instanciamento de dungeons e matchmaking robusto.
3. Multi-shard/cross-server social.
4. LiveOps avancado (feature flags remotos, rollout por cohort, telemetry dashboard completo).
5. Pipeline de patching/launcher e versionamento de conteudo em larga escala.

### 4.4 Diagnostico tecnico completo (baseline real do codigo)

Leitura direta do codigo (server/client/GUE/DB) mostra o seguinte estado:

1. Protocolo ainda sem contratos MMO da Fase 3.
- `server/internal/protocol/packets.go` e `client/src/net/protocol.h` vao ate `PClientWorldReady = 123`.
- Ainda nao existem: `PWeatherChange`, `PQuestLog`, `PProjectile`, `PPartyUpdate`, `PAppearanceUpdate`.

2. Runtime server centralizado em um handler monolitico.
- `server/internal/net/client.go` concentra dispatch + handlers de gameplay.
- Hoje ha handlers para movimento/chat/inventario/combate/spell/shop/dialog, mas nao ha handlers dedicados para quest/party/weather/projectile/appearance-delta.

3. Chat atual nao cobre social MMO basico.
- `handleChatMessage` faz entrega para self + broadcast de area.
- Nao existe fluxo separado para `party`, `whisper`, `ignore`, rate-limit ou mute server-side.

4. Weather existe no schema e no editor, mas nao no ciclo runtime.
- `area_config` ja carrega `weather_rain/snow/fog/storm/wind`.
- GUE ja edita probabilidades de clima.
- Ainda nao existe estado dinamico por area com transicao e broadcast de mudanca em runtime.

5. Appearance existe no spawn/snapshot, mas nao em delta online.
- `PStartGame` e `PNewActor` carregam appearance completo.
- Falta pacote de update incremental para equip/unequip/override em runtime.

6. Base de scripting existe, mas ainda sem event bus de quest loop.
- Registry suporta `Spell`, `npc_interact`, `npc_choice`, `player_action`.
- `FireEvent` existe, mas ainda nao esta plugado no loop de kill/pickup/explore para alimentar quest progression de forma central.

7. Client ainda tem hotspot em `main.cpp`.
- Parsing de muitos pacotes e orquestracao de gameplay/render seguem concentrados em `client/src/core/main.cpp`.
- Ainda nao existem modulos `client/src/gameplay/quests|party|projectiles|weather|appearance`.

8. UI de MMO systems ainda nao existe.
- `client/src/ui` cobre login, char select, inventario, chat, spellbar e efeitos.
- Ainda nao existem `ui/quests` e `ui/party`.

9. GUE ainda sem telas de quest/party/projectile templates.
- `tools/gue/src/tabs` cobre areas/media/items/spells/spawn/zones/input.
- Ainda nao ha editores de quest defs, party rules e projectile templates dedicados.

10. Validacao automatizada ainda e lacuna critica.
- Nao foram encontrados testes automatizados (`*_test.go`, `*_test.cpp`) para os dominios de gameplay/rede.
- Para uma fase de alto impacto, codec tests + testes de dominio viram obrigatorios.

### 4.5 Mapa de impacto por sistema (arquivos de maior mudanca)

1. Quests.
- Server: `server/internal/net/client.go`, `server/internal/db/db.go`, novo `server/internal/mmo/quests/*`.
- Client: `client/src/core/main.cpp`, novo `client/src/gameplay/quests/*`, novo `client/src/ui/quests/*`.
- Tools: novos tabs no GUE para definicao de quest.

2. Party.
- Server: novos handlers de acao + dominio `server/internal/mmo/party/*`.
- Client: parser/UI de convites/lista de membros e integra chat party.
- DB: opcional persistencia curta (se decidido) para reconnect seguro.

3. Projectile.
- Server: simulacao autoritativa em `server/internal/mmo/projectiles/*` + integracao com spells/combat.
- Client: visual/interpolacao desacoplada de spell hardcode.
- Protocol: contrato de spawn/update/impact com reconciliacao.

4. Weather.
- Server: scheduler de clima por area + emissao de eventos.
- Client: blending visual por estado/intensidade sem quebrar legibilidade.
- DB/GUE: reaproveita campos existentes, sem hardcode de estados.

5. Appearance update.
- Server: calculo de delta por ator + emissao de pacote incremental.
- Client: apply path sem reset indevido de animacao.
- Cache: preservar pipeline de materiais/meshes para evitar hitch.

6. Blocos P0 complementares (recomendado nesta fase).
- Chat social baseline, trade autoritativo, loot rules de grupo, status effects e cast/interruption.
- Esses blocos compartilham partes centrais de combate/rede e evitam retrabalho se entrarem junto da Fase 3.

7. Skills + HUD (novo impacto alto).
- Server: novo dominio de skills/mastery e validacoes de loadout/combat action.
- Client: modulos de HUD de combate e skill state desacoplados de `main.cpp`.
- DB/GUE: defs de skill, regras de especializacao e curvas de mastery por arma.

### 4.6 Sequenciamento recomendado em ondas (mais seguro para alto impacto)

1. Onda 0 - Foundation obrigatoria.
- Extracao de dominios (tirar peso de `client.go` e `main.cpp`).
- Fechar contratos de pacotes da fase + codec tests.
- Criar observabilidade basica por dominio.

2. Onda 1 - Core social/combat shared.
- Party baseline + party chat.
- Framework de status effects + cast/interruption (base compartilhada com quest/projectile).
- Regras de loot em grupo e validacao de permissao.

3. Onda 2 - Quest loop completo.
- Tabelas de quest + runtime de progresso + hooks de evento (kill/pickup/dialog/explore).
- UI quest log/tracker e persistencia em relogin.

4. Onda 3 - Projectile + Appearance runtime.
- Projectile autoritativo (spawn/travel/impact/reconcile).
- Appearance delta em runtime (equip/override sem reconnect).

5. Onda 4 - Skills + HUD.
- Mastery por arma, especializacao e sincronizacao de skill state.
- HUD de combate completo (cadeias, recursos, status, party readability).

6. Onda 5 - Weather dinamico + hardening final.
- Estado de clima por area com transicao runtime.
- Abuse tests, reconnect recovery, checklist final da fase.

### 4.7 Sessao de combate (estilo Throne and Liberty)

Objetivo: elevar o combate para um padrao action-MMO moderno, mantendo legibilidade, resposta de input e autoridade do servidor.

#### 4.7.1 Principios de design da sessao

1. Combate hibrido e responsivo.
- Base em alvo/lock com espacos para skillshot e posicionamento.
- Nao depender de tab-target puro nem de hit-scan simplista para tudo.

2. Defesa ativa com janela de habilidade.
- Esquiva com i-frames e custo de stamina/recurso.
- Block/parry com janela de timing e recompensa por execucao correta.

3. Telegraph e leitura de combate.
- Ataques perigosos precisam ser telegrafados (animacao/VFX/SFX/tempo de cast).
- Jogador precisa entender "o que aconteceu" em hit, miss, block, parry, interrupt.

4. Combos e skill chains.
- Habilidades com condicoes de follow-up (estado do alvo, janela temporal, buff ativo).
- Sem hardcode fixo no client: regras de chain devem ser data-driven.

5. CC com DR (diminishing returns).
- Stun/root/knockdown/silence/slow com categorias.
- DR por categoria para evitar perma-CC em PvP/PvE.

6. Pacing 2026.
- Time-to-kill, cadencia, janela de reacao e mobilidade calibrados para gameplay moderno.

#### 4.7.2 Subsistemas obrigatorios da sessao de combate

1. Defense core.
- `dodge`, `guard`, `perfect guard`, `guard break`, `interrupt`.

2. Resource core.
- stamina/resolve para defesa e mobilidade.
- custo/regen configuravel por perfil de classe.

3. Status effect core.
- buff/debuff/DoT/HoT/CC como state machine server-side.
- stacking, prioridade, refresh e expiracao centralizados.

4. Reaction core.
- hit reaction, poise/super-armor, cancel windows, cast interruption.

5. Combat messaging core.
- eventos claros para UI/HUD/feedback: hit type, blocked, parried, interrupted, resisted.

#### 4.7.3 Contrato server-authoritative (combate)

1. Cliente envia intencao; servidor valida e resolve.
- cliente nunca confirma hit/cc/interrupt por conta propria.

2. Regras de anti-exploit obrigatorias.
- validacao de cooldown/range/angulo/linha de visao/estado de movimento.
- idempotencia para evitar duplo processamento em pacotes duplicados.

3. Tuning fora de hardcode.
- parametros em DB/config/Lua (janela de parry, i-frame, custo de stamina, DR, etc.).

#### 4.7.4 DoD da sessao de combate

1. Esquiva, block/parry e interrupt funcionam com validacao server-side.
2. Skill chains funcionam por regra de dados (sem logica fixa em `main.cpp`).
3. Status effects e DR estao ativos e consistentes entre clientes.
4. Logs/counters permitem diagnosticar miss, block, parry, interrupt e CC aplicado.
5. Combate se mantem legivel e responsivo em PvE e em testes de PvP interno.

### 4.8 Skills e HUD (modelo recomendado da fase)

Pergunta de produto: "classe, arma ou EXP de arma?"

Resposta recomendada: modelo hibrido com prioridade em arma.

1. Classe = identidade base, nao jaula de skills.
- Classe/arquetipo define papel macro (tank, bruiser, ranger, mage, support).
- Classe entrega passivas de identidade, crescimento de atributos e especialidade.
- Classe nao deve travar a maior parte das skills ativas.

2. Arma = fonte principal de kit de combate.
- Skills ativas e passivas principais vem do conjunto de armas equipadas.
- Build nasce da combinacao de armas + especializacao.
- Mantem flexibilidade de gameplay moderno e evita rigidez de "classe fixa".

3. EXP de arma = progressao horizontal importante.
- Cada tipo de arma possui trilha propria de mastery.
- Arma ativa recebe EXP total; arma secundaria recebe fracao configuravel (ex.: 50%).
- Mastery libera passivas/nos de especializacao e modifica estilo de jogo.

4. Skill expression = chain + especializacao.
- Skill base + modificadores de chain/efeito/custo/tempo.
- Regras data-driven por arma/classe/situacao de combate.

5. Regra global.
- Cliente so apresenta HUD e input.
- Validacao, custo, cooldown, chain gate e resultado final ficam no servidor.

#### 4.8.1 Estrutura de progressao recomendada

1. Nivel de personagem.
- Power baseline, acesso a conteudo e atributos globais.

2. Mastery por arma.
- Progressao por uso/EXP com pontos para arvore de mastery.
- Mastery altera performance, utilidade e defesa daquela arma.

3. Especializacao de skill.
- Cada skill ativa possui variantes (ex.: burst, sustain, controle, mobilidade).
- Escolhas de variante ficam salvas por loadout.

4. Passivas de arquetipo.
- Bonus de identidade por classe (sem invalidar liberdade por arma).

#### 4.8.2 HUD de combate (alvo da fase)

1. Centro de combate.
- Reticulo/lock, telegraph warnings, prompts de interrupt/parry.

2. Barras de recurso.
- HP, EP/mana, stamina/guard e cast bar.
- Indicadores de break state, CC e imunidades temporarias.

3. Barras de skill.
- Barra principal (arma ativa) + barra secundaria (arma de apoio).
- Cooldown radial, stack count, estado de chain disponivel.

4. Buff/debuff frames.
- Player, target e focus com duracao, stacks e tipo de efeito.

5. Party HUD.
- HP/status dos membros com destaque para alvo de cura/suporte.

6. Combat log sintetico.
- Eventos criticos: block, parry, interrupt, resist, CC aplicado/removido.

#### 4.8.3 DoD de skills + HUD

1. Build por arma e mastery jogavel sem hardcode de regras no client.
2. Arquetipo influencia identidade sem limitar buildcraft principal.
3. HUD mostra o necessario para tomada de decisao em combate de alto ritmo.
4. Feedback de chain/interrupt/defesa ativa esta claro e consistente.
5. Tudo com contrato server-authoritative e tuning data-driven.

## 5) Plano detalhado por etapas (A ate I)

## Etapa A - Foundation, contratos e desacoplamento inicial

Objetivo: preparar terreno para os sistemas novos sem quebrar o que ja funciona.

Status atual (2026-05-12):

1. Feito:
- Contratos de protocolo da Fase 3 adicionados em `server/internal/protocol/packets.go` e `client/src/net/protocol.h` (pacotes 17, 23, 37, 38, 39 e extensoes 124-131).
- Dispatch in-game no server desacoplado para metodo dedicado (`dispatchInGamePacket`).
- Handlers iniciais de acao in-game criados em `server/internal/net/ingame_actions.go`:
  - `handleQuestAction`
  - `handlePartyAction`
  - `handleCombatAction`
  - `handleSkillLoadoutAction`
- Extracao inicial no client: gate de pacotes da Fase 3 movido de `main.cpp` para `client/src/gameplay/ingame_packet_gate.*`.
- Testes de codec dedicados para payloads novos adicionados em `server/internal/net/ingame_actions_test.go`.
- Checklist formal de smoke da Etapa A criado em `doc/FASE3_ETAPA_A_SMOKE_CHECKLIST.md`.
- Matriz de paridade feature -> GUE versionada em `doc/FASE3_GUE_PARITY_MATRIX.md`.
- Validacao tecnica executada:
  - `go test ./...` (server) ok.
  - build do client ok.

2. Pendente nesta etapa:
- Rodada manual do smoke funcional completo (login -> selecao -> world -> movimento -> combate) usando checklist formal.
- Extracao adicional de hooks de gameplay do `main.cpp` para modulos `gameplay/*` conforme os sistemas entrarem (Etapas B+).

### A.1 Descoberta e mapeamento

1. Rodar Graphify para mapear fluxos atuais em:
- `server/internal/net/client.go`
- `server/internal/db/db.go`
- `server/internal/world/*`
- `client/src/core/main.cpp`
- `client/src/net/*`
2. Listar pontos de extensao por sistema e riscos de regressao.

### A.2 Refactor estrutural minimo

1. Extrair handlers de gameplay por dominio do `client.go` para arquivos dedicados.
2. Extrair parsing/render hooks de gameplay do `main.cpp` para modulos `gameplay/*`.
3. Manter comportamento atual identico apos extracao.

### A.3 Contratos de protocolo

1. Definir payloads dos pacotes da Fase 3.
2. Atualizar `server/internal/protocol/packets.go` e `client/src/net/protocol.h`.
3. Adicionar testes de codec (encode/decode simetrico e casos invalidos).

### A.4 Gate de qualidade da etapa

1. Build `client`, `server` e `gue` (quando afetado).
2. Smoke login -> selecao -> world enter -> movimento -> combate.
3. Sem regressao do fluxo atual.
4. Matriz de paridade feature -> GUE criada e versionada para todas as etapas da fase.

DoD Etapa A:

1. Base modular inicial criada.
2. Contratos de pacote fechados e testados.
3. Nenhuma regressao no core loop atual.
4. Regra de paridade GUE operacionalizada (template/checklist ativo para o resto da fase).

## Etapa B - Quests ponta a ponta

Objetivo: entregar sistema de quests funcional, autoritativo e extensivel.

Status atual (2026-05-12):

- Concluido:
1. Migração `migrateV18` criada com tabelas:
`quest_defs`, `quest_objective_defs`, `quest_reward_defs`, `character_quests`, `character_quest_progress`.
2. Seed baseline de quest de teste (`training_camp_cleanup`) com objetivo e recompensa.
3. Handler `PQuestAction` ligado ao runtime real:
accept, abandon e turn-in com idempotencia.
4. `PQuestLog` snapshot server->client implementado e enviado no world-enter.
5. Progressão por eventos integrada:
kill NPC, pickup item, talk/interact NPC e explore (login/portal).
6. Recompensas de turn-in aplicadas (XP, gold, itens) com atualização de HUD/inventory.
7. Testes automatizados adicionados em `server/internal/db/quests_test.go`
(fluxo accept -> progress -> complete -> turn-in + idempotencia).
8. Client Quest UI conectada:
`PQuestLog` parseado no client com `Quest Journal` + `Quest Tracker` e ações de accept/abandon/turn-in.
9. Snapshot de quest ampliado para incluir lista de quests disponiveis para aceite no client.
10. `PQuestLog` evoluido para modo hibrido:
snapshot inicial + delta incremental (upsert/remove para quest log e lista de disponiveis).
11. Paridade de tooling no GUE implementada para Quests:
aba `Quests` com CRUD completo de quest, objetivo e recompensa, duplicacao de quest e toggle de ativacao, com validacoes inline.

- Pendente nesta etapa:
1. Rodada final de smoke funcional (GUE -> server -> client) para fechamento oficial da etapa.

### B.1 Modelo de dados

Criar estrutura em DB (ou migracao em `db.go`) com tabelas base:

1. `quest_defs`
2. `quest_objective_defs`
3. `quest_reward_defs`
4. `character_quests`
5. `character_quest_progress`

Campos essenciais:

1. Tipo de objetivo (kill, collect, talk, explore, interact).
2. Estado (available, active, completed, turned_in, failed).
3. Progresso atual e alvo por objetivo.
4. Regras de pre-requisito e repetibilidade.

### B.2 Runtime server

1. `mmo/quests` com:
- carga de definicoes,
- validacao de aceite,
- tracking de progresso por evento,
- conclusao e entrega de recompensa.
2. Hooks de evento:
- kill NPC,
- pickup item,
- dialog opcao,
- entrada em area trigger.
3. Validacao 100% server-side para impedir spoof de progresso.

### B.3 Protocolo quest

1. `PQuestLog` para snapshot/delta de estado.
2. `PQuestAction` para intencoes do cliente.
3. Regras de idempotencia e retry seguro.

### B.4 Client + UI

1. `ui/quests` com:
- quest log,
- tracker ativo,
- feedback de progresso.
2. Atualizacao por push de pacote, sem polling.
3. Fallback visual para quest desatualizada (estado invalido).

### B.5 Lua/GUE integracao minima

1. Expor API Lua para conceder/avancar quest em scripts.
2. Permitir edicao inicial de quest defs via GUE (mesmo que simples).

DoD Etapa B:

1. Jogador aceita quest, progride, conclui e recebe recompensa.
2. Estado persiste entre relogins.
3. Fluxo e resiliente a pacote duplicado/perdido.

## Etapa C - Party system completo baseline

Objetivo: habilitar jogabilidade em grupo com estado sincronizado e seguro.

### C.1 Dominio server

1. `mmo/party` com:
- convite,
- aceitar/recusar,
- sair do grupo,
- kick,
- troca de lider.
2. Regras de validacao:
- limite de membros por party,
- convites validos por estado/distancia/contexto.

### C.2 Persistencia e ciclo de vida

1. Definir se party e so em memoria (baseline) ou persistida curta.
2. Manter consistencia em disconnect/reconnect.
3. Garantir limpeza de estado em edge cases.

### C.3 Protocolo party

1. `PPartyUpdate` para snapshot/delta de membros e lider.
2. `PPartyAction` para comandos do cliente.
3. Mensagens de erro de negocio explicitamente codificadas.

### C.4 UI e UX

1. `ui/party` com lista de membros, HP basico e lider.
2. Feedback visual de convites e mudancas de estado.

DoD Etapa C:

1. Grupo fecha ciclo completo sem relogar.
2. Estados de party permanecem coerentes para todos os membros.
3. Nenhum cliente consegue forcar alteracao ilegal de party.

Status de implementacao (2026-05-13):

1. Baseline server-authoritative implementado em `server/internal/net/party_runtime.go`:
- convite, aceitar, recusar, sair, kick e transferir lider.
- validacao de lideranca, party cheia, auto-invite e alvo fora de contexto.
- limpeza de estado de party/convites no disconnect.
2. `PPartyUpdate` snapshot funcional:
- payload com party_id, lider, membros (rid/nome/level/hp) e convite pendente.
- payload inclui `notice_code` para resultado/erro de negocio codificado.
- envio inicial no world-enter e sincronizacao apos cada acao de party.
3. `PPartyUpdate` evoluido para sincronizacao hibrida:
- snapshot inicial + delta incremental (upsert/remove de membros), mantendo `notice_code` e `notice`.
4. Divisao de XP em party por proximidade implementada:
- membros elegiveis = mesma party, online/in-game, vivos, mesma area e dentro do raio.
- XP dividido entre elegiveis com resto atribuido ao killer.
5. Client baseline implementado:
- `ui/party_panel.*` com lista de membros, HP e status de lider.
- aceite/recusa de convite e acoes de party por UI.
- comandos rapidos de chat (`/party ...`) conectados ao `PPartyAction`.
6. Testes automatizados adicionados:
- `server/internal/net/party_runtime_test.go` cobrindo invite/accept, regras de lider e cleanup.
- `server/internal/net/party_runtime_delta_test.go` cobrindo snapshot/delta/no-op e remocao de membro.
- `server/internal/net/party_xp_test.go` cobrindo elegibilidade por proximidade e distribuicao de resto.
7. UX e validacao de contexto reforcadas:
- mensagens de party diferenciadas por codigo no client (info x erro).
- convite exige mesma area (sem bloqueio por distancia para o baseline).
- distancia fica reservada para regras futuras de gameplay em grupo (ex.: divisao de XP por proximidade).

## Etapa C.5 - Combat Session TL (core de combate moderno)

Objetivo: implementar o nucleo de combate action-MMO da fase, integrado a Party/Quests/Projectile.

Status parcial (2026-05-13):

1. Slice inicial de defesa ativa implementado:
- `PCombatAction` conectado ao runtime real no server (`server/internal/net/combat_runtime.go`).
- acoes baseline: dodge, guard start/end, parry start/end e interrupt.
2. Resolucao de dano melee atualizada:
- `world.ProcessAttack` agora resolve `dodge/parry/guard` server-side antes de aplicar dano.
- guard reduz dano e consome EP por hit absorvido.
3. Feedback de combate habilitado:
- `PCombatEvent` emitido para eventos de defesa/interrupt/resultado de hit.
- client parseia `PCombatEvent` e exibe feedback em chat.
4. Comandos de teste no client:
- `/combat dodge`
- `/combat guard on`
- `/combat guard off`
- `/combat parry`
- `/combat interrupt`
5. Cobertura automatizada:
- `server/internal/net/combat_runtime_test.go`
- `server/internal/world/combat_defense_test.go`

### C.5.1 Defense e resource loop

1. Implementar `dodge` com janela de i-frame e custo de stamina.
2. Implementar `guard` com consumo/penalidade e `perfect guard` com timing.
3. Implementar `guard break` e recovery state.

### C.5.2 Status effects e interrupt

1. Criar state machine de efeitos (buff/debuff/DoT/HoT/CC).
2. Implementar cast bar/interruption por dano/CC/regras de skill.
3. Integrar DR por categoria de CC.

### C.5.3 Skill chains e telegraph

1. Habilitar regras de chain/follow-up data-driven.
2. Expor sinais de telegraph para client (cast, warning, commit, recover).
3. Garantir feedback visual/sonoro consistente.

### C.5.4 Hardening do combate

1. Validacoes de range, angulo, LOS, cooldown e estado.
2. Counters e logs por tipo de evento de combate.
3. Testes de abuso (spam, reorder, dup, desync).

DoD Etapa C.5:

1. Sessao de combate TL-like jogavel e consistente.
2. Defesa ativa, interrupt e chains funcionando com autoridade do servidor.
3. Parametros de combate ajustaveis por dados (sem hardcode espalhado).

## Etapa C.6 - Skill System + HUD overhaul

Objetivo: fechar o loop de progressao de skills e legibilidade de combate.

### C.6.1 Skill framework

1. Separar skill defs por dominio: arma, arquetipo, utilitario global.
2. Implementar especializacao de skill (variantes) data-driven.
3. Integrar mastery de arma com gates e bonus de skill.

### C.6.2 Progressao por arma

1. Persistir EXP de arma por personagem.
2. Aplicar ganho de EXP por arma ativa + secundaria (fracao configuravel).
3. Aplicar unlock de nos/passivas de mastery por arma.

### C.6.3 HUD de combate

1. Barra de skills primaria/secundaria com chain state.
2. Cast bar, stamina/guard, break state e indicadores de interrupt.
3. Buff/debuff frames de player/target/party com stack+duracao.

### C.6.4 Hardening de UX e rede

1. Reconciliacao de cooldown e estado de skill (server truth).
2. Eventos de combate padronizados para HUD (hit type, parry, resist, etc.).
3. Testes de clareza visual com F8/F9 e cenarios de combate real.

DoD Etapa C.6:

1. Progressao por arma e especializacao de skill funcionando ponta a ponta.
2. HUD passa a suportar combate moderno sem poluicao excessiva.
3. Jogador consegue ler e reagir ao combate com clareza.

## Etapa D - Projectile server-authoritative

Objetivo: sair de hit instantaneo em casos que pedem travel/impact e abrir base para combate moderno.

### D.1 Modelo do projectile

1. Definir tipo:
- hitscan,
- projectile fisico,
- AoE delayed.
2. Parametros data-driven:
- speed,
- radius,
- lifetime,
- collision mask,
- effect id.

### D.2 Simulacao server

1. `mmo/projectiles` com tick de simulacao.
2. Spawn via acao valida (spell/skill).
3. Colisao server-side com atores/terreno e resolucao de impacto.

### D.3 Protocolo projectile

1. `PProjectile` para spawn/update/impact.
2. Cliente interpola visual sem decidir hit final.
3. Reconciliacao para reduzir discrepancia perceptivel.

### D.4 Integracao visual client

1. Sistema de visual projectile no client desacoplado de spells hardcoded.
2. Ligacao com VFX/SFX por IDs de dados.

DoD Etapa D:

1. Projectile nasce, viaja, impacta e aplica resultado consistente.
2. Latencia nao causa estado quebrado (duplo hit, hit fantasma, etc).

## Etapa E - Weather dinamico por area

Objetivo: tornar clima parte viva do mundo, sincronizado e sem quebrar legibilidade.

### E.1 Dominio weather

1. `mmo/weather` com estado por area:
- clear, rain, storm, fog, wind, snow.
2. Uso de pesos/probabilidades do `area_config` como entrada, nao hardcode.
3. Time slicing para transicoes suaves.

### E.2 Protocolo clima

1. `PWeatherChange` com:
- weather state,
- intensidade,
- duracao/transicao.
2. Envio em:
- world enter,
- change area,
- mudanca de clima runtime.

### E.3 Integracao client/render

1. Aplicar clima no pipeline sem quebrar tuning autoritativo ja feito na Fase 2.
2. Usar blending temporal para evitar pop visual.
3. Garantir legibilidade minima de gameplay sob clima forte.

DoD Etapa E:

1. Clima muda em runtime sem reconnect.
2. Todos os clientes na mesma area veem estado coerente.
3. Clima nao degrada jogabilidade de forma extrema.

## Etapa F - Appearance update em runtime

Objetivo: refletir mudancas visuais de personagem em tempo real para todos.

### F.1 Dominio appearance

1. `mmo/appearance` para resolver estado visual final por ator.
2. Atualizacao disparada por:
- equip/unequip,
- override de script/evento,
- transformacoes temporarias.

### F.2 Protocolo appearance

1. `PAppearanceUpdate` para delta eficiente por ator.
2. Estrategia de payload:
- somente campos alterados,
- snapshot completo em fallback.

### F.3 Client apply path

1. Atualizar visual de atores sem destruir estado indevido.
2. Cache de meshes/materials para evitar hitch de reload.

DoD Etapa F:

1. Mudancas visuais aparecem online para todos.
2. Sem precisar relogar/entrar em area novamente.
3. Sem flicker severo ou reset incorreto de animacao.

## Etapa G - Integracao Lua + GUE orientada a dados

Objetivo: evitar hardcoded e dar autonomia de conteudo.

### G.1 Lua API

1. Quests: conceder, avancar, concluir, falhar.
2. Party: hooks de convite/entrada/saida.
3. Weather: trigger/control opcional por evento.
4. Appearance: overrides temporarios por evento/script.

### G.2 GUE baseline para fase 3

1. Editor minimo de quest defs.
2. Ajuste de parametros de projectile templates.
3. Ajuste de weather profile por area.
4. Editor de skill defs/loadouts/especializacoes por arma.
5. Editor de curvas de mastery (xp thresholds, pontos, unlock gates).

### G.3 Workflow de criacao de skill no GUE (obrigatorio)

Pergunta pratica: "skill nova entra em arma, combate ou projetil?"

Resposta: entra em todos os dominios necessarios, com um fluxo unico no GUE.

1. Skill Base (tab Skills).
- `skill_id`, nome, descricao, tipo, custo, cooldown, cast/recovery, tags.
- Define a identidade da skill e seu comportamento base.

2. Vinculo de Arma (tab Skills > Weapon Binding).
- Qual arma pode usar, nivel de mastery minimo, slot de loadout, passivas relacionadas.
- Define progressao e disponibilidade por arma.

3. Perfil de Combate (tab Skills > Combat Profile).
- hit rules (range/angulo/LOS), block/parry interaction, interruptibilidade, chain windows.
- status effects aplicados (buff/debuff/CC/DR categoria).

4. Perfil de Projetil (tab Skills > Projectile Profile) quando aplicavel.
- projectile template, velocidade, lifetime, colisao, AoE, impacto.
- VFX/SFX binding e fallback visual.

5. Perfil de Animacao (tab Skills > Animation Profile).
- action id, windup/hit/recovery frames, cancel windows, anim events.
- sincronizacao com telegraph e combat timeline.

6. Preview + Validate + Publish.
- preview local no GUE (tooltips, cooldown, chain, anim marker).
- validacao de schema/referencias obrigatoria antes de salvar.
- publish versionado para server/client consumirem sem hardcode.

### G.4 Regra de paridade GUE (gate de fase)

1. Nenhuma feature nova de C.5/C.6/D fecha sem suporte no GUE.
2. Toda tabela/config nova de combate/skills/projetil precisa de:
- tela de edicao,
- validacao inline,
- fluxo minimo de criacao/edicao/duplicacao.
3. PR de gameplay sem update de tooling correspondente fica incompleto.

DoD Etapa G:

1. Conteudo principal pode ser iterado sem recompilar client/server.
2. Parametros de gameplay ficam centralizados e versionaveis.
3. Skill nova pode ser criada ponta a ponta no GUE (arma + combate + projetil/animacao quando houver).
4. Paridade GUE mantida para todas as features da fase.

## Etapa H - Hardening, seguranca e resiliencia

Objetivo: garantir que os sistemas novos nao abram vetores de exploit/regressao.

### H.1 Validacoes server-side

1. QuestAction valida estado e pre-requisito.
2. PartyAction valida permissao e contexto.
3. Projectile spawn valida skill/source/cooldown/range.
4. AppearanceUpdate nao aceita estado arbitrario vindo do client.
5. CombatAction valida estado permitido (stamina, lockout, timing window, LOS, range).
6. SkillLoadoutAction valida ownership/unlocks/mastery antes de aplicar.

### H.2 Resiliencia de rede

1. Idempotencia em acoes sensiveis.
2. Tratamento de reorder/dup de pacotes onde aplicavel.
3. Snapshot de recuperacao em reconnect.

### H.3 Observabilidade

1. Logs estruturados por dominio (quest, party, projectile, weather, appearance, combat, skills).
2. Counters minimos por minuto:
- quest actions,
- party transitions,
- projectile spawns/hits,
- weather transitions,
- appearance updates,
- combat events (block/parry/interrupt/cc/break),
- skill loadout changes + mastery XP ticks.

DoD Etapa H:

1. Sistemas passam em testes de abuso basico.
2. Falhas ficam visiveis em logs/counters, sem diagnostico cego.

## Etapa I - Fechamento da fase

Objetivo: consolidar entrega, documentar e liberar base para proxima fase.

1. Documentar contratos finais de pacotes e payloads.
2. Documentar schema final e migracoes aplicadas.
3. Documentar operacao e fluxo de teste manual.
4. Consolidar matriz final de paridade feature -> GUE (sem lacunas).
5. Mover docs da Fase 3 para `doc/done` quando aprovado.

DoD Etapa I:

1. Checklist de fase 100% verde.
2. Documento de fechamento com riscos restantes e proximos passos.
3. Nenhuma feature da Fase 3 sem fluxo correspondente no GUE.

## 6) Matriz de testes da Fase 3

## 6.1 Testes obrigatorios por PR relevante

1. Build server/client/gue (quando afetado).
2. Testes de codec para pacotes alterados.
3. Smoke login -> world enter -> loop basico.

## 6.2 Testes funcionais por sistema

1. Quests:
- aceitar, progredir, concluir, relogar, continuar progresso.
2. Party:
- convite, aceitar, kick, sair, lider trocar.
3. Combat core:
- dodge/parry/guard/interrupt em timing correto e incorreto.
- aplicacao de CC com DR e expiracao consistente.
4. Skills + mastery + HUD:
- ganho de EXP de arma ativa/secundaria, unlock e especializacao.
- sync de cooldown/chain/state para HUD sem dessync perceptivel.
- criacao de skill nova via GUE (arma + combate + projetil/animacao), sem ajuste manual em codigo.
5. Projectile:
- spawn, travel, impact, AoE, alvo morto no meio do trajeto.
6. Weather:
- troca de clima em runtime e em change area.
7. Appearance:
- equip/unequip em runtime com sync para outros clientes.

## 6.3 Testes de regressao cruzada

1. Combat + quest objective kill.
2. Party + projectile em combate conjunto.
3. Weather ativo durante combate e troca de area.
4. Appearance update durante combate/animacoes.
5. Skill loadout swap durante combate (sem corromper cooldown/chain state).
6. Mastery unlock em combate longo sem regressao de rede/HUD.

## 7) KPIs de sucesso da fase

1. Tempo de implementar nova regra de quest/party cai por modularidade.
2. Menos alteracoes diretas em `main.cpp` e `client.go` para features novas.
3. Taxa de regressao em gameplay/render reduz em testes internos.
4. Fluxo MMO percebido como completo: progressao, grupo, combate dinamico, mundo vivo.

## 8) Ordem recomendada de execucao

1. Etapa A (foundation).
2. Etapa B (quests).
3. Etapa C (party).
4. Etapa C.5 (combat session TL core).
5. Etapa D (projectile).
6. Etapa C.6 (skills + HUD overhaul).
7. Etapa E (weather).
8. Etapa F (appearance).
9. Etapas G/H/I (consolidacao).

## 9) Riscos principais e mitigacao

1. Risco: inflar `client.go` e `main.cpp` novamente.
- Mitigacao: regra de extracao por dominio obrigatoria em toda entrega.

2. Risco: drift de protocolo entre server e client.
- Mitigacao: testes de codec + checklist de sync de constantes.

3. Risco: hardcode espalhado para fechar prazo.
- Mitigacao: review com criterio "data-driven ou justificar fallback tecnico".

4. Risco: regressao silenciosa de gameplay antigo.
- Mitigacao: smoke baseline fixo por entrega + logs de dominio.

5. Risco: dessync entre estado de skill/combat e HUD.
- Mitigacao: pacotes de estado dedicados + reconciliacao visual + testes de rede com perda/reorder.

6. Risco: progressao de mastery desbalancear combate.
- Mitigacao: tuning data-driven versionado + limites por faixa + testes comparativos por arma.

## 10) Criterio de conclusao da Fase 3

A Fase 3 fecha quando:

1. Etapas A-I + C.5/C.6 estiverem concluidas.
2. Quests, Party, Projectile, Weather e Appearance estiverem funcionais e testados.
3. Contratos de rede e schema estiverem documentados e estaveis.
4. A base estiver pronta para seguir para escala de conteudo e melhorias de polish sem reabrir arquitetura.
5. Sessao de combate TL-like estiver funcional, validada e observavel.
6. Skill system e HUD de combate estiverem completos, legiveis e data-driven.


