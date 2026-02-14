# gtopt Usage Guide

Detailed usage instructions, examples, and reference for the gtopt solver.

> **Note**: This guide assumes gtopt is already installed. If you need build instructions, see [BUILDING.md](BUILDING.md). For detailed input data structure specifications, see [INPUT_DATA.md](INPUT_DATA.md).

## Table of Contents

- [Basic Usage](#basic-usage)
- [Command-Line Reference](#command-line-reference)
- [System Configuration File](#system-configuration-file)
- [Input Data Directory](#input-data-directory)
- [Running the Sample Case](#running-the-sample-case)
- [Output Files](#output-files)
- [Advanced Usage](#advanced-usage)
- [Batch Execution](#batch-execution)

## Basic Usage

The simplest way to run gtopt is to pass a system JSON file:

```bash
gtopt system_config.json
```

The system file defines the power system (generators, demands, buses, lines),
the simulation parameters (stages, blocks, scenarios), and solver options. By
default, input data is read from the directory specified inside the JSON and
output is written to the current directory.

### Common patterns

```bash
# Run with a specific output directory
gtopt system_c0.json --output-directory results/

# Run with a separate input directory
gtopt --system-file config.json --input-directory data/

# Run quietly (no log output)
gtopt system_c0.json --quiet

# Run with verbose logging
gtopt system_c0.json --verbose

# Output results in Parquet format instead of CSV
gtopt system_c0.json --output-format parquet
```

## Command-Line Reference

System files can be passed as positional arguments or with `-s`/`--system-file`.
Multiple system files can be provided and will be merged.

| Short | Long Flag | Argument | Description |
| ----- | --------- | -------- | ----------- |
| `-h` | `--help` | | Show help message and exit |
| `-V` | `--version` | | Show program version and exit |
| `-v` | `--verbose` | | Activate maximum verbosity (trace-level logging) |
| `-q` | `--quiet` | `[=arg]` | Suppress all log output to stdout |
| `-s` | `--system-file` | `arg` | System JSON file(s) to process (also accepted as positional args) |
| `-D` | `--input-directory` | `arg` | Override the input data directory |
| `-F` | `--input-format` | `arg` | Input data format: `parquet`, `csv` |
| `-d` | `--output-directory` | `arg` | Directory for output files (created if needed) |
| `-f` | `--output-format` | `arg` | Output format: `parquet`, `csv` |
| `-C` | `--compression-format` | `arg` | Parquet compression: `uncompressed`, `gzip`, `zstd`, `lzo` |
| `-b` | `--use-single-bus` | `[=arg]` | Use single-bus mode (ignore network topology) |
| `-k` | `--use-kirchhoff` | `[=arg]` | Use Kirchhoff (DC power flow) mode |
| `-n` | `--use-lp-names` | `[=arg]` | Use named rows/columns in LP (0=off, 1=names, 2=names+map) |
| `-l` | `--lp-file` | `arg` | Save the LP model to a file |
| `-j` | `--json-file` | `arg` | Save the merged system configuration to a JSON file |
| `-e` | `--matrix-eps` | `arg` | Epsilon for matrix sparsity (coefficients below this are zero) |
| `-c` | `--just-create` | `[=arg]` | Build the LP model and exit without solving |
| `-p` | `--fast-parsing` | `[=arg]` | Use fast (non-strict) JSON parsing |

## System Configuration File

The system JSON file is the main input to gtopt. It has three top-level
sections:

```json
{
  "options": { ... },
  "simulation": { ... },
  "system": { ... }
}
```

### Options

Solver and I/O settings:

```json
"options": {
  "annual_discount_rate": 0.1,
  "use_lp_names": true,
  "output_format": "csv",
  "input_format": "parquet",
  "input_directory": "system_c0",
  "use_single_bus": false,
  "use_kirchhoff": true,
  "demand_fail_cost": 1000,
  "scale_objective": 1000
}
```

| Field | Type | Description |
|-------|------|-------------|
| `annual_discount_rate` | float | Discount rate for investment costs |
| `use_lp_names` | bool | Use named variables in LP formulation |
| `output_format` | string | Output format: `"csv"` or `"parquet"` |
| `input_format` | string | Input data format: `"csv"` or `"parquet"` |
| `input_directory` | string | Path to the data directory (relative to the JSON file) |
| `use_single_bus` | bool | Ignore network topology |
| `use_kirchhoff` | bool | Use DC power flow (Kirchhoff's laws) |
| `demand_fail_cost` | float | Penalty cost for unserved demand ($/MWh) |
| `scale_objective` | float | Objective function scaling factor |

### Simulation

Defines the temporal structure of the optimization:

```json
"simulation": {
  "block_array": [
    { "uid": 1, "duration": 1 },
    { "uid": 2, "duration": 2 },
    { "uid": 3, "duration": 3 }
  ],
  "stage_array": [
    { "uid": 1, "first_block": 0, "count_block": 1, "active": 1 },
    { "uid": 2, "first_block": 1, "count_block": 1, "active": 1 },
    { "uid": 3, "first_block": 2, "count_block": 1, "active": 1 }
  ],
  "scenario_array": [
    { "uid": 1, "probability_factor": 1 }
  ]
}
```

- **Blocks**: time subdivisions within a stage (e.g., peak/off-peak hours),
  each with a `duration` in hours.
- **Stages**: planning periods (e.g., years), referencing a range of blocks.
- **Scenarios**: stochastic scenarios with probability weights.

### System

Defines the physical power system components:

```json
"system": {
  "name": "system_c0",
  "bus_array": [
    { "uid": 1, "name": "b1" }
  ],
  "generator_array": [
    {
      "uid": 1, "name": "g1", "bus": "b1",
      "gcost": 100, "capacity": 20,
      "expcap": null, "expmod": null,
      "annual_capcost": null
    }
  ],
  "demand_array": [
    {
      "uid": 1, "name": "d1", "bus": "b1",
      "lmax": "lmax",
      "capacity": 0, "expcap": 20, "expmod": 10,
      "annual_capcost": 8760
    }
  ]
}
```

**Component types:**

| Component | Key Fields | Description |
|-----------|-----------|-------------|
| Bus | `uid`, `name` | Network node |
| Generator | `uid`, `name`, `bus`, `gcost`, `capacity` | Generation unit with cost and capacity |
| Demand | `uid`, `name`, `bus`, `lmax`, `capacity` | Load with optional expansion |
| Line | `uid`, `name`, `bus_from`, `bus_to`, `capacity` | Transmission line (if multi-bus) |

- **`gcost`**: generation cost ($/MWh).
- **`capacity`**: existing installed capacity (MW).
- **`expcap`**: maximum expansion capacity (MW), `null` if no expansion allowed.
- **`expmod`**: expansion module size (MW).
- **`annual_capcost`**: annualized capital cost of expansion ($/MW/year).
- **`lmax`**: references a data file in the input directory for time-varying load.

## Input Data Directory

Time-varying parameters (e.g., load profiles) are stored as data files in the
input directory, organized by component type:

```
system_c0/
└── Demand/
    └── lmax.parquet      # Load profile for demand "d1"
```

When a field like `"lmax": "lmax"` appears in the system JSON, gtopt looks for
a file named `lmax.parquet` (or `lmax.csv`) in the
`<input_directory>/<ComponentType>/` subdirectory.

Data files contain columns indexed by `(scenario, stage, block)` with one
column per component UID.

## Running the Sample Case

The repository includes a sample case in `cases/c0/` that models a simple
single-bus system with one generator and one demand with capacity expansion.

### File layout

```
cases/c0/
├── system_c0.json          # System configuration
├── system_c0/              # Input data directory
│   └── Demand/
│       └── lmax.parquet    # Load profile
├── output/                 # Reference output (for test validation)
│   ├── solution.csv
│   ├── Generator/
│   │   ├── generation_sol.csv
│   │   └── generation_cost.csv
│   ├── Demand/
│   │   ├── load_sol.csv
│   │   ├── fail_sol.csv
│   │   ├── capainst_sol.csv
│   │   └── ...
│   └── Bus/
│       └── balance_dual.csv
└── planning_c0.json        # Alternative planning file
```

### Run the case

```bash
cd cases/c0
gtopt system_c0.json
```

Or specify a separate output directory:

```bash
gtopt system_c0.json --output-directory /tmp/c0_results
```

### Expected output

On success, gtopt logs its progress and exits with code 0:

```
[info] starting gtopt 1.0
[info] parsing input file system_c0.json
[info] parsing all json files 0.001s
[info] creating lp 0.002s
[info] planning  0.010s
[info] writing output  0.001s
```

The solver produces a `solution.csv` summary:

```
obj_value,23.163424133184083
    kappa,1
   status,0
```

- **`obj_value`**: total optimized cost.
- **`kappa`**: number of iterations.
- **`status`**: `0` = optimal solution found.

## Output Files

Output is organized by component type. Each component produces solution
(`_sol`) and cost (`_cost`) files, plus dual values (`_dual`) for constraints.

### Solution summary

`solution.csv` — overall optimization result (objective value, status).

### Generator output

| File | Description |
|------|-------------|
| `Generator/generation_sol.csv` | Generation dispatch per (scenario, stage, block) |
| `Generator/generation_cost.csv` | Generation cost per (scenario, stage, block) |

Example `generation_sol.csv`:

```csv
"scenario","stage","block","uid:1"
1,1,1,10
1,2,2,15
1,3,3,8
1,4,4,20
1,5,5,20
```

Each row is a `(scenario, stage, block)` tuple. Column `uid:1` contains the
dispatch value (MW) for generator with UID 1.

### Demand output

| File | Description |
|------|-------------|
| `Demand/load_sol.csv` | Served load per (scenario, stage, block) |
| `Demand/load_cost.csv` | Load cost per (scenario, stage, block) |
| `Demand/fail_sol.csv` | Unserved demand (load shedding) per (scenario, stage, block) |
| `Demand/fail_cost.csv` | Cost of unserved demand |
| `Demand/capainst_sol.csv` | Installed capacity per stage |
| `Demand/capainst_cost.csv` | Investment cost of installed capacity |
| `Demand/capainst_dual.csv` | Dual of capacity installation constraint |
| `Demand/expmod_sol.csv` | Expansion module decisions per stage |
| `Demand/expmod_cost.csv` | Cost of expansion modules |
| `Demand/capacost_sol.csv` | Capacity cost allocation |
| `Demand/capacost_cost.csv` | Capital cost values |
| `Demand/capacost_dual.csv` | Dual of capacity cost constraint |
| `Demand/capacity_dual.csv` | Dual of capacity limit constraint |
| `Demand/balance_dual.csv` | Dual of demand balance constraint |

### Bus output

| File | Description |
|------|-------------|
| `Bus/balance_dual.csv` | Nodal price (dual of bus balance constraint) per (scenario, stage, block) |

Example `Bus/balance_dual.csv`:

```csv
"scenario","stage","block","uid:1"
1,1,1,100
1,2,2,100
1,3,3,100
1,4,4,100
1,5,5,100
```

The bus balance dual represents the marginal cost of energy at each bus
(locational marginal price).

## Advanced Usage

### Output in Parquet format

For large cases, Parquet format is more compact and faster to read:

```bash
gtopt system_c0.json --output-format parquet
gtopt system_c0.json --output-format parquet --compression-format gzip
```

### Export the LP model

Save the linear programming formulation for debugging or external solvers:

```bash
gtopt system_c0.json --lp-file model.lp
```

### Build without solving

Create the LP and exit (useful for validation or LP export):

```bash
gtopt system_c0.json --lp-file model.lp --just-create
```

### Single-bus mode

Ignore network topology and solve as a single-bus (copper-plate) system:

```bash
gtopt system_c0.json --use-single-bus
```

### Kirchhoff (DC power flow) mode

Enable DC power flow constraints for multi-bus systems:

```bash
gtopt system_c0.json --use-kirchhoff
```

### Merging multiple system files

Multiple JSON files can be passed and will be merged into a single system.
This allows separating system definition from simulation parameters:

```bash
gtopt base_system.json additional_generators.json scenario_data.json
```

### Save merged configuration

Export the merged system configuration as a single JSON file:

```bash
gtopt base.json extensions.json --json-file merged_system.json
```

### Override input directory

If the data files are in a different location than specified in the JSON:

```bash
gtopt system.json --input-directory /data/case_inputs
```

### Control logging

```bash
# Maximum verbosity (trace level)
gtopt system.json --verbose

# No output at all
gtopt system.json --quiet

# Use spdlog environment variable for fine-grained control
SPDLOG_LEVEL=debug gtopt system.json
```

## Batch Execution

### Shell script

```bash
#!/bin/bash
# Run all JSON cases in a directory
for f in cases/*/system_*.json; do
  echo "=== Running $f ==="
  dir=$(dirname "$f")
  name=$(basename "$dir")
  gtopt "$f" --output-directory "results/$name"
done
```

### Python

```python
import subprocess
from pathlib import Path

def run_case(system_file, output_dir=None):
    """Run a single gtopt case and return the exit code."""
    cmd = ["gtopt", str(system_file)]
    if output_dir:
        cmd += ["--output-directory", str(output_dir)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"FAILED: {system_file}")
        print(result.stderr)
    return result.returncode

def run_all_cases(cases_dir):
    """Run all cases under a directory."""
    for system_file in Path(cases_dir).glob("*/system_*.json"):
        case_name = system_file.parent.name
        run_case(system_file, output_dir=f"results/{case_name}")

if __name__ == "__main__":
    run_all_cases("cases/")
```
