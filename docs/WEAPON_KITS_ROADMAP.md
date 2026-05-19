# Weapon Kits + Skill Loadout — Roadmap de Execução

Última atualização: 2026-05-17

## Visão

Sistema de skills baseado em equipamento (não em classe), inspirado em 
Albion Online (simplicidade) + Throne and Liberty (profundidade de loadout).

Player equipa peças → ganha pool de skills daquela peça → escolhe quais usar 
no hotbar → cada skill ganha XP por uso → sobe nível com bonus.

Sem classes. Sem trees. Sem talents. Skill progression é por uso.

## Configuração default da engine

| Slot | Dá kit? | Slots no hotbar |
|---|---|---|
| weapon (0) | sim | 4 |
| chest (3) | sim | 1 |
| feet (7) | sim | 1 |

Total hotbar: 6 slots. Outros slots não contribuem por default.
Game designer pode mudar via GUE (tabela `equipment_slot_config`).

## Fases

### Fase 1 — Fundação de schema (Commits 1-9) ✅ CONCLUÍDA

- weapon_kits + weapon_kit_abilities (pool por kit)
- item_templates.weapon_kit
- GUE: aba Weapon Kits + dropdown no item editor
- Seeds: sword (slash, cleave) + bow (quickshot, aimedshot)
- Server: ResolveActivePlayerKit (read-only do slot 0)

### Fase 2 — Protocolo + cliente parser (Commits 10-13) ✅ CONCLUÍDA

- PSkillState codec (server + client)
- Server emite no world-enter + ao equipar arma
- Cliente parsa e armazena em SkillState global

### Fase 3 — Infraestrutura de configuração (Commits 14-16) ⏳ ATUAL

- 14: Tabela equipment_slot_config + struct + CRUD Go
- 15: Aba GUE "Equipment Slots"
- 16: Seed default (weapon=4, chest=1, feet=1)

### Fase 4 — Loadout pessoal (Commits 17-20)

- 17: Tabela character_skill_loadouts + CRUD
- 18: Server agrega skills de TODOS os slots equipados com kit
- 19: PSkillLoadoutAction handler real (validação + persistência)
- 20: ResolveActivePlayerKit lê loadout do player (não pool fixo)

### Fase 5 — UI cliente (Commits 21-23)

- 21: Hotbar dinâmica (placeholders + skills do loadout)
- 22: Tela "Skill Loadout" (pool à esquerda, slots à direita)
- 23: Hotkey dispara cast via runtime moderno

### Fase 6 — Anti-cheat + lock (Commits 24-25)

- 24: Server valida que cast usa ability no loadout ativo
- 25: Lock de troca de arma em combate

### Fase 7 — Mastery (Commits 26-30)

- 26: Tabela character_skill_progress + skill_progression_config
- 27: Server: XP por uso de skill
- 28: Server: aplicar bonus de nível em dano/cooldown no cast
- 29: Cliente: mostrar nível e XP no tooltip do hotbar
- 30: Cliente: tela de progressão por skill

## Backlog pós-Commit 30 (adiado)

Polimento visual do SkillLoadoutScreen e SkillHotbar foi adiado.

- Background com tema da engine (não cinza ImGui padrão)
- Botões com hover state visualmente claro
- Indicação visual de slot vazio (não apenas texto)
- Ícones por skill (depende de feature de assets)
- Toast para mensagens de erro (em vez de status no header)
- Espaçamento e padding consistente

Quando fazer: após Commit 30 (mastery completo) ou quando incomodar.

## Total estimado

~30 commits. Atualmente em Commit 14 (Fase 3 começando).

## Não-objetivos (fora de escopo desta feature)

- Trait stones / runes modificando skills
- Multi-arma com weapon swap
- Skills concedidas por quest/trainer (todas vêm via equipamento)
- Skill respec/reset complexo
- Visual effects de cada skill (vem com outra feature)

## Débito técnico desta feature

Ver docs/TECH_DEBT.md (itens 1, 2, 3, 4, 5 já registrados durante desenvolvimento).

Após Commit 30: migrar spell_templates → ability_templates e aposentar SpellBar 
legado (TECH_DEBT item 2).

---
