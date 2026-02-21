"""Cache storage and indexing helpers for static-key/round-key artifacts."""

from __future__ import annotations

import csv
import json
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Tuple


@dataclass
class StageArtifacts:
    """Round-level artifact paths produced by an ns-3 run."""

    summary: Path
    flowmon: Path
    netanim: Path


def static_cache_dir(records_base: Path, static_key: str) -> Path:
    """Return the root cache directory for one static key."""
    return records_base / static_key


def round_cache_dir(records_base: Path, static_key: str, round_key: str) -> Path:
    """Return the canonical cache directory for one effective round state."""
    return static_cache_dir(records_base, static_key) / "round_cache" / round_key


def canonical_summary_path(records_base: Path, static_key: str, round_key: str) -> Path:
    """Return the canonical summary path for a cached round state."""
    return round_cache_dir(records_base, static_key, round_key) / "summary.csv"


def round_cache_hit(records_base: Path, static_key: str, round_key: str) -> bool:
    """Check whether canonical summary data exists for a round key."""
    return canonical_summary_path(records_base, static_key, round_key).exists()


def stage_artifacts_for_round(records_base: Path,
                              stage_hash: str,
                              seed: int,
                              round_id: int) -> StageArtifacts:
    """Return legacy stage-output paths used as ingest input."""
    seed_tag = f"seed_{seed}"
    seed_dir = records_base / stage_hash / seed_tag
    return StageArtifacts(
        summary=seed_dir / "csv" / f"round_{round_id}.csv",
        flowmon=seed_dir / "flowmon" / f"flowmon_{round_id}.xml",
        netanim=seed_dir / "viz" / f"netanim_{round_id}.xml",
    )


def cleanup_orchestrator_stage_outputs(records_base: Path, static_key: str) -> int:
    """Remove temporary `seed_*` stage directories for one orchestrated static key."""
    static_dir = static_cache_dir(records_base, static_key)
    if not static_dir.exists():
        return 0

    removed = 0
    for child in static_dir.iterdir():
        if not child.is_dir():
            continue
        if not child.name.startswith("seed_"):
            continue
        shutil.rmtree(child, ignore_errors=True)
        removed += 1
    return removed


def ensure_static_snapshot(records_base: Path, static_key: str, cfg: Dict[str, Any]) -> None:
    """Persist the normalized static config under its static-key directory."""
    target_dir = static_cache_dir(records_base, static_key)
    target_dir.mkdir(parents=True, exist_ok=True)
    path = target_dir / "config.json"
    with path.open("w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")


def ingest_round_to_cache(records_base: Path,
                          static_key: str,
                          round_key: str,
                          stage_hash: str,
                          seed: int,
                          round_id: int,
                          effective_state_payload: Dict[str, Any]) -> None:
    """Copy round outputs into canonical cache and write effective-state metadata."""
    src = stage_artifacts_for_round(records_base, stage_hash, seed, round_id)
    if not src.summary.exists():
        raise FileNotFoundError(f"missing ns3 summary for round {round_id}: {src.summary}")

    target_dir = round_cache_dir(records_base, static_key, round_key)
    target_dir.mkdir(parents=True, exist_ok=True)

    shutil.copy2(src.summary, target_dir / "summary.csv")
    if src.flowmon.exists():
        shutil.copy2(src.flowmon, target_dir / "flowmon.xml")
    if src.netanim.exists():
        shutil.copy2(src.netanim, target_dir / "netanim.xml")

    with (target_dir / "effective_state.json").open("w", encoding="utf-8") as f:
        json.dump(effective_state_payload, f, indent=2)
        f.write("\n")

    note = human_readable_state_note(effective_state_payload.get("effective_state", {}))
    (target_dir / "state_note.txt").write_text(note + "\n", encoding="utf-8")


def round_index_path(records_base: Path, static_key: str) -> Path:
    """Return the consolidated round index path for one static key."""
    return static_cache_dir(records_base, static_key) / "round_index.csv"


def load_round_index(records_base: Path, static_key: str) -> Dict[int, Dict[str, str]]:
    """Load `round_index.csv` as a `round_id -> {round_key, latest_run_at}` map."""
    path = round_index_path(records_base, static_key)
    if not path.exists():
        return {}

    rows: Dict[int, Dict[str, str]] = {}
    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                round_id = int(str(row.get("round_id", "")).strip())
            except ValueError:
                continue
            round_key = str(row.get("round_key", "")).strip()
            if not round_key:
                continue
            rows[round_id] = {
                "round_key": round_key,
                "latest_run_at": str(row.get("latest_run_at", "")).strip(),
            }
    return rows


def upsert_round_index(records_base: Path,
                       static_key: str,
                       round_id: int,
                       round_key: str,
                       latest_run_at: str) -> None:
    """Insert or update one round mapping in `round_index.csv`."""
    rows = load_round_index(records_base, static_key)
    rows[int(round_id)] = {
        "round_key": round_key,
        "latest_run_at": latest_run_at,
    }

    target_dir = static_cache_dir(records_base, static_key)
    target_dir.mkdir(parents=True, exist_ok=True)
    path = round_index_path(records_base, static_key)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["round_id", "round_key", "latest_run_at"])
        writer.writeheader()
        for rid in sorted(rows.keys()):
            row = rows[rid]
            writer.writerow(
                {
                    "round_id": rid,
                    "round_key": row.get("round_key", ""),
                    "latest_run_at": row.get("latest_run_at", ""),
                }
            )


def parse_round_summary(path: Path) -> Dict[str, Any]:
    """Parse the first CSV section from an ns-3 round summary file."""
    text = path.read_text(encoding="utf-8")
    sections = [s for s in text.strip().split("\n\n") if s.strip()]
    if not sections:
        return {}
    lines = sections[0].splitlines()
    if len(lines) < 2:
        return {}
    header = lines[0].split(",")
    values = lines[1].split(",")
    return {k: v for k, v in zip(header, values)}


def collect_round_summaries(records_base: Path,
                            static_key: str,
                            rounds: List[int]) -> Tuple[List[Dict[str, Any]], List[int]]:
    """Resolve requested rounds to canonical summaries and report missing rounds."""
    index = load_round_index(records_base, static_key)
    rows: List[Dict[str, Any]] = []
    missing: List[int] = []

    for r in rounds:
        entry = index.get(r)
        if entry is None:
            missing.append(r)
            continue

        round_key = entry.get("round_key", "")
        if not round_key:
            missing.append(r)
            continue

        summary_path = canonical_summary_path(records_base, static_key, round_key)
        if not summary_path.exists():
            missing.append(r)
            continue

        row = parse_round_summary(summary_path)
        if row:
            row["round"] = r
            rows.append(row)

    return rows, missing


def validate_static_records(records_base: Path, static_key: str) -> List[str]:
    """Validate required canonical files for every mapped round."""
    issues: List[str] = []
    index = load_round_index(records_base, static_key)
    for round_id, entry in sorted(index.items()):
        round_key = str(entry.get("round_key", "")).strip()
        if not round_key:
            issues.append(f"round {round_id}: missing round_key")
            continue
        cache_dir = round_cache_dir(records_base, static_key, round_key)
        if not (cache_dir / "summary.csv").exists():
            issues.append(f"round {round_id}: missing summary.csv for round_key={round_key}")
        if not (cache_dir / "effective_state.json").exists():
            issues.append(f"round {round_id}: missing effective_state.json for round_key={round_key}")
    return issues


def _safe_float(value: Any, default: float = 0.0) -> float:
    """Parse a float value with a fallback default."""
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _format_num(value: float) -> str:
    """Format numeric values compactly for state notes and catalogs."""
    if float(value).is_integer():
        return str(int(value))
    return f"{value:.2f}"


def _selected_indices(mask: List[Any]) -> List[int]:
    """Return selected client indices from a 0/1 mask."""
    out: List[int] = []
    for i, value in enumerate(mask):
        if int(value) == 1:
            out.append(i)
    return out


def _state_summary(effective_state: Dict[str, Any]) -> Dict[str, Any]:
    """Compute compact summary fields for one effective round state."""
    mask = effective_state.get("selected_mask", [])
    positions = effective_state.get("client_positions", [])
    if not isinstance(mask, list):
        mask = []
    if not isinstance(positions, list):
        positions = []

    selected = _selected_indices(mask)
    xs = [_safe_float(p.get("x")) for p in positions if isinstance(p, dict)]
    ys = [_safe_float(p.get("y")) for p in positions if isinstance(p, dict)]

    if xs and ys:
        centroid_x = sum(xs) / len(xs)
        centroid_y = sum(ys) / len(ys)
        min_x = min(xs)
        max_x = max(xs)
        min_y = min(ys)
        max_y = max(ys)
    else:
        centroid_x = 0.0
        centroid_y = 0.0
        min_x = 0.0
        max_x = 0.0
        min_y = 0.0
        max_y = 0.0

    return {
        "num_clients": len(mask),
        "selected_count": len(selected),
        "selected_indices": selected,
        "centroid_x": centroid_x,
        "centroid_y": centroid_y,
        "min_x": min_x,
        "max_x": max_x,
        "min_y": min_y,
        "max_y": max_y,
    }


def human_readable_state_note(effective_state: Dict[str, Any]) -> str:
    """Build a short text summary for one cached effective state."""
    s = _state_summary(effective_state)
    selected = ",".join(str(i) for i in s["selected_indices"]) if s["selected_indices"] else "-"
    return (
        f"selected={s['selected_count']}/{s['num_clients']} "
        f"(clients: {selected}); "
        f"centroid=({_format_num(s['centroid_x'])},{_format_num(s['centroid_y'])}); "
        f"bbox_x=[{_format_num(s['min_x'])},{_format_num(s['max_x'])}] "
        f"bbox_y=[{_format_num(s['min_y'])},{_format_num(s['max_y'])}]"
    )


def export_round_states_catalog(records_base: Path, static_key: str) -> Path:
    """Export a compact CSV catalog of unique cached round states."""
    index = load_round_index(records_base, static_key)
    key_to_rounds: Dict[str, List[int]] = {}
    for round_id, entry in index.items():
        round_key = str(entry.get("round_key", "")).strip()
        if not round_key:
            continue
        key_to_rounds.setdefault(round_key, []).append(round_id)

    static_dir = static_cache_dir(records_base, static_key)
    out_path = static_dir / "round_states.csv"

    if not key_to_rounds:
        with out_path.open("w", encoding="utf-8", newline="") as f:
            writer = csv.DictWriter(
                f,
                fieldnames=[
                    "round_key",
                    "referenced_rounds",
                    "selected_count",
                    "num_clients",
                    "selected_clients",
                    "bbox_x",
                    "bbox_y",
                ],
            )
            writer.writeheader()
        return out_path

    for rounds in key_to_rounds.values():
        rounds.sort()

    rows: List[Dict[str, Any]] = []
    ordered_keys = sorted(key_to_rounds.keys(), key=lambda rk: min(key_to_rounds[rk]))
    for round_key in ordered_keys:
        eff_path = round_cache_dir(records_base, static_key, round_key) / "effective_state.json"
        effective_state: Dict[str, Any] = {}
        if eff_path.exists():
            try:
                payload = json.loads(eff_path.read_text(encoding="utf-8"))
                if isinstance(payload, dict):
                    eff = payload.get("effective_state", {})
                    if isinstance(eff, dict):
                        effective_state = eff
            except (OSError, json.JSONDecodeError):
                effective_state = {}

        summary = _state_summary(effective_state)
        selected_clients = ",".join(str(i) for i in summary["selected_indices"]) if summary["selected_indices"] else "-"
        referenced_rounds = ",".join(str(r) for r in key_to_rounds.get(round_key, []))

        rows.append(
            {
                "round_key": round_key,
                "referenced_rounds": referenced_rounds,
                "selected_count": summary["selected_count"],
                "num_clients": summary["num_clients"],
                "selected_clients": selected_clients,
                "bbox_x": f"[{_format_num(summary['min_x'])},{_format_num(summary['max_x'])}]",
                "bbox_y": f"[{_format_num(summary['min_y'])},{_format_num(summary['max_y'])}]",
            }
        )

    with out_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "round_key",
                "referenced_rounds",
                "selected_count",
                "num_clients",
                "selected_clients",
                "bbox_x",
                "bbox_y",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    return out_path
