# Reservoir Downstream Flow Requirements: Modeling and Optimization

This document surveys how downstream flow requirements -- encompassing minimum
flow constraints, irrigation diversions, environmental flows, and water rights
-- are modeled in hydroelectric reservoir optimization, with emphasis on LP/MILP
formulations relevant to generation and transmission expansion planning (GTEP).

---

## 1. Overview

Hydroelectric reservoirs must balance energy production against a set of
downstream obligations: environmental (ecological) minimum flows, irrigation
diversions, municipal supply, and navigation. These obligations take the form
of hard or soft constraints in optimization models and directly affect the
feasibility and cost of hydro dispatch.

The fundamental tension is that water released through turbines generates
revenue, water diverted for irrigation is consumed (partially or wholly), and
minimum ecological releases may bypass turbines entirely via spillways. An
optimization model must capture all three pathways in a single mass-balance
framework while respecting temporal, spatial, and legal priorities.

---

## 2. Consumptive vs Non-Consumptive Water Use

### 2.1 Definitions

- **Consumptive use**: Water that is evaporated, transpired, incorporated into
  products, or otherwise removed from the immediate water system. Irrigation is
  the canonical consumptive use -- a fraction of diverted water is consumed by
  crops and never returns to the river.

- **Non-consumptive use**: Water that is withdrawn and returned to the same
  source in approximately the same quantity and quality. Hydroelectric
  generation is generally classified as non-consumptive because turbined water
  is discharged back into the river immediately downstream [1]. However,
  reservoir-based hydro does consume water through surface evaporation,
  averaging approximately 18 gal/kWh (68 L/kWh) in U.S. systems [2].

### 2.2 Modeling Implications

The consumptive/non-consumptive distinction is critical for water balance
modeling:

- **Turbine release** (non-consumptive): outflow `q_turb(t)` passes through
  turbines and appears in the downstream river reach. It is available for
  downstream users.

- **Irrigation diversion** (consumptive): diversion `q_irr(t)` is extracted
  from the reservoir or its downstream channel. Only a fraction `alpha`
  (the return flow coefficient, typically 0.3-0.7) returns to the river
  system, and often with a time lag.

- **Spill** (non-consumptive): overflow `q_spill(t)` bypasses turbines but
  remains in the river. It contributes to downstream flow but generates no
  revenue.

- **Evaporation** (consumptive): loss `q_evap(t)` is a function of reservoir
  surface area and meteorological conditions.

The net downstream flow available for downstream rights holders is:

```
q_downstream(t) = q_turb(t) + q_spill(t) + alpha * q_irr(t-lag)
```

where `alpha * q_irr(t-lag)` represents delayed return flows from irrigation.

### 2.3 Legal Frameworks

Water rights systems determine how these flows are allocated:

- **Prior appropriation** (western U.S., Chile): "first in time, first in
  right." Senior water rights holders have priority regardless of location.
  A hydroelectric operator with a junior right must curtail generation to
  satisfy senior irrigation rights [3].

- **Riparian rights** (eastern U.S., parts of Europe): rights are tied to land
  adjacent to the water body. All riparian users share the resource.

- **Chilean water code**: Water rights (derechos de aprovechamiento) are
  tradeable property rights, classified as consumptive or non-consumptive.
  Hydroelectric plants hold non-consumptive rights and must return all water
  to the river. Since 2005, the minimum ecological flow (caudal ecologico
  minimo) is defined as 20% of the average monthly flow, capped at 20% of
  the average annual flow based on at least 25 years of statistics [4].

---

## 3. Reservoir Mass Balance with Extraction Points

### 3.1 Standard Storage Continuity Equation

The foundational constraint in all reservoir optimization models is the storage
continuity (mass balance) equation:

```
S(t) = S(t-1) + I(t) - q_turb(t) - q_spill(t) - q_irr(t) - L(t)
```

where:

| Symbol        | Description                                      |
|---------------|--------------------------------------------------|
| `S(t)`        | Reservoir storage at end of period `t`            |
| `S(t-1)`      | Reservoir storage at end of previous period       |
| `I(t)`        | Natural inflow during period `t`                  |
| `q_turb(t)`   | Turbine discharge (generates power)               |
| `q_spill(t)`  | Spillage (uncontrolled or controlled overflow)     |
| `q_irr(t)`    | Irrigation diversion from reservoir               |
| `L(t)`        | Losses (evaporation + seepage + filtration)        |

This equation is linear and directly embeddable in LP/MILP formulations. All
decision variables (`q_turb`, `q_spill`, `q_irr`) appear with coefficient -1
relative to storage.

### 3.2 Extended Mass Balance for Cascade Systems

In cascade hydro systems (multiple reservoirs in series), the mass balance for
reservoir `j` at time `t` becomes:

```
S_j(t) = S_j(t-1) + I_j(t) + q_turb_{j-1}(t-tau) + q_spill_{j-1}(t-tau)
         - q_turb_j(t) - q_spill_j(t) - q_irr_j(t) - L_j(t)
```

where `tau` is the water travel time between reservoirs `j-1` and `j`, and
`q_irr_j(t)` represents irrigation diversions between the two reservoirs (or
directly from reservoir `j`).

### 3.3 Multiple Extraction Points

In practice, irrigation diversions may occur at several points along a river
reach between two reservoirs. The model can aggregate these into a single
equivalent diversion per reach or model them individually as nodes in a
network flow formulation.

For a river reach with `K` diversion points between reservoirs `j-1` and `j`:

```
q_downstream_j(t) = q_turb_{j-1}(t) + q_spill_{j-1}(t)
                     - sum_{k=1}^{K} q_div_k(t)
                     + sum_{k=1}^{K} alpha_k * q_div_k(t - lag_k)
                     + I_lateral(t)
```

where `I_lateral(t)` captures tributary inflows and `alpha_k` is the return
flow coefficient for diversion point `k`.

---

## 4. Minimum Flow Constraints

### 4.1 Types of Minimum Flow Requirements

Minimum flow constraints fall into several categories:

1. **Environmental / ecological flows**: Legally mandated minimum releases to
   preserve aquatic habitat, riparian vegetation, and water quality. These
   typically vary by month or season to mimic natural hydrological patterns.

2. **Irrigation supply obligations**: Contractual or legal requirements to
   deliver water to irrigation districts, often specified as minimum volumes
   per time period or minimum flow rates at specific delivery points.

3. **Navigation flows**: Minimum depths required for vessel passage.

4. **Water quality flows**: Minimum dilution flows to maintain downstream
   water quality standards.

5. **Senior water rights**: Under prior appropriation, flows required to
   satisfy senior downstream rights holders.

### 4.2 LP Formulation of Minimum Flow Constraints

#### Simple minimum release constraint

The most basic formulation requires total outflow to exceed a minimum:

```
q_turb(t) + q_spill(t) >= q_min(t)    for all t
```

where `q_min(t)` is a time-varying parameter (e.g., seasonal ecological flow).

#### Minimum flow at a downstream control point

When the constraint applies at a specific downstream location (not at the dam):

```
q_turb(t) + q_spill(t) - q_irr_reach(t)
  + alpha * q_irr_reach(t-lag) + I_lateral(t) >= q_min_cp(t)
```

This accounts for irrigation withdrawals and return flows between the dam and
the control point.

#### Separate environmental and irrigation minimums

When environmental flows and irrigation deliveries are independent obligations:

```
q_turb(t) + q_spill(t) >= q_env(t) + q_irr_demand(t)
```

or, if irrigation is diverted before the environmental flow measurement point:

```
q_turb(t) + q_spill(t) - q_irr(t) >= q_env(t)
q_irr(t) >= q_irr_demand(t)
```

### 4.3 Soft Constraints and Penalty Formulations

When minimum flow violations are permitted but penalized (common in
stochastic models where inflows are uncertain):

```
q_turb(t) + q_spill(t) + deficit(t) >= q_min(t)
deficit(t) >= 0
```

The objective function includes a penalty term:

```
minimize ... + sum_t penalty_cost * deficit(t) * duration(t)
```

This converts the hard constraint into a soft constraint, allowing the
optimizer to violate it at a cost. The penalty should be high enough to make
violations rare but not so high as to cause numerical difficulties.

### 4.4 Time-Varying and Seasonal Patterns

Environmental flow requirements typically follow seasonal patterns. In Chilean
regulation, the ecological flow is defined as:

```
q_eco(m) = max(0.20 * Q_avg(m), 0)
```

subject to:

```
sum_m q_eco(m) <= 0.20 * Q_annual_avg * 12
```

where `Q_avg(m)` is the long-term average flow for month `m` and
`Q_annual_avg` is the long-term average annual flow.

In LP models, these become fixed right-hand-side parameters for each time
period.

---

## 5. Environmental Flows Combined with Irrigation

### 5.1 The Competing Demands Problem

Environmental flows and irrigation demands compete for the same water
resource. The key modeling challenge is that:

- Environmental flows must remain in the river (non-consumptive minimum).
- Irrigation diversions remove water from the river (consumptive extraction).
- Both reduce the water available for hydroelectric generation.

The total non-power release requirement is:

```
q_non_power(t) = q_env(t) + q_irr(t)
```

Only `q_env(t)` contributes to downstream flow at the measurement point; the
irrigation component `q_irr(t)` is consumed (minus return flows).

### 5.2 Joint Optimization Framework

A multi-purpose reservoir LP that jointly optimizes hydropower revenue,
irrigation benefit, and environmental compliance takes the form:

```
maximize  sum_t [ p_elec(t) * P(t) * duration(t)
                + p_irr(t) * q_irr(t) * duration(t)
                - c_spill * q_spill(t) * duration(t)
                - c_deficit_env * deficit_env(t) * duration(t)
                - c_deficit_irr * deficit_irr(t) * duration(t) ]
```

subject to:

```
(1) S(t) = S(t-1) + I(t) - q_turb(t) - q_spill(t) - q_irr(t) - L(t)
(2) S_min <= S(t) <= S_max
(3) 0 <= q_turb(t) <= q_turb_max
(4) q_spill(t) >= 0
(5) q_turb(t) + q_spill(t) + deficit_env(t) >= q_env(t) + q_irr(t)
(6) q_irr(t) + deficit_irr(t) >= q_irr_demand(t)
(7) P(t) = eta * rho * g * h(t) * q_turb(t)    [linearized]
(8) deficit_env(t), deficit_irr(t) >= 0
```

where:

| Symbol              | Description                                    |
|---------------------|------------------------------------------------|
| `p_elec(t)`         | Electricity price in period `t`                 |
| `p_irr(t)`          | Value of irrigation water in period `t`         |
| `c_deficit_env`     | Penalty for environmental flow violation         |
| `c_deficit_irr`     | Penalty for irrigation delivery shortfall        |
| `P(t)`              | Power output                                    |
| `eta`               | Turbine efficiency                              |
| `h(t)`              | Net hydraulic head (may require linearization)   |

### 5.3 Priority Ordering

In practice, priorities are enforced through differential penalty costs:

```
c_deficit_env >> c_deficit_irr >> p_elec
```

This ensures that environmental flows are satisfied first, then irrigation
demands, and finally hydropower is maximized with remaining water. Alternative
approaches use lexicographic optimization or hierarchical constraint
satisfaction.

### 5.4 Seasonal Coordination

Irrigation demands are highly seasonal (concentrated in growing seasons),
while environmental flows may peak during spawning or migration periods that
do not always coincide. A well-formulated model captures this seasonality
through time-indexed parameters:

- `q_irr_demand(t)`: irrigation demand profile, driven by crop water
  requirements, typically peaking in dry summer months.
- `q_env(t)`: environmental flow profile, often defined by regulatory
  agencies based on natural flow regime analysis (e.g., Tennant method,
  IFIM, or percentage-of-flow approaches).

---

## 6. SDDP and Stochastic Approaches

### 6.1 SDDP Representation

In Stochastic Dual Dynamic Programming (SDDP) models such as PSR's SDDP
software, downstream flow constraints are represented as:

- **Minimum turbine outflow**: hard lower bound on `q_turb(t)`.
- **Minimum total outflow**: `q_turb(t) + q_spill(t) >= q_min(t)`, which
  may vary seasonally (chronological minimum spillage constraints).
- **Irrigation demands**: modeled as deterministic or stochastic demands
  that reduce available water [5].

The marginal water value (shadow price on the storage continuity constraint)
reflects the opportunity cost of all competing uses, including irrigation and
environmental releases.

### 6.2 State-Dependent Constraints

Environmental and irrigation constraints may be state-dependent (e.g.,
minimum release depends on current storage level or hydrological conditions).
Recent work embeds state-dependent maximum/minimum discharge constraints
within the SDDP algorithm without compromising computational tractability
[6].

---

## 7. Spill Modeling Considerations

### 7.1 Controlled vs Uncontrolled Spill

In LP formulations, spill must be modeled carefully:

- **Uncontrolled spill**: occurs only when the reservoir is full. This
  requires a complementarity condition `q_spill(t) * (S_max - S(t)) = 0`,
  which is non-linear. A common MILP approach introduces a binary variable:

  ```
  q_spill(t) <= M * z(t)
  S(t) >= S_max - M * (1 - z(t))
  ```

  where `z(t) = 1` when spill occurs [7].

- **Controlled spill**: environmental releases through bypass outlets.
  These are free decision variables bounded below by the environmental
  minimum.

### 7.2 Spill in Minimum Flow Satisfaction

When minimum flows can be satisfied by either turbine discharge or spill,
the optimizer naturally prefers turbine discharge (which generates revenue).
Spill is used only when turbine capacity is insufficient to meet minimum
flow requirements, or when the reservoir is full.

---

## 8. Return Flows and Network Effects

### 8.1 Return Flow Modeling

Irrigation return flows are the portion of diverted water that returns to the
river system after use. They are characterized by:

- **Return flow fraction** `alpha`: typically 0.3-0.7, depending on
  irrigation technology (flood irrigation has higher return flows than drip
  irrigation).
- **Time lag**: return flows arrive with a delay of days to weeks.
- **Quality degradation**: return flows may carry salts, nutrients, and
  pesticides.

In LP models, return flows are typically modeled as:

```
q_return_k(t) = alpha_k * q_div_k(t - lag_k)
```

These return flows augment downstream availability and may partially satisfy
downstream minimum flow requirements.

### 8.2 Downstream Rights Dependency

Under prior appropriation doctrine, downstream water rights holders may
legally depend on return flows from upstream irrigation. This creates a
modeling challenge: reducing upstream irrigation diversions (e.g., through
efficiency improvements) can paradoxically violate downstream rights by
reducing return flows [3].

---

## 9. Relevance to GTEP Modeling

### 9.1 Current gtopt Hydro Model

The gtopt solver models hydroelectric plants with reservoir storage, turbine
discharge, and spillage. The existing mass balance and minimum/maximum
constraints can be extended to incorporate:

1. **Irrigation diversion variables**: additional outflow terms in the
   reservoir balance equation.
2. **Minimum downstream flow constraints**: lower bounds on total release
   at specified control points.
3. **Seasonal flow profiles**: time-indexed minimum flow parameters.
4. **Deficit penalties**: soft constraints with configurable penalty costs.

### 9.2 Recommended Extensions

For modeling downstream flow rights in GTEP:

- Add an optional `min_outflow` parameter (time-series) to reservoir
  configuration, representing combined environmental and contractual
  minimum releases.
- Add an optional `irrigation_demand` parameter (time-series) for
  consumptive diversions from the reservoir or its downstream reach.
- Model irrigation diversions as a separate outflow variable in the mass
  balance, distinct from turbine release and spill.
- Support penalty-based soft constraints for both environmental and
  irrigation flow requirements, with configurable priority ordering.
- Consider return flow coefficients for irrigation diversions that feed
  into downstream nodes in cascade systems.

---

## 10. Key References

1. USGS Water-Use Terminology.
   https://www.usgs.gov/mission-areas/water-resources/science/water-use-terminology

2. NREL, "Consumptive Water Use for U.S. Power Production," 2003.
   https://docs.nrel.gov/docs/fy04osti/33905.pdf

3. PERC, "Instream Flow Rights within the Prior Appropriation Doctrine."
   https://www.perc.org/wp-content/uploads/2019/05/Instream-Flow-Rights-within-the-Prior-Appropriation-Doctrine_-Ins.pdf

4. Direccion General de Aguas (Chile), "Reglamento de Caudal Ecologico
   Minimo." https://dga.mop.gob.cl/publican-reglamento-de-caudal-ecologico-minimo/

5. PSR Inc., "SDDP User Manual, Version 17.2."
   https://www.psr-inc.com/wp-content/uploads/softwares/SddpUsrEng.pdf

6. Gjelsvik et al., "Hydropower Scheduling with State-Dependent Discharge
   Constraints: An SDDP Approach," Journal of Water Resources Planning and
   Management, Vol. 148, No. 11, 2022.
   https://ascelibrary.org/doi/10.1061/%28ASCE%29WR.1943-5452.0001609

7. Modelling overflow using mixed integer programming in short-term
   hydropower scheduling, Energy Systems, Springer, 2023.
   https://link.springer.com/article/10.1007/s12667-023-00602-2

8. Ahmadi et al., "A Review of Reservoir Operation Optimisations: from
   Traditional Models to Metaheuristic Algorithms," Archives of
   Computational Methods in Engineering, 2022.
   https://pmc.ncbi.nlm.nih.gov/articles/PMC8877748/

9. Kim et al., "Improvement of Downstream Flow by Modifying SWAT Reservoir
   Operation Considering Irrigation Water and Environmental Flow from
   Agricultural Reservoirs in South Korea," Water, 13(18), 2543, 2021.
   https://www.mdpi.com/2073-4441/13/18/2543

10. IEA Hydro, "Management Models for Hydropower Cascade Reservoirs,"
    Annex XIV, 2021.
    https://www.ieahydro.org/media/02e6ec8e/EAHydro_AnnexXIV_Management%20Models%20for%20Hydropower%20Cascade%20Reservoirs_CASE%20COMPILATION.pdf

11. Optimizing environmental flow based on a new optimization model
    balancing objectives among river ecology, water supply and power
    generation, Journal of Environmental Management, 2023.
    https://www.sciencedirect.com/science/article/abs/pii/S0301479723010496

12. Hughes, "LP Applications and Computer Solutions to Simple Water
    Problems," University of Colorado lecture notes.
    https://civil.colorado.edu/~balajir/CVEN5393/lectures/Hughes-notes-chapter-04.pdf

13. Springer Nature, "Implications of Environmental Constraints in
    Hydropower Scheduling for a Power System with Limited Grid and Reserve
    Capacity," Energy Systems, 2023.
    https://link.springer.com/article/10.1007/s12667-023-00594-z

14. Springer Nature, "Optimizing Environmental Flow Regime by Integrating
    River and Reservoir Ecosystems," Water Resources Management, 2022.
    https://link.springer.com/article/10.1007/s11269-022-03131-2
