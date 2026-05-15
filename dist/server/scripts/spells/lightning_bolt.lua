local spell = Spell.define({
    id          = 3,
    name        = "Lightning Bolt",
    spell_type  = "damage",
    ep_cost     = 30,
    cooldown_ms = 3000,
    range       = 30.0,
    icon        = 2,
})

spell.on_cast = function(caster_id, target_id)
    local dmg = math.random(30, 50)

    -- Extra damage against targets below 30% HP.
    local target_hp     = Actor.get_hp(target_id)
    local target_max_hp = Actor.get_max_hp(target_id)
    if target_max_hp > 0 and (target_hp / target_max_hp) < 0.3 then
        dmg = math.floor(dmg * 1.5)
    end

    Combat.deal_damage(caster_id, target_id, dmg, "magic")
    if Combat.interrupt then
        Combat.interrupt(caster_id, target_id)
    end
end

Spell.register(spell)
