# Fase 3 - Fix 6 (Streaming p99 hits-only) - Relatorio de Medicao

Comparacao entre:
- Antes do Fix 6: `perf_entry_20260515_101723.jsonl`
- Depois do Fix 6: `perf_entry_20260515_105804.jsonl`

## 4 metricas-chave

| Metrica | Antes (Fix 5) | Depois (Fix 6) | Delta |
|---|---:|---:|---:|
| Tempo total de entrada (ate ultimo Actor::Init) | 18.438 s | 15.997 s | -2.441 s |
| `client_init_ms` | 2540 ms | 2460 ms | -80 ms |
| Streaming pos-loading (`PClientWorldReady` -> ultimo `Actor::Init`) | 15.882 s | 13.520 s | -2.362 s |
| `gate_release.reason` / `pending_total` | `absolute_timeout` / 142 | `absolute_timeout` / 137 | melhora pequena |

## Validacao especifica do Fix 6

- `static_stream_post_loading`:
  - inits/frame (media): **1.0 -> 27.4**
  - inits/frame (max): **1 -> 64**
- Rebuild de materiais continua coalescido (1/frame; custo baixo).

## Observacoes tecnicas

- O objetivo do Fix 6 (tirar throttle de 1 init/frame) foi atingido.
- Ainda existem **cache misses raros e caros** no post-loading (`3 misses`: ~1055ms, ~865ms, ~59ms), com regra de 1 miss/frame; isso alonga a cauda.
- O gate continua saindo por `absolute_timeout`, porque com `loading_exit_pending_max=0` ele espera completude durante loading.

Conclusao: Fix 6 melhorou significativamente o throughput de streaming, mas o gargalo final de UX ainda esta no comportamento do gate/timeout (Fix 7).
