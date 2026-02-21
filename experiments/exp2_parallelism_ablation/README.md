# Experiment 2: Silo Runtime Modes Ablation

## Purpose

Compare runtime behavior across four combinations of intra-process and inter-process parallelism.

## What This Experiment Runs

Modes executed by `run_experiment.sh`:
- `baseline_ns3`: Intra-process `Off`, Inter-process `Off` (`baseline_no_unison_p1.json`)
- `unison_auto_p1`: Intra-process `On` (auto threads), Inter-process `Off` (`unison_auto_p1.json`)
- `parallelism20_no_unison`: Intra-process `Off`, Inter-process `On` (`parallelism20_no_unison.json`)
- `parallelism20_unison_t2`: Intra-process `On` (`threads=2`), Inter-process `On` (`parallelism20_unison_t2.json`)

## Experiment-Specific Configuration

Config directory:
- `experiments/exp2_parallelism_ablation/configs/`

Shared exp2 values:
| Key | Value |
| --- | --- |
| `network_type` | `silo` |
| `rounds` | `100` |
| `seed` | `1` |
| `clients` | `20` |
| `selection_pct` | `0.4` |
| `positioning.mode` | `explicit` |
| `positioning.std_m` | `0.0` |

Mode-dependent values:
| Config | `parallelism` | `unison.enabled` | `unison.threads` |
| --- | --- | --- | --- |
| `baseline_no_unison_p1.json` | `1` | `false` | `null` |
| `unison_auto_p1.json` | `1` | `true` | `null` |
| `parallelism20_no_unison.json` | `20` | `false` | `null` |
| `parallelism20_unison_t2.json` | `20` | `true` | `2` |

## Runner Options

Command:

```bash
bash experiments/exp2_parallelism_ablation/run_experiment.sh
```

Options:
- `--dry-run-only`: planning only (`orchestrator.py --dry-run`), no round execution.
- `--keep-records`: keeps existing `ns3/flsim_records/<static_key>/` data before runs.
- `--force`: passes `--force` to `orchestrator.py` (disables cache/dedup skipping).

## Expected Notebook Outputs

Postprocessing notebook:

```bash
jupyter notebook experiments/exp2_parallelism_ablation/exp2_postprocess.ipynb
```

Notebook exports:
- `experiments/exp2_parallelism_ablation/outputs/tables/exp2_execution_summary.csv`
- `experiments/exp2_parallelism_ablation/outputs/tables/exp2_execution_summary.md`
- `experiments/exp2_parallelism_ablation/outputs/tables/exp2_execution_summary_details.csv`
- `experiments/exp2_parallelism_ablation/outputs/tables/exp2_execution_summary_details.md`
- `experiments/exp2_parallelism_ablation/outputs/tables/exp2_per_round_walltime_summary.csv`
- `experiments/exp2_parallelism_ablation/outputs/tables/exp2_per_round_walltime_by_config.csv`
- `experiments/exp2_parallelism_ablation/outputs/figures/exp2_exec_time_bar.png`
- `experiments/exp2_parallelism_ablation/outputs/figures/exp2_avg_exec_time_per_round_bar.png`
- `experiments/exp2_parallelism_ablation/outputs/figures/exp2_wall_time_per_round_lines.png`
