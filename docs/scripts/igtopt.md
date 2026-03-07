# igtopt — Excel to gtopt JSON + Parquet Converter

## Overview

**igtopt** converts Excel workbooks (`.xlsx`) into the gtopt JSON planning
format plus optional time-series Parquet (or CSV) files. It is the recommended
way to prepare gtopt input data for users who prefer spreadsheets over editing
raw JSON.

**Who needs it?** Any user building a gtopt case from scratch. The Excel
format provides a structured, auditable way to define buses, generators,
demands, lines, batteries, and time-series profiles — all in a single
workbook.

**Key features:**

- Reads one or more `.xlsx` workbooks and merges them into a single JSON file
- Writes time-series data (demand profiles, generator profiles, etc.) as
  Parquet or CSV files via `@`-named sheets
- Supports all gtopt planning elements: buses, generators, demands, lines,
  batteries, converters, reservoirs, junctions, and more
- Produces ZIP bundles compatible with `gtopt_guisrv` and `gtopt_websrv`
- Logs conversion statistics (element counts, key options, elapsed time)

---

## Installation

```bash
# Production install (registers igtopt on PATH)
pip install ./scripts

# Editable install with dev/test dependencies
pip install -e "./scripts[dev]"

# Verify
igtopt --version
```

---

## Quick Start

```bash
# Basic conversion — produces system.json + system/ directory
igtopt system.xlsx

# Explicit output path with pretty-printed JSON
igtopt system.xlsx -j output/system.json --pretty

# Skip null/NaN values from output JSON
igtopt system.xlsx --pretty --skip-nulls

# Use CSV instead of Parquet for time-series files
igtopt system.xlsx -f csv

# Create a ZIP bundle for upload to gtopt_guisrv
igtopt system.xlsx --zip

# Merge multiple workbooks into one case
igtopt case_a.xlsx case_b.xlsx -d /data/input

# Debug logging for troubleshooting
igtopt system.xlsx -l DEBUG
```

---

## CLI Options

| Flag | Long form | Type | Default | Description |
|------|-----------|------|---------|-------------|
| (positional) | `XLSX` | path(s) | *required* | One or more Excel workbooks to convert |
| `-j` | `--json-file` | path | `<stem>.json` | Output JSON file path |
| `-d` | `--input-directory` | path | `<stem>/` | Directory for time-series data files from `@` sheets |
| `-f` | `--input-format` | `csv` \| `parquet` | `parquet` | File format for time-series data |
| `-n` | `--name` | string | `<stem>` | System name written to JSON output |
| `-c` | `--compression` | string | `gzip` | Parquet compression (`gzip`, `snappy`, `brotli`, or `''`) |
| `-p` | `--pretty` | bool | `False` | Pretty-print JSON with 4-space indentation |
| `-N` | `--skip-nulls` | bool | `False` | Omit keys with null/NaN values from JSON |
| `-U` | `--parse-unexpected-sheets` | bool | `False` | Process sheets not in the expected list |
| `-z` | `--zip` | bool | `False` | Bundle JSON + data files into a single ZIP archive |
| `-l` | `--log-level` | level | `INFO` | Logging verbosity: `DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL` |
| `-V` | `--version` | — | — | Print version and exit |

The `<stem>` is the filename of the first workbook without its `.xlsx`
extension. For example, `igtopt ieee57b.xlsx` produces `ieee57b.json` and
writes time-series files to `ieee57b/`.

---

## Excel Workbook Format

An igtopt workbook contains three kinds of sheets, distinguished by their
names:

| Sheet name pattern | Action | Example |
|--------------------|--------|---------|
| Starts with `.` | **Skipped** silently | `.notes`, `.scratch` |
| Contains `@` | Written as **time-series file** | `Demand@lmax` → `Demand/lmax.parquet` |
| Everything else | Parsed as **JSON array** | `bus_array`, `generator_array`, `options` |

### 5.1 The `options` Sheet

Two columns: **option** and **value**. Each row sets one planning option.

| option | value |
|--------|-------|
| `use_kirchhoff` | `True` |
| `use_single_bus` | `False` |
| `demand_fail_cost` | `1000` |
| `scale_objective` | `1000` |
| `annual_discount_rate` | `0.1` |
| `input_directory` | `ieee57b` |
| `input_format` | `parquet` |
| `output_format` | `csv` |

Boolean values are written as `True` / `False`. Numeric values are parsed
automatically. See [INPUT_DATA.md](../../INPUT_DATA.md) for the full list of
supported options.

### 5.2 Simulation Sheets

These define the time structure of the optimization problem.

#### `block_array`

| uid | duration |
|-----|----------|
| 1 | 1.0 |
| 2 | 1.0 |
| … | … |

Each row is one time block. `duration` is in hours.

#### `stage_array`

| uid | first_block | count_block | active |
|-----|-------------|-------------|--------|
| 1 | 1 | 24 | True |

`first_block` is 1-indexed. `count_block` is the number of blocks in this
stage.

#### `scenario_array`

| uid | probability_factor |
|-----|--------------------|
| 1 | 1.0 |

For deterministic problems, use a single scenario with `probability_factor=1`.

#### `phase_array` and `scene_array`

Optional sheets for advanced multi-phase / multi-scene simulations. Most cases
do not need them — gtopt auto-creates defaults when they are absent.

### 5.3 System Sheets

Each sheet becomes a JSON array under the `system` key. The 20 recognized
system sheets are:

| Sheet name | Key columns | Description |
|------------|-------------|-------------|
| `bus_array` | uid, name, reference_theta | Electrical nodes |
| `generator_array` | uid, name, bus, gcost, pmin, pmax, capacity | Generators (thermal, renewable, hydro) |
| `generator_profile_array` | uid, name, generator | Time-varying capacity factor profiles |
| `demand_array` | uid, name, bus, lmax | Loads / demands |
| `demand_profile_array` | uid, name, demand | Time-varying demand scaling |
| `line_array` | uid, name, bus_a, bus_b, reactance, tmax_ab, tmax_ba | Transmission lines |
| `battery_array` | uid, name, bus, emax, pmax_charge, pmax_discharge | Energy storage (unified) |
| `converter_array` | uid, name, battery, generator, demand | Battery ↔ generator/demand coupling |
| `reserve_zone_array` | uid, name, urreq, drreq | Spinning reserve zones |
| `reserve_provision_array` | uid, name, generator, reserve_zone, urmax | Reserve contributions |
| `junction_array` | uid, name | Hydraulic nodes |
| `waterway_array` | uid, name, junction_a, junction_b | Water channels |
| `reservoir_array` | uid, name, junction | Hydro reservoirs |
| `turbine_array` | uid, name, waterway, generator | Hydro turbines |
| `flow_array` | uid, name, junction | Inflows / evaporation |
| `filtration_array` | uid, name, waterway, reservoir | Water seepage |
| `outflow_array` | uid, name, junction | Water outflows |
| `emission_zone_array` | uid, name | Emission zones |
| `generator_emission_array` | uid, name, generator, emission_zone | Generator emissions |
| `demand_emissions` | uid, name, demand, emission_zone | Demand emissions |

### 5.4 Time-Series Sheets (`@` Sheets)

Sheets named `Component@field` are written as data files to the input
directory. The naming convention is:

```
<ComponentType>@<field_name>  →  <input_dir>/<ComponentType>/<field_name>.parquet
```

**Examples:**

| Sheet name | Output file | Purpose |
|------------|-------------|---------|
| `Demand@lmax` | `input/Demand/lmax.parquet` | Hourly demand limits |
| `GeneratorProfile@profile` | `input/GeneratorProfile/profile.parquet` | Solar/wind capacity factors |
| `Battery@emax` | `input/Battery/emax.parquet` | Time-varying energy limits |

**Required columns:**

- `scenario` (int) — scenario index (1-based)
- `stage` (int) — stage index (1-based)
- `block` (int) — block index (1-based)
- One column per element, named by the element's `name` field (float values)

**Example `Demand@lmax` sheet (3 demands, 4 blocks):**

| scenario | stage | block | d1 | d2 | d3 |
|----------|-------|-------|----|----|----|
| 1 | 1 | 1 | 100.0 | 50.0 | 75.0 |
| 1 | 1 | 2 | 120.0 | 60.0 | 80.0 |
| 1 | 1 | 3 | 150.0 | 70.0 | 95.0 |
| 1 | 1 | 4 | 130.0 | 55.0 | 85.0 |

When an element's field references a time-series file, the JSON output
contains the **file stem** (without extension) as a string:

```json
{"uid": 1, "name": "d1", "bus": "b1", "lmax": "lmax"}
```

The gtopt solver resolves `"lmax"` → `<input_directory>/Demand/lmax.parquet`
and reads the column named `"d1"`.

---

## Tutorial: IEEE 57-Bus Case (Step by Step)

This tutorial walks through creating an Excel workbook for the standard IEEE
57-bus test network. The result is a single-snapshot DC Optimal Power Flow
with 57 buses, 7 generators, 42 demands, and 80 transmission lines.

> **Reference topology:** For a visual example of a gtopt network topology,
> see the [IEEE 9-bus diagram](../diagrams/ieee9b_electrical.svg). The IEEE
> 57-bus network follows the same principles at larger scale.

### Step 1: Create the `options` Sheet

Create a sheet named `options` with two columns:

| option | value |
|--------|-------|
| `use_kirchhoff` | `True` |
| `use_single_bus` | `False` |
| `demand_fail_cost` | `1000` |
| `scale_objective` | `1000` |
| `input_directory` | `ieee57b` |
| `input_format` | `parquet` |
| `output_format` | `csv` |

**Why these values?**

- `use_kirchhoff=True` enables DC power flow with voltage angles — essential
  for a multi-bus OPF.
- `use_single_bus=False` activates the full network model with 57 buses.
- `demand_fail_cost=1000` sets the penalty for unserved load at $1000/MWh,
  well above the most expensive generator cost ($40/MWh), preventing
  load shedding in feasible cases.
- `scale_objective=1000` divides all objective coefficients by 1000 for
  better solver numerics. The reported `obj_value` is in scaled units.

### Step 2: Create the Simulation Structure

This is a single-snapshot OPF (one block, one stage, one scenario).

**`block_array` sheet:**

| uid | duration |
|-----|----------|
| 1 | 1.0 |

**`stage_array` sheet:**

| uid | first_block | count_block | active |
|-----|-------------|-------------|--------|
| 1 | 1 | 1 | True |

**`scenario_array` sheet:**

| uid | probability_factor |
|-----|--------------------|
| 1 | 1.0 |

### Step 3: Create the `bus_array` Sheet (57 Buses)

The IEEE 57-bus network has 57 electrical nodes. Bus 1 is the **slack bus**
(reference bus) with `reference_theta=0` to anchor voltage angles.

| uid | name | reference_theta |
|-----|------|-----------------|
| 1 | b1 | 0 |
| 2 | b2 | |
| 3 | b3 | |
| 4 | b4 | |
| 5 | b5 | |
| … | … | |
| 55 | b55 | |
| 56 | b56 | |
| 57 | b57 | |

Only bus `b1` has `reference_theta=0`. All other buses leave the column
blank (null). The solver determines their voltage angles as decision
variables.

**Tip:** The `name` column uses the convention `b<N>` where `N` is the bus
number. This naming is used throughout to reference buses in other sheets.

### Step 4: Create the `generator_array` Sheet (7 Generators)

The IEEE 57-bus system has 7 generators at buses 1, 2, 3, 6, 8, 9, and 12.

| uid | name | bus | pmin | pmax | gcost | capacity |
|-----|------|-----|------|------|-------|----------|
| 1 | g1 | 1 | 0 | 575.88 | 20 | 575.88 |
| 2 | g2 | 2 | 0 | 100 | 20 | 100 |
| 3 | g3 | 3 | 0 | 140 | 20 | 140 |
| 4 | g6 | 6 | 0 | 100 | 35 | 100 |
| 5 | g8 | 8 | 0 | 120 | 35 | 120 |
| 6 | g9 | 9 | 0 | 100 | 40 | 100 |
| 7 | g12 | 12 | 0 | 200 | 40 | 200 |

**Key points:**

- The `bus` column references the bus **uid** (integer), not the name.
- `g1` at bus 1 is the slack generator with the largest capacity (575.88 MW).
- Generators at buses 1–3 are cheaper ($20/MWh) than those at buses 6–8
  ($35/MWh) and 9–12 ($40/MWh). The solver dispatches cheaper units first.
- `capacity` equals `pmax` here (no expansion). The solver cannot build
  beyond this limit.
- `pmin=0` for all generators — no minimum output constraint.

### Step 5: Create the `demand_array` Sheet (42 Demands)

The system has 42 demand buses. Each demand has a fixed `lmax` value
representing the load in MW. Here are the first several and last entries:

| uid | name | bus | lmax |
|-----|------|-----|------|
| 1 | d1 | 1 | 55 |
| 2 | d2 | 2 | 3 |
| 3 | d3 | 3 | 41 |
| 4 | d5 | 5 | 13 |
| 5 | d6 | 6 | 75 |
| 6 | d8 | 8 | 150 |
| 7 | d9 | 9 | 121 |
| 8 | d10 | 10 | 5 |
| 9 | d12 | 12 | 377 |
| 10 | d13 | 13 | 18 |
| … | … | … | … |
| 40 | d54 | 54 | 113 |
| 41 | d55 | 55 | 63 |
| 42 | d56 | 56 | 84 |

**Notes:**

- Not every bus has a demand — only 42 of the 57 buses are load buses.
- The `lmax` column is a scalar value (MW). For time-varying demands, use a
  `Demand@lmax` sheet instead (see the bat4b24 tutorial below).
- Demand names use the convention `d<bus_number>`.
- Total system load is approximately 1250.8 MW.

### Step 6: Create the `line_array` Sheet (80 Lines)

The network has 80 transmission lines. Each line connects two buses and has a
reactance value (per-unit) plus thermal limits. Here are the first several:

| uid | name | bus_a | bus_b | reactance | tmax_ab | tmax_ba | voltage |
|-----|------|-------|-------|-----------|---------|---------|---------|
| 1 | l1_2 | 1 | 2 | 0.0625 | 1000 | 1000 | 1.0 |
| 2 | l2_3 | 2 | 3 | 0.0411 | 1000 | 1000 | 1.0 |
| 3 | l3_4 | 3 | 4 | 0.0128 | 1000 | 1000 | 1.0 |
| 4 | l4_5 | 4 | 5 | 0.0086 | 1000 | 1000 | 1.0 |
| 5 | l4_6 | 4 | 6 | 0.0126 | 1000 | 1000 | 1.0 |
| 6 | l6_7 | 6 | 7 | 0.0208 | 1000 | 1000 | 1.0 |
| 7 | l6_8 | 6 | 8 | 0.0556 | 1000 | 1000 | 1.0 |
| 8 | l8_9 | 8 | 9 | 0.0182 | 1000 | 1000 | 1.0 |
| 9 | l9_10 | 9 | 10 | 0.0121 | 1000 | 1000 | 1.0 |
| 10 | l9_11 | 9 | 11 | 0.0297 | 1000 | 1000 | 1.0 |
| … | … | … | … | … | … | … | … |

**Key points:**

- `reactance` is in per-unit (p.u.) on the system base. Required for
  Kirchhoff (DC OPF) mode.
- `tmax_ab` and `tmax_ba` are the thermal limits in each direction (MW).
  Setting both to 1000 MW means lines are effectively unconstrained in this
  base case — the solver uses reactance-based flow distribution.
- Line names use the convention `l<bus_a>_<bus_b>`.
- The 80 lines include both intra-voltage and inter-voltage connections
  (transformers are modeled as lines with appropriate reactances).

### Step 7: Run the Conversion

```bash
igtopt ieee57b.xlsx -j ieee57b.json --pretty
```

Expected output:

```
INFO: Processing workbook: ieee57b.xlsx
INFO: sheet options → options
INFO: sheet block_array → simulation
INFO: sheet stage_array → simulation
INFO: sheet scenario_array → simulation
INFO: sheet bus_array → system
INFO: sheet generator_array → system
INFO: sheet demand_array → system
INFO: sheet line_array → system

=== System statistics ===
  Buses           : 57
  Generators      : 7
  Demands         : 42
  Lines           : 80
=== Simulation statistics ===
  Blocks          : 1
  Stages          : 1
  Scenarios       : 1
=== Key options ===
  use_single_bus  : False
  scale_objective : 1000
  demand_fail_cost: 1000
  input_directory : ieee57b
=== Conversion time ===
  Elapsed         : 0.15s
```

### Step 8: Verify the Output

Check the generated JSON structure:

```bash
# Verify element counts
python -c "
import json
with open('ieee57b.json') as f:
    data = json.load(f)
s = data['system']
print(f'Buses:      {len(s[\"bus_array\"])}')      # 57
print(f'Generators: {len(s[\"generator_array\"])}') # 7
print(f'Demands:    {len(s[\"demand_array\"])}')    # 42
print(f'Lines:      {len(s[\"line_array\"])}')      # 80
"
```

Solve the case with gtopt:

```bash
gtopt ieee57b.json
cat output/solution.csv
# Expected: status=0 (optimal), obj_value ≈ 25.016 (scaled by 1000)
```

The unscaled total cost is approximately **25,016 $/h**. This is the
minimum-cost dispatch satisfying all network constraints (Kirchhoff's laws)
and generator limits.

### Step 9: Inspect the Solution

```bash
# Generator dispatch (MW per generator)
cat output/Generator/generation_sol.csv

# Locational Marginal Prices ($/MWh at each bus)
cat output/Bus/balance_dual.csv

# Verify no load shedding
cat output/Demand/fail_sol.csv   # all values should be 0
```

In the optimal solution, the three cheapest generators (g1, g2, g3 at
$20/MWh) carry most of the load. The more expensive generators at buses 6, 8,
9, and 12 dispatch only what is needed to satisfy local demand and network
flow constraints.

### Step 10: Validate Against pandapower (Optional)

If you have the `gtopt-compare` script installed:

```bash
cd scripts
gtopt-compare --case ieee_case57 --gtopt-output ../output/
```

This runs pandapower's DC OPF on the same network and compares:

- **Total generation** — must match within ±1 MW
- **Total cost** — must match within 0.1%
- **Bus LMPs** — locational marginal prices must match within ±0.5 $/MWh

---

## Tutorial: 4-Bus Battery Case (24-Hour with Time-Series)

This shorter tutorial demonstrates battery storage, solar profiles, and
time-series `@` sheets using the `bat4b24` test case — a 4-bus network with a
24-hour simulation.

> **Topology:** See the [4-bus battery diagram](../diagrams/bat4b_electrical.svg)
> for the network layout.

### Network Description

- **4 buses** (b1–b4)
- **3 generators**: g1 (thermal, $20/MWh), g2 (thermal, $35/MWh),
  g\_solar (solar, $0/MWh with a capacity profile)
- **2 demands**: d3 at bus b3, d4 at bus b4
- **5 transmission lines** connecting the buses
- **1 battery**: bat1 at bus b3 (200 MWh, 60 MW charge/discharge, 95%
  round-trip efficiency)
- **1 generator profile**: gp\_solar (24-hour solar capacity factors)

### Step 1: Options and Simulation

**`options` sheet:**

| option | value |
|--------|-------|
| `use_kirchhoff` | `True` |
| `use_single_bus` | `False` |
| `demand_fail_cost` | `1000` |
| `scale_objective` | `1000` |

**`block_array` sheet** — 24 hourly blocks:

| uid | duration |
|-----|----------|
| 1 | 1.0 |
| 2 | 1.0 |
| … | … |
| 24 | 1.0 |

**`stage_array`:** 1 stage spanning all 24 blocks
(`first_block=1`, `count_block=24`).

**`scenario_array`:** 1 scenario with `probability_factor=1.0`.

### Step 2: System Components

**`generator_array`:**

| uid | name | bus | pmin | pmax | gcost | capacity |
|-----|------|-----|------|------|-------|----------|
| 1 | g1 | 1 | 0 | 300 | 20 | 300 |
| 2 | g2 | 2 | 0 | 200 | 35 | 200 |
| 3 | g_solar | 3 | 0 | 150 | 0 | 150 |

**`demand_array`:**

| uid | name | bus | lmax |
|-----|------|-----|------|
| 1 | d3 | 3 | lmax |
| 2 | d4 | 4 | lmax |

Note that `lmax` is set to the string `"lmax"` — this tells gtopt to read
the demand profile from the file `Demand/lmax.parquet` (produced by the
`Demand@lmax` sheet).

**`battery_array`:**

| uid | name | bus | pmax_charge | pmax_discharge | emax | input_efficiency | output_efficiency |
|-----|------|-----|-------------|----------------|------|-----------------|-------------------|
| 1 | bat1 | 3 | 60 | 60 | 200 | 0.95 | 0.95 |

This uses the **unified battery definition** — charge and discharge are
defined directly on the battery without separate converter/generator/demand
entries. gtopt creates the internal converter, charge demand, and discharge
generator automatically.

**`generator_profile_array`:**

| uid | name | generator | profile |
|-----|------|-----------|---------|
| 1 | gp_solar | 3 | profile |

The string `"profile"` references
`GeneratorProfile/profile.parquet` (from the `GeneratorProfile@profile`
sheet).

### Step 3: Time-Series Sheets

**`Demand@lmax` sheet** — 24-hour demand profiles:

| scenario | stage | block | d3 | d4 |
|----------|-------|-------|----|----|
| 1 | 1 | 1 | 80.0 | 50.0 |
| 1 | 1 | 2 | 75.0 | 45.0 |
| 1 | 1 | 3 | 70.0 | 40.0 |
| 1 | 1 | 4 | 65.0 | 38.0 |
| 1 | 1 | 5 | 70.0 | 40.0 |
| 1 | 1 | 6 | 80.0 | 50.0 |
| 1 | 1 | 7 | 100.0 | 65.0 |
| 1 | 1 | 8 | 130.0 | 80.0 |
| 1 | 1 | 9 | 150.0 | 95.0 |
| 1 | 1 | 10 | 160.0 | 100.0 |
| 1 | 1 | 11 | 170.0 | 105.0 |
| 1 | 1 | 12 | 175.0 | 110.0 |
| 1 | 1 | 13 | 170.0 | 105.0 |
| 1 | 1 | 14 | 165.0 | 100.0 |
| 1 | 1 | 15 | 160.0 | 95.0 |
| 1 | 1 | 16 | 155.0 | 90.0 |
| 1 | 1 | 17 | 160.0 | 95.0 |
| 1 | 1 | 18 | 175.0 | 110.0 |
| 1 | 1 | 19 | 180.0 | 115.0 |
| 1 | 1 | 20 | 170.0 | 105.0 |
| 1 | 1 | 21 | 150.0 | 95.0 |
| 1 | 1 | 22 | 130.0 | 80.0 |
| 1 | 1 | 23 | 110.0 | 65.0 |
| 1 | 1 | 24 | 90.0 | 55.0 |

**`GeneratorProfile@profile` sheet** — solar capacity factors (0.0 to 1.0):

| scenario | stage | block | gp_solar |
|----------|-------|-------|----------|
| 1 | 1 | 1 | 0.0 |
| 1 | 1 | 2 | 0.0 |
| 1 | 1 | 3 | 0.0 |
| 1 | 1 | 4 | 0.0 |
| 1 | 1 | 5 | 0.0 |
| 1 | 1 | 6 | 0.05 |
| 1 | 1 | 7 | 0.15 |
| 1 | 1 | 8 | 0.35 |
| 1 | 1 | 9 | 0.55 |
| 1 | 1 | 10 | 0.75 |
| 1 | 1 | 11 | 0.85 |
| 1 | 1 | 12 | 0.90 |
| 1 | 1 | 13 | 0.85 |
| 1 | 1 | 14 | 0.75 |
| 1 | 1 | 15 | 0.55 |
| 1 | 1 | 16 | 0.35 |
| 1 | 1 | 17 | 0.15 |
| 1 | 1 | 18 | 0.05 |
| 1 | 1 | 19 | 0.0 |
| 1 | 1 | 20 | 0.0 |
| 1 | 1 | 21 | 0.0 |
| 1 | 1 | 22 | 0.0 |
| 1 | 1 | 23 | 0.0 |
| 1 | 1 | 24 | 0.0 |

The profile peaks at block 12 (noon) with a factor of 0.90, meaning the
solar generator can produce 0.90 × 150 = 135 MW at that hour.

### Step 4: Run and Solve

```bash
# Convert Excel to JSON + Parquet
igtopt bat4b24.xlsx -j bat4b24.json --pretty

# Verify time-series files were created
ls bat4b24/Demand/lmax.parquet
ls bat4b24/GeneratorProfile/profile.parquet

# Solve
gtopt bat4b24.json
cat output/solution.csv
# Expected: status=0, obj_value ≈ 44.862 (scaled)
```

The battery charges during midday solar hours (cheap $0 solar energy) and
discharges during evening peak demand, reducing the need for the more
expensive g2 ($35/MWh).

---

## Downloadable Template

The [gtopt Excel template](../templates/gtopt_template.xlsx) provides a
ready-to-use workbook with pre-configured sheets for all gtopt planning
elements. It includes:

- An `options` sheet with common settings pre-filled
- Empty `block_array`, `stage_array`, and `scenario_array` sheets with headers
- All 20 system sheets with column headers
- Example `Demand@lmax` and `GeneratorProfile@profile` time-series sheets

Start from this template and fill in your data to build a complete case.

---

## ZIP Output

The `--zip` flag bundles the JSON file and all time-series data files into a
single ZIP archive:

```bash
igtopt system.xlsx --zip
```

This produces `system.zip` with the structure:

```
system.zip
├── system.json
└── system/
    ├── Demand/
    │   └── lmax.parquet
    └── GeneratorProfile/
        └── profile.parquet
```

**Use cases:**

- Upload to **gtopt_guisrv** (Flask GUI) via the file upload endpoint
- Submit to **gtopt_websrv** (Next.js REST API) as a job payload
- Share complete cases as a single file

---

## Conversion Statistics

After a successful conversion, igtopt logs statistics at the `INFO` level:

```
=== System statistics ===
  Buses           : 4
  Generators      : 3
  Demands         : 2
  Lines           : 5
  Batteries       : 1
=== Simulation statistics ===
  Blocks          : 24
  Stages          : 1
  Scenarios       : 1
=== Key options ===
  use_single_bus  : False
  scale_objective : 1000
  demand_fail_cost: 1000
  input_directory : bat4b24
=== Time-series files ===
  Demand/lmax.parquet           : 24 rows × 2 columns
  GeneratorProfile/profile.parquet : 24 rows × 1 column
=== Conversion time ===
  Elapsed         : 0.18s
```

Use `--log-level DEBUG` for additional detail including per-sheet processing
information and JSON serialization diagnostics.

---

## Integration Test Coverage

The igtopt test suite in
[`scripts/igtopt/tests/test_igtopt.py`](../../scripts/igtopt/tests/test_igtopt.py)
covers:

### Structure Tests

- **IEEE 57-bus**: element counts (57/7/42/80), simulation structure,
  options validation, generator fields, line fields
- **Battery 4-bus 24-hour**: element counts (4/3/2/5/1), battery parameters,
  time-series file generation, demand profiles, solar profiles
- **Reference case (c0)**: JSON round-trip comparison against known-good output

### Solver Validation Tests (marked `@pytest.mark.integration`)

- **IEEE 57-bus solver**: exit status, optimal solution status, objective value
  within 0.1% of 25,016 $/h reference, pandapower DC OPF cross-validation
- **bat4b24 solver**: exit status, optimal solution, objective value within
  0.1% of 44,862 $/h reference, zero load shedding across all 24 blocks,
  pandapower comparison

Run the tests:

```bash
cd scripts

# Unit tests only (fast)
python -m pytest igtopt/tests/ -q

# Integration tests (requires gtopt binary)
python -m pytest igtopt/tests/ -m integration -q
```

---

## See Also

- [SCRIPTS.md](../../SCRIPTS.md) — Overview of all Python scripts including
  igtopt usage examples and the full sheet reference
- [PLANNING_GUIDE.md](../../PLANNING_GUIDE.md) — How to structure planning
  problems for gtopt
- [INPUT_DATA.md](../../INPUT_DATA.md) — Complete reference for gtopt JSON
  input format and all available options
- [MATHEMATICAL_FORMULATION.md](../MATHEMATICAL_FORMULATION.md) — The LP/MIP
  formulation that gtopt assembles from igtopt's output
- [pp2gtopt.md](pp2gtopt.md) — Converting pandapower cases to gtopt (an
  alternative input path)
- [gtopt-compare.md](gtopt-compare.md) — Validating gtopt results
  against pandapower DC OPF
- [plp2gtopt.md](plp2gtopt.md) — Converting PLP cases to gtopt format
