# gtopt Usage Guide

Detailed usage instructions, examples, and reference for the gtopt solver.

> **Note**: This guide assumes gtopt is already installed. If you need build instructions, see [Building Guide](../BUILDING.md). For detailed input data structure specifications, see [Input Data Reference](input-data.md).

## Table of Contents

- [Basic Usage](#basic-usage)
- [Command-Line Reference](#command-line-reference)
- [Configuration File (`.gtopt.conf`)](#configuration-file-gtoptconf)
- [System Configuration File](#system-configuration-file)
- [Input Data Directory](#input-data-directory)
- [Running the Sample Case](#running-the-sample-case)
- [Output Files](#output-files)
- [Advanced Usage](#advanced-usage)
- [Troubleshooting Solver Issues](#troubleshooting-solver-issues)
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
gtopt system_c0.json --set output_directory=results/

# Run with a separate input directory
gtopt --system-file config.json --set input_directory=data/

# Run quietly (no log output)
gtopt system_c0.json --quiet

# Run with verbose logging
gtopt system_c0.json --verbose

# Output results in Parquet format instead of CSV
gtopt system_c0.json --set output_format=parquet
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
| `-S` | `--stats` | | Print pre-solve system statistics and post-solve results summary |
| `-s` | `--system-file` | `arg` | System JSON file(s) to process (also accepted as positional args) |
| `-n` | `--lp-names-level` | `[=arg]` | LP naming level: `0`/`minimal`, `1`/`only_cols`, `2`/`cols_and_rows` (see below) |
| `-l` | `--lp-file` | `arg` | Save the LP model to a file |
| `-j` | `--json-file` | `arg` | Save the merged system configuration to a JSON file |
| `-e` | `--matrix-eps` | `arg` | Epsilon for matrix sparsity (coefficients below this are zero) |
| `-c` | `--lp-build` | `[=arg]` | Build the LP model and exit without solving |
| `-p` | `--fast-parsing` | `[=arg]` | Use fast (non-strict) JSON parsing |
| `-J` | `--check-json` | `[=arg]` | Warn about JSON fields not recognized by the schema |
| `-T` | `--trace-log` | `arg` | Write trace-level log messages to this file |
| | `--solver` | `arg` | LP solver backend: `clp`, `cbc`, `cplex`, `highs` (auto-detected by default) |
| | `--solvers` | | List available LP solver backends and exit |
| | `--check-solvers` | `[=solver]` | Run the solver test suite against all (or a named) solver, then exit |
| | `--method` | `arg` | Planning method: `monolithic`, `sddp`, `cascade` |
| | `--demand-fail-cost` | `arg` | Penalty $/MWh for unserved demand |
| | `--scale-objective` | `arg` | Objective function scaling factor |
| | `--sddp-num-apertures` | `arg` | SDDP backward-pass aperture count: `0`=disabled, `-1`=all, `N`=first N scenarios |
| | `--recover` | `[=arg]` | Enable recovery from a previous SDDP run (loads cuts and state variables) |
| | `--set` | `key=value` | Set any planning option (see below) |

> **`--set` key=value**: The recommended way to pass planning options
> from the CLI. Any option that can appear in the JSON `"options"`
> section can be set via `--set`. Nested keys use dot notation.
> Examples:
>
> ```bash
> gtopt case.json --set output_directory=results/
> gtopt case.json --set use_single_bus=true
> gtopt case.json --set solver_options.algorithm=barrier
> gtopt case.json --set solver_options.threads=4
> gtopt case.json --set sddp_options.max_iterations=300
> gtopt case.json --set sddp_options.convergence_tol=1e-5
> gtopt case.json --set input_directory=data/ --set input_format=parquet
> gtopt case.json --set lp_debug=true --set log_directory=logs
> ```
>
> **Deprecated aliases** (still work, emit a warning): `-b`
> (`--use-single-bus`), `-k` (`--use-kirchhoff`), `-D`
> (`--input-directory`), `-F` (`--input-format`), `-d`
> (`--output-directory`), `-f` (`--output-format`), `-C`
> (`--output-compression`), `--algorithm`, `--threads`,
> `--sddp-max-iterations`, `--sddp-min-iterations`,
> `--sddp-convergence-tol`, `--sddp-elastic-penalty`,
> `--sddp-elastic-mode`, `--sddp-cut-coeff-mode`,
> `--log-directory`, `--cut-directory`, `--lp-debug`,
> `--lp-compression`, `--lp-coeff-ratio`.

## Configuration File (`.gtopt.conf`)

The C++ binary reads default option values from an INI configuration file.
CLI flags always take precedence over config file values, which in turn take
precedence over JSON file values.

### Search order

1. `$GTOPT_CONFIG` environment variable (exact path)
2. `./.gtopt.conf` (current working directory)
3. `~/.gtopt.conf` (home directory)

### Format

```ini
[gtopt]
solver              = highs
algorithm           = barrier
threads             = 4
output-format       = parquet
output-compression  = zstd
sddp-max-iterations = 200
sddp-convergence-tol = 1e-4
use-single-bus      = false
lp-debug            = false
```

### Supported keys

All keys use kebab-case matching the CLI long flags (without `--`):

| Key | Type | Description |
|-----|------|-------------|
| `solver` | string | LP solver backend |
| `algorithm` | string | LP algorithm (`primal`, `dual`, `barrier`) |
| `threads` | int | Number of solver threads |
| `output-format` | string | Output format (`parquet`, `csv`) |
| `output-compression` | string | Compression codec |
| `sddp-max-iterations` | int | Maximum SDDP iterations |
| `sddp-min-iterations` | int | Minimum SDDP iterations |
| `sddp-convergence-tol` | float | SDDP convergence tolerance |
| `use-single-bus` | bool | Single-bus mode |
| `lp-debug` | bool | Save LP debug files |
| `demand-fail-cost` | float | Penalty for unserved demand |
| `scale-objective` | float | Objective scaling factor |
| `method` | string | Planning method |
| `input-directory` | string | Input data directory |
| `output-directory` | string | Output directory |

### Precedence

Values are resolved in this order (highest priority first):

1. **CLI flags** (`--solver highs`, `--set key=value`)
2. **Config file** (`~/.gtopt.conf` `[gtopt]` section)
3. **JSON files** (`"options"` section in planning JSON)
4. **Built-in defaults** (compiled into the binary)

> **Note**: The Python scripts (`run_gtopt`, `gtopt_check_output`, etc.) also
> read `~/.gtopt.conf` but use different sections (`[global]`, `[run_gtopt]`,
> etc.).  The `[gtopt]` section is used exclusively by the C++ binary.  See
> [Scripts Guide](scripts-guide.md#gtopt_config) for Python configuration.

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
  "output_format": "csv",
  "input_format": "parquet",
  "input_directory": "system_c0",
  "use_single_bus": false,
  "use_kirchhoff": true,
  "demand_fail_cost": 1000,
  "scale_objective": 1000,
  "lp_build_options": {
    "names_level": "only_cols"
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `lp_build_options.names_level` | string/int | LP naming level (see [LP naming levels](#lp-naming-levels)) |
| `output_format` | string | Output format: `"csv"` or `"parquet"` |
| `input_format` | string | Input data format: `"csv"` or `"parquet"` |
| `input_directory` | string | Path to the data directory (relative to the JSON file) |
| `use_single_bus` | bool | Ignore network topology |
| `use_kirchhoff` | bool | Use DC power flow (Kirchhoff's laws) |
| `demand_fail_cost` | float | Penalty cost for unserved demand ($/MWh) |
| `scale_objective` | float | Objective function scaling factor |
| `log_directory` | string | Directory for log and error LP files (default: `"logs"`) |
| `lp_debug` | bool | Save LP debug files to `log_directory` before solving (see below) |

#### `lp_debug` — LP debug file output

When `lp_debug` is set to `true`, gtopt saves the LP model as a `.lp` text
file to the `log_directory` before solving.  This is useful for diagnosing
unexpected solver behaviour (infeasibility, unboundedness, suspicious
objective values).

- **Monolithic solver**: one file per `(scene, phase)` →
  `logs/gtopt_lp_<scene>_<phase>.lp`
- **SDDP solver**: one file per `(iteration, scene, phase)` →
  `logs/gtopt_iter_<iter>_<scene>_<phase>.lp`

If `output_compression` is set to a codec (e.g. `"zstd"`, `"gzip"`, or any
value other than `"uncompressed"`), the files are compressed and the originals
are removed.  The default codec is `"zstd"`.  Compression runs asynchronously
so it does not add latency to the solve.

Via JSON:
```json
{ "options": { "lp_debug": true, "log_directory": "logs", "output_compression": "zstd" } }
```

Via CLI:
```
gtopt mycase --set lp_debug=true --set log_directory=logs \
  --set output_compression=zstd
```

> **Important**: LP file output (`--lp-file`, `--lp-debug`, and error LP files)
> requires `names_level >= only_cols`.  At the default `minimal` level, row
> names are not populated and `write_lp()` will fail silently.  Always set
> `--lp-names-level only_cols` (or higher) when you need LP file output.

#### LP naming levels

The `names_level` option (CLI: `--lp-names-level`, JSON:
`lp_build_options.names_level`) controls how much naming metadata gtopt tracks
during LP assembly.  Higher levels consume more memory but enable richer
diagnostics.

| Level | Name | Column names | Row names | Name maps | LP file output | Notes |
|-------|------|:------------:|:---------:|:---------:|:--------------:|-------|
| 0 | `minimal` | state vars only | no | no | **no** | Default. Smallest footprint. |
| 1 | `only_cols` | all | yes | yes | **yes** | Required for `--lp-file`, `--lp-debug`, and error LP output. |
| 2 | `cols_and_rows` | all | yes | yes | **yes** | Same as 1, plus warns on duplicate names. Useful for catching formulation bugs. |

**CLI examples:**
```bash
# Save LP with named variables and constraints
gtopt mycase --lp-file model --lp-names-level only_cols

# Debug LP files with full names
gtopt mycase --lp-debug --lp-names-level 1

# Just pass -n (implicit value: only_cols)
gtopt mycase --lp-file model -n
```

**JSON example:**
```json
{
  "options": {
    "lp_build_options": {
      "names_level": "only_cols"
    }
  }
}
```

### Simulation

Defines the temporal structure of the optimization:

```json
"simulation": {
  "annual_discount_rate": 0.1,
  "boundary_cuts_file": "boundary_cuts.csv",
  "boundary_cuts_valuation": "end_of_horizon",
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

- **`annual_discount_rate`**: discount rate for multi-stage CAPEX.
- **`boundary_cuts_file`**: CSV with boundary (future-cost) cuts.
- **`boundary_cuts_valuation`**: `"end_of_horizon"` (default) or
  `"present_value"`.
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
gtopt system_c0.json --set output_directory=/tmp/c0_results
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
scene,phase,status,status_name,obj_value,kappa,gap,gap_change
0,0,0,optimal,23.163424,1,0,1.0
```

- **`obj_value`**: total optimized cost.
- **`kappa`**: number of iterations.
- **`status`**: `0` = optimal solution found.
- **`gap`**: final SDDP convergence gap (0 for monolithic solver).
- **`gap_change`**: relative gap change over the stationary window (1.0 if
  secondary criterion disabled or for monolithic solver).

## Output Files

Output is organized by component type. Each component produces solution
(`_sol`) and cost (`_cost`) files, plus dual values (`_dual`) for constraints.

### Solution summary

`solution.csv` — overall optimization result (objective value, status, SDDP
convergence metrics).

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
gtopt system_c0.json --set output_format=parquet
gtopt system_c0.json --set output_format=parquet --set output_compression=zstd
```

### Export the LP model

Save the linear programming formulation for debugging or external solvers:

```bash
gtopt system_c0.json --lp-file model.lp
```

### Build without solving

Create the LP and exit (useful for validation or LP export):

```bash
gtopt system_c0.json --lp-file model.lp --lp-build
```

### Single-bus mode

Ignore network topology and solve as a single-bus (copper-plate) system:

```bash
gtopt system_c0.json --set use_single_bus=true
```

### Kirchhoff (DC power flow) mode

Enable DC power flow constraints for multi-bus systems:

```bash
gtopt system_c0.json --set use_kirchhoff=true
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
gtopt system.json --set input_directory=/data/case_inputs
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

## Troubleshooting Solver Issues

This section covers common solver problems, their causes, and diagnostic
steps.

### Exit codes

gtopt uses the following exit codes:

| Exit code | Meaning |
|-----------|---------|
| `0` | Optimal solution found |
| `1` | Non-optimal solution (infeasible or abandoned, but no critical error) |
| `2` | Input error (missing file, invalid JSON, bad options) |
| `3` | Internal error (unexpected exception, solver crash) |

Check `solution.csv` (or `solution.parquet`) for the solver `status` field:
`0` = optimal, `1` = infeasible, `2` = unbounded.

### Infeasible model (status=1)

An infeasible model means no feasible solution exists given the constraints.

**Common causes:**

- **Demand exceeds generation capacity**: total demand in a block exceeds the
  sum of all available generation plus imports.  Check that generator
  `capacity` and `pmax` values are sufficient.
- **Missing bus connection**: a demand bus has no generator or line connecting
  it to the rest of the network.  Verify that every demand bus has at least
  one generator or transmission line.
- **Over-constrained reserves**: spinning reserve requirements that exceed
  available headroom.  Try reducing the reserve requirement or increasing
  `reserve_fail_cost` to allow some unserved reserve.
- **Tight physical bounds on state variables**: reservoir volume or battery
  SoC bounds that are inconsistent across phases (e.g., initial volume
  outside the `[emin, emax]` range).

**Diagnostic steps:**

1. Check `output/Demand/fail_sol.csv` -- if all values are zero but the model
   is infeasible, the issue is likely in network constraints rather than
   generation adequacy.
2. Set `demand_fail_cost` to a high value (e.g., 10000) to allow load
   shedding.  If the model becomes feasible, the infeasibility was caused by
   insufficient generation.
3. Enable LP debug files to inspect the LP formulation:
   ```json
   { "options": { "lp_debug": true, "log_directory": "logs" } }
   ```
4. Try `use_single_bus: true` to eliminate network constraints.  If the model
   becomes feasible, the issue is in the transmission network (missing lines,
   insufficient capacity).
5. For SDDP, check the `logs/` directory for `error_scene_*_phase_*.lp`
   files which indicate which scene/phase is infeasible.

### Unbounded model (status=2)

An unbounded model is rare and typically indicates missing variable bounds.

**Common causes:**

- A generator with negative `gcost` and no `pmax` upper bound.
- Missing bounds on voltage angle variables (`scale_theta` set to 0).
- Incorrect `scale_objective` value (e.g., 0 or negative).

**Diagnostic steps:**

1. Enable `lp_debug: true` and inspect the LP file for variables with
   infinite bounds and negative objective coefficients.
2. Verify that all generators have a finite `capacity` or `pmax`.
3. Check that `scale_objective` and `scale_theta` are positive numbers.

### Slow convergence (SDDP)

When the SDDP gap does not close within the expected number of iterations:

**Suggestions:**

- **Increase `max_iterations`**: the default of 100 may be insufficient for
  large problems.  Try 200--500.
- **Enable the stationary-gap criterion**: some problems converge to a
  non-zero gap plateau.  Set `stationary_tol` to a small positive value
  (e.g. `0.01` = 1% gap-change threshold) with `stationary_window` (default
  10) to declare convergence when the gap stops improving.  The solver will
  log `[CONVERGED]` with `stationary gap convergence` when this criterion
  triggers.
- **Adjust `elastic_penalty`**: if the elastic filter activates frequently,
  try increasing the penalty (e.g., `1e8`) to discourage elastic slack.
- **Try `cut_sharing_mode: "expected"`**: sharing cuts across scenes can
  accelerate convergence for stochastic problems.
- **Check variable scaling**: poorly scaled variables (e.g., reservoir volumes
  in m3 vs dam3) can cause numerical issues.  Use `variable_scales` to
  normalize large-valued state variables.
- **Try `elastic_mode: "multi_cut"`**: when `single_cut` mode produces
  weak cuts, the `multi_cut` mode adds per-slack bound cuts that can
  tighten the approximation faster.
- **Lower `convergence_tol`**: if the gap oscillates near the tolerance,
  try a slightly larger tolerance (e.g., `1e-3` instead of `1e-4`).
- **Check boundary cuts**: for problems where the planning horizon is too
  short, loading `boundary_cuts_file` can provide a better terminal
  approximation and speed convergence.
- **Monitor progress**: use the SDDP monitoring API (`api_enabled: true`)
  and the `sddp_monitor` script to visualize the convergence trajectory.
  Check the `gap_change` column in `solution.csv` to assess whether the
  gap has plateaued.

### Out of memory

When the LP is too large to fit in memory:

**Suggestions:**

- **Reduce the number of blocks/stages**: aggregate time periods or use
  representative days instead of hourly resolution.
- **Use SDDP decomposition**: set `method: "sddp"` to decompose the
  problem into smaller per-phase LPs instead of one large monolithic LP.
- **Use the cascade solver**: set `method: "cascade"` for multi-level
  SDDP that starts with a simplified model and progressively refines.
  See [Cascade Method](methods/cascade.md).
- **Reduce the number of scenes**: fewer scenarios in each scene means
  smaller per-scene LPs.
- **Reduce reserve zones**: spinning reserve constraints add rows per bus
  per block; consolidating reserve zones reduces LP size.
- **Disable LP names**: set `names_level` to `"minimal"` (or `0`) to reduce
  memory overhead from name storage and lookup maps.

### File not found errors

**Common causes and solutions:**

- **Relative path resolution**: `input_directory` is resolved relative to the
  directory containing the JSON file, not the current working directory.
  If the JSON file is at `cases/c0/system.json` and `input_directory` is
  `"system_c0"`, the data files are expected at `cases/c0/system_c0/`.
- **Missing component subdirectory**: data files must be in a subdirectory
  named after the component type (e.g., `Demand/lmax.parquet`,
  `Generator/pmax.parquet`).
- **Format mismatch**: if `input_format: "parquet"` but only CSV files exist
  (or vice versa), the solver will fail to find the data.  gtopt tries the
  preferred format first, then falls back to the other format.
- **Case sensitivity**: file names and component type subdirectories are
  case-sensitive on Linux (e.g., `Demand/` not `demand/`).
- **CLI override**: use `--set input_directory=<path>` to override the
  directory specified in the JSON file.

### LP debug files

Enable LP debug output to inspect the mathematical formulation:

```json
{
  "options": {
    "lp_debug": true,
    "log_directory": "logs",
    "lp_compression": "none"
  }
}
```

**File locations:**

- **Monolithic solver**: `logs/gtopt_lp_<scene>_<phase>.lp`
- **SDDP solver**: `logs/gtopt_iter_<iter>_<scene>_<phase>.lp`

When `lp_compression` is not set to `"none"`, files are compressed
(default: `zstd`).  Decompress with `zstd -d <file>.lp.zst`.

**Inspecting LP files:**

The `.lp` format is a standard text format readable by most LP solvers.
You can:

- Open the file in a text editor to inspect variable names, constraints,
  and objective coefficients.
- Load it into an external solver (e.g., GLPK, Gurobi, CPLEX) for
  independent verification.
- Use `grep` to search for specific variable or constraint names (enabled
  when `names_level` is `only_cols` or `cols_and_rows`).

**Additional debug options:**

- `lp_build: true` builds all LP matrices without solving, useful for
  inspecting the formulation without waiting for the solve.
- `lp_coeff_ratio_threshold` (default: `1e7`) controls when per-scene/phase
  coefficient ratio diagnostics are printed.  Lower the threshold to detect
  numerical conditioning issues.
- `names_level: "cols_and_rows"` assigns column and row names and warns on
  duplicate names, useful for catching formulation bugs.

---

## Batch Execution

### Shell script

```bash
#!/bin/bash
# Run all JSON cases in a directory
for f in cases/*/system_*.json; do
  echo "=== Running $f ==="
  dir=$(dirname "$f")
  name=$(basename "$dir")
  gtopt "$f" --set output_directory="results/$name"
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
        cmd += ["--set", f"output_directory={output_dir}"]
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

---

## See also

- **[Mathematical Formulation](formulation/mathematical-formulation.md)**
  — Full LP/MIP optimization formulation with academic references
- **[Planning Options Reference](planning-options.md)** — Full option
  hierarchy, merge semantics, and solver configuration
- **[Planning Guide](planning-guide.md)** — Step-by-step planning guide
  with worked examples
- **[Input Data Reference](input-data.md)** — Input data structure and file format
  reference (complete options reference)
- **[SDDP Method](methods/sddp.md)** — SDDP decomposition algorithm,
  configuration, and convergence details
- **[Cascade Method](methods/cascade.md)** — Multi-level hybrid SDDP
  method with cut inheritance and progressive refinement
- **[Monolithic Method](methods/monolithic.md)** — Default monolithic
  method, boundary cuts, and sequential mode
- **[Scripts Guide](scripts-guide.md)** — Python conversion utilities
