# Unit Commitment Implementation Plan for gtopt

## Commitment Constraints: Analysis, Literature Review, and Implementation Roadmap

**Date**: 2026-04-06
**Status**: Planning / Pre-Implementation
**Author**: Generated from codebase analysis, literature review, and PLEXOS/PyPSA/GenX comparison

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Background and Motivation](#2-background-and-motivation)
3. [Literature Review](#3-literature-review)
   - [3.1 Classical UC Formulation](#31-classical-uc-formulation)
   - [3.2 Tight MIP Formulations (Morales-España et al.)](#32-tight-mip-formulations-morales-españa-et-al)
   - [3.3 Clustered Integer Commitment (Palmintier)](#33-clustered-integer-commitment-palmintier)
4. [Industry Tool Comparison](#4-industry-tool-comparison)
   - [4.1 PLEXOS](#41-plexos)
   - [4.2 PyPSA](#42-pypsa)
   - [4.3 GenX](#43-genx)
   - [4.4 Feature Comparison Matrix](#44-feature-comparison-matrix)
5. [Current gtopt Architecture](#5-current-gtopt-architecture)
   - [5.1 Generator LP Formulation](#51-generator-lp-formulation)
   - [5.2 Time Structure (Stage/Block)](#52-time-structure-stageblock)
   - [5.3 MIP Infrastructure](#53-mip-infrastructure)
   - [5.4 Component Patterns (AddToLP concept)](#54-component-patterns-addtolp-concept)
6. [Proposed Commitment Element Design](#6-proposed-commitment-element-design)
   - [6.1 Applicability: Chronological Stages](#61-applicability-chronological-stages)
   - [6.2 New Data Structures](#62-new-data-structures)
   - [6.3 JSON Input Schema](#63-json-input-schema)
   - [6.4 Mathematical Formulation](#64-mathematical-formulation)
7. [C++ Implementation Plan](#7-c-implementation-plan)
   - [7.1 New Headers and Sources](#71-new-headers-and-sources)
   - [7.2 System Integration](#72-system-integration)
   - [7.3 LP Assembly Logic](#73-lp-assembly-logic)
   - [7.4 Output Variables](#74-output-variables)
8. [Implementation Phases](#8-implementation-phases)
   - [Phase 1: Core Three-Bin UC](#phase-1-core-three-bin-uc)
   - [Phase 2: Ramp Constraints](#phase-2-ramp-constraints)
   - [Phase 3: Min Up/Down Time](#phase-3-min-updown-time)
   - [Phase 4: Advanced Features](#phase-4-advanced-features)
9. [Testing Strategy](#9-testing-strategy)
10. [Risk Assessment](#10-risk-assessment)
11. [References](#11-references)

---

## 1. Executive Summary

This document proposes adding **unit commitment (UC)** constraints to gtopt via
a new `Commitment` element that associates with existing `Generator` objects.
The commitment element introduces binary on/off variables and the full set of
inter-temporal MIP constraints (startup/shutdown logic, minimum up/down times,
ramp rates, and startup/shutdown costs) needed for realistic thermal generator
scheduling.

**Key design decisions:**

- The commitment element applies **only to chronological stages** where each
  block represents a consecutive hour (e.g., a weekly stage with 168 hourly
  blocks).
- The formulation follows the **three-bin** (u, v, w) approach from
  Morales-España et al. (2013, 2015), which provides the tightest known LP
  relaxation for standard UC constraints.
- The design is a **separate element** (like `GeneratorProfile` or
  `ReserveProvision`) rather than inline generator fields, keeping the
  existing `Generator` struct unchanged and making UC opt-in per generator.
- The implementation reuses gtopt's existing MIP infrastructure (`SparseCol`
  with `is_integer = true`, `FlatLinearProblem::colint`).

**Estimated implementation effort:** 4 phases, from core commitment variables
(Phase 1) through advanced features like startup cost curves and multiple
startup types (Phase 4).

---

## 2. Background and Motivation

### The Unit Commitment Problem

The **unit commitment (UC)** problem determines the optimal on/off schedule of
generating units over a time horizon, subject to physical and operational
constraints. It extends the economic dispatch problem (which gtopt already
solves) by adding:

- **Binary commitment decisions**: whether each generator is online or offline
  in each time period
- **Inter-temporal coupling constraints**: minimum up/down times prevent rapid
  cycling; ramp constraints limit power output changes between consecutive
  periods
- **Startup/shutdown costs**: fixed costs incurred when changing the
  commitment state of a unit

### Why gtopt Needs UC

Currently, gtopt treats generators as continuously dispatchable within
`[pmin, pmax]` bounds. This is appropriate for long-term capacity planning
(GTEP) where blocks may represent aggregated load duration curves rather than
consecutive hours. However, for **operational planning** studies with
chronological hourly resolution:

1. **Thermal generators cannot be turned on/off instantly** — coal plants may
   need 8+ hours minimum up-time after startup
2. **Minimum stable generation** (`pmin`) should only be enforced when the unit
   is online; an offline unit should produce exactly zero
3. **Ramp rate limits** constrain how quickly a unit can change its output from
   one hour to the next
4. **Startup costs** ($5,000–$50,000 per start for large thermal units) are a
   significant component of total operating cost

Without UC constraints, the model may produce infeasible or unrealistic
operating schedules that cannot be implemented in practice.

### Scope: Chronological Stages Only

UC constraints **only apply to stages where blocks represent consecutive time
periods** (chronological representation). This means:

- A stage represents a specific time span (e.g., one week)
- Its blocks represent consecutive hours within that span (e.g., 168 hourly
  blocks for a week)
- The first block is the first hour of the period, the second block is the
  second hour, etc.

This is distinct from gtopt's **load-duration curve** representation where
blocks are ordered by magnitude rather than time sequence. UC constraints are
physically meaningless on non-chronological blocks since inter-temporal
constraints (ramps, min up/down times) require temporal adjacency.

---

## 3. Literature Review

### 3.1 Classical UC Formulation

The standard three-variable (three-bin) UC formulation introduces three sets of
binary variables per generator $g$ and time period $t$:

| Variable | Type | Description |
|----------|------|-------------|
| $u_{g,t}$ | Binary | 1 if generator $g$ is ON at time $t$ |
| $v_{g,t}$ | Binary | 1 if generator $g$ starts up at time $t$ |
| $w_{g,t}$ | Binary | 1 if generator $g$ shuts down at time $t$ |

The **logical constraint** linking these variables is:

$$
u_{g,t} - u_{g,t-1} = v_{g,t} - w_{g,t} \quad \forall g, t > 1
$$

This ensures that a change in commitment status is accompanied by exactly one
startup or shutdown event.

**Generation limits** become commitment-dependent:

$$
P^{\min}_g \cdot u_{g,t} \leq p_{g,t} \leq P^{\max}_g \cdot u_{g,t}
\quad \forall g, t
$$

When $u_{g,t} = 0$ (unit offline), the output is forced to zero. When
$u_{g,t} = 1$ (unit online), the output must be within `[pmin, pmax]`.

**Startup/shutdown costs** are added to the objective:

$$
\sum_{g,t} \left( C^{su}_g \cdot v_{g,t} + C^{sd}_g \cdot w_{g,t} \right)
$$

### 3.2 Tight MIP Formulations (Morales-España et al.)

Morales-España, Gentile, and Ramos (2013, 2015) developed **convex-hull
formulations** for UC that produce significantly tighter LP relaxations than
the classical big-M approach. Key contributions:

1. **Minimum up/down time constraints** using aggregated indicator formulation:

   $$
   \sum_{\tau=t}^{t + UT_g - 1} u_{g,\tau} \geq UT_g \cdot v_{g,t}
   \quad \forall g, t: t + UT_g - 1 \leq T
   $$

   $$
   \sum_{\tau=t}^{t + DT_g - 1} (1 - u_{g,\tau}) \geq DT_g \cdot w_{g,t}
   \quad \forall g, t: t + DT_g - 1 \leq T
   $$

2. **Tight ramp constraints** that distinguish between online ramping and
   startup/shutdown ramping:

   $$
   p_{g,t} - p_{g,t-1} \leq RU_g \cdot u_{g,t-1}
   + SU_g \cdot v_{g,t} \quad \forall g, t > 1
   $$

   $$
   p_{g,t-1} - p_{g,t} \leq RD_g \cdot u_{g,t}
   + SD_g \cdot w_{g,t} \quad \forall g, t > 1
   $$

   Where $RU_g, RD_g$ are online ramp rates and $SU_g, SD_g$ are
   startup/shutdown ramp limits (typically $SU_g = P^{\max}_g - P^{\min}_g$
   for a single-period startup).

3. **Convex hull description**: Their formulation provides the convex hull of
   the feasible integer set for each individual unit, meaning the LP relaxation
   produces integer solutions more frequently, reducing branch-and-bound effort.

### 3.3 Clustered Integer Commitment (Palmintier)

Palmintier (2011, 2013, 2014) introduced **clustered integer commitment** for
capacity expansion models. Instead of one binary variable per unit, identical
units are grouped into clusters with integer (not binary) commitment variables:

$$
0 \leq \nu_{g,t} \leq N_g \quad (\text{integer})
$$

Where $N_g$ is the number of units in cluster $g$. This reduces the number of
integer variables from $N_g \times T$ binaries to $T$ integers per cluster.

This approach is used by GenX and is relevant for gtopt's capacity expansion
context where multiple identical modules may be installed via `expmod`.

---

## 4. Industry Tool Comparison

### 4.1 PLEXOS

PLEXOS (Energy Exemplar) is the industry-standard production cost model with
comprehensive UC support:

| Property | Type | Description |
|----------|------|-------------|
| `Units` | Integer | Number of identical units in the generator group |
| `Min Stable Level` | MW | Minimum output when online |
| `Min Up Time` | Hours | Minimum time unit must stay on after start |
| `Min Down Time` | Hours | Minimum time unit must stay off after shutdown |
| `Start Cost` | $ | Fixed cost per startup event |
| `Shutdown Cost` | $ | Fixed cost per shutdown event |
| `Max Ramp Up` | MW/min | Maximum rate of output increase |
| `Max Ramp Down` | MW/min | Maximum rate of output decrease |
| `Start Profile` | MW sequence | Multi-period startup trajectory |
| `Shutdown Profile` | MW sequence | Multi-period shutdown trajectory |
| `Hot/Warm/Cold Start Cost` | $ | Temperature-dependent startup costs |
| `Hot/Warm/Cold Start Time` | Hours | Time thresholds for startup types |
| `Run Up Rate` | MW/min | Ramp rate during startup phase |
| `Run Down Rate` | MW/min | Ramp rate during shutdown phase |

PLEXOS also supports **integer relaxation** (LP relaxation of binaries) for
faster solution in long-term studies.

### 4.2 PyPSA

PyPSA (Python for Power System Analysis) implements UC through the
`committable=True` attribute on generators:

| Parameter | Type | Description |
|-----------|------|-------------|
| `committable` | bool | Enable UC constraints for this generator |
| `p_min_pu` | p.u. | Minimum output fraction when committed |
| `min_up_time` | int | Minimum consecutive online snapshots |
| `min_down_time` | int | Minimum consecutive offline snapshots |
| `start_up_cost` | $ | Startup cost per event |
| `shut_down_cost` | $ | Shutdown cost per event |
| `ramp_limit_up` | p.u./period | Maximum output increase per period |
| `ramp_limit_down` | p.u./period | Maximum output decrease per period |
| `up_time_before` | int | Initial hours online at horizon start |
| `down_time_before` | int | Initial hours offline at horizon start |
| `stand_by_cost` | $/h | Cost when committed but not at maximum |

### 4.3 GenX

GenX (MIT/Princeton) uses clustered integer commitment with configurable UC
detail:

| Setting | Value | Description |
|---------|-------|-------------|
| `UCommit` | 0 | No UC (continuous dispatch) |
| `UCommit` | 1 | Full integer clustering |
| `UCommit` | 2 | Linearized (relaxed) clustering |

Generator parameters include:
- `Num_Units`: Number of units in cluster
- `Min_Power`: Minimum stable output per unit
- `Up_Time`: Minimum up time (hours)
- `Down_Time`: Minimum down time (hours)
- `Start_Cost_per_MW`: Startup cost
- `Ramp_Up_Percentage`: Max ramp up as fraction of capacity
- `Ramp_Dn_Percentage`: Max ramp down as fraction of capacity

### 4.4 Feature Comparison Matrix

| Feature | PLEXOS | PyPSA | GenX | **gtopt (proposed)** |
|---------|--------|-------|------|---------------------|
| Binary on/off | ✓ | ✓ | ✓ (integer clusters) | ✓ Phase 1 |
| Startup/shutdown variables | ✓ | implicit | ✓ (integer) | ✓ Phase 1 |
| Startup cost | ✓ | ✓ | ✓ | ✓ Phase 1 |
| Shutdown cost | ✓ | ✓ | ✗ | ✓ Phase 1 |
| Min up time | ✓ | ✓ | ✓ | ✓ Phase 3 |
| Min down time | ✓ | ✓ | ✓ | ✓ Phase 3 |
| Ramp up limit | ✓ | ✓ | ✓ | ✓ Phase 2 |
| Ramp down limit | ✓ | ✓ | ✓ | ✓ Phase 2 |
| Startup ramp limit | ✓ | ✗ | ✗ | ✓ Phase 2 |
| Shutdown ramp limit | ✓ | ✗ | ✗ | ✓ Phase 2 |
| Multi-period startup trajectories | ✓ | ✗ | ✗ | ✗ (Phase 4) |
| Hot/warm/cold startup costs | ✓ | ✗ | ✗ | ✗ (Phase 4) |
| Integer clustering | ✓ | ✗ | ✓ | ✗ (Phase 4) |
| Initial conditions | ✓ | ✓ | ✓ | ✓ Phase 1 |
| LP relaxation mode | ✓ | ✗ | ✓ | ✓ Phase 1 |
| Integration with GTEP | ✓ | partial | ✓ | ✓ native |

---

## 5. Current gtopt Architecture

### 5.1 Generator LP Formulation

The current `GeneratorLP` class (`source/generator_lp.cpp`) creates:

1. **Generation variable** $p_{g,b}$ per block with bounds $[P^{\min}_g, P^{\max}_g]$
2. **Bus balance contribution**: $(1 - \lambda_g) \cdot p_{g,b}$ added to the
   bus power balance row
3. **Capacity constraint**: $p_{g,b} \leq C^{inst}_g$ when capacity expansion
   is modeled

```
GeneratorLP
  ├── CapacityObjectLP<Generator>     // capacity expansion
  │     ├── ObjectLP<Generator>       // identity, active status
  │     └── CapacityObjectBase        // capainst, capacost, expmod cols/rows
  ├── pmin, pmax schedules            // OptTBRealSched
  ├── lossfactor, gcost schedules     // OptTRealSched
  ├── generation_cols                 // STBIndexHolder<ColIndex>
  └── capacity_rows                   // STBIndexHolder<RowIndex>
```

Key observation: The current `generation_cols` are indexed by
`(ScenarioUid, StageUid) → BIndexHolder<ColIndex>`, meaning each block
within a stage already has its own LP variable. This is exactly the
structure needed for inter-temporal UC constraints.

### 5.2 Time Structure (Stage/Block)

```
Stage (uid, first_block, count_block, discount_factor)
  └── Block[] (uid, duration in hours)
```

For UC to apply, a stage must have **chronological blocks** where:
- Block durations are uniform (typically 1 hour)
- Blocks are time-consecutive (block $i$ is followed by block $i+1$)
- The stage represents a specific calendar period (e.g., one week = 168 blocks)

The `StageLP` class already provides:
- `blocks()` — ordered vector of `BlockLP` within the stage
- `duration()` — total stage duration in hours
- Block iteration in the order they appear

### 5.3 MIP Infrastructure

gtopt already supports integer variables:

- `SparseCol::is_integer` flag marks a variable as integer-constrained
- `LinearProblem::colints` tracks the count of integer variables
- `FlatLinearProblem::colint` collects integer variable indices for the solver
- The `CapacityObjectLP` uses `is_integer` for `expmod` variables when integer
  expansion is enabled

The solver backends (CBC, CPLEX, HiGHS) all support MIP solving natively.
CLP is LP-only but can be used when UC is relaxed.

### 5.4 Component Patterns (AddToLP concept)

New LP elements must satisfy the `AddToLP` concept:

```cpp
template<typename T>
concept AddToLP = requires(T obj,
                           SystemContext& system_context,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp,
                           OutputContext& output_context) {
  { obj.add_to_lp(system_context, scenario, stage, lp) } -> std::same_as<bool>;
  { obj.add_to_output(output_context) } -> std::same_as<bool>;
};
```

The `Commitment` element will follow this pattern, similar to how
`ReserveProvision` links a `Generator` to a `ReserveZone`, or how
`GeneratorProfile` modifies generator bounds.

---

## 6. Proposed Commitment Element Design

### 6.1 Applicability: Chronological Stages

The commitment element will be processed **only** for stages that meet the
chronological criteria. The stage property `chronological` (a new optional
boolean field on the `Stage` struct) indicates that blocks within this stage
are time-consecutive.

**Detection heuristic** (when `chronological` is not explicitly set):
- All blocks have equal duration
- Duration is exactly 1.0 hour (or a common subdivision like 0.5 or 0.25)
- Block count matches a standard chronological period (24, 48, 168, 720, 8760)

When a commitment element is present and the stage is not chronological, the
commitment constraints are **silently skipped** for that stage (the generator
falls back to its default continuous dispatch behavior).

### 6.2 New Data Structures

#### `Commitment` struct (new, `include/gtopt/commitment.hpp`)

```cpp
struct Commitment
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  // Generator reference
  SingleId generator {unknown_uid};

  // Commitment costs
  OptTRealFieldSched startup_cost {};    // $/start [per stage]
  OptTRealFieldSched shutdown_cost {};   // $/stop  [per stage]

  // Timing constraints
  OptReal min_up_time {};      // Minimum hours online after startup
  OptReal min_down_time {};    // Minimum hours offline after shutdown

  // Ramp rate constraints (MW/h)
  OptTBRealFieldSched ramp_up {};       // Max hourly increase when online
  OptTBRealFieldSched ramp_down {};     // Max hourly decrease when online
  OptReal startup_ramp {};    // Max output in first online period [MW]
  OptReal shutdown_ramp {};   // Max output in last online period [MW]

  // Initial conditions (at start of stage)
  OptReal initial_status {};  // 1.0 = online, 0.0 = offline
  OptReal initial_hours {};   // Hours in current state at stage start

  // Relaxation option
  OptBool relax {};           // true = LP relax (continuous [0,1])
};
```

#### `Stage` struct modification (existing, `include/gtopt/stage.hpp`)

Add an optional `chronological` field:

```cpp
struct Stage
{
  // ... existing fields ...
  OptBool chronological {};  // blocks are time-consecutive hours
};
```

### 6.3 JSON Input Schema

```json
{
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 1},
      ...
      {"uid": 168, "duration": 1}
    ],
    "stage_array": [
      {
        "uid": 1,
        "first_block": 0,
        "count_block": 168,
        "chronological": true
      }
    ]
  },
  "system": {
    "generator_array": [
      {
        "uid": 1, "name": "coal_unit", "bus": "b1",
        "pmin": 100, "pmax": 500, "gcost": 25, "capacity": 500
      }
    ],
    "commitment_array": [
      {
        "uid": 1,
        "name": "coal_commit",
        "generator": "coal_unit",
        "startup_cost": 15000,
        "shutdown_cost": 5000,
        "min_up_time": 8,
        "min_down_time": 6,
        "ramp_up": 100,
        "ramp_down": 80,
        "startup_ramp": 150,
        "shutdown_ramp": 150,
        "initial_status": 1.0,
        "initial_hours": 10
      }
    ]
  }
}
```

### 6.4 Mathematical Formulation

#### Sets and Indices

| Symbol | Description |
|--------|-------------|
| $g \in \mathcal{G}^{uc}$ | Generators with commitment constraints |
| $t \in \mathcal{T}_s$ | Time blocks within chronological stage $s$ |
| $UT_g$ | Minimum up time (number of blocks) |
| $DT_g$ | Minimum down time (number of blocks) |
| $RU_g$ | Ramp-up limit (MW/block) |
| $RD_g$ | Ramp-down limit (MW/block) |
| $SU_g$ | Startup ramp limit (MW) |
| $SD_g$ | Shutdown ramp limit (MW) |

#### Decision Variables

| Variable | Type | Domain | Description |
|----------|------|--------|-------------|
| $p_{g,t}$ | Continuous | $[0, P^{\max}_g]$ | Power output (existing gen var) |
| $u_{g,t}$ | Binary | $\{0, 1\}$ | Commitment status (1 = online) |
| $v_{g,t}$ | Binary | $\{0, 1\}$ | Startup indicator |
| $w_{g,t}$ | Binary | $\{0, 1\}$ | Shutdown indicator |

When `relax = true`, the binary variables become continuous $\in [0, 1]$.

#### Constraints

**C1. Logical status transition** (3-bin linking):

$$
u_{g,t} - u_{g,t-1} = v_{g,t} - w_{g,t} \quad \forall g \in \mathcal{G}^{uc}, \; t > 1
$$

For $t = 1$: $u_{g,1} - u^0_g = v_{g,1} - w_{g,1}$ where $u^0_g$ is the
initial status.

**C2. Commitment-dependent generation limits**:

$$
P^{\min}_g \cdot u_{g,t} \leq p_{g,t} \leq P^{\max}_g \cdot u_{g,t}
\quad \forall g, t
$$

This replaces the current unconditional $[P^{\min}_g, P^{\max}_g]$ bounds on
the generation variable.

**C3. Startup/shutdown exclusion**:

$$
v_{g,t} + w_{g,t} \leq 1 \quad \forall g, t
$$

A unit cannot start up and shut down in the same period.

**C4. Minimum up time** (aggregated form):

$$
\sum_{\tau=t}^{\min(t + UT_g - 1, \, T)} u_{g,\tau} \geq UT_g \cdot v_{g,t}
\quad \forall g, t
$$

With boundary correction for the end of the horizon:
when $t + UT_g - 1 > T$, the constraint becomes
$\sum_{\tau=t}^{T} u_{g,\tau} \geq (T - t + 1) \cdot v_{g,t}$.

**C5. Minimum down time** (aggregated form):

$$
\sum_{\tau=t}^{\min(t + DT_g - 1, \, T)} (1 - u_{g,\tau}) \geq DT_g \cdot w_{g,t}
\quad \forall g, t
$$

With analogous boundary correction.

**C6. Ramp-up constraint** (tight form):

$$
p_{g,t} - p_{g,t-1} \leq RU_g \cdot u_{g,t-1} + SU_g \cdot v_{g,t}
\quad \forall g, t > 1
$$

**C7. Ramp-down constraint** (tight form):

$$
p_{g,t-1} - p_{g,t} \leq RD_g \cdot u_{g,t} + SD_g \cdot w_{g,t}
\quad \forall g, t > 1
$$

#### Objective Function Additions

$$
\text{minimize} \quad \ldots + \sum_{g \in \mathcal{G}^{uc}} \sum_{t \in \mathcal{T}_s}
\delta_s \cdot \pi_s \left(
  C^{su}_g \cdot v_{g,t} + C^{sd}_g \cdot w_{g,t}
\right)
$$

Where $\delta_s$ is the stage discount factor and $\pi_s$ is the scenario
probability factor (consistent with gtopt's existing objective scaling).

---

## 7. C++ Implementation Plan

### 7.1 New Headers and Sources

| File | Purpose |
|------|---------|
| `include/gtopt/commitment.hpp` | `Commitment` struct (data model) |
| `include/gtopt/commitment_lp.hpp` | `CommitmentLP` class declaration |
| `source/commitment_lp.cpp` | `CommitmentLP::add_to_lp` implementation |
| `test/source/test_commitment.cpp` | Unit tests |

### 7.2 System Integration

1. **`system.hpp`**: Add `Array<Commitment> commitment_array {};`

2. **`system_lp.hpp`**: Add `Collection<CommitmentLP>` to `collections_t` tuple
   (must be placed **after** `Collection<GeneratorLP>` since it references
   generator columns)

3. **`system_lp.cpp`**: Construct `CommitmentLP` objects from `commitment_array`
   in the collection initialization

4. **`system.cpp`**: Handle `commitment_array` in `merge()` and JSON parsing

5. **`stage.hpp`**: Add `OptBool chronological {};` field

6. **JSON parsing**: Add `commitment_array` to the DAW JSON mapping for
   `System`, and `chronological` to `Stage`

### 7.3 LP Assembly Logic

The `CommitmentLP::add_to_lp` method follows this pseudocode:

```
add_to_lp(sc, scenario, stage, lp):
  // 1. Check if stage is chronological
  if not stage.is_chronological():
    return true  // skip silently

  // 2. Look up the referenced GeneratorLP
  gen_lp = sc.system_lp.element<GeneratorLP>(generator_sid)
  gen_cols = gen_lp.generation_cols_at(scenario, stage)

  // 3. Create binary variables for each block
  for each block in stage.blocks():
    u[b] = lp.add_col({name="uc_u", lowb=0, uppb=1, is_integer=!relax})
    v[b] = lp.add_col({name="uc_v", lowb=0, uppb=1, is_integer=!relax,
                        cost=startup_cost * discount * prob})
    w[b] = lp.add_col({name="uc_w", lowb=0, uppb=1, is_integer=!relax,
                        cost=shutdown_cost * discount * prob})

  // 4. Modify generation variable bounds (commitment-dependent)
  for each block b:
    gen_col = gen_cols[b]
    // Replace fixed [pmin, pmax] with commitment-dependent constraints
    lp.set_col_bounds(gen_col, 0, Pmax)  // widen to [0, Pmax]

    // C2a: p >= pmin * u
    row_lo = SparseRow{}.greater_equal(0)
    row_lo[gen_col] = 1
    row_lo[u[b]] = -pmin
    lp.add_row(row_lo)

    // C2b: p <= pmax * u
    row_hi = SparseRow{}.greater_equal(0)
    row_hi[u[b]] = pmax
    row_hi[gen_col] = -1
    lp.add_row(row_hi)

  // 5. Logical transition constraints (C1)
  for b = 1 to last_block:
    row = SparseRow{}.equal(0)
    row[u[b]] = 1
    row[u[b-1]] = -1
    row[v[b]] = -1
    row[w[b]] = 1
    lp.add_row(row)

  // For first block: use initial_status
  row0 = SparseRow{}.equal(0)
  row0[u[0]] = 1
  row0[v[0]] = -1
  row0[w[0]] = 1
  row0.lowb = initial_status
  row0.uppb = initial_status
  // ... (encoded as row with constant RHS)

  // 6. Startup/shutdown exclusion (C3)
  for each block b:
    row = SparseRow{}.less_equal(1)
    row[v[b]] = 1
    row[w[b]] = 1
    lp.add_row(row)

  // 7. Min up/down time constraints (C4, C5) — Phase 3
  // 8. Ramp constraints (C6, C7) — Phase 2
```

### 7.4 Output Variables

| Output file | Column pattern | Description |
|-------------|---------------|-------------|
| `Commitment/status_sol.csv` | `uid:N` | Commitment status $u_{g,t}$ |
| `Commitment/startup_sol.csv` | `uid:N` | Startup events $v_{g,t}$ |
| `Commitment/shutdown_sol.csv` | `uid:N` | Shutdown events $w_{g,t}$ |
| `Commitment/startup_cost.csv` | `uid:N` | Startup cost per period |
| `Commitment/shutdown_cost.csv` | `uid:N` | Shutdown cost per period |

---

## 8. Implementation Phases

### Phase 1: Core Three-Bin UC

**Goal**: Binary commitment variables with startup/shutdown logic and costs

**Deliverables**:
- [ ] `commitment.hpp` — `Commitment` struct
- [ ] `commitment_lp.hpp` / `commitment_lp.cpp` — LP class with `add_to_lp`
- [ ] `stage.hpp` — Add `chronological` field
- [ ] `system.hpp` / `system_lp.hpp` — Integration
- [ ] JSON parsing for `commitment_array` and `chronological`
- [ ] Constraints C1 (logic), C2 (gen limits), C3 (exclusion)
- [ ] Startup/shutdown costs in objective
- [ ] Initial conditions handling
- [ ] LP relaxation mode (`relax` flag)
- [ ] Output: `status_sol`, `startup_sol`, `shutdown_sol`, costs
- [ ] Unit tests: 3-generator UC dispatch problem

**Estimated effort**: 3–5 days

**Constraints generated per generator per stage** (N blocks):
- N commitment variables ($u$)
- N startup variables ($v$) with objective costs
- N shutdown variables ($w$) with objective costs
- N−1 logical transition rows (C1)
- 1 initial condition row
- 2N generation limit rows (C2)
- N startup/shutdown exclusion rows (C3)
- **Total: 3N variables, 4N rows**

### Phase 2: Ramp Constraints

**Goal**: Inter-temporal ramp rate limits

**Deliverables**:
- [ ] `ramp_up`, `ramp_down` fields on `Commitment`
- [ ] `startup_ramp`, `shutdown_ramp` fields
- [ ] Constraints C6 (ramp up) and C7 (ramp down)
- [ ] Tests: verify ramp-constrained dispatch

**Estimated effort**: 1–2 days

**Additional constraints**: 2(N−1) ramp rows per generator

### Phase 3: Min Up/Down Time

**Goal**: Minimum operating time constraints

**Deliverables**:
- [ ] `min_up_time`, `min_down_time` fields on `Commitment`
- [ ] Constraints C4 (min up) and C5 (min down)
- [ ] Horizon boundary handling
- [ ] `initial_hours` for carryover from previous stage
- [ ] Tests: verify minimum time enforcement

**Estimated effort**: 2–3 days

**Additional constraints**: Up to 2N rows per generator (fewer near horizon end)

### Phase 4: Advanced Features

**Goal**: Features for parity with PLEXOS and advanced use cases

**Potential deliverables** (prioritize based on user needs):
- [ ] Integer clustering (multiple identical units via `num_units` field)
- [ ] Hot/warm/cold startup cost tiers
- [ ] Multi-period startup/shutdown trajectories
- [ ] Cross-stage commitment state carryover (via `StateVariable` mechanism)
- [ ] `element_column_resolver` integration for user constraints referencing
      commitment variables
- [ ] Integration with SDDP decomposition (commitment as complicating variables)

**Estimated effort**: 1–2 weeks (depending on scope)

---

## 9. Testing Strategy

### Unit Tests

Each phase will include targeted tests following the existing doctest pattern:

**Phase 1 test: Basic UC dispatch**
```cpp
// 2-generator, 4-block chronological stage
// g1: cheap thermal with commitment (pmin=100, pmax=500, startup_cost=10000)
// g2: expensive peaker (no commitment, pmax=200)
// Demand: [150, 400, 300, 100]
// Expected: g1 stays online for blocks 1-3 (up-time cost < shutdown+restart),
//           g1 offline at block 4 (demand < pmin)
// Verify: u, v, w variables; objective includes startup costs
```

**Phase 2 test: Ramp-limited dispatch**
```cpp
// 1-generator, 6-block stage with ramp_up=50, ramp_down=50
// Demand ramp: [100, 200, 350, 400, 300, 100]
// Verify: generation changes ≤ 50 MW/block between consecutive blocks
```

**Phase 3 test: Minimum up/down time**
```cpp
// 1-generator, 12-block stage with min_up_time=4, min_down_time=3
// Demand pattern forcing the unit to cycle
// Verify: once started, unit stays on for ≥ 4 blocks
// Verify: once stopped, unit stays off for ≥ 3 blocks
```

### Integration Tests

- Run existing IEEE benchmark cases with commitment elements added to thermal
  generators and verify solution quality
- Verify that non-chronological stages (existing cases) are unaffected

### Regression Tests

- All existing tests must pass unchanged — the commitment element is purely
  additive and does not modify generator behavior when no `commitment_array`
  is present

---

## 10. Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| MIP solve time too slow for large systems | High | LP relaxation mode; integer clustering (Phase 4); solver timeout options already exist |
| CBC solver insufficient for large MIPs | Medium | HiGHS and CPLEX backends already available; HiGHS has excellent MIP performance |
| Inter-stage commitment state not handled | Medium | Phase 4 addresses cross-stage carryover via existing `StateVariable` mechanism |
| Commitment interacts with SDDP decomposition | Medium | Initial implementation is monolithic-only; SDDP integration deferred to Phase 4 |
| `pmin > 0` breaks when unit is offline | Low | C2 constraints properly handle this; generation variable bounds widened to `[0, pmax]` |
| Numerical scaling issues with binary variables | Low | gtopt's `scale_objective` already handles cost scaling; binary variables are well-conditioned |

---

## 11. References

<a id="ref-UC1"></a>
**[UC1]** Morales-España, G., Gentile, C., Ramos, A. "Tight MIP formulations
of the power-based unit commitment problem." *OR Spectrum*, 37(4):929–950,
2015. [doi:10.1007/s00291-015-0400-4](https://doi.org/10.1007/s00291-015-0400-4)

<a id="ref-UC2"></a>
**[UC2]** Morales-España, G., Latorre, J.M., Ramos, A. "Tight and compact MILP
formulation for the thermal unit commitment problem." *IEEE Transactions on
Power Systems*, 28(4):4897–4908, 2013.
[doi:10.1109/TPWRS.2013.2251373](https://doi.org/10.1109/TPWRS.2013.2251373)

<a id="ref-UC3"></a>
**[UC3]** Gentile, C., Morales-España, G., Ramos, A. "A tight MIP formulation
of the unit commitment problem with start-up and shut-down constraints."
*EURO Journal on Computational Optimization*, 5(1–2):177–201, 2017.
[doi:10.1007/s13675-016-0066-y](https://doi.org/10.1007/s13675-016-0066-y)

<a id="ref-UC4"></a>
**[UC4]** Palmintier, B. "Incorporating operational flexibility into electric
generation planning: impacts and methods for system design and policy analysis."
*PhD Thesis*, MIT, 2013.

<a id="ref-UC5"></a>
**[UC5]** Knueven, B., Ostrowski, J., Watson, J.-P. "On mixed-integer
programming formulations for the unit commitment problem." *INFORMS Journal
on Computing*, 32(4):857–876, 2020.
[doi:10.1287/ijoc.2019.0944](https://doi.org/10.1287/ijoc.2019.0944)

<a id="ref-UC6"></a>
**[UC6]** Rajan, D., Takriti, S. "Minimum up/down polytopes of the unit
commitment problem with start-up costs." *IBM Research Report RC23628*, 2005.

<a id="ref-UC7"></a>
**[UC7]** Brown, T., Hörsch, J., Schlachtberger, D. "PyPSA: Python for Power
System Analysis." *Journal of Open Research Software*, 6(4), 2018.
[doi:10.5334/jors.188](https://doi.org/10.5334/jors.188)

<a id="ref-UC8"></a>
**[UC8]** Jenkins, J.D., Sepulveda, N.A. "Enhanced decision support for a
changing electricity landscape: the GenX configurable electricity resource
capacity expansion model." *MIT Energy Initiative Working Paper*, 2017.

<a id="ref-UC9"></a>
**[UC9]** Energy Exemplar. "PLEXOS Integrated Energy Model — Generator
Properties." Technical documentation. Available at
[energyexemplar.com](https://www.energyexemplar.com).

<a id="ref-UC10"></a>
**[UC10]** Buitrago Villada, J.D., Matus, M., et al. "FESOP: Fabulous Energy
System Optimizer." *IEEE Kansas Power and Energy Conference (KPEC)*, 2022.

---

## See Also

- [Mathematical Formulation](../formulation/mathematical-formulation.md) — existing LP/MIP formulation
- [Planning Guide](../planning-guide.md) — worked examples and time structure concepts
- [Input Data Reference](../input-data.md) — JSON input schema documentation
- [Planning Options](../planning-options.md) — solver and model configuration
- [Critical Review Report](critical-review-report.md) — comprehensive codebase analysis
