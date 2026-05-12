# Fase 2 - Vertical Slice Grafico (Plano Detalhado, Revisao Profunda)

Objetivo da fase: entregar um salto visual claro (inspiracao Throne and Liberty) em uma area de referencia, sem perder estabilidade MMO, sem hardcode evitavel e com tuning controlado por parametros centralizados.

## 1) Objetivo final (definicao objetiva)

1. Uma area "hero" com qualidade visual claramente superior ao baseline atual em:
- leitura de materiais do terreno,
- profundidade de iluminacao/atmosfera,
- coerencia personagem x ambiente.

2. Pipeline visual consistente entre client e tools onde aplicavel, evitando discrepancias acidentais de preset.

3. Performance dentro de budget definido para o slice (sem regressao critica).

4. Parametros visuais centralizados e documentados (evitar "tuning escondido" em locais dispersos).

Area hero definida para esta fase:
- `Training Camp`

## Status atual (checkpoint)

- [x] Etapa A - Baseline visual e metricas (incluindo telemetria de world-enter)
- [x] Etapa B - Terrain/material quality pass
- [x] Etapa C - Lighting e atmosfera
- [x] Etapa D - Integracao com personagens no slice
- [ ] Etapa E - Performance e regressao visual (postergada por decisao do projeto)
- [x] Etapa F - Fechamento

Status de encerramento (2026-05-12):
- Fase 2 concluida com Etapa E postergada por decisao do projeto.
- A base autoritativa de render por area foi consolidada (servidor -> client).

Entregas relevantes ja concluidas dentro da Fase 2 ate este checkpoint:
- world-enter instrumentado em client e server com telemetria de tempos.
- loading gate apos selecao de personagem com preload de nucleo perto do spawn.
- presets de loading `low|medium|high` via `dist/client/config.toml`.
- stream progressivo de estaticos com prioridade por camera/distancia/cache.
- limites anti-stutter por frame time e budget por frame.
- limpeza de logs ruidosos por default para foco de diagnostico.
- fallback robusto para path legado de textura (incluindo exports com caminho absoluto Windows).
- normalizacao de fatores PBR no import (albedo/roughness/metallic) para estabilidade visual.
- bootstrap de perfil de iluminacao por area no client (sun dir/color + volumetrics default), com `Training Camp` como primeiro alvo.
- configuracao de clima/luz migrada para fluxo autoritativo via `PAreaConfig` (servidor -> client), removendo dependencia de arquivo local para render critico.
- fog atmosferico controlado por dados autoritativos de area (cor + densidade) no pipeline, sem hardcode por area.
- tuning critico de render (character readability, scene look, color grading) migrado para fluxo autoritativo via `PAreaConfig` + `area_config` no servidor.
- seed autoritativo em `area_config` para `Training Camp` e `Starter Zone` via backend (INSERT OR IGNORE, preservando rows customizadas).

## 2) Escopo principal (com limites)

- Terrain shading quality pass:
  - per-layer tiling coerente,
  - transicao de blend mais natural,
  - macro variation + distance behavior para reduzir repeticao.

- Lighting/atmosphere tuning:
  - sun/fog/volumetrics/tonemap/exposure com presets claros.

- Character integration minima:
  - materiais/contraste legiveis no novo ambiente.

- Loading/streaming na entrada do mundo:
  - reduzir o tempo percebido e real entre "Start Game" e primeira frame jogavel.
  - eliminar gargalos evitaveis no caminho de boot da area hero.

- Refatoracao incremental obrigatoria nos trechos alterados.

Fora de escopo nesta fase:
- rework completo de todo renderer,
- sistemas MMO novos (quests/party/projectile),
- overhaul geral de tooling alem do necessario para o slice.

## 3) Arquivos e modulos relevantes (Graphify-first + ownership)

Renderer/shared:
- `shared/renderer/src/pipeline.cpp`
- `shared/renderer/src/engine.cpp`
- `shared/renderer/src/model.cpp`

Cliente:
- `client/src/renderer/terrain/terrain.cpp`
- `client/src/core/main.cpp`
- `dist/client/shaders/terrainGBuffer.*`
- `dist/client/shaders/gBuffer*` (quando impactado)
- `client/src/net/connection.cpp`
- `client/src/net/codec.h`

Tools/GUE (se afetado no slice):
- `tools/gue/src/zone_renderer.cpp`
- `tools/gue/src/preview_viewport.cpp`

Ownership por trilha:
- Terrain shading: `terrain.cpp` + `terrainGBuffer.*` + integração em `pipeline.cpp`.
- Lighting/atmosfera: `pipeline.cpp` + `engine.cpp` + presets em `main.cpp`.
- Character visual no slice: `model.cpp`/path de material + submissao em `main.cpp`.
- Coerencia tools: `zone_renderer.cpp` e `preview_viewport.cpp`.
- Loading entrada mundo: caminho `handleStartGame` (server) + inicializacao/lazy-load em `main.cpp` (client).

Docs de referencia:
- `doc/done/LANDSCAPE_ARCHITECTURE.md`
- `doc/done/UE_LANDSCAPE_ANALYSIS.md`
- `doc/done/GRAPHICS_TUNING.md`

## 4) Etapas de execucao (com entregaveis e DoD)

### Etapa A - Baseline visual e metricas

Objetivo:
- fixar cena/area de referencia e baseline visual/perf antes das mudancas.

Acoes:
- escolher 1 area hero (ex: Training Camp ou Starter Zone).
- capturar baseline (screenshots comparativas e frame timing).
- listar knobs visuais ativos por viewport.
- congelar cenario de comparacao (camera/time-of-day/path de movimento).

Checklist rapido da Etapa A (Training Camp):
- cena 1: vista ampla do terreno (medium/far distance)
- cena 2: personagem em primeiro plano com fundo de terrain
- cena 3: deslocamento curto com combate/spell ativo
- registrar frame timing nas 3 cenas (baseline)

Saida:
- baseline documentado para comparacao objetiva.

Definition of Done:
- baseline salvo (imagens + notas + frame timing) e aprovado como referencia da fase.

### Etapa A (detalhamento operacional)

Objetivo operacional:
- sair da Etapa A com um baseline confiavel e reproduzivel para comparar cada iteracao da fase.

#### A.1 Preparacao do ambiente

1. Build alvo:
- usar build limpa e estavel de client/server.

2. Config de teste:
- fixar parametros de movimento/telemetria para nao contaminar medicao visual.
- para loading: habilitar somente logs necessarios de tempo por etapa.

3. Area e personagem:
- area hero: `Training Camp`.
- usar sempre o mesmo personagem de teste.

#### A.2 Cenas oficiais do baseline (congeladas)

Cena A - Vista ampla:
- ponto fixo de camera mostrando terreno em medium/far.
- foco: repeticao de tiling, atmosfera, profundidade.

Cena B - Personagem foreground:
- personagem ocupando parte relevante da tela com terreno/fundo visivel.
- foco: contraste, leitura de material, sombra e silhueta.

Cena C - Movimento + combate:
- deslocamento curto predefinido + acao de combate/spell.
- foco: legibilidade em runtime, estabilidade e custo de frame.

Regra:
- camera path e acao devem ser repetiveis entre baseline e iteracoes.

#### A.3 Coleta de metricas (baseline inicial)

Visual (qualitativo guiado):
- repeticao de pattern no terreno (baixo/medio/alto).
- qualidade de transicao entre layers (ruim/ok/boa).
- legibilidade de personagem em movimento/combate (ruim/ok/boa).

Performance (quantitativo):
- frame time medio por cena.
- p95 frame time por cena.
- observacao de stutter perceptivel (sim/nao + nota curta).

Loading (quantitativo):
- tempo total: `Start Game -> primeira frame jogavel`.
- tempos parciais (se telemetria disponivel):
  - entrada em `handleStartGame` (server),
  - processamento de pacotes iniciais (client),
  - carga inicial de terreno/assets e renderer-ready (client).

#### A.4 Template de registro (obrigatorio)

Para cada cena (A/B/C), registrar:
- `build_id`:
- `timestamp`:
- `scene_id`:
- `frame_time_avg_ms`:
- `frame_time_p95_ms`:
- `stutter_note`:
- `visual_note`:
- `screenshot_ref`:

Para loading:
- `world_enter_total_ms`:
- `server_startgame_ms` (se medido):
- `client_world_init_ms` (se medido):
- `observations`:

#### A.5 Critério de saída da Etapa A

Etapa A so fecha quando:
1. as 3 cenas baseline estao capturadas e identificadas.
2. frame time medio e p95 foram registrados por cena.
3. tempo de entrada no mundo foi registrado ao menos 3 vezes (media simples).
4. baseline foi aceito como referencia oficial do `M1`.

#### A.6 Riscos da Etapa A e mitigacao

Risco:
- baseline inconsistente por variacao de camera/acao.

Mitigacao:
- congelar roteiro de captura e repetir exatamente o mesmo fluxo.

Risco:
- comparar builds com estado diferente de assets/config.

Mitigacao:
- registrar build/config usados no template e reaplicar em cada iteracao.

### Etapa B - Terrain material quality pass

Objetivo:
- reduzir "game look genérico" e aumentar qualidade percebida do solo.

Acoes:
- validar tiling por layer e escalas coerentes por material.
- melhorar blend entre layers (height/weight transitions).
- revisar normal/detail/macro variation para reduzir repeticao.
- manter consistencia com fluxo de materiais do projeto.
- garantir que parametros de terrain fiquem em pontos centrais (nao espalhados).

Saida:
- terreno com transicoes mais naturais e menos repeticao visual.

Definition of Done:
- reducao clara de repeticao de pattern no medium/far distance.
- nenhuma quebra de compatibilidade com assets atuais da area hero.

Checklist de validacao de fechamento (Etapa B):
- [ ] nenhum objeto-chave da area hero aparece com material preto/invalido.
- [ ] texturas legadas com path exportado (ex: absoluto Windows) resolvem por fallback.
- [ ] fatores PBR default estao estaveis em assets sem metadata completa.
- [ ] consistencia visual de materiais em 3 cenas oficiais (A/B/C).
- [ ] sem regressao funcional de carregamento na entrada do mundo.
- [ ] sem hardcode novo fora dos pontos de configuracao previstos.

### Etapa C - Lighting e atmosfera

Objetivo:
- aumentar profundidade visual e mood sem comprometer leitura de gameplay.

Acoes:
- calibrar direcao/cor/intensidade de sol.
- ajustar fog/volumetrics por area.
- revisar tonemapping/exposure/contraste para preservar detalhe.
- harmonizar parametros de client e tools quando necessario.
- manter legibilidade de gameplay como restricao dura (nao somente "ficar bonito").

Saida:
- iluminacao consistente, atmosfera mais cinematica e leitura melhor.

Definition of Done:
- contraste personagem/chao/fundo aceitavel em combate.
- sem clipping severo de highlights/shadows no cenario alvo.

### Etapa D - Integracao com personagens no slice

Objetivo:
- garantir que personagens e NPCs "sentem" bem no ambiente novo.

Acoes:
- validar resposta de materiais de actor na nova luz.
- revisar contraste personagem x fundo.
- ajustar quando necessario para legibilidade de combate.
- verificar consistencia entre actor player e NPC em mesma area/luz.

Saida:
- personagens visualmente coerentes com o cenario e legiveis em gameplay.

Definition of Done:
- leitura de silhueta/animacao/impactos visuais preservada durante movimento/combate.

Fechamento executado (2026-05-11):
- player e NPC marcados explicitamente como `Character` no pipeline (sem afetar cenario estatico).
- G-buffer passou a carregar `character mask` dedicado (canal `gRMA.a`) para tuning seletivo.
- lighting global recebeu tuning seletivo de legibilidade para personagens:
  - `shadow_lift`, `rim_strength`, `rim_exponent`, `min_ndotl`, `ambient_boost`.
- tuning base de legibilidade iniciou centralizado no client (preset fechado no binario), e foi consolidado para fonte autoritativa no servidor via `PAreaConfig`.

### Etapa E - Performance e regressao visual

Objetivo:
- garantir ganho visual com custo controlado.

Acoes:
- medir frame time em cenarios representativos.
- comparar com baseline.
- corrigir regressao severa de performance.
- validar custo em pelo menos 2 cenarios: parado em vista ampla e deslocamento com combate.

Saida:
- vertical slice visual aprovado com budget aceitavel.

Definition of Done:
- regressao controlada dentro do budget acordado para o slice.

### Etapa F - Fechamento

Objetivo:
- congelar presets do slice e registrar o que vira padrao.

Acoes:
- documentar parametros finais.
- registrar backlog do proximo ciclo grafico.
- atualizar status da fase.
- registrar explicitamente o que virou padrao e o que e experimental.

Fechamento parcial executado (2026-05-12):
- fonte de verdade de render critico consolidada no servidor (`area_config` -> `PAreaConfig` -> client runtime).
- client sem dependencia de `config.toml` para tuning critico de iluminacao/atmosfera/cor.
- seeds canonicos de `Training Camp` e `Starter Zone` aplicados no backend para boot consistente.
- registro de fechamento parcial publicado em `doc/FASE2_FECHAMENTO_2026-05-12.md`.

Padrao atual da fase:
- configuracao autoritativa de render por area via servidor.
- fallback de seguranca no client apenas ate chegada da primeira `PAreaConfig`.

Experimental (ainda sujeito a iteracao):
- refinos finos de look cinematografico (intensidade de sol, contraste final, grading fino).
- ultimo passe de regressao/perf visual da Etapa E.

## 5) Criterios de aceite (go/no-go da fase)

1. Area hero apresenta salto visual claro vs baseline comparativo.
2. Terreno: blend/tiling e repeticao melhorados de forma perceptivel.
3. Lighting/atmosfera: qualidade maior sem perda de leitura de gameplay.
4. Personagens legiveis e coerentes no ambiente final do slice.
5. Sem regressao critica de performance no cenario-alvo.
6. Parametros principais centralizados e documentados.
7. Sem hardcode novo evitavel; excecoes com justificativa registrada.

## 6) Riscos principais e mitigacao

1. Visual melhora, mas performance degrada acima do aceitavel.
2. Excesso de pos-processo prejudica leitura de combate.
3. Divergencia visual entre client e tools.
4. Tuning ficar dependente de "ajuste manual escondido" sem documentacao.

Mitigacoes:
- ajustes incrementais com comparacao por baseline.
- validacao de legibilidade durante tuning (nao so screenshot bonita).
- usar knobs centralizados e documentados.
- checkpoint rapido de perf a cada bloco que mexe em shader/pipeline.

## 7) Gate de qualidade por PR (obrigatorio)

- Corretude: sem quebrar fluxo de jogo/render.
- Organizacao: mudancas modulares, sem acoplamento acidental.
- Centralizacao: parametros em pontos claros de configuracao.
- Data-driven: priorizar fonte editavel (config/assets/db) quando aplicavel.
- Debugabilidade: logs/flags suficientes para investigar regressao visual/perf.
- Evidencia: anexar comparativo baseline x atual (imagem + nota de perf).

## 8) Metricas minimas (baseline vs final)

- FPS/frame time no cenario hero (parado e em deslocamento).
- Presenca de artefatos visuais (seam, tiling obvio, flicker de sombra, clipping).
- Legibilidade de personagem em combate (qualitativo guiado por checklist).
- Consistencia client x tools (quando o mesmo conteudo e visualizado em ambos).

## 9) Backlog tecnico inicial da fase (priorizado)

1. Terrain:
- confirmar estado atual de `u_tilings` / macro variation / distance behavior no shader de terrain.
- fechar lacunas restantes do plano de `LANDSCAPE_ARCHITECTURE.md` que ainda nao estao no runtime final.

2. Lighting:
- normalizar preset base de sun/fog/volumetric/tonemap para area hero.
- alinhar toggles/features entre client e preview/tooling.

3. Character readability:
- validar resposta de material do actor em iluminacao final do slice.
- corrigir contraste insuficiente de silhueta se necessario.

## 10) Ordem recomendada

1. Etapa A (baseline)
2. Etapa B (terrain)
3. Etapa C (lighting/atmosfera)
4. Etapa D (personagens no slice)
5. Etapa E (performance/regressao)
6. Etapa F (fechamento)

## 11) Milestones executaveis (alto impacto)

### M1 - Baseline fechado (go gate)
- Area hero congelada: `Training Camp`.
- Cenas de comparacao fixadas (camera path + estado de gameplay).
- Baseline visual/perf registrado.

### M2 - Terrain premium aprovado
- Reducao clara de repeticao de tiling.
- Blend entre layers com transicoes mais naturais.
- Macro variation e comportamento em distancia calibrados.

### M3 - Lighting/atmosfera aprovado
- Preset de luz/fog/volumetrics/tonemap estabilizado.
- Legibilidade de personagem mantida em combate.

### M4 - Slice final aprovado
- Integracao final personagem x ambiente.
- Performance dentro do budget do slice.
- Documentacao de parametros e decisoes congelada.

## 12) Backlog tecnico por trilha e arquivo

### Trilha A - Terrain shading (prioridade maxima)

Objetivo:
- tirar look repetitivo/plastico e subir realismo percebido do chao.

Arquivos foco:
- `dist/client/shaders/terrainGBuffer.fs`
- `dist/client/shaders/terrainGBuffer.vs`
- `client/src/renderer/terrain/terrain.cpp`
- `shared/renderer/src/pipeline.cpp`

Tarefas:
1. auditar uniforms realmente usados no runtime (`u_tilings`, macro, fade params).
2. validar per-layer tiling efetivo por material.
3. revisar blend (height/weight) para reduzir "costura" entre materiais.
4. calibrar macro variation para quebrar repeticao sem sujar leitura.
5. revisar distance behavior para medium/far sem pattern obvio.

Definition of Done da trilha:
- terreno legivel de perto e limpo de padrao repetitivo no medium/far.

### Trilha B - Lighting e atmosfera

Objetivo:
- profundidade cinematica com leitura de gameplay preservada.

Arquivos foco:
- `shared/renderer/src/pipeline.cpp`
- `shared/renderer/src/engine.cpp`
- `client/src/core/main.cpp`

Tarefas:
1. consolidar preset base de sol (direcao/cor/intensidade).
2. calibrar fog + volumetrics por area hero.
3. revisar tonemap/exposure para manter detalhe em highlights/shadows.
4. validar metodos de shadow e custo visual/perf no slice.

Definition of Done da trilha:
- imagem com profundidade maior e sem "lavar" personagem/combate.

### Trilha C - Character readability no slice

Objetivo:
- personagem se destacar corretamente do fundo sem parecer desconectado.

Arquivos foco:
- `shared/renderer/src/model.cpp`
- `client/src/core/main.cpp`
- `shared/renderer/src/pipeline.cpp` (passes relevantes)

Tarefas:
1. validar resposta de material sob iluminacao final.
2. ajustar contraste personagem x terreno/fog.
3. garantir leitura de silhueta durante movimento/combate.

Definition of Done da trilha:
- silhueta e feedback visual de acao continuam claros.

### Trilha D - Coerencia tools (quando impactado)

Objetivo:
- evitar surpresa entre visual no client e visual no fluxo de edicao.

Arquivos foco:
- `tools/gue/src/zone_renderer.cpp`
- `tools/gue/src/preview_viewport.cpp`

Tarefas:
1. alinhar toggles/presets essenciais (quando aplicavel).
2. garantir que a leitura de material no tooling nao contradiga o client.

### Trilha E - Loading da entrada no mundo (alta prioridade)

Objetivo:
- reduzir significativamente a demora apos selecao de personagem ate o jogador assumir controle no mundo.

Arquivos foco:
- `server/internal/net/client.go` (fluxo `handleStartGame`)
- `server/cmd/server/main.go` (dados enviados na entrada, quando aplicavel)
- `client/src/core/main.cpp` (pipeline de inicializacao/world-enter)
- `client/src/renderer/terrain/terrain.cpp` (carga de heightmap/splat/materials)
- `shared/renderer/src/engine.cpp` e `shared/renderer/src/pipeline.cpp` (custos de init/lazy-init)
- `client/src/net/connection.cpp` (telemetria de etapas de rede, se necessario)

Tarefas:
1. Instrumentar tempo por etapa no fluxo de entrada (server e client):
- tempo de `handleStartGame`,
- tempo de recebimento/processamento de pacotes iniciais,
- tempo de carga de terreno/assets/renderer-ready.

2. Identificar gargalos principais e atacar em ordem:
- carregamentos bloqueantes no thread principal,
- inicializacoes redundantes,
- trabalho pesado antes da primeira frame jogavel.

3. Aplicar estrategia de boot progressivo:
- primeira frame jogavel com minimo necessario,
- assets secundarios/nao-criticos carregados depois (lazy/assinc quando seguro).

4. Preservar consistencia visual:
- otimizar sem degradar o resultado final do vertical slice.

Definition of Done da trilha:
- queda perceptivel e mensuravel no tempo de entrada no mundo na area hero.

## 13) Metricas numericas iniciais (alvos do slice)

Observacao:
- alvos sao de fase (nao contrato final de produto) e podem ser refinados.

Visual:
- repeticao de pattern em terreno: reduzir claramente em pelo menos 2/3 cenas de baseline.
- contraste de personagem: manter leitura clara em todas as cenas do checklist.

Performance (Training Camp):
- regressao de frame time medio: ideal <= 15% vs baseline.
- p95 frame time: sem degradacao severa percebida no deslocamento com combate.

Loading (pos selecao de personagem):
- medir tempo total "Start Game -> primeira frame jogavel".
- alvo inicial da fase: reduzir esse tempo em pelo menos 30% vs baseline atual da mesma build/maquina.

Estabilidade:
- 0 crash novo em client/server/tools relacionados ao slice.
- 0 regressao bloqueante de gameplay core durante validacao.

## 14) Matriz de validacao (por cena)

Cena A - Vista ampla do terreno:
- verificar repeticao de tile.
- verificar consistencia de fog/atmosfera.
- medir frame timing.

Cena B - Personagem em primeiro plano:
- verificar contraste e resposta de material.
- verificar sombra/normal detail no personagem e no chao.
- medir frame timing.

Cena C - Deslocamento com combate:
- verificar leitura de VFX/telegraph.
- verificar que atmosfera nao compromete alvo/silhueta.
- medir frame timing e stutter perceptivel.

## 15) Estrategia de rollout e rollback

Rollout:
1. introduzir mudancas por blocos pequenos (terrain -> lighting -> personagem).
2. validar build/smoke a cada bloco.
3. registrar preset/parametro junto da mudanca.

Rollback:
1. manter alteracoes segmentadas por commit/arquivo.
2. se regressao forte, reverter apenas bloco da trilha afetada.
3. preservar melhorias aprovadas de outras trilhas.

## 16) Riscos tecnicos especificos da fase

1. Shader complexity subir demais e gerar stutter em hardware-alvo.
Mitigacao:
- checkpoints de perf por etapa e simplificacao de caminho critico.

2. Volumetrics + fog mascararem leitura de combate.
Mitigacao:
- legibilidade como gate obrigatorio de aceite.

3. Divergencia de preset entre client e tooling.
Mitigacao:
- tabela unica de preset de referencia da area hero.

4. Tuning disperso (hardcode em varios pontos).
Mitigacao:
- centralizar knobs e registrar todos no fechamento da fase.

## 17) Entregaveis finais da Fase 2

1. Vertical slice visual aprovado em `Training Camp`.
2. Parametros finais documentados (terrain + lighting + atmosfera + features).
3. Registro de baseline vs final (comparativos).
4. Relatorio curto de impacto em perf e riscos residuais.
5. Backlog priorizado para Fase 3 grafica (expansao para outras areas).
6. Relatorio de loading da entrada no mundo (baseline, gargalos e resultado final apos otimizacoes).
