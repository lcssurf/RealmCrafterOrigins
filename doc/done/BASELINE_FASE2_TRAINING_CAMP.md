# Baseline - Fase 2 (Training Camp)

Use este template para registrar o baseline oficial da Etapa A.

## Metadados

- data_hora:
- build_id_client:
- build_id_server:
- maquina_gpu_cpu:
- resolucao:
- preset/config:
- observacoes_gerais:

## Loading (Start Game -> primeira frame jogavel)

Coletar 3-5 execucoes e registrar:

Nota:
- `Preload core: X/Y` representa apenas o nucleo de objetos dentro do raio de preload no spawn.
- nao representa 100% dos objetos da area.
- registrar tambem `pending_total_ao_liberar_gameplay` para comparar qualidade de entrada.

### Execucao 1
- startgame_sent:
- pstart_received_ms:
- renderer_ready_total_ms:
- client_init_ms:
- server_handleStartGame_total_ms:
- preload_core_done_count:
- preload_core_total_count:
- pending_total_ao_liberar_gameplay:
- observacoes:

### Execucao 2
- startgame_sent:
- pstart_received_ms:
- renderer_ready_total_ms:
- client_init_ms:
- server_handleStartGame_total_ms:
- preload_core_done_count:
- preload_core_total_count:
- pending_total_ao_liberar_gameplay:
- observacoes:

### Execucao 3
- startgame_sent:
- pstart_received_ms:
- renderer_ready_total_ms:
- client_init_ms:
- server_handleStartGame_total_ms:
- preload_core_done_count:
- preload_core_total_count:
- pending_total_ao_liberar_gameplay:
- observacoes:

### Media simples
- avg_pstart_received_ms:
- avg_renderer_ready_total_ms:
- avg_client_init_ms:
- avg_server_handleStartGame_total_ms:
- avg_pending_total_ao_liberar_gameplay:

## Cena A - Vista ampla do terreno

- camera_setup:
- frame_time_avg_ms:
- frame_time_p95_ms:
- stutter_note:
- visual_note_tiling:
- visual_note_atmosfera:
- screenshot_ref:

## Cena B - Personagem foreground

- camera_setup:
- frame_time_avg_ms:
- frame_time_p95_ms:
- stutter_note:
- visual_note_contraste_personagem:
- visual_note_sombra_material:
- screenshot_ref:

## Cena C - Movimento + combate/spell

- roteiro_movimento:
- frame_time_avg_ms:
- frame_time_p95_ms:
- stutter_note:
- visual_note_legibilidade:
- visual_note_fx:
- screenshot_ref:

## Conclusao da Etapa A

- baseline_aprovado: (sim/nao)
- gaps_prioritarios_identificados:
- proximas_acoes_etapa_B:
