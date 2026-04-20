-- player_events.lua — example event handlers.
-- Remove or expand as the game grows.

Event.on("player_join", function(player_id)
    -- Log("Player joined: " .. player_id)
end)

Event.on("npc_death", function(npc_id, killer_id)
    -- Log("NPC " .. npc_id .. " killed by " .. killer_id)
end)
