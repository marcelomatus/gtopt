# SDDP Solver — Stochastic Dual Dynamic Programming in gtopt

## 1. Introduction

The **SDDP solver** in gtopt implements a **Nested Benders Decomposition**
(NBD) algorithm — also known as **Stochastic Dual Dynamic Programming**
(SDDP) — to decompose the multi-phase power system planning problem into
smaller, per-phase LP subproblems.  This enables the solver to handle
long-horizon problems (many phases/stages) that would be computationally
expensive or memory-intensive to solve monolithically.

The SDDP solver is an alternative to the default **monolithic solver**,
which assembles and solves the entire LP in one shot.  Both solvers produce
equivalent optimal solutions; the SDDP solver trades a single large LP for
multiple smaller LP solves connected by Benders optimality cuts.

### When to use SDDP

| Criterion | Monolithic | SDDP |
|-----------|-----------|------|
| Number of phases | 1–3 | ≥ 2 (designed for 5+) |
| Memory footprint | Large (full LP in memory) | Smaller (per-phase LPs) |
| State variables | Not required | Reservoir volumes, battery SoC, capacity expansion |
| Convergence guarantee | Exact (single solve) | Iterative convergence (exact at optimality) |
| Parallelism | Phases solved sequentially within one LP | Scenes solved in parallel |

---

## 2. Theoretical Background

### 2.1 Benders Decomposition

**Benders decomposition** [[1]](#ref1) is a divide-and-conquer technique for
structured linear programs.  Consider a two-stage LP:

$$
\min_{x, y} \quad c^T x + d^T y
\quad \text{s.t.} \quad Ax + By \ge b, \; x \ge 0, \; y \in Y
$$

The key insight is that for fixed first-stage decisions $\bar{y}$, the
residual (second-stage) problem in $x$ can be solved independently:

$$
\min_x \; c^T x \quad \text{s.t.} \quad Ax \ge b - B\bar{y}, \; x \ge 0
$$

The dual of this residual provides **sensitivity information** (dual
variables / reduced costs) that quantifies how changes in $\bar{y}$ affect
the second-stage cost.  This information is encoded as a **Benders
optimality cut**:

$$
\alpha \ge z^*_2 + \pi^T (y - \bar{y})
$$

where $z^*_2$ is the second-stage objective value, $\pi$ are the reduced
costs of the linking (state) variables, and $\alpha$ is an auxiliary variable
representing the second-stage cost approximation.

The master problem iteratively refines its decisions by accumulating cuts:

$$
\min_{y, \alpha} \; d^T y + \alpha \quad \text{s.t.} \; y \in Y, \;
\{\text{accumulated cuts}\}
$$

**Convergence** is guaranteed when the gap between the master lower bound
(LB) and the actual second-stage cost upper bound (UB) falls below a
tolerance.

### 2.2 Nested Benders Decomposition (SDDP)

For multi-stage problems (more than two stages), **Nested Benders
Decomposition** [[2]](#ref2) applies the Benders technique recursively.
Each stage $t$ has:

- **State variables** $x_t$ (e.g., reservoir volume, battery SoC) that
  link to stage $t+1$
- **Operating variables** (generation dispatch, line flows, demand served)
- A **future cost function** $\alpha_t$ approximated by accumulated cuts

The algorithm iterates between:

1. **Forward pass**: solve stages 1 → T sequentially, fixing state variables
   from the previous stage's solution (trial values)
2. **Backward pass**: solve stages T → 1 in reverse, computing Benders cuts
   from the dual information of each stage and adding them to the previous
   stage's LP

This is the classic **SDDP algorithm** introduced by Pereira and Pinto
[[3]](#ref3) for hydrothermal scheduling, later generalized by Shapiro
[[4]](#ref4) and others.

### 2.3 Optimality Cut Formula

In gtopt's SDDP solver, the optimality cut added to phase $t-1$ from phase
$t$ is:

$$
\alpha_{t-1} \ge z_t^* + \sum_{i \in S} \text{rc}_i \cdot (x_{t-1,i} - \hat{v}_i)
$$

where:
- $z_t^*$ is the optimal objective of phase $t$ (including its own $\alpha_t$)
- $S$ is the set of state-variable links between phases $t-1$ and $t$
- $\text{rc}_i$ is the reduced cost of the dependent column for state variable
  $i$ in phase $t$'s LP
- $x_{t-1,i}$ is the source column in phase $t-1$
- $\hat{v}_i$ is the trial value used in the forward pass

This cut is a linear outer approximation of the true future cost function.
As iterations proceed, the piecewise-linear approximation converges to the
exact cost-to-go function [[3]](#ref3)[[4]](#ref4).

### 2.4 Elastic Filter for Feasibility

When a forward-pass subproblem is infeasible (the trial values from the
previous phase violate the current phase's constraints), the solver applies
an **elastic filter** — a technique from the PLP hydrothermal scheduler
[[5]](#ref5)[[6]](#ref6).

The elastic filter:
1. **Clones** the LP (using `LinearInterface::clone()`, which calls
   `OsiSolverInterface::clone()` — the same pattern as PLP's
   `osi_lp_get_feasible_cut`)
2. **Relaxes** fixed state-variable columns to their physical source bounds
3. Adds **penalized slack variables** (`s⁺`, `s⁻`) to allow violation:
   $x_i + s_i^+ - s_i^- = \hat{v}_i$
4. **Solves** the cloned LP with the elastic objective
5. **Extracts** dual information for cut generation
6. **Discards** the clone — the original LP is never modified

This approach follows the PLP pattern in `osicallsc.cpp` (`osi_lp_get_feasible_cut`),
where the LP is cloned before applying elastic modifications, ensuring the
underlying LP state remains clean for subsequent iterations.

### 2.5 Convergence

At each iteration, the algorithm computes:

- **Lower Bound (LB)**: the phase-0 objective value (includes $\alpha_0$,
  the accumulated future cost approximation)
- **Upper Bound (UB)**: the sum of actual phase operating costs from the
  forward pass (without $\alpha$ contributions)
- **Gap**: $(UB - LB) / \max(1, |UB|)$

The algorithm terminates when the gap falls below the convergence tolerance.
For deterministic problems (single scenario), SDDP converges finitely to
the exact optimum [[3]](#ref3).  For stochastic problems, statistical
convergence criteria apply [[4]](#ref4).

---

## 3. Mapping to gtopt's Time Structure

gtopt's simulation has a hierarchical time structure:

```
Scenario (probability weight)
  └─ Phase (SDDP decomposition unit)
       └─ Stage (investment period, discount factor)
            └─ Block (operating hour, duration in hours)
```

### 3.1 Phases as SDDP Stages

The SDDP algorithm decomposes along **phases**.  Each phase contains one or
more stages and their blocks.  The phase is the natural decomposition unit
because:

- **State variables** (reservoir volumes, battery SoC, expansion capacity)
  link consecutive phases
- Within a phase, the monolithic LP handles all stages and blocks together
- Between phases, Benders cuts approximate the future cost function

| gtopt concept | SDDP role |
|--------------|-----------|
| Phase | SDDP stage (decomposition unit) |
| Stage | Time period within a phase (handled monolithically) |
| Block | Operating hour within a stage |
| Scenario | Stochastic realization |
| Scene | Parallel SDDP instance (groups scenarios) |

### 3.2 State Variables

State variables are the coupling quantities between consecutive phases:

| Component | State variable | Source column | Dependent column |
|-----------|---------------|---------------|-----------------|
| Reservoir | Volume at end of phase | `efin` (last block) | `eini` (first block of next phase) |
| Battery | State of charge at end of phase | `efin` | `eini` |
| Capacity expansion | Installed capacity | `capainst` | `capainst_ini` |

These are registered via `SystemContext::add_state_variable()` during LP
construction.  The SDDP solver discovers them generically through the
`SimulationLP::state_variables()` interface.

### 3.3 Scenes and Parallel Solving

Each **scene** groups one or more scenarios and is solved independently in
the SDDP algorithm.  Scenes are processed in parallel using the adaptive
work pool (`AdaptiveWorkPool`).  Cut sharing between scenes is controlled
by the `cut_sharing_mode` option:

| Mode | Behaviour |
|------|----------|
| `none` | Each scene uses only its own cuts |
| `expected` | Average cut across all scenes is shared |
| `max` | All cuts from all scenes are shared to all scenes |

---

## 4. Algorithm Details

### 4.1 Initialization

1. **Bootstrap solve**: the monolithic solver runs once to establish a
   baseline solution and warm-start the LP basis
2. **Add α variables**: a future-cost variable $\alpha$ is added to every
   phase except the last, with bounds `[alpha_min, alpha_max]` and
   objective coefficient 1.0
3. **Discover state links**: for each phase, the solver enumerates all
   state variables and their dependent columns in the next phase
4. **Load hot-start cuts** (optional): previously saved cuts are loaded
   from a file to accelerate convergence

### 4.2 Forward Pass

For each scene (in parallel):

```
for phase = 0 to T-1:
    if phase > 0:
        propagate trial values from phase-1 solution
        (fix dependent columns to source column values)
    
    solve phase LP
    
    if infeasible:
        clone LP → apply elastic filter → solve clone
        use clone's solution for cost/cut data
        (original LP remains unmodified)
    
    cache: forward_full_obj, forward_col_cost (reduced costs)
    
    opex += objective - α value
```

### 4.3 Backward Pass

For each scene (in parallel):

```
for phase = T-1 down to 1:
    build Benders cut from cached reduced costs:
        α_{phase-1} ≥ z_phase + Σ rc_i · (x_{phase-1,i} - v̂_i)
    
    add cut to phase-1 LP
    store cut for persistence
    
    if phase > 1:
        re-solve phase-1 LP
        if infeasible:
            iterative feasibility backpropagation
            (clone + elastic + cut to phase-2, etc.)
```

### 4.4 Convergence Check

```
LB = average of phase-0 objectives across scenes
UB = average of total forward-pass costs across scenes
gap = (UB - LB) / max(1, |UB|)
converged = (gap < convergence_tol)
```

### 4.5 Cut Sharing (Optional)

After the backward pass, cuts from all scenes are optionally shared.  In
`expected` mode, an average cut is computed and added to all scenes.  In
`max` mode, every cut from every scene is added to all other scenes.

### 4.6 Sentinel File Stop

The solver checks for a **sentinel file** (configurable via
`sentinel_file` in `SDDPOptions`) at the beginning of each iteration.  If
the file exists, the solver stops gracefully after saving all accumulated
cuts.  This is analogous to PLP's `userstop` mechanism.

### 4.7 Incremental Cut Saving

Cuts are saved to the output file after **every iteration** (not just at
the end).  This ensures that if the solver is interrupted — whether by the
sentinel file, a time limit, or an external signal — the accumulated cuts
are available for a subsequent hot-start run.

### 4.8 Solver API for Monitoring and Control

The `SDDPSolver` exposes a thread-safe API designed for GUI integration,
external monitoring tools, and programmatic control of the solve process:

**Iteration callback** — register an `SDDPIterationCallback` via
`set_iteration_callback()`.  It is invoked after every iteration with the
full `SDDPIterationResult` (iteration number, lower/upper bound, gap,
cuts added, feasibility flags).  Return `true` from the callback to
request an immediate stop.

```cpp
sddp.set_iteration_callback([](const SDDPIterationResult& r) {
    fmt::print("iter {} gap={:.6f}\n", r.iteration, r.gap);
    return r.gap < 1e-6;  // true → stop
});
```

**Programmatic stop** — call `request_stop()` from any thread; the solver
checks this atomic flag at the start of each iteration and exits
gracefully, saving all accumulated cuts.

```cpp
// From a UI thread or signal handler:
sddp.request_stop();
```

**Live query** — poll the solver's convergence state at any time via
atomic accessors.  These are safe to call from any thread during solve:

| Method | Returns |
|--------|---------|
| `current_iteration()` | Current iteration number (0 before start) |
| `current_gap()` | Current relative convergence gap |
| `current_lower_bound()` | Current LB (phase-0 obj including α) |
| `current_upper_bound()` | Current UB (sum of forward-pass costs) |
| `has_converged()` | Whether convergence tolerance has been met |
| `num_stored_cuts()` | Number of accumulated Benders cuts |
| `is_stop_requested()` | Whether a stop has been requested |

**Data access** — after solving (or mid-solve via callback), the full
per-phase state is available via `phase_states(scene)`, and all stored
cuts via `stored_cuts()`.

---

## 5. Configuration

### 5.1 SDDPOptions

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `max_iterations` | int | 100 | Maximum SDDP iterations |
| `convergence_tol` | double | 1e-4 | Relative gap tolerance |
| `elastic_penalty` | double | 1e6 | Penalty cost for elastic slack variables |
| `alpha_min` | double | 0.0 | Lower bound for α variables |
| `alpha_max` | double | 1e12 | Upper bound for α variables |
| `cut_sharing` | CutSharingMode | None | Cut sharing strategy between scenes |
| `sentinel_file` | string | "" | Path to sentinel file for graceful stop |
| `cuts_output_file` | string | "" | Path for saving cuts (CSV format) |
| `cuts_input_file` | string | "" | Path for loading cuts (hot-start) |

### 5.2 Options (JSON)

```json
{
  "options": {
    "solver_type": "sddp",
    "cut_sharing_mode": "expected",
    "cut_directory": "cuts",
    "log_directory": "logs"
  }
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `solver_type` | string | `"monolithic"` | Solver to use: `"monolithic"` or `"sddp"` |
| `cut_sharing_mode` | string | `"none"` | Cut sharing: `"none"`, `"expected"`, or `"max"` |
| `cut_directory` | string | `"cuts"` | Directory for Benders cut files |
| `log_directory` | string | `"logs"` | Directory for log and trace files |

### 5.3 CLI

```bash
gtopt my_case.json --trace-log sddp_trace.log --cut-directory cuts --log-directory logs
```

| Flag | Description |
|------|-------------|
| `--trace-log <file>` | Capture all `SPDLOG_TRACE` messages to a file |
| `--cut-directory <dir>` | Directory for Benders cut files (default: `cuts`) |
| `--log-directory <dir>` | Directory for log and trace files (default: `logs`) |

The `--trace-log` option captures all `SPDLOG_TRACE` messages to a file,
providing detailed iteration-by-iteration data including:
- Forward pass phase objectives and α values
- Backward pass cut coefficients and RHS values
- Elastic filter activations
- Convergence metrics

### 5.4 Per-Scene Cut Files

When `cuts_output_file` is set, the solver saves both a combined cut file
and per-scene files to avoid write contention during parallel backward
passes:

```
cuts/
├── sddp_cuts.csv      # Combined cuts from all scenes
├── scene_0.csv         # Cuts generated by scene 0
├── scene_1.csv         # Cuts generated by scene 1
└── ...
```

Each per-scene file has the same CSV format as the combined file. This
allows concurrent scene processing to write independently without
locking. Cut files for infeasible scenes are automatically removed.

### 5.5 Infeasible Scene Handling

When one or more scenes are infeasible during the forward pass:

1. **Skip**: the infeasible scene is excluded from lower/upper bound
   computation and from the backward pass.
2. **Save error LP**: the infeasible LP is written to `log_directory` as
   `error_scene_<N>_phase_<M>.lp` for debugging.
3. **Rename cut files**: per-scene cut files for infeasible scenes are
   renamed with an `error_` prefix (e.g. `error_scene_0.csv`) instead of
   being removed, preserving diagnostic data.
4. **Continue**: the solver continues with remaining feasible scenes.
5. **Error**: if **all** scenes are infeasible, the solver returns an error
   and `gtopt_main` exits with a non-zero exit code.

---

## 6. Integration Tests

Three integration tests validate that the SDDP solver produces the same
optimal solution as the monolithic solver.  Each test exercises a different
type of state-variable coupling between phases: reservoir volume, reservoir
depletion dynamics, and capacity expansion.  A fourth test validates
scalability with a year-long 12-phase hydro simulation.

### 6.1 Reservoir Test (`make_5phase_reservoir_planning`)

**System configuration:**
- 1 bus (single-bus / copper-plate mode)
- 1 hydro generator: 50 MW, $5/MWh
- 1 thermal generator: 200 MW, $80/MWh
- 1 demand: 100 MW constant
- 1 reservoir: 500 dam³ capacity, starts at 300 dam³
- Natural inflow: 8 dam³/h via junction topology (2 junctions, 1 waterway,
  1 turbine)

**Time structure:**
- 5 phases × 1 stage each × 8 blocks of 3 hours = 120 hours total
- 1 scenario, 1 scene

**Expected behaviour:** the reservoir carries water value across phases.
The hydro generator displaces expensive thermal generation when water is
available.

**Results:**
| Solver | Total cost | Phase-0 obj | Iterations | Gap |
|--------|-----------|-------------|------------|-----|
| Monolithic | 865,500 | 155,100 | 1 (single LP) | — |
| SDDP | 865,500 | 155,100 | 5 | 0.000000 |

The SDDP solver converges to the exact monolithic solution in 5 iterations
with zero gap.

### 6.2 Small Reservoir Test (`make_5phase_small_reservoir_planning`)

**System configuration:**
- 1 bus (single-bus / copper-plate mode)
- 1 hydro generator: 80 MW, $3/MWh
- 1 thermal generator: 200 MW, $80/MWh
- 1 demand: 100 MW constant
- 1 small reservoir: 200 dam³ capacity, starts at 180 dam³
- Low natural inflow: 5 dam³/h (reservoir depletes over time)

**Time structure:**
- 5 phases × 1 stage each × 8 blocks of 3 hours = 120 hours total
- 1 scenario, 1 scene

**Expected behaviour:** the reservoir is small relative to demand, so
it depletes across phases.  This creates non-trivial state-variable
values at phase boundaries, forcing the SDDP cuts to properly capture
the marginal water value.

**Results:**
| Solver | Total cost | Iterations | Gap |
|--------|-----------|------------|-----|
| Monolithic | 899,940 | 1 (single LP) | — |
| SDDP | 899,940 | 5 | 0.000000 |

### 6.3 Expansion Test (`make_5phase_expansion_planning`)

**System configuration:**
- 1 bus (single-bus / copper-plate mode)
- 1 expandable generator: 0 MW initial capacity, 50 MW/module,
  max 10 modules, $80/MWh operating cost, $500/module-year investment
- 1 backup generator: 200 MW, $200/MWh (expensive "peaker")
- 1 demand: 100 MW constant

**Time structure:**
- 5 phases × 1 stage each × 8 blocks of 3 hours = 120 hours total
- 1 scenario, 1 scene

**Expected behaviour:** the expandable generator starts at 0 MW and
must invest in capacity modules to displace the expensive backup.
The `capainst` (installed-capacity) state variable links across phases,
ensuring that capacity built in phase $t$ is available in phase $t+1$.
The solver finds the optimal expansion plan that minimizes total
discounted CAPEX + OPEX.

**Results:**
| Solver | Total cost | Iterations | Gap |
|--------|-----------|------------|-----|
| Monolithic | 960,685 | 1 (single LP) | — |
| SDDP | 960,685 | 5 | 0.000000 |

### 6.4 Yearly Hydro Test (`make_12phase_yearly_hydro_planning`)

**System configuration:**
- 1 bus (single-bus / copper-plate mode)
- 1 hydro generator: 25 MW, $5/MWh
- 1 thermal generator: 200 MW, $80/MWh
- 1 demand: 50 MW constant
- 1 reservoir: 150 dam³ capacity, starts at 100 dam³
- Inflow: 10 dam³/h (constant; seasonal patterns can be added)
- 12 phases × 1 stage × 24 hourly blocks = 288 blocks total

**What it tests:**
- Scalability to a full year (12 monthly phases, each with a
  representative 24-hour day)
- Reservoir state coupling across 12 consecutive phases
- Convergence behaviour with a longer planning horizon

**Expected behaviour:**
The monolithic solver solves all 12 phases jointly in one LP.  The SDDP
solver decomposes the problem into 12 monthly subproblems, building
Benders cuts to approximate the future cost of water.  Both should produce
the same total cost (within 5% tolerance due to cut approximation).

### 6.5 Why the Results Match Exactly

For deterministic problems (single scenario, no stochastic uncertainty),
the SDDP algorithm is equivalent to Benders decomposition on a
deterministic multi-stage LP.  With linear cost-to-go functions (no
integer variables), the piecewise-linear approximation built by the Benders
cuts converges to the exact cost-to-go function in a finite number of
iterations [[1]](#ref1)[[3]](#ref3).  The 5-phase test cases are small
enough that convergence is achieved quickly.

---

## 7. Implementation Architecture

### 7.1 Key Classes

| Class | File | Role |
|-------|------|------|
| `SDDPSolver` | `sddp_solver.hpp/cpp` | Core SDDP algorithm |
| `SDDPPlanningSolver` | `sddp_solver.hpp/cpp` | `PlanningSolver` interface adapter |
| `MonolithicSolver` | `planning_solver.hpp/cpp` | Default full-LP solver |
| `PlanningLP` | `planning_lp.hpp/cpp` | LP assembly and phase management |
| `LinearInterface` | `linear_interface.hpp/cpp` | LP solver abstraction (COIN-OR) |
| `AdaptiveWorkPool` | `work_pool.hpp` | Parallel scene processing |

### 7.2 Free Functions

| Function | Purpose |
|----------|---------|
| `propagate_trial_values()` | Fix dependent columns to source values |
| `build_benders_cut()` | Construct optimality cut from reduced costs |
| `relax_fixed_state_variable()` | Apply elastic relaxation to one column |
| `average_benders_cut()` | Average multiple cuts (for `expected` sharing) |
| `parse_cut_sharing_mode()` | Parse string to `CutSharingMode` enum |

### 7.3 LP Clone Pattern

The `LinearInterface::clone()` method (added for the SDDP elastic filter)
uses `OsiSolverInterface::clone(true)` to create a deep copy of the LP
state.  This follows the PLP pattern in `osicallsc.cpp` where the elastic
filter operates on a clone to avoid permanently modifying the original LP:

```cpp
auto cloned = li.clone();          // Deep copy via OsiSolverInterface::clone()
relax_fixed_state_variable(cloned, link, phase, penalty);  // Modify clone
auto result = cloned.resolve(opts);  // Solve clone
auto rc = cloned.get_col_cost();     // Extract dual information
// Clone is destroyed when it goes out of scope.
// Original LP `li` is untouched.
```

### 7.4 Thread Safety

The SDDP solver uses two levels of cut storage:

1. **Per-scene storage** (`m_scene_cuts_`): each scene writes its own
   vector without any mutex, preventing lock contention during parallel
   backward passes.  Per-scene cut files (`scene_N.csv`) are written from
   this storage.

2. **Shared storage** (`m_stored_cuts_`): protected by `m_cuts_mutex_`,
   used for cut sharing between scenes and the combined cut file.

Each scene has its own per-phase LP subproblems, so forward/backward
passes for different scenes can proceed in parallel without LP-level
locking.

---

## 8. Relationship to PLP

The gtopt SDDP solver draws on concepts from the PLP (Programación Lineal
de la Planificación) hydrothermal scheduler [[5]](#ref5)[[6]](#ref6)
maintained in the `marcelomatus/plp_storage` repository:

| PLP concept | gtopt equivalent |
|-------------|-----------------|
| SDDP forward/backward iteration | `SDDPSolver::forward_pass()` / `backward_pass()` |
| Elastic filter (`osi_lp_get_feasible_cut`) | `SDDPSolver::elastic_solve()` via `clone()` |
| LP cloning (`LPCont::get_lpi()`) | `LinearInterface::clone()` |
| `userstop` file | `sentinel_file` option in `SDDPOptions` |
| Cut file persistence | `save_cuts()` / `load_cuts()` in CSV format |
| `scloning` mode | Always-clone approach for elastic filter |

The PLP code (`CEN65/src/osicallsc.cpp`) uses `OsiSolverInterface::clone()`
in `osi_lp_get_feasible_cut` to create a temporary LP copy, zero the
original objective, add elastic slack variables, solve for feasibility,
extract the dual ray, and discard the clone.  gtopt's `elastic_solve()`
follows the same pattern.

---

## 9. References

<a id="ref1"></a>
**[1]** J. F. Benders, "Partitioning procedures for solving mixed-variables
programming problems," *Numerische Mathematik*, vol. 4, pp. 238–252, 1962.
DOI: [10.1007/BF01386316](https://doi.org/10.1007/BF01386316)

<a id="ref2"></a>
**[2]** A. M. Geoffrion, "Generalized Benders decomposition," *Journal of
Optimization Theory and Applications*, vol. 10, no. 4, pp. 237–260, 1972.
DOI: [10.1007/BF00934810](https://doi.org/10.1007/BF00934810)

<a id="ref3"></a>
**[3]** M. V. F. Pereira and L. M. V. G. Pinto, "Multi-stage stochastic
optimization applied to energy planning," *Mathematical Programming*,
vol. 52, pp. 359–375, 1991.
DOI: [10.1007/BF01582895](https://doi.org/10.1007/BF01582895)

<a id="ref4"></a>
**[4]** A. Shapiro, "Analysis of stochastic dual dynamic programming method,"
*European Journal of Operational Research*, vol. 209, no. 1, pp. 63–72,
2011. DOI: [10.1016/j.ejor.2010.08.007](https://doi.org/10.1016/j.ejor.2010.08.007)

<a id="ref5"></a>
**[5]** M. Pereira-Bonvallet, M. Matus, et al., "Stochastic hydrothermal
scheduling under CO2 emissions constraints," *Energy Procedia*, vol. 87,
pp. 63–72, 2016.
DOI: [10.1016/j.egypro.2015.12.359](https://doi.org/10.1016/j.egypro.2015.12.359)

<a id="ref6"></a>
**[6]** M. V. F. Pereira, "Optimal stochastic operations scheduling of
large hydroelectric systems," *International Journal of Electrical Power &
Energy Systems*, vol. 11, no. 3, pp. 161–169, 1989.
DOI: [10.1016/0142-0615(89)90025-2](https://doi.org/10.1016/0142-0615(89)90025-2)

<a id="ref7"></a>
**[7]** A. Philpott and Z. Guan, "On the convergence of stochastic dual
dynamic programming and related methods," *Operations Research Letters*,
vol. 36, no. 4, pp. 450–455, 2008.
DOI: [10.1016/j.orl.2008.01.013](https://doi.org/10.1016/j.orl.2008.01.013)

<a id="ref8"></a>
**[8]** V. L. de Matos, A. B. Philpott, and E. C. Finardi, "Improving the
performance of stochastic dual dynamic programming," *Journal of
Computational and Applied Mathematics*, vol. 290, pp. 196–208, 2015.
DOI: [10.1016/j.cam.2015.04.048](https://doi.org/10.1016/j.cam.2015.04.048)

<a id="ref9"></a>
**[9]** T. Homem-de-Mello, V. L. de Matos, and E. C. Finardi, "Sampling
strategies and stopping criteria for stochastic dual dynamic programming:
a case study in long-term hydrothermal scheduling," *Energy Systems*,
vol. 2, pp. 1–31, 2011.
DOI: [10.1007/s12667-011-0024-y](https://doi.org/10.1007/s12667-011-0024-y)

<a id="ref10"></a>
**[10]** B. Stott, J. Jardim, and O. Alsaç, "DC power flow revisited,"
*IEEE Transactions on Power Systems*, vol. 24, no. 3, pp. 1290–1300, 2009.
DOI: [10.1109/TPWRS.2009.2021235](https://doi.org/10.1109/TPWRS.2009.2021235)

<a id="ref11"></a>
**[11]** R. Romero and A. Monticelli, "A hierarchical decomposition approach
for transmission network expansion planning," *IEE Proceedings — Generation,
Transmission and Distribution*, vol. 141, no. 5, pp. 465–473, 1994.
DOI: [10.1049/ip-gtd:19941354](https://doi.org/10.1049/ip-gtd:19941354)

<a id="ref12"></a>
**[12]** S. Lumbreras and A. Ramos, "The new challenges to transmission
expansion planning. Survey of recent practice and literature review,"
*Electric Power Systems Research*, vol. 134, pp. 19–29, 2016.
DOI: [10.1016/j.epsr.2015.10.013](https://doi.org/10.1016/j.epsr.2015.10.013)

<a id="ref13"></a>
**[13]** J. Forrest and R. Lougee-Heimer, "CBC User Guide," in
*Emerging Theory, Methods, and Applications*, INFORMS, pp. 257–277, 2005.
DOI: [10.1287/educ.1053.0020](https://doi.org/10.1287/educ.1053.0020)

<a id="ref14"></a>
**[14]** L. Buitrago Villada, M. Pereira-Bonvallet, M. Matus, et al.,
"FESOP: A framework for electricity system optimization and planning,"
*IEEE Kansas Power and Energy Conference (KPEC)*, 2022.
DOI: [10.1109/KPEC54747.2022.9814758](https://doi.org/10.1109/KPEC54747.2022.9814758)

---

## 10. See Also

- [MATHEMATICAL_FORMULATION.md](formulation/MATHEMATICAL_FORMULATION.md) —
  full LP/MIP formulation for gtopt
- [PLANNING_GUIDE.md](../PLANNING_GUIDE.md) — worked examples and time
  structure concepts
- [INPUT_DATA.md](../INPUT_DATA.md) — JSON/Parquet input format
  specification
- [USAGE.md](../USAGE.md) — CLI reference including `--trace-log`
- [CONTRIBUTING.md](../CONTRIBUTING.md) — code style and testing guidelines
