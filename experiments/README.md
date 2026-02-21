# Experiments Organization

## Purpose

This directory groups experiment-specific configs, runners, and postprocessing notebooks.

## Directory Layout

- `experiments/<experiment_id>/configs/`: orchestrator config files used by that experiment.
- `experiments/<experiment_id>/run_experiment.sh`: experiment runner script.
- `experiments/<experiment_id>/*postprocess*.ipynb`: notebook for experiment figures/tables.
- `experiments/<experiment_id>/outputs/`: generated run and notebook outputs.
- `experiments/<experiment_id>/README.md`: experiment-specific purpose, options, and expected notebook outputs.

## Standard Workflow

1. Run an experiment script:
   - `bash experiments/<experiment_id>/run_experiment.sh`
2. (Optional) run one config directly:
   - `python orchestrator.py --config experiments/<experiment_id>/configs/<scenario>.json`
3. Run the experiment notebook for figures/tables.

## Output Organization

Generic output layout for each experiment:

- `experiments/<experiment_id>/outputs/`
  - `run_manifest_<timestamp>.json`: timestamped run metadata snapshot.
  - `run_logs/`: per-run runner/orchestrator logs.
  - `source_exports/`: copied raw run artifacts used by notebooks.
  - `tables/`: notebook-generated tabular outputs.
  - `figures/`: notebook-generated plots/figures.

## Current Experiments

- `exp1_presets`: preset sweep across `silo`/`wifi`/`lte`.
- `exp2_parallelism_ablation`: intra-process vs inter-process parallelism ablation.
- `exp3_redundancy_elimination`: staged cache + dedup replay analysis.
- `exp4_scalability_clients`: WiFi scalability with increasing selected clients.
