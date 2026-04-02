# Chilean Water-Energy Models: Irrigation Agreements and Hydroelectric Planning

## 1. Introduction

Chile presents a unique case in water-energy modeling due to its historical
irrigation agreements (*convenios de riego*) that legally constrain
hydroelectric reservoir operation. These agreements, some dating back to the
1940s, create non-convex operational constraints that challenge standard
optimization methods used in electricity system planning. This document surveys
the state of the art in modeling these constraints, focusing on the interaction
between hydroelectric generation and agricultural irrigation in Chilean river
basins.

## 2. Legal and Institutional Framework

### 2.1 Water Rights under Chilean Law

Chile's water governance originates from the 1981 Water Code (*Codigo de
Aguas*), underpinned by the 1980 Constitution, which established a market-based
system of tradeable water rights. Two categories are relevant:

- **Consumptive rights** (*derechos consuntivos*): water is consumed and not
  returned (irrigation, mining, drinking water).
- **Non-consumptive rights** (*derechos no consuntivos*): water must be returned
  to the river after use (hydroelectric generation, tourism).

The 1980 Constitution explicitly created non-consumptive rights to encourage
hydroelectric development. This dual-right system is the root of the
operational conflict: hydroelectric operators hold non-consumptive rights but
share reservoirs with irrigators who hold consumptive rights, and their
seasonal demand profiles are misaligned.

**2022 Reform (Law No. 21.435):** After 11 years of legislative proceedings,
Chile reformed the Water Code in April 2022. New water use rights are now
temporary (up to 30 years, automatically renewable), and the reform introduced
stronger provisions for integrated basin management. The reform was enacted in
a context where 50% of Chile's 345 municipalities were under water scarcity
decrees [1].

### 2.2 The Dirección General de Aguas (DGA)

Within the Ministry of Public Works, the DGA carries out hydrological
measurements, grants water use rights, and develops basin-level water balance
models that consider constituted rights and regularizable uses. Basin planning
under DGA includes hydrological and hydrogeological modeling, water balance
accounting, and determination of availability for new rights.

### 2.3 The Coordinador Electrico Nacional (CEN)

The CEN (formerly CDEC-SIC) is responsible for coordinating the operation of
Chile's National Electric System (SEN). It uses the PLP model for medium- and
long-term operational planning and publishes documentation on how irrigation
agreements are incorporated into the hydrothermal scheduling problem [2].

## 3. Key Irrigation Agreements (Convenios de Riego)

### 3.1 Laja Basin: The 1958 ENDESA-Riego Agreement

**Context.** Laja Lake is a natural volcanic-barrier lake in the Biobio Region,
supplying almost two million inhabitants, seven hydropower plants with
1,150 MW of installed capacity, and approximately 120,000 hectares of
irrigation. Outflow occurs through seepage and a controllable artificial
outlet, with hydroelectric stations at both [3].

**The Agreement.** In 1958, the Direccion de Riego (now DOH) and ENDESA signed
an agreement to manage hydraulic resources of the Laja basin, with ENDESA
responsible for reservoir administration. The agreement established operating
rules that prioritize irrigation water delivery during the growing season
(October--March) while allowing hydroelectric generation during the wet season
(April--September).

**Conflict.** Climate change and the mega-drought (starting circa 2010) have
reduced inflows while demand from both sectors remained constant. Laja Lake
reached its lowest recorded levels (below 600 Hm3), triggering emergency
negotiations. In December 2014, ENDESA, the MOP, and irrigators agreed to
limit extraction, with ENDESA restricting operations of the El Toro
hydroelectric facility (400 MW) [4][5].

**Modeling challenge.** The Laja agreement imposes state-dependent constraints
on reservoir releases that vary by season and lake level. These constraints
are inherently non-convex because they involve conditional rules (e.g., "if
the lake level is below threshold X, then maximum discharge is Y"), which
require binary variables to model exactly.

### 3.2 Maule Basin: The 1947 ENDESA-Riego Agreement

**Context.** Laguna del Maule is a multipurpose reservoir for irrigation and
hydroelectric generation in the Maule Region. The Colbun Reservoir downstream
(3,400 Mm3, the largest in Chile) also regulates waters for both hydroelectric
generation and irrigation.

**The Agreement.** The 1947 Convenio de Riego-ENDESA aimed to integrate
hydroelectric and irrigation use of the Maule River's waters. A 2021 Supreme
Court ruling affirmed that Laguna del Maule waters are primarily for
irrigation, intensifying the legal constraints on hydroelectric operators
(specifically Enel, successor to ENDESA) [6].

**Operational interference.** Since 2011, water scarcity from the mega-drought
has exacerbated the seasonal mismatch: irrigators need maximum water releases
in summer (December--March), while hydroelectric operators prefer to store
water for winter peak demand (June--August). This interference was the subject
of a 2023 Congressional investigative commission [7].

### 3.3 Tinguiririca Basin

The Tinguiririca basin in the O'Higgins Region features run-of-river
hydroelectric plants (La Confluencia, 163 MW; La Higuera) operated by
Tinguiririca Energia. An off-stream regulating reservoir (1.2 Mm3) provides
daily peaking storage. Downstream irrigation channels require continuous flow
monitoring to ensure equitable water distribution after hydroelectric
restitution [8].

## 4. Optimization Models and Approaches

### 4.1 PLP: Hydrothermal Coordination Model

The PLP (*Programacion de Largo Plazo*) model, developed by Colbun S.A. and
used by the CEN, is the primary tool for medium- and long-term operation
planning of Chile's hydrothermal system. Key characteristics:

- **Algorithm:** Stochastic Dual Dynamic Programming (SDDP/PDDE).
- **Scope:** Multi-nodal, multi-reservoir hydrothermal system optimization.
- **Irrigation agreements:** PLP incorporates irrigation agreements as explicit
  constraints. Recent versions include re-formulated irrigation agreement
  constraints and updates to the Laja Agreement representation [2][9].
- **Solver backend:** PLP has incorporated the CLP solver engine (COIN-OR) as
  one of its LP backends, alongside other solvers.

The PLP model is the operational standard for the Chilean electricity system
and is the closest analogue to what gtopt implements. Its treatment of
irrigation agreements is authoritative for the Chilean context.

### 4.2 SDDP (PSR) and the Convexity Challenge

The SDDP model, developed by PSR Inc. (Brazil), is widely used across Latin
America for hydrothermal scheduling. However, it faces a fundamental
limitation with Chilean irrigation agreements:

> Modeling the restrictions provided by irrigation agreements requires adding
> binary variables, which make it infeasible to solve the problem using SDDP,
> since binary variables prevent an optimization problem from having convexity,
> a necessary condition for using SDDP [10].

**Workaround in SDDP.** Irrigation requirements are incorporated by subtracting
them from the hydraulic balance of hydroelectric plants, and a slack variable
allows flexibilization, discriminating between priority and non-priority
irrigation demands.

**Alternative approaches to irrigation in SDDP:**

1. **Fixed water requirement:** Irrigation demand is subtracted as a fixed
   withdrawal during the irrigation season, effectively reducing available
   inflows for hydroelectric generation.
2. **Irrigation cost function:** A penalty cost for unsatisfied irrigation
   demand is added to the objective function, weighted alongside generation
   costs, converting the problem into a multi-objective optimization [11].

### 4.3 Sub-Optimal SDDP with Non-Convex Irrigation Constraints

A key contribution from Chilean research addresses the non-convexity problem
directly. Perez et al. (2015) presented a sub-optimal SDDP-based approach for
hydrothermal scheduling with non-convex irrigation constraints, demonstrated
on the Chilean SIC [12]:

- **Approach:** Portion-dependent constraints are updated dynamically before
  solving each stage problem, maintaining convexity at each LP subproblem
  while approximating the non-convex irrigation rules.
- **Validation:** Compared against a MILP formulation that captures
  non-convexity exactly. The SDDP-based approach finds feasible, near-optimal
  solutions with costs reasonably close to the MILP optimum.
- **Scalability:** The approach permits optimization of very large,
  country-scale systems in an SDDP framework, which is critical for practical
  application.

This represents the most directly relevant methodological contribution for
incorporating Chilean irrigation agreements into SDDP-based planning tools.

### 4.4 Multi-Objective Optimization: NSGA-II for Laja Lake

Bovermann et al. (2024) applied the Non-Dominated Sorting Genetic Algorithm II
(NSGA-II) to multi-objective optimization of Laja Lake operations, comparing
three operating policies [3]:

1. **Original policy:** Favored hydroelectric and agricultural users; found
   inadequate because the lake level decreased over the long term.
2. **Stakeholder-negotiated agreement:** The current policy, established through
   negotiation after the lake reached critically low levels.
3. **NSGA-II optimization:** Explored Pareto-optimal trade-offs between
   hydropower generation, irrigation supply, and tourism (Laja Falls).

**Key finding:** The optimization strategy prioritizes hydropower generation
during spring (when the lake fills before irrigation season), resulting in
reduced storage but improved overall system performance. Different scenarios
from the Pareto front can be explored to balance stakeholder interests.

### 4.5 WEAP Modeling for the Maule Basin

The Water Evaluation and Planning System (WEAP) has been applied to the Maule
basin as part of the DGA's Strategic Water Management Plan [13]:

- **Base model:** The "modelo cordillera" simulates the hydrological system
  including reservoir operation, irrigation diversions, and hydroelectric
  generation.
- **Modified WEAP:** Improved upon the original model to better represent the
  historical volume evolution of Laguna del Maule and incorporate multiple
  climate models (beyond the single CSIRO model in the original).
- **Scenario analysis:** Future scenarios (2022--2046) incorporate climate
  change projections (RCP8.5) and projected hydroelectric demand from a
  long-term electrical system optimization model.
- **Reliability results:** For irrigation, 62% reliability was obtained for an
  85% annual demand threshold; for hydroelectricity, 75% reliability for an
  85% monthly demand threshold.
- **Recommendation:** Use these tools to explore modifications to the 1947
  agreement and adapt to new climatic conditions to reduce operational
  interference.

### 4.6 Multi-Basin Hydrothermal Trade-Off Analysis

A University of Chile thesis studied trade-offs between hydroelectricity and
irrigation across a multi-basin hydrothermal system inspired by the Chilean
SIC [11]:

- **Formulation:** The long-term hydrothermal coordination problem was
  formulated with irrigation incorporated in two ways: (a) as a fixed water
  requirement during irrigation season, and (b) as an irrigation cost function
  weighted in the objective.
- **Economic results:** Depending on how irrigation costs are weighted, gains
  or losses for both sectors reach the same magnitude in dollars (up to
  90 million USD). However, this represents 98% of the irrigation sector's
  costs versus only 2% of the hydroelectric sector's costs, highlighting the
  asymmetric economic impact.
- **Platform:** Optimization was performed on the AMEBA platform.

### 4.7 Multipurpose Reservoir Operation Studies

The Tinguiririca basin has been studied for hydroelectric-irrigation impact
analysis, and a separate thesis examined the economic feasibility of
sustainable operation of multipurpose reservoirs in Chile, considering the
interplay between agricultural production and hydroelectric generation at the
Rapel, Colbun, and Pangue reservoirs [14].

## 5. Mathematical Formulation Considerations

### 5.1 The Core Modeling Challenge

Chilean irrigation agreements impose constraints that are fundamentally
different from standard reservoir operation limits:

| Constraint type | Standard reservoir | Chilean convenio |
|-|-|-|
| Storage bounds | Fixed min/max | State-dependent, seasonal |
| Release limits | Fixed or seasonal | Conditional on lake level |
| Priority rules | Single-purpose | Multi-party, legally binding |
| Temporal structure | Per-stage | Seasonal with hysteresis |
| Convexity | Convex | Non-convex (binary logic) |

### 5.2 Approaches to Non-Convexity

The literature identifies four main approaches:

1. **MILP formulation (exact):** Binary variables model the conditional
   constraints exactly. Suitable for deterministic or small stochastic
   problems but does not scale to country-size systems with SDDP.

2. **Dynamic constraint updating (approximate SDDP):** Perez et al. (2015)
   update portion-dependent constraints before each stage solve, maintaining
   LP convexity per subproblem. Near-optimal for practical systems [12].

3. **Penalty-based relaxation:** Irrigation demand is modeled as a soft
   constraint with penalty costs for violation. Converts the problem to
   multi-objective optimization. Used in SDDP via slack variables [10][11].

4. **Subtraction from hydraulic balance:** Irrigation withdrawals are
   pre-computed and subtracted from inflows before optimization. Simple but
   inflexible; does not capture the trade-off between uses.

### 5.3 Relevance to gtopt

For gtopt's GTEP formulation, Chilean irrigation agreements can be modeled as:

- **Reservoir constraints:** Seasonal minimum/maximum storage levels that
  depend on the irrigation agreement.
- **Release constraints:** Minimum downstream flow requirements during
  irrigation season, potentially state-dependent.
- **Penalty costs:** An irrigation deficit cost analogous to
  `demand_fail_cost`, penalizing unmet irrigation water delivery.
- **Binary constraints (if supported):** For exact representation of
  conditional operating rules, requiring MIP capability.

The PLP model's approach (direct incorporation as constraints within SDDP) and
the penalty-based approach (adding irrigation deficit costs to the objective
function) are the most practical for integration into an SDDP or monolithic
LP/MIP solver like gtopt.

## 6. Key Chilean Basins and Their Modeling Status

| Basin | Agreement | Reservoir capacity | Key model | Status |
|-|-|-|-|-|
| Laja | 1958 ENDESA-Riego | ~5,600 Mm3 (natural lake) | PLP, NSGA-II | Active conflict; NSGA-II Pareto analysis (2024) |
| Maule | 1947 ENDESA-Riego | 3,400 Mm3 (Colbun) | WEAP, PLP | Supreme Court ruling 2021; WEAP scenarios to 2046 |
| Tinguiririca | Operational rules | 1.2 Mm3 (off-stream) | Flow monitoring | Run-of-river; daily regulation |
| Rapel | Operational rules | 1,500 Mm3 | PLP | Multipurpose reservoir |
| Biobio | Multiple | Various | PLP | Multiple plants (Pangue, Ralco) |

## 7. Summary and Recommendations

### State of the Art

1. **PLP is the operational standard** for Chilean hydrothermal planning with
   irrigation agreements. It uses SDDP with direct constraint formulation and
   has been updated to handle the Laja and Maule agreements.

2. **Non-convex irrigation constraints** remain the central technical
   challenge. The sub-optimal SDDP approach of Perez et al. (2015) is the
   most practical method for country-scale systems.

3. **Multi-objective optimization** (NSGA-II) provides valuable insight for
   stakeholder negotiation but is computationally expensive for operational
   planning.

4. **WEAP** is effective for basin-level hydrological simulation but does not
   optimize the electrical system; it complements rather than replaces PLP.

5. **Climate change** is intensifying the conflict by reducing inflows while
   demands remain constant, making agreement adaptation urgent.

### Recommendations for gtopt

- Model irrigation agreements as seasonal reservoir constraints with
  associated penalty costs for violation, analogous to demand curtailment
  penalties.
- Support state-dependent release constraints through conditional bounds or
  piecewise-linear approximations.
- Consider the penalty-based approach (irrigation deficit cost function) as
  the most compatible method with gtopt's LP/MIP formulation.
- For basins with binding irrigation agreements (Laja, Maule), the
  constraints should be parameterized per-stage with seasonal profiles.

## References

[1] IWA Publishing. "Reform of the Chilean water code in 2022: shift from a
neoliberal model to a more public interest model." *Water Policy*, 2026.
<https://iwaponline.com/wp/article/doi/10.2166/wp.2026.347/111120/>

[2] Coordinador Electrico Nacional. "Modelos para la Planificacion y
Programacion de la Operacion."
<https://www.coordinador.cl/operacion/documentos/modelacion-del-sen/modelos-para-la-planificacion-y-programacion-de-la-operacion/>

[3] Bovermann et al. "The Chilean Laja Lake: multi-objective analysis of
conflicting water demands and the added value of optimization strategies."
*AQUA -- Water Infrastructure, Ecosystems and Society*, 73(3), 369, 2024.
<https://iwaponline.com/aqua/article/73/3/369/100571/>

[4] Terram Foundation. "Endesa y regantes limitan extraccion desde lago Laja."
<https://www.terram.cl/endesa-y-regantes-limitan-extraccion-desde-lago-laja/>

[5] La Tercera. "La disputa que enfrenta a canalistas y Endesa por uso de
laguna del Laja."
<https://www.latercera.com/noticia/la-disputa-que-enfrenta-a-canalistas-y-endesa-por-uso-de-laguna-del-laja/>

[6] Cooperativa. "Corte Suprema fallo contra Enel: Aguas del Embalse Laguna
del Maule son para riego."
<https://cooperativa.cl/noticias/pais/region-del-maule/corte-suprema-fallo-contra-enel-aguas-del-embalse-laguna-del-maule-son/2021-03-17/110023.html>

[7] Camara de Diputados. "Comision investigadora sobre Convenio de Riego
Endesa aprobo conclusiones," 2023.
<https://www.camara.cl/cms/noticias/2023/07/27/comision-investigadora-sobre-convenio-de-riego-endesa-aprobo-conclusiones/>

[8] Water-Energy-Food Nexus Platform. "Electricity Generation from Irrigation
Flow by Hidromaule, Chile."
<https://www.water-energy-food.org/resources/water-to-energy-electricity-generation-from-irrigation-flow-by-hidromaule-chile>

[9] Centro de Energia, Universidad de Chile. "Hydrothermal Coordination
Model -- PLP."
<https://centroenergia.cl/en/seleccionados/plp/>

[10] Hrudnick (PUC). "Modelos de Operacion: SDDP y convenios de riego."
<https://hrudnick.sitios.ing.uc.cl/alumno15/rieg/sddps.html>

[11] Universidad de Chile. "Tradeoffs entre hidroelectricidad y riego en un
sistema electrico hidrotermico multi-cuenca."
<https://repositorio.uchile.cl/handle/2250/165722>

[12] Perez et al. "Optimizing Hydrothermal Scheduling with Non-Convex
Irrigation Constraints: Case on the Chilean Electricity System."
*Energy Procedia*, 2015.
<https://www.sciencedirect.com/science/article/pii/S1876610215030246>

[13] Universidad de Chile. "Modelacion para el analisis de la interferencia
operacional entre hidroelectricidad y riego en la cuenca del Maule, Chile."
<https://repositorio.uchile.cl/handle/2250/196765>

[14] Universidad de Chile. "Operacion de embalses multiproposito: trade-offs
entre produccion agricola e hidroelectricidad."
<https://repositorio.uchile.cl/handle/2250/146672>

[15] Prieto & Bauer. "Hydroelectric power generation in Chile: an
institutional critique."
<https://sites.arizona.edu/cjbauer/files/2025/06/Prieto-Bauer-2012.pdf>

[16] Universidad de Chile. "Efecto del convenio de riego del sistema
hidroelectrico Laja sobre la programacion de largo plazo del sistema
interconectado central de Chile."
<http://repositorio.uchile.cl/handle/2250/139279>

[17] Tandfonline. "The water-energy nexus in Chile: a description of the
regulatory framework for hydroelectricity."
*Journal of Energy & Natural Resources Law*, 35(4), 2017.
<https://www.tandfonline.com/doi/10.1080/02646811.2017.1369278>

[18] SOCHID. "Convenio ENDESA-Riego, Maule Basin."
<https://www.sochid.cl/download/congresos/congreso_xxvi/Trabajo-GES-08-Congreso-XXVI.pdf>

[19] Universidad de Chile. "Estudio del impacto de la operacion hidroelectrica
en el uso del agua para riego en la cuenca del rio Tinguiririca, Chile."
<https://repositorio.uchile.cl/handle/2250/184623>

[20] UFRO. "Modelo de Coordinacion Hidrotermica PLP."
<https://bibliotecadigital.ufro.cl/v2/files/original/21f5b30217264574f736a2c93517725b71f74b6a.pdf>
