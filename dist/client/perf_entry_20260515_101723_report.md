# Relatorio de Entrada no Mundo (entry_id=1)

- Arquivo: `D:\Github\RealmCrafterOrigins\dist\client\perf_entry_20260515_101723.jsonl`
- Eventos analisados: `1138`

## A) Composicao dos objetos
- Objetos no carregamento: **422**
- Modelos unicos: **36**
- ModelCacheGet durante streaming pos-loading: **hits=152 / misses=1**

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
- Tempo total de streaming (PClientWorldReady -> ultimo Actor::Init): **15881.675 ms**
- Actor::Init (post_loading=true):
- count=142, min=0.005 ms, max=0.043 ms, mean=0.022 ms, p50=0.022 ms, p90=0.026 ms, p99=0.037 ms
- RebuildMaterialsBuffer (post_loading=true):
- count=143, min=0.068 ms, max=0.349 ms, mean=0.103 ms, p50=0.096 ms, p90=0.118 ms, p99=0.302 ms
- Actor::Init por frame (frames com evento): max=1, media=1.00
- RebuildMaterialsBuffer por frame (frames com evento): max=1, media=1.00

## C) Breakdown do lazy init inicial
- engine_init: 57.133 ms
- load_environment_initial: 470.008 ms
- terrain_init: 27.845 ms
- player_actor_init: 86.361 ms
- rebuild_materials_initial: 0.019 ms
- terrain_load_from_editor: 775.977 ms
- coldata_load: 0.475 ms

## D) Redundancia de LoadEnvironment
- Chamadas em uma entrada: **1**
- callsite=`init_default` path=`assets/ibl/default.hdr` count=1

## E) Ordem de chegada de pacotes
- PStartGame: t=16.055 ms
- PWorldObjects: t=16.715 ms
- PAreaConfig: t=17.223 ms
- PStartGame antes de PWorldObjects: **True**

## F) Estado do gate
- core_done em t=3966.965 ms, core_done=70 / 70, pending=346
- gate_release em t=10461.574 ms, reason=absolute_timeout, pending=142, elapsed_ms=10445.317
- delta core_done -> gate_release: 6494.609 ms

## Notas
- `ModelCacheGet hit/miss` em streaming foi calculado pelos eventos `model_cache_get` com contexto `static_stream_post_loading` e `npc_lazy_init` apos `client_world_ready_sent`.
- Medias por frame foram calculadas somente sobre frames que tiveram ao menos 1 evento.
