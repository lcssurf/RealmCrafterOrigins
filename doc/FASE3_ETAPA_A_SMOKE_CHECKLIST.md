# Fase 3 - Etapa A - Smoke Checklist

Data de atualizacao: 2026-05-12

## 1) Validacoes automatizadas (executadas)

- [x] `go test ./...` em `server` (inclui `server/internal/net` com testes de codec dos novos payloads de acao).
- [x] build do client (`build-client.bat`) com modulo `gameplay/ingame_packet_gate.*`.

## 2) Smoke funcional manual (pendente de rodada dedicada)

- [ ] Login com conta valida.
- [ ] Selecao de personagem.
- [ ] Entrada no mundo (`PStartGame` + `PClientWorldReady` sem regressao).
- [ ] Movimento basico (andar/parar/girar).
- [ ] Acao de combate basica existente (ataque/cast) sem regressao visual.
- [ ] Confirmar logs limpos (sem erro de pacote desconhecido para contratos da Fase 3).

## 3) Resultado parcial da Etapa A

- Foundation de contratos e dispatch modular avancou sem quebrar build.
- Checklist manual fica como gate de fechamento total da Etapa A.
