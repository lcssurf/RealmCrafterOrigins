# Fase 3 - Fix 5 (LoadEnvironment) - Relatorio de Medicao

Arquivo analisado: `D:\Github\RealmCrafterOrigins\dist\client\perf_entry_20260515_101723.jsonl`

## Comparacao com baseline da Fase 2 (valores informados no contexto)

| Metrica | Baseline Fase 2 | Apos Fix 5 | Delta |
|---|---:|---:|---:|
| Tempo total de entrada (ate ultimo Actor::Init) | ~21.0 s | 18.438 s | -2.562 s |
| `client_init_ms` | ~2474 ms | 2540 ms | +66 ms |
| Streaming pos-loading | 8.2 s (148 objs) | 15.882 s (142 objs) | +7.682 s |
| `gate_release.reason` / `pending_total` | `absolute_timeout` / 148 | `absolute_timeout` / 142 | sem mudanca qualitativa |

## Validacao especifica do Fix 5

- `load_environment_call`: **1**
  - `init_default` -> `assets/ibl/default.hdr` (`dur_us=470008`)
- `load_environment_skipped`: **1**
  - `area_apply_pending` -> `assets/ibl/default.hdr`

Conclusao do Fix 5: **OK** (deduplicacao funcionando; segunda carga redundante foi pulada).

## Observacoes relevantes para proximo fix

- `static_stream_post_loading`:
  - total de inits: **142**
  - inits/frame: media **1.0**, max **1**
- `gate_release.reason`: **absolute_timeout**
- `pending_total` no release: **142**

Esses dados confirmam que o gargalo principal restante esta no throttle de streaming (Fix 6).
