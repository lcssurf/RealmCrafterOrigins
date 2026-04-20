-- RCO Server-side Lua scripts
-- Replaces RC 1.26 BriskVM scripting system
-- Functions are called by the Go server via sol2
--
-- In RC 1.26, server-side scripts lived in:
--   Data/Server Data/Scripts/<ActorName>.bb  (BriskBlitz source)
-- compiled by BriskVM and executed per-actor or per-event.
--
-- In RCO, all server-side game logic is written in Lua 5.4.
-- The Go server loads this file (and any require'd modules) at startup
-- via sol2 and calls the named functions in response to game events.
--
-- Naming convention:
--   Global functions  — top-level game events (OnPlayerEnter, etc.)
--   Actor scripts     — tables keyed by actor/NPC name, e.g. Scripts["Guard"]

-- ---------------------------------------------------------------------------
-- Player lifecycle
-- ---------------------------------------------------------------------------

-- Called when a player enters the world (after character select + area load).
-- Equivalent to RC's OnPlayerEnter server event.
-- @param player  table { name, level, area, x, y, z, gold, health }
function OnPlayerEnter(player)
    -- player.name, player.level, player.area available
    print("[Script] " .. player.name .. " entered " .. player.area)
    -- TODO: restore persistent buffs, trigger area entry events, etc.
end

-- Called when a player leaves or disconnects.
-- Equivalent to RC's OnPlayerLeave server event.
-- @param player  table { name, level, area }
function OnPlayerLeave(player)
    print("[Script] " .. player.name .. " left the world")
    -- TODO: persist any unsaved state, clean up party membership, etc.
end

-- ---------------------------------------------------------------------------
-- Progression
-- ---------------------------------------------------------------------------

-- Called immediately after a character's level increases.
-- Equivalent to RC's LevelUp script attached to ActorDef.
-- @param actor   table — the actor that leveled up
-- @param other   table — unused for level-up; reserved for future use
function LevelUp(actor, other)
    if actor then
        print("[Script] " .. actor.name .. " leveled up!")
        -- TODO: calculate new base stats (STR, DEX, INT, etc.)
        -- TODO: send PStatUpdate and PXPUpdate packets to client
        -- TODO: restore health/energy to new maximum values
    end
end

-- ---------------------------------------------------------------------------
-- Combat
-- ---------------------------------------------------------------------------

-- Called on every melee or spell attack before damage is applied.
-- Equivalent to RC's Attack script attached to ActorDef.
-- Returning a modified damage value from this function overrides the default.
-- @param attacker  table — actor performing the attack
-- @param target    table — actor receiving the attack
-- @return          number | nil  — override damage, or nil for default
function Attack(attacker, target)
    if attacker and target then
        -- TODO: custom damage calculation (critical hits, resistances, etc.)
        -- Example: return attacker.str * 2
    end
    -- Returning nil uses the server's built-in formula.
    return nil
end

-- Called when an actor's health reaches zero.
-- Equivalent to RC's Death script attached to ActorDef.
-- @param actor   table — the actor that died
-- @param killer  table | nil — actor that delivered the killing blow
function Death(actor, killer)
    if actor then
        print("[Script] " .. actor.name .. " has died")
        -- TODO: handle respawn timer and position
        -- TODO: apply XP loss rules if configured
        -- TODO: drop loot table items into the world
        if killer then
            -- TODO: grant XP and faction standing to killer
        end
    end
end

-- ---------------------------------------------------------------------------
-- Social
-- ---------------------------------------------------------------------------

-- Called when a party event occurs for an actor.
-- Equivalent to RC's Party script attached to ActorDef.
-- @param actor  table — the actor involved
-- @param func   string — "Join" | "Leave" | "Disband"
function Party(actor, func)
    if actor then
        if func == "Join" then
            print("[Script] " .. actor.name .. " joined a party")
            -- TODO: sync party HP bars to all members
        elseif func == "Leave" then
            print("[Script] " .. actor.name .. " left a party")
        elseif func == "Disband" then
            print("[Script] Party disbanded")
        end
    end
end

-- ---------------------------------------------------------------------------
-- NPC / Actor scripts
-- ---------------------------------------------------------------------------

-- Actor-specific scripts are stored in the Scripts table, keyed by NPC name.
-- The server looks up Scripts[actorDefName] before calling a global fallback.
-- Example NPC script:
Scripts = {}

Scripts["Town Guard"] = {
    -- Called when a player right-clicks this NPC.
    -- Equivalent to RC's RightClick script block.
    OnRightClick = function(npc, player)
        -- TODO: open dialog with player using PDialog packet
        print("[Script] " .. player.name .. " spoke to " .. npc.name)
    end,

    -- Called every server tick while the NPC is alive.
    -- Equivalent to RC's Think/Update loop for scripted actors.
    OnThink = function(npc, dt)
        -- dt = delta time in seconds since last tick
        -- TODO: patrol logic, idle animations, etc.
    end,
}
