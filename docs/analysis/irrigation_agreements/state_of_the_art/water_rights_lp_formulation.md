# LP/MIP Formulations for Water Rights in Hydroelectric-Irrigation Systems

This document surveys LP and MIP formulations for modeling water rights
(irrigation agreements) within electricity grid optimization, with emphasis
on two distinct right types:

- **Flow-based rights** (m3/s): extraction from a river reach.
- **Volume-based rights** (hm3): extraction from a reservoir.

The downstream coupling -- where water released from a reservoir produces
river flows that serve flow-based irrigation rights -- is a central modeling
challenge.

---

## 1. Problem Statement

A hydro system includes reservoirs, river reaches, hydroelectric plants, and
irrigation users. Two classes of irrigation water rights must be honored:

1. **River flow rights**: the right to extract up to `q_irr` m3/s from a
   river reach, subject to the reach carrying sufficient flow.
2. **Reservoir volume rights**: the right to extract up to `V_irr` hm3 from
   a reservoir over a time period (stage, month, season).

Key complications:

- Water released from a reservoir to satisfy volume-based rights flows
  downstream, increasing the flow available to satisfy flow-based rights.
- Irrigation extraction is consumptive: withdrawn water does not return to
  the system (or returns only partially as return flows).
- Hydropower is non-consumptive: turbined water is released downstream.
- The optimizer must jointly dispatch generation and irrigation delivery to
  minimize total system cost while honoring all water rights.

---

## 2. Network Flow Representation

The standard approach models the hydro system as a directed graph [1, 2, 3]:

- **Nodes**: reservoirs, river junctions, demand points, irrigation
  extraction points, hydropower stations.
- **Arcs**: reservoir releases (turbine, spillway), channel flows, irrigation
  withdrawals, return flows, carryover storage.

Each arc carries a flow variable with lower and upper bounds. The only
structural constraints are **mass balance** at each node [1]:

```
  sum(outflows from node i) - sum(inflows to node i) = external_supply_i
```

This formulation is a minimum-cost network flow LP, solvable by specialized
algorithms or general LP solvers.

### 2.1 Reference: WRAP (Water Rights Analysis Package)

The WRAP system [4] allocates streamflow and reservoir storage within a
prior-appropriation water rights framework. Key features:

- Water rights have priorities; higher-priority rights are satisfied first.
- Reservoir storage is divided into operational zones (conservation,
  flood-control, inactive).
- Hydropower targets are modeled alongside irrigation diversions.
- Network flow LP allocates water across the basin per time step.

### 2.2 Reference: MODSIM

MODSIM [5] is a generalized river basin decision support system using network
flow optimization:

- Reservoir balancing routines divide storage into operational zones.
- Hydropower generation depends on discharge and head.
- Water rights and priorities are modeled as arc costs and bounds.
- LP is solved at each time step to allocate flows.

### 2.3 Reference: WEAP

WEAP [6] uses a priority-based LP allocation engine:

- Demand sites have priorities representing water rights seniority.
- Mass balance constraints at all nodes.
- Hydropower constraints limit flow through turbines.
- LP variables for reservoir release, turbine flow, bypass flow.

---

## 3. Core LP Formulation

### 3.1 Index Sets

| Symbol | Description |
|--------|-------------|
| `t in T` | Time periods (stages, months, blocks) |
| `r in R` | Reservoirs |
| `j in J` | River reaches (arcs) |
| `h in H` | Hydropower plants |
| `d_R in D_R` | Reservoir-based irrigation rights |
| `d_J in D_J` | River-flow-based irrigation rights |

### 3.2 Decision Variables

| Variable | Units | Description |
|----------|-------|-------------|
| `V(r,t)` | hm3 | Reservoir `r` storage at end of period `t` |
| `q_turb(h,t)` | m3/s | Turbine discharge at plant `h` |
| `q_spill(r,t)` | m3/s | Spillway discharge from reservoir `r` |
| `q_irr_R(d_R,t)` | m3/s | Irrigation extraction from reservoir `d_R` |
| `q_irr_J(d_J,t)` | m3/s | Irrigation extraction from river reach `d_J` |
| `q_flow(j,t)` | m3/s | Flow in river reach `j` |
| `irr_fail_R(d_R,t)` | hm3 | Irrigation right deficit (reservoir) |
| `irr_fail_J(d_J,t)` | m3/s | Irrigation right deficit (river) |

### 3.3 Objective Function

Minimize total cost over all time periods:

```
  min  sum_t { delta_t * [
    sum_h  C_gen(h) * P(h,t)                    -- generation cost (or negative revenue)
    + sum_{d_R}  C_fail_R * irr_fail_R(d_R,t)   -- penalty for unmet reservoir rights
    + sum_{d_J}  C_fail_J * irr_fail_J(d_J,t)   -- penalty for unmet river flow rights
    + sum_r  C_spill * q_spill(r,t)              -- spillway penalty (optional)
  ]}
```

Where `delta_t` is the duration of period `t` in hours, `P(h,t)` is the
power output, and `C_fail_R`, `C_fail_J` are penalty costs for unmet
irrigation rights (analogous to demand curtailment penalties).

### 3.4 Reservoir Mass Balance

For each reservoir `r` and period `t` [1, 7, 8]:

```
  V(r,t) = V(r,t-1)
           + A(r,t)                              -- natural inflow (hm3)
           - delta_t * q_turb(h_r,t) * K         -- turbine outflow
           - delta_t * q_spill(r,t)  * K         -- spillway outflow
           - delta_t * q_irr_R(d_R,t) * K        -- irrigation extraction
           - E(r,t)                              -- evaporation losses (hm3)
           - F(r,t)                              -- filtration losses (hm3)
```

Where `K` is the unit conversion factor from m3/s to hm3 per time step:
`K = delta_t_seconds / 1e6` (since 1 hm3 = 10^6 m3).

Bounds:

```
  V_min(r) <= V(r,t) <= V_max(r)
```

### 3.5 River Reach Flow Balance

For each river reach `j` and period `t`:

```
  q_flow(j,t) = sum_{upstream arcs} q_out(arc,t)
              + q_lateral(j,t)                    -- lateral inflows
              - sum_{d_J on reach j} q_irr_J(d_J,t)  -- irrigation extraction
              - q_losses(j,t)                     -- transmission losses
```

The upstream outflows include turbine discharges, spillway flows, and
reservoir irrigation extractions that flow downstream:

```
  q_out(r,t) = q_turb(h_r,t) + q_spill(r,t) + q_irr_R(d_R,t)
```

**Critical insight**: reservoir irrigation extractions that are taken from
the reservoir but delivered to a downstream point contribute to river flow
between the reservoir and the delivery point. This coupling is what makes
the joint optimization non-trivial.

However, if the irrigation extraction is consumptive (water is diverted
out of the river basin), then `q_irr_R` leaves the reservoir but does not
appear in the downstream reach flow balance. The modeler must distinguish:

- **At-reservoir extraction**: water is diverted from the reservoir itself
  and exits the basin. It reduces reservoir storage but does not contribute
  to downstream flow.
- **Through-turbine irrigation**: water passes through the turbine (or
  bypasses it) and flows downstream to the irrigation extraction point,
  where it is diverted. This water contributes to downstream flow until the
  point of extraction.

### 3.6 Flow-Based Irrigation Rights Constraints

For each river-flow irrigation right `d_J` on reach `j` in period `t`:

```
  q_irr_J(d_J,t) + irr_fail_J(d_J,t) >= Q_right_J(d_J,t)
  q_irr_J(d_J,t) <= q_flow(j,t)                -- cannot extract more than available
  q_irr_J(d_J,t) >= 0
  irr_fail_J(d_J,t) >= 0
```

Where `Q_right_J(d_J,t)` is the flow right (m3/s) for irrigation right
`d_J` in period `t`. The deficit variable `irr_fail_J` is penalized in the
objective.

**Minimum flow constraint**: after irrigation extraction, the remaining
flow in the reach must satisfy environmental or downstream requirements:

```
  q_flow(j,t) - sum_{d_J on j} q_irr_J(d_J,t) >= q_min(j,t)
```

### 3.7 Volume-Based Irrigation Rights Constraints

For each reservoir irrigation right `d_R` over a time horizon `T_right`:

```
  sum_{t in T_right} delta_t * q_irr_R(d_R,t) * K + irr_fail_R(d_R,t)
      >= V_right_R(d_R)
```

Where `V_right_R(d_R)` is the total volume right (hm3) over the period
`T_right`. This can be decomposed into per-period constraints if the right
specifies monthly or seasonal volumes:

```
  delta_t * q_irr_R(d_R,t) * K + irr_fail_R(d_R,t) >= V_right_R(d_R,t)
  q_irr_R(d_R,t) >= 0
  irr_fail_R(d_R,t) >= 0
```

### 3.8 Hydropower Generation

For each hydro plant `h` in period `t`:

```
  P(h,t) = eta(h) * rho * g * H(h,t) * q_turb(h,t) / 1e6   [MW]
```

In LP formulations, the head `H(h,t)` is typically linearized or
approximated as constant (fixed-head assumption). Under the fixed-head
assumption, power is linear in turbine discharge:

```
  P(h,t) = mu(h) * q_turb(h,t)
```

Where `mu(h) = eta * rho * g * H_avg / 1e6` is the generation coefficient
(MW per m3/s).

Turbine bounds:

```
  0 <= q_turb(h,t) <= Q_turb_max(h)
  0 <= P(h,t) <= P_max(h)
```

### 3.9 Downstream Coupling: Reservoir Extraction Creates River Flow

The key modeling insight for the gtopt problem: when water is extracted from
a reservoir to fulfill a volume-based irrigation right, the extraction can
take two physical forms:

**Form A: Direct extraction (side channel)**

Water exits the reservoir through a dedicated outlet and is delivered to
irrigation users without passing through the river reach below the dam.
In this case:

```
  -- Reservoir balance includes:  -delta_t * q_irr_R(d_R,t) * K
  -- Downstream river flow does NOT include q_irr_R
```

**Form B: Release-and-divert**

Water is released from the reservoir (possibly through the turbine or
spillway), flows downstream through the river reach, and is diverted at
an irrigation intake downstream. In this case:

```
  -- Reservoir balance includes:  -delta_t * q_release_irr(r,t) * K
  -- Downstream reach flow includes: +q_release_irr(r,t)
  -- Downstream irrigation extraction: q_irr_J(d_J,t) = q_release_irr(r,t)
```

This release serves both the reservoir volume right (through reservoir
depletion) and generates downstream river flow that can serve flow-based
rights. The optimizer may find it beneficial to release water from the
reservoir to simultaneously:

1. Generate hydropower (if released through the turbine).
2. Satisfy a downstream flow-based irrigation right.
3. Count toward a reservoir volume-based irrigation right.

This triple benefit creates a synergy that the LP naturally exploits.

---

## 4. Unit Conversion and Time Coupling

### 4.1 Flow to Volume Conversion

The conversion factor between flow rate (m3/s) and volume (hm3) over a
time period of duration `delta_t` hours is:

```
  K = delta_t * 3600 / 1e6   [hm3 per m3/s per period]
```

For example:
- 1 m3/s over 1 month (730 hours) = 2.628 hm3
- 1 m3/s over 1 hour = 0.0036 hm3

### 4.2 Per-Block vs. Per-Stage Rights

In the gtopt time structure (Scenario -> Stage -> Block):

- **Flow rights** (m3/s) are naturally per-block constraints, since flow
  is an instantaneous quantity.
- **Volume rights** (hm3) may span multiple blocks within a stage, or
  multiple stages. The constraint accumulates extractions:

```
  sum_{b in blocks(s)} delta_b * q_irr_R(d_R,b) * K_b >= V_right_R(d_R,s)
```

---

## 5. Extensions and Advanced Formulations

### 5.1 Priority-Based Allocation

Following the prior appropriation doctrine [4, 9], water rights have
priorities. In LP, this is modeled by assigning different penalty costs
to deficit variables:

```
  C_fail(d, priority=1) >> C_fail(d, priority=2) >> ...
```

Higher-priority rights have higher deficit penalties, ensuring the LP
satisfies them first. This is the approach used by WEAP [6] and
WRAP [4].

### 5.2 Return Flows

If a fraction `alpha` of extracted irrigation water returns to the river:

```
  q_return(d,t) = alpha(d) * q_irr(d,t)
```

The return flow is added to a downstream reach:

```
  q_flow(j_return,t) += q_return(d,t)
```

This is important when downstream rights depend on upstream return flows
[10].

### 5.3 Seasonal and Conditional Rights

Some rights are conditional on hydrological conditions:

```
  q_irr_J(d_J,t) <= Q_right_J(d_J,t) * z(d_J,t)    -- MIP: z is binary
```

Where `z(d_J,t) = 1` only if certain hydrological conditions are met
(e.g., reservoir above a threshold level). This introduces binary
variables, converting the LP to a MIP.

### 5.4 Head-Dependent Hydropower (MILP)

When head variation is significant, the generation function becomes
nonlinear. MILP approaches linearize it using piecewise-linear
approximation with binary variables [11, 12]:

```
  P(h,t) = sum_k  mu_k(h) * q_k(h,t)
  q_turb(h,t) = sum_k q_k(h,t)
  q_k(h,t) <= Q_max_k * z_k(h,t)         -- binary z_k
```

### 5.5 Multi-Objective: Hydropower Revenue vs. Irrigation Benefit

Some formulations use a weighted multi-objective approach [13, 14]:

```
  min  w1 * (generation cost) + w2 * (irrigation deficit penalty)
```

Or epsilon-constraint methods to generate Pareto frontiers showing
tradeoffs between hydropower generation and irrigation delivery.

### 5.6 SDDP Integration

In stochastic settings, SDDP naturally handles the irrigation constraints
by incorporating them into the stage subproblems [15, 16]. The irrigation
withdrawal appears in the reservoir mass balance, and the Benders cuts
(future cost functions) capture the marginal value of water for both
hydropower and irrigation:

```
  -- Stage t subproblem:
  min  c_t(x_t) + alpha_{t+1}
  s.t. reservoir balance, irrigation constraints, generation constraints
       alpha_{t+1} >= cut_k (Benders cuts from future stages)
```

The marginal water value (shadow price on the reservoir balance) reflects
the opportunity cost of water across all uses -- generation, irrigation,
and future value.

---

## 6. Formulation Summary for gtopt Implementation

For the gtopt GTEP model, the recommended formulation approach:

### 6.1 New Entities

- **`IrrigationRightFlow`**: a flow-based right (m3/s) attached to a river
  reach, with fields: `uid`, `name`, `river_uid`, `max_flow` (m3/s),
  `priority`, `profile` (time-varying demand).
- **`IrrigationRightVolume`**: a volume-based right (hm3) attached to a
  reservoir, with fields: `uid`, `name`, `reservoir_uid`, `max_volume`
  (hm3), `priority`, `extraction_type` (direct | through_river).

### 6.2 New Variables

| Variable | Type | Bounds | Description |
|----------|------|--------|-------------|
| `irr_flow(d_J,s,b)` | continuous | `[0, max_flow]` | Flow extraction (m3/s) |
| `irr_vol(d_R,s,b)` | continuous | `[0, max_rate]` | Volume extraction rate (m3/s) |
| `irr_flow_fail(d_J,s,b)` | continuous | `[0, +inf)` | Flow right deficit |
| `irr_vol_fail(d_R,s,b)` | continuous | `[0, +inf)` | Volume right deficit |

### 6.3 New Constraints

1. **River flow balance** (modify existing): subtract `irr_flow(d_J,s,b)`
   from the flow available in river reach `j`.

2. **Reservoir mass balance** (modify existing): subtract
   `irr_vol(d_R,s,b) * K_b` from the reservoir storage if extraction is
   direct; otherwise, the release goes through the normal outflow path.

3. **Flow right satisfaction**:
   ```
   irr_flow(d_J,s,b) + irr_flow_fail(d_J,s,b) >= Q_right_flow(d_J,s,b)
   ```

4. **Volume right satisfaction** (per stage):
   ```
   sum_b delta_b * irr_vol(d_R,s,b) * K_b + irr_vol_fail(d_R,s)
       >= V_right_vol(d_R,s)
   ```

5. **Flow availability**: irrigation extraction cannot exceed available flow:
   ```
   irr_flow(d_J,s,b) <= q_reach(j,s,b)
   ```

6. **Minimum downstream flow** (environmental or contractual):
   ```
   q_reach(j,s,b) - irr_flow(d_J,s,b) >= q_min(j,s,b)
   ```

### 6.4 Objective Function Additions

```
  + sum_{d_J,s,b} delta_b * C_fail_flow * irr_flow_fail(d_J,s,b)
  + sum_{d_R,s}   C_fail_vol * irr_vol_fail(d_R,s)
```

The penalty costs should be calibrated relative to `demand_fail_cost` to
reflect the relative priority of irrigation vs. electrical demand.

---

## 7. Literature References

[1] Colorado State Univ., "LP Applications and Computer Solutions to Simple
    Water Problems," Chapter 4 lecture notes.
    <https://civil.colorado.edu/~balajir/CVEN5393/lectures/Hughes-notes-chapter-04.pdf>

[2] PMC, "Network flow and flood routing model for water resources
    optimization," Scientific Reports, 2022.
    <https://pmc.ncbi.nlm.nih.gov/articles/PMC8913779/>

[3] MIT, "Network Models," Chapter 8, Applied Mathematical Programming.
    <https://web.mit.edu/15.053/www/AMP-Chapter-08.pdf>

[4] WRAP, "Water Rights Analysis Package Modeling System Reference Manual,"
    TR-255, Texas A&M Univ.
    <https://wrap.engr.tamu.edu/wp-content/uploads/sites/84/2022/07/ReferenceManual.pdf>

[5] MODSIM, "River Basin Management Decision Support System," User Manual.
    <https://s3.amazonaws.com/rand-marisa/model_documents/modsim_um_90.pdf>

[6] SEI, "WEAP Water Evaluation and Planning System User Guide."
    <https://www.weap21.org/downloads/weap_user_guide.pdf>

[7] Maurer, E., "Management of Water Resources Systems," Chapter 12,
    Hydraulics and Water Resources: Examples Using R.
    <https://www.engr.scu.edu/~emaurer/hydr-watres-book/management-of-water-resources-systems.html>

[8] WEAP, "Mass Balance Constraints."
    <https://www.weap21.org/webhelp/mass_balance_constraints.htm>

[9] Wikipedia, "Prior-appropriation water rights."
    <https://en.wikipedia.org/wiki/Prior-appropriation_water_rights>

[10] USU Extension, "Understanding Irrigation Water Optimization."
     <https://extension.usu.edu/irrigation/research/understanding-irrigation-water-optimization>

[11] Springer, "An MILP Approach for Short-Term Hydro Scheduling and Unit
     Commitment With Head-Dependent Reservoir."
     <https://www.researchgate.net/publication/3268610>

[12] Springer, "Hybrid Linear and Nonlinear Programming Model for
     Hydropower Reservoir Optimization," J. Water Resources Planning and
     Management, Vol 147, No 3, 2021.
     <https://ascelibrary.org/doi/abs/10.1061/(ASCE)WR.1943-5452.0001353>

[13] Springer, "Multipurpose Reservoir Operation: a Multi-Scale Tradeoff
     Analysis between Hydropower Generation and Irrigated Agriculture,"
     Water Resources Management, 2020.
     <https://link.springer.com/article/10.1007/s11269-020-02586-5>

[14] Copernicus, "Agricultural-to-hydropower water transfers: sharing water
     and benefits in hydropower-irrigation systems," HESS, 2009.
     <https://hess.copernicus.org/articles/13/1091/2009/>

[15] SIAM Review, "Stochastic Dual Dynamic Programming and Its Variants:
     A Review," 2024.
     <https://epubs.siam.org/doi/full/10.1137/23M1575093>

[16] GAMS, "Multi-stage Stochastic Water Reservoir Model solved with SDDP."
     <https://www.gams.com/latest/gamslib_ml/libhtml/gamslib_sddp.html>

[17] ScienceDirect, "An integrated framework for the optimal expansion of
     hydro-dependent power systems under water-resource uncertainty," 2025.
     <https://www.sciencedirect.com/science/article/pii/S2590174525004295>

[18] MDPI, "MILP for Optimizing Water Allocation and Reservoir Location:
     A Case Study for the Machangara River Basin, Ecuador," Water 11(5),
     2019.
     <https://www.mdpi.com/2073-4441/11/5/1011>

[19] Springer, "Optimal allocation model and method for parallel reservoir
     and pumping station irrigation system," Applied Water Science, 2023.
     <https://link.springer.com/article/10.1007/s13201-023-02006-0>

[20] RTI Press, "A Hydro-Economic Methodology for the Food-Energy-Water
     Nexus: Valuation and Optimization of Water Resources."
     <https://rtipress.scholasticahq.com/article/25497>

---

## 8. Key Takeaways for gtopt

1. **Network flow LP** is the standard framework for joint
   hydropower-irrigation optimization. The hydro system is modeled as a
   directed graph with mass balance at each node.

2. **Flow-based rights** (m3/s) are naturally per-block constraints that
   limit extraction from a river reach, subject to available flow.

3. **Volume-based rights** (hm3) are cumulative constraints that span
   multiple blocks or stages, requiring the sum of extractions to meet a
   volume target.

4. **Downstream coupling** is handled through the network topology: water
   released from a reservoir (whether for irrigation or generation) flows
   downstream and becomes available for flow-based rights extraction.

5. **Deficit variables** with penalty costs provide a soft-constraint
   mechanism that avoids infeasibility when water is scarce, analogous to
   gtopt's existing `demand_fail_cost` for unserved electrical load.

6. **Priority-based allocation** (prior appropriation) is modeled through
   differentiated penalty costs on deficit variables.

7. **Consumptive vs. non-consumptive** distinction is critical:
   hydropower is non-consumptive (water returns to the river), while
   irrigation is consumptive (water exits the basin). This affects the
   mass balance equations.

8. **SDDP compatibility**: the linear formulation is fully compatible with
   SDDP, as irrigation constraints add linear constraints and variables
   to each stage subproblem without introducing non-convexities.
