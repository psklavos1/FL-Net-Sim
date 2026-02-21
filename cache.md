# Cache Mechanism (Simple Overview)

## What It Does

The cache avoids re-running ns-3 when a round would produce the same effective state.

The system uses two keys:
- `static_key`: identifies the static scenario setup.
- `round_key`: identifies the effective per-round state (who is selected + where clients end up).

If `round_key` already exists in cache, that round is reused.
If it does not exist, ns-3 runs once and the result is stored.

## One-Figure Cache Flow

```text
JSON config
   |
   | strip orchestration
   v
normalize_static -------------------------> static_key
   |
   +--> for each round r:
           set reproducibility.round = r
           apply participation + positioning (deterministic via seed+round)
           effective_state = {selected_mask, realized client positions}
           round_key = H(static_key + effective_state)
                |
                +-- hit?  flsim_records/<static_key>/round_cache/<round_key>/summary.csv
                |         yes -> reuse
                |         no  -> run ns-3, ingest artifacts into canonical round_key dir
                |
                +-- upsert round_index.csv: round_id -> round_key

summary export resolves:
round_id -> round_key -> canonical summary.csv
```

## Key Idea

- `round` number itself is not hashed directly into `round_key`.
- The **effective result drivers** are hashed:
  - selected clients
  - effective client positions
- So different round numbers can map to the same `round_key` if they produce the same effective state.

## Runtime Behavior (Short)

For each requested round:
1. Build `round_key` from effective state.
2. If cached summary exists, reuse it.
3. Otherwise run ns-3 and store artifacts in that `round_key` folder.
4. Record mapping in `round_index.csv` (`round_id -> round_key`).

## Storage Layout

```text
ns3/flsim_records/
  <static_key>/
    round_cache/
      <round_key>/
        summary.csv
        flowmon.xml        # optional
        netanim.xml        # optional
        effective_state.json
    round_index.csv
    config.json
```

## Practical Result

- Repeated effective states are executed once.
- Later requests reuse cached states automatically.
- Exports are still resolved by requested round IDs through `round_index.csv`.
