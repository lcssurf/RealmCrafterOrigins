# Fase 3 - Fix 7 (Gate timeout 5s) - Relatorio de Medicao

Comparacao entre:
- Antes do Fix 7 (pos-Fix 6): `perf_entry_20260515_105804.jsonl`
- Depois do Fix 7: `perf_entry_20260515_140954.jsonl`

## 4 metricas-chave

| Metrica | Antes (Fix 6) | Depois (Fix 7) | Delta |
|---|---:|---:|---:|
| Tempo total de entrada (ate ultimo Actor::Init) | 15.997 s | 16.267 s | +0.270 s |
| `client_init_ms` | 2460 ms | 2476 ms | +16 ms |
| Streaming pos-loading (`PClientWorldReady` -> ultimo `Actor::Init`) | 13.520 s | 13.774 s | +0.254 s |
| `gate_release.reason` / `pending_total` | `absolute_timeout` / 137 | `absolute_timeout` / 191 | timeout mais cedo, com mais pendencias |

## Validacao especifica do Fix 7

- `gate_release.elapsed_ms`:
  - antes: **10307.126 ms**
  - depois: **5976.754 ms**
- Ou seja, o timeout absoluto de 5s foi aplicado corretamente (saida mais cedo do gate).

## Resultado frente ao objetivo esperado

- O objetivo esperado para o gate neste ponto era sair por completude (`all_done`) em cenario normal.
- Nesta medicao, ainda saiu por `absolute_timeout`.
- Com timeout menor, o gate abriu com mais pendencias (`191`), como esperado quando a completude nao cabe no tempo configurado.

Conclusao: Fix 7 foi aplicado corretamente no mecanismo (timeout de 10s -> 5s e razao `all_done` disponivel), mas o cenario medido ainda nao atinge completude antes do timeout.
