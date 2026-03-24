# ts2gtopt — Time-Series Projector

Projects **hourly (or finer) time-series data** onto a gtopt planning horizon,
producing block-aggregated schedule files (Parquet or CSV) ready for use as
gtopt input.  Also embeds an `hour_block_map` in the output planning JSON so
that block-level solver results can be reconstructed back into a full hourly
time-series.

---

## Overview

Real-world time-series data (solar irradiance, wind speed, demand profiles)
is typically recorded at hourly resolution for an entire calendar year (8 760
data points).  gtopt, however, operates on a coarser time structure: blocks
grouped into stages, with each block representing a set of similar hours.

`ts2gtopt` bridges this gap by:

1. **Projecting** each hourly observation into the appropriate `(stage, block)`
   bin of the planning horizon.
2. **Aggregating** the values within each bin (default: mean, which conserves
   energy).
3. **Computing** the effective `duration` of each block (number of hours it
   represents).
4. **Writing** block-schedule Parquet/CSV files compatible with gtopt's input
   format.
5. **Embedding** an `hour_block_map` in the planning JSON for post-solve
   hourly reconstruction.

### Concepts

| Term | Meaning |
|------|---------|
| **Horizon** | A JSON definition of scenarios, stages (investment periods), and blocks (representative operating hours) |
| **Block schedule** | A Parquet/CSV file with `scenario, stage, block, uid:X` columns consumed directly by gtopt as an input schedule (e.g. `Generator/pmax.parquet`) |
| **`hour_block_map`** | An array of `{"hour": i, "stage": s, "block": b}` entries embedded in the planning JSON that maps each calendar hour back to its `(stage, block)` |
| **`output_hour/`** | Post-processing output directory with `scenario, hour, uid:X` files reconstructed from block-level solver results |

---

## Installation

Install as part of the gtopt scripts package:

```bash
# Standard install
pip install ./scripts

# Editable install with dev dependencies
pip install -e "./scripts[dev]"
```

```bash
ts2gtopt --version
ts2gtopt --help
```

### Dependencies

| Package   | Purpose                          |
|-----------|----------------------------------|
| `numpy`   | Numerical array processing       |
| `pandas`  | Time-series DataFrame handling   |
| `pyarrow` | Parquet read/write               |

---

## Quick Start

```bash
# Project hourly demand onto a 12-stage × 24-block horizon
ts2gtopt demand.csv -y 2023 -o input/

# Write 4 seasonal stages × 6 blocks each
ts2gtopt solar.csv -y 2023 --stages 4 --blocks 6 -o input/

# Use a built-in preset
ts2gtopt wind.csv -y 2023 --preset seasonal-3block -o input/

# Use a custom horizon JSON
ts2gtopt pmax.csv -y 2023 -H my_horizon.json --output-horizon updated.json -o input/

# Use the horizon from an existing gtopt planning JSON
ts2gtopt load.csv -y 2023 -P case.json -o input/

# Verify energy conservation after projection
ts2gtopt demand.csv -y 2023 --verify -o input/
```

---

## CLI Options

### Input and output

| Flag | Default | Description |
|------|---------|-------------|
| `INPUT` (positional) | — | One or more input time-series files (CSV or Parquet) |
| `-o, --output DIR` | `.` | Output directory for block-schedule files |
| `-f, --format {parquet,csv}` | `parquet` | Output file format |
| `-c, --compression ALG` | `gzip` | Parquet compression codec (pass `''` to disable) |

### Horizon source (mutually exclusive)

| Flag | Default | Description |
|------|---------|-------------|
| `-H, --horizon FILE` | — | Load planning horizon from a JSON file |
| `-P, --planning FILE` | — | Load horizon from an existing gtopt planning JSON (requires `-y`) |

If neither is given, an auto-generated horizon is created from `-y`, `-s`, and
`-b` parameters.

### Horizon configuration

| Flag | Default | Description |
|------|---------|-------------|
| `-y, --year YEAR` | — | Calendar year of the time-series (required unless `-H`/`-P` provides explicit dates) |
| `-s, --stages N` | `12` | Number of planning stages for auto-horizon |
| `-b, --blocks N` | `24` | Number of blocks per stage for auto-horizon |
| `--preset NAME` | — | Use a built-in horizon preset (overrides `-s`/`-b`) |
| `--list-presets` | — | List available presets and exit |

### Time-series processing

| Flag | Default | Description |
|------|---------|-------------|
| `-i, --interval-hours H` | auto-detected | Sampling interval of input data in hours |
| `-t, --time-column COL` | `datetime` | Name of the datetime column in input files |
| `-a, --agg FUNC` | `mean` | Aggregation function: `mean`, `median`, `min`, `max`, `sum` |

### Output and verification

| Flag | Default | Description |
|------|---------|-------------|
| `--output-horizon FILE` | — | Write the duration-updated horizon JSON to this file |
| `--verify` | off | Print energy-conservation ratios after projection |
| `-l, --log-level LEVEL` | `INFO` | Logging verbosity (`DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL`) |
| `-V, --version` | — | Print version and exit |

---

## Built-in Presets

Use `--preset NAME` to select a pre-configured horizon structure.  Presets
override the `-s` / `-b` parameters.

| Preset name | Stages | Blocks/stage | Description |
|-------------|--------|-------------|-------------|
| `seasonal-3block` | 4 (seasons × 3 months) | 3 | Night (0–6 h), Solar (7–18 h), Evening (19–23 h) |
| `monthly-3block` | 12 (months) | 3 | Same 3-block structure, one stage per month |
| `seasonal-hourly` | 4 (seasons × 3 months) | 24 | Full 24-hour resolution within each season |
| `annual-3block` | 1 (full year) | 3 | Night / Solar / Evening for the entire year |
| `annual-24h` | 1 (full year) | 24 | Full 24-hour resolution for the entire year |

```bash
# List all presets with their descriptions
ts2gtopt --list-presets

# Use the seasonal-3block preset
ts2gtopt solar.csv -y 2023 --preset seasonal-3block -o input/
```

---

## How Projection Works

### Hourly → block aggregation

For each `(scenario, stage, block)` combination, `ts2gtopt`:

1. **Selects** all rows from the input time-series where the timestamp falls
   within the stage's calendar period **and** the hour-of-day falls within the
   block's hour range.
2. **Applies** the aggregation function (default: `mean`) to produce a single
   representative value for each data column.
3. **Computes** the effective `duration` of the block as:
   ```
   duration = n_occurrences × interval_hours
   ```
   where `n_occurrences` is the number of input rows that mapped to this bin.

### Energy conservation (mean aggregation)

When using the `mean` aggregation function (the default), two conservation
properties hold:

| Property | Equation |
|----------|----------|
| **Period conservation** | `Σ_blocks duration_b == total_period_hours` |
| **Energy conservation** | `Σ_blocks (value_b × duration_b) == Σ_hours (original_value_h × interval_hours)` |

This means that the total energy (or total load, total generation, etc.) is
preserved exactly through the projection — no energy is created or lost.

Use `--verify` to print conservation ratios after projection:

```bash
ts2gtopt demand.csv -y 2023 --verify -o input/
# Energy conservation ratio for uid:1: 1.0000
# Energy conservation ratio for uid:2: 1.0000
```

A ratio of `1.0000` confirms perfect conservation.

### Other aggregation functions

| Function | Use case | Conservation |
|----------|----------|-------------|
| `mean` | Demand, generation profiles, costs | ✓ Energy conserved |
| `median` | Robust central tendency (outlier-resistant) | ✗ |
| `min` | Conservative lower bounds | ✗ |
| `max` | Conservative upper bounds | ✗ |
| `sum` | Cumulative quantities (e.g. total inflow volume) | Special handling |

---

## Reconstructing Hourly Output from Solver Results

After solving a projected case with gtopt, the solver produces block-level
output (e.g. `output/Generator/generation_sol.csv`).  The `hour_block_map`
embedded in the planning JSON allows expanding these back to hourly resolution.

### Using `write_output_hours`

```python
from ts2gtopt import write_output_hours

# Reads hour_block_map from case.json, writes output_hour/ next to output/
written = write_output_hours("bat_4b_2023.json")
# → output_hour/Generator/generation_sol.csv
#    scenario  hour   uid:1   uid:2
#    1          0    127.5    0.0
#    1          1    124.9    0.0
#    ...  (8760 rows for a full-year case)
```

### Using the public API

```python
from ts2gtopt import build_hour_block_map, reconstruct_output_hours, load_horizon

h = load_horizon("my_horizon.json")
hour_map = build_hour_block_map(h, year=2023)
reconstruct_output_hours("output/", hour_map, output_hour_dir="output_hour/")
```

### How reconstruction works

Each hour in the year is mapped to exactly one `(stage, block)` pair via the
`hour_block_map`.  For each output file (e.g. `generation_sol.csv`), the
block-level value is assigned to every hour that belongs to that block.  This
produces a step-wise hourly time-series that preserves the block-level dispatch
decisions.

---

## Python API Examples

### Projecting time-series programmatically

```python
from ts2gtopt import project_timeseries, load_horizon, build_horizon

# Auto-generate a 4-season × 3-block horizon
horizon = build_horizon(year=2023, stages=4, blocks=3)

# Project a pandas DataFrame
import pandas as pd
df = pd.read_csv("solar_profile.csv", parse_dates=["datetime"])
projected = project_timeseries(df, horizon, agg="mean", time_column="datetime")
projected.to_parquet("input/GeneratorProfile/profile.parquet")
```

### Loading and inspecting a horizon

```python
from ts2gtopt import load_horizon

h = load_horizon("my_horizon.json")
print(f"Stages: {len(h['stages'])}, Blocks: {len(h['blocks'])}")
for stage in h["stages"]:
    print(f"  Stage {stage['uid']}: {stage['start']} → {stage['end']}")
```

---

## Example: Projecting a Year of Solar Data onto 4 Seasonal Stages

```bash
# Input: solar_2023.csv with columns [datetime, uid:1, uid:2]
# 8760 rows (hourly data for year 2023)

# Project onto 4 seasonal stages × 3 blocks (night/solar/evening)
ts2gtopt solar_2023.csv \
  -y 2023 \
  --preset seasonal-3block \
  -o input/ \
  --output-horizon horizon_2023.json \
  --verify

# Output:
#   input/solar_2023.parquet          (12 rows: 4 stages × 3 blocks)
#   horizon_2023.json                 (updated with computed durations + hour_block_map)
#
# Verification output:
#   Energy conservation ratio for uid:1: 1.0000
#   Energy conservation ratio for uid:2: 1.0000

# The resulting schedule file has columns:
#   scenario | stage | block | uid:1 | uid:2
#   1        | 1     | 1     | 0.000 | 0.000    ← winter night (no solar)
#   1        | 1     | 2     | 0.412 | 0.385    ← winter solar hours (mean capacity factor)
#   1        | 1     | 3     | 0.000 | 0.000    ← winter evening
#   1        | 2     | 1     | 0.000 | 0.000    ← spring night
#   ...

# Use the projected schedule in a gtopt case
gtopt my_case.json
# (my_case.json references input/solar_2023.parquet via GeneratorProfile)
```

---

## See Also

- [SCRIPTS.md](../scripts-guide.md) — Overview of all gtopt Python scripts
- [PLANNING\_GUIDE.md](../planning-guide.md) — Guide to gtopt planning concepts
  (stages, blocks, scenarios)
- [INPUT\_DATA.md](../input-data.md) — gtopt input file format reference
- [cvs2parquet](cvs2parquet.md) — Convert existing CSV files to Parquet
- [plp2gtopt](plp2gtopt.md) — Convert PLP cases (includes time-series data)
