# Assets: Integration with External Arbitrary FL Frameworks

`assets/` contains optional sample inputs for integrating external/arbitrary FL frameworks.
These files are used when external FL metrics need to be combined with ns-3 communication metrics.
They are loaded only when explicitly referenced in config/CLI (not default orchestrator runtime inputs).

--- 

## Purpose

The simulator measures communication behavior (upload/download and network timing).
External FL frameworks typically measure training behavior (for example `training_time_s`, accuracy, loss).
This folder connects both sides:
- `participation.csv` replicates which clients were active each round.
- `fl-results*.csv` adds external FL metrics so reports can align computation with communication fields per round (and per client when available).

--- 

## Folder Layout

```text
assets/
  participation.csv
  fl-results.csv
  fl-results/
    client_1.csv
    client_2.csv
```

--- 

## How Files Are Used

`participation.csv`
- Purpose: fixed client participation plan per round.
- Format: 0/1 matrix (`rows=rounds`, `columns=clients`).
- Hook as Configuration file feature:  `orchestration.participation.file`.
- Notes: matrix size must match requested rounds and client count, otherwise orchestration falls back to `selection_pct` (if configured).

`fl-results.csv`
- Purpose: external FL metrics merged into exported summaries aggregating per-client statisits in per-round basis.
- Format: CSV with at least a `round` column.
- Hook as CMD option: `--merge-stats assets/fl-results.csv`.
- Typical use: one already-aggregated FL output per round.

`fl-results/` (directory)
- Purpose: merge multiple external CSV result files.
- Behavior: if `summary.csv` exists, it is used; otherwise all `*.csv` files with a `round` column are loaded.
- Hook as CMD option: `--merge-stats assets/fl-results`.
- Typical use: per-client FL outputs, later aligned with communication stats of corresponding clients in exported analytics.

--- 


## Examples

```bash
python orchestrator.py --config configs/wifi.json --merge-stats assets/fl-results.csv
python orchestrator.py --config configs/lte.json --merge-stats assets/fl-results
```
