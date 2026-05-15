#!/usr/bin/env python3
import argparse
import json
import math
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Dict, List, Optional


def percentile(values: List[float], p: float) -> Optional[float]:
    if not values:
        return None
    xs = sorted(values)
    if len(xs) == 1:
        return xs[0]
    idx = (len(xs) - 1) * p
    lo = int(math.floor(idx))
    hi = int(math.ceil(idx))
    if lo == hi:
        return xs[lo]
    w = idx - lo
    return xs[lo] * (1.0 - w) + xs[hi] * w


def series_stats(values: List[float]) -> Dict[str, Optional[float]]:
    if not values:
        return {
            "count": 0,
            "min": None,
            "max": None,
            "mean": None,
            "p50": None,
            "p90": None,
            "p99": None,
        }
    return {
        "count": len(values),
        "min": min(values),
        "max": max(values),
        "mean": sum(values) / len(values),
        "p50": percentile(values, 0.50),
        "p90": percentile(values, 0.90),
        "p99": percentile(values, 0.99),
    }


def fmt_num(v: Optional[float], ndigits: int = 2) -> str:
    if v is None:
        return "n/a"
    return f"{v:.{ndigits}f}"


def fmt_us_to_ms(v: Optional[float], ndigits: int = 3) -> str:
    if v is None:
        return "n/a"
    return f"{(v / 1000.0):.{ndigits}f}"


def choose_entry(events: List[Dict[str, Any]], entry_id: Optional[int]) -> int:
    ids = sorted({int(e["entry_id"]) for e in events if "entry_id" in e})
    if not ids:
        raise RuntimeError("Nenhum entry_id encontrado no JSONL.")
    if entry_id is None:
        return ids[-1]
    if entry_id not in ids:
        raise RuntimeError(f"entry_id={entry_id} nao encontrado. Disponiveis: {ids}")
    return entry_id


def load_events(path: Path) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as f:
        for i, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError as ex:
                raise RuntimeError(f"JSON invalido na linha {i}: {ex}") from ex
    return out


def markdown_report(path: Path, entry_events: List[Dict[str, Any]], entry_id: int) -> str:
    entry_events = sorted(entry_events, key=lambda e: int(e.get("ts_us", 0)))
    by_event: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    for e in entry_events:
        by_event[e.get("event", "")].append(e)

    # Base timestamps
    entry_start = by_event.get("entry_start", [])
    entry_start_ts = int(entry_start[0]["ts_us"]) if entry_start else int(entry_events[0].get("ts_us", 0))

    world_ready_events = by_event.get("client_world_ready_sent", [])
    world_ready_ts = int(world_ready_events[0]["ts_us"]) if world_ready_events else None

    # A) Composition
    obj_summary = by_event.get("world_objects_summary", [])
    obj_summary_evt = obj_summary[0] if obj_summary else {}
    obj_count = int(obj_summary_evt.get("object_count", 0))
    unique_models = int(obj_summary_evt.get("unique_models", 0))

    model_counts = []
    for e in by_event.get("world_object_model_count", []):
        model_counts.append((str(e.get("model", "")), int(e.get("count", 0))))
    model_counts.sort(key=lambda x: (-x[1], x[0]))
    top10 = model_counts[:10]

    cache_stream = []
    for e in by_event.get("model_cache_get", []):
        ctx = str(e.get("context", ""))
        ts = int(e.get("ts_us", 0))
        if ctx not in {"static_stream_post_loading", "npc_lazy_init"}:
            continue
        if world_ready_ts is not None and ts < world_ready_ts:
            continue
        cache_stream.append(e)
    cache_hits = sum(1 for e in cache_stream if bool(e.get("hit", False)))
    cache_misses = len(cache_stream) - cache_hits

    # B) Post-loading streaming costs
    actor_stream = [
        e for e in by_event.get("actor_init", [])
        if bool(e.get("post_loading", False))
    ]
    actor_stream_durs = [float(e.get("dur_us", 0)) for e in actor_stream]
    actor_stream_stats = series_stats(actor_stream_durs)

    rebuild_stream = [
        e for e in by_event.get("rebuild_materials", [])
        if bool(e.get("post_loading", False))
    ]
    rebuild_stream_durs = [float(e.get("dur_us", 0)) for e in rebuild_stream]
    rebuild_stream_stats = series_stats(rebuild_stream_durs)

    last_actor_init_ts = max((int(e.get("ts_us", 0)) for e in actor_stream), default=None)
    total_stream_tail_ms = None
    if world_ready_ts is not None and last_actor_init_ts is not None and last_actor_init_ts >= world_ready_ts:
        total_stream_tail_ms = (last_actor_init_ts - world_ready_ts) / 1000.0

    actor_per_frame = Counter(int(e.get("frame", -1)) for e in actor_stream)
    actor_frame_values = [v for _, v in actor_per_frame.items() if v > 0]
    actor_per_frame_avg = (sum(actor_frame_values) / len(actor_frame_values)) if actor_frame_values else None
    actor_per_frame_max = max(actor_frame_values) if actor_frame_values else None

    rebuild_per_frame = Counter(int(e.get("frame", -1)) for e in rebuild_stream)
    rebuild_frame_values = [v for _, v in rebuild_per_frame.items() if v > 0]
    rebuild_per_frame_avg = (sum(rebuild_frame_values) / len(rebuild_frame_values)) if rebuild_frame_values else None
    rebuild_per_frame_max = max(rebuild_frame_values) if rebuild_frame_values else None

    # C) Lazy init breakdown
    lazy_steps = {}
    for e in by_event.get("lazy_stage", []):
        step = str(e.get("stage", ""))
        if step not in lazy_steps:
            lazy_steps[step] = float(e.get("dur_us", 0))
    expected_steps = [
        "engine_init",
        "load_environment_initial",
        "terrain_init",
        "player_actor_init",
        "rebuild_materials_initial",
        "terrain_load_from_editor",
        "coldata_load",
    ]

    # D) LoadEnvironment redundancy
    load_env_calls = by_event.get("load_environment_call", [])
    load_env_counter = Counter((str(e.get("callsite", "")), str(e.get("path", ""))) for e in load_env_calls)

    # E) Packet arrival order
    packet_events = [e for e in by_event.get("packet_arrival", [])
                     if str(e.get("packet", "")) in {"PStartGame", "PWorldObjects", "PAreaConfig"}]
    packet_events_sorted = sorted(packet_events, key=lambda e: int(e.get("ts_us", 0)))
    first_packet_ts = {str(e.get("packet", "")): int(e.get("ts_us", 0)) for e in packet_events_sorted}
    pstart_before_pobjects = None
    if "PStartGame" in first_packet_ts and "PWorldObjects" in first_packet_ts:
        pstart_before_pobjects = first_packet_ts["PStartGame"] < first_packet_ts["PWorldObjects"]

    # F) Gate state
    core_done_evt = by_event.get("gate_core_done", [])
    gate_release_evt = by_event.get("gate_release", [])
    cd = core_done_evt[0] if core_done_evt else None
    gr = gate_release_evt[0] if gate_release_evt else None

    lines: List[str] = []
    lines.append(f"# Relatorio de Entrada no Mundo (entry_id={entry_id})")
    lines.append("")
    lines.append(f"- Arquivo: `{path}`")
    lines.append(f"- Eventos analisados: `{len(entry_events)}`")
    lines.append("")

    lines.append("## A) Composicao dos objetos")
    lines.append(f"- Objetos no carregamento: **{obj_count}**")
    lines.append(f"- Modelos unicos: **{unique_models}**")
    lines.append(f"- ModelCacheGet durante streaming pos-loading: **hits={cache_hits} / misses={cache_misses}**")
    lines.append("")
    lines.append("Top 10 modelos por reutilizacao:")
    if not top10:
        lines.append("- n/a")
    else:
        for model, cnt in top10:
            lines.append(f"- `{model}`: {cnt}")
    lines.append("")

    lines.append("## B) Custo do streaming pos-loading")
    lines.append(f"- Tempo total de streaming (PClientWorldReady -> ultimo Actor::Init): **{fmt_num(total_stream_tail_ms, 3)} ms**")
    lines.append("- Actor::Init (post_loading=true):")
    lines.append(f"- count={actor_stream_stats['count']}, min={fmt_us_to_ms(actor_stream_stats['min'])} ms, max={fmt_us_to_ms(actor_stream_stats['max'])} ms, mean={fmt_us_to_ms(actor_stream_stats['mean'])} ms, p50={fmt_us_to_ms(actor_stream_stats['p50'])} ms, p90={fmt_us_to_ms(actor_stream_stats['p90'])} ms, p99={fmt_us_to_ms(actor_stream_stats['p99'])} ms")
    lines.append("- RebuildMaterialsBuffer (post_loading=true):")
    lines.append(f"- count={rebuild_stream_stats['count']}, min={fmt_us_to_ms(rebuild_stream_stats['min'])} ms, max={fmt_us_to_ms(rebuild_stream_stats['max'])} ms, mean={fmt_us_to_ms(rebuild_stream_stats['mean'])} ms, p50={fmt_us_to_ms(rebuild_stream_stats['p50'])} ms, p90={fmt_us_to_ms(rebuild_stream_stats['p90'])} ms, p99={fmt_us_to_ms(rebuild_stream_stats['p99'])} ms")
    lines.append(f"- Actor::Init por frame (frames com evento): max={actor_per_frame_max if actor_per_frame_max is not None else 'n/a'}, media={fmt_num(actor_per_frame_avg)}")
    lines.append(f"- RebuildMaterialsBuffer por frame (frames com evento): max={rebuild_per_frame_max if rebuild_per_frame_max is not None else 'n/a'}, media={fmt_num(rebuild_per_frame_avg)}")
    lines.append("")

    lines.append("## C) Breakdown do lazy init inicial")
    for step in expected_steps:
        dur = lazy_steps.get(step)
        lines.append(f"- {step}: {fmt_us_to_ms(dur)} ms")
    lines.append("")

    lines.append("## D) Redundancia de LoadEnvironment")
    lines.append(f"- Chamadas em uma entrada: **{len(load_env_calls)}**")
    if not load_env_calls:
        lines.append("- n/a")
    else:
        for (callsite, path_value), cnt in sorted(load_env_counter.items(), key=lambda x: (-x[1], x[0][0], x[0][1])):
            lines.append(f"- callsite=`{callsite}` path=`{path_value}` count={cnt}")
    lines.append("")

    lines.append("## E) Ordem de chegada de pacotes")
    if not packet_events_sorted:
        lines.append("- n/a")
    else:
        for e in packet_events_sorted:
            pkt = str(e.get("packet", ""))
            ts_us = int(e.get("ts_us", 0))
            rel_ms = (ts_us - entry_start_ts) / 1000.0
            lines.append(f"- {pkt}: t={rel_ms:.3f} ms")
    lines.append(f"- PStartGame antes de PWorldObjects: **{pstart_before_pobjects if pstart_before_pobjects is not None else 'n/a'}**")
    lines.append("")

    lines.append("## F) Estado do gate")
    if cd is None:
        lines.append("- core_done: n/a")
    else:
        cd_rel_ms = (int(cd.get("ts_us", 0)) - entry_start_ts) / 1000.0
        lines.append(
            f"- core_done em t={cd_rel_ms:.3f} ms, core_done={cd.get('core_done')} / {cd.get('core_total')}, pending={cd.get('pending_total')}"
        )
    if gr is None:
        lines.append("- gate_release: n/a")
    else:
        gr_rel_ms = (int(gr.get("ts_us", 0)) - entry_start_ts) / 1000.0
        lines.append(
            f"- gate_release em t={gr_rel_ms:.3f} ms, reason={gr.get('reason')}, pending={gr.get('pending_total')}, elapsed_ms={gr.get('elapsed_ms')}"
        )
    if cd is not None and gr is not None:
        dt_ms = (int(gr.get("ts_us", 0)) - int(cd.get("ts_us", 0))) / 1000.0
        lines.append(f"- delta core_done -> gate_release: {dt_ms:.3f} ms")
    lines.append("")

    lines.append("## Notas")
    lines.append("- `ModelCacheGet hit/miss` em streaming foi calculado pelos eventos `model_cache_get` com contexto `static_stream_post_loading` e `npc_lazy_init` apos `client_world_ready_sent`.")
    lines.append("- Medias por frame foram calculadas somente sobre frames que tiveram ao menos 1 evento.")
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description="Analisa perf_entry_*.jsonl e gera relatorio markdown.")
    ap.add_argument("jsonl", type=Path, help="Arquivo perf_entry_*.jsonl")
    ap.add_argument("--entry-id", type=int, default=None, help="entry_id especifico (default: ultimo)")
    ap.add_argument("--out", type=Path, default=None, help="Arquivo .md de saida")
    args = ap.parse_args()

    events = load_events(args.jsonl)
    if not events:
        raise RuntimeError("Arquivo vazio.")
    entry_id = choose_entry(events, args.entry_id)
    entry_events = [e for e in events if int(e.get("entry_id", -1)) == entry_id]

    md = markdown_report(args.jsonl, entry_events, entry_id)
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(md, encoding="utf-8")
    print(md)


if __name__ == "__main__":
    main()
