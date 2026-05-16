# Relatorio de Entrada no Mundo (entry_id=1)

- Arquivo: `D:\Github\RealmCrafterOrigins\dist\client\perf_entry_20260515_105804.jsonl`
- Eventos analisados: `998`

## A) Composicao dos objetos
- Objetos no carregamento: **422**
- Modelos unicos: **36**
- ModelCacheGet durante streaming pos-loading: **hits=144 / misses=4**

Top 10 modelos por reutilizacao:
- `assets/models/MainZone/castle_wall_LOD01.b3d`: 118
- `assets/models/MainZone/wall_clean.b3d`: 110
- `assets/models/MainZone/tower_LOD01.b3d`: 24
- `assets/models/MainZone/straw_bail.b3d`: 23
- `assets/models/MainZone/barrel_closed.b3d`: 20
- `assets/models/MainZone/chopping_block.b3d`: 18
- `assets/models/MainZone/wall_broken.b3d`: 16
- `assets/models/MainZone/barrel_broken.b3d`: 12
- `assets/models/MainZone/barrel_open.b3d`: 12
- `assets/models/MainZone/sack01.b3d`: 11

## B) Custo do streaming pos-loading
- Tempo total de streaming (PClientWorldReady -> ultimo Actor::Init): **13519.789 ms**
- Actor::Init (post_loading=true):
- count=137, min=0.002 ms, max=1055.090 ms, mean=14.446 ms, p50=0.003 ms, p90=0.004 ms, p99=574.588 ms
- RebuildMaterialsBuffer (post_loading=true):
- count=6, min=0.072 ms, max=0.253 ms, mean=0.121 ms, p50=0.104 ms, p90=0.185 ms, p99=0.246 ms
- Actor::Init por frame (frames com evento): max=64, media=27.40
- RebuildMaterialsBuffer por frame (frames com evento): max=1, media=1.00

## C) Breakdown do lazy init inicial
- engine_init: 46.961 ms
- load_environment_initial: 470.897 ms
- terrain_init: 27.479 ms
- player_actor_init: 83.383 ms
- rebuild_materials_initial: 0.019 ms
- terrain_load_from_editor: 726.202 ms
- coldata_load: 0.101 ms

## D) Redundancia de LoadEnvironment
- Chamadas em uma entrada: **1**
- callsite=`init_default` path=`assets/ibl/default.hdr` count=1

## E) Ordem de chegada de pacotes
- PStartGame: t=16.142 ms
- PWorldObjects: t=16.596 ms
- PAreaConfig: t=16.925 ms
- PStartGame antes de PWorldObjects: **True**

## F) Estado do gate
- core_done em t=3858.446 ms, core_done=69 / 69, pending=347
- gate_release em t=10323.424 ms, reason=absolute_timeout, pending=137, elapsed_ms=10307.126
- delta core_done -> gate_release: 6464.978 ms

## Notas
- `ModelCacheGet hit/miss` em streaming foi calculado pelos eventos `model_cache_get` com contexto `static_stream_post_loading` e `npc_lazy_init` apos `client_world_ready_sent`.
- Medias por frame foram calculadas somente sobre frames que tiveram ao menos 1 evento.
