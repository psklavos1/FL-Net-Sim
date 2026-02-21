"""Summary post-processing and export helpers."""

from __future__ import annotations

import csv
import io
import json
import logging
import re
from collections import defaultdict
from pathlib import Path
from statistics import mean
from typing import Any, Dict, List, Tuple

import pandas as pd

from .cache import canonical_summary_path, load_round_index


def load_external_results(path: Path) -> pd.DataFrame | None:
    """Load external FL metrics from a CSV file or CSV directory."""
    if path.is_file():
        return pd.read_csv(path)
    if path.is_dir():
        summary_path = path / "summary.csv"
        if summary_path.exists():
            return pd.read_csv(summary_path)

        csv_files = sorted(p for p in path.glob("*.csv") if p.is_file())
        frames: List[pd.DataFrame] = []
        for csv_path in csv_files:
            try:
                df = pd.read_csv(csv_path)
            except Exception:
                continue
            if "round" in df.columns:
                frames.append(df)
        if not frames:
            return None
        ext = pd.concat(frames, ignore_index=True)
        if "round" not in ext.columns:
            return None

        numeric_cols = set(ext.select_dtypes(include="number").columns)
        agg: Dict[str, str] = {}
        for col in ext.columns:
            if col == "round":
                continue
            if col in numeric_cols:
                agg[col] = "mean"
            else:
                agg[col] = "first"
        return ext.groupby("round", as_index=False).agg(agg)
    return None


def _coerce_round_column(df: pd.DataFrame) -> pd.DataFrame:
    """Normalize `round` to integer dtype and drop invalid rows."""
    if "round" not in df.columns:
        return df
    df = df.copy()
    df["round"] = pd.to_numeric(df["round"], errors="coerce")
    df = df[df["round"].notna()]
    df["round"] = df["round"].astype(int)
    return df


def _coerce_scalar(value: Any) -> Any:
    """Convert scalar strings to bool/int/float when possible."""
    if value is None:
        return None
    if isinstance(value, (bool, int, float)):
        return value

    text = str(value).strip()
    if text == "":
        return None

    low = text.lower()
    if low in {"true", "false"}:
        return low == "true"
    if re.fullmatch(r"[+-]?\d+", text):
        try:
            return int(text)
        except ValueError:
            pass
    if re.fullmatch(r"[+-]?(\d+(\.\d*)?|\.\d+)([eE][+-]?\d+)?", text):
        try:
            return float(text)
        except ValueError:
            pass
    return text


def _normalize_row(row: Dict[str, Any]) -> Dict[str, Any]:
    """Normalize a CSV row dictionary to typed scalar values."""
    out: Dict[str, Any] = {}
    for key, value in row.items():
        if key is None:
            continue
        out[str(key).strip()] = _coerce_scalar(value)
    return out


def _parse_csv_section(section_text: str) -> Tuple[List[str], List[Dict[str, Any]]]:
    """Parse one CSV section and return field names and normalized rows."""
    reader = csv.DictReader(io.StringIO(section_text))
    if not reader.fieldnames:
        return [], []

    fieldnames = [str(name).strip() for name in reader.fieldnames if name is not None]
    rows: List[Dict[str, Any]] = []
    for row in reader:
        if not row:
            continue
        if not any(str(v).strip() for v in row.values() if v is not None):
            continue
        rows.append(_normalize_row(row))
    return fieldnames, rows


def _exclude_log_field(field: str) -> bool:
    """Return true when a field should be excluded from exported logs."""
    normalized = field.strip().lower()
    if not normalized:
        return True
    return "queue_disc" in normalized or normalized == "server_side_queue_disc_type"


def _parse_round_report(summary_path: Path) -> Dict[str, Any]:
    """Parse canonical round summary.csv into aggregate/flow/client sections."""
    payload: Dict[str, Any] = {"aggregate": {}, "flow": {}, "clients": [], "extra": {}}
    text = summary_path.read_text(encoding="utf-8")
    sections = [section for section in text.split("\n\n") if section.strip()]

    for section in sections:
        fields, rows = _parse_csv_section(section)
        if not fields or not rows:
            continue
        field_set = set(fields)
        if "client" in field_set and "selected" in field_set:
            payload["clients"] = rows
            continue
        if "flow_monitor_enabled" in field_set:
            payload["flow"] = rows[0]
            continue
        if "description" in field_set and "round" in field_set:
            payload["aggregate"] = rows[0]
            continue
        if len(rows) == 1:
            row = rows[0]
            for key, value in row.items():
                if _exclude_log_field(str(key)):
                    continue
                payload["extra"][str(key)] = value
    return payload


def _find_compute_column(columns: List[str]) -> str | None:
    """Pick the external computation-time column when available."""
    for candidate in ("training_time_s", "compute_time_s", "computation_time_s"):
        if candidate in columns:
            return candidate
    return None


def _ingest_external_compute_frame(df: pd.DataFrame,
                                   aggregated: Dict[int, float],
                                   per_client: Dict[int, Dict[str, float]],
                                   default_client_id: str | None,
                                   compute_col_name: List[str]) -> None:
    """Ingest one external frame into aggregated and optional per-client compute maps."""
    if "round" not in df.columns:
        return
    compute_col = _find_compute_column(list(df.columns))
    if compute_col is None:
        return
    if not compute_col_name:
        compute_col_name.append(compute_col)

    frame = df.copy()
    frame["round"] = pd.to_numeric(frame["round"], errors="coerce")
    frame[compute_col] = pd.to_numeric(frame[compute_col], errors="coerce")
    frame = frame[frame["round"].notna() & frame[compute_col].notna()]
    if frame.empty:
        return

    client_col = None
    for candidate in ("client", "client_id", "trainer", "trainer_id", "silo", "participant", "node_id"):
        if candidate in frame.columns:
            client_col = candidate
            break

    if client_col is not None:
        for _, row in frame.iterrows():
            round_id = int(row["round"])
            client_id = str(row[client_col]).strip()
            if not client_id:
                continue
            per_client[round_id][client_id] = float(row[compute_col])
        return

    if default_client_id is not None:
        for _, row in frame.iterrows():
            round_id = int(row["round"])
            per_client[round_id][default_client_id] = float(row[compute_col])
        return

    grouped = frame.groupby("round", as_index=True)[compute_col].mean()
    for round_id, value in grouped.items():
        aggregated[int(round_id)] = float(value)


def _load_external_compute_maps(merge_stats: str, logger: logging.Logger) -> Tuple[Dict[int, float],
                                                                                    Dict[int, Dict[str, float]],
                                                                                    str]:
    """Load external compute stats as round-aggregated and per-client maps."""
    aggregated: Dict[int, float] = {}
    per_client: Dict[int, Dict[str, float]] = defaultdict(dict)
    compute_col_name: List[str] = []

    if not merge_stats:
        return aggregated, {}, ""

    path = Path(merge_stats).resolve()
    if not path.exists():
        logger.warning("external results path not found: %s", path)
        return aggregated, {}, ""

    if path.is_file():
        try:
            frame = pd.read_csv(path)
        except Exception:
            return aggregated, {}, ""
        _ingest_external_compute_frame(frame, aggregated, per_client, None, compute_col_name)
    elif path.is_dir():
        summary_path = path / "summary.csv"
        if summary_path.exists():
            try:
                frame = pd.read_csv(summary_path)
            except Exception:
                frame = None
            if frame is not None:
                _ingest_external_compute_frame(frame, aggregated, per_client, None, compute_col_name)
        else:
            for csv_path in sorted(p for p in path.glob("*.csv") if p.is_file()):
                try:
                    frame = pd.read_csv(csv_path)
                except Exception:
                    continue
                _ingest_external_compute_frame(
                    frame,
                    aggregated,
                    per_client,
                    csv_path.stem,
                    compute_col_name,
                )

    for round_id, client_map in per_client.items():
        if round_id not in aggregated and client_map:
            aggregated[round_id] = float(mean(client_map.values()))

    return aggregated, dict(per_client), (compute_col_name[0] if compute_col_name else "")


def _selected_client_ids(clients: List[Dict[str, Any]]) -> List[str]:
    """Collect selected client identifiers as strings."""
    out: List[str] = []
    for row in clients:
        selected = int(_coerce_scalar(row.get("selected")) or 0) == 1
        if not selected:
            continue
        cid_raw = _coerce_scalar(row.get("client"))
        if cid_raw is None:
            continue
        cid = str(cid_raw)
        out.append(cid)
    return out


def _is_selected_client(row: Dict[str, Any]) -> bool:
    """Return True when a parsed client row is selected for the round."""
    return int(_coerce_scalar(row.get("selected")) or 0) == 1


def _map_compute_to_clients(round_id: int,
                            clients: List[Dict[str, Any]],
                            aggregated_compute: Dict[int, float],
                            per_client_compute: Dict[int, Dict[str, float]]) -> Tuple[Dict[str, float], str]:
    """Map external compute values to selected clients for one round."""
    selected_ids = _selected_client_ids(clients)
    round_client_compute = per_client_compute.get(round_id, {})

    if not selected_ids:
        return {}, "none"

    if round_client_compute:
        if all(cid in round_client_compute for cid in selected_ids):
            return {cid: float(round_client_compute[cid]) for cid in selected_ids}, "per_client_exact"

        if len(round_client_compute) == len(selected_ids):
            selected_sorted = sorted(selected_ids, key=_client_sort_key)
            external_sorted = sorted(round_client_compute.items(), key=lambda item: item[0])
            mapped: Dict[str, float] = {}
            for cid, (_, compute_s) in zip(selected_sorted, external_sorted):
                mapped[cid] = float(compute_s)
            return mapped, "per_client_ordered"

    if round_id in aggregated_compute:
        value = float(aggregated_compute[round_id])
        return {cid: value for cid in selected_ids}, "aggregated_assumed_equal"

    return {}, "none"


def _client_communication_time_s(client_row: Dict[str, Any]) -> float:
    """Compute per-client communication time from DL/UL durations."""
    dl_s = _as_float(client_row.get("dl_dur_s"), default=0.0)
    ul_s = _as_float(client_row.get("ul_dur_s"), default=0.0)
    dl_s = dl_s if dl_s > 0 else 0.0
    ul_s = ul_s if ul_s > 0 else 0.0
    return dl_s + ul_s


def _average(values: List[float]) -> float | None:
    """Return arithmetic mean when values are present."""
    if not values:
        return None
    return float(mean(values))


def _as_float(value: Any, default: float = 0.0) -> float:
    """Parse a scalar into float with a default fallback."""
    parsed = _coerce_scalar(value)
    if isinstance(parsed, (int, float)):
        return float(parsed)
    return default


def _client_sort_key(client_id: str) -> Tuple[int, str]:
    """Sort numeric client ids numerically and others lexicographically."""
    if re.fullmatch(r"[+-]?\d+", client_id):
        return (0, f"{int(client_id):010d}")
    return (1, client_id)


def finalize_summary_df(df: pd.DataFrame, merge_stats: str, logger: logging.Logger) -> pd.DataFrame:
    """Merge optional external stats and derive communication/learning columns."""
    df = _coerce_round_column(df)
    merged_external = False

    if merge_stats:
        ext_path = Path(merge_stats).resolve()
        if ext_path.exists():
            ext = load_external_results(ext_path)
            if ext is not None and "round" in ext.columns and "round" in df.columns:
                ext = _coerce_round_column(ext)
                df = df.merge(ext, on="round", how="left")
                merged_external = True
            else:
                logger.warning("external results missing round column: %s", ext_path)
        else:
            logger.warning("external results path not found: %s", ext_path)

    if "max_dl_s" in df.columns:
        df["comm_dl_s"] = pd.to_numeric(df["max_dl_s"], errors="coerce")
    if "max_ul_s" in df.columns:
        df["comm_ul_s"] = pd.to_numeric(df["max_ul_s"], errors="coerce")
    if "comm_dl_s" in df.columns and "comm_ul_s" in df.columns:
        df["comm_total_s"] = df["comm_dl_s"] + df["comm_ul_s"]

    if merged_external:
        compute_source = None
        for candidate in ("training_time_s", "compute_time_s", "computation_time_s"):
            if candidate in df.columns:
                compute_source = candidate
                break
        if compute_source is not None:
            df["compute_time_s"] = pd.to_numeric(df[compute_source], errors="coerce")
            if "comm_total_s" in df.columns:
                df["total_communication_time_s"] = pd.to_numeric(df["comm_total_s"], errors="coerce")
                df["total_computation_time_s"] = df["compute_time_s"]
                df["total_round_time_s"] = (
                    df["total_communication_time_s"] + df["total_computation_time_s"]
                )

        drop_cols = [c for c in ("training_time_s",
                                 "computation_time_s",
                                 "total_learning_time_s",
                                 "max_compute_wait_s",
                                 "compute_wait_s",
                                 "max_compute_s") if c in df.columns]
        if drop_cols:
            df = df.drop(columns=drop_cols)
    else:
        drop_cols = [c for c in ("training_time_s",
                                 "compute_time_s",
                                 "computation_time_s",
                                 "total_learning_time_s",
                                 "total_communication_time_s",
                                 "total_computation_time_s",
                                 "total_round_time_s",
                                 "max_compute_wait_s",
                                 "compute_wait_s",
                                 "max_compute_s") if c in df.columns]
        if drop_cols:
            df = df.drop(columns=drop_cols)
    return df


def _collect_column_order(rows: List[Dict[str, Any]], prefix: List[str]) -> List[str]:
    """Build deterministic column order from rows, preserving first-seen keys."""
    ordered = list(prefix)
    seen = set(ordered)
    for row in rows:
        for key in row.keys():
            key_s = str(key)
            if key_s in seen:
                continue
            ordered.append(key_s)
            seen.add(key_s)
    return ordered


def export_summary_detailed_csv(records_base: Path,
                                static_key: str,
                                rounds: List[int],
                                summary_rows: List[Dict[str, Any]],
                                merge_stats: str,
                                export_path: Path,
                                label: str,
                                logger: logging.Logger) -> None:
    """Write detailed multi-round CSV with index rows then client rows."""
    if not rounds:
        return

    index_rows: List[Dict[str, Any]] = []
    client_rows: List[Dict[str, Any]] = []
    round_index = load_round_index(records_base, static_key)
    missing_rounds: List[int] = []
    merged_by_round: Dict[int, Dict[str, Any]] = {}

    if summary_rows:
        df = pd.DataFrame(summary_rows)
        df = finalize_summary_df(df, merge_stats, logger)
        if "round" in df.columns:
            for _, row in df.iterrows():
                rid = int(row["round"])
                merged_by_round[rid] = {
                    str(col): _coerce_scalar(row[col]) for col in df.columns if col != "round"
                }

    for idx, round_id in enumerate(rounds, start=1):
        entry = round_index.get(round_id)
        if entry is None:
            missing_rounds.append(round_id)
            continue
        round_key = str(entry.get("round_key", "")).strip()
        if not round_key:
            missing_rounds.append(round_id)
            continue
        summary_path = canonical_summary_path(records_base, static_key, round_key)
        if not summary_path.exists():
            missing_rounds.append(round_id)
            continue

        parsed = _parse_round_report(summary_path)
        row: Dict[str, Any] = {
            "index": idx,
            "round": round_id,
            "round_key": round_key,
            "latest_run_at": str(entry.get("latest_run_at", "")),
        }
        for section in (parsed.get("aggregate", {}), parsed.get("extra", {}), parsed.get("flow", {})):
            if not isinstance(section, dict):
                continue
            for key, value in section.items():
                key_s = str(key)
                if _exclude_log_field(key_s):
                    continue
                row[key_s] = value
        merged_fields = merged_by_round.get(round_id, {})
        for key, value in merged_fields.items():
            key_s = str(key)
            if _exclude_log_field(key_s):
                continue
            row[key_s] = value
        index_rows.append(row)

        clients = parsed.get("clients", [])
        if not isinstance(clients, list):
            clients = []
        for client in clients:
            if not isinstance(client, dict):
                continue
            if not _is_selected_client(client):
                continue
            client_row: Dict[str, Any] = {
                "index": idx,
                "round": round_id,
                "round_key": round_key,
                "latest_run_at": str(entry.get("latest_run_at", "")),
            }
            for key, value in client.items():
                key_s = str(key)
                if _exclude_log_field(key_s):
                    continue
                client_row[key_s] = value
            client_rows.append(client_row)

    export_path.parent.mkdir(parents=True, exist_ok=True)
    with export_path.open("w", encoding="utf-8", newline="") as f:
        if index_rows:
            index_columns = _collect_column_order(
                index_rows,
                ["index", "round", "round_key", "latest_run_at"],
            )
            writer = csv.DictWriter(f, fieldnames=index_columns)
            writer.writeheader()
            writer.writerows(index_rows)
        if client_rows:
            if index_rows:
                f.write("\n")
            client_columns = _collect_column_order(
                client_rows,
                ["index", "round", "round_key", "latest_run_at", "client"],
            )
            writer = csv.DictWriter(f, fieldnames=client_columns)
            writer.writeheader()
            writer.writerows(client_rows)

    rounds_text = ",".join(str(r) for r in rounds)
    if missing_rounds:
        logger.warning(
            "%s exported with missing rounds: %s | path=%s",
            label,
            ",".join(str(r) for r in missing_rounds),
            export_path,
        )
    else:
        logger.info("%s exported: %s | rounds=%s", label, export_path, rounds_text)


def _round_payload(round_id: int,
                   round_key: str,
                   latest_run_at: str,
                   aggregate_row: Dict[str, Any],
                   parsed_report: Dict[str, Any],
                   aggregated_compute: Dict[int, float],
                   per_client_compute: Dict[int, Dict[str, float]]) -> Dict[str, Any]:
    """Build one round JSON payload with aggregate and per-client details."""
    clients_raw = parsed_report.get("clients", [])
    clients: List[Dict[str, Any]] = []
    for row in clients_raw:
        normalized = _normalize_row(row)
        selected = int(_coerce_scalar(normalized.get("selected")) or 0) == 1
        normalized["selected"] = 1 if selected else 0
        normalized["communication_time_s"] = _client_communication_time_s(normalized) if selected else 0.0
        clients.append(normalized)

    compute_by_client, compute_mode = _map_compute_to_clients(
        round_id,
        clients,
        aggregated_compute,
        per_client_compute,
    )

    selected_clients: List[Dict[str, Any]] = []
    for client in clients:
        if client.get("selected", 0) != 1:
            continue
        cid = str(client.get("client"))
        compute_s = compute_by_client.get(cid)
        if compute_s is not None:
            client["compute_time_s"] = float(compute_s)
            client["total_client_time_s"] = float(client["communication_time_s"]) + float(compute_s)
        selected_clients.append(client)

    comm_values = [float(c.get("communication_time_s", 0.0)) for c in selected_clients]
    totals: Dict[str, Any] = {}
    if comm_values:
        totals["total_communication_time_s"] = max(comm_values)

    compute_values = [
        float(c["compute_time_s"]) for c in selected_clients if c.get("compute_time_s") is not None
    ]
    total_values = [
        float(c["total_client_time_s"]) for c in selected_clients if c.get("total_client_time_s") is not None
    ]
    if compute_values and len(compute_values) == len(selected_clients):
        totals["total_computation_time_s"] = max(compute_values)
    if total_values and len(total_values) == len(selected_clients):
        totals["total_round_time_s"] = max(total_values)

    dl_values = [_as_float(c.get("dl_dur_s"), default=-1.0) for c in selected_clients]
    dl_values = [value for value in dl_values if value >= 0.0]
    ul_values = [_as_float(c.get("ul_dur_s"), default=-1.0) for c in selected_clients]
    ul_values = [value for value in ul_values if value >= 0.0]
    dl_tp_values = [_as_float(c.get("dl_throughput_mbps"), default=-1.0) for c in selected_clients]
    dl_tp_values = [value for value in dl_tp_values if value >= 0.0]
    ul_tp_values = [_as_float(c.get("ul_throughput_mbps"), default=-1.0) for c in selected_clients]
    ul_tp_values = [value for value in ul_tp_values if value >= 0.0]

    round_averages: Dict[str, Any] = {
        "selected_clients_count": len(selected_clients),
        "valid_selected_dl_samples": len(dl_values),
        "valid_selected_ul_samples": len(ul_values),
        "valid_selected_dl_throughput_samples": len(dl_tp_values),
        "valid_selected_ul_throughput_samples": len(ul_tp_values),
        "avg_selected_client_communication_time_s": _average(comm_values),
        "avg_selected_dl_time_s": _average(dl_values),
        "avg_selected_ul_time_s": _average(ul_values),
        "avg_selected_dl_throughput_mbps": _average(dl_tp_values),
        "avg_selected_ul_throughput_mbps": _average(ul_tp_values),
    }
    if compute_values and len(compute_values) == len(selected_clients):
        round_averages["avg_selected_computation_time_s"] = _average(compute_values)
    if total_values and len(total_values) == len(selected_clients):
        round_averages["avg_selected_total_client_time_s"] = _average(total_values)

    return {
        "round": round_id,
        "round_key": round_key,
        "latest_run_at": latest_run_at,
        "aggregate": aggregate_row,
        "flow_monitor": parsed_report.get("flow", {}),
        "totals": totals,
        "round_averages": round_averages,
        "clients": selected_clients,
        "compute_mapping": {
            "mode": compute_mode,
            "source": "external" if compute_mode != "none" else "none",
        },
    }


def export_summary_json(records_base: Path,
                        static_key: str,
                        rounds: List[int],
                        summary_rows: List[Dict[str, Any]],
                        merge_stats: str,
                        export_path: Path,
                        label: str,
                        logger: logging.Logger) -> None:
    """Write a structured JSON summary with per-round and per-client statistics."""
    if not rounds:
        return

    df = pd.DataFrame(summary_rows)
    df = finalize_summary_df(df, merge_stats, logger)
    aggregate_by_round: Dict[int, Dict[str, Any]] = {}
    if "round" in df.columns:
        for _, row in df.iterrows():
            rid = int(row["round"])
            aggregate_by_round[rid] = {
                str(col): _coerce_scalar(row[col])
                for col in df.columns
                if col != "round"
            }

    round_index = load_round_index(records_base, static_key)
    aggregated_compute, per_client_compute, compute_col = _load_external_compute_maps(merge_stats, logger)
    round_payloads: List[Dict[str, Any]] = []
    missing_rounds: List[int] = []

    for round_id in rounds:
        entry = round_index.get(round_id)
        if entry is None:
            missing_rounds.append(round_id)
            continue
        round_key = str(entry.get("round_key", "")).strip()
        if not round_key:
            missing_rounds.append(round_id)
            continue
        summary_path = canonical_summary_path(records_base, static_key, round_key)
        if not summary_path.exists():
            missing_rounds.append(round_id)
            continue

        parsed = _parse_round_report(summary_path)
        aggregate = aggregate_by_round.get(round_id, parsed.get("aggregate", {}))
        round_payloads.append(
            _round_payload(
                round_id=round_id,
                round_key=round_key,
                latest_run_at=str(entry.get("latest_run_at", "")),
                aggregate_row=aggregate,
                parsed_report=parsed,
                aggregated_compute=aggregated_compute,
                per_client_compute=per_client_compute,
            )
        )

    overall: Dict[str, Any] = {
        "num_rounds": len(round_payloads),
        "round_ids": [payload["round"] for payload in round_payloads],
    }
    comm_round_values = [
        float(payload["totals"]["total_communication_time_s"])
        for payload in round_payloads
        if "total_communication_time_s" in payload.get("totals", {})
    ]
    if comm_round_values:
        overall["avg_total_communication_time_s"] = _average(comm_round_values)
    comp_round_values = [
        float(payload["totals"]["total_computation_time_s"])
        for payload in round_payloads
        if "total_computation_time_s" in payload.get("totals", {})
    ]
    if comp_round_values:
        overall["avg_total_computation_time_s"] = _average(comp_round_values)
    round_total_values = [
        float(payload["totals"]["total_round_time_s"])
        for payload in round_payloads
        if "total_round_time_s" in payload.get("totals", {})
    ]
    if round_total_values:
        overall["avg_total_round_time_s"] = _average(round_total_values)

    payload = {
        "schema_version": 1,
        "static_key": static_key,
        "requested_rounds": list(rounds),
        "external_compute": {
            "enabled": bool(merge_stats),
            "compute_column": compute_col if compute_col else None,
        },
        "missing_rounds": missing_rounds,
        "overall": overall,
        "rounds": round_payloads,
    }

    export_path.parent.mkdir(parents=True, exist_ok=True)
    with export_path.open("w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)
        f.write("\n")

    rounds_text = ",".join(str(r) for r in rounds) if rounds else "-"
    logger.info("%s exported: %s | rounds=%s", label, export_path, rounds_text)


def append_round_analytics_log(log_path: Path,
                               summary_path: Path,
                               round_id: int,
                               round_key: str,
                               latest_run_at: str) -> None:
    """Append a compact, human-readable analytics block to one round log file."""
    parsed = _parse_round_report(summary_path)
    aggregate = parsed.get("aggregate", {})
    flow = parsed.get("flow", {})
    extra = parsed.get("extra", {})
    clients = parsed.get("clients", [])

    lines: List[str] = []
    lines.append("")
    lines.append("===== ROUND ANALYTICS =====")
    lines.append(
        f"round={round_id} round_key={round_key} latest_run_at={latest_run_at or '-'}"
    )

    if isinstance(aggregate, dict) and aggregate:
        agg_pairs = ", ".join(f"{key}={value}" for key, value in aggregate.items())
        lines.append(f"aggregate: {agg_pairs}")

    if isinstance(extra, dict) and extra:
        extra_pairs = ", ".join(f"{key}={value}" for key, value in extra.items())
        lines.append(f"network: {extra_pairs}")

    if isinstance(flow, dict) and flow:
        flow_pairs = ", ".join(f"{key}={value}" for key, value in flow.items())
        lines.append(f"flow_monitor: {flow_pairs}")

    if isinstance(clients, list) and clients:
        selected_clients: List[Dict[str, Any]] = [
            row for row in clients if isinstance(row, dict) and _is_selected_client(row)
        ]
    else:
        selected_clients = []

    if selected_clients:
        columns = [
            "client",
            "tier",
            "selected",
            "dl_dur_s",
            "ul_dur_s",
            "dl_throughput_mbps",
            "ul_throughput_mbps",
            "dl_completion_ratio",
            "ul_completion_ratio",
        ]
        lines.append("selected_clients:")
        lines.append(",".join(columns))
        for client_row in selected_clients:
            values = [str(client_row.get(column, "")) for column in columns]
            lines.append(",".join(values))

    with log_path.open("a", encoding="utf-8") as f:
        f.write("\n".join(lines))
        f.write("\n")
