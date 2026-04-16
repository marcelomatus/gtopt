# gtopt Input Data Structure Documentation

This document describes the input data structure required to define and run
optimization cases with **gtopt** (Generation and Transmission Optimization
Planning Tool).

> For a step-by-step tutorial with worked examples and time-series workflow,
> see **[Planning Guide](planning-guide.md)**.
> For auto-generated field tables from source code run:
> `python3 scripts/gtopt_field_extractor.py --format html --output field_reference.html`

---

## Overview

A gtopt case is defined by one or more **JSON configuration files** that
contain three top-level sections:

| Section        | Description |
|----------------|-------------|
| `options`      | Global solver and I/O settings (includes `solver_options` and `variable_scales`) |
| `simulation`   | Time structure: blocks, stages, scenarios, phases, scenes, apertures |
| `system`       | Physical system: buses, generators, demands, lines, etc. |

The JSON file may reference **external data files** (CSV or Parquet) for
time-series or profile data.  These files are stored in a subdirectory
specified by `options.input_directory`.

### Directory Layout

```text
case_name/
├── case_name.json            # Main configuration file
└── <input_directory>/        # External data files
    ├── Demand/
    │   └── lmax.parquet      # Demand load profile
    ├── Generator/
    │   └── pmax.parquet      # Generator capacity profile
    └── ...
```

---

## 1. Options

Global settings that control solver behavior and I/O formats.  All fields are
optional -- when absent, the solver applies built-in defaults (shown below).

#### Input settings

| Field              | Type   | Default      | Description |
|--------------------|--------|--------------|-------------|
| `input_directory`  | string | `"input"`    | Root directory for external data files (resolved relative to JSON file) |
| `input_format`     | string | `"parquet"`  | Preferred input format: `"parquet"` or `"csv"` |

#### Model parameters

| Field                  | Type    | Default   | Units         | Description |
|------------------------|---------|-----------|---------------|-------------|
| `demand_fail_cost`     | number  | *(none)*  | $/MWh         | Penalty cost for unserved demand (value of lost load) |
| `reserve_fail_cost`    | number  | *(none)*  | $/MWh         | Penalty cost for unserved spinning reserve |
| `hydro_fail_cost`      | number  | *(none)*  | $/m³          | Default penalty cost for unmet hydro (irrigation) rights. Per-element `fail_cost` overrides this |
| `hydro_use_value`      | number  | *(none)*  | $/m³          | Default value (benefit) of exercising hydro rights. Per-element `use_value` overrides this |
| `state_fail_cost`      | number  | *(none)*  | $/MWh         | Penalty cost for SDDP state-variable violations. Fallback when a reservoir/storage element lacks its own `scost`; converted via element's `mean_production_factor` |
| `use_line_losses`      | boolean | `true`    | --            | **Deprecated** — use `line_losses_mode` instead. Enable resistive line-loss modeling |
| `line_losses_mode`     | string  | `"adaptive"` | --         | Line-loss model: `"none"`, `"linear"`, `"piecewise"`, `"bidirectional"`, `"adaptive"`, `"dynamic"` |
| `loss_segments`        | integer | `1`       | --            | Number of piecewise-linear segments for quadratic line losses (1 = linear) |
| `use_kirchhoff`        | boolean | `true`    | --            | Enable DC Kirchhoff voltage-law constraints |
| `use_single_bus`       | boolean | `false`   | --            | Collapse network to a single bus (copper-plate model) |
| `kirchhoff_threshold`  | number  | `0`       | kV            | Minimum bus voltage below which Kirchhoff is not applied |
| `scale_objective`      | number  | `1000`    | dimensionless | Divisor applied to all objective coefficients for numerical stability |
| `scale_theta`          | number  | `1000`    | dimensionless | Scaling factor for voltage-angle variables |
| `emission_cost`        | number or schedule | *(none)* | $/tCO₂ | System-wide CO₂ emission price. Generators with non-zero `emission_factor` incur an extra cost of `emission_cost × emission_factor` per MWh |
| `emission_cap`         | number or schedule | *(none)* | tCO₂/year | Per-stage CO₂ emission cap. The dual of the cap constraint is the endogenous carbon price |
| `continuous_phases`    | string  | `"none"`  | --            | Phase range where all integer/binary variables relax to continuous. Syntax: `"all"`, `"none"`, `"0"`, `"1,3:5,8:"`, `":3"` |

> **Note**: `annual_discount_rate` has moved to the `simulation`
> section (see [Section 2](#2-simulation)).  For backward
> compatibility, it is still accepted here.

#### Output settings

| Field                | Type    | Default      | Description |
|----------------------|---------|--------------|-------------|
| `output_directory`   | string  | `"output"`   | Root directory for output result files |
| `output_format`      | string  | `"parquet"`  | Output format: `"parquet"` or `"csv"` |
| `output_compression` | string  | `"zstd"`     | Compression codec: `"uncompressed"`, `"gzip"`, `"zstd"`, `"lz4"`, `"bzip2"`, `"xz"` |
| `use_uid_fname`      | boolean | `true`       | Use element UIDs instead of names in output filenames |

> **LP variable/row names** are enabled automatically when the CLI flag
> `--lp-file` or `--lp-debug` is set; there is no separate JSON or CLI
> option to control the naming level.  See
> [Planning Options → LpMatrixOptions](planning-options.md#lpmatrixoptions-fields).

#### Solver selection

| Field         | Type   | Default        | Description |
|---------------|--------|----------------|-------------|
| `method` | string | `"monolithic"` | Planning solver: `"monolithic"` (default), `"sddp"`, or `"cascade"`. See [SDDP Solver](methods/sddp.md), [Cascade Solver](methods/cascade.md), and [Monolithic Solver](methods/monolithic.md) |
| `build_mode` | string | `"scene-parallel"` | How `PlanningLP::create_systems` assembles per-cell `SystemLP`s: `"serial"`, `"scene-parallel"` (default), `"full-parallel"`, or `"direct-parallel"`. Serial builds in the calling thread with no pool/build-buffer overhead |

#### Logging and debugging

| Field                       | Type    | Default  | Description |
|-----------------------------|---------|----------|-------------|
| `log_directory`             | string  | `"logs"` | Directory for log, trace, and error LP files |
| `lp_debug`                  | boolean | `false`  | Save LP debug files to `log_directory` before solving. Monolithic: one file per `(scene, phase)` named `gtopt_lp_<scene>_<phase>.lp`. SDDP: one file per `(iteration, scene, phase)` named `gtopt_iter_<iter>_<scene>_<phase>.lp` |
| `lp_compression`            | string  | `""`     | Compression codec for LP debug files: `""` (inherit from output), `"none"` (no compression), or a codec name (`"zstd"`, `"gzip"`, `"lz4"`, `"bzip2"`, `"xz"`) |
| `lp_only`                   | boolean | `false`  | Build all LP matrices but skip solving entirely (CLI: `--lp-only` / `-c`). Combine with `lp_debug: true` to export every scene/phase LP |
| `lp_coeff_ratio_threshold`  | number  | `1e7`    | When the global max/min coefficient ratio exceeds this value, per-scene/phase breakdown is printed |
| `lp_debug_scene_min`        | integer | —        | Minimum scene UID (inclusive) for LP debug file saving |
| `lp_debug_scene_max`        | integer | —        | Maximum scene UID (inclusive) for LP debug file saving |
| `lp_debug_phase_min`        | integer | —        | Minimum phase UID (inclusive) for LP debug file saving |
| `lp_debug_phase_max`        | integer | —        | Maximum phase UID (inclusive) for LP debug file saving |
| `lp_fingerprint`            | boolean | `false`  | Compute LP structural fingerprint after solving. Output: `lp_fingerprint_scene_{S}_phase_{P}.json` per scene/phase. See [LP Fingerprint](lp-fingerprint.md) |

#### Constraint handling

| Field             | Type   | Default    | Description |
|-------------------|--------|------------|-------------|
| `constraint_mode` | string | `"strict"` | User-constraint runtime error policy: `"normal"` (warn + drop offending constraint), `"strict"` (default; abort with diagnostic), `"debug"` (strict + verbose per-row lowering trace). See [User Constraints → constraint_mode](user-constraints.md#constraint_mode--runtime-error-policy) |

#### Deprecated LP solver fields

These fields are still accepted for backward compatibility but are superseded
by the `solver_options` sub-object (see Section 1.1).

| Field          | Type    | Default | Description |
|----------------|---------|---------|-------------|
| `lp_algorithm` | integer | --      | LP algorithm: 0=auto, 1=primal, 2=dual, 3=barrier |
| `lp_threads`   | integer | --      | Solver threads (0=auto) |
| `lp_presolve`  | boolean | --      | Enable LP presolve |

### Example

```json
{
  "options": {
    "output_format": "csv",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true,
    "input_directory": "system_c0",
    "input_format": "parquet",
    "method": "monolithic",
    "solver_options": {
      "algorithm": 3,
      "presolve": true
    }
  }
}
```

### 1.1 SolverOptions

LP solver configuration exposed as a sub-object under `options`.
Individual top-level fields (`lp_algorithm`, `lp_threads`, `lp_presolve`)
are still respected for backward compatibility and take precedence
over the corresponding `solver_options` sub-fields.

| Field          | Type    | Default     | Description |
|----------------|---------|-------------|-------------|
| `algorithm`    | integer | `3` (barrier) | LP algorithm: 0 = auto, 1 = primal simplex, 2 = dual simplex, 3 = barrier |
| `threads`      | integer | `0`         | Number of solver threads (0 = automatic) |
| `presolve`     | boolean | `true`      | Enable LP presolve optimizations |
| `optimal_eps`  | number  | solver default | Optimality tolerance (omit to keep solver default) |
| `feasible_eps` | number  | solver default | Feasibility tolerance (omit to keep solver default) |
| `barrier_eps`  | number  | solver default | Barrier convergence tolerance (omit to keep solver default) |
| `log_level`    | integer | `0`         | Solver output verbosity (0 = silent) |
| `log_mode`     | string  | `"nolog"`   | Solver log-file policy: `"nolog"` (no files) or `"detailed"` (one file per scene/phase/aperture, `<solver>_sc<N>_ph<N>[_ap<N>].log`) |
| `time_limit`   | number  | solver default | Per-LP wall-clock time limit in seconds. Exceeding it aborts the solve; caller must check `is_optimal()` |
| `scaling`      | string  | solver default | Internal solver scaling strategy. See `SolverScaling` enum |
| `crossover`    | boolean | `true`      | When `algorithm == barrier`, convert the interior-point solution to a simplex basis (required for duals). SDDP forward pass sets it false for speed |
| `max_fallbacks`| integer | `2`         | On non-optimal exit, cycle through barrier → dual → primal up to this many times. `0` disables fallback |
| `reuse_basis`  | boolean | `false`    | Enable basis-reuse optimizations for resolve on cloned LPs (forces dual simplex, disables presolve) |

**Example:**

```json
{
  "options": {
    "solver_options": {
      "algorithm": 3,
      "threads": 4,
      "presolve": true,
      "optimal_eps": 1e-8,
      "feasible_eps": 1e-8,
      "log_level": 1
    }
  }
}
```

### 1.2 VariableScale

Per-class or per-element LP variable scaling factors. Convention:
`physical_value = LP_value * scale`. Defined as an array under
`options.variable_scales`.

Resolution priority when the solver looks up a scale:

1. Per-element entry matching `(class_name, variable, uid)`
2. Per-class entry matching `(class_name, variable)` with `uid = -1`
3. Fallback: `1.0` (no scaling)

> **Note:** Global options (`scale_theta`) take precedence over entries
> in `variable_scales`.

| Field        | Type    | Default | Description |
|--------------|---------|---------|-------------|
| `class_name` | string  | —       | Element class (e.g. `"Bus"`, `"Reservoir"`, `"Battery"`) |
| `variable`   | string  | —       | Variable name (e.g. `"theta"`, `"volume"`, `"energy"`) |
| `uid`        | integer | `-1`    | Element UID (`-1` = apply to all elements of this class) |
| `scale`      | number  | `1.0`   | Scale factor: `physical = LP * scale` |

**Example:**

```json
{
  "options": {
    "variable_scales": [
      {"class_name": "Bus", "variable": "theta",
       "uid": -1, "scale": 0.001},
      {"class_name": "Reservoir", "variable": "volume",
       "uid": -1, "scale": 1000.0},
      {"class_name": "Battery", "variable": "energy",
       "uid": 1, "scale": 10.0}
    ]
  }
}
```

### 1.3 SDDPOptions

SDDP-specific solver configuration, specified as a sub-object under
`options.sddp_options`.  Field names omit the `sddp_` prefix since the
section name already provides the namespace.  All fields are optional.

For full algorithmic details, see [SDDP Solver](methods/sddp.md).

#### Iteration control

| Field              | Type    | Default  | Description |
|--------------------|---------|----------|-------------|
| `max_iterations`   | integer | `100`    | Maximum number of forward/backward iterations |
| `min_iterations`   | integer | `2`      | Minimum iterations before declaring convergence |
| `convergence_tol`  | number  | `1e-4`   | Relative gap tolerance for convergence (primary criterion) |
| `stationary_tol`   | number  | `0.0`    | Secondary convergence: relative gap-change tolerance. When the gap stops improving over the look-back window, convergence is declared even if gap > `convergence_tol`. Set to e.g. `0.01` to enable. `0.0` = disabled |
| `stationary_window`| integer | `10`     | Number of iterations to look back when checking gap stationarity (only used when `stationary_tol > 0`) |

#### Advanced tuning

| Field                | Type    | Default        | Description |
|----------------------|---------|----------------|-------------|
| `elastic_penalty`    | number  | `1e6`          | Penalty for elastic slack variables in feasibility |
| `elastic_mode`       | string  | `"single_cut"` | Elastic filter mode: `"single_cut"` (alias `"cut"`), `"multi_cut"`, or `"backpropagate"` |
| `multi_cut_threshold`| integer | `10`           | Forward-pass infeasibility count before auto-switching from single_cut to multi_cut (0 = never) |
| `alpha_min`          | number  | `0.0`          | Lower bound for future cost variable alpha |
| `alpha_max`          | number  | `1e12`         | Upper bound for future cost variable alpha |
| `cut_sharing_mode`   | string  | `"none"`       | Cut sharing across scenes: `"none"`, `"expected"`, `"accumulate"`, or `"max"` |
| `efficiency_update_skip` | integer | `0`        | Iterations to skip between efficiency coefficient updates (0 = every iteration) |

#### Cut file management

| Field                | Type    | Default  | Description |
|----------------------|---------|----------|-------------|
| `cut_directory`      | string  | `"cuts"` | Directory for Benders cut files |
| `save_per_iteration` | boolean | `true`   | Save cuts to CSV after each iteration (vs. only at end) |
| `cuts_input_file`    | string  | `""`     | File path for loading initial cuts (empty = cold start) |
| `named_cuts_file`    | string  | `""`     | CSV file with named-variable cuts for hot-start across all phases |
| `sentinel_file`      | string  | `""`     | Path to a sentinel file; if it exists, the solver stops gracefully |

#### Boundary cuts

> **Note**: `boundary_cuts_file` has moved to the `simulation`
> section (see [Section 2](#2-simulation)).  For backward
> compatibility, it is still accepted in `sddp_options`.

| Field                      | Type    | Default       | Description |
|----------------------------|---------|---------------|-------------|
| `boundary_cuts_mode`       | string  | `"separated"` | Load mode: `"noload"`, `"separated"` (per-scene), or `"combined"` (broadcast) |
| `boundary_max_iterations`  | integer | `0`           | Max SDDP iterations to load from boundary file (0 = all) |

#### Apertures

| Field                 | Type      | Default   | Description |
|-----------------------|-----------|-----------|-------------|
| `apertures`           | int array | *(absent)*| Aperture UIDs for backward pass. Absent = use per-phase `aperture_set`. Empty `[]` = pure Benders. Non-empty = use exactly these UIDs |
| `aperture_directory`  | string    | `""`      | Directory for aperture-specific scenario data (empty = use `input_directory`) |
| `aperture_timeout`    | number    | `15.0`    | Timeout in seconds for individual aperture LP solves (0 = no timeout) |
| `save_aperture_lp`    | boolean   | `false`   | Save LP files for infeasible apertures to `log_directory` |

#### Timeouts and warm-start

| Field            | Type    | Default | Description |
|------------------|---------|---------|-------------|
| `solve_timeout`  | number  | `180.0` | Forward-pass LP solve timeout in seconds (0 = no timeout) |
| `warm_start`     | boolean | `true`  | Enable warm-start (dual simplex from saved basis) for aperture and elastic resolves |
| `state_variable_lookup_mode` | string | `"warm_start"` | How `update_lp` elements (seepage, production factor, discharge limit) obtain reservoir volume between phases: `"warm_start"` (warm solution / vini, no cross-phase lookup) or `"cross_phase"` (previous phase's efin). Does **not** affect SDDP state-variable chaining or cut generation. |

#### Hot-start modes

| Field            | Type    | Default  | Description |
|------------------|---------|----------|-------------|
| `hot_start`      | boolean | `false`  | Load previously saved cuts on startup (deprecated: use `cut_recovery_mode`) |
| `cut_recovery_mode` | string  | `"none"` | Hot-start mode: `"none"`, `"keep"`, `"append"`, or `"replace"`. Takes precedence over boolean `hot_start` |

#### Cut pruning

| Field                  | Type    | Default  | Description |
|------------------------|---------|----------|-------------|
| `max_cuts_per_phase`   | integer | `0`      | Maximum retained cuts per (scene, phase) LP. 0 = unlimited |
| `cut_prune_interval`   | integer | `10`     | Iterations between cut pruning passes |
| `prune_dual_threshold` | number  | `1e-8`   | Dual threshold for inactive cut detection |
| `single_cut_storage`   | boolean | `false`  | Store cuts in per-scene vectors only |
| `max_stored_cuts`      | integer | `0`      | Maximum total stored cuts per scene (0 = unlimited) |
| `low_memory_mode`      | string  | `"off"`  | `off`, `snapshot`, `compress`, or `rebuild` (see SDDP method docs) |
| `memory_codec`         | string  | `"auto"` | Compression codec for `compress` mode |

#### Simulation mode

| Field             | Type    | Default | Description |
|-------------------|---------|---------|-------------|
| `simulation_mode` | boolean | `false` | Run in simulation mode: forward-only evaluation of the policy from loaded cuts. Sets `max_iterations=0` and disables cut saving. Feasibility cuts from the simulation pass are discarded by default for hot-start reproducibility |

#### Monitoring API

| Field         | Type    | Default | Description |
|---------------|---------|---------|-------------|
| `api_enabled` | boolean | `true`  | Enable the SDDP monitoring API (writes JSON status file each iteration) |

### 1.4 CascadeOptions (multi-level hybrid solver)

These options configure the cascade solver (`method = "cascade"`), which
runs a multi-level hybrid algorithm with progressive LP refinement.  They are
set in their own `cascade_options` sub-object (not inside `sddp_options`).
See [Cascade Method](methods/cascade.md) for full documentation, and
[SDDP Solver -- S10](methods/sddp.md#10-cascade-solver--multi-level-hybrid-solver)
for a summary.

The `cascade_options` object contains three top-level fields:

| Field           | Type   | Description |
|-----------------|--------|-------------|
| `model_options` | object | Global `ModelOptions` defaults for all levels |
| `sddp_options`  | object | Global `SddpOptions` defaults for all levels. `max_iterations` here is the global budget across all levels |
| `level_array`   | array  | Array of `CascadeLevel` objects. When empty or omitted, a built-in 4-level default is used |

Each element in the `level_array` is an object with the following optional
fields:

| Field           | Type    | Description |
|-----------------|---------|-------------|
| `uid`           | integer | Unique identifier for this level |
| `name`          | string  | Human-readable level name (for logging) |
| `model_options` | object  | LP formulation overrides; when present, a new LP is built. When absent, the previous level's LP is reused |
| `sddp_options`  | object  | Solver parameters for this level (per-level overrides) |
| `transition`    | object  | Transfer rules from the previous level |

**`model_options` fields** (same structure at both cascade and level scope):

| Field                  | Type    | Default | Description |
|------------------------|---------|---------|-------------|
| `use_single_bus`       | boolean | `false` | Aggregate all buses into one |
| `use_kirchhoff`        | boolean | `true`  | Enable DC power flow (voltage angles) |
| `use_line_losses`      | boolean | `false` | Model line losses |
| `kirchhoff_threshold`  | number  | `0.0`   | Threshold for Kirchhoff constraint activation |
| `loss_segments`        | integer | `1`     | Number of piecewise-linear loss segments |
| `scale_objective`      | number  | `1000`  | Divisor for objective coefficients |
| `scale_theta`          | number  | `1000`  | Scaling factor for voltage-angle variables |
| `demand_fail_cost`     | number  | *(none)*| Penalty cost for unserved demand [$/MWh] |
| `reserve_fail_cost`    | number  | *(none)*| Penalty cost for unserved reserve [$/MWh] |

> **Note**: `annual_discount_rate` has moved to the `simulation`
> section.  For backward compatibility, it is still accepted in
> `model_options`.

**`sddp_options` fields** (per-level solver parameters):

| Field             | Type      | Default              | Description |
|-------------------|-----------|----------------------|-------------|
| `max_iterations`  | integer   | from global `sddp_options` | Maximum iterations for this level |
| `min_iterations`  | integer   | from global `sddp_options` | Minimum iterations before convergence |
| `apertures`       | int array | from global `sddp_options` | Aperture UIDs (absent = inherit, `[]` = Benders) |
| `convergence_tol` | number    | from global `sddp_options` | Convergence tolerance |
| `stationary_tol`  | number    | from global `sddp_options` | Stationary gap-change tolerance (0 = disabled) |
| `stationary_window`| integer  | from global `sddp_options` | Look-back window for stationary gap check |

**`transition` fields:**

See [CASCADE_SOLVER.md §4.5](methods/cascade.md) for detailed cut
forgetting semantics and the two-phase solve behavior.

| Field                        | Type    | Default | Description |
|------------------------------|---------|---------|-------------|
| `inherit_optimality_cuts`    | integer | `0`     | `0` = do not inherit; `-1` = inherit and keep forever; `N > 0` = inherit, then forget after N training iterations |
| `inherit_feasibility_cuts`   | integer | `0`     | Same semantics as `inherit_optimality_cuts` |
| `inherit_targets`            | integer | `0`     | `0` = no targets; `-1` = inherit forever; `N > 0` = inherit with forgetting |
| `target_rtol`                | number  | `0.05`  | Relative tolerance for target band (fraction of abs(v)) |
| `target_min_atol`            | number  | `1.0`   | Minimum absolute tolerance for target band |
| `target_penalty`             | number  | `500`   | Elastic penalty cost per unit target violation |
| `optimality_dual_threshold`  | number  | `0.0`   | Minimum abs(dual) threshold for transferring cuts. Cuts with abs(dual) below this are skipped. 0 = transfer all |

**SDDP example:**

```json
{
  "options": {
    "method": "sddp",
    "sddp_options": {
      "max_iterations": 200,
      "convergence_tol": 1e-5,
      "stationary_tol": 0.01,
      "stationary_window": 10,
      "cut_sharing_mode": "expected",
      "cut_directory": "cuts",
      "elastic_mode": "single_cut",
      "elastic_penalty": 1e7,
      "cut_recovery_mode": "keep",
      "boundary_cuts_mode": "separated",
      "apertures": []
    }
  }
}
```

**Cascade example:**

```json
{
  "options": {
    "method": "cascade",
    "sddp_options": {
      "max_iterations": 30,
      "convergence_tol": 0.01
    },
    "cascade_options": {
      "level_array": [
        {
          "name": "uninodal_benders",
          "model_options": {
            "use_single_bus": true,
            "use_kirchhoff": false,
            "use_line_losses": false
          },
          "sddp_options": {
            "max_iterations": 10,
            "apertures": [],
            "convergence_tol": 0.05
          }
        },
        {
          "name": "transport_sddp",
          "model_options": {
            "use_single_bus": false,
            "use_kirchhoff": false
          },
          "sddp_options": {
            "max_iterations": 15,
            "apertures": [1, 2, 3]
          },
          "transition": {
            "inherit_targets": true,
            "target_rtol": 0.05,
            "target_min_atol": 1.0,
            "target_penalty": 500
          }
        },
        {
          "name": "full_sddp",
          "model_options": {
            "use_kirchhoff": true,
            "use_line_losses": true
          },
          "sddp_options": {
            "max_iterations": 30,
            "apertures": [1, 2, 3]
          },
          "transition": {
            "inherit_optimality_cuts": true,
            "inherit_feasibility_cuts": true,
            "optimality_dual_threshold": 1e-6
          }
        }
      ]
    }
  }
}
```

**Simulation mode example:**

```json
{
  "options": {
    "method": "sddp",
    "sddp_options": {
      "simulation_mode": true,
      "cut_recovery_mode": "keep",
      "cut_directory": "cuts"
    }
  }
}
```

### 1.5 MonolithicOptions

Monolithic-solver-specific configuration, specified as a sub-object under
`options.monolithic_options`.  All fields are optional.

For full details, see [Monolithic Solver](methods/monolithic.md).

| Field                      | Type    | Default       | Description |
|----------------------------|---------|---------------|-------------|
| `solve_mode`               | string  | `"monolithic"`| Solve mode: `"monolithic"` (all phases in one LP) or `"sequential"` (phase-by-phase) |
| `boundary_cuts_mode`       | string  | `"separated"` | Load mode: `"noload"`, `"separated"` (per-scene), or `"combined"` (broadcast) |
| `boundary_max_iterations`  | integer | `0`           | Max iterations to load from boundary file (0 = all) |
| `solve_timeout`            | number  | `18000.0`     | LP solve timeout in seconds (0 = no timeout) |

> **Note**: `boundary_cuts_file` has moved to the `simulation`
> section.  For backward compatibility, it is still accepted in
> `monolithic_options`.

**Example:**

```json
{
  "options": {
    "method": "monolithic",
    "monolithic_options": {
      "solve_mode": "monolithic",
      "boundary_cuts_mode": "separated"
    }
  },
  "simulation": {
    "boundary_cuts_file": "boundary_cuts.csv"
  }
}
```

---

## 2. Simulation

Defines the temporal structure of the optimization. The `simulation`
object contains arrays of time-structure elements organized in a
hierarchy:

```text
Scenario  (probability_factor)
  └─ Phase
       └─ Stage  (discount_factor, first_block, count_block)
            └─ Block  (duration [h])
```

Scenes group scenarios; Apertures define SDDP backward-pass openings.

### Simulation-level fields

In addition to the arrays below, the `simulation` object accepts these
scalar fields:

| Field                      | Type   | Default            | Description |
|----------------------------|--------|--------------------|-------------|
| `annual_discount_rate`     | number | `0.0`              | Annual discount rate for multi-stage CAPEX discounting [p.u./year] |
| `boundary_cuts_file`       | string | `""`               | CSV file with boundary (future-cost) cuts for the last phase |
| `boundary_cuts_valuation`  | string | `"end_of_horizon"` | Boundary-cut valuation mode: `"end_of_horizon"` (default) or `"present_value"` |

> **Backward compatibility**: `annual_discount_rate` is also accepted
> in `options` or `options.model_options`.  `boundary_cuts_file` is
> also accepted in `options.sddp_options` or
> `options.monolithic_options`.  The `simulation` location is
> preferred.

| Array              | Element   | Required | Description |
|--------------------|-----------|----------|-------------|
| `block_array`      | Block     | Yes      | Indivisible time units |
| `stage_array`      | Stage     | Yes      | Planning/investment periods |
| `scenario_array`   | Scenario  | Yes      | Stochastic realizations |
| `phase_array`      | Phase     | No       | Groups of consecutive stages |
| `scene_array`      | Scene     | No       | Groups of scenarios |
| `aperture_array`   | Aperture  | No       | SDDP backward-pass openings |

When `phase_array` or `scene_array` are empty, a single default
Phase / Scene covering all stages / scenarios is created automatically.

### 2.1 Block

A block is the smallest indivisible time unit. `energy [MWh] = power [MW] × duration [h]`.

| Field      | Type   | Units | Required | Description |
|------------|--------|-------|----------|-------------|
| `uid`      | integer| —     | Yes      | Unique identifier |
| `name`     | string | —     | No       | Optional name |
| `duration` | number | h     | Yes      | Duration of the block |

### 2.2 Stage

A stage groups consecutive blocks into a planning/investment period.

| Field            | Type    | Units | Required | Description |
|------------------|---------|-------|----------|-------------|
| `uid`            | integer | —     | Yes      | Unique identifier |
| `name`           | string  | —     | No       | Optional name |
| `first_block`    | integer | —     | Yes      | 0-based index of the first block in this stage |
| `count_block`    | integer | —     | Yes      | Number of consecutive blocks in this stage |
| `discount_factor`| number  | p.u.  | No       | Present-value cost multiplier for this stage |
| `active`         | boolean | —     | No       | Whether the stage is active |

### 2.3 Scenario

A scenario represents a possible future realization (hydrology, demand level, etc.).

| Field               | Type    | Units | Required | Description |
|---------------------|---------|-------|----------|-------------|
| `uid`               | integer | —     | Yes      | Unique identifier |
| `name`              | string  | —     | No       | Optional name |
| `probability_factor`| number  | p.u.  | No       | Probability weight (values are normalised to sum to 1) |
| `active`            | boolean | —     | No       | Whether the scenario is active |

### 2.4 Phase

A phase groups consecutive planning stages into a higher-level period.
This allows modelling distinct investment or operational windows
(e.g. a 5-year construction phase followed by a 20-year operational
phase). When only one phase is needed (the common case), it is created
automatically with defaults covering all stages.

| Field          | Type    | Units | Required | Description |
|----------------|---------|-------|----------|-------------|
| `uid`          | integer | —     | Yes      | Unique identifier |
| `name`         | string  | —     | No       | Optional name |
| `active`       | boolean | —     | No       | Whether the phase is active |
| `first_stage`  | integer | —     | Yes      | 0-based index of the first stage in this phase |
| `count_stage`  | integer | —     | No       | Number of stages (`-1` or omit = all remaining) |
| `aperture_set` | array   | —     | No       | Array of aperture UIDs for this phase's SDDP backward pass (empty = use all) |

### 2.5 Scene

A scene groups consecutive scenarios into a logical set. Scenes are
used by the solver to partition scenarios when building LP sub-problems
(one LP per scene/phase combination). When no `scene_array` is
provided, a single default scene covering all scenarios is created.

| Field            | Type    | Units | Required | Description |
|------------------|---------|-------|----------|-------------|
| `uid`            | integer | —     | Yes      | Unique identifier |
| `name`           | string  | —     | No       | Optional name |
| `active`         | boolean | —     | No       | Whether the scene is active |
| `first_scenario` | integer | —     | Yes      | 0-based index of the first scenario in this scene |
| `count_scenario` | integer | —     | No       | Number of scenarios (`-1` or omit = all remaining) |

### 2.6 Aperture

An aperture represents one hydrological (or stochastic) realization
used in the SDDP backward pass. Each aperture references a **source
scenario** whose affluent data (flow bounds) are applied to the cloned
phase LP before solving. Apertures allow the backward pass to sample
a different set of scenarios than the forward pass.

When no `aperture_array` is provided, the SDDP solver falls back to
the legacy behaviour controlled by `sddp_num_apertures` (first N
scenarios or all scenarios).

| Field                | Type    | Units | Required | Description |
|----------------------|---------|-------|----------|-------------|
| `uid`                | integer | —     | Yes      | Unique identifier |
| `name`               | string  | —     | No       | Optional name |
| `active`             | boolean | —     | No       | Whether the aperture is active |
| `source_scenario`    | integer | —     | Yes      | UID of the scenario whose affluent data to use |
| `probability_factor` | number  | p.u.  | No       | Probability weight (normalised to sum 1 across active apertures; default: 1) |

> **Note:** When `aperture_directory` is set in `sddp_options`,
> the source scenario is first looked up in that directory; if not
> found there, it falls back to the regular `input_directory`.

### Example

```json
{
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1},
      {"uid": 2, "first_block": 1, "count_block": 1, "active": 1}
    ],
    "scenario_array": [
      {"uid": 1, "probability_factor": 1}
    ],
    "phase_array": [
      {"uid": 1, "first_stage": 0, "count_stage": 2}
    ],
    "scene_array": [
      {"uid": 1, "first_scenario": 0, "count_scenario": 1}
    ],
    "aperture_array": [
      {"uid": 1, "source_scenario": 1, "probability_factor": 0.5},
      {"uid": 2, "source_scenario": 2, "probability_factor": 0.5}
    ]
  }
}
```

---

## 3. System

The system section defines all physical components of the power system.

### 3.1 Bus

An electrical bus (node) in the network.

| Field             | Type    | Units | Required | Description |
|-------------------|---------|-------|----------|-------------|
| `uid`             | integer | —     | Yes      | Unique identifier |
| `name`            | string  | —     | Yes      | Bus name |
| `active`          | boolean | —     | No       | Whether the bus is active |
| `voltage`         | number  | kV    | No       | Nominal voltage level |
| `reference_theta` | number  | rad   | No       | Fixed voltage angle (reference bus: set to 0) |
| `use_kirchhoff`   | boolean | —     | No       | Override global Kirchhoff setting for this bus |

### 3.2 Generator

A generation unit connected to a bus.

| Field              | Type                | Units        | Required | Description |
|--------------------|---------------------|--------------|----------|-------------|
| `uid`              | integer             | —            | Yes      | Unique identifier |
| `name`             | string              | —            | Yes      | Generator name |
| `bus`              | integer\|string     | —            | Yes      | Connected bus UID or name |
| `active`           | boolean             | —            | No       | Whether the generator is active |
| `pmin`             | number\|array\|string| MW          | No       | Minimum active power output |
| `pmax`             | number\|array\|string| MW          | No       | Maximum active power output |
| `gcost`            | number\|array\|string| $/MWh       | No       | Variable generation cost |
| `lossfactor`       | number\|array\|string| p.u.        | No       | Network loss factor |
| `capacity`         | number\|array\|string| MW          | No       | Installed capacity |
| `expcap`           | number\|array\|string| MW          | No       | Capacity added per expansion module |
| `expmod`           | number\|array\|string| —           | No       | Maximum number of expansion modules |
| `capmax`           | number\|array\|string| MW          | No       | Absolute maximum capacity |
| `annual_capcost`   | number\|array\|string| $/MW-year   | No       | Annualized investment cost |
| `annual_derating`  | number\|array\|string| p.u./year   | No       | Annual capacity derating factor |

> **Note:** Fields that accept `number|array|string` can be a numeric constant,
> an inline array (indexed by `[stage][block]`), or a filename referencing an
> external Parquet/CSV file in `input_directory/Generator/`.

### 3.3 Demand

An electrical demand (load) connected to a bus.

| Field              | Type                | Units        | Required | Description |
|--------------------|---------------------|--------------|----------|-------------|
| `uid`              | integer             | —            | Yes      | Unique identifier |
| `name`             | string              | —            | Yes      | Demand name |
| `bus`              | integer\|string     | —            | Yes      | Connected bus UID or name |
| `active`           | boolean             | —            | No       | Whether the demand is active |
| `lmax`             | number\|array\|string| MW          | No       | Maximum served load |
| `lossfactor`       | number\|array\|string| p.u.        | No       | Network loss factor |
| `fcost`            | number\|array\|string| $/MWh       | No       | Demand curtailment cost |
| `emin`             | number\|array\|string| MWh         | No       | Minimum energy that must be served per stage |
| `ecost`            | number\|array\|string| $/MWh       | No       | Energy-shortage cost |
| `capacity`         | number\|array\|string| MW          | No       | Installed capacity |
| `expcap`           | number\|array\|string| MW          | No       | Capacity added per expansion module |
| `expmod`           | number\|array\|string| —           | No       | Maximum number of expansion modules |
| `capmax`           | number\|array\|string| MW          | No       | Absolute maximum capacity |
| `annual_capcost`   | number\|array\|string| $/MW-year   | No       | Annualized investment cost |
| `annual_derating`  | number\|array\|string| p.u./year   | No       | Annual capacity derating factor |

### 3.4 Line

A transmission line connecting two buses.

| Field              | Type                | Units        | Required | Description |
|--------------------|---------------------|--------------|----------|-------------|
| `uid`              | integer             | —            | Yes      | Unique identifier |
| `name`             | string              | —            | Yes      | Line name |
| `bus_a`            | integer\|string     | —            | Yes      | Sending-end (from) bus |
| `bus_b`            | integer\|string     | —            | Yes      | Receiving-end (to) bus |
| `active`           | boolean             | —            | No       | Whether the line is active |
| `voltage`          | number\|array\|string| kV          | No       | Nominal voltage level. Omit or set to 1.0 for per-unit mode |
| `resistance`       | number\|array\|string| Ω (p.u.)    | No       | Series resistance. Use Ω when voltage is in kV; p.u. when voltage is omitted |
| `reactance`        | number\|array\|string| Ω (p.u.)    | No       | Series reactance (DC power flow). Use Ω when voltage is in kV; p.u. when voltage is omitted. Susceptance: $B = V^2 / X$ |
| `lossfactor`       | number\|array\|string| p.u.        | No       | Lumped loss factor |
| `type`             | string              | —            | No       | Element type tag; use `"transformer"` for transformers |
| `tap_ratio`        | number\|array\|string| p.u.        | No       | Off-nominal tap ratio τ (default 1.0). A tap-changing transformer with τ ≠ 1 has effective susceptance $B/\tau$. Supports per-stage schedule. |
| `phase_shift_deg`  | number\|array\|string| degrees     | No       | Phase-shift angle φ in degrees (default 0). Models a Phase-Shifting Transformer (PST); shifts the Kirchhoff constraint RHS by $-\sigma_\theta \cdot \phi_{\text{rad}}$. |
| `tmax_ab`          | number\|array\|string| MW          | No       | Max flow in A→B direction |
| `tmax_ba`          | number\|array\|string| MW          | No       | Max flow in B→A direction |
| `tcost`            | number\|array\|string| $/MWh       | No       | Variable transmission cost |
| `capacity`         | number\|array\|string| MW          | No       | Installed capacity |
| `expcap`           | number\|array\|string| MW          | No       | Capacity added per expansion module |
| `expmod`           | number\|array\|string| —           | No       | Maximum number of expansion modules |
| `capmax`           | number\|array\|string| MW          | No       | Absolute maximum capacity |
| `annual_capcost`   | number\|array\|string| $/MW-year   | No       | Annualized investment cost |
| `annual_derating`  | number\|array\|string| p.u./year   | No       | Annual capacity derating factor |

### 3.5 Battery

A battery energy storage system (BESS).

#### Standalone battery (unified definition)

When the optional `bus` field is set (without `source_generator`), the system
auto-generates a discharge Generator, a charge Demand, and a linking Converter
during preprocessing. Both charge and discharge connect to the same external
bus. Only a single Battery element is needed — no separate Converter,
Generator, or Demand definitions required.

```json
{
  "uid": 1, "name": "bess1",
  "bus": 3,
  "input_efficiency": 0.95, "output_efficiency": 0.95,
  "emin": 0, "emax": 200,
  "pmax_charge": 60, "pmax_discharge": 60,
  "gcost": 0,
  "capacity": 200
}
```

#### Generation-coupled battery (hybrid / behind-the-meter)

When both `bus` and `source_generator` are set, the battery operates in
*generation-coupled* mode: the referenced generator directly charges the
battery through an auto-created internal bus. This models a solar or wind
plant that sits "behind" the battery (e.g. a DC-coupled or AC-coupled
hybrid system).

`System::expand_batteries()` will:
- Create an internal bus (name = `<battery.name>_int_bus`)
- Connect the **discharge Generator** to the external `bus`
- Connect the **charge Demand** to the internal bus
- Rewire the **source generator** to the internal bus

The `source_generator` value is the UID or name of a Generator element
already defined in `generator_array`. Its own `bus` field is overwritten
with the internal bus.

```json
{
  "uid": 1, "name": "bess1",
  "bus": 3,
  "source_generator": "solar1",
  "input_efficiency": 0.95, "output_efficiency": 0.95,
  "emin": 0, "emax": 200,
  "pmax_charge": 60, "pmax_discharge": 60,
  "gcost": 0,
  "capacity": 200
}
```

#### Traditional definition

Without the `bus` field, a separate Converter, Generator, and Demand must
be defined manually (see §3.6 Converter).

| Field               | Type                | Units        | Required | Description |
|---------------------|---------------------|--------------|----------|-------------|
| `uid`               | integer             | —            | Yes      | Unique identifier |
| `name`              | string              | —            | Yes      | Battery name |
| `active`            | boolean             | —            | No       | Whether the battery is active |
| `bus`               | integer\|string     | —            | No       | Bus connection (enables unified / coupled definition) |
| `source_generator`  | integer\|string     | —            | No       | Source generator for generation-coupled mode (battery name or UID) |
| `input_efficiency`  | number\|array\|string| p.u.        | No       | Charging efficiency |
| `output_efficiency` | number\|array\|string| p.u.        | No       | Discharging efficiency |
| `annual_loss`       | number\|array\|string| p.u./year   | No       | Annual self-discharge rate |
| `emin`              | number\|array\|string| MWh         | No       | Minimum state of charge |
| `emax`              | number\|array\|string| MWh         | No       | Maximum state of charge |
| `ecost`             | number\|array\|string| $/MWh       | No       | Storage usage cost (penalty) |
| `eini`              | number              | MWh          | No       | Initial state of charge |
| `efin`              | number              | MWh          | No       | Terminal state of charge |
| `pmax_charge`       | number\|array\|string| MW          | No       | Max charging power (unified definition) |
| `pmax_discharge`    | number\|array\|string| MW          | No       | Max discharging power (unified definition) |
| `gcost`             | number\|array\|string| $/MWh       | No       | Discharge generation cost (unified definition) |
| `capacity`          | number\|array\|string| MWh         | No       | Installed energy capacity |
| `expcap`            | number\|array\|string| MWh         | No       | Energy capacity per expansion module |
| `expmod`            | number\|array\|string| —           | No       | Maximum number of expansion modules |
| `capmax`            | number\|array\|string| MWh         | No       | Absolute maximum energy capacity |
| `annual_capcost`    | number\|array\|string| $/MWh-year  | No       | Annualized investment cost |
| `annual_derating`   | number\|array\|string| p.u./year   | No       | Annual capacity derating factor |
| `type`              | string              | —            | No       | Optional battery type tag (metadata, e.g. `"lithium"`, `"flow"`) |
| `soft_emin`         | number\|array\|string| MWh         | No       | Soft minimum energy per stage (penalized slack below `emin`) |
| `soft_emin_cost`    | number\|array\|string| $/MWh       | No       | Penalty cost per unit of `soft_emin` slack violation |
| `use_state_variable`| boolean             | —            | No       | Enable stage/phase coupling of the battery state variable |
| `daily_cycle`       | boolean             | —            | No       | Enforce daily cycling (initial SoC equals final SoC each day) |

### 3.6 Converter

Links a battery to a discharge generator and a charge demand.

| Field              | Type                | Units           | Required | Description |
|--------------------|---------------------|-----------------|----------|-------------|
| `uid`              | integer             | —               | Yes      | Unique identifier |
| `name`             | string              | —               | Yes      | Converter name |
| `active`           | boolean             | —               | No       | Whether the converter is active |
| `battery`          | integer\|string     | —               | Yes      | Associated battery UID or name |
| `generator`        | integer\|string     | —               | Yes      | Discharge generator UID or name |
| `demand`           | integer\|string     | —               | Yes      | Charge demand UID or name |
| `conversion_rate`  | number\|array\|string| MW/(MWh/h)     | No       | Electrical output per unit stored energy withdrawn |
| `capacity`         | number\|array\|string| MW              | No       | Installed power capacity |
| `expcap`           | number\|array\|string| MW              | No       | Power capacity per expansion module |
| `expmod`           | number\|array\|string| —               | No       | Maximum number of expansion modules |
| `capmax`           | number\|array\|string| MW              | No       | Absolute maximum power capacity |
| `annual_capcost`   | number\|array\|string| $/MW-year       | No       | Annualized investment cost |
| `annual_derating`  | number\|array\|string| p.u./year       | No       | Annual capacity derating factor |

### 3.7 Junction

A hydraulic node in the water network.

| Field    | Type    | Units | Required | Description |
|----------|---------|-------|----------|-------------|
| `uid`    | integer | —     | Yes      | Unique identifier |
| `name`   | string  | —     | Yes      | Junction name |
| `active` | boolean | —     | No       | Whether the junction is active |
| `drain`  | boolean | —     | No       | If true, excess water can leave the system freely |

### 3.8 Waterway

A water channel connecting two junctions.

| Field        | Type                | Units  | Required | Description |
|--------------|---------------------|--------|----------|-------------|
| `uid`        | integer             | —      | Yes      | Unique identifier |
| `name`       | string              | —      | Yes      | Waterway name |
| `active`     | boolean             | —      | No       | Whether the waterway is active |
| `junction_a` | integer\|string     | —      | Yes      | Upstream junction UID or name |
| `junction_b` | integer\|string     | —      | Yes      | Downstream junction UID or name |
| `capacity`   | number\|array\|string| m³/s  | No       | Maximum flow capacity |
| `lossfactor` | number\|array\|string| p.u.  | No       | Transit loss coefficient |
| `fmin`       | number\|array\|string| m³/s  | No       | Minimum required water flow |
| `fmax`       | number\|array\|string| m³/s  | No       | Maximum allowed water flow |

### 3.9 Reservoir

A water reservoir connected to a junction.  Volume units: **hm³** (1 hm³ = 10⁶ m³).

| Field                  | Type                | Units       | Required | Description |
|------------------------|---------------------|-------------|----------|-------------|
| `uid`                  | integer             | —           | Yes      | Unique identifier |
| `name`                 | string              | —           | Yes      | Reservoir name |
| `active`               | boolean             | —           | No       | Whether the reservoir is active |
| `junction`             | integer\|string     | —           | Yes      | Associated junction UID or name |
| `spillway_capacity`    | number              | m³/s        | No       | Maximum uncontrolled spill capacity |
| `spillway_cost`        | number              | $/hm³      | No       | Penalty per unit of spilled water |
| `capacity`             | number\|array\|string| hm³       | No       | Total usable storage capacity |
| `annual_loss`          | number\|array\|string| p.u./year  | No       | Annual evaporation/seepage loss rate |
| `emin`                 | number\|array\|string| hm³       | No       | Minimum allowed stored volume |
| `emax`                 | number\|array\|string| hm³       | No       | Maximum allowed stored volume |
| `ecost`                | number\|array\|string| $/hm³     | No       | Water value (shadow cost of stored water) |
| `eini`                 | number              | hm³        | No       | Initial stored volume |
| `efin`                 | number              | hm³        | No       | Target final stored volume |
| `fmin`                 | number              | m³/s        | No       | Minimum net inflow |
| `fmax`                 | number              | m³/s        | No       | Maximum net inflow |
| `flow_conversion_rate` | number              | hm³/(m³/s·h)| No     | Converts m³/s × hours to hm³ (default: 0.0036) |
| `scost`                | number\|array\|string| $/hm³     | No       | Short-run water shortage cost |
| `soft_emin`            | number\|array\|string| hm³       | No       | Soft minimum volume per stage (penalized slack below `emin`) |
| `soft_emin_cost`       | number\|array\|string| $/hm³     | No       | Penalty cost per unit of `soft_emin` slack violation |
| `use_state_variable`   | boolean             | —           | No       | Enable stage/phase coupling of the reservoir state variable |
| `daily_cycle`          | boolean             | —           | No       | Enforce daily cycling (initial volume equals final volume each day) |

### 3.10 Turbine

A hydro turbine linking a waterway to a generator.

| Field             | Type                | Units      | Required | Description |
|-------------------|---------------------|------------|----------|-------------|
| `uid`             | integer             | —          | Yes      | Unique identifier |
| `name`            | string              | —          | Yes      | Turbine name |
| `active`          | boolean             | —          | No       | Whether the turbine is active |
| `waterway`        | integer\|string     | —          | Yes      | Associated waterway UID or name |
| `generator`       | integer\|string     | —          | Yes      | Associated generator UID or name |
| `drain`           | boolean             | —          | No       | If true, turbine can spill water without generating |
| `conversion_rate` | number\|array\|string| MW·s/m³   | No       | Water-to-power conversion factor |
| `capacity`        | number\|array\|string| MW        | No       | Maximum turbine power output |
| `main_reservoir`  | integer\|string     | —          | No       | Reservoir whose volume drives the efficiency curve |

### 3.11 Flow (Inflow)

A water inflow or outflow at a junction.

| Field       | Type                | Units | Required | Description |
|-------------|---------------------|-------|----------|-------------|
| `uid`       | integer             | —     | Yes      | Unique identifier |
| `name`      | string              | —     | Yes      | Flow name |
| `active`    | boolean             | —     | No       | Whether the flow is active |
| `direction` | integer             | —     | No       | +1 = inflow, −1 = outflow |
| `junction`  | integer\|string     | —     | Yes      | Associated junction UID or name |
| `discharge` | number\|array\|string| m³/s | Yes      | Water discharge schedule |

### 3.12 Reservoir Seepage (formerly Filtration)

Piecewise-linear seepage model from a waterway to an adjacent reservoir,
representing water losses due to soil permeability (Darcy's law
approximation).  The seepage flow through the waterway is
constrained to:

```text
seepage [m³/s] = slope [m³/s/hm³] × avg_reservoir_volume [hm³] + constant [m³/s]
```

where `avg_reservoir_volume = (eini + efin) / 2`.  This captures the
hydrostatic-head dependence of seepage.  Corresponds to PLP **plpfilemb.dat**.

When piecewise-linear `segments` are provided, the active segment is
selected based on the reservoir's current volume (vini from the previous
phase).  The LP constraint coefficients (slope and RHS) are updated
dynamically — the same mechanism used by Reservoir Production Factor for
turbine conversion rates.  If `segments` is empty, the static `slope` and
`constant` fields are used as a simple linear model.

**JSON array name:** `reservoir_seepage_array`

| Field       | Type    | Units        | Required | Description |
|-------------|---------|--------------|----------|-------------|
| `uid`       | integer | —            | Yes      | Unique identifier |
| `name`      | string  | —            | Yes      | Seepage element name |
| `active`    | boolean | —            | No       | Whether the seepage element is active |
| `waterway`  | integer\|string | —    | Yes      | Source waterway UID or name |
| `reservoir` | integer\|string | —    | Yes      | Receiving reservoir UID or name |
| `slope`     | number  | m³/s / hm³   | No       | Default seepage slope (used when `segments` is empty) |
| `constant`  | number  | m³/s         | No       | Default constant seepage rate (used when `segments` is empty) |
| `segments`  | array   | —            | No       | Piecewise-linear concave segments (see below) |

Each segment in the `segments` array describes one piece of the concave
filtration envelope:

| Field      | Type   | Units          | Description |
|------------|--------|----------------|-------------|
| `volume`   | number | hm³            | Volume breakpoint |
| `slope`    | number | m³/s / hm³     | Seepage slope at this breakpoint |
| `constant` | number | m³/s           | Seepage rate at this breakpoint |

The seepage rate at volume *V* is computed as the minimum over all
segments: `seepage(V) = min_i { constant_i + slope_i × (V − volume_i) }`.
This is analogous to the piecewise-linear evaluation used in
[Reservoir Production Factor](#313-reservoir-production-factor).

**Example with segments:**

```json
{
  "uid": 1,
  "name": "filt1",
  "waterway": "w1_2",
  "reservoir": "r1",
  "slope": 0.001,
  "constant": 1.0,
  "segments": [
    { "volume": 0.0, "slope": 0.0003, "constant": 0.5 },
    { "volume": 5000.0, "slope": 0.0001, "constant": 2.0 }
  ]
}
```

### 3.13 Reservoir Production Factor

Piecewise-linear turbine efficiency as a function of reservoir volume
(hydraulic head).  Models the PLP "rendimiento" concept: the turbine
conversion rate varies with the current water level in the reservoir.
Corresponds to PLP **plpcenre.dat** (*Archivo de Rendimiento de Embalses*).

When the SDDP solver runs, it updates the turbine's conversion-rate LP
coefficient at each forward-pass iteration using the current reservoir
volume.  For the first iteration, the reservoir's `eini` value is used.
If the LP solver does not support in-place coefficient modification
(`supports_set_coeff()`), the static `conversion_rate` from the Turbine
element is used unchanged.

**JSON array name:** `reservoir_production_factor_array`

| Field                        | Type    | Units          | Required | Description |
|------------------------------|---------|----------------|----------|-------------|
| `uid`                        | integer | —              | Yes      | Unique identifier |
| `name`                       | string  | —              | Yes      | Production factor element name |
| `active`                     | boolean | —              | No       | Whether the element is active |
| `turbine`                    | integer\|string | —      | Yes      | Associated turbine UID or name |
| `reservoir`                  | integer\|string | —      | Yes      | Associated reservoir UID or name |
| `mean_production_factor`     | number  | MW·s/m³        | No       | Fallback production factor (default: 1.0) |
| `segments`                   | array   | —              | No       | Piecewise-linear concave segments |

Each segment in the `segments` array has the following fields:

| Field      | Type   | Units           | Description |
|------------|--------|-----------------|-------------|
| `volume`   | number | hm³             | Volume breakpoint |
| `slope`    | number | efficiency/hm³  | Slope at this breakpoint |
| `constant` | number | MW·s/m³         | Efficiency at this breakpoint (point-slope form) |

The efficiency is computed as the **minimum** over all segments:

```text
efficiency(V) = min_i { constant_i + slope_i × (V − volume_i) }
```

Segments should have slopes in **decreasing** order so the function forms
a concave envelope.  The result is clamped to zero (efficiency cannot be
negative).

**Example:**

```json
{
  "reservoir_production_factor_array": [
    {
      "uid": 1,
      "name": "eff_colbun",
      "turbine": "COLBUN",
      "reservoir": "COLBUN",
      "mean_production_factor": 1.53,
      "segments": [
        { "volume": 0.0, "slope": 0.0002294, "constant": 1.2558 },
        { "volume": 500.0, "slope": 0.0001, "constant": 1.53 }
      ]
    }
  ]
}
```

### 3.14 Reservoir Discharge Limit

Piecewise-linear volume-dependent discharge limit for reservoirs.  This is
a safety/environmental constraint that limits the hourly-average discharge
as a function of the reservoir volume (e.g. to prevent landslides or
excessive drawdown).

The constraint per stage is:

```text
qeh ≤ slope × V_avg + intercept
```

where `qeh` is the stage-average hourly discharge [m³/s], `V_avg` is the
average reservoir volume `(eini + efin) / 2` [hm³], and slope/constant are
from the active piecewise-linear segment.

Generalizes the PLP "Ralco" constraint (`plpralco.dat`).

**JSON array name:** `reservoir_discharge_limit_array`

| Field       | Type    | Units   | Required | Description |
|-------------|---------|---------|----------|-------------|
| `uid`       | integer | —       | Yes      | Unique identifier |
| `name`      | string  | —       | Yes      | Discharge limit element name |
| `active`    | boolean | —       | No       | Whether the element is active |
| `waterway`  | integer\|string | — | Yes   | Source waterway UID or name |
| `reservoir` | integer\|string | — | Yes   | Associated reservoir UID or name |
| `segments`  | array   | —       | No       | Piecewise-linear segments |

Each segment has:

| Field       | Type   | Units        | Description |
|-------------|--------|-------------|-------------|
| `volume`    | number | hm³         | Volume breakpoint |
| `slope`     | number | m³/s / hm³  | Discharge limit slope |
| `intercept` | number | m³/s        | Discharge limit intercept |

### 3.15 Generator Profile

A time-varying capacity-factor profile for a generator.

| Field       | Type                | Units | Required | Description |
|-------------|---------------------|-------|----------|-------------|
| `uid`       | integer             | —     | Yes      | Unique identifier |
| `name`      | string              | —     | Yes      | Profile name |
| `active`    | boolean             | —     | No       | Whether the profile is active |
| `generator` | integer\|string     | —     | Yes      | Associated generator UID or name |
| `profile`   | number\|array\|string| p.u. | Yes      | Capacity-factor profile (0–1) |
| `scost`     | number\|array\|string| $/MWh| No       | Short-run generation cost override |

### 3.16 Demand Profile

A time-varying load-shape profile for a demand element.

| Field    | Type                | Units | Required | Description |
|----------|---------------------|-------|----------|-------------|
| `uid`    | integer             | —     | Yes      | Unique identifier |
| `name`   | string              | —     | Yes      | Profile name |
| `active` | boolean             | —     | No       | Whether the profile is active |
| `demand` | integer\|string     | —     | Yes      | Associated demand UID or name |
| `profile`| number\|array\|string| p.u. | Yes      | Load-scaling profile (0–1) |
| `scost`  | number\|array\|string| $/MWh| No       | Short-run load-shedding cost override |

### 3.17 Reserve Zone

A spinning-reserve requirement zone.

| Field    | Type                | Units  | Required | Description |
|----------|---------------------|--------|----------|-------------|
| `uid`    | integer             | —      | Yes      | Unique identifier |
| `name`   | string              | —      | Yes      | Zone name |
| `active` | boolean             | —      | No       | Whether the zone is active |
| `urreq`  | number\|array\|string| MW    | No       | Up-reserve requirement |
| `drreq`  | number\|array\|string| MW    | No       | Down-reserve requirement |
| `urcost` | number\|array\|string| $/MW  | No       | Up-reserve shortage penalty |
| `drcost` | number\|array\|string| $/MW  | No       | Down-reserve shortage penalty |

### 3.18 Reserve Provision

A generator's contribution to reserve zones.

| Field                  | Type                | Units  | Required | Description |
|------------------------|---------------------|--------|----------|-------------|
| `uid`                  | integer             | —      | Yes      | Unique identifier |
| `name`                 | string              | —      | Yes      | Provision name |
| `active`               | boolean             | —      | No       | Whether the provision is active |
| `generator`            | integer\|string     | —      | Yes      | Associated generator UID or name |
| `reserve_zones`        | string              | —      | Yes      | Comma-separated reserve zone UIDs or names |
| `urmax`                | number\|array\|string| MW    | No       | Maximum up-reserve offer |
| `drmax`                | number\|array\|string| MW    | No       | Maximum down-reserve offer |
| `ur_capacity_factor`   | number\|array\|string| p.u.  | No       | Up-reserve capacity factor |
| `dr_capacity_factor`   | number\|array\|string| p.u.  | No       | Down-reserve capacity factor |
| `ur_provision_factor`  | number\|array\|string| p.u.  | No       | Up-reserve provision factor |
| `dr_provision_factor`  | number\|array\|string| p.u.  | No       | Down-reserve provision factor |
| `urcost`               | number\|array\|string| $/MW  | No       | Up-reserve bid cost |
| `drcost`               | number\|array\|string| $/MW  | No       | Down-reserve bid cost |

---

### 3.19 User Constraint

User-defined linear constraints applied to the LP formulation.
See **[User Constraints](user-constraints.md)** for the full syntax
reference and examples.

| Field         | Type            | Unit  | Required | Description |
|---------------|-----------------|-------|----------|-------------|
| `uid`         | integer         | —     | Yes      | Unique identifier |
| `name`        | string          | —     | Yes      | Human-readable constraint name |
| `active`      | boolean         | —     | No       | Activation flag (default: true) |
| `expression`  | string          | —     | Yes      | Constraint expression in AMPL-inspired syntax |

**System-level fields:**

| Field                     | Type            | Description |
|---------------------------|-----------------|-------------|
| `user_constraint_array`   | array           | Inline array of UserConstraint objects |
| `user_constraint_file`    | string          | Path to external JSON file with constraint array |
| `user_constraint_files`   | array of string | Paths to multiple external JSON files with constraint arrays |

### 3.20 Flow Right

Water-right constraints on waterway flow (m³/s).  See
**[Irrigation Agreements](irrigation-agreements.md)** for the full reference.

**JSON array name:** `flow_right_array`

| Field        | Type            | Units | Required | Description |
|--------------|-----------------|-------|----------|-------------|
| `uid`        | integer         | —     | Yes      | Unique identifier |
| `name`       | string          | —     | Yes      | Flow right name |
| `active`     | boolean         | —     | No       | Activation flag (default: true) |
| `waterway`   | integer\|string | —     | Yes      | Target waterway UID or name |
| `fmin`       | number\|array\|string | m³/s | No | Minimum required flow (floor right) |
| `fmax`       | number\|array\|string | m³/s | No | Maximum allowed flow (ceiling right) |
| `fcost`      | number\|array\|string | $/m³/s | No | Penalty cost per unit of right violation |

See [irrigation-agreements.md](irrigation-agreements.md) for details and examples.

### 3.21 Volume Right

Water-right constraints on reservoir volume (hm³).  See
**[Irrigation Agreements](irrigation-agreements.md)** for the full reference.

**JSON array name:** `volume_right_array`

| Field        | Type            | Units | Required | Description |
|--------------|-----------------|-------|----------|-------------|
| `uid`        | integer         | —     | Yes      | Unique identifier |
| `name`       | string          | —     | Yes      | Volume right name |
| `active`     | boolean         | —     | No       | Activation flag (default: true) |
| `reservoir`  | integer\|string | —     | Yes      | Target reservoir UID or name |
| `emin`       | number\|array\|string | hm³ | No | Minimum required volume (floor right) |
| `emax`       | number\|array\|string | hm³ | No | Maximum allowed volume (ceiling right) |
| `ecost`      | number\|array\|string | $/hm³ | No | Penalty cost per unit of right violation |
| `saving_rate`| number\|array\|string | — | No | Water-saving attribution rate |

See [irrigation-agreements.md](irrigation-agreements.md) for details and examples.

### 3.22 User Parameter

Named scalar parameters usable in user constraint expressions.

**JSON array name:** `user_param_array`

| Field   | Type    | Units | Required | Description |
|---------|---------|-------|----------|-------------|
| `uid`   | integer | —     | Yes      | Unique identifier |
| `name`  | string  | —     | Yes      | Parameter name (used in constraint expressions) |
| `value` | number\|array\|string | — | Yes | Parameter value (per block/stage/scenario schedule) |

### 3.23 Commitment

Unit commitment parameters for a generator (three-bin status/startup/shutdown
formulation).  Commitment constraints are only enforced on stages marked as
chronological — on non-chronological (duration-weighted) stages the
commitment is skipped and the generator dispatches normally.  See
formulation §5.15 for the full LP definition.

**JSON array name:** `commitment_array`

| Field                   | Type                  | Units      | Required | Description |
|-------------------------|-----------------------|------------|----------|-------------|
| `uid`                   | integer               | —          | Yes      | Unique identifier |
| `name`                  | string                | —          | Yes      | Human-readable name |
| `active`                | boolean\|array\|string| —          | No       | Activation schedule (default: active) |
| `generator`             | integer\|string       | —          | Yes      | Foreign key to a `Generator` (UID or name) |
| `startup_cost`          | number\|array\|string | $/start    | No       | Startup cost (stage-schedulable) |
| `shutdown_cost`         | number\|array\|string | $/stop     | No       | Shutdown cost |
| `noload_cost`           | number                | $/hr       | No       | No-load cost while committed |
| `min_up_time`           | number                | hours      | No       | Minimum up-time constraint |
| `min_down_time`         | number                | hours      | No       | Minimum down-time constraint |
| `ramp_up`               | number                | MW/hr      | No       | Ramp-up limit while online |
| `ramp_down`             | number                | MW/hr      | No       | Ramp-down limit while online |
| `startup_ramp`          | number                | MW         | No       | Maximum output in the startup block |
| `shutdown_ramp`         | number                | MW         | No       | Maximum output in the shutdown block |
| `initial_status`        | number                | 0 or 1     | No       | Initial on/off state at t=0 |
| `initial_hours`         | number                | hours      | No       | Hours spent in the current state at t=0 |
| `relax`                 | boolean               | —          | No       | LP relaxation: u/v/w continuous in [0,1] |
| `must_run`              | boolean               | —          | No       | Force committed: u = 1 always |
| `commitment_period`     | number                | hours      | No       | Coarser period for u/v/w (default: one per block) |
| `pmax_segments`         | array<number>         | MW         | No       | Cumulative power breakpoints for piecewise heat rate |
| `heat_rate_segments`    | array<number>         | GJ/MWh     | No       | Heat rate per segment (must match `pmax_segments` length) |
| `fuel_cost`             | number\|array\|string | $/GJ       | No       | Fuel cost (used with heat-rate segments) |
| `fuel_emission_factor`  | number\|array\|string | tCO2/GJ    | No       | Emission factor per GJ of fuel |
| `hot_start_cost`        | number                | $/start    | No       | Startup cost when recently offline |
| `warm_start_cost`       | number                | $/start    | No       | Startup cost at medium offline duration |
| `cold_start_cost`       | number                | $/start    | No       | Startup cost when long offline |
| `hot_start_time`        | number                | hours      | No       | Max offline hours for hot start |
| `cold_start_time`       | number                | hours      | No       | Min offline hours for cold start |

**JSON example:**

```json
{
  "uid": 1,
  "name": "thermal1_uc",
  "generator": "thermal1",
  "startup_cost": 5000,
  "shutdown_cost": 1000,
  "noload_cost": 50,
  "min_up_time": 4,
  "min_down_time": 2,
  "initial_status": 1,
  "initial_hours": 8
}
```

### 3.24 Simple Commitment

Simplified commitment: a single binary per block enforcing
`p ∈ [dispatch_pmin, Pmax] when u=1` and `p = 0 when u=0`.  No
startup/shutdown, no timing, no ramp.  When `relax = true` this provides
the continuous relaxation used by PLP-style inertia formulations
(see formulation §5.16).

**JSON array name:** `simple_commitment_array`

| Field           | Type                     | Units | Required | Description |
|-----------------|--------------------------|-------|----------|-------------|
| `uid`           | integer                  | —     | Yes      | Unique identifier |
| `name`          | string                   | —     | Yes      | Human-readable name |
| `active`        | boolean\|array\|string   | —     | No       | Activation schedule |
| `generator`     | integer\|string          | —     | Yes      | Foreign key to a `Generator` |
| `dispatch_pmin` | number\|array\|string    | MW    | No       | Minimum output when dispatched (defaults to generator's `pmin`) |
| `relax`         | boolean                  | —     | No       | LP relaxation: u continuous in [0,1] |
| `must_run`      | boolean                  | —     | No       | Force u = 1 always |

### 3.25 Inertia Zone

System-wide or regional minimum synchronous-inertia requirement.
Generators couple to zones via `InertiaProvision` entries (§3.26).
See formulation §5.17 for the LP constraint.

**JSON array name:** `inertia_zone_array`

| Field         | Type                     | Units  | Required | Description |
|---------------|--------------------------|--------|----------|-------------|
| `uid`         | integer                  | —      | Yes      | Unique identifier |
| `name`        | string                   | —      | Yes      | Human-readable name |
| `active`      | boolean\|array\|string   | —      | No       | Activation schedule |
| `requirement` | number\|array\|string    | MW·s   | No       | Minimum system inertia requirement |
| `cost`        | number\|array\|string    | $/MW·s | No       | Shortage penalty; when omitted, the requirement is a hard equality |

### 3.26 Inertia Provision

Links a generator to one or more inertia zones and defines its
effectiveness factor `Φ = H·S/Pmin [MW·s/MW]`.  The user may provide
either the factor directly via `provision_factor` or the raw machine
data `inertia_constant` (H) and `rated_power` (S).

**JSON array name:** `inertia_provision_array`

| Field              | Type                     | Units   | Required | Description |
|--------------------|--------------------------|---------|----------|-------------|
| `uid`              | integer                  | —       | Yes      | Unique identifier |
| `name`             | string                   | —       | Yes      | Human-readable name |
| `active`           | boolean\|array\|string   | —       | No       | Activation schedule |
| `generator`        | integer\|string          | —       | Yes      | FK to the providing generator |
| `inertia_zones`    | string                   | —       | Yes      | Colon-separated list of InertiaZone UIDs or names |
| `inertia_constant` | number                   | seconds | No       | Machine inertia constant H |
| `rated_power`      | number                   | MVA     | No       | Rated apparent power S |
| `provision_max`    | number\|array\|string    | MW      | No       | Upper bound on inertia provision (defaults to generator's `pmin`) |
| `provision_factor` | number\|array\|string    | MW·s/MW | No       | Explicit effectiveness factor (overrides H·S/Pmin) |
| `cost`             | number\|array\|string    | $/MW    | No       | Cost per MW of provision |

### 3.27 Pump

Hydraulic pump that consumes electrical power from a `Demand` and
pushes water upstream through a `Waterway` (junction_a → junction_b is
the pumping direction).  Reversible turbine-pump units (e.g. HB Maule)
are modelled as two separate elements: a `Turbine` on the generation
waterway and a `Pump` on the pumping waterway.  See formulation §5.10bis.

**JSON array name:** `pump_array`

| Field            | Type                     | Units         | Required | Description |
|------------------|--------------------------|---------------|----------|-------------|
| `uid`            | integer                  | —             | Yes      | Unique identifier |
| `name`           | string                   | —             | Yes      | Human-readable name |
| `active`         | boolean\|array\|string   | —             | No       | Activation schedule |
| `waterway`       | integer\|string          | —             | Yes      | FK to the pumping waterway |
| `demand`         | integer\|string          | —             | Yes      | FK to the electrical demand representing the pump load |
| `pump_factor`    | number\|array\|string    | MW / (m³/s)   | No       | Power consumed per unit flow |
| `efficiency`     | number\|array\|string    | p.u.          | No       | Pump efficiency (default 1.0) |
| `capacity`       | number\|array\|string    | m³/s          | No       | Maximum pump flow |
| `main_reservoir` | integer\|string          | —             | No       | Reservoir whose volume drives a variable pump factor (SDDP; future feature) |

**JSON example:**

```json
{
  "uid": 1,
  "name": "HB_MAULE_PUMP",
  "waterway": "MACHICURA_pump_33_28",
  "demand": "HB_MAULE_DEMAND",
  "pump_factor": 1.88,
  "efficiency": 0.85,
  "capacity": 40.0
}
```

### 3.28 LNG Terminal

LNG storage terminal with delivery schedule, boil-off gas, regasification,
and fuel coupling to linked thermal generators.  The tank volume balance,
delivery schedule, and generator heat-rate coupling are documented in
formulation §5.14.

**JSON array name:** `lng_terminal_array`

| Field                    | Type                     | Units              | Required | Description |
|--------------------------|--------------------------|--------------------|----------|-------------|
| `uid`                    | integer                  | —                  | Yes      | Unique identifier |
| `name`                   | string                   | —                  | Yes      | Human-readable name |
| `active`                 | boolean\|array\|string   | —                  | No       | Activation schedule |
| `emin`                   | number\|array\|string    | m³                 | No       | Minimum tank level |
| `emax`                   | number\|array\|string    | m³                 | No       | Maximum tank level |
| `ecost`                  | number\|array\|string    | $/m³               | No       | Storage holding cost |
| `eini`                   | number                   | m³                 | No       | Initial tank level |
| `efin`                   | number                   | m³                 | No       | End-of-horizon minimum level |
| `annual_loss`            | number\|array\|string    | p.u./year          | No       | Boil-off rate |
| `sendout_max`            | number                   | m³/h               | No       | Max regasification rate (default 10 000) |
| `sendout_min`            | number                   | m³/h               | No       | Min regasification rate |
| `delivery`               | number\|array\|string    | m³/stage           | No       | Scheduled LNG arrival per stage |
| `spillway_cost`          | number                   | $/m³               | No       | Penalty for venting LNG |
| `spillway_capacity`      | number                   | m³/h               | No       | Max venting rate (default 1 000) |
| `use_state_variable`     | boolean                  | —                  | No       | Propagate tank level across SDDP stages |
| `mean_production_factor` | number                   | MWh/m³             | No       | Energy content of LNG (default 5.0) |
| `scost`                  | number\|array\|string    | $/m³               | No       | SDDP state penalty (multiplied by `mpf` → $/MWh) |
| `soft_emin`              | number\|array\|string    | m³                 | No       | Soft lower bound on tank level |
| `soft_emin_cost`         | number\|array\|string    | $/m³               | No       | Penalty for crossing `soft_emin` |
| `flow_conversion_rate`   | number                   | m³/(m³/h·h)        | No       | Unit-conversion factor (default 1.0) |
| `generators`             | array<object>            | —                  | No       | Linked generators with per-link `heat_rate` (see below) |

**`generators` element (`LngGeneratorLink`)**

| Field       | Type            | Units      | Required | Description |
|-------------|-----------------|------------|----------|-------------|
| `generator` | integer\|string | —          | Yes      | FK to the consuming generator |
| `heat_rate` | number          | m³_LNG/MWh | No       | Fuel consumption per MWh (default 1.0) |

**JSON example:**

```json
{
  "uid": 1,
  "name": "GNL_Quintero",
  "emin": 5000,
  "emax": 150000,
  "eini": 80000,
  "sendout_max": 2000,
  "delivery": [50000, 0, 50000, 0],
  "spillway_cost": 100,
  "annual_loss": 0.001,
  "use_state_variable": true,
  "generators": [
    {"generator": 10, "heat_rate": 0.18},
    {"generator": 11, "heat_rate": 0.20}
  ]
}
```

---

## 4. External Data Files

When a field value is a **string** instead of a number, it refers to an
external data file in the `input_directory`.  Files are organized by
component type:

```text
<input_directory>/
├── Bus/
├── Generator/
│   └── pmax.parquet
├── Demand/
│   └── lmax.parquet
├── Line/
├── Battery/
└── ...
```

### File Format

External data files (CSV or Parquet) follow a tabular format with columns:

| Column     | Description |
|------------|-------------|
| `scenario` | Scenario UID |
| `stage`    | Stage UID |
| `block`    | Block UID |
| `uid:<N>`  | Value for element with UID N |

### Example CSV

```csv
"scenario","stage","block","uid:1"
1,1,1,10
1,2,2,15
1,3,3,8
```

---

## 5. Output Structure

After running gtopt, results are written to the `output_directory`:

```text
output/
├── solution.csv              # Objective value, status
├── Bus/
│   └── balance_dual.csv      # Marginal costs per bus
├── Generator/
│   ├── generation_sol.csv    # Generation dispatch
│   └── generation_cost.csv   # Generation costs
└── Demand/
    ├── load_sol.csv           # Served load
    ├── fail_sol.csv           # Unserved demand
    ├── capacity_dual.csv      # Capacity dual values
    └── ...
```

Output files use the same tabular format as input files, with
`scenario`, `stage`, `block`, and `uid:<N>` columns.

---

## 6. Planning (Root Container)

The `Planning` object is the root of the gtopt input data model.
A JSON configuration file represents a single `Planning` instance
with three top-level sections:

| Field        | Type       | Required | Description |
|--------------|------------|----------|-------------|
| `options`    | Options    | No       | Global solver and I/O settings (see [Section 1](#1-options)) |
| `simulation` | Simulation | Yes      | Time structure: blocks, stages, scenarios, phases, scenes, apertures (see [Section 2](#2-simulation)) |
| `system`     | System     | Yes      | Physical network components (see [Section 3](#3-system)) |

Multiple JSON files can be merged with `Planning::merge()`, allowing
the configuration to be split across files (e.g. time structure in
one file, network elements in another). The merge uses first-value
semantics for scalar options and concatenation for arrays.

**Example (minimal single-file):**

```json
{
  "options": {
    "demand_fail_cost": 1000,
    "use_single_bus": true,
    "input_directory": "input",
    "output_directory": "output"
  },
  "simulation": {
    "block_array": [{"uid": 1, "duration": 1}],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "bus_array": [{"uid": 1, "name": "bus1"}],
    "generator_array": [
      {"uid": 1, "name": "gen1", "bus": 1, "pmax": 100,
       "gcost": 20}
    ],
    "demand_array": [
      {"uid": 1, "name": "dem1", "bus": 1, "lmax": 50}
    ]
  }
}
```

---

## 7. Complete Example

See `cases/c0/system_c0.json` for a minimal working example with:
- 1 bus, 1 generator, 1 demand
- 5 time blocks, 5 stages, 1 scenario
- Demand expansion planning

---

## See also

- **[Mathematical Formulation](formulation/mathematical-formulation.md)**
  — Full LP/MIP optimization formulation with academic references
- **[Planning Guide](planning-guide.md)** — Step-by-step planning guide
  with worked examples
- **[Usage Guide](usage.md)** — Command-line options and advanced usage
- **[Scripts Guide](scripts-guide.md)** — Python conversion utilities
