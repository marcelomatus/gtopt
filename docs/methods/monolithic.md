# Monolithic Solver

## 1. Introduction

The **monolithic solver** is the default solver in gtopt.  It assembles the
full LP formulation for each scene (across all phases and stages) and solves
it in a single call to the LP solver.  Different scenes are solved in
parallel using the adaptive work pool.

The monolithic solver is the simplest and most robust approach: it produces
the exact optimal solution in one solve, without iterative decomposition.
For problems with many phases and state variables, the SDDP solver may be
more efficient (see [SDDP Method](sddp.md)).

### When to use the monolithic solver

| Criterion | Monolithic | SDDP |
|-----------|:----------:|:----:|
| Default solver | Yes | No (set `method: "sddp"`) |
| Number of phases | Any (1+) | 2+ required |
| Solution quality | Exact (single LP solve) | Iterative convergence |
| Memory | Full LP in memory per scene | Per-phase LPs (smaller) |
| State variables | Handled implicitly (shared LP) | Explicit Benders cuts |
| Boundary cuts | Supported (optional) | Supported (standard) |

---

## 2. Solver Modes

The monolithic solver supports two solve modes, configured via
`monolithic_options.solve_mode`:

### 2.1 `"monolithic"` (default)

All phases are assembled into a single LP per scene.  State variables
(reservoir volumes, battery SoC) are shared columns within the same LP,
so inter-phase coupling is handled implicitly.  This is the most robust
mode and produces the globally optimal solution.

### 2.2 `"sequential"` (planned)

Phases are solved sequentially within each scene, propagating state
variable values from phase $t$ to phase $t+1$.  This mode uses less
memory (one phase LP in memory at a time) but may require boundary cuts
for the last phase to approximate future costs.

> **Note**: In the current implementation, `resolve_scene_phases()`
> already solves phases sequentially.  The "sequential" mode flag is
> reserved for future optimizations (lazy phase construction).

---

## 3. Equivalence with SDDP

Under specific conditions, the monolithic and SDDP solvers produce
**identical optimal solutions**.  This equivalence is guaranteed when:

1. **No apertures** (`num_apertures` = number of scenarios, or
   aperture sampling disabled) --- SDDP backward pass sees the same
   scenarios as the forward pass.

2. **No cut sharing** (`cut_sharing: "none"`) --- cuts are not
   broadcast across scenes.

3. **SDDP has converged** --- the lower bound (from cuts) equals the
   upper bound (from forward simulation) within tolerance.

### When equivalence breaks

| Condition | Effect |
|-----------|--------|
| Aperture sampling (subset of scenarios) | SDDP backward pass uses a subset; monolithic uses all |
| Cut sharing (`"max"` or `"expected"`) | SDDP shares cuts across scenes; monolithic has implicit coupling |
| SDDP not converged | SDDP lower bound < true optimum |
| No state variables | Both are equivalent (no inter-phase coupling) |

### Formal equivalence

For a deterministic problem (single scenario) with $T$ phases, the
monolithic LP is:

$$\min \sum_{t=1}^{T} c_t^T x_t \quad \text{s.t.} \quad
A_t x_t + B_t x_{t-1} \ge b_t \; \forall t$$

The SDDP decomposition solves this via iterative forward/backward
passes.  At convergence, the SDDP cuts exactly represent the
recourse function $Q_t(x_{t-1})$ for each phase, and the SDDP
solution matches the monolithic optimum [[1]](#ref1).

For stochastic problems with multiple scenarios, equivalence holds
when the backward pass evaluates all scenarios (no aperture sampling)
and cuts are not shared across scenes.

---

## 4. Configuration

### 4.1 JSON Options

```json
{
  "options": {
    "method": "monolithic",
    "monolithic_options": {
      "solve_mode": "monolithic",
      "solve_timeout": 18000,
      "boundary_cuts_mode": "separated",
      "boundary_max_iterations": 0
    }
  },
  "simulation": {
    "boundary_cuts_file": "boundary_cuts.csv",
    "boundary_cuts_valuation": "end_of_horizon"
  }
}
```

### 4.2 Option Reference

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `solve_mode` | string | `"monolithic"` | `"monolithic"` or `"sequential"` |
| `solve_timeout` | double | 18000.0 | LP solve timeout in seconds (0 = no timeout) |
| `boundary_cuts_mode` | string | `"separated"` | `"noload"`, `"separated"`, or `"combined"` |
| `boundary_max_iterations` | int | `0` (all) | Max iterations to load from boundary file |

### 4.3 Solve Timeout

The monolithic solver has a default `solve_timeout` of **18,000 seconds**
(300 minutes / 5 hours).  This is intentionally much longer than the SDDP
default (180 seconds) because monolithic solves handle the full LP in a
single call.

When a scene's LP solve exceeds the timeout, the solver writes the LP to
a debug file in `log_directory` and returns an error for that scene.  The
remaining scenes continue solving.

Set `solve_timeout` to `0` to disable the time limit entirely.

### 4.4 Boundary Cuts

Boundary cuts approximate the expected future cost beyond the planning
horizon.  They are typically generated by a prior SDDP run or an
external model.

> **Note**: `boundary_cuts_file` has moved to the `simulation`
> section.  For backward compatibility, it is still accepted in
> `monolithic_options`.  The `simulation` section also accepts
> `boundary_cuts_valuation` (`"end_of_horizon"` or
> `"present_value"`).

When `boundary_cuts_file` is set, the monolithic solver loads the cuts
into the last phase of each scene's LP **before** the parallel solve
dispatch.  This adds a future-cost variable ($\alpha$) to the objective
and constrains it via the loaded cuts:

$$\alpha \ge \text{rhs}_k + \sum_j \pi_{k,j} \cdot x_j
\quad \forall k \in \text{cuts}$$

**Load modes** (`boundary_cuts_mode`):

| Mode | Description |
|------|-------------|
| `"noload"` | Skip loading even if a file is specified |
| `"separated"` (default) | Each cut assigned to its matching scene UID |
| `"combined"` | All cuts broadcast to all scenes |

**Iteration filtering** (`boundary_max_iterations`):

When set to a positive integer $N$, only cuts from the last $N$ distinct
SDDP iterations (by the `iteration` column) are loaded.  Set to `0` to
load all cuts (default).

The CSV format is identical to the SDDP boundary cuts format
(see [SDDP Method](sddp.md)).

---

## 5. Implementation

### 5.1 Architecture

```
gtopt_main()
  |
  +-> validate_planning(planning)   ← JSON input validation
  |
  +-> PlanningLP::resolve()
        |
        +-> make_planning_method(options, num_phases)
        |     |
        |     +-> MonolithicMethod  (default, or SDDP fallback for 1 phase)
        |     +-> SDDPPlanningMethod (when method="sddp" and phases >= 2)
        |
        +-> solver->solve(planning_lp, lp_opts)
```

### 5.2 Input Validation

Before LP construction, `gtopt_main()` calls `validate_planning()` to
check the JSON input for structural correctness: missing required fields,
invalid UIDs, inconsistent array sizes, and other semantic errors.
Validation errors are reported with specific messages and the solver
exits before any LP is assembled.

### 5.3 MonolithicMethod::solve()

1. **Load boundary cuts** (if configured) --- before parallel dispatch
2. **Write LP debug files** (if `lp_debug` enabled)
3. **Parallel scene dispatch** --- each scene submitted to work pool
4. **Per-scene**: `resolve_scene_phases()` solves phases sequentially,
   propagating state variable values between phases
5. **Collect results** --- wait for all futures, report timing

### 5.4 Log Directory

By default, the `log_directory` resolves to `<output_directory>/logs`
(e.g., `output/logs`), consolidating all solver output under a single
root directory.  Both the monolithic and SDDP solvers use this directory
for error LP dumps and diagnostic files.  Set `log_directory` explicitly
in the JSON to override this default.

### 5.5 Single-Phase SDDP Fallback

When `method: "sddp"` is requested but only 1 phase exists, the
factory automatically falls back to the monolithic solver with an
informational log message.  This prevents the SDDP "requires at least
2 phases" error.

---

## 6. References

<a id="ref1"></a>
[1] J. F. Benders, "Partitioning procedures for solving mixed-variables
programming problems," *Numerische Mathematik*, vol. 4, pp. 238--252,
1962. DOI: [10.1007/BF01386316](https://doi.org/10.1007/BF01386316)

---

## See Also

- [SDDP Method](sddp.md) --- SDDP solver documentation
  (iterative decomposition, cut persistence, hot-start)
- [Usage Guide](../usage.md) --- CLI reference and output interpretation
- [Input Data Reference](../input-data.md) --- JSON input format specification
- [Mathematical Formulation](../formulation/mathematical-formulation.md)
  --- LP/MIP formulation details
- [Planning Guide](../planning-guide.md) --- worked examples and time
  structure concepts
