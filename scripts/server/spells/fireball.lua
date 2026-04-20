local spell = Spell.define({
    id          = 1,
    name        = "Fireball",
    spell_type  = "damage",
    ep_cost     = 20,
    cooldown_ms = 2000,
    range       = 25.0,
    icon        = 0,
})

spell.on_cast = function(caster_id, target_id)
    local dmg = math.random(20, 35)
    Log("Fireball: caster=" .. caster_id .. " target=" .. target_id .. " dmg=" .. dmg)
    Combat.deal_damage(caster_id, target_id, dmg, "magic")
end

Spell.register(spell)
