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
        options = { "O que você vende?", "Talvez mais tarde." },
    },
    Innkeeper = {
        text    = "Entre, entre! Descanse os ossos. A lareira está acesa.",
        options = { "Me fale sobre este lugar.", "Até logo." },
    },
}

local NPC_CHOICES = {
    Guard = {
        [1] = "Avistamos Goblins a leste e Slimes a oeste. Fique alerta e não se aventure longe demais sozinho.",
    },
    Merchant = {
        [1] = "Poções, armas, armaduras — tenho de tudo. Minha loja abre em breve. Volte mais tarde!",
    },
    Innkeeper = {
        [1] = "Esta é a Zona Inicial. Aventureiros novatos começam aqui. Quando estiver pronto, use o portal para a Floresta — os desafios lá são bem maiores.",
    },
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
    local name    = Actor.get_name(npc_id)
    local replies = NPC_CHOICES[name]
    if replies and replies[choice] then
        Dialog.send(replies[choice], {})
    end
end)
