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

The algorithm terminates when the gap falls below the convergence tolerance
(**primary criterion**).  For deterministic problems (single scenario),
SDDP converges finitely to the exact optimum [[3]](#ref3).  For stochastic
problems, statistical convergence criteria apply [[4]](#ref4).

#### Secondary criterion — stationary gap

Some problems converge to a **non-zero stationary gap** due to stochastic
noise, problem structure, or numerical conditioning.  The gap stops
improving but never reaches the primary tolerance $\varepsilon$.  The
**stationary-gap criterion** detects this plateau:

```
gap_change = |gap[k] − gap[k − window]| / max(1e-10, gap[k − window])
if gap_change < stationary_tol → declare convergence
```

When triggered, the solver sets `converged = true` and
`stationary_converged = true`, allowing downstream tools to distinguish
primary from secondary convergence.

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
| `none` (default) | Each scene uses only its own cuts; scenes are solved independently in parallel with no synchronization |
| `expected` | Probability-weighted average cut across all scenes is shared |
| `accumulate` | Sum of all scene cuts shared (correct when LP objectives include probability factors) |
| `max` | All cuts from all scenes are shared to all scenes |

**Feasibility cuts** are never shared between scenes regardless of the cut
sharing mode — they remain local to the originating scene.

When cut sharing is **disabled** (`none`), each scene's backward pass runs
its full phase sweep independently in parallel, with no waiting or coupling
between scenes.

When cut sharing is **enabled** (any mode other than `none`), the backward
pass is **synchronized per-phase**: all scenes complete the backward step
for a given phase, then optimality cuts are shared across scenes for that
phase before proceeding to the previous phase.  This allows shared cuts to
inform earlier phases within the same iteration.

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
5. **Load boundary cuts** (optional): external future-cost cuts for the
   last phase are loaded from a CSV file (see §4.11)

### 4.2 Forward Pass

For each scene (in parallel):

```
for phase = 0 to T-1:
    if phase > 0:
        propagate trial values from phase-1 solution
        (fix dependent columns to source column values)
    
    update LP coefficients (see §4.6)
    solve phase LP
    
    if infeasible:
        clone LP → apply elastic filter → solve clone
        use clone's solution for cost/cut data
        (original LP remains unmodified)
    
    cache: forward_full_obj; mirror reduced costs onto each StateVariable
    
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

### 4.4 Backward Pass with Apertures

When `num_apertures ≠ 0`, the backward pass uses **apertures** (also called
hydrological openings in PLP) to compute the expected future-cost cut.
Instead of using the cached forward-pass solution to build a single Benders
cut, the solver solves the phase LP once per aperture, each time with
different stochastic parameters (flow/inflow values), and averages the
resulting cuts:

```
for phase = T-1 down to 1:
    for ap = 0 to num_apertures-1:
        clone the phase LP
        update scenario-dependent bounds for aperture ap
        solve clone via work pool
        if optimal:
            build Benders cut from clone's reduced costs
            store cut with probability weight

    if no apertures produced a valid cut:
        skip this phase

    compute expected cut = probability-weighted average of aperture cuts
    add expected cut to phase-1 LP
    store cut for persistence

    if phase > 1:
        re-solve phase-1 LP via work pool
```

**Aperture data updates**: When switching to an aperture scenario, the solver
updates all scenario-dependent LP elements via a unified `update_aperture()`
method.  Each element that satisfies the `HasUpdateAperture` concept receives
an `ApertureValueFn` — a `std::function<std::optional<double>(StageUid, BlockUid)>`
— that provides the new values.  The value function abstracts over two data
sources:

- **LP scenario data** (when the aperture's source scenario is in the forward
  set): reads directly from the element's schedule data via `aperture_value()`.
- **Aperture data cache** (when the source scenario is external): looks up
  pre-loaded parquet data via `ApertureDataCache::lookup()`.

| Element | What is updated | Mechanism |
|---------|----------------|-----------|
| `FlowLP` | Discharge column bounds (m³/s) | `set_col_low` / `set_col_upp` |
| `GeneratorProfileLP` | Profile constraint (capacity factor) | Row coefficient or RHS |
| `DemandProfileLP` | Profile constraint (demand factor) | Row coefficient or RHS |

This ensures that hydro inflows, solar/wind capacity factors, and demand
forecast uncertainty are all varied across aperture scenarios during the
backward pass.  State variable bounds remain fixed at the forward-pass
trial values.

**Aperture data cache**: The `ApertureDataCache` loads all `*.parquet` files
from the `sddp_aperture_directory` at construction time into a `flat_map`
keyed by `(class_name, element_name, scenario_uid, stage_uid, block_uid)`.
The directory structure is `aperture_dir/ClassName/ElementName.parquet`,
with each parquet file containing `stage`, `block`, and `uid:N` columns
(one per scenario UID).  Loading is O(n log n) via vectorized bulk insert.

**Configuration**:
- `num_apertures = 0` — disabled (default, uses standard backward pass)
- `num_apertures = -1` — use all available scenarios as apertures
- `num_apertures = N > 0` — use the first N scenarios (capped at total)

**PLP correspondence**: In PLP (`CEN65/src/osicallsc.cpp`), apertures are
called "aberturas hidrologicas" (hydrological openings).  PLP iterates over
all hydrological realizations for each stage, solves each one, and computes
the expected cut weighted by scenario probabilities.  The gtopt implementation
follows the same pattern: clone the LP, update scenario-dependent bounds,
solve, collect cuts, and compute the probability-weighted average.

### 4.5 Convergence Check

Two convergence criteria are evaluated at the end of each **training
iteration** (a paired forward + backward pass that generates cuts).
The simulation pass (see S4.5.1) does **not** determine convergence
on its own — it inherits the convergence status from the last training
iteration.  When `max_iterations = 0` (e.g. `simulation_mode`), no
training iterations run, so `converged` is always `false`.

#### Primary criterion

```
LB = average of phase-0 objectives across scenes
UB = average of total forward-pass costs across scenes
gap = (UB - LB) / max(1, |UB|)
converged = (gap < convergence_tol) AND (iter >= min_iterations)
```

#### Secondary criterion — stationary gap

When `stationary_tol > 0` and at least `stationary_window` iterations
have completed, the solver checks whether the gap has stopped improving:

```
gap_change = |gap[iter] - gap[iter - window]| / max(1e-10, gap[iter - window])
if gap_change < stationary_tol AND iter >= min_iterations:
    converged = true
    stationary_converged = true
```

This criterion fires **only** when the primary criterion has not been met.
It handles SDDP problems where the gap converges to a non-zero stationary
value — a known theoretical limitation of Benders/SDDP on certain
stochastic programs.

#### Iteration result fields

| Field | Description |
|-------|-------------|
| `gap` | Relative optimality gap: $(UB - LB) / \max(1, \lvert UB \rvert)$ |
| `gap_change` | Relative change in gap over the look-back window (1.0 if not yet checked) |
| `converged` | `true` if either criterion was met |
| `stationary_converged` | `true` if convergence was declared by the stationary criterion |

### 4.5.1 Simulation Pass

After all training iterations complete (or when `max_iterations = 0`),
the solver runs a **simulation pass** — a forward-only policy
evaluation with no backward pass and no cut generation.  The simulation
pass solves all phases sequentially using the accumulated cuts from
training and records the resulting operating costs and state-variable
trajectories.

The simulation pass always runs, regardless of whether training
converged.  Its purpose is to evaluate the final policy under the full
set of Benders cuts without modifying the LP.

**Feasibility cuts during simulation**: if the simulation pass
encounters an infeasible trial point, the elastic filter is activated
and may produce a feasibility cut.  By default these cuts are
**discarded** (`save_simulation_cuts: false`) to ensure that hot-start
reproducibility is not affected by the evaluation pass.  Set
`save_simulation_cuts: true` to persist them.

**`simulation_mode`**: set `"simulation_mode": true` in `sddp_options`
to skip all training iterations (`max_iterations` is forced to 0) and
disable cut saving entirely.  Only the simulation pass runs, evaluating
the policy from previously loaded cuts (typically via `hot_start`).
This is useful for policy evaluation without modifying any cut state.

### 4.6 Cut Sharing (Optional)

After the backward pass, cuts from all scenes are optionally shared.  In
`expected` mode, an average cut is computed and added to all scenes.  In
`max` mode, every cut from every scene is added to all other scenes.

### 4.7 LP Coefficient Updates

The `update_lp` mechanism updates LP matrix coefficients, bounds, and RHS
values that depend on the current state of the system (e.g., reservoir
volumes).  It uses a **two-layer architecture**:

**Layer 1 — `SystemLP::update_lp()`**: Unconditional dispatch.  Iterates
over all (scenario, stage) pairs in this system's scene/phase and calls
`element.update_lp(sys, scenario, stage)` on every element that satisfies
the `HasUpdateLP` concept.  Returns the total number of LP modifications.

**Layer 2 — `SDDPMethod::dispatch_update_lp(scene, iteration)`**:
Conditional wrapper called at the start of each forward pass (per scene,
in the per-scene thread).  Applies three-way iteration logic before
delegating to `SystemLP::update_lp()` for each phase:

- `iteration.update_lp == false` → skip (explicitly disabled)
- `iteration.update_lp == true` → force (bypass skip count)
- not specified → apply global `update_lp_skip` count

**Call sites**:

- **After LP assembly** (`planning_lp.cpp`): `sys.update_lp()` is called
  unconditionally for every system, setting volume-dependent coefficients
  from reservoir `eini` values before any solver runs.
- **SDDP forward pass** (`sddp_forward_pass.cpp`):
  `dispatch_update_lp(scene, iteration)` runs before the phase solve loop.

**Currently implemented element updates**:

1. **Turbine efficiency** (`ReservoirProductionFactorLP`) — evaluates the
   piecewise-linear efficiency curve at the current reservoir volume and
   sets the turbine's conversion-rate LP coefficient via `set_coeff()`.
2. **Seepage** (`ReservoirSeepageLP`) — updates slope and RHS of seepage
   constraints based on volume-dependent piecewise-linear segments.
3. **Discharge limits** (`ReservoirDischargeLimitLP`) — updates slope and
   RHS of volume-dependent discharge limit constraints.

**Volume source by iteration**:

| Iteration | Phase 0 | Phase > 0 |
|-----------|---------|-----------|
| Initial (0) | `eini` from reservoir | `eini` from reservoir |
| First (1)   | `eini` from reservoir | `eini` from reservoir |
| Subsequent  | `eini` from reservoir | Previous phase efin (via state variable) |

**Skip count**: the `update_lp_skip` option in `sddp_options` controls
how often the update is applied.  A skip of $N$ means "update every
$N{+}1$ iterations".  Per-iteration overrides via the `iteration_array`
take precedence over the global skip count.

**Solver fallback**: if the LP solver does not support in-place matrix
coefficient modification (`supports_set_coeff()` returns `false`), the
update is skipped entirely and static coefficients are used unchanged.

### 4.8 Sentinel File Stop

The solver checks for a **sentinel file** (configurable via
`sentinel_file` in `SDDPOptions`) at the beginning of each iteration.  If
the file exists, the solver stops gracefully after saving all accumulated
cuts.  This is analogous to PLP's `userstop` mechanism.

### 4.9 Incremental Cut Saving

By default (`save_per_iteration: true`), cuts are saved to the output file
after **every iteration** (not just at the end).  This ensures that if the
solver is interrupted --- whether by the sentinel file, a time limit, or an
external signal --- the accumulated cuts are available for a subsequent
hot-start run.

Set `save_per_iteration: false` to defer all cut saving until the solve
completes or is stopped.  This reduces I/O overhead when saving is not
needed between iterations.

### 4.10 Cut Types

The SDDP solver generates two types of Benders cuts:

**Optimality cuts** (`CutType::Optimality`, CSV marker `o`) are the
standard Benders cuts that approximate the future cost function.  They
are generated during the backward pass from the reduced costs of the
solved phase LPs (see S2.3).

**Feasibility cuts** (`CutType::Feasibility`, CSV marker `f`) are
generated when the elastic filter is activated due to an infeasible
forward-pass trial point.  The dual information from the elastic-clone LP
produces a cut that tightens the previous phase's feasible region,
preventing the same infeasible trial point from reappearing.

Both cut types are now **stored and saved** to the cut CSV files.
Previously, feasibility cuts were transient (applied to the LP but not
persisted).  Saving them ensures that hot-start runs can benefit from
previously discovered feasibility information.

In the cut CSV files, the `type` column distinguishes the two kinds:

| Marker | CutType | Description |
|--------|---------|-------------|
| `o` | Optimality | Standard Benders optimality cut |
| `f` | Feasibility | Feasibility cut from elastic filter |

### 4.11 Solve Timeouts

Two timeout settings protect against runaway LP solves:

**`solve_timeout`** (default: 180 seconds / 3 minutes) applies to each
forward-pass LP solve.  When a forward-pass phase LP exceeds this time,
the solver writes the LP to a debug file in `log_directory`, logs a
CRITICAL message, and marks the scene as failed.  The remaining scenes
continue solving.

**`aperture_timeout`** (default: 15 seconds) applies to individual
aperture LP solves in the backward pass.  When an aperture LP exceeds
this time, it is treated as infeasible (skipped), a WARNING is logged,
and the solver continues with the remaining apertures for that phase.

Set either timeout to `0` to disable the corresponding time limit.

```json
{
  "options": {
    "sddp_options": {
      "solve_timeout": 300,
      "aperture_timeout": 30
    }
  }
}
```

### 4.12 Single-Phase Fallback

When `method: "sddp"` is requested but only **1 phase** exists in
the planning model, the solver factory automatically falls back to the
monolithic solver with an informational log message.  This prevents the
SDDP "requires at least 2 phases" error and allows the same JSON
configuration to work for both single-phase and multi-phase models.

### 4.13 Solver API for Monitoring and Control

The `SDDPMethod` exposes a thread-safe API designed for GUI integration,
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

### 4.14 Boundary Cuts (Future-Cost Approximation)

Boundary cuts approximate the expected future cost beyond the planning
horizon.  They are the SDDP analogue of PLP's "planos de embalse"
(reservoir future-cost function).  Each cut is of the form:

$$\alpha \;\ge\; \beta_0 \;+\; \sum_{i} \rho_i \cdot x_i$$

where $\alpha$ is the future-cost variable added to the last phase,
$x_i$ are the state variables (reservoir volumes, battery SoC), $\rho_i$
are gradient coefficients, and $\beta_0$ is the intercept (RHS).

**CSV format** (`boundary_cuts_file` in `simulation`, or
`sddp_options` for backward compatibility):

```
name,iteration,scene,rhs,Reservoir1,Reservoir2,...
bc_1_1,1,1,-5000.0,0.25,0.75,...
bc_1_2,1,2,-4800.0,0.30,0.60,...
bc_2_1,2,1,-5100.0,0.26,0.74,...
```

| Column | Description |
|--------|-------------|
| `name` | Cut identifier |
| `iteration` | SDDP iteration number (PLP: `IPDNumIte`) |
| `scene` | Scene UID (matches `uid` in `scene_array`; PLP: `ISimul`) |
| `rhs` | Intercept $\beta_0$ (PLP: $-$`LDPhiPrv`) |
| *State columns* | Gradient coefficients $\rho_i$ per state variable |

**Load modes** (`boundary_cuts_mode` in `sddp_options`; the file
itself is now specified via `boundary_cuts_file` in `simulation`):

| Mode | Description |
|------|-------------|
| `"noload"` | Skip loading even if a file is specified |
| `"separated"` (default) | Each cut is assigned to the scene matching its `scene` UID |
| `"combined"` | All cuts are broadcast to all scenes |

**Iteration filtering** (`boundary_max_iterations` in `sddp_options`):

When set to a positive integer $N$, only cuts from the last $N$ distinct
SDDP iterations (by the `iteration` column) are loaded.  This is useful
for PLP cases where many iterations of cuts are available but only the
most recent ones are relevant.  Set to `0` to load all cuts (default).

**Example** --- load the last 3 iterations of cuts, per-scene:

```json
{
  "options": {
    "sddp_options": {
      "boundary_cuts_mode": "separated",
      "boundary_max_iterations": 3
    }
  },
  "simulation": {
    "boundary_cuts_file": "boundary_cuts.csv",
    "boundary_cuts_valuation": "end_of_horizon"
  }
}
```

### 4.15 Named Cuts (Multi-Phase Hot-Start)

Named cuts extend boundary cuts to **all phases** (not just the last).
Each row includes a `phase` column indicating which phase the cut belongs
to.  The solver resolves named state-variable headers (reservoir, battery,
junction) to LP column indices in the specified phase, then adds each cut
as a lower-bound constraint on the corresponding future-cost variable:

$$\alpha_{\text{phase}} \;\ge\; \text{rhs} \;+\; \sum_{i} \rho_i \cdot x_i[\text{phase}]$$

**CSV format** (`named_cuts_file` in `sddp_options`):

```
name,iteration,scene,phase,rhs,Reservoir1,Reservoir2,...
hs_1_1_3,1,1,3,-5000.0,0.25,0.75,...
hs_2_1_4,2,1,4,-4800.0,0.30,0.60,...
```

| Column | Description |
|--------|-------------|
| `name` | Cut identifier |
| `iteration` | SDDP iteration number |
| `scene` | Scene UID |
| `phase` | Phase UID (determines which phase LP receives the cut) |
| `rhs` | Intercept $\beta_0$ |
| *State columns* | Gradient coefficients per state variable (by name) |

**Example**:

```json
{
  "options": {
    "sddp_options": {
      "named_cuts_file": "planos_cuts.csv"
    }
  }
}
```

---

## 5. Configuration

### 5.1 SDDPOptions

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `max_iterations` | int | 100 | Maximum SDDP iterations |
| `min_iterations` | int | 2 | Minimum iterations before declaring convergence |
| `convergence_tol` | double | 1e-4 | Relative gap tolerance |
| `elastic_penalty` | double | 1e6 | Penalty cost for elastic slack variables |
| `elastic_filter_mode` | ElasticFilterMode | single_cut | Elastic filter strategy (see S5.4) |
| `multi_cut_threshold` | int | 10 | Infeasibility count before auto-switching to multi_cut (0 = always multi_cut, <0 = disabled) |
| `alpha_min` | double | 0.0 | Lower bound for α variables |
| `alpha_max` | double | 1e12 | Upper bound for α variables |
| `cut_sharing_mode` | CutSharingMode | none | Cut sharing strategy between scenes |
| `save_per_iteration` | bool | true | Save cuts to CSV after each iteration |
| `hot_start` | bool | false | Load previously saved cuts on startup |
| `solve_timeout` | double | 180.0 | Forward-pass LP solve timeout in seconds (0 = no timeout) |
| `aperture_timeout` | double | 15.0 | Aperture LP solve timeout in seconds (0 = no timeout) |
| `sentinel_file` | string | "" | Path to sentinel file for graceful stop |
| `cuts_output_file` | string | "" | Path for saving cuts (CSV format) |
| `cuts_input_file` | string | "" | Path for loading cuts (hot-start) |
| `log_directory` | string | "logs" | Directory for log and error LP files |
| `lp_debug` | bool | false | Save debug LP file for every (iter, scene, phase) |
| `lp_only` | bool | false | Build LP matrices and exit without solving (CLI: `--lp-only` / `-c`) |
| `lp_debug_compression` | string | "" | Compression for LP debug files (`"gzip"` / `""`) |
| `enable_api` | bool | true | Enable monitoring API (JSON status file) |
| `api_status_file` | string | "" | Path for the JSON status file |
| `api_stop_request_file` | string | "" | Path for monitoring API stop-request file |
| `api_update_interval` | ms | 500 | Interval between monitoring samples |
| `num_apertures` | int | 0 | Apertures per backward-pass phase (0 = disabled, -1 = all scenarios) |
| `boundary_cuts_mode` | string | "separated" | Load mode: `"noload"`, `"separated"`, `"combined"` |
| `boundary_max_iterations` | int | 0 | Max iterations to load (0 = all) |
| `named_cuts_file` | string | "" | CSV file with named multi-phase cuts (S4.15) |
| `max_cuts_per_phase` | int | 0 | Max retained cuts per (scene, phase) LP (0 = unlimited) |
| `cut_prune_interval` | int | 10 | Iterations between cut pruning passes |
| `prune_dual_threshold` | double | 1e-8 | Dual threshold for inactive cut detection |
| `single_cut_storage` | bool | false | Store cuts per-scene only (halves memory) |
| `max_stored_cuts` | int | 0 | Max stored cuts per scene (0 = unlimited) |
| `simulation_mode` | bool | false | Skip training (force `max_iterations=0`), run simulation pass only, disable cut saving |
| `stationary_tol` | double | 0.0 | Secondary convergence: relative gap-change tolerance (0 = disabled; e.g. 0.01 = 1%) |
| `stationary_window` | int | 10 | Look-back window for stationary gap check (only used when `stationary_tol > 0`) |
| `save_simulation_cuts` | bool | false | Persist feasibility cuts from the simulation pass (see S4.5.1) |

### 5.2 Options (JSON)

The SDDP solver and all key tuning parameters are fully configurable from
the JSON planning file.

```json
{
  "options": {
    "method": "sddp",
    "log_directory": "logs",
    "sddp_options": {
      "cut_sharing_mode": "expected",
      "cut_directory": "cuts",
      "max_iterations": 200,
      "convergence_tol": 1e-5,
      "elastic_penalty": 1e7,
      "elastic_mode": "backpropagate",
      "hot_start": true,
      "solve_timeout": 300,
      "aperture_timeout": 30,
      "stationary_tol": 0.01,
      "stationary_window": 10
    }
  }
}
```

The top-level `method` field selects the planning solver.  SDDP-specific
options live in the nested `sddp_options` sub-object (without the `sddp_`
prefix, since the section name already provides the namespace).

**Top-level `options` fields:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `method` | string | `"monolithic"` | Solver: `"monolithic"` or `"sddp"` (recommended shorthand) |
| `log_directory` | string | `"<output_directory>/logs"` | Directory for log and trace files |

**`sddp_options` sub-object fields:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `cut_sharing_mode` | string | `"none"` | Cut sharing: `"none"`, `"expected"`, `"accumulate"`, or `"max"` |
| `cut_directory` | string | `"cuts"` | Directory for Benders cut files |
| `max_iterations` | int | 100 | Maximum SDDP iterations |
| `min_iterations` | int | 2 | Minimum iterations before declaring convergence |
| `convergence_tol` | double | 1e-4 | Relative gap convergence tolerance |
| `elastic_penalty` | double | 1e6 | Penalty for elastic slack variables |
| `elastic_mode` | string | `"single_cut"` | Elastic filter mode: `"single_cut"` (alias `"cut"`), `"multi_cut"`, or `"backpropagate"` |
| `multi_cut_threshold` | int | 10 | Auto-switch to multi_cut after N consecutive infeasibilities |
| `alpha_min` | double | 0.0 | Lower bound for future cost variable α |
| `alpha_max` | double | 1e12 | Upper bound for future cost variable α |
| `hot_start` | bool | false | Load previously saved cuts on startup (from `cut_directory`) |
| `save_per_iteration` | bool | true | Save cuts to CSV after each iteration |
| `solve_timeout` | double | 180.0 | Forward-pass LP solve timeout in seconds (0 = no timeout) |
| `aperture_timeout` | double | 15.0 | Aperture LP solve timeout in seconds (0 = no timeout) |
| `num_apertures` | int | 0 | Apertures per backward-pass phase (0 = disabled, -1 = all) |
| `aperture_directory` | string | `""` | Directory for aperture-specific scenario data |
| `api_enabled` | bool | true | Enable SDDP monitoring API (JSON status file) |
| `efficiency_update_skip` | int | 0 | Iterations to skip between efficiency coefficient updates |
| `boundary_cuts_mode` | string | `"separated"` | Load mode: `"noload"`, `"separated"`, `"combined"` |
| `boundary_max_iterations` | int | 0 | Max SDDP iterations to load from boundary cuts (0 = all) |
| `named_cuts_file` | string | `""` | CSV file with named multi-phase cuts (S4.15) |
| `simulation_mode` | bool | `false` | Skip training, run simulation pass only (S4.5.1) |
| `save_simulation_cuts` | bool | `false` | Save feasibility cuts from simulation pass |
| `stationary_tol` | double | 0.0 | Secondary convergence: relative gap-change tolerance (0 = disabled) |
| `stationary_window` | int | 10 | Look-back window for stationary gap detection |

> **Note**: `boundary_cuts_file` has moved to the `simulation`
> section.  For backward compatibility, it is still accepted in
> `sddp_options`.  See also `boundary_cuts_valuation` in
> `simulation` (`"end_of_horizon"` or `"present_value"`).

### 5.3 CLI

```bash
# Run with SDDP solver, custom cut directory, and tight convergence
gtopt my_case.json \
  --set sddp_options.cut_directory=cuts \
  --set log_directory=logs \
  --set sddp_options.max_iterations=300 \
  --set sddp_options.convergence_tol=1e-5 \
  --set sddp_options.elastic_mode=backpropagate \
  --trace-log sddp_trace.log
```

| Flag | Description |
|------|-------------|
| `--trace-log <file>` | Capture all `SPDLOG_TRACE` messages to a file |
| `--set sddp_options.cut_directory=<dir>` | Directory for Benders cut files (default: `cuts`) |
| `--set log_directory=<dir>` | Directory for log and trace files (default: `logs`) |
| `--set sddp_options.max_iterations=<n>` | Maximum SDDP iterations (default: 100) |
| `--set sddp_options.convergence_tol=<tol>` | Relative gap convergence tolerance (default: 1e-4) |
| `--set sddp_options.elastic_penalty=<p>` | Elastic slack penalty coefficient (default: 1e6) |
| `--set sddp_options.elastic_mode=<mode>` | Elastic filter mode: `cut` or `backpropagate` (default: `cut`) |

The `--trace-log` option captures all `SPDLOG_TRACE` messages to a file,
providing detailed iteration-by-iteration data including:
- Forward pass phase objectives and α values
- Backward pass cut coefficients and RHS values
- Elastic filter activations
- Convergence metrics

### 5.4 Elastic Filter Modes

When a backward-pass phase is infeasible due to the fixed trial values of
state variables from the forward pass, the elastic filter handles the
infeasibility by temporarily relaxing the state-variable bounds using
penalised slack variables.  Three modes are available:

#### Mode 1: `"single_cut"` (default, alias `"cut"`)

This is the standard Nested Benders Decomposition approach.  When the
elastic clone is solved, its dual information is used to build a single
**feasibility cut** for the previous phase.  The feasibility cut is added
to the previous phase's LP and the phase is re-solved.  This tightens the
previous phase's feasible region and prevents the same infeasible trial
point from reappearing.

**When to use:** when you want the classical NBD guarantee that all phases
remain strictly feasible after each iteration and cuts accurately capture
the future cost.

#### Mode 2: `"multi_cut"`

Like `single_cut`, this mode adds a feasibility cut, but also adds one
additional bound-constraint cut per activated slack variable.  This
provides stronger information to the previous phase's LP, potentially
reducing the number of iterations needed to achieve feasibility.

The solver can **auto-switch** from `single_cut` to `multi_cut` when a
specific (scene, phase) pair has been infeasible for more than
`multi_cut_threshold` consecutive forward passes (default: 10).  Set the
threshold to `0` to always use `multi_cut` for any infeasibility, or to
a negative value to disable auto-switching entirely.

#### Mode 3: `"backpropagate"` (BackpropagateBounds --- PLP mechanism)

This is based on the PLP hydrothermal scheduler mechanism in
`osicallsc.cpp`.  Instead of adding a cut, the solver propagates the
**elastic-clone solution values** back as tightened bounds on the source
state columns in the previous phase.  Specifically, for each state variable
link, the source column's upper and lower bounds in the previous phase are
set to the value found by the elastic clone.  This forces the previous
phase to produce a trial point that is known to be feasible for the current
phase.

**When to use:** when infeasibility is caused by physically unreachable
trial points (e.g., reservoir levels outside capacity bounds) and you want
to quickly correct the trial trajectory without adding cut rows.  This can
converge faster in practice for hydrothermal problems with tight physical
bounds, but may produce a different (non-cut-based) convergence path.

**Comparison:**

| Aspect | `single_cut` | `multi_cut` | `backpropagate` |
|--------|-------------|-------------|-----------------|
| Adds cut rows | 1 | 1 + per-slack | No |
| Modifies previous phase bounds | No | No | Yes |
| Convergence guarantee | Standard NBD | Standard NBD | Heuristic |
| Best for | General SDDP | Persistent infeasibility | Hydrothermal with tight bounds |
| PLP origin | No | No | Yes (`osicallsc.cpp`) |

### 5.5 Cut CSV Format

Cut files use a CSV format with a header line and one row per cut.  The
format includes a `type` column and uses **name-based coefficients** for
portability across LP structure changes:

```
# scale_objective=1000
type,phase,scene,name,rhs,coefficients
o,1,1,sddp_scut_1_1_3_0,-5000.0,Res1_efin=0.25,Res2_efin=0.75
f,1,1,sddp_fcut_1_1_3_1,-4800.0,Res1_efin=0.30
o,2,1,sddp_scut_1_2_4_0,-5100.0,Res1_efin=0.26
```

| Column | Description |
|--------|-------------|
| `type` | Cut type: `o` = optimality, `f` = feasibility |
| `phase` | Phase UID this cut was added to |
| `scene` | Scene UID that generated this cut |
| `name` | Cut identifier (encodes scene, phase, iteration, offset) |
| `rhs` | Right-hand side in physical objective units |
| `coefficients` | Variable-coefficient pairs (see below) |

**Coefficient format**: coefficients are written as comma-separated
`col_name=coeff` pairs using LP column names (e.g.,
`Reservoir1_efin=0.25`).  This name-based format is portable across runs
where the LP column order may change.  Values are stored in fully physical
space (scaled by `scale_objective` and inverse column scales).

**Backward-compatible loading**: the loader also accepts the legacy
`col_index:coeff` format (e.g., `42:0.25`), where `col_index` is a
0-based LP column index.  If a named column is not found in the current
LP (due to structural changes), the coefficient is skipped with a warning.

### 5.6 Per-Scene Cut Files

When `cuts_output_file` is set, the solver saves both a combined cut file
and per-scene files to avoid write contention during parallel backward
passes.  File names use **scene UIDs** (not 0-based indices):

```
cuts/
├── sddp_cuts.csv      # Combined cuts from all scenes
├── scene_1.csv         # Cuts generated by scene with UID 1
├── scene_2.csv         # Cuts generated by scene with UID 2
└── ...
```

Each per-scene file has the same CSV format as the combined file.  This
allows concurrent scene processing to write independently without
locking.  Cut files for infeasible scenes are automatically renamed
with an `error_` prefix (e.g. `error_scene_2.csv`).

The `phase` and `scene` columns in cut CSV files contain **UIDs**
(matching the `uid` field in `phase_array` and `scene_array`), not
0-based C++ indices.

### 5.7 Infeasible Scene Handling

When one or more scenes are infeasible during the forward pass:

1. **Skip**: the infeasible scene is excluded from lower/upper bound
   computation and from the backward pass.
2. **Save error LP**: the infeasible LP is written to `log_directory` as
   `error_scene_<UID>_phase_<UID>.lp` for debugging (using scene and
   phase UIDs).
3. **Rename cut files**: per-scene cut files for infeasible scenes are
   renamed with an `error_` prefix (e.g. `error_scene_0.csv`) instead of
   being removed, preserving diagnostic data.
4. **Continue**: the solver continues with remaining feasible scenes.
5. **Error**: if **all** scenes are infeasible, the solver returns an error
   and `gtopt_main` exits with a non-zero exit code.

### 5.8 Hot-Start and Error File Filtering

Hot-start requires setting `hot_start: true` explicitly in `sddp_options`.
When enabled, the solver loads cuts from the cut directory to warm-start
the Benders approximation.  If `cuts_input_file` is also set, it takes
precedence as the source of hot-start cuts.

Files with the `error_` prefix (from infeasible scenes in previous runs)
are automatically **skipped** to prevent loading invalid cuts:

```
cuts/
├── sddp_cuts.csv        ← loaded (combined cuts)
├── scene_0.csv           ← loaded (valid scene)
├── scene_1.csv           ← loaded (valid scene)
├── error_scene_2.csv     ← SKIPPED (infeasible in previous run)
└── error_scene_3.csv     ← SKIPPED (infeasible in previous run)
```

The `load_scene_cuts_from_directory()` method handles this filtering.
Only files matching `scene_<N>.csv` or `sddp_cuts.csv` are loaded.

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
| `SDDPMethod` | `sddp_method.hpp/cpp` | Core SDDP algorithm |
| `SDDPPlanningMethod` | `sddp_method.hpp/cpp` | `PlanningMethod` interface adapter |
| `MonolithicMethod` | `planning_method.hpp/cpp` | Default full-LP solver |
| `SolverMonitor` | `solver_monitor.hpp` | Background CPU/worker monitoring (SDDP + Monolithic) |
| `PlanningLP` | `planning_lp.hpp/cpp` | LP assembly and phase management |
| `LinearInterface` | `linear_interface.hpp/cpp` | LP solver abstraction (COIN-OR) |
| `AdaptiveWorkPool` | `work_pool.hpp` | Parallel scene processing |
| `ApertureDataCache` | `aperture_data_cache.hpp/cpp` | Pre-loaded aperture parquet data |
| `build_effective_apertures()` | `sddp_aperture.hpp/cpp` | Deduplicate aperture UIDs |
| `build_synthetic_apertures()` | `sddp_aperture.hpp/cpp` | Generate apertures from scenarios |
| `solve_apertures_for_phase()` | `sddp_aperture.hpp/cpp` | Clone + update + solve per aperture |

### 7.2 SolverMonitor

`SolverMonitor` is a reusable class that samples `AdaptiveWorkPool`
statistics (CPU load, active worker count) in a background `std::jthread`
and writes them to a JSON status file for external monitoring tools such as
`scripts/gtopt_monitor/gtopt_monitor.py`.

Both `SDDPMethod` and `MonolithicMethod` create a local `SolverMonitor`
during their `solve()` call.

**Solver status file** (`solver_status.json`) contains:
- `"version"`, `"timestamp"`, `"elapsed_s"`, `"status"`, `"iteration"`
- `"lower_bound"`, `"upper_bound"`, `"gap"`, `"converged"`, `"max_iterations"`
- `"solver"`, `"method"`: runtime solver identity and planning method
- `"history"`: per-iteration array with `lower_bound`, `upper_bound`, `gap`,
  `converged`, `cuts_added`, `scene_upper_bounds`, `scene_lower_bounds`
- `"phase_grid"`: per-(iteration, scene, phase) activity grid for TUI display
- `"async"`: async scene execution state (when `max_async_spread > 0`)
- `"realtime"`: rolling arrays of `timestamps`, `cpu_loads`, `active_workers`

Both SDDP and monolithic methods write to the same `solver_status.json` file.

The status file is written **atomically** (write to `.tmp`, then rename) to
allow external tools to read it without seeing a partial write.

### 7.3 Free Functions

| Function | Purpose |
|----------|---------|
| `propagate_trial_values()` | Fix dependent columns to source values |
| `build_benders_cut()` | Construct optimality cut from reduced costs |
| `relax_fixed_state_variable()` | Apply elastic relaxation to one column |
| `average_benders_cut()` | Average multiple cuts (for `expected` sharing) |
| `accumulate_benders_cuts()` | Sum multiple cuts (for `accumulate` sharing) |
| `share_cuts_for_phase()` | Share cuts across scenes for a phase (`sddp_cut_sharing.hpp`) |
| `cut_sharing_mode_from_name()` | Parse string to `CutSharingMode` enum |
| `parse_elastic_filter_mode()` | Parse `"cut"` / `"backpropagate"` to `ElasticFilterMode` |
| `weighted_average_benders_cut()` | Probability-weighted average of aperture cuts |

### 7.4 LP Clone Pattern

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

For aperture backward-pass solves, each aperture task creates its own
clone of the source LP inline via `LinearInterface::clone()` (see
`source/sddp_aperture.cpp`).  Clones live only inside the parallel
worker task that consumes them and are destroyed when the task scope
exits, so concurrent aperture solves never share LP state and there is
no need for a cross-task clone cache.

### 7.5 Thread Safety

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

When `single_cut_storage` is enabled, cuts are stored only in the
per-scene vectors.  The combined cut list for persistence is built on
demand from the per-scene vectors, halving the memory cost of cut
metadata.

### 7.6 Memory Management

Long SDDP runs accumulate Benders cuts as LP constraint rows that are
never automatically deleted.  The CLP solver's internal matrix grows
monotonically, which can cause memory exhaustion.  Several mechanisms
mitigate this:

1. **Cut pruning** (`max_cuts_per_phase`, `cut_prune_interval`,
   `prune_dual_threshold`): periodically removes inactive cuts (those
   with |dual| below the threshold) from each (scene, phase) LP via
   `OsiSolverInterface::deleteRows()`.  A "base row count" is saved
   during initialization so structural constraints are never pruned.

2. **Stored cut capping** (`max_stored_cuts`): limits the in-memory cut
   metadata per scene, dropping the oldest cuts when the cap is exceeded.

3. **Single cut storage** (`single_cut_storage`): avoids duplicating cut
   metadata across per-scene and shared storage.

4. **Low-memory mode** (`low_memory_mode`): controls how aggressively the
   per-(scene, phase) LP backend is released between solves.

   - `off` (default): backend stays loaded; no FlatLinearProblem held.
     Maximum throughput, maximum memory.
   - `snapshot`: backend released after every solve; the
     FlatLinearProblem is kept in memory and replayed on the next
     access via `LinearInterface::reconstruct_backend()`.  Cuts the
     solver-resident memory but still pays one snapshot per cell.
   - `compress`: same as `snapshot`, but the snapshot is compressed
     (lz4 / zstd, ~2–4× ratio) when the backend is released and
     decompressed on demand.
   - `rebuild`: no FlatLinearProblem snapshot is kept.  The initial
     up-front "Build LP" loop is **skipped entirely** — each
     `(scene, phase)` LP is built lazily inside the same task that
     solves or clones it (forward pass, backward source LP for
     apertures, elastic-filter base LP, `--lp-only` validation).
     Aperture clones still come from a single rebuild per phase per
     iteration: the source LP is rebuilt once, every aperture task
     clones it inline, and the source is discarded once the apertures
     finish.  Persistent SDDP state — the alpha column added by the
     solver and the accumulated Benders cuts — is preserved across
     rebuilds via `m_dynamic_cols_` / `m_active_cuts_` on
     `LinearInterface` and replayed on every rebuild, so SDDP
     convergence is unchanged.  Lowest steady-state memory; highest
     CPU cost — pick `rebuild` only when memory is the binding
     constraint.

5. **Warm-start pre-padding**: forward-pass solution vectors are padded
   with zeros at save time so that `set_warm_start_solution()` can use a
   subspan instead of allocating a temporary vector every call.

6. **Monitor history cap**: the `SolverMonitor` background sampling
   thread caps its history at 7200 entries (~1 hour at 500ms), dropping
   the oldest 25% when full.

---

## 8. Relationship to PLP

The gtopt SDDP solver draws on concepts from the PLP (Programación Lineal
de la Planificación) hydrothermal scheduler [[5]](#ref5)[[6]](#ref6)
maintained in the `marcelomatus/plp_storage` repository:

| PLP concept | gtopt equivalent |
|-------------|-----------------|
| SDDP forward/backward iteration | `SDDPMethod::forward_pass()` / `backward_pass()` |
| Elastic filter (`osi_lp_get_feasible_cut`) | `SDDPMethod::elastic_solve()` via `clone()` |
| LP cloning (`LPCont::get_lpi()`) | `LinearInterface::clone()` |
| `userstop` file | `sentinel_file` option in `SDDPOptions` |
| Cut file persistence | `save_cuts()` / `load_cuts()` in CSV format |
| `scloning` mode | Always-clone approach for elastic filter |
| Bound backpropagation from elastic filter | `ElasticFilterMode::BackpropagateBounds` |

The PLP code (`CEN65/src/osicallsc.cpp`) uses `OsiSolverInterface::clone()`
in `osi_lp_get_feasible_cut` to create a temporary LP copy, zero the
original objective, add elastic slack variables, solve for feasibility,
extract the dual ray, and discard the clone.  gtopt's `elastic_solve()`
follows the same pattern.

The `BackpropagateBounds` elastic filter mode
(`--set sddp_options.elastic_mode=backpropagate`) is a direct translation of the PLP bound-update mechanism:
instead of building a feasibility cut, the elastic-clone solution values are
propagated back as tightened bounds on the source columns in the previous
phase, forcing the trial trajectory to remain within the feasible region.

### 8.1 Cut-coefficient extraction: why we removed the `row_dual` mode

**Context.** Until 2026-04, `SDDPMethod` supported two ways of extracting
Benders-cut coefficients at the end of a backward solve:

1. **`reduced_cost` (kept, now the only mode)** — build the cut directly from
   the reduced costs of the *dependent columns* that implement the inter-phase
   state transition.  By strong duality, when a dependent column is fixed at
   the trial value `v̂ᵢ` of its source variable, the reduced cost at optimum
   equals the dual multiplier of that fixing constraint.  The cut is
   `α ≥ z* + Σ rcᵢ·(xᵢ − v̂ᵢ)`.

2. **`row_dual` (removed)** — explicitly add one coupling row per state
   variable to the phase LP (`xᵢ − dependentᵢ = 0`), solve, and read the
   row duals `πᵢ` to build `α ≥ z* + Σ πᵢ·(xᵢ − v̂ᵢ)`.  This mirrors the
   PLP Fortran implementation (`CEN65/src/plpsddp.f`) and the broader
   CEPEL family (PSR SDDP, NEWAVE, DECOMP, SUISHI).

**Why `row_dual` was removed.**

- **Mathematical redundancy.**  For an LP at optimum, the reduced cost of a
  column fixed to `v̂ᵢ` and the row dual of an equivalent fixing row are
  identical (both equal the Lagrange multiplier of the fixing constraint).
  The two modes produced cuts that differed only by floating-point noise.
  The equivalence is well-known — see Birge & Louveaux §5.1
  [[16]](#ref16), Shapiro §3 [[4]](#ref4), and Dowson §3.2 [[15]](#ref15).

- **Cost asymmetry.**  `row_dual` requires mutating the LP (adding one row
  per state variable before each solve and removing it after), which
  invalidates the solver's basis and warm-start caches and inflates the
  constraint matrix.  `reduced_cost` reads a value the solver already
  computes — it is strictly free after `resolve()`.

- **Literature survey.**  Modern SDDP implementations use the reduced-cost
  formulation exclusively:

  | Implementation | Cut coefficient source | Reference |
  |----------------|------------------------|-----------|
  | SDDP.jl (Dowson) | reduced cost of state-in columns | [[15]](#ref15) |
  | FAST / MSPPy | reduced cost | [[17]](#ref17) |
  | SDDiP (Zou, Ahmed, Sun) | reduced cost / Lagrangian | 2019 |
  | SINTEF EMPS | reduced cost | Wolfgang et al. 2009 |
  | Philpott & Guan analysis | reduced cost | [[7]](#ref7) |
  | Shapiro textbook derivation | reduced cost | [[4]](#ref4)[[16]](#ref16) |

  The `row_dual` formulation is a PLP/CEPEL dialect — it appears in the
  Brazilian operational-planning tool chain (PSR SDDP, NEWAVE, DECOMP,
  SUISHI, and the Chilean PLP derivative) because those codes predate
  modern LP solver APIs that expose reduced costs reliably on fixed
  columns.  It is not referenced in the peer-reviewed SDDP algorithmics
  literature outside that lineage.

- **Numerical behaviour.**  Both modes converge to the same lower bound on
  well-posed problems.  The former `SDDPMethod — reduced_cost and row_dual
  produce same objective` test (removed with this change) verified this
  empirically on a 2-phase linear planning problem.

- **PLP cut-file interoperability is preserved.**  The
  `boundary_cuts.csv` format generated by PLP stores only the final
  numeric coefficients `(πᵢ, α₀)` — it has no field identifying how they
  were computed.  By strong duality, PLP's `πᵢ` and gtopt's `rcᵢ` are
  the same number at optimum, so the existing name-based loader in
  `source/sddp_cut_io.cpp` handles PLP-generated cut files unchanged.

**Consequences of the removal.**

- `SddpOptions::cut_coeff_mode`, the `CutCoeffMode` enum, the
  `build_benders_cut_from_row_duals()` / `propagate_trial_values_row_dual()`
  free functions, the `StateVarLink::coupling_row` field, and the
  `--sddp-cut-coeff-mode` CLI flag (plus JSON key and bash completion) are
  all gone.
- `PhaseStateInfo::forward_row_dual` is kept solely as a warm-start hint
  for the next backward solve; it is no longer consumed by cut
  construction.
- The `StateVariable` runtime slot holds `col_sol()` (trial value) and
  `reduced_cost()` (cut coefficient).  There is no `row_dual()` slot —
  the former `std::optional<double>` placeholder was dropped before the
  final removal because every downstream consumer would have needed a
  sentinel branch.

If a future requirement demands a PLP-verbatim dual path (e.g. for
bit-exact comparison with a running PLP instance during migration), the
cleanest reintroduction would be a standalone `plp_compat` reader in
`scripts/plp2gtopt` that converts PLP's solver logs to reduced-cost form
once, not a dual extraction mode inside the solver.

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

<a id="ref15"></a>
**[15]** O. Dowson and L. Kapelevich, "SDDP.jl: a Julia package for
stochastic dual dynamic programming," *INFORMS Journal on Computing*,
vol. 33, no. 1, pp. 27–33, 2021.
DOI: [10.1287/ijoc.2020.0987](https://doi.org/10.1287/ijoc.2020.0987)

<a id="ref16"></a>
**[16]** J. R. Birge and F. Louveaux, *Introduction to Stochastic
Programming*, 2nd ed. New York: Springer, 2011.
DOI: [10.1007/978-1-4614-0237-4](https://doi.org/10.1007/978-1-4614-0237-4)

<a id="ref17"></a>
**[17]** S. Rebennack, "Combining sampling-based and scenario-based
nested Benders decomposition methods: application to stochastic dual
dynamic programming," *Mathematical Programming*, vol. 156,
pp. 343–389, 2016.
DOI: [10.1007/s10107-015-0884-3](https://doi.org/10.1007/s10107-015-0884-3)

<a id="ref18"></a>
**[18]** V. Zverovich, C. I. Fabian, E. F. D. Ellison, and G. Mitra,
"A computational study of a solver system for processing two-stage
stochastic LPs with enhanced Benders decomposition," *Mathematical
Programming Computation*, vol. 4, pp. 211–238, 2012.
DOI: [10.1007/s12532-012-0038-z](https://doi.org/10.1007/s12532-012-0038-z)

---

## 10. Cascade Solver — Multi-Level Hybrid Solver

> For full cascade solver documentation, see
> [Cascade Method](cascade.md).

The **Cascade solver** (`method = "cascade"`) is a multi-level hybrid
algorithm that progressively refines the LP formulation and solver strategy
across a variable number of levels.  It accelerates convergence by starting
with a simplified network model (e.g. single bus, no Kirchhoff) and
gradually adding complexity, transferring solution information between
levels via cuts and target constraints.

### 10.1 Multi-Level Architecture

The cascade solver executes a sequence of **levels** defined in the
`cascade_options.level_array` array.  Each level has:

- **`name`** — human-readable identifier (for logging).
- **`model_options`** — LP construction overrides (`use_single_bus`,
  `use_kirchhoff`, `use_line_losses`, `kirchhoff_threshold`,
  `loss_segments`, `scale_objective`, `scale_theta`,
  `demand_fail_cost`, `reserve_fail_cost`).
  When present, a fresh `PlanningLP` is built.  When absent, the
  previous level's LP is reused.
- **`sddp_options`** — per-level solver settings (`max_iterations`,
  `min_iterations`, `apertures`, `convergence_tol`).  Absent fields
  inherit from the global `sddp_options`.
- **`transition`** — how to receive information from the previous level
  (cut inheritance, target constraints).

**LP rebuild rule**: when `model_options` is present (even if all its
fields match the previous level), the LP is rebuilt from scratch.  When
absent, the previous level's LP and solver state are reused, allowing a
level to change only solver parameters (e.g. enable apertures) without
the cost of LP reconstruction.

**Named transfer**: cuts and target constraints use LP **column names**
(e.g. `Reservoir1_efin`) for cross-LP resolution.  This allows cuts
generated with one LP structure (e.g. single-bus) to be applied to a
different LP structure (e.g. multi-bus with Kirchhoff), as long as the
state-variable columns share the same names.

If the solver converges at any level, it returns immediately without
proceeding to subsequent levels.

### 10.2 Built-in Default Levels

When `cascade_options.level_array` is empty (or omitted), a built-in
4-level default is used:

| Level | Name | LP | Solver | Transition |
|-------|------|----|--------|------------|
| 0 | `uninodal_benders` | Single bus, no Kirchhoff, no losses | Benders only (no apertures) | -- |
| 1 | `transport_benders` | Multi-bus, no Kirchhoff, no losses | Benders only | Target constraints from level 0 |
| 2 | `transport_sddp` | Reuses level 1 LP | SDDP with apertures | Target constraints from level 1 |
| 3 | `full_sddp` | Kirchhoff + line losses | SDDP with apertures | Inherits optimality + feasibility cuts |

This default progression moves from a fast coarse approximation to the
full network model, transferring solution information at each step.

### 10.3 Level Configuration

Each level in `level_array` can contain three optional sub-objects.

#### `model_options` — LP Formulation

Controls how the LP is constructed for this level.  When present, a
fresh `PlanningLP` is built.  When absent, the previous level's LP is
reused.  Only set fields override the global `model_options`; absent
fields inherit from the global configuration.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `use_single_bus` | bool | `false` | Aggregate all buses into one |
| `use_kirchhoff` | bool | `true` | Enable DC power flow (voltage angles) |
| `use_line_losses` | bool | `false` | Model resistive line losses |
| `kirchhoff_threshold` | real | 0.0 | Min bus voltage [kV] for Kirchhoff |
| `loss_segments` | int | 1 | Piecewise-linear loss segments |
| `scale_objective` | real | 1000 | Divisor for objective coefficients |
| `scale_theta` | real | 1.0 | Scaling for voltage-angle variables |
| `demand_fail_cost` | real | 1000 | Penalty for unserved demand [$/MWh] |
| `reserve_fail_cost` | real | 1000 | Penalty for unserved reserve [$/MWh] |

> **Note**: `annual_discount_rate` has moved to the `simulation`
> section.  For backward compatibility, it is still accepted in
> `model_options`.

#### `sddp_options` — Per-Level Solver Parameters

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `max_iterations` | int | from global `sddp_options` | Max training iterations for this level |
| `min_iterations` | int | from global `sddp_options` | Min iterations before convergence |
| `apertures` | array of UIDs | from global `sddp_options` | Aperture UIDs (empty = Benders only) |
| `convergence_tol` | real | from global `sddp_options` | Convergence tolerance |

#### `transition` — Transfer from Previous Level

Controls how information from the previous level is carried forward.
Level 0 has no transition (it is the starting point).

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `inherit_optimality_cuts` | bool | `false` | Carry forward Benders optimality cuts |
| `inherit_targets` | bool | `false` | Add elastic target constraints from previous solution |
| `target_rtol` | real | 0.05 | Relative tolerance for target band (5% of \|v\|) |
| `target_min_atol` | real | 1.0 | Minimum absolute tolerance for target band |
| `target_penalty` | real | 500 | Elastic penalty per unit target violation |
| `optimality_dual_threshold` | real | 0.0 | Min \|dual\| for cut transfer (0 = all) |

### 10.4 Target Constraints

Target constraints are LP rows (not column bound changes) that constrain
state variables near the previous level's solution trajectory.  They use
an elastic formulation with a penalty cost, avoiding hard infeasibility
while guiding the optimizer:

$$v_{\text{prev}} - \text{atol} \le v \le v_{\text{prev}} + \text{atol}$$

where $\text{atol} = \max(\text{rtol} \cdot |v_{\text{prev}}|, \text{min\_atol})$.  Violations beyond the tolerance band incur a cost
of `target_penalty` per unit.

Target rows are named `cascade_target_s{scene}_p{phase}_c{col}` for
easy identification in LP debug output.

### 10.5 Configuration Example

Cascade options are set in their own `cascade_options` sub-object in the
JSON configuration (not inside `sddp_options`).  The global
`sddp_options` provide defaults that individual levels can override.

**2-level cascade** — uninodal warm-start followed by full network:

```json
{
  "options": {
    "method": "cascade",
    "model_options": {
      "use_kirchhoff": true,
      "demand_fail_cost": 5000
    },
    "sddp_options": {
      "max_iterations": 50,
      "convergence_tol": 0.001
    },
    "cascade_options": {
      "level_array": [
        {
          "name": "uninodal",
          "model_options": {
            "use_single_bus": true,
            "use_kirchhoff": false
          },
          "sddp_options": {
            "max_iterations": 15,
            "convergence_tol": 0.01
          }
        },
        {
          "name": "full_network",
          "model_options": {
            "use_single_bus": false,
            "use_kirchhoff": true
          },
          "sddp_options": {
            "max_iterations": 50,
            "convergence_tol": 0.001
          },
          "transition": {
            "inherit_optimality_cuts": true,
            "inherit_targets": true,
            "target_rtol": 0.05,
            "target_min_atol": 1.0,
            "target_penalty": 500
          }
        }
      ]
    }
  }
}
```

**Reusing the LP** — a level without `model_options` reuses the
previous level's LP and solver state.  This is useful when you want
to change solver parameters (e.g. enable apertures) without rebuilding:

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
          "name": "benders_warm_start",
          "model_options": {
            "use_single_bus": true,
            "use_kirchhoff": false
          },
          "sddp_options": {
            "max_iterations": 10
          }
        },
        {
          "name": "sddp_with_apertures",
          "sddp_options": {
            "max_iterations": 20,
            "apertures": [1, 2, 3]
          },
          "transition": {
            "inherit_targets": true
          }
        }
      ]
    }
  }
}
```

To use the built-in 4-level default, simply omit `level_array` (or set
it to an empty array):

```json
{
  "options": {
    "method": "cascade",
    "sddp_options": {
      "max_iterations": 30,
      "convergence_tol": 0.01
    },
    "cascade_options": {}
  }
}
```

### 10.6 Iteration Budget

Each level has its own `max_iterations` budget (from its `sddp_options`
or from the global `sddp_options` default).  The global
`cascade_options.sddp_options.max_iterations` serves as the **global
iteration budget** across all levels.  Per-level
`sddp_options.max_iterations` controls iterations within each level.

If a level exhausts its budget without converging, the cascade proceeds
to the next level.  If the final level exhausts its budget, the solver
reports non-convergence.

---

## 11. See Also

- [Cascade Method](cascade.md) --- cascade (multi-level hybrid)
  solver documentation
- [Monolithic Method](monolithic.md) --- monolithic solver
  documentation (default solver, boundary cuts, solve timeout)
- [Mathematical Formulation](../formulation/mathematical-formulation.md) ---
  full LP/MIP formulation for gtopt
- [Planning Guide](../planning-guide.md) --- worked examples and time
  structure concepts
- [Input Data Reference](../input-data.md) --- JSON/Parquet input format
  specification
- [Usage Guide](../usage.md) --- CLI reference including `--trace-log`
- [Contributing Guide](../../CONTRIBUTING.md) --- code style and testing guidelines
