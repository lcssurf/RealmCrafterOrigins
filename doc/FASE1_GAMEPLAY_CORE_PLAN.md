# Fase 1 - Gameplay Core 2026 (Plano Detalhado)

Objetivo da fase: elevar a qualidade de movimento e combate-base para um padrao MMO moderno, mantendo estabilidade do que ja funciona.

Escopo principal desta fase:
- Character Controller (cliente) com comportamento robusto e previsivel.
- Validacao server-side de movimentacao (height/slope sanity checks).
- Melhor leitura e "feel" de gameplay sem quebrar loops atuais.
- Aplicar regra global de qualidade: codigo correto, centralizado, modular e data-driven.

## Status da Fase 1

- [x] Etapa A - Baseline e instrumentacao minima
- [x] Etapa B - Consolidacao do Character Controller (cliente)
- [x] Etapa C - Validacao server-side de movimento
- [x] Etapa D - Integracao com gameplay atual
- [x] Etapa E - Fechamento da fase

Entregas ja concluidas:
- Telemetria de movimento cliente/servidor com controle por configuracao.
- Tunables do `PlayerController` centralizados em `PlayerController::Config`.
- Validacao server-side em `handleStandardUpdate` (deslocamento horizontal + coerencia vertical por heightmap).
- Correcao automatica via `PRepositionActor` em updates invalidos.
- Thresholds de validacao externalizados para `dist/server/config.toml` (`[movement]`).
- Logging de rejeicao com motivo (`horizontal`, `vertical`, `horizontal+vertical`) e amostragem configuravel.

Estado da fase:
- Fase 1 concluida.

## 1) O que queremos ao final da fase

1. Movimento de player com:
- slope handling consistente (nao escalar parede),
- gravidade confiavel,
- salto e queda previsiveis,
- step/snap suave no terreno.

2. Servidor validando movimento reportado:
- rejeitar Y invalido e deslocamento suspeito,
- corrigir com reposicionamento suave quando necessario.

3. Sem regressao nos sistemas existentes:
- combate, chat, inventario, spells, portals, AI de NPC e tools continuam estaveis.

## 2) Estado atual (diagnostico real do codigo)

Ja existe base de Character Controller no cliente:
- `client/src/core/player_controller.h/.cpp`
- slope, slide, gravidade, jump, auto-run, click-to-move ja implementados.

Ja existe base de heightmap no servidor:
- loader/sampling pronto em `server/internal/world/heightmap.go`
- world carrega heightmaps em `server/internal/world/world.go` (`LoadHeightmaps`)
- NPC ja corrige Y por heightmap em `server/internal/world/area.go` (moveNPCToward).

Lacuna critica atual:
- `server/internal/net/client.go` (`handleStandardUpdate`) aceita `x/y/z/yaw` do cliente sem validacao de movimento.

Conclusao:
- Fase 1 nao e "do zero". E uma fase de consolidacao, hardening e tuning.

## 3) Arquivos relevantes (impacto direto)

Cliente:
- `client/src/core/player_controller.h`
- `client/src/core/player_controller.cpp`
- `client/src/core/main.cpp`
- `client/src/renderer/terrain/terrain.h`
- `client/src/renderer/terrain/terrain.cpp`
- `client/src/ui/controls_ui.h/.cpp` (debug/tuning in-game opcional)

Servidor:
- `server/internal/net/client.go`
- `server/internal/world/heightmap.go`
- `server/internal/world/world.go`
- `server/internal/world/area.go`
- `server/internal/world/actor.go`

Protocolo:
- `client/src/net/protocol.h`
- `server/internal/protocol/packets.go`

## 4) Plano de execucao (incremental e seguro)

### Etapa A - Baseline e instrumentacao minima

Objetivo:
- medir comportamento atual antes de alterar logica.

Acoes:
- adicionar logs debug opcionais (cliente/servidor) para:
  - transicoes `on_ground <-> in_air`,
  - eventos de bloqueio por slope,
  - correcao de posicao pelo servidor.
- criar checklist de cenarios de teste manual (subida/queda/salto/slope extremo/portal/combate).

Impacto:
- baixo; sem mudanca funcional obrigatoria.

### Etapa B - Consolidacao do Character Controller (cliente)

Objetivo:
- fechar divergencias entre implementacao atual e especificacao do `SYSTEMS.md`.

Acoes:
- revisar e padronizar parametros:
  - `max_slope_deg`, `gravity`, `jump_vel`, `snap_down`, limites de queda.
- centralizar parametros de movimento em um ponto unico de configuracao (evitar tunables espalhados em arquivos diferentes).
- preparar caminho para externalizacao gradual desses tunables para fonte data-driven (GUE/DB/config), mantendo fallback seguro no codigo.
- reforcar transicoes de estado (ground/air) para evitar jitter.
- validar interacao com:
  - camera (RMB/action mode),
  - colisao (`ColData::Resolve`),
  - click-to-move e auto-interact.
- opcional: expor knobs em painel debug para tuning rapido.

Impacto:
- medio (core de movimento do player).

Risco:
- sensação de controle piorar se tuning ficar agressivo.

Mitigacao:
- ajuste por pequenos passos com cenarios de teste fixos.

### Etapa C - Validacao server-side de movimento (anti-abuso + consistencia)

Objetivo:
- nao confiar cegamente no Y e deslocamento enviados pelo cliente.

Acoes em `handleStandardUpdate`:
- calcular `expectedY` via `area.Heightmap.SampleWorld(x,z)` quando existir.
- aplicar tolerancia vertical configuravel.
- limitar deslocamento por tick (speed sanity check, com margem para latencia).
- evitar thresholds hardcoded "soltos": usar constantes centralizadas/configuraveis com defaults seguros.
- em violacao:
  - corrigir posicao no server,
  - enviar `PRepositionActor` (ou equivalente ja existente) ao cliente.

Impacto:
- alto valor; reduz exploits e desync.

Risco:
- rubber-banding se thresholds forem muito apertados.

Mitigacao:
- iniciar com thresholds conservadores e telemetria de correcoes.

### Etapa D - Integracao com gameplay atual

Objetivo:
- garantir que loops atuais continuam bons.

Acoes:
- validar com combate melee/spell em movimento.
- validar portal traverse durante corrida/salto.
- validar update remoto dos outros atores (PStandardUpdate) sem regressao visual.

Impacto:
- medio; foco em regressao cross-system.

### Etapa E - Fechamento da fase

Objetivo:
- congelar tuning e documentar defaults.

Acoes:
- registrar valores finais dos parametros de movimento.
- registrar tolerancias server-side.
- atualizar docs de sistema/status.
- registrar backlog de externalizacao de parametros ainda hardcoded, com prioridade e dono.

Resultado:
- concluido.

## 5) Necessidade e justificativa (por que essa fase e critica)

- Sem movement core robusto, qualquer avanco de gameplay 2026 fica fraco.
- Sem validacao server-side, o MMO fica vulneravel a desync/abuso.
- Navmesh/pathfinding e sistemas avancados dependem de base de locomocao confiavel.

## 6) Impacto arquitetural esperado

Positivo:
- fronteira cliente/servidor de movimento mais clara.
- menor acoplamento do `main.cpp` com regras de locomocao (controlador como modulo real).
- mais debuggabilidade via logs/counters de movimento.
- maior centralizacao de regras de gameplay e tunables, reduzindo risco de divergencia.

Cuidado:
- evitar espalhar regras de movimento em varios arquivos sem dono.
- manter parametros centralizados no controller/config.

## 7) Riscos principais

1. Regressao de feeling (movimento pesado ou escorregadio demais).
2. Rubber-banding por validacao excessiva no servidor.
3. Efeitos colaterais em combate/portal/camera.

## 8) Criterios de aceite

1. Player nao sobe slopes acima do limite configurado.
2. Salto e queda funcionam sem jitter e sem "snap" incoerente.
3. Servidor corrige posicoes invalidas com baixa incidencia de falso positivo.
4. Fluxos existentes (combate, portal, click-to-move, spells) permanecem estaveis.
5. Build de client e server passando apos as mudancas.
6. Parametros e thresholds da fase estao centralizados (sem espalhamento acidental).
7. Sem hardcode novo evitavel para gameplay; quando houver excecao, justificativa e plano de externalizacao documentados.

## 10) Gate de qualidade por PR (obrigatorio nesta fase)

Checklist de validacao para cada entrega da Fase 1:
- Corretude funcional: comportamento esperado + sem regressao visivel.
- Organizacao: codigo legivel, responsabilidades claras, baixo acoplamento local.
- Centralizacao: regras/tunables em ponto unico (sem duplicacao de constantes).
- Data-driven: preferir GUE/DB/Lua/config; codigo apenas como fallback.
- Debugabilidade: logs/counters suficientes para diagnosticar falhas em runtime.

## 11) Checklist de teste manual (Fase 1)

1. Subir slope suave: player deve subir sem jitter.
2. Tentar subir slope extremo: player nao deve escalar como parede.
3. Em slope extremo: deve haver slide/escorregamento coerente.
4. Correr + pular em terreno irregular: sem "teleporte" de Y.
5. Cair de borda/declive: transicao para queda sem travar animacao/movimento.
6. Aterrissar apos queda: sem quicar ou entrar no terreno.
7. Click-to-move em morro: caminho local deve respeitar limites de slope.
8. Modo RMB/action + movimento: yaw/camera continuam consistentes.
9. Movimento perto de colisores (col_data): sem atravessar volume e sem jitter severo.
10. Entrar em portal correndo/pulando: troca de area continua estavel.
11. Combate em movimento (melee): range e hit continuam corretos.
12. Cast de spell em movimento: cooldown, target e efeitos sem regressao.
13. Multiplayer local: outro cliente recebe `PStandardUpdate` suave (sem stutter novo).
14. Area sem heightmap no servidor: nao quebrar loop de update.
15. Com telemetria ativada: logs legiveis para diagnosticar step distance e erro de Y.

## 12) Playbook de tuning (movement validation)

1. Em `dist/server/config.toml`, configurar temporariamente:
- `enable_telemetry = true`
- `log_rejections = true`
- `telemetry_sample_ms = 500`

2. Rodar 10-15 minutos com cenarios do checklist:
- corrida, salto, slopes extremos, portal, combate em movimento.

3. Ler logs:
- `move-telemetry`: baseline de `step` e `y_err`.
- `move-reject` + `move-reject-detail`: identifica se o problema e horizontal, vertical, ou ambos.

4. Ajustes recomendados:
- falso positivo horizontal: subir `base_step_allowance` ou `speed_slack_mult`.
- falso positivo vertical: ajustar `max_above_ground`/`max_below_ground`.
- aceitar "teleporte" indevido: reduzir `max_move_speed` e/ou `speed_slack_mult`.

5. Fechamento:
- quando os rejects legitimamente anormais prevalecerem e os falsos positivos ficarem baixos, voltar:
  - `enable_telemetry = false`
  - `log_rejections = true` (ou false em producao, conforme necessidade).

## 13) Baseline final adotado (Fase 1)

PlayerController (cliente) - centralizado em `PlayerController::Config`:
- `speed = 8.0`
- `back_mult = 0.65`
- `sprint_mult = 1.65`
- `turn_rate = 150.0`
- `max_slope_deg = 45.0`
- `jump_vel = 9.0`
- `gravity = 20.0`
- `snap_down = 0.8`
- `click_stop_radius = 0.08`
- `min_move_len_sq = 1e-8`
- `min_proj_len = 0.001`
- `min_dir_len_sq = 0.001`

Validacao de movimento (servidor) - `dist/server/config.toml` `[movement]`:
- `min_delta_sec = 0.016`
- `max_delta_sec = 1.0`
- `base_step_allowance = 0.75`
- `max_move_speed = 18.0`
- `speed_slack_mult = 1.25`
- `max_below_ground = 1.0`
- `max_above_ground = 12.0`
- `enable_telemetry = false` (runtime normal)
- `log_rejections = true`
- `telemetry_sample_ms = 500`

## 14) Backlog de externalizacao (pos-fase)

1. Expor `PlayerController::Config` em fonte data-driven (config/DB/GUE), mantendo fallback em codigo.
2. Definir perfil de movement por modo de jogo (dev/staging/prod) via config versionada.
3. Opcional: painel debug no cliente para ajuste temporario dos tunables sem rebuild (apenas ambiente dev).

## 9) Ordem recomendada de implementacao

1. Etapa A (baseline/logs).
2. Etapa B (controller).
3. Etapa C (validacao server-side).
4. Etapa D (regressao cross-systems).
5. Etapa E (fechamento/documentacao).
