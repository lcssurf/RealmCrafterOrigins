# Lua Quest API - Referencia Rapida

Modulo global disponivel para scripts: `Quest`

## Funcoes

```lua
Quest.accept(player_id, quest_id) -> bool
Quest.abandon(player_id, quest_id) -> bool
Quest.turn_in(player_id, quest_id) -> bool
Quest.sync(player_id) -> bool

Quest.progress_kill(player_id, npc_name, delta_opt) -> bool
Quest.progress_collect(player_id, item_id, delta_opt) -> bool
Quest.progress_talk(player_id, npc_name, delta_opt) -> bool
Quest.progress_explore(player_id, area_name, delta_opt) -> bool
Quest.progress_interact(player_id, npc_name, delta_opt) -> bool

Quest.progress(player_id, objective_type, {
    npc_name = "",
    item_id = 0,
    area = "",
    delta = 1
}) -> bool
```

## Constantes

```lua
Quest.TYPE_KILL      -- 1
Quest.TYPE_COLLECT   -- 2
Quest.TYPE_TALK      -- 3
Quest.TYPE_EXPLORE   -- 4
Quest.TYPE_INTERACT  -- 5
```

## Semantica de retorno

- `true`: mudou estado/progresso.
- `false`: sem mudanca, erro de regra, player offline, ou dados invalidos.

## Observacoes importantes

- `player_id` e o runtime ID do player (ex.: recebido em callbacks `Event.on(...)`).
- `quest_id` e o ID numerico da tabela `quest_defs.id`.
- `delta_opt` <= 0 e tratado como 1 no backend.
- Em objetivos por nome/area, a comparacao e exata.

## Exemplo minimo

```lua
Event.on("npc_choice", function(player_id, npc_id, choice)
    if choice == 1 then
        Quest.accept(player_id, 1)
    elseif choice == 2 then
        Quest.turn_in(player_id, 1)
    end
end)
```
