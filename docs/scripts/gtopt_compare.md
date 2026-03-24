# gtopt_compare — DC OPF Validation Tool

Validates gtopt solver results by comparing them against **pandapower's DC
Optimal Power Flow** reference implementation.

---

## Overview

After solving a case with gtopt, it is important to verify that the solution is
correct.  `gtopt_compare` rebuilds the same network in pandapower, runs
its DC OPF solver, and compares the results against gtopt's output CSV files.

This tool is primarily used for:

- **Regression testing**: ensuring that code changes in gtopt do not alter
  dispatch results for known benchmark cases.
- **Cross-validation**: confirming that gtopt's LP formulation and solver
  produce results consistent with an independent reference implementation.
- **Debugging**: pinpointing which buses or generators differ when a test
  fails.

The tool supports several standard test networks, from a simple 1-bus case to
the IEEE 57-bus standard, as well as a battery storage case with 24 hourly
blocks.

---

## Installation

Install as part of the gtopt scripts package:

```bash
# Standard install
pip install ./scripts

# Editable install with dev dependencies
pip install -e "./scripts[dev]"
```

### Dependencies

| Package      | Purpose                                    |
|--------------|--------------------------------------------|
| `pandapower` | Reference DC OPF solver                    |
| `numpy`      | Numerical comparison                       |
| `pandas`     | CSV reading and DataFrame operations       |

---

## Usage

```bash
# Compare gtopt output for the IEEE 4-bus case
gtopt_compare --case ieee_4b_ori --gtopt-output output/

# Compare the IEEE 30-bus case with custom tolerances
gtopt_compare --case ieee30b --gtopt-output output/ --tol 0.5 --tol-lmp 0.05

# Save the pandapower network for reuse
gtopt_compare --case case57 --save-pandapower-file case57_pp.json

# Load a previously saved network (skip rebuild)
gtopt_compare --case case57 --pandapower-file case57_pp.json --gtopt-output output/
```

---

## CLI Options

| Flag | Default | Description |
|------|---------|-------------|
| `--case NAME` | — | Test case name (required); see supported cases below |
| `--gtopt-output DIR` | — | Directory containing gtopt CSV output files |
| `--pandapower-file FILE` | — | Load the pandapower network from this JSON file instead of rebuilding |
| `--save-pandapower-file FILE` | — | Save the built pandapower network to this JSON file and exit |
| `--tol MW` | `1.0` | Generation / total-power tolerance in MW |
| `--tol-lmp $/MWh` | `0.1` | Bus LMP (Locational Marginal Price) tolerance in $/MWh |

---

## Supported Cases

| Case name | Buses | Generators | Blocks | Description |
|-----------|-------|------------|--------|-------------|
| `s1b` | 1 | 2 | 1 | Single-bus with 2 generators and 1 load |
| `ieee_4b_ori` | 4 | 2 | 1 | 4-bus Grainger & Stevenson |
| `ieee30b` | 30 | 6 | 1 | IEEE 30-bus Washington |
| `ieee_57b` | 57 | 7 | 1 | IEEE 57-bus standard |
| `bat_4b_24` | 4 | 2 + solar | 24 | 4-bus with solar profile and battery storage (24 hourly blocks) |

---

## What It Compares

### 1. Generator dispatch (MW)

For each generator, compares the active power output from gtopt
(`Generator/generation_sol.csv`) against the pandapower DC OPF result.
Differences exceeding `--tol` MW are flagged.

### 2. Objective cost ($)

Compares the total generation cost from `solution.csv` against the sum of
`gcost × generation` from pandapower.  The comparison accounts for
`scale_objective` (gtopt divides its objective by this factor).

### 3. Bus LMPs ($/MWh)

Compares the Locational Marginal Prices from gtopt (`Bus/balance_dual.csv`)
against the bus shadow prices from pandapower's DC OPF.  Differences
exceeding `--tol-lmp` $/MWh are flagged.

### Battery case handling (`bat_4b_24`)

For the battery case, the tool additionally:

- Reads battery charge/discharge power from `Battery/fout_sol.csv` and
  `Battery/finp_sol.csv`.
- Adjusts the effective load at each bus per block to account for battery
  charging and discharging.
- Validates generator dispatch consistency block by block (24 comparisons).

---

## Tolerance Parameters

| Parameter | Default | Units | Description |
|-----------|---------|-------|-------------|
| `--tol` | `1.0` | MW | Maximum acceptable difference in generation dispatch per generator |
| `--tol-lmp` | `0.1` | $/MWh | Maximum acceptable difference in bus LMP |

These defaults are appropriate for most cases.  Tighter tolerances may be
needed for high-precision studies; looser tolerances may be required for large
cases where small numerical differences in the LP solver accumulate.

---

## Exit Codes

| Code | Meaning |
|------|---------|
| `0` | **PASS** — all comparisons within tolerance |
| `1` | **FAIL** — one or more comparisons exceeded tolerance |
| `2` | **ERROR** — runtime error (missing files, invalid case name, etc.) |

---

## Files Read from gtopt Output

| File path | Contents used |
|-----------|--------------|
| `Generator/generation_sol.csv` | Generator active power dispatch (MW) |
| `Bus/balance_dual.csv` | Bus shadow prices / LMPs ($/MWh) |
| `solution.csv` | Objective value, solver status |
| `Battery/fout_sol.csv` | Battery discharge power (battery cases only) |
| `Battery/finp_sol.csv` | Battery charge power (battery cases only) |

---

## Example Workflow

```bash
# Step 1: Convert an IEEE 30-bus network from pandapower
pp2gtopt -n ieee30b -o ieee30b.json

# Step 2: Solve with gtopt
gtopt ieee30b.json

# Step 3: Validate against pandapower
gtopt_compare --case ieee30b --gtopt-output output/
# Expected: PASS (all differences within default tolerances)

# Step 4 (optional): Save pandapower network for faster re-runs
gtopt_compare --case ieee30b --save-pandapower-file ieee30b_pp.json
gtopt_compare --case ieee30b --pandapower-file ieee30b_pp.json --gtopt-output output/
```

---

## See Also

- [SCRIPTS.md](../scripts-guide.md) — Overview of all gtopt Python scripts
- [pp2gtopt](pp2gtopt.md) — Convert pandapower networks to gtopt JSON
- [PLANNING\_GUIDE.md](../planning-guide.md) — Guide to gtopt planning concepts
- [USAGE.md](../usage.md) — Running gtopt and interpreting output files
