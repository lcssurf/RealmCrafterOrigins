# RCO Quests - Guia Completo (DB + Runtime + Scripts)

Este guia documenta como criar, publicar e testar quests no RealmCrafter: Origins.

## 1. Visao geral do sistema

O sistema de quests e server-authoritative:

- Definicao da quest: tabelas `quest_*` no banco.
- Estado por personagem: `character_quests` e `character_quest_progress`.
- Cliente recebe quest log via pacote `PQuestLog` (snapshot + delta).
- Aceite/abandono/turn-in podem vir do cliente (`PQuestAction`) ou de scripts Lua (`Quest.*`).
- Progresso de objetivos e aplicado no servidor por eventos de gameplay.

## 2. Estados da quest

Estados persistidos em `character_quests.state`:

- `active`
- `completed`
- `turned_in`
- `failed`
- `abandoned`

Fluxo normal:

1. `active`
2. `completed` (quando todos os objetivos batem target)
3. `turned_in` (apos entrega e aplicacao de recompensa)

## 3. Objetivos suportados

`quest_objective_defs.objective_type`:

- `1` = Kill (`target_npc_name`)
- `2` = Collect (`target_item_id`)
- `3` = Talk (`target_npc_name`)
- `4` = Explore (`target_area_name`)
- `5` = Interact (`target_npc_name`)

## 4. Eventos automaticos de progresso

Hoje o servidor ja marca progresso automatico para:

- Kill: morte de NPC por melee/spell.
- Collect: pickup de item no mundo.
- Talk: interacao com NPC (right-click).
- Interact: interacao com NPC (right-click).
- Explore: login/entrada na area e troca por portal.

Se o seu design usa apenas esses gatilhos, nao precisa chamar `Quest.progress_*` manualmente.

## 5. Como criar uma quest no banco

A forma mais segura e inserir em 3 tabelas:

- `quest_defs`
- `quest_objective_defs`
- `quest_reward_defs`

### 5.1 Exemplo SQL (SQLite/Postgres)

```sql
-- 1) Definicao principal
INSERT INTO quest_defs
(code, title, description, min_level, repeatable, auto_accept, prerequisite_quest_id, is_active)
VALUES
('wolves_at_the_gate', 'Wolves at the Gate', 'Defeat 6 Wolves near the forest road.', 3, 0, 0, 0, 1);

-- 2) Objetivo
INSERT INTO quest_objective_defs
(quest_id, objective_order, objective_type, description, target_npc_name, target_item_id, target_area_name, target_count)
SELECT id, 1, 1, 'Defeat Wolves (0/6)', 'Wolf', 0, '', 6
FROM quest_defs
WHERE code = 'wolves_at_the_gate';

-- 3) Recompensa
INSERT INTO quest_reward_defs
(quest_id, xp_reward, gold_reward, item_id, item_qty)
SELECT id, 320, 90, 0, 0
FROM quest_defs
WHERE code = 'wolves_at_the_gate';
```

## 6. Campos importantes e validacoes

- `quest_defs.code`: unico. Use slug estavel.
- `min_level`: bloqueia aceite abaixo do nivel.
- `repeatable`: se `0`, nao pode aceitar de novo apos `completed/turned_in`.
- `prerequisite_quest_id`: exige quest pre-requisito em `turned_in`.
- `target_count`: servidor forca minimo 1.
- Reward item:
  - `item_id` e `item_qty` devem estar coerentes (ambos zero ou ambos preenchidos).
  - `item_qty` > 255 invalida reward.
  - Inventario cheio pode impedir turn-in (quest fica `completed`, sem perder estado).

## 7. API Lua de Quests

Modulo global: `Quest`

### 7.1 Acoes de estado

- `Quest.accept(player_id, quest_id) -> bool`
- `Quest.abandon(player_id, quest_id) -> bool`
- `Quest.turn_in(player_id, quest_id) -> bool`
- `Quest.sync(player_id) -> bool`

`true` = houve mudanca efetiva; `false` = sem mudanca/erro/cliente offline.

### 7.2 Progresso manual

- `Quest.progress_kill(player_id, npc_name, delta_opt)`
- `Quest.progress_collect(player_id, item_id, delta_opt)`
- `Quest.progress_talk(player_id, npc_name, delta_opt)`
- `Quest.progress_explore(player_id, area_name, delta_opt)`
- `Quest.progress_interact(player_id, npc_name, delta_opt)`
- `Quest.progress(player_id, objective_type, { npc_name=..., item_id=..., area=..., delta=... })`

Constantes Lua:

- `Quest.TYPE_KILL`
- `Quest.TYPE_COLLECT`
- `Quest.TYPE_TALK`
- `Quest.TYPE_EXPLORE`
- `Quest.TYPE_INTERACT`

## 8. Exemplo de script de NPC (aceitar e entregar)

```lua
Event.on("npc_choice", function(player_id, npc_id, choice)
    local npc_name = Actor.get_name(npc_id)
    if npc_name ~= "Guard" then
        return
    end

    -- Botao 1: aceitar quest id 1
    if choice == 1 then
        local changed = Quest.accept(player_id, 1)
        if changed then
            Dialog.send("Quest accepted.", {})
        else
            Dialog.send("Could not accept quest.", {})
        end
        return
    end

    -- Botao 2: tentar entrega
    if choice == 2 then
        local changed = Quest.turn_in(player_id, 1)
        if changed then
            Dialog.send("Quest turned in. Well done.", {})
        else
            Dialog.send("Quest is not ready to turn in.", {})
        end
        return
    end
end)
```

## 9. Exemplo de progresso customizado

Use isso quando o objetivo nao vem de kill/pickup/talk automatico.

```lua
Event.on("player_action", function(player_id, action, state)
    if action == "UseAncientLever" and state == 0 then
        -- Conta como interacao com um alvo logico
        Quest.progress_interact(player_id, "Ancient Lever", 1)
    end
end)
```

## 10. Checklist de publicacao

1. Inserir quest/objectives/rewards no banco.
2. Garantir que nomes batem exatamente (`target_npc_name`, `target_area_name`).
3. Conferir item reward existe em `item_templates`.
4. Testar aceite, progresso, completed e turn-in com 1 personagem novo.
5. Validar no cliente que `PQuestLog` atualiza apos cada mudanca.
6. Revisar script NPC/dialog se houver fluxo de aceite/entrega via Lua.

## 11. Troubleshooting rapido

- Nao aparece para aceitar:
  - nivel abaixo de `min_level`
  - prerequisito nao entregue
  - `is_active = 0`
  - ja ativa/completa e `repeatable = 0`

- Nao progride:
  - tipo de objetivo incorreto
  - nome/area nao bate exatamente
  - script chama `Quest.progress_*` com alvo errado

- Nao entrega:
  - quest ainda nao `completed`
  - reward invalida (item_id/item_qty)
  - inventario sem espaco para item reward

## 12. Arquivos de referencia

- `server/internal/db/db.go` (Accept/Abandon/TurnIn/Progress)
- `server/internal/net/quest_runtime.go` (sync snapshot/delta)
- `server/internal/scripting/api.go` (modulo Lua `Quest`)
- `server/internal/scripting/registry.go` (bridge de quest)
