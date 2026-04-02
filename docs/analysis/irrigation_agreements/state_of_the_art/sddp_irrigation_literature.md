# SDDP Irrigation Water Rights Literature Review

This document surveys how Stochastic Dual Dynamic Programming (SDDP) models
incorporate irrigation water rights alongside hydroelectric generation,
covering academic research, commercial software (PSR SDDP), and hydro-economic
modeling platforms.

---

## 1. Overview: SDDP and the Water-Energy-Agriculture Nexus

SDDP, originally developed by Pereira and Pinto (1991) for hydrothermal
scheduling, solves multi-stage stochastic optimization problems by iteratively
constructing piecewise-linear approximations of the future cost (or benefit)
function through Benders cuts. Because reservoir storage couples decisions
across time and space, SDDP is a natural framework for jointly optimizing
hydropower generation and irrigation water allocation.

The core challenge is that water in a reservoir has competing uses:

- **Hydropower generation**: turbined outflow produces electricity.
- **Irrigation withdrawals**: consumptive diversions reduce available water.
- **Environmental/ecological flows**: minimum downstream releases.
- **Flood control**: maximum storage limits.
- **Evaporation and filtration losses**: non-controllable water losses.

SDDP provides marginal water values (shadow prices on reservoir balance
constraints) that reveal the opportunity cost of allocating water to each use.

---

## 2. Key Papers and Modeling Approaches

### 2.1 Tilmant et al. (2008) -- Marginal Water Values in Multipurpose Systems

**Title**: "Assessing marginal water values in multipurpose multireservoir
systems via stochastic programming"

**Authors**: Tilmant, A., Pinte, D., Goor, Q.

**Journal**: Water Resources Research, 44, W12431

**Year**: 2008

**DOI**: 10.1029/2008WR007024

**URL**: <https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2008WR007024>

**Key contributions**:

- Extends SDDP to include **irrigation benefits directly in the objective
  function**, alongside hydropower revenues.
- Introduces **"dummy" state variables** representing accumulated water
  diverted to crops, added to the SDDP state vector. This handles the fact
  that irrigation benefits depend on the total volume allocated over the
  growing season (volume-based rights), not just instantaneous flow.
- The **marginal water values** are the Lagrange multipliers associated with
  the mass balance equations of the reservoirs, providing shadow prices in
  the absence of a water market.
- Accounts for **irrigation return flows** topology.

**Mathematical formulation**:

The optimal operation is a multistage decision problem:

```
max Z = E[ sum_{t=1}^{T} f_t(u_t, r_t, q_t) + v(u_{T+1}) ]
```

where:

- `u_t` = state vector (reservoir storages + accumulated irrigation diversions)
- `r_t` = release/allocation decisions
- `q_t` = stochastic inflows
- `f_t` = immediate benefit (hydropower revenue + irrigation benefit)
- `v(u_{T+1})` = terminal value function

**Irrigation benefit modeling**: At the end of the irrigation season,
agricultural benefits are proportional to the accumulated water diverted to
crops since their planting dates. The "dummy reservoir" state variable tracks
this cumulative diversion.

**Application**: Cascade of hydroelectric-irrigation reservoirs in the
Euphrates river basin (Turkey and Syria).

---

### 2.2 Tilmant et al. (2009) -- Agricultural-to-Hydropower Water Transfers

**Title**: "Agricultural-to-hydropower water transfers: sharing water and
benefits in hydropower-irrigation systems"

**Authors**: Tilmant, A., Goor, Q., Pinte, D.

**Journal**: Hydrology and Earth System Sciences, 13, 1091--1101

**Year**: 2009

**URL**: <https://hess.copernicus.org/articles/13/1091/2009/>

**Key contributions**:

- Contrasts **static** vs **dynamic** water allocation for irrigation:
  - **Static allocation**: farmers have fixed annual water rights (a
    prescribed volume per year) -- analogous to volume-based rights.
  - **Dynamic allocation**: SDDP continuously adjusts allocations based on
    current hydrologic state, temporarily reallocating upstream irrigation
    water to downstream hydropower when marginal values favor it.
- Shows that dynamic allocation yields **~6% higher annual benefits** on
  average vs static rights.
- Proposes a compensation mechanism: downstream hydropower beneficiaries
  compensate upstream farmers whose water is temporarily reallocated.

**Relevance to flow-based vs volume-based rights**:

- **Volume-based rights** are modeled as annual cumulative diversion targets
  (the "dummy reservoir" approach from Tilmant 2008).
- **Flow-based rights** correspond to minimum instantaneous diversion rates,
  modeled as lower bounds on diversion decision variables at each time step.

---

### 2.3 Rouge and Tilmant (2016) -- Multiple Near-Optimal Solutions

**Title**: "Using stochastic dual dynamic programming in problems with
multiple near-optimal solutions"

**Authors**: Rouge, C., Tilmant, A.

**Journal**: Water Resources Research, 52

**Year**: 2016

**DOI**: 10.1002/2016WR018608

**URL**: <https://agupubs.onlinelibrary.wiley.com/doi/full/10.1002/2016WR018608>

**Key contributions**:

- Addresses a fundamental challenge: SDDP solutions for multipurpose
  reservoir systems (hydropower + irrigation) can exhibit **large variations
  for small changes in input data** when multiple near-optimal solutions
  exist.
- Proposes a **reoptimization method** that simulates system decisions by
  periodically applying cuts from a given year, yielding steady-state
  probability distributions.
- Applied to a cascade of **7 multipurpose reservoirs** in the Euphrates
  river basin with competing hydropower and irrigation demands.

---

### 2.4 Tilmant and Kelman (2007) -- Stochastic Approach to Trade-offs

**Title**: "A stochastic approach to analyze trade-offs and risks associated
with large-scale water resources systems"

**Authors**: Tilmant, A., Kelman, R.

**Journal**: Water Resources Research, 43

**Year**: 2007

**DOI**: 10.1029/2006WR005094

**URL**: <https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2006WR005094>

**Key contributions**:

- Foundation paper for using SDDP in multipurpose reservoir management.
- Demonstrates trade-off analysis between hydropower and irrigation under
  hydrologic uncertainty.

---

### 2.5 Pereira-Cardenal et al. (2014, 2016) -- Joint Water-Power Optimization

**Title**: "Assessing climate change impacts on the Iberian power system using
a coupled water-power model"

**Authors**: Pereira-Cardenal, S.J., et al.

**Journal**: Climatic Change, 126(3-4)

**Year**: 2014

**URL**: <https://link.springer.com/article/10.1007/s10584-014-1221-1>

**Title**: "Joint optimization of regional water-power systems"

**Authors**: Pereira-Cardenal, S.J., et al.

**Journal**: Advances in Water Resources, 92

**Year**: 2016

**URL**: <https://www.sciencedirect.com/science/article/abs/pii/S0309170816300872>

**Key contributions**:

- Couples an SDDP-based hydrological model with a power market model for the
  **seven major river basins of the Iberian Peninsula**.
- Models **irrigation demands as constraints** competing with hydropower for
  the same water.
- Finds that current water allocations between hydropower and irrigation are
  **suboptimal** because they do not reflect the relative economic benefits
  of each use.
- Under climate change (lower precipitation, higher temperatures):
  hydropower production drops ~24%, thermal generation increases ~6.7%, and
  irrigation demands grow.
- Demonstrates that SDDP's spatial representation of the hydrological system
  enables optimization that other DP methods cannot achieve.

---

### 2.6 Goor, Tilmant, et al. (2009) -- Dynamic Management of Hydropower-Irrigation

**Title**: "Dynamic Management of Hydropower-Irrigation Systems"

**Authors**: Goor, Q., Tilmant, A., et al.

**Book**: Springer, 2009

**URL**: <https://link.springer.com/chapter/10.1007/978-3-642-02493-1_3>

**Key contributions**:

- Compares **static** management (fixed annual irrigation rights) vs
  **dynamic** management (SDDP-optimized allocation responding to current
  hydrologic state).
- In the dynamic approach, irrigation water is no longer a static asset but
  is allocated to maximize overall benefits considering latest hydrologic
  conditions and productivities of all users.
- SDDP simultaneously maximizes net benefits from both hydropower generation
  and irrigation, providing optimal allocation decisions and marginal water
  values while considering temporally and spatially correlated inflows.

---

### 2.7 Hjelmeland et al. (2022) -- State-Dependent Discharge Constraints

**Title**: "Hydropower Scheduling with State-Dependent Discharge Constraints:
An SDDP Approach"

**Authors**: Hjelmeland, M.N., et al.

**Journal**: Journal of Water Resources Planning and Management, 148(11)

**Year**: 2022

**URL**: <https://ascelibrary.org/doi/10.1061/%28ASCE%29WR.1943-5452.0001609>

**Key contributions**:

- Addresses **state-dependent environmental constraints** (minimum water
  levels for ecological purposes, irrigation, and drinking water) that
  introduce nonconvexity.
- Proposes **constraint relaxation** combined with auxiliary time-dependent
  lower reservoir volume boundaries to maintain SDDP computational
  performance.
- Relevant to irrigation because minimum flow requirements often serve dual
  ecological and irrigation purposes.

---

## 3. PSR SDDP Software: Irrigation Modeling

**URL**: <https://www.psr-inc.com/en/software/sddp/>

**Documentation**: <https://www.psr-inc.com/wp-content/uploads/softwares/SddpUsrEng.pdf>

PSR's SDDP is the most widely used commercial implementation, deployed in
power systems across Latin America, Europe, and Asia. Its handling of
irrigation is as follows:

### 3.1 Water Balance Equation

The hydro plant water balance in PSR SDDP is:

```
V(t) = V(t-1) + Q(t) - U(t) - S(t) - Irr(t) - Evap(t) - Filt(t) + upstream(t)
```

where:

- `V(t)` = reservoir storage at end of period t
- `V(t-1)` = storage at beginning of period
- `Q(t)` = natural inflow
- `U(t)` = turbined outflow (generates electricity)
- `S(t)` = spillage
- `Irr(t)` = irrigation withdrawal (in m^3/s)
- `Evap(t)` = evaporation losses
- `Filt(t)` = filtration/seepage losses
- `upstream(t)` = contributions from upstream plants

### 3.2 Irrigation as a Soft Constraint

**Irrigation values (in m^3/s) are subtracted from the water balance
equation.** Irrigation is NOT modeled as a hard constraint. Instead:

- A **slack variable** is associated with the irrigation demand of each hydro
  plant.
- **Penalties** are assigned to the slack variables in the objective function.
- Two priority modes are available:
  1. **Irrigation priority**: meet irrigation demand first, then produce
     power.
  2. **Power priority**: meet energy demand first, then irrigate.
- Users can also define **custom penalty values** for irrigation shortfalls.

### 3.3 Additional Hydro Constraints

PSR SDDP also models constraints relevant to irrigation contexts:

- **Minimum/maximum reservoir storage** (seasonal, can reflect irrigation
  storage requirements)
- **Minimum/maximum turbined outflow** (can enforce minimum downstream
  releases for irrigation)
- **Minimum/maximum total outflow** (turbined + spilled, relevant for
  downstream flow rights)
- **Variable production coefficient** (head-dependent efficiency)
- **Non-controllable spillage** thresholds

### 3.4 Relevance to Flow-Based vs Volume-Based Rights

| Right Type | PSR SDDP Modeling Approach |
|---|---|
| **Flow-based** | Minimum outflow constraints (m^3/s) at each time step; minimum turbined outflow bounds |
| **Volume-based** | Seasonal/annual cumulative irrigation withdrawal targets; penalty on shortfall slack variables |

---

## 4. AQUAPLAN-SDDP: Hydro-Economic Water Allocation

**URL**: <https://www.hydroeconomics.com/sddp/>

AQUAPLAN-SDDP is a Python-based stochastic hydro-economic model (using
GUROBI as the LP solver) that applies SDDP to river basin water economics.

### 4.1 Key Features

- **Arc-node representation** of the water system connecting demands with
  supplies.
- **Spatially-distributed water demands**: municipal/industrial, irrigated
  agriculture, energy generation, in-stream flow requirements.
- Determines **weekly or monthly water allocation** over many hydrologic
  scenarios (dry to wet).
- Maximizes **expected basin-wide net benefits** subject to operational
  constraints.

### 4.2 Decision Variables and Constraints

Allocation decisions include:

- Water abstractions (irrigation diversions)
- Downstream releases
- Storage volumes
- Spillage losses
- Evaporation losses

Constraints include:

- Max/min **storage capacity** (reservoirs)
- Max/min **water diversion capacity** (irrigation, industrial, municipal)
- Max/min **turbining capacity** (hydropower)
- **Environmental flow** requirements
- **Flood control** limits

### 4.3 Shadow Prices

The model produces **shadow prices ($/m^3)** for each constraint, revealing
the marginal economic value of relaxing storage, diversion, or flow
constraints -- directly useful for evaluating irrigation water rights.

---

## 5. GAMS SDDP Example: Multi-Stage Water Reservoir Model

**URL**: <https://www.gams.com/latest/gamslib_ml/libhtml/gamslib_sddp.html>

The official GAMS library includes a multi-stage stochastic water reservoir
model solved with SDDP (data from Vattenfall Energy Trading).

### 5.1 Core Water Balance Constraint

```
RES(t) = RES(t-1) + inflow(t) - X(t,'Hydro') - SPILL(t)
```

- `RES(t)` = reservoir level
- `X(t,'Hydro')` = turbined outflow
- `SPILL(t)` = spillage
- `inflow(t)` = stochastic water inflow

Slack variables `SLACKUP` and `SLACKLO` handle upper and lower reservoir
bounds respectively.

Irrigation can be added by introducing a diversion term `Irr(t)` subtracted
from the balance and an irrigation benefit term in the objective.

---

## 6. Mathematical Formulation Summary

### 6.1 General SDDP with Irrigation

The stage-t subproblem for a single reservoir (extends to multiple via
decomposition):

**Objective** (minimize cost or maximize benefit):

```
min  c_gen * U(t) + c_spill * S(t) + c_irr_slack * delta_irr(t)
     + alpha(t+1)
```

or equivalently (benefit maximization):

```
max  p_elec * rho * U(t) + B_irr(W_irr(t)) - c_spill * S(t)
     - c_irr_slack * delta_irr(t) + alpha(t+1)
```

where:

- `U(t)` = turbined outflow (m^3/s)
- `S(t)` = spillage (m^3/s)
- `rho` = production coefficient (MW per m^3/s)
- `p_elec` = electricity price ($/MWh)
- `B_irr` = irrigation benefit function
- `W_irr(t)` = water allocated to irrigation
- `delta_irr(t)` = irrigation shortfall slack
- `c_irr_slack` = penalty for irrigation shortfall
- `alpha(t+1)` = future cost/benefit function (approximated by Benders cuts)

**Water balance**:

```
V(t) = V(t-1) + Q(t) - U(t) - S(t) - W_irr(t) - Evap(t) - Filt(t)
```

**Irrigation demand constraint**:

```
W_irr(t) + delta_irr(t) >= D_irr(t)
```

where `D_irr(t)` is the required irrigation diversion at time t.

**Benders cuts** (from SDDP backward pass):

```
alpha(t+1) >= beta_k + lambda_k * V(t)    for k = 1, ..., K
```

### 6.2 Flow-Based vs Volume-Based Rights

**Flow-based rights** (instantaneous minimum diversion):

```
W_irr(t) >= W_irr_min(t)     for all t
```

This is a per-period lower bound on the irrigation diversion, enforced as a
hard constraint or with a penalty slack.

**Volume-based rights** (cumulative seasonal allocation):

```
sum_{t in season} W_irr(t) * Delta_t >= V_irr_right
```

where `V_irr_right` is the total volume the irrigator is entitled to over the
season, and `Delta_t` is the duration of period t. Following Tilmant (2008),
this is tracked via an auxiliary state variable:

```
W_cum(t) = W_cum(t-1) + W_irr(t) * Delta_t
```

with `W_cum(t)` added to the SDDP state vector so that Benders cuts capture
the dependency of future benefits on cumulative irrigation allocation.

---

## 7. Chilean Context

Chile's Water Code (1981, amended 2005, 2022) distinguishes between:

- **Consumptive water rights** ("derechos consuntivos"): primarily irrigation,
  water is consumed and not returned.
- **Non-consumptive water rights** ("derechos no consuntivos"): primarily
  hydropower, water must be returned to the river.

Key aspects relevant to SDDP modeling:

- Water rights can be traded independently of land ownership.
- Irrigation rights are typically volume-based (annual allocations from
  reservoir storage).
- Hydropower rights are flow-based (pass-through with return obligation).
- Conflicts arise when reservoir operators prioritize hydropower release
  timing over irrigation delivery schedules.
- The CNR (Comision Nacional de Riego) regulates hydroelectric plants
  associated with irrigation works (see `CNR_Centrales_Hidroelectricas_Obras_Riego.pdf`
  in this directory).

---

## 8. Summary of Modeling Approaches

| Approach | Flow-Based Rights | Volume-Based Rights | Software |
|---|---|---|---|
| **PSR SDDP** | Min outflow constraints per period | Seasonal withdrawal targets with penalty slack | Commercial (PSR) |
| **Tilmant SDDP** | Lower bounds on diversion variables | Dummy state variable tracking cumulative diversions | Academic (custom) |
| **AQUAPLAN-SDDP** | Min/max diversion capacity per node | Shadow prices on seasonal allocation constraints | Python + GUROBI |
| **GAMS SDDP** | Diversion term in water balance | Extensible via cumulative constraints | GAMS |

---

## 9. References

1. Pereira, M.V.F., Pinto, L.M.V.G. (1991). "Multi-stage stochastic
   optimization applied to energy planning." Mathematical Programming, 52,
   359--375.

2. Tilmant, A., Kelman, R. (2007). "A stochastic approach to analyze
   trade-offs and risks associated with large-scale water resources systems."
   Water Resources Research, 43, W06425.
   <https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2006WR005094>

3. Tilmant, A., Pinte, D., Goor, Q. (2008). "Assessing marginal water
   values in multipurpose multireservoir systems via stochastic programming."
   Water Resources Research, 44, W12431.
   <https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2008WR007024>

4. Tilmant, A., Goor, Q., Pinte, D. (2009). "Agricultural-to-hydropower
   water transfers: sharing water and benefits in hydropower-irrigation
   systems." Hydrology and Earth System Sciences, 13, 1091--1101.
   <https://hess.copernicus.org/articles/13/1091/2009/>

5. Goor, Q., Tilmant, A., et al. (2009). "Dynamic Management of
   Hydropower-Irrigation Systems." Springer.
   <https://link.springer.com/chapter/10.1007/978-3-642-02493-1_3>

6. Pereira-Cardenal, S.J., et al. (2014). "Assessing climate change impacts
   on the Iberian power system using a coupled water-power model." Climatic
   Change, 126(3-4).
   <https://link.springer.com/article/10.1007/s10584-014-1221-1>

7. Rouge, C., Tilmant, A. (2016). "Using stochastic dual dynamic programming
   in problems with multiple near-optimal solutions." Water Resources
   Research, 52.
   <https://agupubs.onlinelibrary.wiley.com/doi/full/10.1002/2016WR018608>

8. Pereira-Cardenal, S.J., et al. (2016). "Joint optimization of regional
   water-power systems." Advances in Water Resources, 92.
   <https://www.sciencedirect.com/science/article/abs/pii/S0309170816300872>

9. Hjelmeland, M.N., et al. (2022). "Hydropower Scheduling with
   State-Dependent Discharge Constraints: An SDDP Approach." Journal of
   Water Resources Planning and Management, 148(11).
   <https://ascelibrary.org/doi/10.1061/%28ASCE%29WR.1943-5452.0001609>

10. PSR Energy. "SDDP -- Systems Operation Planning."
    <https://www.psr-inc.com/en/software/sddp/>

11. PSR Energy. "SDDP User Manual VERSION 17.2."
    <https://www.psr-inc.com/wp-content/uploads/softwares/SddpUsrEng.pdf>

12. AQUAPLAN-SDDP. "Hydro-economic solutions."
    <https://www.hydroeconomics.com/sddp/>

13. GAMS. "sddp.gms: Multi-stage Stochastic Water Reservoir Model solved
    with SDDP."
    <https://www.gams.com/latest/gamslib_ml/libhtml/gamslib_sddp.html>

14. Dowson, O., et al. (2021). "Stochastic Dual Dynamic Programming and its
    Variants -- A Review." Optimization Online.
    <https://optimization-online.org/wp-content/uploads/2021/01/SDDP-Review.pdf>

15. PSR Energy. "SDDP Product Folder."
    <https://www.psr-inc.com/wp-content/uploads/softwares/SDDPFolderEng.pdf>
