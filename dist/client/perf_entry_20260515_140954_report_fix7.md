# Relatorio de Entrada no Mundo (entry_id=1)

- Arquivo: `D:\Github\RealmCrafterOrigins\dist\client\perf_entry_20260515_140954.jsonl`
- Eventos analisados: `990`

## A) Composicao dos objetos
- Objetos no carregamento: **422**
- Modelos unicos: **36**
- ModelCacheGet durante streaming pos-loading: **hits=195 / misses=7**

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
- Tempo total de streaming (PClientWorldReady -> ultimo Actor::Init): **13773.906 ms**
- Actor::Init (post_loading=true):
- count=191, min=0.002 ms, max=1128.474 ms, mean=24.414 ms, p50=0.002 ms, p90=0.003 ms, p99=976.197 ms
- RebuildMaterialsBuffer (post_loading=true):
- count=9, min=0.061 ms, max=0.335 ms, mean=0.137 ms, p50=0.109 ms, p90=0.237 ms, p99=0.325 ms
- Actor::Init por frame (frames com evento): max=64, media=23.88
- RebuildMaterialsBuffer por frame (frames com evento): max=1, media=1.00

## C) Breakdown do lazy init inicial
- engine_init: 59.255 ms
- load_environment_initial: 509.563 ms
- terrain_init: 28.583 ms
- player_actor_init: 89.892 ms
- rebuild_materials_initial: 0.019 ms
- terrain_load_from_editor: 741.518 ms
- coldata_load: 0.506 ms

## D) Redundancia de LoadEnvironment
- Chamadas em uma entrada: **1**
- callsite=`init_default` path=`assets/ibl/default.hdr` count=1

## E) Ordem de chegada de pacotes
- PStartGame: t=16.175 ms
- PWorldObjects: t=16.648 ms
- PAreaConfig: t=16.999 ms
- PStartGame antes de PWorldObjects: **True**

## F) Estado do gate
- core_done em t=3924.542 ms, core_done=69 / 69, pending=347
- gate_release em t=5993.095 ms, reason=absolute_timeout, pending=191, elapsed_ms=5976.754
- delta core_done -> gate_release: 2068.553 ms

## Notas
- `ModelCacheGet hit/miss` em streaming foi calculado pelos eventos `model_cache_get` com contexto `static_stream_post_loading` e `npc_lazy_init` apos `client_world_ready_sent`.
- Medias por frame foram calculadas somente sobre frames que tiveram ao menos 1 evento.
