# Combat Ability Runtime API (Lua)

Este documento cobre as APIs novas de combate data-driven para scripts Lua no servidor.

## 1) Contrato de Cast Intent

Toda tentativa de skill usa o mesmo contrato:

- `caster_rid`
- `target_rid`
- `ability_id`
- `action_override` (opcional)
- `reason_tag` (opcional)
- `client_trace_id` (opcional)

O core valida range, cooldown, recurso, override permitido e executa timeline autoritativa.

## 2) API de NPC

### `NPCCombat.can_cast(npc_id, ability_id, target_id) -> bool`

Valida elegibilidade sem iniciar cast.

### `NPCCombat.try_cast(npc_id, ability_id, target_id [, opts]) -> bool`

Tenta iniciar o cast (windup autoritativo).

`opts`:

- `action_override = "AttackHeavy"` (opcional)
- `reason_tag = "npc_ai"` (opcional)
- `client_trace_id = "trace-123"` (opcional)

## 3) API de Player

### `PlayerCombat.can_cast(player_id, ability_id, target_id) -> bool`

Mesmo contrato de validação para player.

### `PlayerCombat.try_cast(player_id, ability_id, target_id [, opts]) -> bool`

Mesmo pipeline autoritativo do NPC, incluindo consumo de recurso e cooldown de ability.

## 4) Exemplo real (NPC) com `npc_ai_decide`

```lua
Event.on("npc_ai_decide", function(npc_id, target_id, now_ms)
    -- Exemplo: tenta usar a ability 1 quando possível.
    -- Troque o ID para uma ability real da sua tabela ability_templates.
    if NPCCombat.can_cast(npc_id, 1, target_id) then
        NPCCombat.try_cast(npc_id, 1, target_id, {
            action_override = "AttackHeavy",
            reason_tag = "boss_phase"
        })
    end
end)
```

## 5) Exemplo real (Player) via script

```lua
-- Exemplo de chamada direta para um player e alvo já conhecidos:
PlayerCombat.try_cast(player_id, 7301, target_id, {
    action_override = "AttackHeavy",
    reason_tag = "player_input"
})
```

## 6) Regras importantes

- Script decide intencao. Core decide verdade.
- `action_override` so vale se:
  - `allow_action_override = true` na ability;
  - o actor possui essa action no `actor_def`;
  - a action bate com `allowed_action_tags_json`.
- Se override for invalido, o core faz fallback para a action default da habilidade.

## 7) Testes de referencia no codigo

- `server/internal/world/cast_intent_test.go`
- `server/internal/scripting/npc_combat_api_test.go`
- `server/internal/scripting/player_combat_api_test.go`
- `server/internal/scripting/npc_ai_decision_test.go`

## 8) Bridge com hotbar (`PCastSpell`)

Para migracao gradual da hotbar para cast intent sem quebrar spells antigos:

- use a coluna `spell_templates.runtime_ability_id`.
- `runtime_ability_id = 0`: spell segue caminho legacy (Lua de spell atual).
- `runtime_ability_id > 0`: spell entra no pipeline de `PlayerCombat`/`cast_intent`.

Isso permite mover skill por skill para o modo action, sem desligar o sistema antigo de uma vez.

## 9) GUE: aba `Combat Abilities` (real, pronta para uso)

No GUE agora existe a aba **Combat Abilities** com 4 subtabs:

- `Ability Templates`: CRUD completo de `ability_templates` (timings, dano, telegraph, actions, VFX/SFX, override, enabled).
- `NPC Loadouts`: CRUD completo de `npc_ability_loadouts` (por `npc_spawn_id` e/ou `actor_def_id`, prioridade, peso, distancia, HP%, fase e condition lua).
- `Combat Profiles`: CRUD completo de `npc_combat_profiles` (global_gcd, decision_tick, chain cast, etc.).
- `Profile Bindings`: CRUD completo de `npc_profile_bindings` (bind por spawn e/ou actor_def).

Validacoes basicas aplicadas no editor:

- `windup_ms >= parry_window_ms`
- `range_max >= range_min`
- dano maximo >= dano minimo
- loadout precisa de `ability_id` valido
- loadout precisa de `npc_spawn_id` ou `actor_def_id`
- `min/max target hp` dentro de `0..100`
- profile binding precisa de `profile_id` valido

## 10) Runtime: profiles aplicados no servidor

Com `combat_ability_runtime_v1` ligado:

- `npc_combat_profiles.global_gcd_ms` controla o GCD entre especiais de NPC.
- `npc_combat_profiles.decision_tick_ms` controla a frequencia de avaliacao de script/special (`npc_ai_decide` + seletor de loadout).
- `npc_combat_profiles.allow_chain_cast` e `max_consecutive_specials` controlam quantos especiais podem sair em sequencia.
- quando `allow_chain_cast = false`, o NPC fica limitado a 1 especial por cadeia e precisa quebrar a cadeia com ataque basico ou timeout.
- `npc_profile_bindings` resolve profile por prioridade:
  - primeiro `npc_spawn_id`
  - depois `actor_def_id`
  - fallback para `default_profile`

## 11) Runtime: `phase_tag` e `condition_lua` no loadout

O seletor de `npc_ability_loadouts` agora aplica os filtros abaixo antes de tentar cast:

- `phase_tag`
- `condition_lua`
- cooldown/range/hp/weight (ja existentes)

### `phase_tag` suportado

- vazio, `any`, `*` -> sempre passa
- `phase_1`, `phase_2`, `phase_3` -> baseado no HP% atual do NPC
- `enrage` -> NPC com `hp <= 20%`
- `execute` -> alvo com `hp <= 30%`
- lista por virgula -> exemplo: `phase_2,enrage`

### `condition_lua` suportado (subset performatico)

Expressao booleana com `and` / `or` e comparadores:

- comparadores: `<`, `<=`, `>`, `>=`, `==`, `!=`
- variaveis numericas:
  - `distance`
  - `npc_hp_pct`, `target_hp_pct`
  - `npc_sp_pct`, `target_sp_pct`
  - `npc_mp_pct`, `target_mp_pct`
  - `phase` (1, 2, 3)
  - `rand_pct` (0..100)
- variavel textual:
  - `phase_tag` (somente `==` / `!=`)

Exemplos:

- `target_hp_pct <= 30 and distance <= 2.0`
- `phase >= 2 and npc_sp_pct >= 20`
- `phase_tag == "phase_3" or rand_pct < 15`

Obs: condicao invalida falha fechado (nao casta essa entry).

## 12) NPCCombat introspeccao (novo)

Para scripts de encounter, agora existe introspeccao do runtime:

- `NPCCombat.get_loadout(npc_id) -> table[]`
- `NPCCombat.get_context(npc_id, target_id) -> table`

`get_loadout` retorna o loadout efetivo ja resolvido por prioridade:
- primeiro por `npc_spawn_id`
- fallback por `actor_def_id`

Campos principais por entry:
- `ability_id`, `priority`, `weight`
- `min_distance`, `max_distance`
- `min_target_hp_pct`, `max_target_hp_pct`
- `phase_tag`, `condition_lua`, `enabled`

`get_context` retorna snapshot normalizado usado pelo seletor:
- `distance`
- `npc_hp_pct`, `target_hp_pct`
- `npc_sp_pct`, `target_sp_pct`
- `npc_mp_pct`, `target_mp_pct`
- `phase_tag`, `phase`

## 13) Eventos de decisao (novo)

### NPC: nome canonico + compatibilidade

O hook de decisao de NPC aceita os dois nomes:

- `Event.on("npc_decide_ability", fn)` (canonico)
- `Event.on("npc_ai_decide", fn)` (legacy, mantido por compatibilidade)

Assinatura:

```lua
function(npc_id, target_id, now_ms)
```

### Player: pre-processamento de cast intent

Novo evento:

- `Event.on("player_before_cast_intent", fn)`

Assinatura:

```lua
function(player_id, target_id, ability_id, reason_tag)
  return {
    action_override = "AttackHeavy", -- opcional
    reason_tag = "script_combo",     -- opcional
    client_trace_id = "trace-123",   -- opcional
    cancel = false                   -- opcional
  }
end
```

Regras:
- evento so ajusta metadados do intent (nao aplica dano direto);
- validacao final continua no core autoritativo;
- `cancel=true` impede o cast de entrar no runtime.

## 14) Encounter faseado real (ativo)

Foi adicionado um encounter faseado real para `Forest Troll` em:

- `dist/server/scripts/events/forest_troll_boss.lua`

O script:

- usa `Event.on("npc_decide_ability", ...)`;
- consulta `NPCCombat.get_loadout` + `NPCCombat.get_context`;
- escolhe cast por fase (`phase_1`, `phase_2`, `phase_3/enrage`);
- envia `reason_tag` por fase (`boss_phase_1`, `boss_phase_2`, `boss_phase_3`);
- usa `action_override` quando o actor possui a action.

Dados baseline do encounter sao semeados por migracao (`migrateV21`):

- abilities: `forest_troll_crushing_blow_v1`, `forest_troll_brutal_slam_v1`, `forest_troll_enrage_cleave_v1`;
- profile: `forest_troll_boss_profile_v1`;
- bindings e loadouts para spawns de `Forest Troll`.

## 15) Telemetria por `reason_tag`

O runtime de `cast_intent` agora agrega telemetria por `reason_tag`:

- `attempts`
- `started`
- `rejected`
- distribuicao de `reject_reason`

A cada intervalo (30s), o servidor publica snapshot consolidado no log:

- prefixo: `telemetry: cast_intent`
- chave principal: `reason_tag`

## 16) Telegraph visual por ability/fase (cliente)

No `CombatEventSpecialWindup`, o servidor agora envia metadados no campo `text`
com prefixo `meta:` (ao inves de texto de chat), por exemplo:

`meta:telegraph=parry;ability=123;reason=boss_phase_2;radius=3.20;color=1,0.45,0.15,0.75;style=ring_close`

Campos usados pelo cliente:

- `reason`: fase/contexto (`boss_phase_1`, `boss_phase_2`, `boss_phase_3`, etc.)
- `radius`: raio do circulo de telegraph
- `color`: cor RGBA (0..1) do circulo externo
- `style`: tipo de telegraph (extensivel)

Com isso, o cliente desenha telegraph com estilo data-driven por ability/fase
(cor/raio), incluindo countdown visual no timing de parry.
