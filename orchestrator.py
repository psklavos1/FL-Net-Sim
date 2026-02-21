#!/usr/bin/env python3
from __future__ import annotations

import argparse
import logging
import os
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, List

from utils.cache import (canonical_summary_path, collect_round_summaries, ensure_static_snapshot,
                         export_round_states_catalog, ingest_round_to_cache, load_round_index,
                         round_cache_hit, static_cache_dir, upsert_round_index,
                         validate_static_records, cleanup_orchestrator_stage_outputs)
from utils.config import (clamp_parallelism, load_config, minimal_validate,
                          parse_rounds_spec, resolve_path, set_round,
                          strip_orchestration, write_config)
from utils.hashing import (compute_round_key, compute_static_key,
                           effective_round_state)
from utils.orchestration import (apply_round_variation, load_participation_matrix,
                                 participation_file_from_orch,
                                 selection_pct_from_orch,
                                 validate_participation_matrix)
from utils.reporting import (append_round_analytics_log, export_summary_detailed_csv,
                             export_summary_json)
from utils.records import (cleanup_logs, cleanup_records, list_records_compact,
                           refresh_legacy_records_index)


SUPPORTED_NETWORKS = {
    "lte": "scratch/flsim/lte.cc",
    "wifi": "scratch/flsim/wifi.cc",
    "silo": "scratch/flsim/silo.cc",
}

LOGGER = logging.getLogger("fl-net-sim")


def configure_logging(log_path: Path | None = None) -> None:
    """Configure console logging and optionally mirror logs to a file."""
    LOGGER.setLevel(logging.INFO)
    LOGGER.handlers.clear()

    formatter = logging.Formatter(
        fmt="%(asctime)s | %(levelname)s | %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    stream = logging.StreamHandler()
    stream.setFormatter(formatter)
    LOGGER.addHandler(stream)

    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        file_handler = logging.FileHandler(log_path, encoding="utf-8")
        file_handler.setFormatter(formatter)
        LOGGER.addHandler(file_handler)


def parse_args() -> argparse.Namespace:
    """Parse orchestrator CLI arguments."""
    p = argparse.ArgumentParser(description="FLSim orchestration runner")
    p.add_argument("--config", help="Path to scenario config JSON")
    p.add_argument("--rounds", default=None, help="Rounds spec: N or A..B (inclusive)")
    p.add_argument("--parallelism",
                   type=int,
                   default=None,
                   help="Max concurrent runs (clamped to CPU count)")
    p.add_argument("--no-mtp", action="store_true", help="Disable MTP in ns3 entrypoints (wifi/silo)")
    p.add_argument("--mtp-threads",
                   type=int,
                   default=None,
                   help="Enable MTP with a fixed thread count in ns3 entrypoints (wifi/silo)")
    p.add_argument("--force", action="store_true", help="Re-run even if cached")
    p.add_argument("--merge-stats",
                   default="",
                   help="External CSV (or directory of CSVs) to merge on round")
    p.add_argument("--dry-run", action="store_true", help="Log planned rounds only")
    p.add_argument("--cleanup", action="store_true", help="Delete all logs and records")
    p.add_argument("--cleanup-logs", action="store_true", help="Delete all logs")
    p.add_argument("--cleanup-records", action="store_true", help="Delete all records")
    p.add_argument("--list-records", action="store_true", help="List cached records in compact form")
    p.add_argument("--build-ns3", action="store_true", help="Build ns3 before running (To be used when ns3 code changed and ./ns3 build was not run)")


    return p.parse_args()


def build_ns3_command(ns3_bin: Path,
                      program: str,
                      temp_config: Path,
                      extra_args: List[str] | None = None,
                      build: bool = False) -> List[str]:
    """Build the `ns3 run ...` command for one round configuration."""
    base = f"{program} --config={temp_config}"
    if extra_args:
        base = f"{base} " + " ".join(extra_args)
    if build:
        return [str(ns3_bin), "run", base]
    return [str(ns3_bin), "run", "--no-build", base]


def allocate_run_dir(logs_root: Path, suffix: str) -> Path:
    """Allocate a unique run directory under `logs/` using a timestamp prefix."""
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    base = f"{timestamp}_{suffix}"
    candidate = logs_root / base
    if not candidate.exists():
        return candidate
    idx = 1
    while True:
        candidate = logs_root / f"{base}_{idx}"
        if not candidate.exists():
            return candidate
        idx += 1


def _parse_optional_positive_int(value: Any, field_name: str) -> int | None:
    """Parse optional positive integer from JSON-like input."""
    if value is None or value == "":
        return None
    if isinstance(value, bool):
        raise ValueError(f"{field_name} must be an integer >= 1 or null")
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{field_name} must be an integer >= 1 or null") from exc
    if parsed < 1:
        raise ValueError(f"{field_name} must be >= 1 when provided")
    return parsed


def _resolve_unison_settings(args: argparse.Namespace, orch: Dict[str, Any]) -> tuple[bool, int | None]:
    """Resolve UNISON runtime settings from CLI flags and orchestration JSON."""
    if args.no_mtp and args.mtp_threads is not None:
        raise ValueError("--no-mtp and --mtp-threads cannot be used together")

    unison = orch.get("unison", {})
    if unison is None:
        unison = {}
    if not isinstance(unison, dict):
        raise ValueError("orchestration.unison must be an object")

    enabled_cfg = unison.get("enabled", None)
    if enabled_cfg is None:
        enabled = True
    elif isinstance(enabled_cfg, bool):
        enabled = enabled_cfg
    else:
        raise ValueError("orchestration.unison.enabled must be true, false, or null")

    threads = _parse_optional_positive_int(unison.get("threads"), "orchestration.unison.threads")
    if threads is not None and not enabled:
        LOGGER.warning("orchestration.unison.threads ignored because orchestration.unison.enabled=false")
        threads = None

    if args.no_mtp:
        return False, None
    if args.mtp_threads is not None:
        if args.mtp_threads < 1:
            raise ValueError("--mtp-threads must be >= 1")
        return True, int(args.mtp_threads)
    return enabled, threads


def main() -> int:
    """Run orchestration for requested rounds and return a process exit code."""
    configure_logging()
    args = parse_args()
    root = Path(__file__).resolve().parent
    ns3_root = root / "ns3"
    records_base = ns3_root / "flsim_records"
    logs_root = root / "logs"

    did_cleanup = False
    if args.cleanup or args.cleanup_logs:
        removed = cleanup_logs(root / "logs")
        LOGGER.info("cleanup logs: removed %d entries", removed)
        did_cleanup = True

    if args.cleanup or args.cleanup_records:
        removed = cleanup_records(records_base)
        LOGGER.info("cleanup records: removed %d entries", removed)
        did_cleanup = True

    if args.list_records and not args.config:
        run_dir = allocate_run_dir(logs_root, "records")
        configure_logging(run_dir / "orchestrator.log")
        for line in list_records_compact(records_base):
            LOGGER.info("%s", line)
        return 0

    if did_cleanup and not args.config and not args.list_records:
        return 0

    if not args.config:
        LOGGER.error("Missing --config. Provide a config or use --list-records alone.")
        return 2

    ns3_bin = ns3_root / "ns3"
    if not ns3_bin.exists():
        LOGGER.error("ns3 executable not found: %s", ns3_bin)
        return 2

    cfg_path = Path(args.config).resolve()
    cfg = load_config(cfg_path)
    minimal_validate(cfg, tuple(SUPPORTED_NETWORKS.keys()))

    orch = cfg.get("orchestration", {}) if isinstance(cfg.get("orchestration"), dict) else {}

    rounds_spec = args.rounds if args.rounds is not None else orch.get("rounds", None)
    all_rounds = parse_rounds_spec(rounds_spec)
    if not all_rounds:
        all_rounds = [1]

    cpu_max = os.cpu_count() or 1
    par_value = args.parallelism if args.parallelism is not None else int(orch.get("parallelism", cpu_max))
    parallelism = clamp_parallelism(par_value, cpu_max)

    force = args.force or bool(orch.get("force", False))

    network_type = cfg.get("network_type")
    program = SUPPORTED_NETWORKS[network_type]
    unison_enabled, unison_threads = _resolve_unison_settings(args, orch)

    extra_args: List[str] = []
    if network_type in ("wifi", "silo"):
        if not unison_enabled:
            extra_args.append("--no-mtp")
        elif unison_threads is not None:
            extra_args.append(f"--mtp-threads={unison_threads}")
        thread_label = unison_threads if unison_threads is not None else "auto"
        LOGGER.info("unison runtime: enabled=%s threads=%s", unison_enabled, thread_label)
    else:
        if args.no_mtp:
            LOGGER.warning("--no-mtp requested but ignored for network_type=%s", network_type)
        if args.mtp_threads is not None:
            LOGGER.warning("--mtp-threads requested but ignored for network_type=%s", network_type)
        if isinstance(orch.get("unison"), dict):
            LOGGER.warning("orchestration.unison settings ignored for network_type=%s", network_type)

    cleaned = strip_orchestration(cfg)
    static_key = compute_static_key(cleaned)
    seed = int(cleaned.get("reproducibility", {}).get("seed", 1))

    suffix = "dry_run" if args.dry_run else str(static_key)
    run_dir = allocate_run_dir(logs_root, suffix)
    configure_logging(run_dir / "orchestrator.log")

    if args.list_records:
        for line in list_records_compact(records_base):
            LOGGER.info("%s", line)

    participation: List[List[int]] | None = None
    participation_file = participation_file_from_orch(orch)
    if participation_file:
        path = resolve_path(str(participation_file), cfg_path.parent)
        if not path.exists():
            raise FileNotFoundError(f"participation file not found: {path}")
        loaded = load_participation_matrix(path)
        net = cleaned.get("network")
        clients = net.get("clients") if isinstance(net, dict) else None
        num_clients = len(clients) if isinstance(clients, list) else 0
        required_rounds = max(all_rounds) if all_rounds else 0
        if validate_participation_matrix(loaded, required_rounds, num_clients):
            participation = loaded
        else:
            if selection_pct_from_orch(orch) is None:
                raise ValueError(
                    "participation file shape is invalid for requested rounds/clients "
                    "and selection_pct is not configured"
                )
            LOGGER.warning(
                "participation file shape invalid for rounds/clients; falling back to selection_pct"
            )

    round_cfg_by_round: Dict[int, Dict[str, Any]] = {}
    round_key_by_round: Dict[int, str] = {}
    stage_hash_by_round: Dict[int, str] = {}
    effective_state_by_round: Dict[int, Dict[str, Any]] = {}
    to_run: List[int] = []
    existing_cached = 0
    deduped_rounds = 0
    cached_round_ids: List[int] = []
    deduped_round_ids: List[int] = []

    seen_missing_round_keys: Dict[str, int] = {}
    stable_stage_hash = static_key
    hash_json_override = {"_force_hash_dir": stable_stage_hash}
    for r in all_rounds:
        round_cfg = set_round(cleaned, r)
        apply_round_variation(round_cfg, r, seed, participation, orch)
        round_cfg["hash_json"] = hash_json_override

        round_cfg_by_round[r] = round_cfg
        round_key = compute_round_key(static_key, round_cfg)
        round_key_by_round[r] = round_key
        stage_hash_by_round[r] = stable_stage_hash
        effective_state_by_round[r] = {
            "static_key": static_key,
            "round_key": round_key,
            "round_id": r,
            "seed": seed,
            "network_type": network_type,
            "effective_state": effective_round_state(round_cfg),
        }

        cached = False
        if not force:
            cached = round_cache_hit(records_base, static_key, round_key)
            if cached:
                existing_cached += 1
                cached_round_ids.append(r)
        if not cached and not force and round_key in seen_missing_round_keys:
            cached = True
            deduped_rounds += 1
            deduped_round_ids.append(r)

        if not cached:
            to_run.append(r)
            if not force:
                seen_missing_round_keys[round_key] = r

    LOGGER.info(
        "network_type=%s rounds=%d parallelism=%d cached=%d deduped=%d",
        network_type,
        len(all_rounds),
        parallelism,
        existing_cached,
        deduped_rounds,
    )
    if cached_round_ids:
        LOGGER.info("cache hit rounds: %s", ",".join(str(r) for r in cached_round_ids))
    if deduped_round_ids:
        LOGGER.info(
            "deduped rounds: %s (same effective state within request)",
            ",".join(str(r) for r in deduped_round_ids),
        )

    if args.dry_run:
        planned_ids = ",".join(str(r) for r in to_run) if to_run else "-"
        LOGGER.info(
            "planned %d rounds: %s | requested=%d cached=%d deduped=%d",
            len(to_run),
            planned_ids,
            len(all_rounds),
            existing_cached,
            deduped_rounds,
        )
        return 0

    ensure_static_snapshot(records_base, static_key, cleaned)

    log_dir = run_dir
    log_dir.mkdir(parents=True, exist_ok=True)
    round_logs_dir = log_dir / "rounds"
    round_logs_dir.mkdir(parents=True, exist_ok=True)

    def run_round(round_id: int, build: bool = False) -> int:
        temp_cfg = round_cfg_by_round[round_id]
        temp_path = run_dir / f"tmp_{network_type}_round_{round_id}.json"
        write_config(temp_path, temp_cfg)
        cmd = build_ns3_command(ns3_bin, program, temp_path, extra_args, build=build)
        LOGGER.info("round %d start", round_id)
        proc = subprocess.run(cmd, cwd=ns3_root, capture_output=True, text=True)
        log_path = round_logs_dir / f"{network_type}_round_{round_id}.log"
        log_path.write_text(proc.stdout + "\n" + proc.stderr, encoding="utf-8")

        if proc.returncode == 0:
            try:
                temp_path.unlink()
            except OSError:
                pass
            LOGGER.info("round %d complete", round_id)
        else:
            LOGGER.error("round %d failed (exit=%d). log=%s", round_id, proc.returncode, log_path)
        return proc.returncode

    if to_run:
        from concurrent.futures import ThreadPoolExecutor, as_completed
        failures = []
        with ThreadPoolExecutor(max_workers=parallelism) as ex:
            future_map = {ex.submit(run_round, r, args.build_ns3): r for r in to_run}
            for fut in as_completed(future_map):
                r = future_map[fut]
                code = fut.result()
                if code != 0:
                    failures.append(r)
                    continue

                try:
                    ingest_round_to_cache(records_base,
                                          static_key,
                                          round_key_by_round[r],
                                          stage_hash_by_round[r],
                                          seed,
                                          r,
                                          effective_state_by_round[r])
                except Exception as exc:
                    LOGGER.error("round %d cache ingest failed: %s", r, exc)
                    failures.append(r)

        if failures:
            LOGGER.error("failed rounds: %s", ",".join(str(r) for r in sorted(set(failures))))
            return 3
    else:
        LOGGER.info("all rounds cached; nothing to run")

    # Orchestrator ingests canonical cache and does not keep temporary seed_* stage outputs.
    removed_stage_dirs = cleanup_orchestrator_stage_outputs(records_base, static_key)
    if removed_stage_dirs > 0:
        LOGGER.info(
            "cleaned temporary stage outputs: static_key=%s removed_seed_dirs=%d",
            static_key,
            removed_stage_dirs,
        )
        refresh_legacy_records_index(records_base)

    latest_run_at = datetime.now().isoformat(timespec="seconds")
    for r in all_rounds:
        upsert_round_index(records_base, static_key, r, round_key_by_round[r], latest_run_at)
    LOGGER.info(
        "round index updated: rounds=%d latest_run_at=%s",
        len(all_rounds),
        latest_run_at,
    )

    index_after = load_round_index(records_base, static_key)
    unique_states = len({v.get("round_key", "") for v in index_after.values() if v.get("round_key", "")})
    issues = validate_static_records(records_base, static_key)
    if issues:
        LOGGER.error("record validation failed for static_key=%s (%d issues)", static_key, len(issues))
        for issue in issues[:20]:
            LOGGER.error("record issue: %s", issue)
        if len(issues) > 20:
            LOGGER.error("record issue: ... and %d more", len(issues) - 20)
        return 4
    LOGGER.info(
        "records validated: static_key=%s mapped_rounds=%d unique_states=%d",
        static_key,
        len(index_after),
        unique_states,
    )
    states_catalog_path = export_round_states_catalog(records_base, static_key)
    LOGGER.info("round state catalog exported: %s", states_catalog_path)

    rows, missing = collect_round_summaries(records_base, static_key, all_rounds)
    if missing:
        LOGGER.error("missing requested summaries for rounds: %s", ",".join(str(r) for r in missing))
        return 4
    cumulative_rounds = sorted(index_after.keys())
    cum_rows, cum_missing = collect_round_summaries(records_base, static_key, cumulative_rounds)
    if cum_missing:
        LOGGER.error("missing cumulative summaries for rounds: %s", ",".join(str(r) for r in cum_missing))
        return 4
    for r in all_rounds:
        entry = index_after.get(r)
        if entry is None:
            continue
        round_key = str(entry.get("round_key", "")).strip()
        if not round_key:
            continue
        summary_path = canonical_summary_path(records_base, static_key, round_key)
        if not summary_path.exists():
            continue
        round_log_path = round_logs_dir / f"{network_type}_round_{r}.log"
        if not round_log_path.exists():
            round_log_path.write_text(
                f"round {r} served from cache (ns3 not executed in this run)\n",
                encoding="utf-8",
            )
        try:
            append_round_analytics_log(
                round_log_path,
                summary_path,
                round_id=r,
                round_key=round_key,
                latest_run_at=str(entry.get("latest_run_at", "")),
            )
        except Exception as exc:
            LOGGER.warning("round %d analytics append skipped: %s", r, exc)

    LOGGER.info("")
    LOGGER.info("===== CAHCE RECORDS =====")
    static_dir = static_cache_dir(records_base, static_key)
    export_summary_detailed_csv(records_base,
                                static_key,
                                cumulative_rounds,
                                cum_rows,
                                args.merge_stats,
                                static_dir / "all_rounds.csv",
                                "records all_rounds csv",
                                LOGGER)
    export_summary_json(records_base,
                        static_key,
                        cumulative_rounds,
                        cum_rows,
                        args.merge_stats,
                        static_dir / "all_rounds.json",
                        "records all_rounds json",
                        LOGGER)
    LOGGER.info("")
    LOGGER.info("===== EXPERIMENT LOGS =====")
    export_summary_detailed_csv(records_base,
                                static_key,
                                all_rounds,
                                rows,
                                args.merge_stats,
                                run_dir / "requested_rounds.csv",
                                "logs requested_rounds csv",
                                LOGGER)
    export_summary_json(records_base,
                        static_key,
                        all_rounds,
                        rows,
                        args.merge_stats,
                        run_dir / "requested_rounds.json",
                        "logs requested_rounds json",
                        LOGGER)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
