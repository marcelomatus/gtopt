# Investment Master — OptGen-style Expansion Planning in gtopt

## 1. Introduction

The **investment master** (`solve_investment_master`,
`include/gtopt/sddp_investment_master.hpp`) separates **here-and-now
integer investment** from **multistage continuous operation**, in the
style of PSR's OptGen paired with SDDP.  Integer build decisions live
ONLY in a small MIP master; the operational recourse is a convex LP
oracle (the gtopt SDDP method run on a copy of the planning with
expansion integrality relaxed).  The two exchange **capacity-space
Benders cuts** until the master lower bound meets the operational
upper bound.

This is the exact reason PSR runs an LP operational model under OptGen
rather than full SDDiP: operations are convex in capacity (capacities
enter the operational LP as bounds / RHS), so the expected operational
cost `Φ(K)` is convex piecewise-linear and each extracted cut is a
valid support — the master's branch-and-bound is then exact.

## 2. The two-level loop

```
  ┌────────────────────────────────────────────────┐
  │  Master MIP:  min Σ_s θ_s                        │
  │    n_g ∈ [0, expmod]  (integer build count)      │
  │    K_g = base + expcap · n_g   (capacity state)  │
  │    θ_s ≥ V_s + Σ_j g_j·(K_j − K̂_j)   (cuts)      │
  └───────────────┬─────────────────────▲───────────┘
      propose n*  │                     │  cut (V_s, g_j, K̂_j)
                  ▼                     │
  ┌────────────────────────────────────┴───────────┐
  │  Operational oracle: SDDP on a copy of Planning  │
  │    expansion integrality RELAXED (LP oracle)     │
  │    expmod PINNED to n*  (prices K = expcap·n*)   │
  │    → V_s = phase-0 scene cost (OPEX + CAPEX + α)  │
  │    → g_j = ∂V_s/∂K_j  (capacity subgradient)      │
  └──────────────────────────────────────────────────┘
```

- **LB** = master objective `Σ_s θ_s` (a valid lower bound once each
  scene has a cut).
- **UB** = the operational cost at a PINNED master proposal (iter ≥ 1);
  the iteration-0 seeding run uses full continuous-relaxed headroom, so
  its cost is a relaxation bound, not a valid integer UB.
- **Stop** on `UB − LB ≤ tol` with `builds == best_builds`, or the
  iteration cap.

The per-scene θ carries the FULL discounted scene cost (OPEX + the
CAPEX folded into the `capacost` state), so the master needs NO
separate CAPEX term — the annualised investment charge is already
inside every capacity cut.

## 3. The capacity subgradient (and the convergence bug it fixed)

The load-bearing quantity is `g_j = ∂V_s/∂K_j`, the slope of the
per-scene value function w.r.t. installed capacity.  Getting its
**source** right is what makes the loop converge.

### 3.1 What does NOT work

- **`capainst` column reduced cost** (`get_col_cost()`): identically
  **0**.  `capainst` is a BASIC column, structurally pinned by its own
  defining equality `capainst = base + expcap·expmod + prev_capainst`
  (`source/capacity_object_lp.cpp`), so its reduced cost is 0 by
  simplex stationarity.  Reading `g_j` here makes every cut FLAT
  (`θ_s ≥ V_s`, no K-dependence): the master never learns capacity has
  value, builds 0, and **falsely converges** at LB = UB far above the
  true optimum.  This was the original defect.
- **`expmod` column reduced cost**: 0 at the value-function kinks
  (a module beyond served demand has no value) and degenerate at the
  bound-pinned point.
- **`capainst` / `capacost` equality-row duals**: masked to a constant
  by row-max equilibration of the ±`expcap` coefficient — capacity-
  independent, not the true slope.
- **cross-phase `capainst_prev` dependent-column rc**: 0 when each
  stage builds its own capacity via its own pinned `expmod` (no
  stage-to-stage capacity coupling to price).

### 3.2 What works: the `capacity` constraint dual

The marginal value of one more MW of INSTALLED capacity is the shadow
price of the dispatch ceiling `generation ≤ capainst`
(`source/generator_lp.cpp`, `CapacityName` row).  Its dual `π_cap ≥ 0`
is a valid subgradient of the convex value function at the trial point
(standard LP sensitivity).  Each per-block dual is already folded by
its `cost_factor = prob × discount × duration`, the SAME probability-
folded, `scale_objective`-unscaled space as `V_s = get_obj_value()`
(= `scene_lower_bounds`).  Therefore

```
  g_j = − Σ_{(phase, block)} π_cap
```

is the slope directly in the master's θ_s units — no extra scaling.
Summing across phases folds in the stage-≥1 cost-to-go (the master
pins the same build to every stage, so every stage's ceiling moves
with `K_j`).  The cut is assembled with the validated backward-pass
convention (`row[K_j] = −g_j`, `rhs = V_s − Σ g_j·K̂_j`), matching
`build_benders_cut_physical` in `source/benders_cut.cpp`.

See `SceneCapSubgradients` in `source/sddp_investment_master.cpp` for
the full derivation, and
[`sddip_integer_expansion_2026-07.md`](../analysis/investigations/sddp/sddip_integer_expansion_2026-07.md)
§7.4 for the campaign record.

## 4. Scope and caveats (v1)

1. The projection to capacity coordinates is exact only when the
   non-capacity phase-0 states (reservoir energy) are pinned across
   master iterations (they are — `eini` is data).  A pure-expansion
   fixture (no reservoir) makes the projection exact — the acceptance
   test (`test/source/test_sddp_investment_master.cpp`) uses one.
2. A single here-and-now build per candidate is the tested path: the
   master aggregates a candidate's capacity across the horizon into one
   `K_g` column and pins the same `n*` to every stage.  Per-stage
   staggered builds are the documented remainder.

## 5. Related

- [SDDP Method](sddp.md) — the operational oracle.
- [Monolithic Method](monolithic.md) — the convergence target
  (the extensive-form MIP the master reproduces).
