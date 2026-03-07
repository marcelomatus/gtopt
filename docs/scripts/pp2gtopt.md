# pp2gtopt — pandapower to gtopt Converter

Converts a **pandapower** network to a self-contained gtopt JSON file, ready
to be solved directly with the `gtopt` binary or submitted to
**gtopt\_guisrv** / **gtopt\_websrv**.

---

## Overview

[pandapower](https://www.pandapower.org/) is a widely used open-source Python
library for power system modelling and analysis.  It provides a rich collection
of standard IEEE test networks and supports importing from MATPOWER and other
formats.

`pp2gtopt` bridges pandapower and gtopt by converting any pandapower network —
either a built-in test case or a file saved to disk — into the gtopt JSON
format.  The conversion handles:

- Bus definitions with voltage levels.
- Generator linearisation (quadratic cost → single linear segment).
- Transmission line impedance conversion (Ohm → per-unit reactance).
- Transformer modelling as lossless lines.
- Demand extraction from pandapower load tables.

The output JSON is a complete, single-file gtopt case with inline data (no
external Parquet files needed), suitable for single-snapshot dispatch or
DC OPF studies.

---

## Installation

Install as part of the gtopt scripts package:

```bash
# Standard install
pip install ./scripts

# Editable install with dev dependencies
pip install -e "./scripts[dev]"
```

pandapower is a required dependency and is installed automatically.

```bash
pp2gtopt --version
pp2gtopt --help
```

### Dependencies

| Package      | Purpose                           |
|--------------|-----------------------------------|
| `pandapower` | Network data and IEEE test cases  |
| `numpy`      | Numerical computations            |
| `pandas`     | DataFrame handling                |

---

## Quick Start

```bash
# Convert the default IEEE 30-bus network → ieee30b.json
pp2gtopt

# Convert a specific built-in test network
pp2gtopt -n case57 -o ieee57b.json

# List all available built-in networks
pp2gtopt --list-networks

# Convert a pandapower JSON file
pp2gtopt -f my_network.json -o my_case.json

# Convert a MATPOWER case file
pp2gtopt -f case39.m -o case39.json

# Convert an Excel workbook
pp2gtopt -f network.xlsx -o network.json
```

---

## CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| `-f, --file FILE` | — | pandapower network file (`.json`, `.xlsx`/`.xls`, `.m`); mutually exclusive with `-n` |
| `-n, --network NAME` | `ieee30b` | Built-in pandapower test network name; mutually exclusive with `-f` |
| `-o, --output FILE` | `<stem>.json` | Output JSON file path |
| `--list-networks` | — | Print all available built-in network names and exit |
| `-V, --version` | — | Print version and exit |

> **Note**: `-f` and `-n` are mutually exclusive.  If neither is given, the
> default `ieee30b` network is used.

---

## Available Built-in Networks

| Network name | pandapower function | Buses | Description |
|---|---|---|---|
| `ieee30b` *(default)* | `case_ieee30` | 30 | IEEE 30-bus (Washington) |
| `case4gs` | `case4gs` | 4 | 4-bus Glover–Sarma |
| `case5` | `case5` | 5 | 5-bus example |
| `case6ww` | `case6ww` | 6 | 6-bus Wood–Wollenberg |
| `case9` | `case9` | 9 | IEEE 9-bus (Anderson–Fouad) |
| `case14` | `case14` | 14 | IEEE 14-bus |
| `case33bw` | `case33bw` | 33 | 33-bus Baran–Wu distribution |
| `case57` | `case57` | 57 | IEEE 57-bus |
| `case118` | `case118` | 118 | IEEE 118-bus |

Use `pp2gtopt --list-networks` to see the full list in your installed version.

---

## Input from File (`-f / --file`)

The `-f` flag loads any pandapower network saved to disk.  The format is
auto-detected from the file extension:

| Extension | Format | Produced by |
|-----------|--------|-------------|
| `.json` | pandapower JSON | `pandapower.to_json()` |
| `.xlsx` / `.xls` | pandapower Excel | `pandapower.to_excel()` |
| `.m` | MATPOWER case file | MATPOWER / Octave |

The output JSON system `name` is derived from the file stem (e.g. `case39.m`
→ `"case39"`).

---

## How the Conversion Works

### 1. Bus mapping

Each pandapower bus is mapped to a gtopt `Bus` with:
- `uid` from the pandapower bus index.
- `name` from `bus.name` (or generated if absent).
- `kv` (voltage level in kV) from `bus.vn_kv`.
- The slack bus is identified and its `reference_theta` set to `true`.

### 2. Per-unit reactance

pandapower stores line impedances in Ohm.  The converter transforms them to
per-unit values using the system base MVA and the bus voltage:

```
x_pu = x_ohm / Z_base
Z_base = (vn_kv)² / sn_mva
```

The resulting reactance is written to the gtopt `Line.reactance` field,
which is what the DC OPF (Kirchhoff mode) requires.

### 3. Generator cost linearisation

pandapower supports polynomial cost functions (quadratic: `cp0 + cp1·P + cp2·P²`).
Since gtopt uses a linear cost model, the converter extracts the **linear
coefficient `cp1`** ($/MWh) as the gtopt `gcost`.  Quadratic and constant
terms are dropped — this is appropriate for DC OPF where the linear
approximation is standard.

If no cost data is available, `gcost` defaults to `0.0`.

### 4. Transformer modelling

pandapower transformers are converted to **lossless transmission lines** with
the transformer's short-circuit reactance as the line reactance.  Tap ratio
effects on reactance are included.  Thermal limits (`max_i_ka`) are disabled
(set to infinity) to avoid artificial constraints.

### 5. Load / demand extraction

Each pandapower load entry becomes a gtopt `Demand` with:
- `bus` referencing the pandapower bus index.
- `lmax` set to the load's active power `p_mw`.

---

## Output JSON Structure

The output is a standard gtopt JSON file with inline arrays:

```json
{
  "system": {
    "name": "case57",
    "bus_array": [ ... ],
    "generator_array": [ ... ],
    "demand_array": [ ... ],
    "line_array": [ ... ]
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1.0}],
    "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1.0}]
  },
  "options": {
    "use_kirchhoff": true,
    "scale_objective": 1000,
    "demand_fail_cost": 1000
  }
}
```

The simulation is a single snapshot (1 block, 1 stage, 1 scenario) suitable
for static DC OPF dispatch.

---

## Example: Converting IEEE 57-Bus and Verifying with gtopt

```bash
# Step 1: Convert the IEEE 57-bus network
pp2gtopt -n case57 -o ieee57b.json

# Step 2: Solve with gtopt (single-snapshot DC OPF)
gtopt ieee57b.json

# Step 3: Check the solution
cat output/solution.csv
# → status=0 (optimal), obj_value=... (total cost / scale_objective)

# Step 4: Inspect generator dispatch
cat output/Generator/generation_sol.csv

# Step 5: Check bus marginal prices (LMPs)
cat output/Bus/balance_dual.csv

# Step 6 (optional): Cross-validate against pandapower DC OPF
# Note: the compare-pandapower case name 'ieee_57b' differs from the
# pp2gtopt built-in network name 'case57' — they refer to the same network
compare-pandapower --case ieee_57b --gtopt-output output/
```

---

## See Also

- [SCRIPTS.md](../../SCRIPTS.md) — Overview of all gtopt Python scripts
- [compare-pandapower](compare-pandapower.md) — Validate gtopt results against
  pandapower DC OPF
- [plp2gtopt](plp2gtopt.md) — Convert PLP cases to gtopt
- [PLANNING\_GUIDE.md](../../PLANNING_GUIDE.md) — Guide to gtopt planning concepts
