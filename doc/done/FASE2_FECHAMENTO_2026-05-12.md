# Fase 2 - Fechamento Parcial (2026-05-12)

Objetivo deste fechamento:
- consolidar o que virou padrao oficial do vertical slice grafico (`Training Camp`);
- registrar o que ainda e experimental;
- definir proxima etapa executavel sem perder foco da fase.

## 1) O que virou padrao oficial

1. Render critico por area e autoritativo no servidor:
- fonte de verdade: `server/internal/db/db.go` (`area_config`);
- transporte: `PAreaConfig` (`server/internal/net/client.go`);
- aplicacao runtime no client: `client/src/core/main.cpp`.

2. Config local do client nao e mais fonte de verdade para tuning critico:
- `dist/client/config.toml` ficou restrito ao preset de loading (`low|medium|high`);
- tuning de luz/fog/readability/look/color vem do servidor.

3. Boot de mundo com loading gate e stream progressivo:
- primeira frame jogavel ocorre cedo;
- restante entra de forma progressiva com budget por frame.

## 2) Entrega tecnica consolidada neste ciclo

1. `PAreaConfig` expandido para carregar:
- skybox;
- luz/fog/volumetrics;
- character readability;
- scene look;
- color grading.

2. `area_config` expandida e seed canonico atualizado para:
- `Training Camp`;
- `Starter Zone`.

3. Fluxo de aplicacao no client atualizado:
- reset seguro ao trocar/entrar em area;
- aplicacao deferida quando pacote autoritativo chega;
- clamps defensivos nos ranges.

## 3) O que ainda e experimental

1. Tuning fino de look cinematografico:
- intensidade final de sol;
- contraste/saturacao/vibrance finais por area;
- micro ajuste de atmosfera por direcao de camera.

2. Regressao/perf final da Etapa E:
- foi postergada por decisao de projeto;
- deve voltar no fechamento final da fase.

## 4) Resultado percebido ate aqui

1. Entrada no mundo reduziu de forma significativa no fluxo real de jogo.
2. Visual ficou mais consistente apos migracao para fonte autoritativa.
3. Risco de divergencia por arquivo local de render critico foi reduzido.

## 5) Proxima etapa recomendada (na Fase 2)

1. Fechar Etapa C/B com passes finais de arte:
- ajustar look final do `Training Camp` com base no perfil autoritativo;
- estabilizar leitura de materiais de terreno em medium/far.

2. Rodar checklist de aceite rapido (A/B/C):
- vista ampla;
- personagem em foreground;
- movimento + combate.

3. Congelar preset final da area hero:
- promover Etapa F para concluida;
- abrir backlog priorizado para Fase 3 grafica.
