#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
EXP_DIR="$SCRIPT_DIR"
EXP_ID="$(basename "$EXP_DIR")"
ROOT="$(cd -- "$EXP_DIR/../.." && pwd)"

CONFIG_DIR="$EXP_DIR/configs"
OUTPUTS_DIR="$EXP_DIR/outputs"
TABLES_DIR="$OUTPUTS_DIR/tables"
FIGURES_DIR="$OUTPUTS_DIR/figures"
RUN_LOGS_DIR="$OUTPUTS_DIR/run_logs"
SOURCE_EXPORTS_DIR="$OUTPUTS_DIR/source_exports"
LOGS_DIR="$ROOT/logs"
RECORDS_DIR="$ROOT/ns3/flsim_records"

if [[ ! -f "$ROOT/orchestrator.py" ]]; then
  echo "orchestrator.py not found at expected root: $ROOT" >&2
  exit 2
fi

if [[ ! -d "$CONFIG_DIR" ]]; then
  echo "missing configs directory: $CONFIG_DIR" >&2
  exit 2
fi

RUN_DRY_ONLY=0
KEEP_RECORDS=0
RUN_FORCE=0
ROWS_FILE=""

EXPERIMENTS=(
  "baseline_ns3;Intra-process: Off | Inter-process: Off;baseline_no_unison_p1.json"
  "unison_auto_p1;Intra-process: On | Inter-process: Off;unison_auto_p1.json"
  "parallelism20_no_unison;Intra-process: Off | Inter-process: On;parallelism20_no_unison.json"
  "parallelism20_unison_t2;Intra-process: On | Inter-process: On;parallelism20_unison_t2.json"
)

usage() {
  cat <<EOF2
Usage: $(basename "$0") [--dry-run-only] [--keep-records] [--force]

Options:
  --dry-run-only   Run orchestrator dry-runs only for all modes.
  --keep-records   Do not delete existing static_key records before each mode.
  --force          Pass --force to orchestrator (disables dedup/cache skipping).
  -h, --help       Show this help.
EOF2
}

parse_args() {
  while (($#)); do
    case "$1" in
      --dry-run-only)
        RUN_DRY_ONLY=1
        shift
        ;;
      --keep-records)
        KEEP_RECORDS=1
        shift
        ;;
      --force)
        RUN_FORCE=1
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        echo "unknown argument: $1" >&2
        usage
        exit 2
        ;;
    esac
  done
}

compute_static_key() {
  local cfg_path="$1"
  (cd "$ROOT" && python - "$cfg_path" <<'PY'
import json
import sys
from pathlib import Path
from utils.config import strip_orchestration
from utils.hashing import compute_static_key

cfg = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
print(compute_static_key(strip_orchestration(cfg)))
PY
  )
}

list_log_dirs() {
  if [[ -d "$LOGS_DIR" ]]; then
    find "$LOGS_DIR" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort
  fi
}

parse_orchestrator_stats() {
  local log_capture="$1"
  local requested_csv="$2"
  local requested_json="$3"
  python - "$log_capture" "$requested_csv" "$requested_json" <<'PY'
import csv
import json
import re
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding='utf-8', errors='ignore')
requested_csv = Path(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2] else None
requested_json = Path(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] else None

network = re.findall(r"network_type=\w+\s+rounds=(\d+)\s+parallelism=(\d+)\s+cached=(\d+)\s+deduped=(\d+)", text)
planned = re.findall(r"planned\s+(\d+)\s+rounds:", text)

rounds_requested = int(network[-1][0]) if network else 0
parallelism = int(network[-1][1]) if network else 0
cached = int(network[-1][2]) if network else 0
deduped = int(network[-1][3]) if network else 0
planned_rounds = int(planned[-1]) if planned else 0

# Fallback for orchestrator versions that do not emit "planned X rounds" log lines.
if planned_rounds == 0 and requested_json and requested_json.exists():
    try:
        payload = json.loads(requested_json.read_text(encoding='utf-8'))
        overall = payload.get("overall", {})
        if isinstance(overall.get("num_rounds"), int):
            planned_rounds = int(overall["num_rounds"])
        elif isinstance(payload.get("requested_rounds"), list):
            planned_rounds = len(payload["requested_rounds"])
    except Exception:
        pass

if planned_rounds == 0 and requested_csv and requested_csv.exists():
    try:
        csv_text = requested_csv.read_text(encoding='utf-8', errors='ignore')
        first_section = csv_text.split('\n\n', 1)[0].strip()
        rows = list(csv.DictReader(first_section.splitlines()))
        if rows:
            planned_rounds = len(rows)
    except Exception:
        pass

if planned_rounds == 0:
    planned_rounds = rounds_requested

print(f"{rounds_requested}\t{parallelism}\t{cached}\t{deduped}\t{planned_rounds}")
PY
}

main() {
  parse_args "$@"

  mkdir -p "$OUTPUTS_DIR" "$TABLES_DIR" "$FIGURES_DIR" "$RUN_LOGS_DIR" "$SOURCE_EXPORTS_DIR"

  ROWS_FILE="$(mktemp)"
  trap 'rm -f "${ROWS_FILE:-}"' EXIT

  local spec
  for spec in "${EXPERIMENTS[@]}"; do
    IFS=';' read -r config_id config_label cfg_name <<<"$spec"

    local cfg="$CONFIG_DIR/$cfg_name"
    if [[ ! -f "$cfg" ]]; then
      echo "missing config: $cfg" >&2
      exit 2
    fi

    local static_key
    static_key="$(compute_static_key "$cfg")"
    local records_dir="$RECORDS_DIR/$static_key"

    if ((KEEP_RECORDS == 0)) && [[ -d "$records_dir" ]]; then
      rm -rf "$records_dir"
    fi

    local before_file after_file new_name latest_run_dir requested_csv requested_json
    before_file="$(mktemp)"
    after_file="$(mktemp)"
    list_log_dirs >"$before_file"

    local capture_log="$RUN_LOGS_DIR/${config_id}.log"
    local start_ns end_ns elapsed_ns exec_time_s
    local force_label=""
    local force_args=()
    if ((RUN_FORCE == 1)); then
      force_label=" --force"
      force_args+=(--force)
    fi

    echo "[$config_id] label=$config_label"
    echo "[$config_id] config=$cfg"
    echo "[$config_id] static_key=$static_key"

    start_ns="$(date +%s%N)"
    if ((RUN_DRY_ONLY == 1)); then
      echo "[$config_id] run: python orchestrator.py --config $cfg${force_label} --dry-run"
      (cd "$ROOT" && python orchestrator.py --config "$cfg" "${force_args[@]}" --dry-run 2>&1 | tee "$capture_log")
    else
      echo "[$config_id] run: python orchestrator.py --config $cfg${force_label}"
      (cd "$ROOT" && python orchestrator.py --config "$cfg" "${force_args[@]}" 2>&1 | tee "$capture_log")
    fi
    end_ns="$(date +%s%N)"
    elapsed_ns=$((end_ns - start_ns))
    exec_time_s="$(awk -v ns="$elapsed_ns" 'BEGIN{printf "%.6f", ns/1000000000.0}')"

    list_log_dirs >"$after_file"
    new_name="$(comm -13 "$before_file" "$after_file" | tail -n 1 || true)"
    rm -f "$before_file" "$after_file"

    latest_run_dir=""
    requested_csv=""
    requested_json=""
    if [[ -n "$new_name" ]]; then
      latest_run_dir="$LOGS_DIR/$new_name"
      requested_csv="$latest_run_dir/requested_rounds.csv"
      requested_json="$latest_run_dir/requested_rounds.json"
    fi

    local all_rounds_csv="$records_dir/all_rounds.csv"
    local all_rounds_json="$records_dir/all_rounds.json"
    local round_index_csv="$records_dir/round_index.csv"
    local round_states_csv="$records_dir/round_states.csv"

    local exports_dir="$SOURCE_EXPORTS_DIR/$config_id"
    local copied_requested_csv=""
    local copied_requested_json=""
    local copied_all_rounds_csv=""
    local copied_all_rounds_json=""
    local copied_round_index_csv=""
    local copied_round_states_csv=""
    mkdir -p "$exports_dir"
    cp "$capture_log" "$exports_dir/orchestrator_capture.log"

    if [[ -f "$requested_csv" ]]; then
      copied_requested_csv="$exports_dir/requested_rounds.csv"
      cp "$requested_csv" "$copied_requested_csv"
    fi
    if [[ -f "$requested_json" ]]; then
      copied_requested_json="$exports_dir/requested_rounds.json"
      cp "$requested_json" "$copied_requested_json"
    fi
    if [[ -f "$all_rounds_csv" ]]; then
      copied_all_rounds_csv="$exports_dir/all_rounds.csv"
      cp "$all_rounds_csv" "$copied_all_rounds_csv"
    fi
    if [[ -f "$all_rounds_json" ]]; then
      copied_all_rounds_json="$exports_dir/all_rounds.json"
      cp "$all_rounds_json" "$copied_all_rounds_json"
    fi
    if [[ -f "$round_index_csv" ]]; then
      copied_round_index_csv="$exports_dir/round_index.csv"
      cp "$round_index_csv" "$copied_round_index_csv"
    fi
    if [[ -f "$round_states_csv" ]]; then
      copied_round_states_csv="$exports_dir/round_states.csv"
      cp "$round_states_csv" "$copied_round_states_csv"
    fi

    local parsed rounds_requested parallelism cached deduped planned_rounds
    parsed="$(parse_orchestrator_stats "$capture_log" "$requested_csv" "$requested_json")"
    IFS=$'\t' read -r rounds_requested parallelism cached deduped planned_rounds <<<"$parsed"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$config_id" \
      "$config_label" \
      "$cfg" \
      "$static_key" \
      "$records_dir" \
      "$exec_time_s" \
      "$rounds_requested" \
      "$planned_rounds" \
      "$cached" \
      "$deduped" \
      "$parallelism" \
      "$latest_run_dir" \
      "$requested_csv" \
      "$requested_json" \
      "$exports_dir" \
      "$copied_requested_csv" \
      "$copied_requested_json" \
      "$copied_all_rounds_csv" \
      "$copied_all_rounds_json" \
      "$copied_round_index_csv" >>"$ROWS_FILE"
  done

  local manifest_latest="$OUTPUTS_DIR/last_run_manifest.json"
  local timestamp manifest_snapshot
  timestamp="$(date +%Y%m%d_%H%M%S)"
  manifest_snapshot="$OUTPUTS_DIR/run_manifest_${timestamp}.json"

  python - "$ROWS_FILE" "$manifest_latest" "$manifest_snapshot" "$EXP_ID" "$ROOT" "$RUN_DRY_ONLY" "$KEEP_RECORDS" "$RUN_FORCE" "$EXP_DIR" <<'PY'
import csv
import json
import sys
from datetime import datetime
from pathlib import Path

rows_path = Path(sys.argv[1])
latest = Path(sys.argv[2])
snapshot = Path(sys.argv[3])
exp_id = sys.argv[4]
root = sys.argv[5]
run_dry_only = bool(int(sys.argv[6]))
keep_records = bool(int(sys.argv[7]))
run_force = bool(int(sys.argv[8]))
exp_dir = sys.argv[9]

rows = []
with rows_path.open("r", encoding="utf-8") as f:
    reader = csv.reader(f, delimiter="\t")
    for item in reader:
        rows.append(
            {
                "config_id": item[0],
                "config_label": item[1],
                "config_path": item[2],
                "static_key": item[3],
                "records_dir": item[4],
                "exec_time_s": float(item[5]),
                "rounds_requested": int(item[6]),
                "planned_rounds": int(item[7]),
                "cached_rounds": int(item[8]),
                "deduped_rounds": int(item[9]),
                "parallelism_reported": int(item[10]),
                "latest_run_dir": item[11] or None,
                "requested_rounds_csv": item[12] or None,
                "requested_rounds_json": item[13] or None,
                "exports_dir": item[14] or None,
                "copied_requested_rounds_csv": item[15] or None,
                "copied_requested_rounds_json": item[16] or None,
                "copied_all_rounds_csv": item[17] or None,
                "copied_all_rounds_json": item[18] or None,
                "copied_round_index_csv": item[19] or None,
            }
        )

payload = {
    "experiment_id": exp_id,
    "created_at": datetime.now().isoformat(timespec="seconds"),
    "root_dir": root,
    "experiment_dir": exp_dir,
    "outputs_dir": str(latest.parent),
    "mode": {
      "dry_run_only": run_dry_only,
      "keep_records": keep_records,
      "force": run_force,
    },
    "runs": rows,
}

text = json.dumps(payload, indent=2) + "\n"
latest.write_text(text, encoding="utf-8")
snapshot.write_text(text, encoding="utf-8")
PY

  echo ""
  echo "Experiment 2 automation complete."
  echo "Manifest: $manifest_latest"
  while IFS=$'\t' read -r config_id config_label cfg static_key records_dir exec_time rounds_req planned cached deduped par latest_run requested_csv requested_json exports_dir copied_requested_csv copied_requested_json copied_all_rounds_csv copied_all_rounds_json copied_round_index_csv; do
    echo "[$config_id] exec_time_s=$exec_time rounds_requested=$rounds_req planned=$planned cached=$cached deduped=$deduped"
    echo "[$config_id] source_exports_dir=$exports_dir"
    if [[ -n "$copied_requested_csv" ]]; then
      echo "[$config_id] copied_requested_rounds_csv=$copied_requested_csv"
    fi
  done <"$ROWS_FILE"
}

main "$@"
