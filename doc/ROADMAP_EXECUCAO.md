# Roadmap de Execucao - RCO MMO Engine

Objetivo: manter a facilidade de criacao do RealmCrafter, com qualidade tecnica e visual no padrao Unreal, com foco principal em graficos e gameplay moderno (2026).

## Diagnostico Atual (baseado em Graphify + docs)

- Renderer migrado para `shared/renderer` e em producao no client e tools.
- Base MMO funcional: login, personagens, combate basico, inventario, drops, shop, spells, audio/particles.
- GUE centralizado com edicao de DB e terrain/zones.
- Pendencias funcionais declaradas: quests, weather, projectile, party, appearance update.
- Hotspots de acoplamento:
  - `server/internal/db/db.go`
  - `server/internal/net/client.go`
  - `server/internal/world/area.go`
  - `tools/gue/src/tabs/zones.cpp`
  - `tools/gue/src/tabs/media.cpp`
  - `shared/renderer/src/pipeline.cpp`
  - `shared/renderer/src/model.cpp`

## Principios de Execucao

- Toda feature relevante entra com refactor incremental para modularidade.
- Graphify-first para descoberta e analise de impacto.
- Sincronia estrita de protocolo entre server/client.
- Build e smoke test por entrega (client, server e GUE quando afetado).
- Primeiro consolidar foundations, depois escalar qualidade visual/gameplay.

## Regra Global - Refatoracao Continua e Segura (transversal)

Meta: evoluir arquitetura sem "big-bang", preservando o que ja funciona.

Esta regra vale para todas as fases e todos os subsistemas (client, server, renderer, tools, DB, rede, gameplay).

1. Em toda entrega relevante
- Implementar a feature/correcao.
- Refatorar incrementalmente o trecho tocado para melhorar modularidade e clareza.
- Validar build/test/smoke antes de fechar.

2. Escopo de refatoracao por mudanca
- Apenas no raio de impacto da entrega atual (e dependencias imediatas).
- Sem congelar roadmap para uma fase isolada de "so refatoracao".
- Sem concentrar apenas em `db.go`/`client.go`: aplicar em qualquer area alterada.

3. Guardrails obrigatorios
- Sem regressao comportamental.
- Sem reescrita ampla desnecessaria.
- Melhorar observabilidade local quando tocar areas criticas (logs/counters/tracing basico).

4. Prioridade de hotspots (quando naturalmente tocados)
- `server/internal/db/db.go`
- `server/internal/net/client.go`
- `server/internal/world/area.go`
- `tools/gue/src/tabs/zones.cpp`
- `tools/gue/src/tabs/media.cpp`
- `shared/renderer/src/pipeline.cpp`
- `shared/renderer/src/model.cpp`

## Fase 1 - Gameplay Core 2026 (4-8 semanas)

Meta: elevar "feel" e base de combate/movimento.

1. Character Controller moderno (prioridade alta)
- Implementar pipeline do `SYSTEMS.md`: slope detection, sliding, gravity, jump, step absorption.
- Extrair de `client/src/core/main.cpp` para `player_controller`.

2. Server-side validation
- Height sampling no server para validar Y e reduzir exploits de movimento.
- Ajustes de reconciliacao suave (sem rubber band agressivo).

3. Combate e leitura
- Melhorar telegraph/feedback (hit confirmation visual/audio).
- Cooldowns, range readability e resposta de input.

Criterio de saida:
- Movimento com qualidade "action MMO", sem subida irreal de slopes.

## Fase 2 - Vertical Slice Grafico (6-10 semanas)

Meta: entregar um "slice" visual de alto impacto inspirado em Throne and Liberty.

1. Terrain/Landscape quality pass
- Aplicar backlog de `LANDSCAPE_ARCHITECTURE.md` e `UE_LANDSCAPE_ANALYSIS.md`:
  - tiling por layer
  - normal detail/consistencia
  - melhorias de blend e leitura de materiais

2. Lighting e atmosfera
- Pipeline de day lighting consistente para zone/client.
- Ajustar fog, volumetrics, tone mapping e exposure com presets por area.

3. Character visual fidelity
- Multi-mesh rendering completo (hair/helm/weapon/attachments).
- Material override runtime por actor appearance.

Criterio de saida:
- Uma area "hero" com qualidade visual claramente acima do estado atual.

## Fase 3 - MMO Systems Expansion (6-12 semanas)

Meta: fechar lacunas de MMO liveable.

1. Quests (PQuestLog=23)
- Schema + fluxo de quest state.
- UI de log e progresso.

2. Weather e ambiente dinamico (PWeatherChange=17)
- Estados climaticos por area + sincronizacao server->client.

3. Projectile/Party/Appearance updates
- `PProjectile=37`, `PPartyUpdate=38`, `PAppearanceUpdate=39`.
- Priorizar impacto de gameplay em grupo.

Criterio de saida:
- Loop MMO mais completo para grupos e progressao.

## Fase 4 - Tooling e Conteudo em Escala (continuo)

Meta: produtividade de criacao no padrao "facilidade RealmCrafter".

1. GUE UX pass
- Fluxos guiados para Media/Actor Def/Area/Zones.
- Validacao inline (paths, referencias quebradas, constraints).

2. Pipeline de conteudo
- Importadores robustos (model/material/anim) com feedback claro.
- Regras de naming/estrutura para assets.

3. Automation
- Checks de consistencia de DB e assets.
- Smoke tests de inicializacao das tres apps.

## Fase 5 - Performance e Estabilidade (continuo)

Meta: suportar crescimento sem perder qualidade.

1. Performance budgets
- Frame time alvo por tier de GPU.
- CPU budget por subsistema (render/net/gameplay).

2. Profiling continuo
- Telemetria basica em runtime.
- Regressao de performance como blocker de merge em mudancas grandes.

3. Networking robustness
- Tratamento de latencia/perda com UX resiliente.
- Reconciliacao e interpolacao revisadas.

## Ordem Recomendada Imediata (proximas 3 entregas)

1. Character Controller completo + validacao server-side de height.
2. Vertical slice grafico do terrain/material/lighting em 1 area de referencia.
3. Quests baseline (schema + fluxo + UI minima), com refatoracao incremental no que for tocado.

## KPIs de Sucesso

- Tempo para implementar nova feature cai por menor acoplamento.
- Menos regressao em mudancas de net/gameplay/render.
- Aumento perceptivel de qualidade visual no slice de referencia.
- Melhor "combat feel" e legibilidade em testes internos.
