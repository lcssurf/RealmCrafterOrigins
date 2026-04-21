-- dialog.lua — per-NPC dialog trees.
-- Second-level responses use empty options {} so [Close] is the only button,
-- preventing the choice handler from looping back.

local NPC_DIALOGS = {
    Guard = {
        text    = "Alto lá. Esta área está sob minha vigilância. O que você precisa?",
        options = { "Há algum problema por aqui?", "Até logo." },
    },
    Merchant = {
        text    = "Bem-vindo! Tenho mercadorias para o aventureiro exigente. O que posso fazer por você?",
        options = { "Ver loja.", "Talvez mais tarde." },
    },
    Innkeeper = {
        text    = "Entre, entre! Descanse os ossos. A lareira está acesa.",
        options = { "Ver o que você vende.", "Me fale sobre este lugar.", "Até logo." },
    },
}

-- Text replies per NPC and choice index (nil = no text reply).
local NPC_CHOICES = {
    Guard = {
        [1] = "Avistamos Goblins a leste e Slimes a oeste. Fique alerta e não se aventure longe demais sozinho.",
    },
    Innkeeper = {
        [2] = "Esta é a Zona Inicial. Aventureiros novatos começam aqui. Quando estiver pronto, use o portal para a Floresta — os desafios lá são bem maiores.",
    },
}

-- Choices that open the NPC shop instead of a text reply.
local SHOP_CHOICES = {
    Merchant  = { [1] = true },
    Innkeeper = { [1] = true },
}

Event.on("npc_interact", function(player_id, npc_id)
    local name = Actor.get_name(npc_id)
    local dlg  = NPC_DIALOGS[name]
    if dlg then
        Dialog.send(dlg.text, dlg.options)
    else
        Dialog.send("Olá, viajante. Boas aventuras.", {})
    end
end)

Event.on("npc_choice", function(player_id, npc_id, choice)
    local name = Actor.get_name(npc_id)

    -- Check if this choice should open the NPC's shop.
    if SHOP_CHOICES[name] and SHOP_CHOICES[name][choice] then
        Dialog.open_shop()
        return
    end

    -- Otherwise send the text reply for this choice.
    local replies = NPC_CHOICES[name]
    if replies and replies[choice] then
        Dialog.send(replies[choice], {})
    end
end)
