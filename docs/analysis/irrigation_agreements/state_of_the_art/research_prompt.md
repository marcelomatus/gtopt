# State of the Art: Water Rights and Electricity Grid Modeling — Research Prompt

## Objective

Survey the academic and technical literature on modeling water rights (irrigation)
jointly with electricity grid optimization, particularly in the context of SDDP
and long-term hydrothermal planning.

## Core Modeling Problem

The fundamental challenge is joint optimization of hydroelectric generation and
irrigation delivery, where two types of water rights coexist:

1. **Flow-based irrigation rights** — Rights to extract water from a river at a
   given flow rate (m³/s). These are downstream of reservoirs and depend on
   reservoir releases plus natural inflows.

2. **Volume-based irrigation rights** — Rights to extract a volume of water (hm³)
   from a reservoir. This water is stored on behalf of irrigators and released
   according to seasonal irrigation schedules.

The key coupling: water extracted from the reservoir for irrigation produces flows
downstream that can serve the flow-based irrigation rights of downstream users.
The optimizer must balance:
- Hydroelectric generation value (turbined water)
- Irrigation delivery obligations (both flow and volume)
- Reservoir level constraints (volume zones, operating rules)
- Seasonal patterns (irrigation season vs. non-irrigation season)
- Stochastic hydrology (inflow uncertainty)

## Search Topics

### 1. SDDP with Irrigation Constraints
- How SDDP (Stochastic Dual Dynamic Programming) formulations incorporate
  irrigation water rights
- PSR SDDP software handling of irrigation
- Non-convexity challenges (conditional rules based on reservoir levels require
  binary variables, incompatible with standard SDDP)
- Feasibility cuts and slack variable approximations as workarounds

Search terms:
```
SDDP irrigation water rights hydroelectric modeling
"stochastic dual dynamic programming" irrigation hydroelectric
SDDP "water rights" reservoir generation planning
PSR SDDP irrigation constraints formulation
SDDP "irrigation rights" long term planning energy
"dual dynamic programming" "water allocation" irrigation electricity
```

### 2. LP/MIP Formulations for Water Rights
- Linear/mixed-integer programming models for flow-based vs volume-based rights
- Reservoir-river coupling constraints
- Consumptive vs non-consumptive water rights in optimization

Search terms:
```
"water rights" "linear programming" hydroelectric irrigation formulation
"irrigation rights" reservoir river flow volume optimization model
"water allocation" LP MIP hydroelectric irrigation constraints
consumptive "water rights" hydroelectric optimization formulation
"water rights" electricity generation planning LP model
reservoir irrigation downstream flow rights optimization
```

### 3. Combined Hydro-Irrigation Long-Term Planning
- Models that plan over a year or multiple years
- Seasonal irrigation demand modeling
- Multi-objective reservoir operation (generation + irrigation)
- Water-energy nexus optimization

Search terms:
```
"hydro-irrigation" planning optimization long term reservoir
combined irrigation hydroelectric generation planning model
"water-energy nexus" reservoir irrigation hydroelectric optimization
multi-objective irrigation hydroelectric reservoir operation optimization
seasonal irrigation demand reservoir hydroelectric planning stochastic
"integrated water resources" hydroelectric irrigation LP optimization
```

### 4. Reservoir Downstream Flow Requirements
- Minimum flow constraints downstream of reservoirs
- Environmental flows combined with irrigation flows
- Consumptive vs non-consumptive rights interaction
- Reservoir mass balance with irrigation extraction

Search terms:
```
downstream minimum flow requirement reservoir hydroelectric optimization
"minimum flow" constraint reservoir hydroelectric irrigation model
consumptive non-consumptive water rights hydroelectric model
"environmental flow" reservoir hydroelectric irrigation optimization LP
reservoir release downstream irrigation flow requirement constraint
"derechos consuntivos" "no consuntivos" hidroeléctrica modelo optimización
```

### 5. Chilean Water-Energy Models
- Chile-specific models: WEAP, PLP, SDDP with convenios de riego
- How Chilean convenios de riego are formulated as optimization constraints
- Policy implications of water rights on electricity system planning

Search terms:
```
Chile "water rights" hydroelectric planning model optimization
Chile "derechos de agua" hidroeléctrica planificación modelo
"convenio de riego" modelo optimización Chile hidroeléctrica
Chile water-energy nexus irrigation hydroelectric reservoir
WEAP Chile hydroelectric irrigation Maule Laja model
PLP Chile "convenio riego" SDDP modelación
"programación largo plazo" Chile riego hidroeléctrica restricciones
```

## Key Websites to Search
- scholar.google.com — Academic papers
- sciencedirect.com — Elsevier journals (Water Resources Research, etc.)
- researchgate.net — Preprints and papers
- psr-inc.com — PSR SDDP documentation
- coordinador.cl — Chilean grid operator
- repositorio.uchile.cl — University of Chile theses

## Output Structure

Save all materials to `docs/analysis/irrigation_agreements/state_of_the_art/`:
- `sddp_irrigation_literature.md` — SDDP + irrigation modeling
- `water_rights_lp_formulation.md` — LP/MIP formulations
- `combined_hydro_irrigation_planning.md` — Long-term joint planning
- `reservoir_downstream_flow_rights.md` — Downstream flow constraints
- `chilean_water_energy_models.md` — Chile-specific models
- `research_prompt.md` — This file

## Relevance to gtopt

gtopt currently has NO implementation of irrigation agreement constraints.
The PLP Fortran code models Laja and Maule agreements as hard-coded modules
with 3 operating modes (off/original/full LP). Key challenges for gtopt:
- Non-convex conditional constraints (reservoir level thresholds)
- Multiple coupled reservoirs (Maule/Invernada/Colbún)
- Seasonal on/off irrigation patterns
- State variables for SDDP (irrigation economics, volume zone tracking)
- ~30 constraint rows and ~40 LP variables per stage (Maule)
