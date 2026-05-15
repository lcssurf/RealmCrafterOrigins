# Fase 3 - Matriz de Paridade Feature -> GUE

Data de atualizacao: 2026-05-12

Objetivo: garantir que cada feature da Fase 3 feche com fluxo de operacao no GUE.

## Regras de status

- `planned`: feature mapeada, painel ainda nao implementado.
- `in_progress`: painel/validacoes em desenvolvimento.
- `ready`: criar/editar/duplicar/validar funcionando no GUE.

## Matriz

1. Quests
- Dominio: `quest_defs`, objetivos, recompensas, progresso de personagem.
- GUE: tab de quests + editor de objetivo/recompensa + validacao de referencias.
- Status: `ready`.

2. Party
- Dominio: convite, estados de membro, lideranca, permissao.
- GUE: configuracoes de regras de party (limites, permissao de kick/lead transfer).
- Status: `planned` (runtime iniciado; authoring GUE ainda nao implementado).

3. Projectile
- Dominio: templates de projetil, travel/impact/AoE.
- GUE: editor de template (speed, radius, lifetime, collision, VFX/SFX binding).
- Status: `planned`.

4. Weather
- Dominio: perfil de clima por area.
- GUE: edicao em zonas/areas (probabilidades + transicoes) com validacao de faixa.
- Status: `in_progress` (probabilidades base ja existem).

5. Appearance Update
- Dominio: bindings de mesh/material/animacao por actor.
- GUE: editor de overrides com preview e validacao de asset path.
- Status: `planned`.

6. Combat Session
- Dominio: dodge/guard/parry/interrupt, DR, telegraph.
- GUE: perfis de combate e janelas (timing, custo, lockout, DR category).
- Status: `planned`.

7. Skills + HUD + Weapon Mastery
- Dominio: skill defs, loadouts, especializacoes, progressao por arma.
- GUE: fluxo unico de skill (base, arma, combate, projetil, animacao, publish).
- Status: `planned`.

8. Status Effects
- Dominio: buff/debuff/DoT/HoT/CC com stacking e expiracao.
- GUE: editor de efeitos + regras de stack/refresh/priority.
- Status: `planned`.

## Gate de fechamento por feature

Uma feature da Fase 3 so fecha com:

1. Fluxo `criar/editar/duplicar/validar` ativo no GUE.
2. Persistencia de dados versionada e sem hardcode de tuning.
3. Smoke basico de authoring (criar no GUE -> consumir em runtime).
