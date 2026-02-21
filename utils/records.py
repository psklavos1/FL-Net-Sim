"""Record listing and cleanup utilities for cache/log directories."""

from __future__ import annotations

import json
import shutil
import csv
from pathlib import Path
from typing import Any, Dict, List

from .cache import load_round_index


def _compact_ranges(values: List[int]) -> str:
    """Format sorted integers as compact ranges (for example `1-3,7`)."""
    if not values:
        return "-"
    values = sorted(set(values))
    parts = []
    start = prev = values[0]
    for v in values[1:]:
        if v == prev + 1:
            prev = v
            continue
        parts.append(f"{start}-{prev}" if start != prev else f"{start}")
        start = prev = v
    parts.append(f"{start}-{prev}" if start != prev else f"{start}")
    return ",".join(parts)


def _sanitize_label(value: str) -> str:
    """Normalize labels for table-friendly compact output."""
    value = value.strip().replace(" ", "_")
    return value if value else "-"


def _collect_rounds(path: Path, prefix: str, suffix: str) -> List[int]:
    """Collect round ids from files named `{prefix}<round>{suffix}`."""
    rounds: List[int] = []
    if not path.exists() or not path.is_dir():
        return rounds
    for item in path.iterdir():
        if not item.is_file():
            continue
        name = item.name
        if not (name.startswith(prefix) and name.endswith(suffix)):
            continue
        rid_text = name[len(prefix):-len(suffix)]
        try:
            rounds.append(int(rid_text))
        except ValueError:
            continue
    return sorted(set(rounds))


def _most_common(values: List[str]) -> str:
    """Return most common value with deterministic tie breaking."""
    if not values:
        return ""
    counts: Dict[str, int] = {}
    for v in values:
        counts[v] = counts.get(v, 0) + 1
    return sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[0][0]


def _build_description(cfg: Dict[str, Any]) -> str:
    """Build a short human-readable description from config content."""
    desc = str(cfg.get("description", "")).strip()
    if desc:
        return desc

    net = cfg.get("network", {})
    clients = net.get("clients")
    num_clients = None
    selected = None
    tiers: List[str] = []
    mobilities: List[str] = []

    if isinstance(clients, list):
        num_clients = len(clients)
        has_selected = False
        selected_count = 0
        for c in clients:
            if not isinstance(c, dict):
                continue
            if "selected" in c:
                has_selected = True
                if bool(c.get("selected", False)):
                    selected_count += 1
            tier = c.get("device_tier") or c.get("preset") or c.get("tier")
            if isinstance(tier, str) and tier:
                tiers.append(tier)
            mobility = c.get("mobility_preset")
            if isinstance(mobility, str) and mobility:
                mobilities.append(mobility)
        selected = selected_count if has_selected else num_clients

    parts: List[str] = []
    if num_clients is not None:
        parts.append(f"clients_{num_clients}")
    if selected is not None:
        parts.append(f"selected_{selected}")
    tier = _most_common(tiers)
    if tier:
        parts.append(f"performance_{tier}")
    mobility = _most_common(mobilities)
    if mobility:
        parts.append(f"mobility_{mobility}")

    if not parts:
        return "experiment"
    return _sanitize_label("_".join(parts))


def list_records_compact(records_root: Path) -> List[str]:
    """Return a plain-text table summarizing canonical cached records."""
    records_root.mkdir(parents=True, exist_ok=True)
    rows: List[Dict[str, Any]] = []

    for static_dir in sorted(records_root.iterdir()):
        if not static_dir.is_dir() or static_dir.name.startswith("."):
            continue

        index_path = static_dir / "round_index.csv"
        if not index_path.exists():
            continue

        static_key = static_dir.name
        index = load_round_index(records_root, static_key)
        rounds = sorted(index.keys())
        round_keys = sorted({v.get("round_key", "") for v in index.values() if v.get("round_key", "")})

        round_cache = static_dir / "round_cache"
        csv_count = 0
        flowmon_count = 0
        viz_count = 0
        for rk in round_keys:
            base = round_cache / rk
            if (base / "summary.csv").exists():
                csv_count += 1
            if (base / "flowmon.xml").exists():
                flowmon_count += 1
            if (base / "netanim.xml").exists():
                viz_count += 1

        network_type = "-"
        description = "-"
        cfg_path = static_dir / "config.json"
        if cfg_path.exists():
            try:
                data = json.loads(cfg_path.read_text(encoding="utf-8"))
                if isinstance(data, dict):
                    network_type = str(data.get("network_type", "-"))
                    description = _build_description(data)
            except (OSError, json.JSONDecodeError):
                pass

        rows.append(
            {
                "static_key": static_key,
                "net": network_type,
                "description": description,
                "covered_rounds": _compact_ranges(rounds),
                "unique_states": len(round_keys),
                "csv": csv_count,
                "flowmon": flowmon_count,
                "viz": viz_count,
            }
        )

    if not rows:
        return ["no records found"]

    headers = [
        "static_key",
        "net",
        "description",
        "covered_rounds",
        "unique_states",
        "csv",
        "flowmon",
        "viz",
    ]
    widths = {h: len(h) for h in headers}
    for row in rows:
        for h in headers:
            widths[h] = max(widths[h], len(str(row[h])))

    def fmt_row(values: Dict[str, Any]) -> str:
        return "  ".join(str(values[h]).ljust(widths[h]) for h in headers)

    lines = [fmt_row({h: h for h in headers}), "  ".join("-" * widths[h] for h in headers)]
    for row in rows:
        lines.append(fmt_row(row))
    return lines


def refresh_legacy_records_index(records_root: Path) -> None:
    """Rebuild legacy `index.csv`/`index.json` from current `seed_*` stage folders."""
    records_root.mkdir(parents=True, exist_ok=True)
    rows: List[Dict[str, Any]] = []

    for exp_dir in sorted(records_root.iterdir()):
        if not exp_dir.is_dir() or exp_dir.name.startswith("."):
            continue

        for seed_dir in sorted(exp_dir.iterdir()):
            if not seed_dir.is_dir() or not seed_dir.name.startswith("seed_"):
                continue

            try:
                seed = int(seed_dir.name[len("seed_"):])
            except ValueError:
                seed = 0

            csv_rounds = _collect_rounds(seed_dir / "csv", "round_", ".csv")
            flow_rounds = _collect_rounds(seed_dir / "flowmon", "flowmon_", ".xml")
            viz_rounds = _collect_rounds(seed_dir / "viz", "netanim_", ".xml")
            all_rounds = sorted(set(csv_rounds + flow_rounds + viz_rounds))

            network_type = "-"
            description = "-"
            cfg_path = seed_dir / "config.json"
            if cfg_path.exists():
                try:
                    data = json.loads(cfg_path.read_text(encoding="utf-8"))
                    if isinstance(data, dict):
                        network_type = str(data.get("network_type", "-"))
                        description = str(data.get("description", "-"))
                except (OSError, json.JSONDecodeError):
                    pass

            rows.append(
                {
                    "exp_hash": exp_dir.name,
                    "seed": seed,
                    "network_type": network_type,
                    "description": description,
                    "rounds": all_rounds,
                    "rounds_compact": _compact_ranges(all_rounds),
                    "csv": len(csv_rounds),
                    "flowmon": len(flow_rounds),
                    "viz": len(viz_rounds),
                    "exp_dir": exp_dir.as_posix(),
                }
            )

    index_json_path = records_root / "index.json"
    index_csv_path = records_root / "index.csv"

    with index_json_path.open("w", encoding="utf-8") as f:
        json.dump(rows, f, indent=2)
        f.write("\n")

    with index_csv_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=["exp_hash",
                        "seed",
                        "network_type",
                        "description",
                        "rounds",
                        "csv",
                        "flowmon",
                        "viz",
                        "exp_dir"],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    "exp_hash": row["exp_hash"],
                    "seed": row["seed"],
                    "network_type": row["network_type"],
                    "description": row["description"],
                    "rounds": row["rounds_compact"],
                    "csv": row["csv"],
                    "flowmon": row["flowmon"],
                    "viz": row["viz"],
                    "exp_dir": row["exp_dir"],
                }
            )


def cleanup_dir_keep_gitkeep(path: Path) -> int:
    """Delete directory contents except `.gitkeep` and return removed count."""
    path.mkdir(parents=True, exist_ok=True)
    gitkeep = path / ".gitkeep"
    if not gitkeep.exists():
        gitkeep.touch()
    removed = 0
    for item in path.iterdir():
        if item.name == ".gitkeep":
            continue
        try:
            if item.is_dir() and not item.is_symlink():
                shutil.rmtree(item)
            else:
                item.unlink()
            removed += 1
        except OSError:
            continue
    return removed


def cleanup_logs(logs_root: Path) -> int:
    """Cleanup logs directory while keeping `.gitkeep`."""
    return cleanup_dir_keep_gitkeep(logs_root)


def cleanup_records(records_root: Path) -> int:
    """Cleanup records directory while keeping `.gitkeep`."""
    return cleanup_dir_keep_gitkeep(records_root)
