# Experiment 1: Preset Throughput Sweep

## Purpose

Measure throughput/reliability differences across preset quality tiers for `silo`, `wifi`, and `lte`.

## What This Experiment Runs

- Settings: `silo`, `wifi`, `lte`
- Presets: `very_weak`, `weak`, `basic`, `strong`, `very_strong`

Preset semantics:
- `silo`: sweeps `network.clients[].preset`.
- `wifi`: sweeps `network.access_points[].ap_quality` (client `device_tier` fixed to `strong`).
- `lte`: sweeps `network.enbs[].cell_quality` (client `device_tier` fixed to `strong`).

## Experiment-Specific Configuration

Config files:
- `experiments/exp1_presets/configs/<setting>_<preset>.json`

Shared exp1 values:
| Key | Value |
| --- | --- |
| `rounds` | `5` |
| `seed` | `1` |
| `clients` | `20` |
| `selection_pct` | `0.4` |
| `parallelism` | `5` |
| `positioning.mode` | `explicit` |
| `positioning.std_m` | `0.0` |

## Runner Options

Command:

```bash
bash experiments/exp1_presets/run_experiment.sh
```

Options:
- `--dry-run-only`: planning only (`orchestrator.py --dry-run`), no round execution.
- `--keep-records`: keeps existing `ns3/flsim_records/<static_key>/` data before runs.
- `--force`: passes `--force` to `orchestrator.py` (disables cache/dedup skipping).
- `--settings silo,wifi,lte`: run a subset of settings.

Examples:

```bash
bash experiments/exp1_presets/run_experiment.sh --settings wifi
bash experiments/exp1_presets/run_experiment.sh --settings silo,wifi
```

## Expected Notebook Outputs

Postprocessing notebook:

```bash
jupyter notebook experiments/exp1_presets/postprocessing.ipynb
```

Optional setting filter in notebook:

```python
ACTIVE_SETTINGS_OVERRIDE = ['wifi']
```

Notebook exports:
- `experiments/exp1_presets/outputs/figures/exp1_silo_throughput_by_preset.png`
- `experiments/exp1_presets/outputs/figures/exp1_wifi_throughput_by_preset.png`
- `experiments/exp1_presets/outputs/figures/exp1_lte_throughput_by_preset.png`
- `experiments/exp1_presets/outputs/tables/exp1_throughput_summary.csv`
- `experiments/exp1_presets/outputs/tables/exp1_throughput_summary.md`
- `experiments/exp1_presets/outputs/tables/exp1_reliability_summary.csv`
- `experiments/exp1_presets/outputs/tables/exp1_reliability_summary.md`
- `experiments/exp1_presets/outputs/tables/exp1_per_round_metrics.csv`
