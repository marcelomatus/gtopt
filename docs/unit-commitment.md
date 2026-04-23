# Unit Commitment in gtopt

> **Status**: Implemented (Phases 1–3).  
> **Since**: v0.9 (April 2025)  
> **See also**: [Input Data Reference](input-data.md) · [Planning Guide](planning-guide.md) ·
> [Mathematical Formulation](formulation/mathematical-formulation.md) · [Usage](usage.md)

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Background and Motivation](#2-background-and-motivation)
3. [Quick Start](#3-quick-start)
4. [The Three-Bin Formulation](#4-the-three-bin-formulation)
   - 4.1 [Decision Variables](#41-decision-variables)
   - 4.2 [Core Constraints (C1–C3)](#42-core-constraints-c1c3)
   - 4.3 [Ramp Constraints (C4–C5)](#43-ramp-constraints-c4c5)
   - 4.4 [Minimum Up/Down Time (C6–C7)](#44-minimum-updown-time-c6c7)
   - 4.5 [Startup Cost Tiers (C8–C10)](#45-startup-cost-tiers-c8c10)
5. [Piecewise Heat Rate Curves](#5-piecewise-heat-rate-curves)
6. [Emission Cost and Cap](#6-emission-cost-and-cap)
7. [Reserve Integration](#7-reserve-integration)
8. [Commitment Periods](#8-commitment-periods)
9. [Relaxation Control](#9-relaxation-control)
10. [JSON Input Reference](#10-json-input-reference)
    - 10.1 [Commitment Object](#101-commitment-object)
    - 10.2 [Stage — Chronological Flag](#102-stage--chronological-flag)
    - 10.3 [Generator — Emission Factor](#103-generator--emission-factor)
    - 10.4 [Model Options — Emission and Relaxation](#104-model-options--emission-and-relaxation)
11. [Output Files](#11-output-files)
12. [Worked Examples](#12-worked-examples)
    - 12.1 [Example 1: Basic UC with Two Generators](#121-example-1-basic-uc-with-two-generators)
    - 12.2 [Example 2: Piecewise Heat Rate](#122-example-2-piecewise-heat-rate)
    - 12.3 [Example 3: Startup Cost Tiers](#123-example-3-startup-cost-tiers)
    - 12.4 [Example 4: Emission Cost Shifting Dispatch](#124-example-4-emission-cost-shifting-dispatch)
    - 12.5 [Example 5: Emission Cap](#125-example-5-emission-cap)
13. [Integration with Planning Framework](#13-integration-with-planning-framework)
14. [Known Limitations](#14-known-limitations)
15. [Comparison with Other Tools](#15-comparison-with-other-tools)
16. [Academic References](#16-academic-references)
17. [Roadmap](#17-roadmap)

---

## 1. Introduction

**Unit commitment (UC)** is the problem of deciding which generating units to
turn on (commit) and off (decommit) over a planning horizon, subject to
technical constraints such as minimum up/down times, ramp rates, and startup
costs.  It is a fundamental problem in power system operations, sitting between
long-term capacity expansion planning and real-time economic dispatch.

gtopt implements a **tight-and-compact three-bin (u, v, w) mixed-integer
programming (MIP) formulation** based on the work of Morales-España, Latorre,
and Ramos (2013).  This formulation is widely recognized as producing the
tightest LP relaxation among compact UC formulations, which translates to
faster solution times and smaller integrality gaps in practice.

### Key Features

- **Three-bin model**: status ($u$), startup ($v$), and shutdown ($w$) binary
  variables with tight coupling constraints
- **Ramp constraints**: tight ramp-up/down limits that depend on commitment
  state, including dedicated startup/shutdown ramp limits
- **Minimum up/down time**: aggregated formulation at the period level
- **Hot/warm/cold startup tiers**: time-dependent startup costs based on
  offline duration
- **Piecewise linear heat rate curves**: multi-segment fuel cost modeling
  with per-segment emission accounting
- **Emission cost and cap**: system-wide carbon price and/or annual CO₂ cap
  with endogenous carbon pricing via dual variables
- **Reserve integration**: spinning reserve headroom conditional on commitment
  status
- **Commitment periods**: optional coarser time resolution for binary variables
  while keeping generation at fine granularity
- **LP relaxation control**: per-element and per-phase relaxation for
  benchmarking and gap analysis
- **Chronological gating**: UC constraints only apply on stages marked as
  chronological; non-chronological stages (load-duration curves) dispatch
  normally

---

## 2. Background and Motivation

### Why Unit Commitment Matters

In power systems with significant thermal generation, the decision to commit
or decommit a generator is not instantaneous — it has physical and economic
consequences:

- **Startup costs**: heating a boiler, synchronizing a turbine, and ramping to
  minimum output can cost thousands to hundreds of thousands of dollars
- **Minimum operating levels**: once committed, a thermal unit must operate
  above its technical minimum ($P_\min$), even if demand could be served more
  cheaply by another unit
- **Minimum up/down times**: a coal plant that takes 8 hours to start cannot
  be cycled on and off hourly
- **Ramping limits**: a nuclear plant or large coal unit can only increase or
  decrease output by a limited amount per hour

Without UC, an economic dispatch model implicitly assumes all generators can
be turned on and off instantaneously at zero cost — a poor approximation for
thermal-dominated systems and increasingly important for systems with high
renewable penetration where thermal units must cycle frequently.

### When to Use Unit Commitment in gtopt

Use UC when your system includes:

- **Thermal generators** (coal, gas, nuclear) with significant startup costs,
  minimum up/down times, or steep ramp limits
- **High renewable penetration** where thermal units must cycle to accommodate
  variable generation
- **Detailed operational studies** (hourly or sub-hourly time resolution) where
  the binary on/off decision materially affects dispatch cost and feasibility
- **Spinning reserve requirements** that depend on which units are online

Do **not** use UC for:

- **Long-term expansion planning** with representative blocks (use
  non-chronological stages — UC is automatically skipped)
- **Hydro-only systems** where generation is fully flexible (no startup costs
  or minimum up times)
- **Screening studies** where approximate dispatch is sufficient

---

## 3. Quick Start

### Minimal JSON example

Add a `commitment_array` to your system and mark the stage as `chronological`:

```json
{
  "system": {
    "generator_array": [
      {
        "uid": 1,
        "name": "coal1",
        "bus": 1,
        "pmin": 200,
        "pmax": 600,
        "gcost": 25,
        "capacity": 600
      },
      {
        "uid": 2,
        "name": "gas_peaker",
        "bus": 1,
        "pmin": 0,
        "pmax": 300,
        "gcost": 60,
        "capacity": 300
      }
    ],
    "commitment_array": [
      {
        "uid": 1,
        "name": "coal1_uc",
        "generator": 1,
        "startup_cost": 50000,
        "shutdown_cost": 5000,
        "noload_cost": 500,
        "min_up_time": 8,
        "min_down_time": 6,
        "ramp_up": 100,
        "ramp_down": 100,
        "initial_status": 1,
        "initial_hours": 12
      }
    ]
  },
  "simulation": {
    "stage_array": [
      {
        "uid": 0,
        "first_block": 0,
        "count_block": 24,
        "chronological": true
      }
    ]
  }
}
```

### Running

```bash
gtopt my_case.json
# Or with LP relaxation for faster solves:
gtopt my_case.json --set model_options.relaxed_phases=all
```

### What happens

1. gtopt detects the `commitment_array` and the `chronological: true` stage
2. For each committed generator, it creates binary variables $u_t$, $v_t$,
   $w_t$ at every block (or commitment period)
3. Core constraints (logic, generation bounds, exclusion) are added
4. Optional constraints (ramp, min up/down, startup tiers) are added based
   on which fields are present
5. The LP/MIP is solved; output files include `status_sol`, `startup_sol`,
   and `shutdown_sol` per committed generator

---

## 4. The Three-Bin Formulation

The formulation uses three families of binary decision variables per committed
generator $g$ and time period $t$:

### 4.1 Decision Variables

| Variable | Type | Domain | Meaning |
|----------|------|--------|---------|
| $u_{g,t}$ | Binary | $\{0, 1\}$ | **Status**: 1 if generator $g$ is online at time $t$ |
| $v_{g,t}$ | Binary | $\{0, 1\}$ | **Startup**: 1 if generator $g$ starts up at time $t$ |
| $w_{g,t}$ | Binary | $\{0, 1\}$ | **Shutdown**: 1 if generator $g$ shuts down at time $t$ |

When `relax = true` or the phase is in `relaxed_phases`, these variables
become continuous in $[0, 1]$.

### 4.2 Core Constraints (C1–C3)

These three constraints form the backbone of the UC formulation and are
**always** present for every committed generator on every chronological stage.

#### C1: Logical Transition

$$u_{g,t} - u_{g,t-1} = v_{g,t} - w_{g,t}$$

For the first period ($t = 0$):

$$u_{g,0} - u_{g,\text{init}} = v_{g,0} - w_{g,0}$$

where $u_{g,\text{init}}$ is `initial_status` (1.0 = online, 0.0 = offline).

This constraint links the three variables: if a unit transitions from off to
on ($u$ goes from 0 to 1), then $v = 1$ (startup event); if it transitions
from on to off, then $w = 1$ (shutdown event).

#### C2: Generation Bounds

$$P_{\min} \cdot u_{g,t} \leq p_{g,t} \leq P_{\max} \cdot u_{g,t}$$

When the unit is offline ($u = 0$), generation is forced to zero. When online
($u = 1$), generation must be within $[P_\min, P_\max]$.

*Implementation note*: The lower bound of the generation variable is set to
zero (`gcol.lowb = 0.0`), and the bounds are enforced via separate inequality
rows. This allows the LP relaxation to produce fractional commitment states.

#### C3: Exclusion

$$v_{g,t} + w_{g,t} \leq 1$$

A unit cannot simultaneously start up and shut down in the same period.

### 4.3 Ramp Constraints (C4–C5)

Ramp constraints limit how fast a generator can change output between
consecutive blocks.  The **tight formulation** uses state-dependent ramp
limits:

#### C4: Ramp Up

$$p_{g,t} - p_{g,t-1} \leq RU_g \cdot u_{g,t-1} + SU_g \cdot v_{g,t}$$

where:
- $RU_g$ = `ramp_up` × block duration [MW] — normal ramp-up rate
- $SU_g$ = `startup_ramp` [MW] — maximum output in the startup block

When a unit starts up ($v_t = 1$, $u_{t-1} = 0$), the ramp limit is $SU_g$
(typically ≤ $P_\max$).  When already online ($u_{t-1} = 1$, $v_t = 0$), the
ramp limit is $RU_g$.

#### C5: Ramp Down

$$p_{g,t-1} - p_{g,t} \leq RD_g \cdot u_{g,t} + SD_g \cdot w_{g,t}$$

where:
- $RD_g$ = `ramp_down` × block duration [MW] — normal ramp-down rate
- $SD_g$ = `shutdown_ramp` [MW] — maximum output in the shutdown block

**When to use**: Set `ramp_up` and/or `ramp_down` for generators with
meaningful ramping limitations (large coal, nuclear).  Set `startup_ramp`
and `shutdown_ramp` when the startup/shutdown trajectory differs from
normal ramping.  If omitted, the startup/shutdown ramp defaults to $P_\max$.

### 4.4 Minimum Up/Down Time (C6–C7)

These constraints prevent short cycling of thermal units.

#### C6: Minimum Up Time

$$\sum_{\tau=t}^{\min(t + UT_g - 1,\, T)} u_{g,\tau} \geq UT_g \cdot v_{g,t}$$

If a unit starts up at time $t$ ($v_t = 1$), it must remain online for at
least $UT_g$ consecutive periods.  The implementation counts periods (not
blocks) and accumulates durations to find how many periods cover
`min_up_time` hours.

#### C7: Minimum Down Time

$$\sum_{\tau=t}^{\min(t + DT_g - 1,\, T)} (1 - u_{g,\tau}) \geq DT_g \cdot w_{g,t}$$

Equivalently:

$$\sum_{\tau=t}^{t + DT_g - 1} u_{g,\tau} + DT_g \cdot w_{g,t} \leq \text{span}$$

where $\text{span}$ is the number of periods in the window.

If a unit shuts down at time $t$ ($w_t = 1$), it must remain offline for at
least $DT_g$ periods.

**Trivial skip**: When a single period's duration already covers
`min_up_time` (or `min_down_time`), the constraint is trivially satisfied
(1 ≥ 1·$v$) and is not added to the LP.

**Non-uniform block durations**: The implementation correctly handles
variable-length blocks by accumulating durations rather than counting
fixed-length blocks.

### 4.5 Startup Cost Tiers (C8–C10)

Many thermal plants have temperature-dependent startup costs: a unit that
was recently offline (still warm) is cheaper to restart than one that has
been cold for days.

When **all five** tier fields are provided (`hot_start_cost`, `warm_start_cost`,
`cold_start_cost`, `hot_start_time`, `cold_start_time`), the single
`startup_cost` on $v$ is replaced by three tier variables:

| Variable | Cost | Condition |
|----------|------|-----------|
| $y^{\text{hot}}_{g,t}$ | `hot_start_cost` | Offline < `hot_start_time` hours |
| $y^{\text{warm}}_{g,t}$ | `warm_start_cost` | `hot_start_time` ≤ offline < `cold_start_time` |
| $y^{\text{cold}}_{g,t}$ | `cold_start_cost` | Offline ≥ `cold_start_time` hours |

#### C8: Startup Type Selection

$$v_{g,t} = y^{\text{hot}}_{g,t} + y^{\text{warm}}_{g,t} + y^{\text{cold}}_{g,t}$$

Each startup is exactly one tier.

#### C9: Hot Start Window

$$y^{\text{hot}}_{g,t} \leq \sum_{q \in [\,t - T^{\text{hot}},\, t-1\,]} w_{g,q}$$

A hot start is only possible if there was a recent shutdown within the hot
time window.

#### C10: Warm Start Window

$$y^{\text{warm}}_{g,t} \leq \sum_{q \in [\,t - T^{\text{cold}},\, t - T^{\text{hot}} - 1\,]} w_{g,q}$$

A warm start requires a shutdown in the warm window (between hot and cold
thresholds).

Cold start is the residual: when neither hot nor warm is feasible, the
optimizer is forced to use $y^{\text{cold}}$ (the most expensive tier).

**Validation**: `cold_start_time` must be ≥ `hot_start_time`.  If this
condition is violated, a warning is logged and startup tiers are skipped,
falling back to the flat `startup_cost` on $v$.

**Initial conditions**: When `initial_status = 0` (offline at $t = 0$) and
`initial_hours` is provided, the tier selection at $t = 0$ uses the initial
offline duration to determine which tier window is active.

---

## 5. Piecewise Heat Rate Curves

Real thermal generators have nonlinear fuel consumption: the marginal
heat rate increases at higher output levels.  gtopt models this with
**piecewise linear segments**.

### Segment Definition

The generation range $[P_\min, P_\max]$ is decomposed into $K$ segments:

$$p_{g,t} = P_\min \cdot u_{g,t} + \sum_{k=1}^{K} \delta_{g,k,t}$$

where $\delta_{g,k,t}$ is the power dispatched in segment $k$:

$$0 \leq \delta_{g,k,t} \leq (\bar{P}_k - \bar{P}_{k-1}) \cdot u_{g,t}$$

Here $\bar{P}_0 = P_\min$ and `pmax_segments` $= [\bar{P}_1, \ldots, \bar{P}_K]$
gives the cumulative breakpoints.

### Cost Calculation

The cost per segment is:

$$c_k = \text{fuel\_cost} \times \text{heat\_rate\_segments}[k] \quad [\$/\text{MWh}]$$

When `fuel_emission_factor` and `emission_cost` are both set, emission
costs are added per segment:

$$c_k^{\text{total}} = c_k + \text{emission\_cost} \times \text{fuel\_emission\_factor} \times \text{heat\_rate\_segments}[k]$$

### Linking Constraint

$$p_{g,t} - P_\min \cdot u_{g,t} - \sum_{k=1}^{K} \delta_{g,k,t} = 0$$

### Segment Bound (Status-Dependent)

$$\delta_{g,k,t} \leq (\bar{P}_k - \bar{P}_{k-1}) \cdot u_{g,t}$$

When the unit is offline ($u = 0$), all segment variables are forced to zero.

### JSON Example

```json
{
  "uid": 1,
  "name": "coal1_uc",
  "generator": 1,
  "initial_status": 1,
  "relax": true,
  "must_run": true,
  "pmax_segments": [200, 400, 600],
  "heat_rate_segments": [8.5, 9.2, 11.0],
  "fuel_cost": 3.5,
  "fuel_emission_factor": 0.09
}
```

This defines three segments for a 600 MW coal unit with $P_\min$ from the
generator:
- Segment 1: $[P_\min, 200]$ MW at 8.5 GJ/MWh → $29.75/MWh
- Segment 2: $[200, 400]$ MW at 9.2 GJ/MWh → $32.20/MWh
- Segment 3: $[400, 600]$ MW at 11.0 GJ/MWh → $38.50/MWh

---

## 6. Emission Cost and Cap

gtopt supports two complementary mechanisms for internalizing carbon costs:

### 6.1 Emission Cost (Carbon Price)

Set `model_options.emission_cost` ($/tCO₂) to add a carbon price to dispatch
costs.  For committed generators with `emission_factor` (tCO₂/MWh):

$$c_{g,t}^{\text{effective}} = c_{g,t}^{\text{gen}} + \tau_s \times e_g$$

where $\tau_s$ is the emission cost (can be stage-indexed) and $e_g$ is the
generator's emission factor.

For piecewise segments with `fuel_emission_factor` (tCO₂/GJ):

$$c_{g,k,t}^{\text{effective}} = c_{g,k} + \tau_s \times \text{fuel\_ef} \times h_k$$

### 6.2 Emission Cap

Set `model_options.emission_cap` (tCO₂/year) to add a hard constraint per stage:

$$\sum_g \sum_b \left( e_g \cdot p_{g,b} \cdot \Delta_b \right) \leq E^{\text{cap}}_s$$

The dual variable of this constraint gives the **endogenous carbon price** —
the marginal cost of the CO₂ budget at optimality.

### 6.3 Combined Use

Both can be used simultaneously:
- `emission_cost` acts as a **floor** carbon price that all emitters pay
- `emission_cap` imposes an absolute limit; when binding, the effective
  carbon price is `emission_cost` + the cap's dual variable

### JSON Example

```json
{
  "options": {
    "model_options": {
      "emission_cost": 30.0,
      "emission_cap": 500000.0
    }
  }
}
```

---

## 7. Reserve Integration

When a generator has both a `Commitment` entry and a `ReserveProvision`
entry, the reserve headroom constraints become conditional on the
commitment status:

### Without UC (standard)

$$p_{g,t} + r^{\text{up}}_{g,t} \leq P_\max$$

### With UC

$$p_{g,t} + r^{\text{up}}_{g,t} \leq P_\max \cdot u_{g,t}$$

Similarly for down-reserve:

$$p_{g,t} - r^{\text{dn}}_{g,t} \geq P_\min \cdot u_{g,t}$$

This ensures that an offline unit ($u = 0$) cannot provide spinning reserve.
The modification is done **in-place** on existing reserve provision rows,
not by adding new constraints.

---

## 8. Commitment Periods

By default, binary variables ($u$, $v$, $w$) are created **per block** — one
set of binaries for every time step.  For problems with fine time resolution
(e.g., 15-minute blocks in a 24-hour horizon = 96 blocks), this creates many
integer variables.

The `commitment_period` field allows grouping consecutive blocks into
**commitment periods** with one binary per period, while keeping generation
variables at the original fine resolution.

### How it Works

When `commitment_period = 2.0` (hours) with 15-minute blocks:
- Each 2-hour window groups 8 blocks into one commitment period
- One $u$, $v$, $w$ variable per 2-hour period (12 periods × 3 = 36 binaries
  instead of 96 × 3 = 288)
- Generation $p_{g,t}$ remains at 15-minute resolution (96 variables)
- All blocks in a period share the same $u$ value

### Cost Scaling

The no-load cost is scaled by the period duration:

$$c^{\text{noload}} = c_{\text{noload}} \times \frac{\text{period\_duration}}{\text{rep\_block\_duration}} \times \text{ecost\_factor}$$

### JSON Example

```json
{
  "uid": 1,
  "name": "coal1_uc",
  "generator": 1,
  "commitment_period": 2.0,
  "startup_cost": 50000,
  "noload_cost": 500,
  "initial_status": 1
}
```

---

## 9. Relaxation Control

### Per-Element Relaxation

Set `relax: true` on a `Commitment` entry to make its $u$, $v$, $w$ variables
continuous in $[0, 1]$ instead of binary $\{0, 1\}$:

```json
{
  "uid": 1,
  "generator": 1,
  "relax": true
}
```

This is useful for:
- **Faster solves**: LP relaxation avoids the branch-and-bound search
- **Lower bounds**: the LP relaxation objective is a lower bound on the
  MIP objective, useful for gap analysis
- **Approximate dispatch**: fractional commitment often gives good dispatch
  approximations

### Per-Phase Relaxation

Set `model_options.relaxed_phases` to relax binaries for specific planning
phases without modifying individual commitment entries:

```json
{
  "options": {
    "model_options": {
      "relaxed_phases": "all"
    }
  }
}
```

The `relaxed_phases` field accepts a phase range expression:

| Expression | Meaning |
|------------|---------|
| `"none"` or `""` | No relaxation (default) — all UC binaries are integer |
| `"all"` | All phases relaxed — pure LP, no integers |
| `"3"` | Only phase 3 is relaxed |
| `"1,3,5"` | Phases 1, 3, and 5 are relaxed |
| `"2:5"` | Phases 2 through 5 (inclusive) are relaxed |
| `"3:"` | Phase 3 and all subsequent phases |
| `":5"` | Phases 0 through 5 |
| `"0,3:5,8:"` | Phases 0, 3–5, and 8 onward |

The CLI equivalent:

```bash
gtopt my_case.json --set model_options.relaxed_phases="all"
```

### Must-Run

Set `must_run: true` to force $u = 1$ at all times (lower bound = 1):

```json
{
  "uid": 1,
  "generator": 1,
  "must_run": true
}
```

The generator must remain online regardless of cost.  This is useful for
baseload units, system stability requirements, or contractual obligations.

---

## 10. JSON Input Reference

### 10.1 Commitment Object

The `commitment_array` lives inside the `system` section. Each entry links
to exactly one generator via a foreign key.

| Field | Type | Required | Default | Unit | Description |
|-------|------|----------|---------|------|-------------|
| `uid` | integer | yes | — | — | Unique identifier |
| `name` | string | no | `""` | — | Human-readable name |
| `active` | boolean | no | `true` | — | Whether this commitment is active |
| `generator` | integer/string | **yes** | — | — | Foreign key to Generator (uid or name) |
| `startup_cost` | real / array | no | 0 | $/start | Cost per startup event (stage-schedulable) |
| `shutdown_cost` | real / array | no | 0 | $/stop | Cost per shutdown event (stage-schedulable) |
| `noload_cost` | real | no | 0 | $/hr | Fixed hourly cost when committed |
| `min_up_time` | real | no | — | hours | Minimum time online after startup |
| `min_down_time` | real | no | — | hours | Minimum time offline after shutdown |
| `ramp_up` | real | no | — | MW/hr | Maximum ramp-up rate while online |
| `ramp_down` | real | no | — | MW/hr | Maximum ramp-down rate while online |
| `startup_ramp` | real | no | $P_\max$ | MW | Maximum output in startup block |
| `shutdown_ramp` | real | no | $P_\max$ | MW | Maximum output in shutdown block |
| `initial_status` | real | no | 0 | — | Initial state: 1.0 = online, 0.0 = offline |
| `initial_hours` | real | no | — | hours | Hours in current state at $t = 0$ |
| `relax` | boolean | no | `false` | — | LP relaxation: $u, v, w \in [0, 1]$ |
| `must_run` | boolean | no | `false` | — | Force $u = 1$ at all times |
| `commitment_period` | real | no | — | hours | Coarser binary variable resolution |
| `pmax_segments` | array\<real\> | no | `[]` | MW | Cumulative power breakpoints |
| `heat_rate_segments` | array\<real\> | no | `[]` | GJ/MWh | Heat rate per segment |
| `fuel_cost` | real / array | no | — | $/GJ | Fuel cost (stage-schedulable) |
| `fuel_emission_factor` | real / array | no | — | tCO₂/GJ | Fuel emission factor (stage-schedulable) |
| `hot_start_cost` | real | no | — | $/start | Startup cost for hot start |
| `warm_start_cost` | real | no | — | $/start | Startup cost for warm start |
| `cold_start_cost` | real | no | — | $/start | Startup cost for cold start |
| `hot_start_time` | real | no | — | hours | Maximum offline hours for hot start |
| `cold_start_time` | real | no | — | hours | Minimum offline hours for cold start |

**Notes**:
- All fields except `generator` are optional.  Omitting a field disables the
  corresponding constraint (e.g., no `min_up_time` → no C6 constraint).
- `startup_cost`, `shutdown_cost`, `fuel_cost`, and `fuel_emission_factor`
  accept either a scalar (applies to all stages) or an array (per-stage values).
- Startup tiers require **all five** fields (`hot_start_cost`, `warm_start_cost`,
  `cold_start_cost`, `hot_start_time`, `cold_start_time`) to be present.
  Partial specification is silently ignored.

### 10.2 Stage — Chronological Flag

```json
{
  "uid": 0,
  "first_block": 0,
  "count_block": 24,
  "chronological": true
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `chronological` | boolean | `false` | Blocks are consecutive time periods |

When `chronological = true`, blocks in this stage represent sequential hours
(or sub-hours) and UC constraints are applied.  When `false` (default),
blocks may represent duration-weighted representative periods (load-duration
curves) where temporal ordering is meaningless — UC is skipped silently.

### 10.3 Generator — Emission Factor

```json
{
  "uid": 1,
  "name": "coal1",
  "bus": 1,
  "gcost": 25,
  "capacity": 600,
  "emission_factor": 0.95
}
```

| Field | Type | Default | Unit | Description |
|-------|------|---------|------|-------------|
| `emission_factor` | real / array | — | tCO₂/MWh | CO₂ emission rate (stage-schedulable) |

Used with `emission_cost` to add a carbon price adder, and with `emission_cap`
to constrain total emissions.

### 10.4 Model Options — Emission and Relaxation

```json
{
  "options": {
    "model_options": {
      "emission_cost": 30.0,
      "emission_cap": 500000.0,
      "relaxed_phases": "none"
    }
  }
}
```

| Field | Type | Default | Unit | Description |
|-------|------|---------|------|-------------|
| `emission_cost` | real / array | — | $/tCO₂ | System-wide carbon price |
| `emission_cap` | real / array | — | tCO₂ | Annual CO₂ cap per stage |
| `relaxed_phases` | string | `"none"` | — | Phase range expression for LP relaxation |

---

## 11. Output Files

When unit commitment is active, the following output files are produced in
the `output/Commitment/` directory:

| File | Contents |
|------|----------|
| `status_sol.{csv,parquet}` | Commitment status $u$ solution (1 = online) |
| `status_cost.{csv,parquet}` | No-load cost contribution per block |
| `startup_sol.{csv,parquet}` | Startup event $v$ solution (1 = started) |
| `startup_cost.{csv,parquet}` | Startup cost per block |
| `shutdown_sol.{csv,parquet}` | Shutdown event $w$ solution (1 = shut down) |
| `shutdown_cost.{csv,parquet}` | Shutdown cost per block |
| `logic_dual.{csv,parquet}` | Dual variable of C1 logic constraint |
| `gen_upper_dual.{csv,parquet}` | Dual of upper generation bound constraint |
| `gen_lower_dual.{csv,parquet}` | Dual of lower generation bound constraint |
| `exclusion_dual.{csv,parquet}` | Dual of C3 exclusion constraint |
| `ramp_up_dual.{csv,parquet}` | Dual of C4 ramp-up constraint |
| `ramp_down_dual.{csv,parquet}` | Dual of C5 ramp-down constraint |
| `segment_sol.{csv,parquet}` | Piecewise segment dispatch (when segments active) |
| `segment_cost.{csv,parquet}` | Per-segment fuel cost |
| `segment_dual.{csv,parquet}` | Dual of segment linking constraint |
| `min_up_time_dual.{csv,parquet}` | Dual of C6 min-up-time constraint |
| `min_down_time_dual.{csv,parquet}` | Dual of C7 min-down-time constraint |
| `hot_start_sol.{csv,parquet}` | Hot start tier selection |
| `hot_start_cost.{csv,parquet}` | Hot start cost |
| `warm_start_sol.{csv,parquet}` | Warm start tier selection |
| `warm_start_cost.{csv,parquet}` | Warm start cost |
| `cold_start_sol.{csv,parquet}` | Cold start tier selection |
| `cold_start_cost.{csv,parquet}` | Cold start cost |
| `startup_type_dual.{csv,parquet}` | Dual of C8 type selection |
| `hot_start_dual.{csv,parquet}` | Dual of C9 hot window constraint |
| `warm_start_dual.{csv,parquet}` | Dual of C10 warm window constraint |

All files follow the standard gtopt output format:
`scenario, stage, block, uid:<commitment_uid>`.

---

## 12. Worked Examples

### 12.1 Example 1: Basic UC with Two Generators

**Scenario**: A single-bus system with a committed coal unit and an uncommitted
gas peaker serving 100 MW of demand over 4 hourly blocks.

```json
{
  "system": {
    "bus_array": [{"uid": 1, "name": "b1"}],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": 1, "capacity": 100}
    ],
    "generator_array": [
      {
        "uid": 1, "name": "coal",
        "bus": 1, "pmin": 20, "pmax": 100,
        "gcost": 10, "capacity": 100
      },
      {
        "uid": 2, "name": "gas",
        "bus": 1, "pmin": 0, "pmax": 80,
        "gcost": 30, "capacity": 80
      }
    ],
    "commitment_array": [
      {
        "uid": 1, "name": "coal_uc",
        "generator": 1,
        "startup_cost": 100,
        "noload_cost": 5,
        "initial_status": 1,
        "relax": true
      }
    ]
  },
  "simulation": {
    "block_array": [
      {"uid": 0, "duration": 1},
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 1},
      {"uid": 3, "duration": 1}
    ],
    "stage_array": [
      {"uid": 0, "first_block": 0, "count_block": 4, "chronological": true}
    ],
    "scenario_array": [{"uid": 0}]
  }
}
```

**Expected result**: Coal dispatches 100 MW in all blocks (cheapest at
$10/MWh + $5/hr noload).  Status $u = 1$ throughout.  No startups or
shutdowns ($v = w = 0$).

### 12.2 Example 2: Piecewise Heat Rate

**Scenario**: A committed generator with a 2-segment heat rate curve competing
against a flat-cost generator.

```json
{
  "system": {
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": 1, "pmin": 0, "pmax": 100,
       "gcost": 0, "capacity": 100},
      {"uid": 2, "name": "g2", "bus": 1, "pmin": 0, "pmax": 100,
       "gcost": 50, "capacity": 100}
    ],
    "commitment_array": [
      {
        "uid": 1, "name": "g1_uc", "generator": 1,
        "initial_status": 1, "relax": true, "must_run": true,
        "pmax_segments": [50, 100],
        "heat_rate_segments": [8.0, 12.0],
        "fuel_cost": 5.0
      }
    ]
  }
}
```

**Effective costs**:
- g1 segment 1: $5 \times 8.0 = 40$ $/MWh (0–50 MW)
- g1 segment 2: $5 \times 12.0 = 60$ $/MWh (50–100 MW)
- g2: $50$ $/MWh

With demand = 80 MW: g1 dispatches 50 MW (segment 1 at $40), g2 dispatches
30 MW (at $50 < segment 2 at $60).

### 12.3 Example 3: Startup Cost Tiers

**Scenario**: A thermal unit offline for 3 hours with hot/warm/cold startup
tiers.

```json
{
  "commitment_array": [
    {
      "uid": 1, "name": "g1_uc", "generator": 1,
      "initial_status": 0,
      "initial_hours": 3.0,
      "relax": true,
      "hot_start_cost": 100,
      "warm_start_cost": 300,
      "cold_start_cost": 500,
      "hot_start_time": 2.0,
      "cold_start_time": 6.0
    }
  ]
}
```

Since the unit has been offline for 3 hours:
- Hot start (offline < 2h): **not available** (3h > 2h)
- Warm start (2h ≤ offline < 6h): **available** (3h is in range)
- Cold start (offline ≥ 6h): **available** but more expensive

The optimizer selects warm start at $300 (cheapest feasible tier).

### 12.4 Example 4: Emission Cost Shifting Dispatch

**Scenario**: Two generators — g1 is cheap but dirty, g2 is expensive but
clean.  Adding a carbon price reverses the merit order.

```json
{
  "system": {
    "generator_array": [
      {"uid": 1, "name": "coal", "bus": 1, "gcost": 10, "capacity": 100,
       "emission_factor": 1.0},
      {"uid": 2, "name": "gas", "bus": 1, "gcost": 30, "capacity": 100}
    ],
    "commitment_array": [
      {"uid": 1, "generator": 1, "initial_status": 1, "relax": true}
    ]
  },
  "options": {
    "model_options": {
      "emission_cost": 25.0
    }
  }
}
```

- Without carbon price: g1 effective cost = $10/MWh → dispatches first
- With $25/tCO₂: g1 effective cost = $10 + $25 × 1.0 = $35/MWh > g2 at $30
  → g2 dispatches first

### 12.5 Example 5: Emission Cap

**Scenario**: An emission cap limits dirty generation, forcing a cost-optimal
split between generators.

```json
{
  "options": {
    "model_options": {
      "emission_cap": 30.0
    }
  }
}
```

With g1 (`emission_factor = 1.0 tCO₂/MWh`) and demand = 80 MW over 1 hour:
- Cap = 30 tCO₂ → g1 can produce at most 30 MWh (30 / 1.0 / 1h)
- g2 (clean) must produce the remaining 50 MW

---

## 13. Integration with Planning Framework

### Architecture

```
Planning
  └─ System
       ├─ generator_array      ← Generator definitions
       ├─ commitment_array     ← UC parameters (1:1 with generators)
       ├─ reserve_zone_array   ← Reserve requirements
       └─ reserve_provision_array ← Generator-reserve links
  └─ Simulation
       └─ stage_array          ← chronological flag per stage
```

### LP Assembly Order

```
1. GeneratorLP     → creates p_{g,t} generation variables
2. CommitmentLP    → creates u/v/w variables, C1–C10 constraints
3. ReserveProvisionLP → creates reserve variables, headroom rows
4. CommitmentLP    → modifies reserve headroom rows (reserve-UC integration)
```

### SystemLP Integration

`CommitmentLP` is registered in `SystemLP` as a `Collection<CommitmentLP>`.
It is instantiated after `GeneratorLP` (its dependency) and before the LP
is solved.  The call sequence for each (scenario, stage) pair:

1. `CommitmentLP::add_to_lp()` — adds variables and constraints
2. Accesses `GeneratorLP::generation_cols_at()` to reference the generation
   variable for C2 bounds
3. Iterates over `ReserveProvisionLP` entries to modify headroom rows

### Chronological Gating

The `add_to_lp()` method checks `stage.is_chronological()`:
- `true`: UC constraints are applied
- `false`: method returns early, generator dispatches normally without UC

This allows mixed-mode planning: expansion planning stages use
non-chronological blocks (load-duration curves), while operational stages
use chronological blocks with full UC.

---

## 14. Known Limitations

### Current Implementation

1. **Initial min-up obligation**: When a unit is initially online
   (`initial_status = 1`) with `initial_hours < min_up_time`, the current
   formulation does **not** prevent an early shutdown.  The min-up constraint
   only fires on startups ($v_t = 1$) within the horizon, not on pre-horizon
   obligations.  *Workaround*: set `must_run = true` for the initial period
   or extend the horizon.

2. **Emission cap uses flat emission factor**: When piecewise segments are
   active with `fuel_emission_factor`, the emission cap constraint still uses
   the flat `generator.emission_factor` on the total generation variable $p$,
   rather than per-segment emission rates.  This is conservative but
   approximate.

3. **Cross-stage state persistence**: Commitment state ($u$, $v$, $w$) does
   not persist across stages.  Each stage starts fresh with `initial_status`.
   For multi-stage problems, the last-period state should be passed as initial
   conditions for the next stage (not yet automated).

4. **No startup/shutdown profiles**: The current implementation models startup
   and shutdown as single-block events.  Multi-period startup trajectories
   (e.g., a 3-hour ramp from cold start) are not yet supported.

### Planned Improvements

- **Phase 4**: Startup/shutdown profiles (multi-block ramp trajectory)
- **Phase 5**: CCGT configuration tracking (multi-shaft combined cycle)
- **Phase 6**: Frequency/inertia constraints (RoCoF, nadir, system strength)
- **Phase 7**: Storage commitment (3-state battery, pumped hydro modes)

---

## 15. Comparison with Other Tools

| Feature | gtopt | PyPSA | GenX | PLEXOS |
|---------|-------|-------|------|--------|
| **UC formulation** | 3-bin tight (Morales-España) | 1-bin UC (basic) | 1-bin UC | 3-bin tight |
| **Startup tiers** | Hot/warm/cold ✓ | No | No | Hot/warm/cold ✓ |
| **Piecewise heat rate** | ✓ Per-segment | ✓ Marginal cost | ✓ Piecewise | ✓ Piecewise |
| **Min up/down time** | Aggregated ✓ | Basic ✓ | Linked ✓ | Multiple forms ✓ |
| **Ramp constraints** | Tight (state-dependent) ✓ | Basic ✓ | Basic ✓ | Tight ✓ |
| **Emission cost** | ✓ Per-generator + per-segment | ✓ Global | ✓ Per-zone | ✓ Detailed |
| **Emission cap** | ✓ With endogenous pricing | ✓ | ✓ | ✓ |
| **Reserve-UC** | ✓ Conditional headroom | ✓ | ✓ | ✓ |
| **Commitment periods** | ✓ Variable resolution | No | No | ✓ |
| **Phase relaxation** | ✓ Per-phase | No | No | ✓ |
| **Language** | C++26 | Python | Julia | C++/C# |
| **Open source** | ✓ BSD-3 | ✓ MIT | ✓ GPL-2 | Commercial |

### Key Differentiators

- **Tight formulation**: The 3-bin model produces the tightest LP relaxation
  among compact UC formulations, reducing solve times for MIP
- **Flexible time resolution**: Commitment periods decouple binary and
  generation variable resolution
- **Integrated with expansion planning**: UC and capacity expansion in the
  same optimization framework
- **High-performance C++**: LP assembly uses `flat_map` and sparse matrices
  for fast construction of large-scale problems

---

## 16. Academic References

<a id="ref1"></a>
**[1]** G. Morales-España, J. M. Latorre, and A. Ramos, "Tight and Compact
MILP Formulation for the Thermal Unit Commitment Problem," *IEEE Transactions
on Power Systems*, vol. 28, no. 4, pp. 4897–4908, Nov. 2013.
[DOI: 10.1109/TPWRS.2013.2251373](https://doi.org/10.1109/TPWRS.2013.2251373)

> The foundational paper for the three-bin (u, v, w) formulation used in gtopt.
> Demonstrates that this compact formulation produces the tightest LP relaxation.

<a id="ref2"></a>
**[2]** M. Carrión and J. M. Arroyo, "A Computationally Efficient Mixed-Integer
Linear Formulation for the Thermal Unit Commitment Problem," *IEEE Transactions
on Power Systems*, vol. 21, no. 3, pp. 1371–1378, Aug. 2006.
[DOI: 10.1109/TPWRS.2006.876672](https://doi.org/10.1109/TPWRS.2006.876672)

> The 1-bin UC formulation that preceded the 3-bin model.

<a id="ref3"></a>
**[3]** B. Knueven, J. Ostrowski, and J.-P. Watson, "On Mixed-Integer
Programming Formulations for the Unit Commitment Problem," *INFORMS Journal
on Computing*, vol. 32, no. 4, pp. 857–876, 2020.
[DOI: 10.1287/ijoc.2019.0944](https://doi.org/10.1287/ijoc.2019.0944)

> Comprehensive comparison of UC formulations; establishes the theoretical
> tightness hierarchy.

<a id="ref4"></a>
**[4]** D. A. Tejada-Arango, S. Lumbreras, P. Sánchez-Martín, and A. Ramos,
"Which Unit-Commitment Formulation is Best? A Comparison Framework," *IEEE
Transactions on Power Systems*, vol. 35, no. 4, pp. 2926–2936, Jul. 2020.
[DOI: 10.1109/TPWRS.2019.2962024](https://doi.org/10.1109/TPWRS.2019.2962024)

> Practical comparison of UC formulations on real-world instances.

<a id="ref5"></a>
**[5]** G. Morales-España, C. Gentile, and A. Ramos, "Tight MIP Formulations
of the Power-Based Unit Commitment Problem," *OR Spectrum*, vol. 37, no. 4,
pp. 929–950, Oct. 2015.
[DOI: 10.1007/s00291-015-0400-4](https://doi.org/10.1007/s00291-015-0400-4)

> Extension of the 3-bin model with power-based tightening.

---

## 17. Roadmap

### Completed ✓

| Phase | Feature | Status |
|-------|---------|--------|
| 1 | Three-bin core (C1–C3) + noload cost + startup/shutdown cost | ✓ |
| 1 | Emission cost and cap framework | ✓ |
| 2 | Ramp constraints (C4–C5) with startup/shutdown ramps | ✓ |
| 2 | Reserve-UC integration | ✓ |
| 3 | Minimum up/down time (C6–C7) | ✓ |
| 3 | Hot/warm/cold startup tiers (C8–C10) | ✓ |
| 3 | Piecewise heat rate curves | ✓ |
| 3 | Commitment periods (variable binary resolution) | ✓ |
| 3 | Phase-based relaxation control | ✓ |

### Planned

| Phase | Feature | Priority |
|-------|---------|----------|
| 4 | Startup/shutdown profiles (multi-block ramp trajectory) | High |
| 5 | CCGT configuration tracking (multi-shaft combined cycle) | Medium |
| 6 | Frequency security (inertia, RoCoF, system strength) | Medium |
| 7 | Storage commitment (3-state battery, pumped hydro modes) | Medium |
| 7 | Integer clustering for large-scale UC | Low |
| 7 | Stochastic UC (scenario-dependent commitment) | Low |

---

*Last updated: April 2025*
