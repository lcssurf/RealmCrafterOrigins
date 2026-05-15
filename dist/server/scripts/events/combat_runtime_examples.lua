-- combat_runtime_examples.lua
-- Exemplos reais para o runtime de combat abilities.
-- Por seguranca, fica desligado por padrao.

local ENABLE_RUNTIME_EXAMPLES = false

if not ENABLE_RUNTIME_EXAMPLES then
    return
end

-- Ajuste para uma ability existente em ability_templates.
local BOSS_SPECIAL_ABILITY_ID = 1

-- Exemplo: no tick de decisao da IA do NPC, tenta castar uma ability data-driven.
Event.on("npc_ai_decide", function(npc_id, target_id, now_ms)
    if NPCCombat.can_cast(npc_id, BOSS_SPECIAL_ABILITY_ID, target_id) then
        NPCCombat.try_cast(npc_id, BOSS_SPECIAL_ABILITY_ID, target_id, {
            action_override = "AttackHeavy",
            reason_tag = "npc_ai_example"
        })
    end
end)
