local spell = Spell.define({
    id          = 2,
    name        = "Heal",
    spell_type  = "heal",
    ep_cost     = 15,
    cooldown_ms = 3000,
    range       = 0.0,
    icon        = 1,
})

spell.on_cast = function(caster_id, target_id)
    local amount = math.random(30, 50)
    Combat.heal(caster_id, amount)
end

Spell.register(spell)
