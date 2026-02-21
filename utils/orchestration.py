"""Round-level participation and positioning materialization logic."""

from __future__ import annotations

import csv
import math
import random
from pathlib import Path
from typing import Any, Dict, List

from .hashing import fnv1a64


def load_participation_matrix(path: Path) -> List[List[int]]:
    """Load a 0/1 participation matrix from CSV-like text."""
    rows: List[List[int]] = []
    with path.open("r", encoding="utf-8") as f:
        reader = csv.reader(f)
        for raw in reader:
            if not raw:
                continue
            parts = [p.strip() for p in raw if p.strip() != ""]
            if len(parts) == 1 and (" " in parts[0] or "\t" in parts[0]):
                parts = [p for p in parts[0].replace("\t", " ").split(" ") if p]
            if not parts:
                continue
            if any(p not in ("0", "1") for p in parts):
                continue
            rows.append([1 if p == "1" else 0 for p in parts])
    return rows


def validate_participation_matrix(matrix: List[List[int]],
                                  required_rounds: int,
                                  num_clients: int) -> bool:
    """Validate matrix shape against requested rounds and client count."""
    if not matrix:
        return False
    if required_rounds < 1:
        return True
    if len(matrix) < required_rounds:
        return False
    for row in matrix:
        if len(row) != num_clients:
            return False
    return True


def _orch_block(orch: Dict[str, Any], key: str) -> Dict[str, Any]:
    """Return a typed orchestration sub-block or raise on invalid type."""
    value = orch.get(key, {})
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError(f"orchestration.{key} must be an object")
    return value


def participation_file_from_orch(orch: Dict[str, Any]) -> str | None:
    """Return normalized participation file path value, if configured."""
    participation = _orch_block(orch, "participation")
    value = participation.get("file")
    if value is None:
        return None
    text = str(value).strip()
    return text if text else None


def selection_pct_from_orch(orch: Dict[str, Any]) -> float | None:
    """Return selection percentage or `None` when not defined."""
    participation = _orch_block(orch, "participation")
    if "selection_pct" not in participation:
        return None
    value = participation.get("selection_pct")
    if value is None:
        return None
    if isinstance(value, str) and not value.strip():
        return None
    try:
        return float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(
            "orchestration.participation.selection_pct must be a number in (0, 1] or null"
        ) from exc


def _position_std_m_from_orch(orch: Dict[str, Any]) -> float:
    """Read positioning jitter standard deviation in meters."""
    positioning = _orch_block(orch, "positioning")
    std_m = float(positioning.get("std_m", 0.0))
    if std_m < 0.0:
        raise ValueError("orchestration.positioning.std_m must be >= 0")
    return std_m


def _position_mode_from_orch(orch: Dict[str, Any]) -> str:
    """Read positioning mode (`explicit` or `radius`)."""
    positioning = _orch_block(orch, "positioning")
    mode = str(positioning.get("mode", "explicit")).strip().lower()
    if mode not in ("explicit", "radius"):
        raise ValueError("orchestration.positioning.mode must be 'explicit' or 'radius'")
    return mode


def _rng_for(seed: int, round_id: int, tag: str) -> random.Random:
    """Create a deterministic RNG stream keyed by seed, round, and tag."""
    key = f"{seed}:{round_id}:{tag}"
    h = fnv1a64(key)
    return random.Random(h)


def _apply_selection(cfg: Dict[str, Any],
                     round_id: int,
                     seed: int,
                     participation: List[List[int]] | None,
                     selection_pct: float | None) -> None:
    """Apply effective client selection for a single round."""
    net = cfg.get("network")
    if not isinstance(net, dict):
        return
    clients = net.get("clients")
    if not isinstance(clients, list) or not clients:
        return

    num_clients = len(clients)
    selected_indices: set[int] | None = None

    if participation is not None:
        row = participation[round_id - 1]
        selected_indices = {i for i, v in enumerate(row) if v == 1}
    elif selection_pct is not None:
        if selection_pct <= 0.0 or selection_pct > 1.0:
            raise ValueError("selection_pct must be in (0, 1]")
        k = max(1, int(math.ceil(num_clients * selection_pct)))
        rng = _rng_for(seed, round_id, "selection")
        selected_indices = set(rng.sample(range(num_clients), k))

    if selected_indices is None:
        return

    for i, client in enumerate(clients):
        if not isinstance(client, dict):
            continue
        client["selected"] = i in selected_indices


def _get_anchor_position(network_type: str,
                         net: Dict[str, Any],
                         client: Dict[str, Any]) -> Dict[str, float] | None:
    """Resolve anchor coordinates used by radius-mode positioning."""
    if network_type == "wifi":
        aps = net.get("access_points")
        if isinstance(aps, list):
            ap_idx = int(client.get("ap", 0))
            if 0 <= ap_idx < len(aps) and isinstance(aps[ap_idx], dict):
                pos = aps[ap_idx].get("position")
                if isinstance(pos, dict):
                    return {
                        "x": float(pos.get("x", 0.0)),
                        "y": float(pos.get("y", 0.0)),
                        "z": float(pos.get("z", 0.0)),
                    }
    if network_type == "lte":
        enbs = net.get("enbs")
        if isinstance(enbs, list):
            enb_idx = int(client.get("enb", 0))
            if 0 <= enb_idx < len(enbs) and isinstance(enbs[enb_idx], dict):
                pos = enbs[enb_idx].get("position")
                if isinstance(pos, dict):
                    return {
                        "x": float(pos.get("x", 0.0)),
                        "y": float(pos.get("y", 0.0)),
                        "z": float(pos.get("z", 0.0)),
                    }
    return None


def _discretize_random_coordinate(value: float) -> int:
    """Discretize randomized coordinates to integer meters."""
    return int(round(value))


def _apply_positions(cfg: Dict[str, Any],
                     round_id: int,
                     seed: int,
                     position_std_m: float,
                     position_mode: str) -> None:
    """Apply effective client positions for a single round."""
    net = cfg.get("network")
    if not isinstance(net, dict):
        return
    clients = net.get("clients")
    if not isinstance(clients, list) or not clients:
        return

    network_type = str(cfg.get("network_type", "")).strip().lower()
    rng = _rng_for(seed, round_id, "positions")

    for i, client in enumerate(clients):
        if not isinstance(client, dict):
            continue

        if position_mode == "explicit":
            pos = client.get("position")
            if not isinstance(pos, dict) or "x" not in pos or "y" not in pos:
                raise ValueError(
                    f"orchestration.positioning.mode=explicit requires "
                    f"network.clients[{i}].position with x and y"
                )
            # Silo uses fixed client coordinates; do not apply per-round jitter.
            if network_type != "silo" and position_std_m > 0.0:
                pos["x"] = _discretize_random_coordinate(
                    rng.gauss(float(pos.get("x", 0.0)), position_std_m)
                )
                pos["y"] = _discretize_random_coordinate(
                    rng.gauss(float(pos.get("y", 0.0)), position_std_m)
                )
            continue

        # radius mode
        anchor = _get_anchor_position(network_type, net, client)
        radius = float(client.get("radius_m", 0.0))
        if anchor is None or radius <= 0.0:
            raise ValueError(
                f"orchestration.positioning.mode=radius requires "
                f"network.clients[{i}] to have valid anchor and radius_m > 0"
            )

        angle = rng.random() * 2.0 * math.pi
        dist = math.sqrt(rng.random()) * radius
        client["position"] = {
            "x": _discretize_random_coordinate(anchor["x"] + dist * math.cos(angle)),
            "y": _discretize_random_coordinate(anchor["y"] + dist * math.sin(angle)),
            "z": anchor.get("z", 0.0),
        }


def apply_round_variation(cfg: Dict[str, Any],
                          round_id: int,
                          seed: int,
                          participation: List[List[int]] | None,
                          orch: Dict[str, Any]) -> None:
    """Materialize round-specific selection and positioning in-place."""
    selection_pct = selection_pct_from_orch(orch)
    _apply_selection(cfg, round_id, seed, participation, selection_pct)

    position_std_m = _position_std_m_from_orch(orch)
    position_mode = _position_mode_from_orch(orch)
    _apply_positions(cfg, round_id, seed, position_std_m, position_mode)
