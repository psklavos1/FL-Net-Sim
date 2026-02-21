# FLSim Scenarios (`scratch/flsim`)

This directory contains the ns-3 FL scenario entrypoints used by the project.

## Scenarios

| Scenario | Entrypoint | Modeled setting |
| --- | --- | --- |
| `silo` | `scratch/flsim/silo.cc` | Cross-silo FL over wired point-to-point links |
| `wifi` | `scratch/flsim/wifi.cc` | Wi-Fi clients behind APs |
| `lte` | `scratch/flsim/lte.cc` | Cellular clients over LTE |

## Quick Start

Run from `ns3/`:

```bash
./ns3 run "scratch/flsim/silo.cc --config=scratch/flsim/configs/silo.json"
./ns3 run "scratch/flsim/wifi.cc --config=scratch/flsim/configs/wifi.json"
./ns3 run "scratch/flsim/lte.cc --config=scratch/flsim/configs/lte.json"
```

## Optional Smoke Runs (No JSON)

```bash
./ns3 run "scratch/flsim/silo.cc --simTime=20 --modelSizeMb=0.2 --no-mtp"
./ns3 run "scratch/flsim/wifi.cc --simTime=20 --modelSizeMb=0.2 --no-mtp"
./ns3 run "scratch/flsim/lte.cc --simTime=10 --modelSizeMb=0.2 --computeS=1"
```

## Practical Runtime Flags

| Flag | Meaning |
| --- | --- |
| `--config=<path>` | Load scenario JSON. |
| `--round=<N>` | Round id / RNG run id. |
| `--seed=<N>` | RNG seed. |
| `--simTime=<seconds>` | Simulation stop time. |
| `--modelSizeMb=<MB>` | Model transfer size. |
| `--no-mtp` | Disable UNISON multithreading (`silo`/`wifi`). |
| `--mtp-threads=<N>` | Enable UNISON with fixed thread count (`silo`/`wifi`). |
| `--selectedClients=<i,j,...>` | Explicit selected client indices (`lte`). |

## Config Categories (JSON)

Scenario configs use these top-level categories:

| Category | Purpose |
| --- | --- |
| `description` | Run label |
| `reproducibility` | `round` and `seed` controls |
| `sim` | simulation timing parameters |
| `presets` | preset dictionaries used by network entities |
| `network` | topology, links, per-client/AP/eNB settings |
| `fl_traffic` | transport and FL payload behavior |
| `metrics` | output toggles (`flow_monitor`, `event_log`, `netanim`) |
| `lte` | LTE-specific stack/runtime options (LTE scenario) |

## Outputs

Each run produces:
- round summary CSV: `<network_type>_<description>_round_<round>.csv`
- optional `flowmon_<round>.xml` (when flow monitor is enabled)
- optional `netanim_<round>.xml` (when NetAnim is enabled)

Artifacts are then archived under:

```text
flsim_records/
  index.csv
  index.json
  <record_id>/
    seed_<seed>/
      config.json
      csv/round_<round>.csv
      flowmon/flowmon_<round>.xml        # optional
      viz/netanim_<round>.xml            # optional
```

## Orchestrator Context

These entrypoints are also invoked by `orchestrator.py`.
When run via orchestrator, stage artifacts are later ingested into canonical cache layout under
`ns3/flsim_records/<static_key>/round_cache/<round_key>/...`.
