# Planning Options Reference

Comprehensive reference for the gtopt option hierarchy, JSON interface,
merge semantics, and solver configuration.

## Option Class Hierarchy

```
MainOptions                    (CLI-only, not in JSON)
  |
  +-- PlanningOptions          (JSON key: "options")
        |
        +-- ModelOptions           (JSON key: "model_options")
        +-- MonolithicOptions      (JSON key: "monolithic_options")
        |     +-- SolverOptions        (JSON key: "solver_options")
        +-- SddpOptions            (JSON key: "sddp_options")
        |     +-- SolverOptions        (JSON key: "forward_solver_options")
        |     +-- SolverOptions        (JSON key: "backward_solver_options")
        +-- CascadeOptions         (JSON key: "cascade_options")
        +-- SolverOptions          (JSON key: "solver_options")
        +-- LpBuildOptions         (JSON key: "lp_build_options")
        +-- VariableScale[]        (JSON key: "variable_scales")
```

**C++ headers:**

| Class | Header |
|-------|--------|
| `PlanningOptions` | `planning_options.hpp` |
| `PlanningOptionsLP` | `planning_options_lp.hpp` |
| `MainOptions` | `main_options.hpp` |
| `SolverOptions` | `solver_options.hpp` |
| `SddpOptions` | `sddp_options.hpp` |
| `MonolithicOptions` | `monolithic_options.hpp` |
| `CascadeOptions` | `cascade_options.hpp` |
| `ModelOptions` | `model_options.hpp` |
| `VariableScale` | `variable_scale.hpp` |

> **Backward-compatibility aliases**: `Options` = `PlanningOptions`,
> `OptionsLP` = `PlanningOptionsLP`.  The JSON key `"options"` is unchanged.

## PlanningOptions Fields

All fields are `std::optional` -- absent fields inherit built-in defaults
(see `PlanningOptionsLP` for resolved values).

### Input/Output

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `input_directory` | string | `"input"` | Root directory for external data files |
| `input_format` | string | `"parquet"` | Input format: `"parquet"` or `"csv"` |
| `output_directory` | string | `"output"` | Root directory for result files |
| `output_format` | string | `"parquet"` | Output format: `"parquet"` or `"csv"` |
| `output_compression` | string | `"zstd"` | Compression codec for output files |
| `use_uid_fname` | bool | `false` | Use UIDs instead of names in filenames |

### Model Parameters

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `demand_fail_cost` | float | -- | Penalty $/MWh for unserved demand |
| `reserve_fail_cost` | float | -- | Penalty $/MWh for unserved reserve |
| `use_line_losses` | bool | `true` | Model resistive line losses |
| `loss_segments` | int | `1` | Piecewise-linear segments for losses |
| `use_kirchhoff` | bool | `false` | DC Kirchhoff voltage-law constraints |
| `use_single_bus` | bool | `false` | Copper-plate mode (no network) |
| `kirchhoff_threshold` | float | `0.0` | Min voltage [kV] for Kirchhoff |
| `scale_objective` | float | `1000` | Objective coefficient divisor |
| `scale_theta` | float | `1000` | Voltage-angle variable scaling |
| `annual_discount_rate` | float | -- | Annual discount rate for CAPEX |

### Solver Selection

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `method` | string | `"monolithic"` | Planning method: `monolithic`, `sddp`, `cascade` |

### Logging and Debug

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `log_directory` | string | `"logs"` | Directory for log and trace files |
| `lp_debug` | bool | `false` | Save LP debug files before solving |
| `lp_compression` | string | -- | Compression codec for LP files |
| `lp_build` | bool | `false` | Build LP matrices without solving |

### Grouped Sub-objects

| Field | Type | Description |
|-------|------|-------------|
| `model_options` | `ModelOptions` | Power system model configuration |
| `monolithic_options` | `MonolithicOptions` | Monolithic solver settings |
| `sddp_options` | `SddpOptions` | SDDP solver settings |
| `cascade_options` | `CascadeOptions` | Cascade solver settings |
| `solver_options` | `SolverOptions` | Global LP solver configuration |
| `lp_build_options` | `LpBuildOptions` | LP assembly configuration |
| `variable_scales` | `VariableScale[]` | Per-class/variable LP scale overrides |

## Solver Options Precedence

Each planning method can specify per-pass solver options that override the
global `solver_options`.  The merge chain (highest priority first):

| Method | Merge chain |
|--------|-------------|
| Monolithic | `monolithic_options.solver_options` -> `solver_options` |
| SDDP forward | `sddp_options.forward_solver_options` -> `solver_options` |
| SDDP backward | `sddp_options.backward_solver_options` -> `solver_options` |

For each field in `SolverOptions`, the per-pass value is used if set;
otherwise the global `solver_options` value applies.

### JSON Example

```json
{
  "options": {
    "solver_options": {
      "algorithm": 3,
      "threads": 4,
      "optimal_eps": 1e-8
    },
    "sddp_options": {
      "forward_solver_options": {
        "algorithm": 1
      },
      "backward_solver_options": {
        "threads": 1
      }
    }
  }
}
```

In this example, the forward pass uses algorithm 1 (primal) with 4 threads
and 1e-8 tolerance (from global), while the backward pass uses algorithm 3
(barrier) with 1 thread and 1e-8 tolerance.

## Variable Scales Precedence

Variable scaling factors are resolved in this order (highest priority first):

1. Per-element fields (e.g., `Battery::energy_scale`, `Reservoir::energy_scale`)
2. Global options (`scale_theta` for bus voltage angles)
3. `variable_scales` entries matching by `(class_name, variable, uid)`
4. `variable_scales` entries matching by `(class_name, variable, uid=-1)` (wildcard)
5. Default scale = 1.0

### JSON Example

```json
{
  "options": {
    "scale_theta": 0.001,
    "variable_scales": [
      {"class_name": "Reservoir", "variable": "energy", "uid": -1, "scale": 1000.0},
      {"class_name": "Battery", "variable": "energy", "uid": 1, "scale": 10.0}
    ]
  }
}
```

## Merge Semantics

When multiple JSON files are passed to gtopt, their `"options"` sections are
merged left to right.  For each field:

- **Scalars**: the first file's value wins (later files fill in missing fields)
- **Sub-objects**: merged recursively (same rule per field)
- **`variable_scales`**: appended (later files add entries, earlier entries
  take precedence for the same key)

### Precedence layers

1. **CLI flags** (`--solver highs`, `--set use_single_bus=true`)
2. **Config file** (`~/.gtopt.conf` `[gtopt]` section)
3. **JSON files** (first file wins for each field)
4. **Built-in defaults** (`PlanningOptionsLP` compile-time defaults)

## MainOptions (CLI-only)

`MainOptions` is used only by the standalone binary and is not part of the
JSON interface.  It carries all CLI-parsed values plus the list of planning
files.

| Field | CLI flag | Description |
|-------|----------|-------------|
| `planning_files` | positional / `-s` | System JSON file paths |
| `solver` | `--solver` | LP solver backend |
| `method` | `--method` | Planning method |
| `demand_fail_cost` | `--demand-fail-cost` | Unserved demand penalty |
| `scale_objective` | `--scale-objective` | Objective scaling factor |
| *(any option)* | `--set key=value` | Set any planning option (see below) |

> **`--set` key=value**: all other options are now set via `--set`.
> Examples: `--set use_single_bus=true`, `--set output_directory=results/`,
> `--set solver_options.algorithm=barrier`,
> `--set sddp_options.max_iterations=300`.
>
> **Deprecated aliases** (still work, emit a warning): `-b`, `-k`,
> `-D`, `-F`, `-d`, `-f`, `-C`, `--algorithm`, `--threads`,
> `--sddp-max-iterations`, `--sddp-min-iterations`,
> `--sddp-convergence-tol`, `--lp-debug`, `--log-directory`,
> `--cut-directory`, `--lp-compression`, `--lp-coeff-ratio`.

## Full JSON Example

```json
{
  "options": {
    "input_directory": "system_data",
    "input_format": "parquet",
    "output_directory": "output",
    "output_format": "parquet",
    "output_compression": "zstd",
    "demand_fail_cost": 1000,
    "use_kirchhoff": true,
    "use_single_bus": false,
    "scale_objective": 1000,
    "annual_discount_rate": 0.1,
    "method": "sddp",
    "lp_debug": false,
    "model_options": {
      "use_parallel_scenes": true
    },
    "solver_options": {
      "algorithm": 3,
      "threads": 4,
      "optimal_eps": 1e-8,
      "feasible_eps": 1e-8
    },
    "sddp_options": {
      "max_iterations": 200,
      "convergence_tol": 1e-4,
      "forward_solver_options": {
        "algorithm": 1
      }
    },
    "lp_build_options": {
      "names_level": "only_cols"
    },
    "variable_scales": [
      {"class_name": "Reservoir", "variable": "energy", "uid": -1, "scale": 1000.0}
    ]
  }
}
```

## See Also

- [Usage Guide](usage.md) -- CLI reference and examples
- [Planning Guide](planning-guide.md) -- Step-by-step planning guide
- [SDDP Method](methods/sddp.md) -- SDDP solver configuration
- [Cascade Method](methods/cascade.md) -- Cascade solver configuration
- [Monolithic Method](methods/monolithic.md) -- Monolithic solver configuration
