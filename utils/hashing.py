"""Deterministic hashing helpers for static and effective round cache keys."""

from __future__ import annotations

import struct
from copy import deepcopy
from typing import Any, Dict, List


def fnv1a64(s: str) -> int:
    """Compute 64-bit FNV-1a hash for a UTF-8 string."""
    h = 14695981039346656037
    for c in s.encode("utf-8"):
        h ^= c
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def _fnv1a64_bytes(data: bytes, h: int = 14695981039346656037) -> int:
    """Update a running 64-bit FNV-1a hash with raw bytes."""
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def _hash_json_value(value: Any, h: int) -> int:
    """Hash JSON-like data with explicit type tags and stable dict ordering."""
    if value is None:
        return _fnv1a64_bytes(b"N", h)
    if isinstance(value, bool):
        return _fnv1a64_bytes(b"T" if value else b"F", h)
    if isinstance(value, int) and not isinstance(value, bool):
        h = _fnv1a64_bytes(b"I", h)
        if -(1 << 63) <= value < (1 << 63):
            return _fnv1a64_bytes(struct.pack("<q", value), h)
        return _fnv1a64_bytes(str(value).encode("utf-8"), h)
    if isinstance(value, float):
        h = _fnv1a64_bytes(b"D", h)
        return _fnv1a64_bytes(struct.pack("<d", value), h)
    if isinstance(value, str):
        h = _fnv1a64_bytes(b"S", h)
        return _fnv1a64_bytes(value.encode("utf-8"), h)
    if isinstance(value, list):
        h = _fnv1a64_bytes(b"[", h)
        for item in value:
            h = _hash_json_value(item, h)
        return _fnv1a64_bytes(b"]", h)
    if isinstance(value, dict):
        h = _fnv1a64_bytes(b"{", h)
        for key in sorted(value.keys()):
            h = _fnv1a64_bytes(b"K", h)
            h = _fnv1a64_bytes(str(key).encode("utf-8"), h)
            h = _hash_json_value(value[key], h)
        return _fnv1a64_bytes(b"}", h)
    return _fnv1a64_bytes(str(value).encode("utf-8"), h)


def to_hex_16(value: int) -> str:
    """Render a 64-bit integer hash as fixed 16-char lowercase hex."""
    return f"{value:016x}"


def _normalize_static_cfg(exp_cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Drop non-semantic fields before building the static cache key."""
    cfg = deepcopy(exp_cfg)

    cfg.pop("orchestration", None)
    cfg.pop("description", None)
    cfg.pop("scenario", None)
    cfg.pop("sim", None)
    cfg.pop("metrics", None)

    repro = cfg.get("reproducibility")
    if isinstance(repro, dict):
        repro.pop("round", None)

    fl_traffic = cfg.get("fl_traffic")
    if isinstance(fl_traffic, dict):
        fl_traffic.pop("sync_start_jitter_ms", None)
        fl_traffic.pop("compute_s", None)

    net = cfg.get("network")
    if not isinstance(net, dict):
        return cfg

    aps = net.get("access_points")
    if isinstance(aps, list):
        for ap in aps:
            if isinstance(ap, dict):
                ap.pop("ssid", None)

    clients = net.get("clients")
    if isinstance(clients, list):
        for client in clients:
            if isinstance(client, dict):
                client.pop("selected", None)

    network_type = str(cfg.get("network_type", "")).strip().lower()
    if network_type == "wifi":
        server_side = net.get("server_side")
        if isinstance(server_side, dict):
            for node_name in ("server_ap", "server"):
                node = server_side.get(node_name)
                if isinstance(node, dict):
                    node.pop("position", None)
    elif network_type == "lte":
        server_side = net.get("server_side")
        if isinstance(server_side, dict):
            for node_name in ("server_ap", "server"):
                node = server_side.get(node_name)
                if isinstance(node, dict):
                    node.pop("position", None)
    elif network_type == "silo":
        if isinstance(clients, list):
            for client in clients:
                if isinstance(client, dict):
                    client.pop("position", None)
        server_side = net.get("server_side")
        if isinstance(server_side, dict):
            server_side.pop("access_link", None)
            for node_name in ("server_ap", "server"):
                node = server_side.get(node_name)
                if isinstance(node, dict):
                    node.pop("position", None)

    return cfg


def _normalize_ns3_stage_cfg(exp_cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Build a legacy stage hash view for locating ns-3 output folders."""
    cfg = deepcopy(exp_cfg)
    cfg.pop("orchestration", None)
    cfg.pop("description", None)
    cfg.pop("scenario", None)

    repro = cfg.get("reproducibility")
    if isinstance(repro, dict):
        repro.pop("seed", None)
        repro.pop("round", None)
        if not repro:
            cfg.pop("reproducibility", None)

    network_type = str(cfg.get("network_type", "")).strip().lower()
    if network_type == "silo":
        net = cfg.get("network")
        if isinstance(net, dict):
            server_side = net.get("server_side")
            if isinstance(server_side, dict):
                server_side.pop("access_link", None)
            clients = net.get("clients")
            if isinstance(clients, list):
                for client in clients:
                    if isinstance(client, dict):
                        client.pop("position", None)
            server_side = net.get("server_side")
            if isinstance(server_side, dict):
                for node_name in ("server_ap", "server"):
                    node = server_side.get(node_name)
                    if isinstance(node, dict):
                        node.pop("position", None)
                        if not node:
                            server_side.pop(node_name, None)
    elif network_type == "lte":
        net = cfg.get("network")
        if isinstance(net, dict):
            server_side = net.get("server_side")
            if isinstance(server_side, dict):
                for node_name in ("server_ap", "server"):
                    node = server_side.get(node_name)
                    if isinstance(node, dict):
                        node.pop("position", None)
                        if not node:
                            server_side.pop(node_name, None)
    return cfg


def compute_static_key(exp_cfg: Dict[str, Any]) -> str:
    """Compute the static cache key from normalized experiment config."""
    hash_cfg = _normalize_static_cfg(exp_cfg)
    h = _hash_json_value(hash_cfg, 14695981039346656037)
    return to_hex_16(h)


def compute_ns3_stage_hash(exp_cfg: Dict[str, Any]) -> str:
    """Compute legacy stage hash used by transient ns-3 output paths."""
    hash_cfg = _normalize_ns3_stage_cfg(exp_cfg)
    h = _hash_json_value(hash_cfg, 14695981039346656037)
    return to_hex_16(h)


def effective_round_state(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Extract selected mask and realized client positions for round keying."""
    net = cfg.get("network")
    clients = net.get("clients") if isinstance(net, dict) else None
    if not isinstance(clients, list):
        return {"selected_mask": [], "client_positions": []}

    selected_mask: List[int] = []
    client_positions: List[Dict[str, Any]] = []

    for i, client in enumerate(clients):
        if not isinstance(client, dict):
            raise ValueError(f"network.clients[{i}] must be an object")

        selected_mask.append(1 if bool(client.get("selected", True)) else 0)

        pos = client.get("position")
        if not isinstance(pos, dict) or "x" not in pos or "y" not in pos:
            raise ValueError(f"network.clients[{i}].position must include x and y")
        client_positions.append(
            {
                "x": pos.get("x"),
                "y": pos.get("y"),
                "z": pos.get("z", 0.0),
            }
        )

    return {
        "selected_mask": selected_mask,
        "client_positions": client_positions,
    }


def compute_round_key(static_key: str, cfg: Dict[str, Any]) -> str:
    """Compute round key from static key + effective round state."""
    payload = {
        "static_key": static_key,
        "effective_state": effective_round_state(cfg),
    }
    h = _hash_json_value(payload, 14695981039346656037)
    return to_hex_16(h)


def compute_exp_hash(exp_cfg: Dict[str, Any]) -> str:
    """Compatibility alias for `compute_static_key`."""
    # Kept for compatibility with existing callsites; now maps to static key policy.
    return compute_static_key(exp_cfg)
