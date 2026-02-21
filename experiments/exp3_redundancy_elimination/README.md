# Experiment 3: Redundancy Elimination (Cache + Dedup)

## Purpose

Quantify cache-hit and dedup elimination effects using staged replay.

## What This Experiment Runs

Supported scenarios:
- `silo`
- `wifi`

Scenario configs:
- `experiments/exp3_redundancy_elimination/configs/silo_parallelism20_unison_t2.json`
- `experiments/exp3_redundancy_elimination/configs/wifi_parallelism20_unison_t2_strong.json`

Staged execution model:
- `phase_a_full_100`: rounds `1..100`, forced cold baseline.
- `phase_b_warmup_80`: rounds `1..80`, warmup/cache seed.
- `phase_c_replay_100`: rounds `1..100`, replay after warmup.

Default record handling:
- records cleared before phases A and B.
- phase C reuses B records.

## Experiment-Specific Configuration

Shared exp3 values:
| Key | Value |
| --- | --- |
| `rounds` | `100` |
| `seed` | `1` |
| `clients` | `20` |
| `selection_pct` | `0.4` |
| `parallelism` | `20` |
| `unison.enabled` | `true` |
| `unison.threads` | `2` |
| `positioning.mode` | `explicit` |
| `positioning.std_m` | `0.0` |

## Runner Options

Command:

```bash
bash experiments/exp3_redundancy_elimination/run_experiment.sh
```

Options:
- `--dry-run-only`: planning only (`orchestrator.py --dry-run`), no round execution.
- `--keep-records`: keeps existing `ns3/flsim_records/<static_key>/` data at phase boundaries.
- `--force`: passes `--force` to `orchestrator.py` (phase A is already forced by design).
- `--scenario silo|wifi`: select scenario config.

Examples:

```bash
bash experiments/exp3_redundancy_elimination/run_experiment.sh --scenario wifi
bash experiments/exp3_redundancy_elimination/run_experiment.sh --scenario silo
```

## Expected Notebook Outputs

Postprocessing notebook:

```bash
jupyter notebook experiments/exp3_redundancy_elimination/postprocessing.ipynb
```

Notebook exports:
- `experiments/exp3_redundancy_elimination/outputs/tables/exp3_phase_summary.csv`
- `experiments/exp3_redundancy_elimination/outputs/tables/exp3_phase_summary.md`
- `experiments/exp3_redundancy_elimination/outputs/tables/exp3_replay_last20_summary.csv`
- `experiments/exp3_redundancy_elimination/outputs/tables/exp3_replay_last20_summary.md`
- `experiments/exp3_redundancy_elimination/outputs/tables/exp3_replay_last20_details.csv`
- `experiments/exp3_redundancy_elimination/outputs/figures/exp3_phase_cache_dedup_ratios.png`
- `experiments/exp3_redundancy_elimination/outputs/figures/exp3_phase_exec_time.png`
- `experiments/exp3_redundancy_elimination/outputs/figures/exp3_replay_last20_breakdown.png`
