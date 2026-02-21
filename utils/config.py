"""Helpers for loading, validating, and shaping orchestrator JSON configs."""

from __future__ import annotations

import json
from collections import OrderedDict
from copy import deepcopy
from pathlib import Path
from typing import Any, Dict, List


def load_config(path: Path) -> OrderedDict:
    """Load a config file while preserving key order."""
    with path.open("r", encoding="utf-8") as f:
        return json.load(f, object_pairs_hook=OrderedDict)


def write_config(path: Path, cfg: Dict[str, Any]) -> None:
    """Write a config snapshot with stable JSON formatting."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")


def strip_orchestration(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Return a deep copy without the `orchestration` section."""
    cleaned = deepcopy(cfg)
    cleaned.pop("orchestration", None)
    return cleaned


def set_round(cfg: Dict[str, Any], round_id: int) -> Dict[str, Any]:
    """Return a deep copy with `reproducibility.round` set to `round_id`."""
    cfg = deepcopy(cfg)
    repro = cfg.get("reproducibility")
    if not isinstance(repro, dict):
        repro = {}
        cfg["reproducibility"] = repro
    repro["round"] = int(round_id)
    return cfg


def clamp_parallelism(value: int, max_cpus: int) -> int:
    """Clamp requested parallelism to available CPU capacity."""
    if value and value > 0:
        return min(value, max_cpus)
    return max_cpus


def resolve_path(value: str, base_dir: Path) -> Path:
    """Resolve a possibly relative path against `base_dir`."""
    p = Path(value)
    if not p.is_absolute():
        p = (base_dir / p).resolve()
    return p


def parse_rounds_spec(spec: Any) -> List[int]:
    """Parse round input as `N` or `A..B` and return explicit round ids."""
    if spec is None:
        return []
    if isinstance(spec, int):
        return list(range(1, max(1, spec) + 1))

    s = str(spec).strip()
    if not s:
        return []
    if ".." in s:
        parts = s.split("..", 1)
        start = int(parts[0])
        end = int(parts[1])
        if start < 1 or end < 1:
            raise ValueError("rounds range must be >= 1")
        if end < start:
            raise ValueError("rounds range end must be >= start")
        return list(range(start, end + 1))

    count = int(s)
    return list(range(1, max(1, count) + 1))


def minimal_validate(cfg: Dict[str, Any], supported_networks: List[str] | tuple[str, ...]) -> None:
    """Run minimal required validation before orchestration."""
    network_type = cfg.get("network_type")
    if network_type not in supported_networks:
        raise ValueError(f"network_type must be one of: {', '.join(supported_networks)}")
    repro = cfg.get("reproducibility")
    if not isinstance(repro, dict) or "seed" not in repro:
        raise ValueError("reproducibility.seed must be set")
    if "network" not in cfg:
        raise ValueError("network section is required")
    if "fl_traffic" not in cfg:
        raise ValueError("fl_traffic section is required")
