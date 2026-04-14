# Planning Options Reference

Comprehensive reference for the gtopt option hierarchy, JSON interface,
merge semantics, and solver configuration.

## Option Class Hierarchy

```text
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
        +-- LpMatrixOptions        (JSON key: "lp_matrix_options")
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

### Model Parameters (deprecated flat fields)

> **Deprecated**: The flat model fields below are kept for backward
> compatibility but emit a deprecation warning when parsed from JSON.
> Use the `model_options` sub-object instead (see [ModelOptions
> Fields](#modeloptions-fields)).

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `demand_fail_cost` | float | -- | **Deprecated** — use `model_options.demand_fail_cost` |
| `reserve_fail_cost` | float | -- | **Deprecated** — use `model_options.reserve_fail_cost` |
| `hydro_fail_cost` | float | -- | **Deprecated** — use `model_options.hydro_fail_cost` |
| `hydro_use_value` | float | -- | **Deprecated** — use `model_options.hydro_use_value` |
| `use_line_losses` | bool | `true` | **Deprecated** — use `model_options.use_line_losses` |
| `loss_segments` | int | `1` | **Deprecated** — use `model_options.loss_segments` |
| `use_kirchhoff` | bool | `false` | **Deprecated** — use `model_options.use_kirchhoff` |
| `use_single_bus` | bool | `false` | **Deprecated** — use `model_options.use_single_bus` |
| `kirchhoff_threshold` | float | `0.0` | **Deprecated** — use `model_options.kirchhoff_threshold` |
| `scale_objective` | float | `1000` | **Deprecated** — use `model_options.scale_objective` |
| `scale_theta` | float | `1000` | **Deprecated** — use `model_options.scale_theta` |

> **Note**: `annual_discount_rate` belongs in the `simulation` section.
> For backward compatibility it is still accepted as a flat field in
> `options`, but emits a deprecation warning.

### Solver Selection

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `method` | string | `"monolithic"` | Planning method: `monolithic`, `sddp`, `cascade` |
| `build_mode` | string | `"scene-parallel"` | How per-cell `SystemLP` objects are assembled: `"serial"`, `"scene-parallel"`, `"full-parallel"`, `"direct-parallel"`. `serial` skips the `AdaptiveWorkPool` / build-buffer for apples-to-apples comparison against the pre-parallel code path |

### Logging and Debug

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `log_directory` | string | `"logs"` | Directory for log and trace files |
| `lp_debug` | bool | `false` | Save LP debug files before solving |
| `lp_compression` | string | -- | Compression codec for LP files |
| `lp_only` | bool | `false` | Build LP matrices without solving (CLI: `--lp-only` / `-c`) |
| `lp_fingerprint` | bool | `false` | Write [LP fingerprint](lp-fingerprint.md) JSON for formulation audit |
| `lp_debug_scene_min` | int | -- | Minimum scene UID (inclusive) for LP debug file saving |
| `lp_debug_scene_max` | int | -- | Maximum scene UID (inclusive) for LP debug file saving |
| `lp_debug_phase_min` | int | -- | Minimum phase UID (inclusive) for LP debug file saving |
| `lp_debug_phase_max` | int | -- | Maximum phase UID (inclusive) for LP debug file saving |

### Constraint Handling

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `constraint_mode` | string | `"strict"` | User-constraint runtime error policy: `"normal"` (warn + drop offending constraint), `"strict"` (default; abort with diagnostic), `"debug"` (strict + verbose per-row lowering trace). See [User Constraints → constraint_mode](user-constraints.md#constraint_mode--runtime-error-policy) |

### Grouped Sub-objects

| Field | Type | Description |
|-------|------|-------------|
| `model_options` | `ModelOptions` | Power system model configuration (**canonical location** for model fields) |
| `monolithic_options` | `MonolithicOptions` | Monolithic solver settings |
| `sddp_options` | `SddpOptions` | SDDP solver settings |
| `cascade_options` | `CascadeOptions` | Cascade solver settings |
| `solver_options` | `SolverOptions` | Global LP solver configuration |
| `lp_matrix_options` | `LpMatrixOptions` | LP assembly / equilibration / stats configuration |
| `variable_scales` | `VariableScale[]` | Per-class/variable LP scale overrides |

> **Migration**: Model parameter fields (`use_kirchhoff`, `use_single_bus`,
> etc.) can appear either at the top level of `options` (deprecated, backward-
> compatible) or inside the `model_options` sub-object (preferred).  When
> both are present, the top-level flat value takes precedence.  Flat fields
> emit a deprecation warning when parsed from JSON.

## ModelOptions Fields

The `model_options` sub-object is the **canonical location** for
LP-construction parameters.  It enables per-level overrides in cascade
solver configurations.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `use_single_bus` | bool | `false` | Collapse network to single bus (copper-plate) |
| `use_kirchhoff` | bool | `false` | Apply DC Kirchhoff voltage-law constraints |
| `use_line_losses` | bool | `true` | **Deprecated** — use `line_losses_mode` instead. Model resistive line losses (on/off) |
| `line_losses_mode` | string | `"adaptive"` | Line loss formulation: `"none"`, `"linear"`, `"piecewise"`, `"bidirectional"`, `"adaptive"`, `"dynamic"` |
| `kirchhoff_threshold` | float | `0.0` | Min bus voltage [kV] for Kirchhoff activation |
| `loss_segments` | int | `1` | Piecewise-linear segments for quadratic losses |
| `scale_objective` | float | `1000` | Objective coefficient divisor |
| `scale_theta` | float | `1000` | Voltage-angle variable scaling |
| `demand_fail_cost` | float | -- | Penalty $/MWh for unserved demand |
| `reserve_fail_cost` | float | -- | Penalty $/MWh for unserved reserve |
| `hydro_fail_cost` | float | -- | Default penalty $/m³ for unmet hydro rights (falls back to `0.0` when unset). Overridden by per-element `fail_cost` |
| `hydro_use_value` | float | -- | Default benefit $/m³ for exercising hydro rights (falls back to `0.0` when unset). Overridden by per-element `use_value` |
| `state_fail_cost` | float | -- | Penalty $/MWh for state-variable violations in SDDP elastic filter. Fallback when an element (reservoir, etc.) does not define its own `scost`. Converted to physical units via the element's `mean_production_factor` |
| `emission_cost` | float/schedule | -- | System-wide CO₂ emission cost [$/tCO₂]. When set, generators with non-zero `emission_factor` pay `emission_cost × emission_factor` per MWh |
| `emission_cap` | float/schedule | -- | System-wide CO₂ cap [tCO₂] per stage. Adds `Σ emission_factor · p · duration ≤ cap_s`; the dual is the endogenous carbon price |
| `continuous_phases` | string | `"none"` | Phase-range expression controlling which phases run LP-relaxed (integers become continuous). Syntax: `"all"`, `"none"`, `"0"`, `"1,3:5,8:"`, `":3"`. Settable per cascade level |

## SolverOptions Fields

The `solver_options` sub-object (also embedded in `monolithic_options`,
`sddp_options.forward_solver_options`, and `sddp_options.backward_solver_options`)
configures the LP backend.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `algorithm` | string/int | `"barrier"` | LP algorithm: `"default"` (0), `"primal"` (1), `"dual"` (2), `"barrier"` (3) |
| `threads` | int | `0` | Number of parallel solver threads (0 = solver default) |
| `presolve` | bool | `true` | Apply LP presolve before solving |
| `log_level` | int | `0` | Solver output verbosity (0 = none) |
| `log_mode` | string | `"nolog"` | Solver log-file policy: `"nolog"` or `"detailed"` (per-scene/phase/aperture file) |
| `reuse_basis` | bool | `false` | Reuse basis from a previous solve (warm-start). Forces dual simplex and disables presolve |
| `optimal_eps` | float | -- | Optimality tolerance (nullopt = solver default) |
| `feasible_eps` | float | -- | Feasibility tolerance (nullopt = solver default) |
| `barrier_eps` | float | -- | Barrier convergence tolerance (nullopt = solver default) |
| `time_limit` | float | -- | Per-solve time limit in seconds (0 = no limit) |
| `scaling` | string | -- | Internal solver scaling strategy (nullopt = solver default). See `SolverScaling` enum |
| `crossover` | bool | `true` | Convert interior-point solution to a simplex basis for duals (only meaningful with `algorithm=barrier`) |
| `max_fallbacks` | int | `2` | On non-optimal exit, cycle through barrier → dual → primal up to this many times. `0` disables fallback |

## MonolithicOptions Fields

> **Note**: `boundary_cuts_file` has moved to the `simulation`
> section.  For backward compatibility, it is still accepted here.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `solve_mode` | string | `"monolithic"` | Solve mode: `"monolithic"` or `"sequential"` |
| `boundary_cuts_mode` | string | `"separated"` | How to load boundary cuts: `"noload"`, `"separated"`, `"combined"` |
| `boundary_max_iterations` | int | `0` | Max iterations to load from boundary cuts (0 = all) |
| `solver_options` | `SolverOptions` | -- | Per-method LP solver configuration |

## LpMatrixOptions Fields

The `lp_matrix_options` sub-object configures how the flat (column-major)
LP representation is built and conditioned before being handed to the
backend.  Only the fields listed below are exposed in JSON; the four name
flags (`col_with_names`, `row_with_names`, `col_with_name_map`,
`row_with_name_map`) are set internally and are enabled together when
`--lp-file` or `--lp-debug` is present on the CLI.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `equilibration_method` | string | `"row_max"` | Matrix scaling: `"none"`, `"row_max"`, `"ruiz"` (auto-enabled for Kirchhoff models) |
| `fast_sqrt_method` | string | `"ieee_halve"` | Approximate sqrt used by Ruiz scaling; see `FastSqrtMethod` enum |
| `compute_stats` | bool | `false` | Compute and log coefficient min/max/ratio during `flatten()` |
| `lp_coeff_ratio_threshold` | float | `1e7` | When global max/min coefficient ratio exceeds this, print a per-scene/phase breakdown |

> **LP naming**: There is **no** `names_level` field or `--lp-names-level`
> flag.  Pass `--lp-file <path>` or `--lp-debug` to have gtopt enable all
> four naming fields on `LpMatrixOptions` so the generated `.lp` dump is
> human-readable.

## SddpOptions Fields

See [SDDP Method](methods/sddp.md) for full documentation with examples.

### Iteration Control

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `max_iterations` | int | `100` | Maximum forward/backward iterations |
| `min_iterations` | int | `2` | Minimum iterations before convergence is checked |
| `convergence_tol` | float | `1e-4` | Relative gap tolerance for convergence |

### Convergence Criteria

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `convergence_mode` | string | `"statistical"` | Criterion mode: `"gap_only"`, `"gap_stationary"`, `"statistical"` |
| `stationary_tol` | float | `0.01` | Tolerance for stationary-gap convergence (0 = disabled) |
| `stationary_window` | int | `10` | Look-back window for stationary-gap check |
| `convergence_confidence` | float | `0.95` | Confidence level for PLP-style statistical convergence (0 = disabled) |

### Cuts and Recovery

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `cut_directory` | string | `"cuts"` | Directory for Benders cut files |
| `cut_sharing_mode` | string | `"none"` | Cut sharing: `"none"`, `"expected"`, `"accumulate"`, `"max"` |
| `cut_coeff_mode` | string | `"reduced_cost"` | Cut coefficient extraction: `"reduced_cost"` or `"row_dual"` |
| `max_cuts_per_phase` | int | `0` | Maximum stored cuts per (scene, phase) (0 = unlimited) |
| `cut_prune_interval` | int | `10` | Iterations between cut pruning passes |
| `prune_dual_threshold` | float | `1e-8` | Dual threshold for inactive cut detection |
| `single_cut_storage` | bool | `false` | Use single-cut storage mode |
| `max_stored_cuts` | int | `0` | Maximum total stored cuts per scene (0 = unlimited) |
| `save_per_iteration` | bool | `true` | Save cuts after every iteration (not just at end) |
| `cut_recovery_mode` | string | `"none"` | Cut persistence: `"none"`, `"keep"`, `"append"`, `"replace"` |
| `recovery_mode` | string | `"none"` | Recovery from previous run: `"none"`, `"cuts"`, `"full"` |
| `cuts_input_file` | string | -- | CSV file for hot-start cuts |
| `named_cuts_file` | string | -- | CSV file with named-variable cuts spanning all phases |
| `boundary_cuts_mode` | string | `"separated"` | How to load boundary cuts: `"noload"`, `"separated"`, `"combined"` |
| `boundary_max_iterations` | int | `0` | Max iterations to load from boundary cuts (0 = all) |
| `missing_cut_var_mode` | string | `"skip_coeff"` | Action when cut references unknown state variable: `"skip_coeff"` or `"skip_cut"` |

### Feasibility and Elastic Filter

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `elastic_penalty` | float | `1e6` | Penalty for elastic slack variables in feasibility |
| `elastic_mode` | string | `"single_cut"` | Elastic filter mode: `"single_cut"` (alias `"cut"`), `"multi_cut"`, `"backpropagate"` |
| `multi_cut_threshold` | int | `10` | Infeasibility count threshold for switching to multi_cut (0 = never) |

### Apertures (Backward-Pass Sampling)

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `apertures` | array of UIDs | -- | Aperture UIDs (absent = from Phase, `[]` = pure Benders) |
| `aperture_directory` | string | -- | Alternate data directory for aperture scenarios |
| `aperture_timeout` | float | `15.0` | Per-aperture LP solve timeout in seconds (0 = no limit) |
| `save_aperture_lp` | bool | `false` | Save LP files for infeasible apertures to log directory |

### LP Updates and State Variables

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `update_lp_skip` | int | `0` | Iterations to skip between LP coefficient updates (0 = every iteration) |
| `state_variable_lookup_mode` | string | `"warm_start"` | Volume lookup for LP updates: `"warm_start"` or `"cross_phase"` |
| `warm_start` | bool | `true` | Reuse previous solutions as warm-start for clone LP solves |
| `use_clone_pool` | bool | `true` | Reuse cached LP clones for aperture solves |

### Monitoring and Control

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `api_enabled` | bool | `true` | Write JSON status file each iteration (for monitoring tools) |
| `sentinel_file` | string | -- | File path; if it exists, solver stops gracefully after current iteration |
| `simulation_mode` | bool | `false` | Skip training (max_iterations=0), run forward-only policy evaluation |
| `alpha_min` | float | `0.0` | Lower bound for future cost variable α |
| `alpha_max` | float | `1e12` | Upper bound for future cost variable α |

### Per-Pass Solver Options

| Field | Type | Description |
|-------|------|-------------|
| `forward_solver_options` | `SolverOptions` | LP solver overrides for SDDP forward pass |
| `backward_solver_options` | `SolverOptions` | LP solver overrides for SDDP backward pass |

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

1. Global options (`scale_theta` for bus voltage angles)
2. `variable_scales` entries matching by `(class_name, variable, uid)`
3. `variable_scales` entries matching by `(class_name, variable, uid=-1)` (wildcard)
4. Default scale = 1.0

### JSON Example

```json
{
  "options": {
    "model_options": {
      "scale_theta": 0.001
    },
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
    "method": "sddp",
    "lp_debug": false,
    "model_options": {
      "demand_fail_cost": 1000,
      "use_kirchhoff": true,
      "use_single_bus": false,
      "use_line_losses": true,
      "scale_objective": 1000
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
    "lp_matrix_options": {
      "equilibration_method": "ruiz",
      "compute_stats": true
    },
    "variable_scales": [
      {"class_name": "Reservoir", "variable": "energy", "uid": -1, "scale": 1000.0}
    ]
  },
  "simulation": {
    "annual_discount_rate": 0.1,
    "boundary_cuts_file": "boundary_cuts.csv",
    "boundary_cuts_valuation": "end_of_horizon"
  }
}
```

## See Also

- [Usage Guide](usage.md) -- CLI reference and examples
- [Planning Guide](planning-guide.md) -- Step-by-step planning guide
- [SDDP Method](methods/sddp.md) -- SDDP solver configuration
- [Cascade Method](methods/cascade.md) -- Cascade solver configuration
- [Monolithic Method](methods/monolithic.md) -- Monolithic solver configuration
