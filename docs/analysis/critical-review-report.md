# Critical Analysis of gtopt: Implementation Review

## Electric Grid Modeling, SDDP/Benders, and Comparison with PLP, pandapower, and Literature

**Date**: 2026-03-28
**Scope**: Full C++ codebase review with 19 parallel research agents

---

## 1. Executive Summary

gtopt is a well-architected C++26 GTEP solver that modernizes PLP's Fortran SDDP
framework while adding substantial new capabilities. The DC-OPF formulation is
mathematically sound and follows industry best practices. The SDDP implementation
is faithful to Pereira-Pinto (1991) with valuable extensions including the novel
cascade method.

**Critical findings (ordered by severity):**

| # | Finding | Severity | Status |
|---|---------|----------|--------|
| 1 | Probability double-counting in `expected` cut sharing mode | **High** | Bug (open) |
| 2 | Elastic penalty default too low for long-block problems | **High** | ✅ Fixed (now 1e6) |
| 3 | No dynamic reservoir volume scaling (vs PLP's `ScaleVol`) | **Medium-High** | Documented |
| 4 | Island detection is static-only (no per-stage awareness) | **Medium** | Gap (open) |
| 5 | No `ScalePhi` equivalent for SDDP alpha variable | **Medium** | Missing feature |
| 6 | Loss allocation fixed at 100% receiver (PLP default is 50/50) | **Low** | Design choice (documented) |
| 7 | Missing PLP's `DCMod` paired-charging for ESS | **Low** | Missing feature |

**Positive findings:**
- Benders cut construction via reduced costs is **mathematically proven correct**
- State variable propagation is **always correctly chained** in forward pass
- The cascade method is a **novel engineering contribution**
- gtopt supports both `reduced_cost` and `row_dual` cut coefficient modes

---

## 2. DC-OPF / Electric Grid Modeling

### 2.1 Formulation Choice: B-theta (Correct)

gtopt uses the B-theta (susceptance-angle) formulation with per-line Kirchhoff
constraints. This is the **correct choice for GTEP** — the PTDF alternative
requires recomputing the entire distribution factor matrix when candidate lines
are added/removed.

```
-theta_a + theta_b + tau * x * fp - tau * x * fn = -scale_theta * phi_rad
```

| Tool | Formulation | Line Vars | Suitable for GTEP? |
|------|------------|-----------|-------------------|
| **gtopt** | B-theta (per-line) | Split fp/fn | Yes |
| **PLP** | B-theta (per-line) | Split fp/fn per segment | Yes |
| **pandapower** | B-theta (matrix Bf*Va) | Single signed f | No (no expansion) |
| **MATPOWER** | B-theta (matrix Bf*Va) | Single signed f | No (no expansion) |

### 2.2 Split Flow Variables

gtopt's split-flow approach (two non-negative vars `fp >= 0`, `fn >= 0`) is
inherited from PLP and is **necessary** for:

- Directional loss modeling (sender loses more than receiver gets)
- Asymmetric line limits (`tmax_ab != tmax_ba`)
- Per-direction capacity constraints for expansion

When `lossfactor = 0` and no expansion, gtopt correctly collapses to a single
bidirectional variable (`line_lp.cpp:368-378`).

### 2.3 Reference Bus and Island Detection

gtopt has island detection implemented in `bus_island.cpp` using Union-Find.
It runs at LP-build time via `System::setup_reference_bus()` and assigns
`reference_theta = 0` to one bus per disconnected component.

**Gap: Static-only island detection.** The detection runs once before LP
construction using the static `Line` data. It does NOT consider:

- **Per-stage line activation** (`Line::active` schedule) — a line inactive at
  stage 2 could split the network, leaving an island without a reference bus
- **Per-stage reactance availability** — `bus_island.cpp:122` checks
  `line.reactance.has_value()` at the top level, not per-stage
- **No runtime warning** when dynamic topology changes create islands

**Impact:** If a line becomes inactive at a particular stage, theta variables in
the resulting island are unconstrained. The LP remains feasible (the solver finds
arbitrary angle values) but **LMPs within the unanchored island become
unreliable**.

**Recommendation:** Run island detection per-stage, filtering lines by their
`active` status and reactance at each stage. Alternatively, add a runtime check
after all lines are processed for a stage, verifying each theta-connected
component has exactly one reference bus.

### 2.4 Susceptance and Unit Conventions

gtopt computes: `x = scale_theta * (X / V^2)` where `X` = reactance and
`V` = voltage (defaulting to 1.0).

**Potential confusion:** The `reactance` field is documented as "p.u."
(`line.hpp:81`), but the `voltage` field on Bus is in kV. If a user sets
`voltage = 220` (kV) and `reactance = 0.01` (p.u.), the computation
`220^2 / 0.01 = 4,840,000` gives an enormous susceptance.

pandapower handles this cleanly: users input physical parameters
(`x_ohm_per_km`, `length_km`, `vn_kv`) and the library converts to per-unit
internally.

**Recommendation:** Document clearly whether reactance should be in p.u. or
Ohms. Consider adding a validation check when `|B_eff| > threshold`.

### 2.5 Transmission Loss Models

gtopt supports two loss models, both superior to pandapower (which has no
DC-OPF losses):

| Model | Activation | Formula |
|-------|-----------|---------|
| **Linear** | `lossfactor > 0` | `P_loss = lossfactor * \|f\|` |
| **Quadratic** | `resistance > 0`, `loss_segments > 1` | `P_loss ~ R * f^2 / V^2` (piecewise) |

The piecewise-linear quadratic model uses the **incremental cost trick**:
segment k has loss coefficient `width * R * (2k-1) / V^2`. The LP naturally
fills lower-cost segments first, correctly approximating the concave envelope.

### 2.6 Loss Allocation: 100% Receiver vs PLP's 50/50

**gtopt** allocates 100% of losses to the receiving bus:
- Sender: coefficient = `-1` (full flow withdrawn)
- Receiver: coefficient = `+(1 - lossfactor)` (flow minus ALL losses)

**PLP** defaults to 50/50 split (configurable via `FPerdLin`):
- Sender: `-(1 + 0.5 * losscoeff)`
- Receiver: `+(1 - 0.5 * losscoeff)`

**Mathematical impact on LMPs** (for 5% loss, `alpha = 0.05`):

| Model | lambda_B / lambda_A | If lambda_A = 100 |
|-------|--------------------|--------------------|
| gtopt (100% receiver) | 1/(1-0.05) = 1.0526 | lambda_B = 105.26 |
| PLP default (50/50) | 1.025/0.975 = 1.0513 | lambda_B = 105.13 |

The difference is second-order in `alpha` (~0.13% for 5% losses), but compounds
in meshed networks with multiple lossy lines.

**Recommendation:** Consider adding a configurable loss allocation parameter
(PLP-style `FPerdLin`) for compatibility with PLP-derived cases.

### 2.7 Phase-Shifting Transformers (gtopt Extension)

gtopt extends standard DC-OPF with `tap_ratio` and `phase_shift_deg` — features
not present in PLP. The formulation is correct:
`B_eff = V^2 / (tau * X)`, with phase shift entering the Kirchhoff RHS.

### 2.8 Capacity Expansion

gtopt uses **continuous LP relaxation** (no Big-M):
```
fp <= capainst
capainst = prev_capainst + expcap * expmod
```

This avoids Big-M numerical problems and is compatible with SDDP (which requires
convex subproblems). Commercial tools like PLEXOS use the same approach within
SDDP subproblems.

---

## 3. SDDP & Benders Implementation

### 3.1 Algorithm Fidelity

gtopt's SDDP faithfully implements Pereira-Pinto (1991):

| Step | Literature | gtopt | PLP |
|------|-----------|-------|-----|
| Forward pass | Solve phases 1→T, chain states | `sddp_forward_pass.cpp` | `plp-faseprim.f` |
| Backward pass | Solve T→1, generate cuts | `sddp_method.cpp:624` | `plp-fasedual.f` |
| Cut construction | `alpha >= z + rc'(x - x_hat)` | `benders_cut.cpp:84` | `plp-agrespd.f` |
| Convergence | `(UB-LB)/\|UB\| < tol` | `sddp_iteration.cpp` | `plp-pdconvrg.f` |
| Apertures | Multi-scenario backward | `sddp_aperture_pass.cpp` | `FaseDuali` + `NApert` |

### 3.2 Cut Construction: Reduced Costs (Mathematically Correct)

gtopt uses **reduced costs** of fixed dependent columns (default mode). This was
proven equivalent to PLP's row-dual approach:

**Proof:** For a column fixed at `v_hat` via bounds (`lo = hi = v_hat`), the
reduced cost `rc = c_j - lambda^T * A_j`. From KKT conditions of the explicit
coupling constraint formulation: `lambda^T * A_j + pi = c_j`, therefore
**`rc = pi`** exactly.

gtopt also supports a `row_dual` mode (via `CutCoeffMode`) that matches PLP's
approach: keep dependent columns at physical bounds, add explicit coupling rows,
extract row duals. Both modes produce identical cuts at the same optimal basis.

The `reduced_cost` mode is slightly more efficient (no extra rows added to LP).

### 3.3 State Variable Propagation (Correctly Chained)

**Correction from initial analysis:** The `StateVariableLookupMode` enum
(`warm_start` vs `cross_phase`) does **NOT** control SDDP state chaining. It
only controls how nonlinear LP coefficient updates (seepage, production factor,
discharge limits) obtain the reservoir volume for their linearization point.

Trial values are **always correctly chained** from the previous phase's solved
primal in `sddp_forward_pass.cpp:97-108`, regardless of the lookup mode. Both
modes are theoretically valid for SDDP convergence. The `warm_start` default
introduces a one-iteration lag in nonlinear coefficient linearization (compared
to PLP's `cross_phase` behavior), which may slightly slow convergence for
problems with highly volume-sensitive seepage/production factors but does not
break correctness.

### 3.4 CRITICAL: Probability Double-Counting in `expected` Cut Sharing

**This is a bug.** The `expected` cut sharing mode double-counts probability
weights:

1. LP objectives already include probability via `block_ecost()`:
   `LP_coeff = cost * probability * discount * duration / scale_objective`

2. The Benders cut's `z*` and reduced costs inherit this probability weighting.

3. The `expected` sharing mode then applies probability weights *again*:
   `shared_cut = sum_s (p_s / P_total) * cut_s`

4. The effective weight on scene `s` becomes `p_s^2 / P_total` instead of
   `p_s / P_total`.

**Correct formula:** Since cuts already carry probability in their coefficients,
the shared expected-value cut should be:
```
shared_cut = (1 / P_total) * sum_s cut_s
```
This is what the `accumulate` mode does (when `P_total = 1`).

**The `accumulate` mode** sums all scene cuts, which is correct when each scene
produces exactly one cut and probabilities sum to 1.0. However, if scenes produce
multiple cuts per phase step, the sum overcounts.

**The `max` mode** broadcasts all cuts without probability manipulation —
individually valid but potentially loose for foreign scenes.

**The `none` mode** is always correct (no sharing).

**Recommendation:** Fix `expected` mode to divide each cut's coefficients and
RHS by the scene's probability before the weighted average. Or, more simply, use
arithmetic mean (uniform weights) since probability is already embedded.

### 3.5 CRITICAL: Elastic Penalty Default Too Low

**The default `elastic_penalty = 1000` is inadequate** for problems with long
block durations.

The elastic penalty is set directly as an LP objective coefficient (NOT passed
through the cost scaling chain). Normal costs are scaled by
`duration / scale_objective`, so for:

| Block Duration | demand_fail LP coeff | elastic_penalty LP coeff | Ratio |
|---------------|---------------------|-------------------------|-------|
| 1 hour | 1.0 | 1,000 | 1000x (OK) |
| 720 hours | 720 | 1,000 | 1.4x (marginal) |
| 8,760 hours | 8,760 | 1,000 | **0.11x (penalty dominated!)** |

For annual blocks, the LP solver would prefer violating state-variable coupling
constraints (cheap at 1,000/unit) over shedding load (expensive at 8,760/unit).

**Evidence:** All SDDP tests in `test_sddp_feasibility.hpp` explicitly set
`elastic_penalty = 1e6`, not the default 1,000. The `sddp_options.hpp` comment
says "default: 1e6" but the actual default in `planning_options_lp.hpp:473`
resolves to 1,000.

**Recommendation:** Change the default to 1e6 (matching tests), or make the
penalty scale-aware by multiplying by `max_duration / scale_objective`.

### 3.6 Elastic Filter Design

gtopt's elastic filter is more sophisticated than PLP's:

| Feature | gtopt | PLP |
|---------|-------|-----|
| Clone LP before relaxation | Yes | No (modifies LP directly) |
| Penalized slack variables | Yes | N/A |
| Multi-cut mode | Yes (per-slack bound cuts) | No |
| Iterative backpropagation | Yes (backward through phases) | Partial |
| Auto-escalation | Infeasibility counter → multi_cut | No |

### 3.7 Convergence Criteria

| Criterion | gtopt | PLP |
|-----------|-------|-----|
| Gap tolerance | Deterministic: `(UB-LB)/\|UB\|` | Statistical: confidence interval |
| Stationary gap | Optional secondary criterion | Gradient convergence |
| Max iterations | Configurable (default 100) | Configurable |
| Simulation pass | Always after training | `FLastPass = .TRUE.` |

PLP's statistical convergence is more appropriate for stochastic problems with
many scenarios. gtopt's deterministic gap is simpler but may not account for UB
variance.

---

## 4. Numerical Scaling

### 4.1 The Critical Gap: No Dynamic Volume Scaling

PLP uses **dynamic per-reservoir scaling**: `ScaleVol(i) = max(1, Vmax_i/1000)`.
This normalizes all volume LP variables to the ~[0, 1000] range regardless of
physical size.

gtopt uses a **static** `energy_scale` (default 1.0) per element, with no
automatic computation.

**Impact for a Chilean-scale system:**

| Reservoir | Vmax (dam3) | PLP ScaleVol | PLP LP bound | gtopt LP bound |
|-----------|------------|-------------|-------------|----------------|
| Lake Laja | 6,000,000 | 6,000 | 1,000 | 6,000,000 |
| Colbun | 1,500,000 | 1,500 | 1,000 | 1,500,000 |
| Rapel | 200,000 | 200 | 1,000 | 200,000 |
| Small pond | 100 | 1 | 100 | 100 |

**LP coefficient ratio:**
- PLP: ~2,500 (excellent)
- gtopt (default): ~1.7e9 (very poor — beyond 1e8 safe zone)

The `VariableScaleMap` mechanism exists in gtopt for manual per-element
configuration, but there is no automatic `max(1, emax/1000)` computation.

**Recommendation:** Add automatic dynamic volume scaling (compute
`energy_scale = max(1, emax/threshold)` from input data), or at minimum document
that `energy_scale` must be configured for large reservoirs.

### 4.2 No ScalePhi Equivalent

PLP independently scales the future cost variable `varphi` with `ScalePhi`
(default 1e7 in Scale Mode). gtopt's alpha variable is scaled only through
`scale_objective` (1000).

For a national-scale system with future cost ~1e9, the SDDP cut RHS in gtopt
would be `1e9 / 1000 = 1e6`, while generator cost coefficients are ~0.2.
**Ratio: 5e6.** PLP achieves `1e9 / 1e7 = 100` — much better.

### 4.3 Less Aggressive Default Scaling

| Parameter | PLP (Scale Mode) | gtopt (default) | Ratio |
|-----------|-----------------|-----------------|-------|
| Objective | 1e7 | 1e3 | 1e4 |
| Angles | 1e4 | 1e3 | 10 |
| Volumes | Dynamic (Vmax/1000) | 1.0 (static) | up to 6000x |
| Future cost | 1e7 | N/A (uses scale_obj) | 1e4 |

For IEEE benchmark cases (small systems), gtopt's defaults produce ratios in the
1e5-1e6 range — adequate. For large hydrothermal systems, the gap is significant.

### 4.4 Comparison Table

| Aspect | PLP | gtopt | Impact |
|--------|-----|-------|--------|
| Volume scaling | Dynamic per-reservoir | Static (default 1.0) | Poor conditioning for large reservoirs |
| Scale mode defaults | ScaleObj=1e7, ScalePhi=1e7, ScaleAng=1e4 | scale_obj=1e3, scale_theta=1e3 | 3-4 orders less aggressive |
| Flow-duration removal | FScaleQs=true → bdursc=1.0 | No equivalent | Small coefficients in short blocks |
| Future cost scaling | Independent ScalePhi | Only scale_objective | Gap of ~1e4 for national SDDP |
| Configurability | Environment variables | JSON VariableScaleMap | gtopt more flexible in principle |

---

## 5. Component Modeling Comparison

### 5.1 Thermal Generators

| Feature | gtopt | PLP | pandapower | PLEXOS |
|---------|-------|-----|------------|--------|
| Linear cost | Yes (`gcost`) | Yes (`CenCVar`) | Polynomial | Poly + PWL |
| Quadratic cost | No | No | Yes | Yes |
| Ramp rates | No | No | No | Yes |
| Min up/down time | No | No | No | Yes |
| Startup cost | No | No | No | Yes |
| Unit commitment | No (LP only) | No | No | Yes (MIP) |

gtopt's simple generator model is appropriate for long-term planning. PLEXOS
uses LP relaxation in its LT Plan SDDP mode (same simplification).

### 5.2 Hydro Cascade

| Feature | gtopt | PLP |
|---------|-------|-----|
| Network topology | Explicit junction/waterway graph | Implicit `CenGHid` arrays |
| Seepage | `ReservoirSeepage` (piecewise-linear) | `genpdfil.f` (piecewise) |
| Production factor | `ReservoirProductionFactor` | `RendParam`, `PmaxParam` |
| Discharge limits | `ReservoirDischargeLimit` | Hardcoded Ralco |
| Run-of-river | Turbine with `flow` mode | Series plants (`NCenSer`) |
| Overflow modeling | Simple `spillway_capacity` + drain | Explicit overflow vars (`genpdreb.f`) |
| Chilean conventions | Via user constraints | Hardcoded Laja/Maule/Ralco/GNL |

gtopt's junction/waterway graph is a significant improvement over PLP's implicit
topology, supporting arbitrary configurations without specialized code.

### 5.3 Battery / Energy Storage

| Feature | gtopt | PLP (old bat) | PLP (new ESS) |
|---------|-------|---------------|---------------|
| Unified model | Yes | No | No |
| Charge/discharge eff | `input_eff/output_eff` | `FPC`/`FPD` | `nc`/`nd` |
| Self-discharge | `annual_loss` | None | `mloss` (monthly) |
| Multiple charge sources | Via bus | `MaxIny` per battery | `CenCarga` |
| Daily cycling | `daily_cycle = true` (default) | `edur/24` scaling | Not explicit |
| State variable in SDDP | Optional | Not SDDP-coupled | Not SDDP-coupled |
| Capacity expansion | Yes | No | No |
| Paired-charging (DCMod) | No equivalent | No | Yes (3 modes) |

**Missing:** PLP's `DCMod` paired-charging mode (constraining charge proportional
to a specific plant) has no direct equivalent in gtopt.

### 5.4 Demand

gtopt's demand modeling is significantly more flexible than PLP's:
- Demand as explicit LP variable (PLP uses RHS constants)
- Per-demand failure cost (PLP has global only)
- Minimum energy constraints (`emin`)
- Demand expansion via `CapacityObjectLP`
- Distribution loss factor

---

## 6. Novel Contributions in gtopt

### 6.1 Cascade Method

The cascade method is a **novel engineering synthesis** of established techniques:

1. Hierarchical decomposition with progressive model refinement
   (Romero & Monticelli 1994)
2. SDDP/Nested Benders (Pereira & Pinto 1991) as per-level solver
3. Regularized/stabilized Benders — elastic target constraints
4. Warm-started Benders — name-based cut transfer across structurally different LPs
5. Cut forgetting — two-phase solve with inherited cut expiration

**Novel aspects:**
- Configurable multi-level SDDP with heterogeneous LP formulations per level
- Name-based cut and state transfer across structurally different LPs
- Explicit cut forgetting with two-phase solve mechanism
- Elastic target regularization for SDDP forward passes

No published algorithm combines all of these. The typical cascade progression is:
Level 0 (copper-plate) → Level 1 (transport) → Level 2 (Kirchhoff with losses).

### 6.2 Other Unique Features

- **Plugin solver architecture**: Runtime loading of LP backends via `dlopen`
- **User constraints**: AMPL-inspired expression syntax for arbitrary linear constraints
- **Phase-shifting transformers** in DC-OPF (not in PLP)
- **Reserve modeling**: Structured zones and provisions
- **Capacity expansion for all elements**: Unified `CapacityObjectLP` template
- **Dual cut coefficient modes**: `reduced_cost` (default) and `row_dual` (PLP-compatible)

---

## 7. Issues Summary

### 7.1 Bugs

| # | Issue | Severity | Status | Location |
|---|-------|----------|--------|----------|
| 1 | **Probability double-counting in `expected` cut sharing**: LP objectives include probability via `block_ecost()`, then `expected` mode applies probability weights again, giving weight `p_s^2/P_total` instead of `p_s/P_total` | **High** | Open | `sddp_cut_sharing.cpp:65-126` |
| 2 | **Elastic penalty default mismatch**: `sddp_options.hpp` comment said "default: 1e6" but the resolved default was 1000. Tests use 1e6. Default 1000 was dominated by demand_fail cost for blocks > 1000 hours | **High** | ✅ Fixed (`planning_options_lp.hpp:473` now 1e6) | `planning_options_lp.hpp:473` |

### 7.2 Missing Features (vs PLP)

| # | Feature | Severity | Status | Notes |
|---|---------|----------|--------|-------|
| 3 | **Dynamic volume scaling** (`ScaleVol`): PLP auto-scales reservoir volumes to ~[0,1000]; gtopt defaults to 1.0, creating 1e9 coefficient ratios for Lake Laja-scale reservoirs | **Medium-High** | Documented | Manual `energy_scale` required; see [Planning Guide §8.2](../planning-guide.md) |
| 4 | **Stage-aware island detection**: Current detection is static, doesn't handle per-stage line deactivation | **Medium** | Open | `bus_island.cpp` runs once at startup |
| 5 | **ScalePhi equivalent**: No independent scaling for SDDP alpha/future-cost variable | **Medium** | Open | Could be added to `SddpOptions` |
| 6 | **Configurable loss allocation**: PLP supports Emitter/Mixed/Receiver via `FPerdLin` | **Low** | Design choice (documented) | gtopt fixed at 100% receiver; documented in [Mathematical Formulation §5.5](formulation/mathematical-formulation.md) |
| 7 | **Paired-charging mode (DCMod)**: PLP ESS supports proportional/complementary charge constraints | **Low** | Open | Could be modeled via user constraints |
| 8 | **Statistical convergence**: PLP uses confidence interval test; gtopt uses deterministic gap | **Low** | Partially addressed | `convergence_mode: "statistical"` added; see `SddpOptions.convergence_confidence` |

### 7.3 Documentation Gaps

| # | Gap | Status |
|---|-----|--------|
| 9 | Reactance unit convention (p.u. vs Ohms) | ✅ Documented in [Mathematical Formulation §5.6](formulation/mathematical-formulation.md) |
| 10 | `energy_scale` importance for large reservoirs | ✅ Documented in [Mathematical Formulation §6.3](formulation/mathematical-formulation.md) and [Planning Guide §8.2](../planning-guide.md) |
| 11 | Loss allocation convention (100% receiver) | ✅ Documented in [Mathematical Formulation §5.5](formulation/mathematical-formulation.md) |

---

## 8. Comparison Summary Table

| Aspect | gtopt | PLP | pandapower | PLEXOS | PSR SDDP |
|--------|-------|-----|------------|--------|----------|
| **DC-OPF** | B-theta (per-line) | B-theta (per-line) | B-theta (matrix) | B-theta | Zonal/Nodal |
| **Losses** | Linear + piecewise quad | Piecewise quad, 50/50 | None (DC) | Piecewise | Iterative |
| **Phase shifters** | Yes | No | Yes | Yes | Yes |
| **SDDP** | Yes (modern) | Yes (classic) | No | Yes | Yes (reference) |
| **Cascade/multi-level** | Yes (novel) | No | No | LT/MT/ST hierarchy | No |
| **Expansion** | All elements, continuous | None | None | MIP | LP relax + MIP |
| **Unit commitment** | No | No | No | Yes (MIP) | No |
| **Hydro** | Junction/waterway graph | Implicit arrays | N/A | Detailed | Detailed |
| **Battery** | Unified `StorageLP` | Two separate models | Basic | Detailed | Basic |
| **Solver backends** | CLP/CBC/HiGHS/CPLEX | CLP/CPLEX (OSI) | PIPS/IPOPT/CPLEX | Proprietary | Proprietary |
| **Volume scaling** | Static (default 1.0) | Dynamic per-reservoir | N/A | Unknown | Unknown |
| **Language** | C++26 | Fortran 90 | Python | C#/.NET | Unknown |
| **Parallelism** | AdaptiveWorkPool | OpenMP | NumPy | Multi-threaded | Multi-threaded |

---

## 9. Recommendations (Priority Order)

1. **Fix `expected` cut sharing** *(open)* — divide each cut's coefficients by
   the scene's probability before the weighted average, or use uniform weights

2. **~~Fix elastic penalty default~~** *(✅ fixed)* — default is now 1e6
   in `planning_options_lp.hpp:473`

3. **Add dynamic volume scaling** *(workaround documented)* — compute
   `energy_scale = max(1, emax/1000)` automatically from input data for
   reservoirs, matching PLP's `ScaleVol`. Until automated, users should set
   this manually (see [Planning Guide §8.2](../planning-guide.md)).

4. **Add stage-aware island detection** *(open)* — run
   `detect_islands_and_fix_references` per stage, or add a runtime check
   during LP construction

5. **Add ScalePhi equivalent** *(open)* — independent scaling for the alpha
   variable and cut coefficients in SDDP

6. **Add configurable loss allocation** *(low priority)* — `loss_allocation`
   field on Line with options matching PLP's `E`/`M`/`R` modes

7. **~~Document~~ reactance units, energy_scale importance, and loss
   allocation** *(✅ documented)* — all three gaps have been addressed in the
   Mathematical Formulation and Planning Guide

---

## 10. References

### Academic Literature
- Pereira & Pinto (1991). Multi-stage stochastic optimization applied to energy
  planning. *Mathematical Programming* 52, 359-375.
- Romero & Monticelli (1994). Hierarchical decomposition approach for
  transmission expansion planning. *IEEE Trans. Power Systems* 9(1), 373-380.
- de Matos, Philpott & Finardi (2015). Improving the performance of SDDP.
  *Journal of Computational and Applied Mathematics* 290, 196-208.
- Zou, Ahmed & Sun (2019). Stochastic dual dynamic integer programming.
  *Mathematical Programming* 175, 461-502.
- Pecci & Jenkins (2024). Regularized Benders decomposition for capacity
  expansion. arXiv:2403.02559.

### Software References
- pandapower: Thurner et al. (2018). pandapower. arXiv:1709.06743.
- MATPOWER: Zimmerman et al. (2011). MATPOWER: Steady-state operations,
  planning, and analysis tools. *IEEE Trans. Power Systems* 26(1), 12-19.
- PSR SDDP: https://www.psr-inc.com/software/sddp/
- PLEXOS: https://www.energyexemplar.com/plexos
- SDDP.jl: https://odow.github.io/SDDP.jl/

### gtopt Source Files Referenced
- `source/benders_cut.cpp` — Cut construction, elastic filter
- `source/sddp_forward_pass.cpp` — Forward pass, state propagation
- `source/sddp_cut_sharing.cpp` — Cut sharing modes (bug location)
- `source/sddp_method.cpp` — Main SDDP loop, alpha variable
- `source/line_lp.cpp` — Kirchhoff constraints, loss models
- `source/bus_lp.cpp` — Bus balance, theta variables
- `source/bus_island.cpp` — Island detection (static)
- `source/cascade_method.cpp` — Cascade solver
- `source/reservoir_lp.cpp` — Reservoir LP assembly
- `include/gtopt/storage_lp.hpp` — Storage balance, state coupling
- `include/gtopt/planning_options_lp.hpp` — Default constants
