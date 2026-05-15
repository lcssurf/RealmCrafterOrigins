# Plano AAA - Combat Ability Runtime (Core + Dados + Script)

Objetivo: evoluir o combate atual para um modelo de MMO AAA, escalavel e sem gambiarra, onde:

- o servidor continua 100% autoritativo;
- conteudo e balanceamento ficam data-driven;
- comportamento de mobs fica em script;
- o motor de combate permanece no core nativo.

---

## 1. Visao de Arquitetura

### 1.1 Principio central

Separar o sistema em tres camadas com responsabilidades claras:

1. Core de simulacao (Go, autoritativo)
- timeline de habilidades;
- validacoes de range, LOS, cooldown, recurso, estado;
- resolucao de hit/parry/block/dodge;
- aplicacao de dano/heal/buffs/debuffs;
- replicacao para clientes.

2. Dados (SQLite/Postgres + GUE)
- templates de habilidade;
- loadout por NPC;
- parametros de telegraph, timing e dano;
- mapeamentos de animacao e VFX/SFX.

3. Script (Lua)
- decisao de IA: quando usar cada habilidade;
- regras de encounter/fase;
- prioridades condicionais de boss;
- composicao de comportamento, sem mutar estado critico direto.

### 1.2 Regra de ouro

- Script decide intencao.
- Core decide verdade.

### 1.3 Contrato unificado de Cast Intent (NPC e Player)

Toda tentativa de habilidade entra no core via um contrato unico:

- `caster_rid`
- `target_rid` (ou ground target)
- `ability_id`
- `action_override` (opcional, ex: `CastHeavy`, `AttackLunge`)
- `reason_tag` (`npc_ai`, `player_input`, `script_combo`, etc)
- `client_trace_id` (opcional para debug)

Fluxo:

1. Origem (script de NPC ou input de player) cria `cast_intent`.
2. Core valida.
3. Core resolve qual animacao tocar:
- usa `action_override` se permitido e valido;
- caso contrario usa defaults da habilidade (`action_windup/impact/recover`).
4. Core executa timeline e replica para todos os clientes.

Resultado:

- NPC e player usam o mesmo pipeline tecnico.
- Conteudo ganha liberdade sem quebrar consistencia de combate.

---

## 2. Estado Atual e Lacunas

### 2.1 O que ja existe

- Combate autoritativo no servidor.
- Eventos de combate via `PCombatEvent`.
- Telegraph visual de especial com circulo fechando no cliente.
- Parry/Guard/Dodge com SP.

### 2.2 O que falta para padrao AAA

- Especial de mob ainda hardcoded em constantes no core.
- Sem tabelas de habilidade de NPC com timelines completas.
- Sem runtime generico de ability stage (windup/impact/recover).
- IA de habilidade ainda acoplada ao loop principal de chase.
- Scripting com um unico estado Lua global protegido por mutex.

---

## 3. Arquitetura Alvo (Target State)

## 3.1 Runtime de habilidade no servidor

Criar um subsistema dedicado:

- `server/internal/combat/ability_runtime.go`
- `server/internal/combat/ability_state.go`
- `server/internal/combat/ability_resolver.go`
- `server/internal/combat/ability_queue.go`

Responsabilidades:

- iniciar habilidade com snapshot de contexto;
- processar transicoes de stage com timestamps;
- resolver janelas de counter (parry, interrupt);
- resolver acoes de animacao por stage com fallback seguro;
- emitir eventos de rede padronizados;
- manter determinismo e auditabilidade.

## 3.2 Modelo de stages

Toda habilidade ofensiva/defensiva usa stages:

1. `windup`
- abre telegraph;
- abre janela de leitura para o alvo.

2. `impact`
- valida counters;
- calcula resultado (hit/parry/block/dodge/miss).

3. `recover`
- aplica lock curto e finaliza estado.

## 3.3 Motor de eventos de combate

Padronizar eventos em enum unica, com payload versionado:

- `event_code`
- `source_rid`
- `target_rid`
- `ability_id`
- `stage`
- `value`
- `text`

Obs: manter compatibilidade backward com o layout atual durante migracao.

## 3.4 Politica de animacao por habilidade

Cada stage pode ter acao default:

- `action_windup`
- `action_impact`
- `action_recover`

Regra de resolucao:

1. `action_override` do intent (quando permitido e existente no actor def);
2. default da habilidade no stage;
3. fallback global (`Attack`/`Cast`/`Idle`).

Nunca confiar no cliente para decidir animacao final.
Animacao nunca altera timeline autoritativa de hit/counter/dano.

---

## 4. Modelo de Dados (DB) - Data Driven

## 4.1 Tabelas novas

1. `ability_templates`
- `id` (PK)
- `name`
- `family` (`melee_special`, `spell`, `defensive`, etc)
- `resource_type` (`mp`, `sp`, `none`)
- `resource_cost`
- `cooldown_ms`
- `range_min`
- `range_max`
- `windup_ms`
- `impact_delay_ms`
- `recover_ms`
- `parry_window_ms`
- `interruptible` (bool)
- `base_damage_min`
- `base_damage_max`
- `damage_stat_scale_json`
- `armor_pierce_pct`
- `crit_policy_json`
- `telegraph_type` (`ring_close`, `cone`, `line`, `none`)
- `telegraph_radius`
- `telegraph_color_rgba`
- `action_windup`
- `action_impact`
- `action_recover`
- `allow_action_override` (bool)
- `allowed_action_tags_json`
- `vfx_id_windup`
- `vfx_id_impact`
- `sfx_id_windup`
- `sfx_id_impact`
- `enabled`

2. `npc_ability_loadouts`
- `id` (PK)
- `npc_spawn_id` (FK `npc_spawns.id`, opcional se usar por spawn)
- `actor_def_id` (FK opcional para default por archetype)
- `ability_id` (FK `ability_templates.id`)
- `priority`
- `weight`
- `min_distance`
- `max_distance`
- `min_target_hp_pct`
- `max_target_hp_pct`
- `phase_tag`
- `condition_lua` (string curta opcional)
- `enabled`

3. `npc_combat_profiles`
- `id` (PK)
- `name`
- `global_gcd_ms`
- `decision_tick_ms`
- `aggro_style`
- `allow_chain_cast`
- `max_consecutive_specials`

4. `npc_profile_bindings`
- vincula `npc_spawns` ou `actor_def_id` ao profile.

## 4.2 Tabelas existentes a aproveitar

- `media_actor_anims` e `media_anim_clips` continuam como fonte de clips.
- `npc_spawns` continua definindo posicionamento e ranges base.

---

## 5. Scripting de IA de Habilidade

## 5.1 Objetivo do script

Script decide:

- qual habilidade tentar;
- em qual alvo;
- em qual prioridade contextual;
- qual acao/animacao sugerir no cast (`action_override`).

Script nao decide:

- hit final;
- dano final aplicado;
- consumo final de recurso;
- estado autoritativo de parry/guard/dodge.

Script pode sugerir, core confirma.

## 5.2 API Lua proposta (fase 1)

Novo modulo `NPCCombat`:

1. `NPCCombat.try_cast(npc_id, ability_id, target_id, opts_table) -> bool`
- apenas solicita ao core;
- `opts_table` inicial:
  - `action_override = "CastHeavy"` (opcional)
  - `reason_tag = "npc_ai"` (opcional)
- retorna se a solicitacao entrou na fila.

2. `NPCCombat.can_cast(npc_id, ability_id, target_id) -> bool`
- consulta rapida de elegibilidade.

3. `NPCCombat.get_loadout(npc_id) -> table`
- leitura de habilidades configuradas.

4. `NPCCombat.get_context(npc_id, target_id) -> table`
- distancia, hp pct, estado de combate, fase.

5. `Event.on("npc_decide_ability", fn)`
- callback de decisao em tick de IA.

Exemplo de uso:

- script detecta telegraph de fase;
- escolhe `ability_id`;
- envia `try_cast(..., { action_override = "CastHeavy" })`;
- core valida e executa com fallback caso a acao nao exista.

## 5.3 API para player (mesmo contrato)

Novo modulo `PlayerCombat` (fase 2):

1. `PlayerCombat.try_cast(player_id, ability_id, target_id, opts_table) -> bool`
- usado pelo runtime de input/hotbar no servidor e por scripts de classe.

2. `Event.on("player_before_cast_intent", fn)`
- permite ajustar `action_override`/`reason_tag` sem bypass do core.

Garantia:

- player nunca aplica dano direto via script;
- script so emite intent.
- cliente nao envia `action_override` arbitrario como verdade.
- servidor valida/resolve override com regras de whitelist.

## 5.4 Modelo de execucao de script

Evolucao em duas etapas:

1. Curto prazo
- manter runtime atual, mas limitar chamadas por evento/tick controlado.

2. Medio prazo
- pool de VMs Lua por area/worker;
- evitar um unico lock global.

---

## 6. Cliente - Telegraph, Feedback e UX

## 6.1 Telegraph padronizado por ability

Cliente deixa de adivinhar regras e usa payload:

- tipo de telegraph;
- duracao;
- raio;
- cor;
- alvo do telegraph.

## 6.2 Feedback de counter

Estados visuais distintos:

- `Parry Perfect` (janela exata);
- `Parry Early/Late`;
- `Guarded`;
- `Interrupted`.

## 6.3 Feedback de animacao de habilidade

Cliente deve refletir a acao decidida pelo core:

- ao receber evento de stage, reproduz a acao resolvida;
- nao calcula acao localmente;
- em caso de perda de pacote, reconcilia com estado atual do ator.

## 6.4 Compatibilidade

Durante transicao:

- manter fallback para eventos antigos;
- ativar novo fluxo por feature flag.

---

## 7. GUE - Ferramentas de Conteudo

## 7.1 Nova aba: `Combat Abilities` [CONCLUIDO]

CRUD de `ability_templates`:

- timings;
- dano e escalas;
- telegraph;
- acoes de animacao;
- VFX/SFX;
- recurso e cooldown.

## 7.2 Profiles e Bindings no GUE [CONCLUIDO]

Editor de loadout/profile por NPC e archetype (na propria aba `Combat Abilities`):

- selecionar habilidades;
- prioridade/peso;
- condicoes simples;
- profile de combate.

## 7.3 Validacoes no editor

Regras minimas:

- `windup_ms >= parry_window_ms`;
- ranges consistentes;
- cooldown > 0 quando habilidade ofensiva;
- action names existentes no actor def.
- se `allow_action_override = true`, validar tags permitidas.

---

## 8. Observabilidade e Operacao

## 8.1 Telemetria por habilidade

Medir por `ability_id`:

- usos por minuto;
- taxa de acerto;
- taxa de parry;
- dano medio;
- latencia de resolucao;
- rejeicoes por invalidez.

## 8.2 Logs estruturados

Eventos criticos:

- start ability;
- stage transition;
- resolve outcome;
- cancel/interrupt reason.

## 8.3 Debug tools

Comando admin:

- listar abilities ativas por area;
- inspecionar cooldown e stage de um RID;
- forcar cast controlado para QA.

---

## 9. Testes e Qualidade

## 9.1 Unit tests

- resolucao de janela de parry;
- prioridade de stage;
- calculo de dano com armor pierce;
- consumo de recurso.

## 9.2 Integration tests

- fluxo completo `windup -> impact -> recover`;
- counter success/failure;
- sincronismo de eventos client/server;
- override de animacao com fallback quando action invalida.

## 9.3 Soak/perf tests

Meta inicial:

- 200+ NPCs por area com decision tick controlado;
- sem degradacao grave de tick de simulacao;
- p95 de loop dentro da meta operacional.

---

## 10. Plano de Migracao por Fases

## Fase A - Fundacao Runtime [CONCLUIDO]

Entrega:

- subsistema `Ability Runtime` no servidor;
- enums/eventos padronizados;
- feature flag `combat_ability_runtime_v1`.

Criterio de saida:

- especial atual rodando dentro do runtime novo sem perda funcional.

## Fase B - Data Layer + GUE [CONCLUIDO]

Entrega:

- schema novo;
- CRUD no GUE;
- migracao de 1-2 habilidades atuais para DB.

Criterio de saida:

- balanceamento basico sem recompilar servidor.

## Fase C - Scripted Decision [CONCLUIDO]

Entrega:

- API `NPCCombat` Lua;
- decisao de habilidade em script;
- `action_override` por script com fallback no core;
- core mantendo validacao final.

Criterio de saida:

- encounter simples de boss faseado controlado por script.

## Fase C2 - Player Intent Unificado [CONCLUIDO]

Entrega:

- API `PlayerCombat.try_cast`;
- input do player/hotbar migrado para `cast_intent`;
- telemetria por `reason_tag`.

Criterio de saida:

- player e NPC usando o mesmo pipeline de habilidade.

## Fase D - Escala e Hardening [PENDENTE]

Entrega:

- budget de script;
- melhorias de runtime Lua (pool por area/worker);
- telemetria operacional completa.

Criterio de saida:

- testes de carga aprovados e rollout seguro em producao.

---

## 11. Riscos e Mitigacoes

1. Risco: script influenciar estado autoritativo indevidamente.
- Mitigacao: API de script somente declarativa e validada.

2. Risco: gargalo por Lua global lock.
- Mitigacao: limitar frequencia de callbacks e migrar para pool por area.

3. Risco: regressao em combate existente.
- Mitigacao: feature flags + dual path + smoke tests por etapa.

4. Risco: explosao de combinacoes de dados mal configuradas.
- Mitigacao: validadores no GUE e checagens no boot do servidor.

5. Risco: override de animacao quebrar leitura de combate.
- Mitigacao: whitelist por tag, fallback obrigatorio no core, testes de regressao.

6. Risco: spoof de `action_override` por cliente.
- Mitigacao: cliente envia apenas intent basico; servidor resolve override final.

---

## 12. Definicao de Pronto (DoD)

Considerar completo quando:

1. Nenhuma habilidade de mob critica depende de constante hardcoded.
2. Conteudo consegue criar/ajustar habilidade e loadout pelo GUE.
3. Script decide uso de habilidade sem bypass do core autoritativo.
4. Script (NPC/player) pode sugerir acao de animacao sem bypass do core.
5. Cliente exibe telegraphs por dados de habilidade, nao por regra fixa.
6. Fallback de animacao funciona para action override invalida.
7. Testes unitarios + integracao + carga estao verdes.
8. Observabilidade permite debug rapido de outcome de combate.

---

## 13. Proximos Passos Imediatos (execucao)

1. [CONCLUIDO] Criar schema inicial das tabelas `ability_templates` e `npc_ability_loadouts`.
2. [CONCLUIDO] Implementar `Ability Runtime` minimo com stages e feature flag.
3. [CONCLUIDO] Migrar o especial atual para 1 `ability_template` real.
4. [CONCLUIDO] Expor primeira API Lua `NPCCombat.try_cast(..., opts)`.
5. [CONCLUIDO] Adicionar `action_override` no intent com fallback no core.
6. [CONCLUIDO] Adicionar aba GUE `Combat Abilities` com validacoes basicas.
7. [CONCLUIDO] Iniciar C2 com API Lua `PlayerCombat.try_cast(..., opts)` no mesmo contrato de intent.
8. [CONCLUIDO] Adicionar bridge gradual hotbar (`PCastSpell`) -> `cast_intent` via `runtime_ability_id`.
9. [CONCLUIDO] Adicionar CRUD no GUE para `npc_combat_profiles` e `npc_profile_bindings`.
10. [CONCLUIDO] Aplicar `decision_tick_ms` e `global_gcd_ms` dos profiles no runtime de decisao/cast de NPC.
11. [CONCLUIDO] Aplicar `allow_chain_cast` e `max_consecutive_specials` no runtime de especiais de NPC.
12. [CONCLUIDO] Aplicar filtros de `phase_tag` e `condition_lua` na selecao de `npc_ability_loadouts`.
13. [CONCLUIDO] Expor introspeccao Lua com `NPCCombat.get_loadout(npc_id)` e `NPCCombat.get_context(npc_id, target_id)`.
14. [CONCLUIDO] Adicionar hooks/eventos de decisao: `player_before_cast_intent` e alias `npc_decide_ability` (compat com `npc_ai_decide`).
15. [CONCLUIDO] Entregar encounter faseado real em script (`dist/server/scripts/events/forest_troll_boss.lua`) com loadouts/profiles seeded por migracao.
16. [CONCLUIDO] Adicionar telemetria agregada por `reason_tag` no runtime de `cast_intent`.

---

## 14. Revisao Final do Plano (Checklist)

Itens que este plano ja cobre:

1. Core autoritativo com timeline de habilidade.
2. Decisao de habilidade em script (NPC) e extensao para player.
3. Override de animacao por script com fallback seguro.
4. Data-driven completo (timing, dano, telegraph, animacao, loadout).
5. Ferramenta GUE para criacao/edicao de conteudo.
6. Escalabilidade operacional (telemetria, budget, hardening).

Pendencias de definicao antes da implementacao:

1. Definir formato final de `allowed_action_tags_json`.
2. Definir limite maximo de intents por segundo por ator.
3. Definir politica de cancelamento (`cast break`) por knockback/stun.
4. Definir estrategia de migracao do pacote de evento (versao/compat).

---

## 15. Pacote de Escala de Conteudo (Normal, Elite, Boss)

Objetivo: permitir criar dezenas/centenas de mobs e encounters sem explosao de complexidade.

## 15.1 Arquitetura de conteudo em camadas

Cada mob deve ser montado por composicao:

1. Base archetype
- define perfil geral de combate (`npc_combat_profile`);
- define conjunto inicial de habilidades.

2. Variantes
- ajustam loadout, pesos e condicoes;
- reaproveitam habilidades do catalogo.

3. Override por spawn (quando necessario)
- apenas para casos especiais de encounter.

Regra:

- preferir override por `actor_def_id` (archetype) em vez de por spawn.
- usar override por spawn somente para bosses/encounters pontuais.

## 15.2 Classes de mob padrao

Padrao recomendado para time de conteudo:

1. Mob normal
- 1-2 habilidades ofensivas;
- 1 habilidade utilitaria opcional;
- sem fases;
- script simples de prioridade por distancia.

2. Elite
- 2-4 habilidades;
- 1 habilidade de telegraph forte (parry/interrupt check);
- cooldown management mais agressivo;
- condicoes de uso por HP do alvo e contexto.

3. Boss
- 4-8 habilidades;
- fases (`phase_tag`);
- janelas claras de counter;
- scripts de transicao de fase e rotacao contextual.

## 15.3 Catalogo de habilidades e reutilizacao

Organizar `ability_templates` por familias:

- `melee_light`
- `melee_heavy`
- `gap_closer`
- `cone_cleave`
- `ground_aoe`
- `defensive_guard`
- `counter_parry_check`
- `summon_or_buff`

Boas praticas:

- evitar "skill unica por mob" quando o comportamento pode ser parametrico.
- manter "skin de conteudo" (animacao/vfx/sfx) separada da logica de resolucao.
- usar versao semantica de template (`name` + `revision`) para migracoes seguras.

## 15.4 Convencoes de naming e tags

Convencao recomendada:

- `ability_id` tecnico + `name` amigavel.
- `family`: agrupa logica de balance.
- `phase_tag`: `phase_1`, `phase_2`, `enrage`, `execute`.
- `reason_tag` em runtime: `npc_ai`, `boss_phase`, `player_input`, `script_combo`.

Convencoes para `action_override`:

- tags permitidas por habilidade (`allowed_action_tags_json`);
- exemplo de tags: `light`, `heavy`, `lunge`, `channel`, `finisher`.

## 15.5 Fluxo de producao em lote (pipeline)

1. Design cria/ajusta templates no GUE.
2. Combate valida ranges/timings e telegraph readability.
3. Script monta regras de decisao por classe de mob.
4. QA roda checklist de encounter.
5. Telemetria valida performance e efetividade.

Checklist minimo por novo mob:

1. Possui profile atribuido.
2. Possui loadout valido sem habilidade quebrada.
3. Possui pelo menos uma janela de leitura clara para o player.
4. Possui fallback de animacao funcional.
5. Passa smoke test de combate com 3+ players.

## 15.6 Escalabilidade operacional do time

Para escalar conteudo sem travar engenharia:

1. Catalogo fechado por sprint
- engenharia adiciona familias novas por janela;
- conteudo produz variantes dentro do catalogo.

2. Bibliotecas de script por padrao
- script base de mob normal;
- script base de elite;
- script base de boss faseado.

3. Budget de complexidade por encounter
- limite de habilidades ativas simultaneas;
- limite de transicoes de fase por minuto;
- limite de summons/eventos extras por area.

4. Review de conteudo orientado por dados
- aprovar encounter com base em metrica (TTK, taxa de parry, picos de dano),
- nao apenas por sensacao subjetiva.

## 15.7 Metas de escala recomendadas

Meta operacional inicial (apos fases C/D):

1. Criacao de 20-40 mobs novos por sprint sem mudanca no core.
2. Criacao de 2-4 encounters de boss por sprint com scripts reutilizaveis.
3. Ajustes de balance 100% via GUE/DB para 90% dos casos.
4. Sem regressao de tick de simulacao com 200+ NPCs por area.
