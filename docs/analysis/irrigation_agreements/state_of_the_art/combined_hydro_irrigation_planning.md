# Combined Irrigation and Hydroelectric Generation Long-Term Planning Models

## State of the Art Review

This document surveys long-term planning models that jointly optimize
hydroelectric generation and irrigation water allocation in multipurpose
reservoir systems. The focus is on models with annual or multi-year planning
horizons that account for seasonal irrigation demands, stochastic hydrology,
and reservoir management trade-offs.

---

## 1. Problem Definition and Scope

### 1.1 The Core Planning Problem

Multipurpose reservoirs must serve competing objectives: maximizing
hydroelectric revenue, satisfying irrigation water demands, maintaining
environmental flows, and managing flood risk. Globally, approximately
1,537 km^3 (19%) of total dam storage volume is dedicated to combined
irrigation and hydropower use [1]. The fundamental trade-off is temporal:
irrigation demands peak in dry/warm seasons when reservoir inflows are often
low, while hydroelectric operators may prefer to store water for peak-price
electricity periods. A long-term planner must determine monthly or seasonal
water release schedules that balance these competing objectives under
hydrological uncertainty.

### 1.2 Planning Horizons and Time Resolution

Long-term combined hydro-irrigation planning models typically span:

- **Medium-term**: 1--5 years, monthly or weekly time steps. Used for seasonal
  reservoir operation policies and irrigation campaign planning.
- **Long-term**: 5--30 years, monthly or seasonal time steps. Used for capacity
  expansion (new dams, irrigation infrastructure, hydropower units) and
  long-term water allocation agreements.
- **Strategic/investment**: 10--40 years, annual or multi-year blocks. Used for
  infrastructure planning under climate change scenarios.

Most operational models use monthly time steps to capture seasonal irrigation
patterns while remaining computationally tractable for stochastic formulations.

---

## 2. Mathematical Formulations

### 2.1 Linear Programming (LP) Approaches

LP is one of the most established techniques for reservoir operation
optimization [2][3]. The standard formulation for a single multipurpose
reservoir is:

**Objective function** (minimize total cost or maximize total benefit):

    min sum_t [ C_thermal(t) * P_thermal(t)
              + C_deficit(t) * D_irr(t)
              + C_spill(t) * S(t) ]

or equivalently:

    max sum_t [ B_hydro(t) * P_hydro(t)
              + B_irr(t) * W_irr(t) ]

**Water balance constraint** (for each reservoir r, time step t):

    V(r,t) = V(r,t-1) + I(r,t) - Q(r,t) - W_irr(r,t) - S(r,t) - E(r,t)

where:
- V(r,t): reservoir volume at end of period t
- I(r,t): natural inflow during period t
- Q(r,t): turbined outflow (for hydropower generation)
- W_irr(r,t): irrigation withdrawal (consumptive)
- S(r,t): spillage
- E(r,t): evaporation and infiltration losses

**Key constraints**:
- Reservoir capacity: V_min(r) <= V(r,t) <= V_max(r)
- Turbine capacity: 0 <= Q(r,t) <= Q_max(r)
- Irrigation demand satisfaction: W_irr(r,t) >= D_irr(r,t) * (1 - alpha)
  where alpha is the allowable deficit fraction
- Environmental flow: Q(r,t) + S(r,t) >= Q_env(r,t)
- Power generation (linearized): P_hydro(r,t) = eta * rho * g * h(r,t) * Q(r,t)
  (often piecewise-linearized as a function of head and flow)

The LP formulation is attractive because it guarantees global optimality and
provides dual variables (shadow prices) that represent the marginal value of
water in each reservoir at each time step -- critical information for
water-rights valuation.

### 2.2 Mixed-Integer Linear Programming (MILP)

MILP extends the LP formulation to handle:
- Binary startup/shutdown decisions for hydropower units
- Piecewise-linear approximations of nonlinear power-head relationships
- Discrete irrigation schedules (irrigate or not)
- Minimum generation requirements (unit commitment)

Recent work (2024-2025) uses MILP formulations with piecewise linearization of
power equations to approximate the nonlinear head-flow-power relationship,
involving binary variables for segment selection [4].

### 2.3 Nonlinear Programming (NLP) and MINLP

The hydropower generation function P = eta * rho * g * h(V) * Q is inherently
nonlinear because the effective head h depends on the reservoir volume V.
NLP formulations capture this directly but lose the guarantee of global
optimality. MINLP approaches combine integer decisions with nonlinear
physics, and have been applied to generation and transmission expansion
planning with hydro resources -- e.g., the HA-GTEP (Hydrology-Aware GTEP)
framework for the Ecuadorian power system (2025) [5].

### 2.4 Multi-Objective Formulations

Real-world hydro-irrigation planning involves inherently conflicting
objectives. Common multi-objective formulations include:

1. **Weighted-sum approach**: Scalarize objectives into a single function
   with policy-dependent weights.
2. **Epsilon-constraint**: Optimize one objective while constraining others.
3. **Pareto-front generation**: Use evolutionary algorithms (NSGA-II, NSGA-III,
   MOEA/D) to produce non-dominated solution sets.

Typical objectives include:
- Maximize hydropower generation revenue
- Minimize irrigation water deficit
- Minimize environmental flow violations
- Minimize flood risk (maximum reservoir level)
- Maximize agricultural profit

A Pareto analysis by Hassaballah et al. (2025) showed that at a 90%
confidence level, a 14.8% reduction in water scarcity requires a 3.15%
decline in energy production, providing actionable trade-off information for
policymakers [6].

---

## 3. Stochastic Optimization Methods

### 3.1 Stochastic Dual Dynamic Programming (SDDP)

SDDP, introduced by Pereira and Pinto (1991), is the dominant algorithm for
long-term hydrothermal scheduling worldwide. It solves multi-stage stochastic
linear programs by iteratively constructing piecewise-linear approximations of
the future cost function through Benders cuts (supporting hyperplanes).

**Key advantages for hydro-irrigation planning**:
- Handles multiple reservoirs without discretizing the continuous state space
- Provides marginal water values (dual variables on reservoir balance
  constraints) that reveal the opportunity cost of water allocation
- Naturally integrates with LP solver backends
- Scales to systems with dozens of reservoirs

**Irrigation in SDDP models**: Irrigation demands can be incorporated as:

1. **Hard constraints**: Minimum irrigation releases that must be met in each
   period, potentially creating infeasibility if inflows are very low.
   Handled via feasibility cuts.
2. **Soft constraints with penalty**: Irrigation deficits incur a penalty cost
   in the objective function, analogous to demand-fail costs in power systems.
3. **Conditional rules**: Irrigation allocations that depend on reservoir
   levels (e.g., reduced allocation when storage falls below a threshold).
   These introduce non-convexity, requiring binary variables or
   approximations.

The challenge of non-convex irrigation rules in SDDP has been studied
specifically for the Chilean Central Interconnected System (SIC), where
prescribed rules coordinate water uses between electricity and agriculture
sectors. Salazar et al. (2015) proposed a sub-optimal SDDP-based approach
to handle these non-convex irrigation constraints [7].

**Joint water-power optimization with SDDP**: Tilmant et al. (2008, 2012)
demonstrated that SDDP can couple power market and hydrological models to
assess spatio-temporal interactions between hydropower and irrigation, showing
that economically efficient water allocation considers marginal benefits of
irrigation and marginal costs of power production simultaneously [8].

### 3.2 Stochastic Dynamic Programming (SDP)

Classical SDP discretizes the state space (reservoir volumes, inflow states)
and solves Bellman equations backward in time. While it provides globally
optimal policies for small systems, computational cost grows exponentially
with the number of reservoirs (the "curse of dimensionality").

Dudley (1993) developed SDP models integrating irrigation water supply and
demand management for surface reservoir systems, with decisions about timing
and quantity of reservoir water releases into delivery systems [9].

### 3.3 Two-Stage and Multi-Stage Stochastic Programming

Two-stage stochastic optimization has been applied to robust operation of
multipurpose reservoirs. In the first stage, reservoir release policies are
determined; in the second stage, recourse actions address deviations from
expected inflows. This approach balances computational tractability with
uncertainty representation [10].

### 3.4 Chance-Constrained Programming

Chance constraints specify that irrigation demands must be met with at least
a given probability (reliability level P). The model then maximizes annual
hydropower production while satisfying:

    Pr[ W_irr(t) >= D_irr(t) ] >= P, for all t

This approach was developed specifically for reservoir operation balancing
hydropower and irrigation, determining the maximum annual hydropower produced
at a specified irrigation reliability level [11].

### 3.5 Deep Reinforcement Learning (Recent Advances)

Transformer-based deep reinforcement learning (T-DRL) methods have been
proposed for multiobjective hydropower reservoir operation (2023-2025).
These methods provide higher solution efficiency than direct deep RL and
superior generalization compared to multi-objective evolutionary algorithms
like NSGA-III and MOEA/D [12]. However, they lack the interpretability and
optimality guarantees of LP/SDDP approaches.

---

## 4. Seasonal Irrigation Demand Modeling

### 4.1 Crop Water Requirements

Irrigation demands follow strong seasonal patterns driven by:
- **Crop growth stages**: Planting, vegetative growth, flowering, and
  maturation have different water requirements.
- **Effective precipitation**: Rainfall that reaches the root zone reduces
  irrigation needs.
- **Evapotranspiration**: Temperature, humidity, wind, and solar radiation
  determine potential ET (typically estimated via Penman-Monteith).
- **Crop mix and area**: The portfolio of crops and irrigated area determines
  aggregate demand.

Stochastic models of irrigation demand use ecohydrological models based on
soil moisture dynamics to derive analytical expressions for seasonal net
irrigation requirements probabilistically [13].

### 4.2 Integration with Reservoir Operations

The seasonal mismatch between irrigation demand and reservoir inflows creates
the primary planning challenge:

- **Spring/summer**: High irrigation demand, often coinciding with low
  reservoir inflows (in Mediterranean and semi-arid climates) or snowmelt
  peaks (in mountain systems).
- **Autumn/winter**: Low irrigation demand, reservoir refilling period,
  potential for maximizing hydropower output during peak electricity prices.

Xinfengjiang reservoir case study (China) demonstrated that optimizing
reservoir operation considering estimated irrigation water demand alongside
hydropower generation improves overall system performance [14].

### 4.3 Crop Pattern Optimization

Advanced models jointly optimize crop patterns and irrigation water allocation
under climate change. Azari et al. (2020) developed a coupled network flow
programming-heuristic optimization model that determines both optimal crop
patterns and compatible irrigation schedules [15].

---

## 5. Water-Food-Energy Nexus Frameworks

### 5.1 Integrated Nexus Optimization

The Water-Food-Energy (WFE) Nexus framework provides the most comprehensive
approach to combined hydro-irrigation planning by considering all sectors
jointly. Key findings from recent research:

- **Nexus vs. silo planning**: Silo frameworks (optimizing water, energy, or
  food independently) lead to investments that generate more trade-offs, while
  nexus frameworks select investments that maximize cross-sector synergies,
  leading to co-investments in renewable energy, irrigation efficiency, and
  power infrastructure that matches the seasonality of hydropower and demand
  [16].

- **Multi-objective reservoir optimization in Taiwan**: Year-round joint
  operation reduced water shortage rates by up to 10%, increased food
  production by up to 47%, and increased hydropower benefit by up to
  USD 9.33 million per year in wet years [17].

### 5.2 Recent WFE Nexus Models (2025)

Hassaballah et al. (2025) developed an integrated simulation-optimization
framework for the Sefidroud irrigation and drainage network using system
dynamics modeling and multi-objective genetic algorithm optimization,
simultaneously pursuing four objectives: minimizing water scarcity,
maximizing agricultural profit, reducing GHG emissions through pump
electrification, and optimizing hydropower production [6].

Salehi et al. (2025) addressed the WFE nexus by developing a multi-objective
optimization model incorporating stochastic uncertainty through
chance-constrained programming, applying MOEA to simultaneously minimize
agricultural water shortages and maximize hydropower production [18].

Okola et al. (2025) provided a comprehensive review of multi-objective
optimization for the food-energy-water nexus, cataloguing objective functions,
decision variables, and optimization techniques across the literature [19].

---

## 6. Chilean Context: Hydrothermal Scheduling with Irrigation

### 6.1 Regulatory Framework

Chile's Water Code (1981, amended 2005 and 2022) establishes a system of
tradeable water rights (derechos de aprovechamiento) that may be transferred
independently of land ownership. Two types of rights coexist:

- **Consumptive rights** (irrigation): Water is extracted and not returned.
- **Non-consumptive rights** (hydropower): Water passes through turbines and
  is returned to the watercourse.

The Supreme Court has generally ruled in favor of non-consumptive use when
disputes arise between irrigators and generators, on grounds of higher
economic value [20].

### 6.2 Non-Convex Irrigation Constraints in the Chilean SIC

The Chilean Central Interconnected System (SIC) includes reservoirs where
prescribed rules coordinate water uses between electricity and agriculture.
These rules are often conditional on reservoir levels (e.g., "if reservoir
is below X hm3, reduce irrigation allocation by Y%"), creating non-convex
constraints incompatible with standard SDDP.

Salazar et al. (2015) proposed a sub-optimal SDDP-based method using
relaxation and penalty approaches to handle these non-convexities in the
Chilean system [7]. The National Irrigation Commission (CNR, created 1975)
oversees irrigation infrastructure and coordinates with the electricity
sector on water allocation agreements (convenios de riego).

### 6.3 Relevance to PLP/gtopt

The PLP (Programacion de Largo Plazo) software used in Chilean electricity
planning implements SDDP with irrigation constraints. Irrigation water rights
appear as:
- Flow-based rights: minimum downstream flow requirements
- Volume-based rights: allocated reservoir volumes for irrigators
- Seasonal schedules: irrigation campaign periods with increased releases

gtopt's SDDP implementation can model these constraints through:
- Minimum flow constraints on reservoir releases
- Seasonal demand profiles modifying water availability
- Penalty costs for irrigation deficit (analogous to demand_fail_cost)

---

## 7. Optimization Algorithms: Comparative Summary

| Method | Strengths | Limitations | Best For |
|--------|-----------|-------------|----------|
| LP | Global optimum, dual values, fast | Cannot handle nonlinearities | Operational scheduling |
| MILP | Handles discrete decisions, piecewise linearization | Computational cost grows with binaries | Unit commitment + scheduling |
| SDDP | Scales to large systems, handles uncertainty | Requires convexity, sub-optimal for non-convex rules | Long-term hydrothermal planning |
| SDP | Globally optimal policies | Curse of dimensionality (few reservoirs only) | Small systems (1-3 reservoirs) |
| NSGA-II/III | Multi-objective Pareto fronts | No optimality guarantee, slow convergence | Policy exploration, trade-off analysis |
| Deep RL | Adaptive, handles complex dynamics | Black-box, no optimality guarantee | Emerging research, real-time control |
| Chance-constrained | Explicit reliability levels | Conservative, computationally intensive | Reliability-focused design |

---

## 8. Key Modeling Challenges

### 8.1 Non-Convexity of Irrigation Rules

Many real-world irrigation agreements specify conditional rules (e.g.,
reduced allocation below certain reservoir thresholds). These create
non-convex feasible regions that are incompatible with LP and SDDP. Common
workarounds include:
- Penalty-based relaxation (soft constraints)
- Piecewise-linear approximation of conditional rules
- Scenario-dependent constraint activation
- Binary variable introduction (converting to MILP)

### 8.2 Stochastic Hydrology

Inflow uncertainty is the primary source of risk. Models typically represent
it through:
- Historical scenario trees (equiprobable or probability-weighted)
- Autoregressive (AR) or periodic autoregressive (PAR) models
- Vector autoregressive (VAR) models for spatially correlated basins
- Copula-based models for tail dependence
- Climate change scenarios (GCM-driven perturbations)

### 8.3 Multi-Reservoir Coordination

Systems with multiple reservoirs connected by river networks require
coordinated operation. The spatial coupling (upstream releases become
downstream inflows) and temporal coupling (stored water has future value)
make these problems computationally challenging. SDDP handles this naturally
through its decomposition structure.

### 8.4 Climate Change Adaptation

Long-term planning models increasingly incorporate climate change projections,
adjusting both inflow distributions and irrigation demand patterns. This
compounds the uncertainty and may invalidate historical inflow statistics [21].

---

## 9. Software and Tools

| Tool | Type | Irrigation Support | Reference |
|------|------|-------------------|-----------|
| PSR SDDP | Commercial | Irrigation constraints, water rights | PSR Inc. |
| GAMS (sddp.gms) | Modeling language | Generic multi-use reservoir | GAMS library [22] |
| WEAP | Simulation | Detailed irrigation modeling | Stockholm Environment Institute |
| MODSIM | Network flow | Water rights, irrigation priorities | Colorado State University |
| RiverWare | Rule-based simulation | Policy-driven reservoir operation | CADSWES, U. of Colorado |
| HEC-ResSim | Simulation | Reservoir operation rules | US Army Corps of Engineers |
| gtopt | Optimization (LP/MIP) | Via constraints on hydro releases | This project |

---

## 10. Research Gaps and Opportunities

1. **Integrated expansion + operation**: Most models separate capacity
   expansion (which dams/plants to build) from operational scheduling (how
   to operate them). Integrated frameworks like HA-GTEP [5] are emerging
   but remain computationally challenging.

2. **Non-convex irrigation rules in SDDP**: Efficient methods for handling
   conditional irrigation allocations within the SDDP framework remain an
   open problem. Current approaches rely on relaxation or penalty methods
   that sacrifice optimality.

3. **Climate-adaptive irrigation planning**: Models that jointly optimize
   crop patterns, irrigation infrastructure, and hydropower expansion under
   climate uncertainty are still rare.

4. **High-resolution temporal modeling**: Most long-term models use monthly
   time steps, but irrigation demands can vary significantly within a month.
   Bridging long-term planning with short-term operational detail is an
   active research area.

5. **Market integration**: As electricity markets become more complex
   (ancillary services, capacity markets), the opportunity cost of water
   allocation to irrigation versus hydropower becomes harder to evaluate.

---

## References

[1] Dams for hydropower and irrigation: Trends, challenges, and alternatives.
    *Renewable and Sustainable Energy Reviews*, 2024.
    https://www.sciencedirect.com/science/article/pii/S136403212400162X

[2] Introduction to linear programming as a popular tool in optimal reservoir
    operation: A review. *Academia*, 2015.
    https://www.academia.edu/14196412

[3] Optimization of Reservoir Operation using Linear Programming.
    *ResearchGate*, 2020.
    https://www.researchgate.net/publication/338790628

[4] A scalable mixed-integer programming model for multi-period hydropower
    planning in a single-reservoir system. *Applied Energy*, 2025.
    https://www.sciencedirect.com/science/article/abs/pii/S0306261925011067

[5] An integrated framework for the optimal expansion of hydro-dependent
    power systems under water-resource uncertainty. *Sustainable Energy,
    Grids and Networks*, 2025.
    https://www.sciencedirect.com/science/article/pii/S2590174525004295

[6] Evaluation of planning policy scenarios for the water-food and energy
    nexus through the development of a multi-objective optimization model.
    *Scientific Reports*, 2025.
    https://www.nature.com/articles/s41598-025-17085-z

[7] Optimizing Hydrothermal Scheduling with Non-Convex Irrigation
    Constraints: Case on the Chilean Electricity System. *Energy Procedia*,
    2015.
    https://www.sciencedirect.com/science/article/pii/S1876610215030246

[8] Tilmant, A. et al. Assessing marginal water values in multipurpose
    multireservoir systems via stochastic programming. *Water Resources
    Research*, 2008.

[9] Dudley, N. Integrating irrigation water demand, supply, and delivery
    management in a stochastic environment. *Water Resources Research*, 1993.
    https://agupubs.onlinelibrary.wiley.com/doi/abs/10.1029/93WR01099

[10] A Two-Stage Stochastic Optimization for Robust Operation of Multipurpose
     Reservoirs. *Water Resources Management*, 2019.
     https://link.springer.com/article/10.1007/s11269-019-02337-1

[11] Reservoir operation for hydropower optimization: A chance-constrained
     approach. *ResearchGate*, 2007.
     https://www.researchgate.net/publication/226588655

[12] Multiobjective Hydropower Reservoir Operation Optimization with
     Transformer-Based Deep Reinforcement Learning. *arXiv*, 2023.
     https://arxiv.org/html/2307.05643

[13] Probabilistic estimation of irrigation requirement under climate
     uncertainty. *Advances in Water Resources*, 2012.
     https://www.sciencedirect.com/science/article/abs/pii/S0309170812003168

[14] Estimating irrigation water demand using an improved method and
     optimizing reservoir operation for water supply and hydropower generation.
     *Agricultural Water Management*, 2012.
     https://www.sciencedirect.com/science/article/abs/pii/S0378377412002636

[15] Crop pattern planning and irrigation water allocation compatible with
     climate change. *Hydrological Sciences Journal*, 2020.
     https://www.tandfonline.com/doi/full/10.1080/02626667.2020.1844889

[16] Nexus vs. Silo Investment Planning Under Uncertainty. *Frontiers in
     Water*, 2021.
     https://www.frontiersin.org/articles/10.3389/frwa.2021.672382/full

[17] Exploring synergistic benefits of Water-Food-Energy Nexus through
     multi-objective reservoir optimization schemes. *Science of the Total
     Environment*, 2018.
     https://pubmed.ncbi.nlm.nih.gov/29574378/

[18] Development of a stochastic multi-objective optimization model for
     managing the water, food, and energy nexus in agriculture. *Scientific
     Reports*, 2025.
     https://www.nature.com/articles/s41598-025-31197-6

[19] Multi-Objective Optimization of the Food-Energy-Water Nexus Problem:
     A Review. *Earth's Future*, 2025.
     https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2024EF004718

[20] Fifty Years of Hydroelectric Development in Chile. *Water Alternatives*,
     2010.
     https://www.water-alternatives.org/index.php/all-abs/90-a3-2-5/file

[21] A State-of-the-Art Review of Optimal Reservoir Control for Managing
     Conflicting Demands in a Changing World. *Water Resources Research*, 2021.
     https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2021WR029927

[22] GAMS SDDP library model: Multi-stage Stochastic Water Reservoir Model.
     https://www.gams.com/latest/gamslib_ml/libhtml/gamslib_sddp.html

[23] A Review of Reservoir Operation Optimisations: from Traditional Models
     to Metaheuristic Algorithms. *Archives of Computational Methods in
     Engineering*, 2022.
     https://pmc.ncbi.nlm.nih.gov/articles/PMC8877748/

[24] Stochastic Dual Dynamic Programming and Its Variants: A Review.
     *SIAM Review*, 2024.
     https://epubs.siam.org/doi/full/10.1137/23M1575093

[25] A stochastic policy algorithm for seasonal hydropower planning.
     *Energy Systems*, 2023.
     https://link.springer.com/article/10.1007/s12667-023-00609-9

[26] Multi-usage hydropower single dam management: chance-constrained
     optimization and stochastic viability. *Energy Systems*, 2015.
     https://link.springer.com/article/10.1007/s12667-015-0174-4

[27] Optimising Water Allocation for Combined Irrigation and Hydropower
     Systems. *Engineering Proceedings*, MDPI.
     https://www.mdpi.com/2673-4591/69/1/66

[28] Applying the new multi-objective algorithms for the operation of a
     multi-reservoir system in hydropower plants. *Scientific Reports*, 2024.
     https://www.nature.com/articles/s41598-024-54326-z
