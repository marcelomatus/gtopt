# Critical Analysis of gtopt: Implementation Review (v3)

## Electric Grid Modeling, SDDP/Benders, and Comparison with PLP, pandapower, and Literature

**Date**: 2026-03-30 (v3, verified with 11+1 agents)
**Scope**: Full C++ codebase review with 11 parallel research agents + 1 verification
agent. Covers SDDP literature, PLP Fortran source (CEN65), pandapower/MATPOWER
DC-OPF, DC-OPF best practices, commercial tools (PLEXOS/PSR/PLP/SPPD), and
deep-dive into gtopt's SDDP/LP, network, components, objective/scaling, and data model.
All top findings from v2 independently verified against source code.

---

## 1. Executive Summary

gtopt is a well-architected C++26 GTEP solver that modernizes PLP's Fortran SDDP
framework while adding substantial new capabilities. The DC-OPF formulation is
mathematically sound and follows industry best practices. The SDDP implementation
is faithful to Pereira-Pinto (1991) with valuable extensions including the novel
cascade method.

### Overall Assessment

**Strengths:**
- Modern C++26 codebase with clean separation of concerns
- Mathematically correct B-theta DC-OPF with split-flow variables
- Faithful SDDP implementation with both `reduced_cost` and `row_dual` cut modes
- Novel cascade method combining hierarchical decomposition with SDDP
- Plugin solver architecture (CLP, CBC, HiGHS, CPLEX via `dlopen`)
- Generic `StorageLP` template unifying reservoir and battery modeling
- Flexible data model with `FieldSched` variant (scalar/array/file)
- Capacity expansion for all element types via `CapacityObjectLP`

**Areas for Improvement:**
- Numerical scaling defaults inadequate for large hydrothermal systems
- Several PLP features not yet ported (dynamic volume scaling, paired charging)
- Static island detection doesn't handle per-stage topology changes
- Probability handling in `expected` cut sharing mode needs verification

### Critical Findings (ordered by severity)

| # | Finding | Severity | Area | Status | Verified |
|---|---------|----------|------|--------|----------|
| 1 | Probability double-counting in `expected` cut sharing | **High** | SDDP | ✅ Fixed (`e2c24740`) | ✅ `sddp_cut_sharing.cpp:87` uses `accumulate_benders_cuts` |
| 2 | Dynamic reservoir volume scaling | ~~High~~ | Numerics | ✅ Already default | ✅ `reservoir.hpp:174` returns `auto_scale` when unset |
| 3 | `scale_objective` default 1M vs PLP's 1e7 | **Medium** | Numerics | Design choice | ✅ `planning_options_lp.hpp:57` confirms 1M default |
| 4 | `scale_alpha` for future cost variable | ~~Medium~~ | SDDP | ✅ Exists (1M default) | ✅ `sddp_method.hpp` + `planning_options_lp.hpp:477` |
| 5 | Island detection static-only | ~~Medium~~ | DC-OPF | ✅ Fixed (`d4e945a0`) | ✅ `system_lp.cpp:140-259` runtime DSU per stage |
| 6 | Loss allocation 100% receiver | **Low** | DC-OPF | Design choice | ✅ `line_lp.cpp:117,149-150` confirmed |
| 7 | Capacity expansion is continuous | N/A | Design | Correct | ✅ `capacity_object_lp.cpp:102-106` no integer restriction |
| 8 | PLP `DCMod` paired-charging | ~~Low~~ | Components | ✅ Implemented in plp2gtopt | ✅ All 3 modes (0/1/2) handled |
| 9 | No autoregressive inflow model | **Low** | SDDP | Gap | N/A |
| 10 | No risk measures (CVaR) | **Low** | SDDP | Gap | N/A |
| 11 | Elastic penalty default | N/A | SDDP | ✅ Correct (1e6) | ✅ `planning_options_lp.hpp:471` |
| 12 | Both cut coefficient modes | N/A | SDDP | Correct | ✅ `benders_cut.cpp:83-127` both modes verified |

---

## 2. DC-OPF / Electric Grid Modeling

### 2.1 Formulation Choice: B-theta (Correct)

gtopt uses the B-theta (susceptance-angle) formulation with per-line Kirchhoff
constraints. This is the **correct choice for GTEP** — the PTDF alternative
requires recomputing the entire distribution factor matrix when candidate lines
are added/removed, and its dense matrix structure scales poorly for large networks.

**Kirchhoff constraint (scaled):**
```
-θ'_a + θ'_b + x_τ·f_p - x_τ·f_n = -φ'
```
where `x = scale_θ · (X / V²)`, `x_τ = τ · x`, `φ' = scale_θ · φ_rad`

**Physical equivalent (dividing by scale_θ):**
```
f_net = (V² / (τ·X)) · (θ_a - θ_b + φ)  =  B_eff · (θ_a - θ_b + φ)
```

This exactly matches the standard B-theta formulation with phase shift and tap ratio.

| Tool | Formulation | Line Vars | Suitable for GTEP? |
|------|------------|-----------|-------------------|
| **gtopt** | B-theta (per-line) | Split fp/fn | Yes |
| **PLP** | B-theta (per-line) | Split fp/fn per segment | Yes |
| **pandapower** | B-theta (matrix Bf*Va) | Single signed f | No (no expansion) |
| **MATPOWER** | B-theta (matrix Bf*Va) | Single signed f | No (no expansion) |
| **PLEXOS** | B-theta (nodal) | Varies | Yes |

### 2.2 Split Flow Variables

gtopt's split-flow approach (two non-negative vars `fp >= 0`, `fn >= 0`) is
inherited from PLP and is **necessary** for:

- Directional loss modeling (sender loses more than receiver gets)
- Asymmetric line limits (`tmax_ab != tmax_ba`)
- Per-direction capacity constraints for expansion
- Piecewise-linear quadratic loss approximation (segments fill monotonically)

When `lossfactor = 0` and no expansion, gtopt correctly collapses to a single
bidirectional variable (`line_lp.cpp:368-378`).

**Comparison with pandapower/MATPOWER:** These use a single signed flow variable
`P_ij = B_ij(θ_i - θ_j)` which is sufficient for fixed-topology dispatch but
inadequate for expansion planning with directional losses.

### 2.3 Reference Bus and Island Detection

gtopt implements island detection in `bus_island.cpp` using Union-Find (DSU).
It runs at LP-build time and assigns `reference_theta = 0` to one bus per
disconnected component. Multi-island handling is correct.

**Static detection** runs once before LP construction on the `Line` data. It
does not consider per-stage line activation.

**Runtime fix (added in `d4e945a0`):** `fix_stage_islands()` in
`system_lp.cpp:140-259` runs after all elements are added to the LP for each
(scenario, stage). It builds a DSU over theta-bearing buses connected by active
lines with Kirchhoff rows, detects components without a reference bus, and pins
one orphaned theta to zero per disconnected island. Logs a `SPDLOG_WARN` when
a runtime reference is pinned.

Three integration tests cover this: line inactive at one stage, all lines active
(no false positives), and multi-stage with island at one stage only.

### 2.4 Susceptance and Unit Conventions

gtopt computes: `x = scale_theta * (X / V^2)` where `X` = reactance [Ω],
`V` = voltage [kV] (defaulting to 1.0).

**Potential issue:** The `reactance` field is documented as "p.u." but `voltage`
is in kV. If a user sets `voltage = 220` (kV) and `reactance = 0.01` (p.u.),
the computation `220^2 / 0.01 = 4,840,000` gives an enormous susceptance.

pandapower handles this cleanly: users input physical parameters (`x_ohm_per_km`,
`length_km`, `vn_kv`) and the library converts internally.

**Recommendation:** Add validation when `|B_eff| > threshold` to warn about
potential unit mismatches.

### 2.5 Transmission Loss Models

gtopt supports two loss models, both superior to pandapower (which has no
DC-OPF losses):

| Model | Activation | Formula |
|-------|-----------|---------|
| **Linear** | `lossfactor > 0` | `P_loss = lossfactor × \|f\|` |
| **Quadratic** | `resistance > 0`, `loss_segments > 1` | `P_loss ≈ R × f² / V²` (piecewise) |

The piecewise-linear quadratic model uses the incremental cost trick:
segment k has loss coefficient `seg_width × R × (2k-1) / V²`. The LP naturally
fills lower-cost segments first, correctly approximating the convex envelope.

**Comparison with PLP:** PLP uses the same piecewise approach (`genpdlin.f`) with
loss coefficients `LinRes × (2*IFlu-1) / LinVNom²`. The formulations are
mathematically equivalent.

### 2.6 Loss Allocation: 100% Receiver vs PLP's 50/50

**gtopt** allocates 100% of losses to the receiving bus:
- Sender: coefficient = `-1` (full flow withdrawn)
- Receiver: coefficient = `+(1 - lossfactor)` (flow minus ALL losses)

**PLP** defaults to 50/50 split (configurable via `PerdEms`/`PerdRec`):
- Sender: `-(1 + PerdEms × factor)`
- Receiver: `+(1 - PerdRec × factor)`

**Mathematical impact on LMPs** (for 5% loss):
- gtopt: `λ_B / λ_A = 1/(1-0.05) = 1.0526`
- PLP 50/50: `λ_B / λ_A = 1.025/0.975 = 1.0513`

The difference is second-order (~0.13% for 5% losses) but compounds in meshed
networks.

### 2.7 Phase-Shifting Transformers (gtopt Extension)

gtopt extends standard DC-OPF with `tap_ratio` and `phase_shift_deg` — features
not present in PLP. The formulation is correct:
```
B_eff = V² / (τ × X)
Kirchhoff RHS = -scale_θ × φ_rad
```

This matches the standard PST DC-OPF model from MATPOWER/pandapower.

### 2.8 Capacity Expansion

gtopt uses **continuous LP relaxation** (no Big-M):
```
flow ≤ capainst
capainst = prev_capainst × (1 - derating) + expcap × expmod
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
| Backward pass | Solve T→1, generate cuts | `sddp_method.cpp:685-801` | `plp-fasedual.f` |
| Cut construction | `α ≥ z + β'(x - x̂)` | `benders_cut.cpp:83-127` | `plp-agrespd.f` |
| Convergence | `(UB-LB)/max(1,\|UB\|) < tol` | `sddp_iteration.cpp:118-250` | `plp-pdconvrg.f` |
| Apertures | Multi-scenario backward | `sddp_aperture_pass.cpp` | `FaseDuali` + `NApert` |
| Cut sharing | 4 modes (none/expected/accumulate/max) | `sddp_method.cpp:805-815` | `FOnePhi`, `FSeparaLP` |

### 3.2 Cut Construction: Two Modes

**Mode 1: Reduced Cost (Default)**
File: `benders_cut.cpp:83-104`

Dependent columns are fixed via bounds (`lo = hi = trial_value`). Reduced costs
of these fixed columns give the Benders cut coefficients. This is mathematically
proven correct: for a column fixed at `v̂`, the reduced cost `rc = c_j - π^T A_j`,
and from KKT conditions `rc ≡ π` (the coupling dual).

**Mode 2: Row Dual (PLP-compatible)**
File: `benders_cut.cpp:106-127`

Dependent columns kept at physical bounds, explicit coupling constraints
`x_dep = trial_value` added, row duals extracted. Matches PLP's approach in
`plp-agrespd.f`.

Both modes produce identical cuts at the same optimal basis. The `reduced_cost`
mode is slightly more efficient (no extra rows).

### 3.3 State Variable Propagation

Trial values are **always correctly chained** from the previous phase's primal
solution in `sddp_forward_pass.cpp:100-111`, regardless of
`StateVariableLookupMode`. The lookup mode only controls how nonlinear LP
coefficient updates (seepage, production factor, discharge limits) obtain the
reservoir volume for linearization.

**State variable types linked across phases:**
- Reservoir volume: `efin[phase_t] → sini[phase_{t+1}]`
- Battery SoC: same mechanism via `StorageLP`
- Installed capacity: `capainst[phase_t] → capainst_ini[phase_{t+1}]`
- Cumulative investment cost: `capacost[phase_t] → capacost_ini[phase_{t+1}]`

### 3.4 FINDING: Probability Double-Counting in `expected` Cut Sharing

**Severity: High. Status: ✅ Fixed in commit `e2c24740` (2026-03-30).**

The `expected` cut sharing mode **was** double-counting probability weights:

1. LP objectives include probability via `block_ecost()`:
   `LP_coeff = cost × probability × discount × duration / scale_objective`
   (see `cost_helper.hpp:74-81,121-127`)
2. Benders cut's `z*` (from `forward_full_obj`) and reduced costs inherit this
   probability weighting because they derive from probability-weighted LP costs
3. The old `expected` mode then applied probability weights **again** via
   `weighted_average_benders_cut()`, giving effective weight `p_s²/P_total`

**Fix:** `sddp_cut_sharing.cpp:65-99` now uses `average_benders_cut()` within
each scene (unweighted arithmetic mean), then `accumulate_benders_cuts()` to
sum across scenes. Since probability is already embedded in the LP, the sum of
all scene cuts correctly gives the expected-value cut:
```
E[cut] = Σ_s cut_s = Σ_s [z_s + ρ_s'(x - x̂)]   (where z_s, ρ_s already contain π_s)
```

The same commit also added `ProbabilityRescaleMode` for probability validation
and changed `scale_objective`/`scale_alpha` defaults from 1e3 to 1e6.

### 3.5 Elastic Filter Design (Superior to PLP)

gtopt's elastic filter is more sophisticated than PLP's:

| Feature | gtopt | PLP |
|---------|-------|-----|
| Clone LP before relaxation | Yes (preserves original) | No (modifies LP) |
| Penalized slack variables | Yes (sup/sdn per link) | Artificial variables |
| Three modes | `single_cut`, `multi_cut`, `backpropagate` | Single mode |
| Iterative backpropagation | Yes (backward through phases) | Partial |
| Auto-escalation | Infeasibility counter → multi_cut | No |
| Infeasibility tracking | Per-scene per-phase counter | None |

### 3.6 Convergence Criteria

gtopt supports three convergence modes (`sddp_enums.hpp:239-244`):

| Mode | Test | gtopt | PLP |
|------|------|-------|-----|
| `gap_only` | `(UB-LB)/max(1,\|UB\|) < tol` | Yes | Yes |
| `gap_stationary` | + gap change < `stationary_tol` over window | Yes | `FConvPGradX` |
| `statistical` | + CI overlap: `UB-LB ≤ z_{α/2} × σ` | Yes (default) | Yes (`UmbIntConf`) |

PLP also has a variance convergence test (`FConvPVar`) tracking multi-scenario
cost variance stabilization, which gtopt lacks.

### 3.7 Cut Management

| Feature | gtopt | PLP | SDDP.jl |
|---------|-------|-----|---------|
| Cut pruning | Yes (inactive cut removal) | No | Yes (`CutOracle`) |
| Cut capping | Yes (max cuts per phase) | No | No |
| Cut warm-start | Yes (load from file) | Yes (`LoadPlanes`) | Yes |
| Cut sharing across scenes | 4 modes | `FSeparaLP`/`FOnePhi` | Full sharing |

### 3.8 Alpha Variable Bounds and Scaling

The alpha (future cost) variable is bounded `[alpha_min, alpha_max]` with defaults
`[0, 1e12]` and scaled by `scale_alpha` (default `1,000,000`). This is the gtopt
equivalent of PLP's `ScalePhi`. The effective LP variable is `α_LP = α / scale_alpha`
with objective coefficient `scale_alpha`.

**Note:** This is an improvement over the previous report which stated no
`ScalePhi` equivalent exists. The `scale_alpha` parameter serves this purpose,
though its default (1M) may be insufficient for national-scale systems where
future costs reach 1e9-1e10.

---

## 4. Component Modeling Comparison

### 4.1 Thermal Generators

| Feature | gtopt | PLP | pandapower | PLEXOS | PSR SDDP |
|---------|-------|-----|------------|--------|----------|
| Linear cost | Yes (`gcost`) | Yes (`CenCVar`) | Polynomial | Poly + PWL | Yes |
| Quadratic cost | No | No | Yes | Yes | No |
| Network loss factor | Yes | Yes (`CenRen`) | N/A | Yes | Yes |
| Ramp rates | No | No | No | Yes | No |
| Min up/down time | No | No | No | Yes | No |
| Startup cost | No | No | No | Yes | No |
| Unit commitment | No (LP only) | No | No | Yes (MIP) | LP relax |
| Capacity expansion | Yes | No | No | Yes | LP relax |

gtopt's generator model is appropriate for long-term planning. PLEXOS uses
LP relaxation in its LT Plan mode (same simplification).

### 4.2 Hydro Cascade

| Feature | gtopt | PLP |
|---------|-------|-----|
| Network topology | Explicit junction/waterway graph | Implicit `CenGHid` arrays |
| Seepage | `ReservoirSeepage` (piecewise-linear, dynamic) | `genpdfil.f` (piecewise) |
| Production factor | `ReservoirProductionFactor` (concave envelope) | `RendParam` |
| Discharge limits | `ReservoirDischargeLimit` (piecewise) | Hardcoded Ralco |
| Run-of-river | Turbine with `flow` mode | Series plants (`NCenSer`) |
| Overflow modeling | Simple `spillway_capacity` + drain | Explicit overflow vars (`genpdreb.f`) |
| Flow conversion rate | `0.0036 hm³/(m³/s·h)` | `bdursc = BloDur/ScaleVol` |
| Chilean conventions | Via user constraints | Hardcoded Laja/Maule/Ralco/GNL |
| Autoregressive inflows | No (scenarios are independent) | PAR(p) models |

gtopt's junction/waterway graph is a significant improvement over PLP's implicit
topology, supporting arbitrary hydro configurations without specialized code.

**Gap:** PLP's overflow/rebalse model (`genpdreb.f`) with 3 variables per overflow
reservoir (discharge, positive deviation, negative deviation) is more detailed
than gtopt's simple spillway drain.

### 4.3 Battery / Energy Storage

| Feature | gtopt | PLP (old bat) | PLP (new ESS) |
|---------|-------|---------------|---------------|
| Unified model | Yes (Generator+Demand+Converter) | No | No |
| Charge/discharge eff | `input_eff/output_eff` | `FPC`/`FPD` | `nc`/`nd` |
| Self-discharge | `annual_loss` (exponential) | None | `mloss` (monthly) |
| Energy balance | `SoC' = SoC×(1-loss) + η_c×P_c - P_d/η_d` | SoC tracking | Similar |
| Multiple charge sources | Via bus connection | `MaxIny` per battery | `CenCarga` |
| Daily cycling | `daily_cycle = true` (default) | `edur/24` scaling | Not explicit |
| State variable in SDDP | Optional (`use_state_variable`) | Not coupled | Not coupled |
| Capacity expansion | Yes | No | No |
| Paired-charging (DCMod) | No equivalent | No | Yes (3 modes) |
| Soft minimum SoC | Yes (`soft_emin` + penalty) | No | No |

**All three PLP `DCMod` modes are supported:**
- **DCMod=0** (standalone): Battery in `battery_array`, no `source_generator`
- **DCMod=1** (generation-coupled): Battery with `source_generator` set to `cenpc`
- **DCMod=2** (regulation tank): Mapped to hydro `Reservoir` via
  `get_regulation_reservoirs()` in `battery_writer.py:421-495`, connected to the
  paired generator's junction with `daily_cycle=true`

The plp2gtopt converter handles all modes with validation (rejects non-hydro
centrals for DCMod=2) and comprehensive test coverage (`test_battery_writer.py`).

### 4.4 Demand

| Feature | gtopt | PLP |
|---------|-------|-----|
| Representation | LP variable with bounds | RHS constant |
| Per-demand failure cost | Yes (`fcost`) | Global only |
| Minimum energy | Yes (`emin`, `ecost`) | No |
| Loss factor | Yes (`lossfactor`) | No |
| Capacity expansion | Yes | No |
| Per-block profiles | `DemandProfile` | `BloPot` from file |

gtopt's demand modeling is significantly more flexible.

### 4.5 Reserve Modeling

gtopt has structured `ReserveZone` and `ReserveProvision` for spinning reserves
with up/down capacity, cost, and provision factors. PLP has minimal reserve
modeling. PLEXOS and PSR SDDP have more detailed reserve representations
including contingency-based requirements.

---

## 5. Numerical Conditioning

### 5.1 Dynamic Volume Scaling (Matches PLP)

**Status: ✅ Already implemented and default.**

PLP uses `ScaleVol(i) = max(1, Vmax_i/1000)`. gtopt has the same via
`EnergyScaleMode::auto_scale`, which computes `max(1.0, emax/1000)`.

The default is already `auto_scale` — when `energy_scale_mode` is unset,
`energy_scale_mode_enum()` returns `EnergyScaleMode::auto_scale`
(`reservoir.hpp:174`). An explicit `energy_scale` field overrides auto-scaling.

| Reservoir | Vmax (hm³) | auto_scale factor | LP variable range |
|-----------|-----------|-------------------|-------------------|
| Lake Laja | 6,000 | 6 | [0, 1000] |
| Colbun | 1,500 | 1.5 | [0, 1000] |
| Rapel | 200 | 1 | [0, 200] |
| Small pond | 0.1 | 1 | [0, 0.1] |

This matches PLP's behavior. No action needed.

### 5.2 Scaling Parameter Comparison

| Parameter | PLP (Scale Mode) | gtopt (default) | Ratio |
|-----------|-----------------|-----------------|-------|
| Objective | `ScaleObj` = 1e7 | `scale_objective` = 1e6 | 10x |
| Angles | `ScaleAng` = 1e4 | `scale_theta` = 1e3 | 10x |
| Volumes | Dynamic `ScaleVol(i)` | `energy_scale` (configurable) | Varies |
| Future cost | `ScalePhi` = 1e7 | `scale_alpha` = 1e6 | 10x |

gtopt's defaults are consistently ~10x less aggressive than PLP's. For IEEE
benchmark cases (small systems), this produces adequate coefficient ratios
(1e5-1e6). For large hydrothermal systems, users should increase these values.

### 5.3 LP Coefficient Monitoring

gtopt has built-in coefficient ratio monitoring (`lp_coeff_ratio_threshold`,
default 1e7) with automatic logging when the ratio exceeds the threshold.
Statistics include min/max absolute coefficients and their column names.
This is a valuable diagnostic tool not present in PLP.

### 5.4 Kappa (Basis Condition Number) Tracking

gtopt tracks the LP solver's basis condition number (`kappa`) per solve and
warns when it exceeds `kappa_threshold` (default 1e9). This can optionally
save the LP file for debugging (`KappaWarningMode::save_lp`). PLP does not
have this feature.

---

## 6. Features Unique to gtopt

### 6.1 Cascade Method (Novel Contribution)

The cascade method is a **novel engineering synthesis** of established techniques:

1. **Hierarchical decomposition** with progressive model refinement
2. **SDDP** as per-level solver
3. **Elastic target constraints** for regularization
4. **Name-based cut/state transfer** across structurally different LPs
5. **Cut forgetting** with configurable inheritance policies

**Novel aspects:**
- Configurable multi-level SDDP with heterogeneous LP formulations per level
  (e.g., Level 0: copper-plate, Level 1: transport, Level 2: Kirchhoff)
- Name-based state target transfer with elastic penalties and tolerance bands
- Explicit cut inheritance policies (`inherit_optimality_cuts`,
  `inherit_feasibility_cuts`, `inherit_targets`) with configurable lifetimes
- Global iteration budget distributed across levels

No published algorithm combines all of these.

### 6.2 Plugin Solver Architecture

Runtime loading of LP backends via `dlopen` with priority-based auto-detection:
cplex > highs > cbc > clp. Supports warm-starting, kappa extraction, and
solver-specific options through a clean `SolverBackend` virtual interface.

### 6.3 User Constraints

AMPL-inspired expression syntax for arbitrary linear constraints referencing
any LP variable by element class, name, and field.

### 6.4 Phase-Shifting Transformers in DC-OPF

Correct PST modeling with `tap_ratio` and `phase_shift_deg`, entering the
Kirchhoff constraint as RHS offset and susceptance modifier. Not present in PLP.

### 6.5 Unified Capacity Expansion

All dispatchable elements (generators, demands, batteries, lines) support
expansion via the generic `CapacityObjectLP` template with stage-wise derating.

### 6.6 FieldSched Data Model

Three-form parameter specification (scalar/array/file) enables concise JSON
input while supporting large time-varying datasets via Arrow/Parquet files.
Lazy loading with Arrow caching prevents redundant I/O.

---

## 7. Potential Implementation Issues

| # | Issue | Severity | Area | Recommendation |
|---|-------|----------|------|----------------|
| 1 | **Probability double-counting in `expected` cut sharing**: LP objectives included probability via `block_ecost()`, then `expected` mode applied probability weights again | **High** | SDDP | ✅ Fixed in `e2c24740`: uses `accumulate_benders_cuts()` instead of `weighted_average_benders_cut()` |
| 2 | **Dynamic volume scaling**: `energy_scale_mode` defaults to `auto_scale` (`max(1, emax/1000)`) matching PLP's `ScaleVol` | ~~High~~ | Numerics | ✅ Already default (`reservoir.hpp:174`) |
| 3 | ~~**Static island detection**~~: Runtime per-stage fix added | ~~Medium~~ | DC-OPF | ✅ Fixed in `d4e945a0` (`fix_stage_islands()`) |
| 4 | **Scaling defaults 10x less aggressive than PLP**: May cause numerical issues for national-scale systems | **Medium** | Numerics | Document recommended values for large systems |
| 5 | **No configurable loss allocation**: Fixed at 100% receiver; PLP uses 50/50 | **Low** | DC-OPF | Add `loss_allocation` field on Line |
| 6 | ~~PLP DCMod modes~~: All 3 modes implemented in plp2gtopt | ~~Low~~ | Components | ✅ DCMod 0/1/2 fully handled (`battery_writer.py`) |
| 7 | **No autoregressive inflow model**: Scenarios assumed independent | **Low** | SDDP | Add Markov state support for inflow correlation |
| 8 | **No risk measures (CVaR)**: Only expected-value optimization | **Low** | SDDP | Add risk-adjusted cut generation |
| 9 | **No PLP variance convergence test**: `FConvPVar` absent | **Low** | SDDP | Add optional criterion |
| 10 | **Overflow/rebalse modeling simpler than PLP**: Simple drain vs 3-variable model | **Low** | Hydro | Enhance spillway with overflow constraints |

---

## 8. Comparison Summary Table

| Aspect | gtopt | PLP | pandapower | PLEXOS | PSR SDDP |
|--------|-------|-----|------------|--------|----------|
| **DC-OPF** | B-theta (per-line) | B-theta (per-line) | B-theta (matrix) | B-theta | Zonal/Nodal |
| **Losses** | Linear + PWL quad | PWL quad, 50/50 | None (DC) | Piecewise | Iterative |
| **Phase shifters** | Yes | No | Yes | Yes | Yes |
| **SDDP** | Yes (modern C++) | Yes (classic Fortran) | No | Yes | Yes (reference) |
| **Cascade/multi-level** | Yes (novel) | No | No | LT/MT/ST | No |
| **Expansion** | All elements, continuous | None | None | MIP | LP relax + MIP |
| **Unit commitment** | No | No | No | Yes (MIP) | No (LP relax) |
| **Hydro** | Junction/waterway graph | Implicit arrays | N/A | Detailed | Detailed |
| **Battery** | Unified `StorageLP` | Two models | Basic | Detailed | Basic |
| **Solver backends** | CLP/CBC/HiGHS/CPLEX | CLP/CPLEX (OSI) | PIPS/IPOPT | Proprietary | Proprietary |
| **Volume scaling** | Configurable (auto available) | Dynamic per-reservoir | N/A | Auto | Unknown |
| **Coeff monitoring** | Yes (ratio + kappa) | No | No | Internal | Unknown |
| **Language** | C++26 | Fortran 90 | Python | C#/.NET | C/C++ |
| **Parallelism** | AdaptiveWorkPool | OpenMP | NumPy | Multi-threaded | Multi-threaded |
| **Risk measures** | No | No | N/A | Yes (CVaR) | Yes (CVaR) |
| **Open source** | Yes | No (regulated) | Yes | No | No |

---

## 9. Conclusions

### 9.1 Overall Assessment

gtopt is a **production-quality** GTEP solver with a mathematically correct
formulation and a modern, extensible architecture. It faithfully implements
the core SDDP algorithm while introducing novel features (cascade method,
plugin solvers, unified storage template) not found in any single commercial
or academic tool.

### 9.2 Most Impactful Improvement Opportunities

1. ~~**Verify `expected` cut sharing probability handling**~~ — ✅ Fixed in
   `e2c24740`. The `expected` mode now correctly sums probability-weighted cuts
   without re-weighting.

2. ~~**Default auto-scale for reservoir volumes**~~ — ✅ Already the default.
   `energy_scale_mode_enum()` returns `auto_scale` when unset (`reservoir.hpp:174`).

3. ~~**Per-stage island detection**~~ — ✅ Fixed in `d4e945a0`.
   `fix_stage_islands()` detects and fixes orphaned theta variables per stage.

4. **Document recommended scaling for large systems** — A table of
   recommended `scale_objective`, `scale_theta`, `energy_scale` values for
   different system sizes (IEEE 14-bus vs Chilean national system).

5. **Add risk measures** — CVaR support would align with PSR SDDP and PLEXOS,
   important for risk-averse planning studies.

### 9.3 Key Correctness Confirmations

- **B-theta DC-OPF formulation**: Mathematically correct, matches pandapower/MATPOWER
- **Benders cut construction**: Both `reduced_cost` and `row_dual` modes proven correct
- **State variable propagation**: Always correctly chained in forward pass
- **Piecewise-linear loss model**: Equivalent to PLP's formulation
- **Capacity expansion**: Correct continuous relaxation without Big-M
- **Phase shifter modeling**: Correct tap ratio and phase shift treatment

---

## 10. References

### Academic Literature
- Pereira & Pinto (1991). Multi-stage stochastic optimization applied to energy
  planning. *Mathematical Programming* 52, 359-375.
- Romero & Monticelli (1994). Hierarchical decomposition for TEP.
  *IEEE Trans. Power Systems* 9(1), 373-380.
- de Matos, Philpott & Finardi (2015). Improving SDDP performance.
  *J. Computational and Applied Mathematics* 290, 196-208.
- Zou, Ahmed & Sun (2019). Stochastic dual dynamic integer programming.
  *Mathematical Programming* 175, 461-502.
- Thurner et al. (2018). pandapower. arXiv:1709.06743.
- Zimmerman et al. (2011). MATPOWER. *IEEE Trans. Power Systems* 26(1), 12-19.

### Software References
- PSR SDDP: https://www.psr-inc.com/software/sddp/
- PLEXOS: https://www.energyexemplar.com/plexos
- SDDP.jl: https://odow.github.io/SDDP.jl/

### gtopt Source Files Referenced
- `source/benders_cut.cpp` — Cut construction (lines 83-181), elastic filter
- `source/sddp_forward_pass.cpp` — Forward pass, state propagation (lines 52-362)
- `source/sddp_method.cpp` — SDDP main loop, backward pass (lines 685-801)
- `source/sddp_iteration.cpp` — Convergence criteria (lines 118-315)
- `source/sddp_feasibility.cpp` — Elastic filter (lines 27-152)
- `source/line_lp.cpp` — Kirchhoff constraints (190-263), losses (83-186)
- `source/bus_lp.cpp` — Bus balance, theta variables (30-108)
- `source/bus_island.cpp` — Island detection (65-200)
- `source/cascade_method.cpp` — Cascade solver (31-150)
- `source/reservoir_lp.cpp` — Reservoir LP assembly (45-165)
- `source/generator_lp.cpp` — Generator LP (62-153)
- `source/demand_lp.cpp` — Demand LP (31-185)
- `source/battery_lp.cpp` — Battery LP (43-132)
- `source/capacity_object_lp.cpp` — Capacity expansion (20-129)
- `source/cost_helper.cpp` — Cost factor computation (18-101)
- `source/linear_problem.cpp` — LP statistics (70-120)
- `source/lp_stats.cpp` — Coefficient ratio logging (18-135)
- `include/gtopt/storage_lp.hpp` — Storage balance template (314-632)
- `include/gtopt/planning_options_lp.hpp` — Default constants
- `include/gtopt/sddp_enums.hpp` — SDDP mode enumerations
- `include/gtopt/state_variable.hpp` — Cross-phase state coupling (37-182)
