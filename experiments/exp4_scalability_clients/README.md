# Experiment 4: WiFi Scalability by Client Count

## Purpose

Measure runtime scalability as selected client count increases, and compare Unison `on` vs `off`.

## What This Experiment Runs

Scalability points:
- client counts: `1, 2, 4, 8, 16, 32, 64, 128`
- rounds per run: `1`
- unison modes: `on`, `off`

Per-point topology mapping:
- AP count follows `max(1, log2(clients))`
- resulting AP counts: `1, 1, 2, 3, 4, 5, 6, 7`


## Experiment-Specific Configuration

Shared exp4 values:
| Key | Value |
| --- | --- |
| `network_type` | `wifi` |
| `selection_pct` | `1.0` |
| `rounds` | `1` |
| `parallelism` | `1` |
| `model_size_mb` | `10` |
| `access_points[].ap_quality` | `strong` |
| `positioning.mode` | `explicit` |
| `positioning.std_m` | `1.0` |


## Runner Options

Command:

```bash
bash experiments/exp4_scalability_clients/run_experiment.sh
```

Options:
- `--dry-run-only`: planning only (`orchestrator.py --dry-run`), no round execution.
- `--keep-records`: keeps existing `ns3/flsim_records/<static_key>/` data before runs.
- `--force`: passes `--force` to `orchestrator.py` (disables cache/dedup skipping).
- `--clients 8,16,32`: run a subset of client-count points.
- `--unison-modes on,off`: run a subset of unison modes.

Examples:

```bash
bash experiments/exp4_scalability_clients/run_experiment.sh --clients 8,16,32
bash experiments/exp4_scalability_clients/run_experiment.sh --clients 32,64,128 --unison-modes on
```

## Expected Notebook Outputs

Postprocessing notebook:

```bash
jupyter notebook experiments/exp4_scalability_clients/postprocessing.ipynb
```

Notebook exports:
- `experiments/exp4_scalability_clients/outputs/tables/exp4_scalability_summary.csv`
- `experiments/exp4_scalability_clients/outputs/tables/exp4_scalability_summary.md`
- `experiments/exp4_scalability_clients/outputs/tables/exp4_unison_comparison.csv`
- `experiments/exp4_scalability_clients/outputs/tables/exp4_unison_comparison.md`
- `experiments/exp4_scalability_clients/outputs/tables/exp4_scalability_per_round.csv`
- `experiments/exp4_scalability_clients/outputs/figures/exp4_exec_time_vs_clients_by_unison.png`
- `experiments/exp4_scalability_clients/outputs/figures/exp4_avg_round_time_vs_clients_by_unison.png`
- `experiments/exp4_scalability_clients/outputs/figures/exp4_unison_speedup_vs_clients.png`
