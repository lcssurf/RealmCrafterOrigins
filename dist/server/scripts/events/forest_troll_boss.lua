-- forest_troll_boss.lua
-- Real phased encounter script for Forest Troll bosses.
-- Uses NPCCombat loadout/context APIs and the unified cast-intent runtime.

local BOSS_BY_NAME = {
    ["Forest Troll"] = true,
    ["Hunger"] = true
}

local function split_csv(raw)
    local out = {}
    local s = tostring(raw or "")
    for part in string.gmatch(s, "([^,]+)") do
        local token = string.lower((part:gsub("^%s+", ""):gsub("%s+$", "")))
        if token ~= "" then
            table.insert(out, token)
        end
    end
    return out
end

local function row_matches_phase(row, ctx)
    local phase_tag = string.lower(tostring(row.phase_tag or ""))
    if phase_tag == "" or phase_tag == "any" or phase_tag == "*" then
        return true
    end

    local current_phase_tag = string.lower(tostring(ctx.phase_tag or "phase_1"))
    local npc_hp_pct = tonumber(ctx.npc_hp_pct or 0) or 0
    local target_hp_pct = tonumber(ctx.target_hp_pct or 0) or 0

    for _, token in ipairs(split_csv(phase_tag)) do
        if token == current_phase_tag then
            return true
        end
        if token == "enrage" and npc_hp_pct <= 20 then
            return true
        end
        if token == "execute" and target_hp_pct <= 30 then
            return true
        end
    end
    return false
end

local function resolve_action_override(npc_id, phase)
    if phase >= 2 and Actor.has_action(npc_id, "AttackHeavy") then
        return "AttackHeavy"
    end
    if Actor.has_action(npc_id, "Attack") then
        return "Attack"
    end
    return ""
end

Event.on("npc_decide_ability", function(npc_id, target_id, now_ms)
    local npc_name = Actor.get_name(npc_id)
    if not BOSS_BY_NAME[npc_name] then
        return
    end

    local rows = NPCCombat.get_loadout(npc_id)
    if rows == nil or #rows == 0 then
        return
    end

    local ctx = NPCCombat.get_context(npc_id, target_id)
    local phase = tonumber(ctx.phase or 1) or 1
    local reason_tag = "boss_phase_" .. tostring(phase)

    for _, row in ipairs(rows) do
        local ability_id = tonumber(row.ability_id or 0) or 0
        local enabled = row.enabled ~= false
        if enabled and ability_id > 0 and row_matches_phase(row, ctx) then
            if NPCCombat.can_cast(npc_id, ability_id, target_id) then
                local opts = { reason_tag = reason_tag }
                local action_override = resolve_action_override(npc_id, phase)
                if action_override ~= "" then
                    opts.action_override = action_override
                end
                if NPCCombat.try_cast(npc_id, ability_id, target_id, opts) then
                    return
                end
            end
        end
    end
end)
