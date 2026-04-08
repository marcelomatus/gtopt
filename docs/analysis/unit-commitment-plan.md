# Unit Commitment Implementation Plan for gtopt

## Comprehensive Analysis, Literature Review, and Competitive Implementation Roadmap

**Date**: 2026-04-08
**Status**: Planning / Pre-Implementation
**Author**: Generated from codebase analysis, comprehensive literature review,
PLEXOS/PyPSA/GenX/commercial tool comparison, and emerging grid technology analysis

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Background and Motivation](#2-background-and-motivation)
3. [PLEXOS UC: Complete Feature Analysis](#3-plexos-uc-complete-feature-analysis)
   - [3.1 Basic Commitment Properties](#31-basic-commitment-properties)
   - [3.2 Multi-Band Startup Costs (Hot/Warm/Cold)](#32-multi-band-startup-costs-hotwarmcold)
   - [3.3 Startup/Shutdown Profiles and Trajectories](#33-startupshutdown-profiles-and-trajectories)
   - [3.4 Piecewise-Linear Heat Rates](#34-piecewise-linear-heat-rates)
   - [3.5 Combined Cycle Gas Turbine (CCGT) Configurations](#35-combined-cycle-gas-turbine-ccgt-configurations)
   - [3.6 Reserve and Ancillary Service Co-optimization](#36-reserve-and-ancillary-service-co-optimization)
   - [3.7 Frequency, Inertia, and System Strength](#37-frequency-inertia-and-system-strength)
   - [3.8 Forced Outage and Maintenance](#38-forced-outage-and-maintenance)
   - [3.9 Synchronous Condenser Mode](#39-synchronous-condenser-mode)
   - [3.10 Emission Constraints During Startup](#310-emission-constraints-during-startup)
   - [3.11 Known Criticisms of PLEXOS UC](#311-known-criticisms-of-plexos-uc)
4. [Open-Source Tool Comparison](#4-open-source-tool-comparison)
   - [4.1 PyPSA](#41-pypsa)
   - [4.2 GenX](#42-genx)
   - [4.3 Feature Comparison Matrix](#43-feature-comparison-matrix)
   - [4.4 Competitive Gap Analysis](#44-competitive-gap-analysis)
5. [Literature Review](#5-literature-review)
   - [5.1 Classical UC and Tight Formulations](#51-classical-uc-and-tight-formulations)
   - [5.2 Stochastic and Robust UC](#52-stochastic-and-robust-uc)
   - [5.3 Machine-Learning-Assisted UC](#53-machine-learning-assisted-uc)
   - [5.4 Decomposition Methods](#54-decomposition-methods)
   - [5.5 Frequency-Constrained UC](#55-frequency-constrained-uc)
   - [5.6 Data-Driven Reserve Sizing](#56-data-driven-reserve-sizing)
6. [Renewable and Storage Challenges](#6-renewable-and-storage-challenges)
   - [6.1 Solar Integration (Duck Curve, Cloud Transients)](#61-solar-integration)
   - [6.2 Wind Forecast Uncertainty](#62-wind-forecast-uncertainty)
   - [6.3 Curtailment Optimization](#63-curtailment-optimization)
   - [6.4 Battery Degradation-Aware Commitment](#64-battery-degradation-aware-commitment)
   - [6.5 Hybrid Plant Commitment (Solar+Storage, Wind+Storage)](#65-hybrid-plant-commitment)
   - [6.6 Pumped Hydro Mode Switching](#66-pumped-hydro-mode-switching)
   - [6.7 Hydrogen Electrolyzer Commitment](#67-hydrogen-electrolyzer-commitment)
   - [6.8 Long-Duration Storage](#68-long-duration-storage)
7. [System Stability Constraints](#7-system-stability-constraints)
   - [7.1 Inertia-Constrained UC](#71-inertia-constrained-uc)
   - [7.2 RoCoF Constraints](#72-rocof-constraints)
   - [7.3 Frequency Nadir Constraints](#73-frequency-nadir-constraints)
   - [7.4 System Strength (Short-Circuit Ratio)](#74-system-strength-short-circuit-ratio)
   - [7.5 Grid-Forming Inverter Capabilities](#75-grid-forming-inverter-capabilities)
8. [Current gtopt Architecture](#8-current-gtopt-architecture)
   - [8.1 Generator LP Formulation](#81-generator-lp-formulation)
   - [8.2 Time Structure (Stage/Block)](#82-time-structure-stageblock)
   - [8.3 MIP Infrastructure](#83-mip-infrastructure)
   - [8.4 Component Patterns (AddToLP concept)](#84-component-patterns-addtolp-concept)
9. [Proposed Implementation Design](#9-proposed-implementation-design)
   - [9.1 Commitment Element](#91-commitment-element)
   - [9.2 Frequency Security Element](#92-frequency-security-element)
   - [9.3 Mathematical Formulation](#93-mathematical-formulation)
   - [9.4 JSON Input Schema](#94-json-input-schema)
   - [9.5 C++ Implementation Plan](#95-c-implementation-plan)
10. [Implementation Phases](#10-implementation-phases)
    - [Phase 1: Core Three-Bin UC](#phase-1-core-three-bin-uc)
    - [Phase 2: Ramp Constraints and Startup Profiles](#phase-2-ramp-constraints-and-startup-profiles)
    - [Phase 3: Min Up/Down Time and Multi-Band Startup Costs](#phase-3-min-updown-time-and-multi-band-startup-costs)
    - [Phase 4: Frequency Security and Inertia](#phase-4-frequency-security-and-inertia)
    - [Phase 5: CCGT Configurations](#phase-5-ccgt-configurations)
    - [Phase 6: Storage Commitment and Hybrid Plants](#phase-6-storage-commitment-and-hybrid-plants)
    - [Phase 7: Advanced Features](#phase-7-advanced-features)
11. [Testing Strategy](#11-testing-strategy)
12. [Risk Assessment](#12-risk-assessment)
13. [References](#13-references)

---

## 1. Executive Summary

This document proposes a comprehensive **unit commitment (UC)** implementation
for gtopt that goes significantly beyond the original plan. Based on a deep
analysis of PLEXOS's complete UC feature set, open-source tool limitations,
academic state-of-the-art formulations, and emerging challenges from
high-renewable grids, the design targets a **competitive, open-source
alternative** to commercial production cost models.

**Key design principles:**

1. **Tight formulation first**: Use the Morales-España three-bin tight-and-
   compact (TaC) formulation [[UC1]](#ref-UC1), [[UC2]](#ref-UC2) — proven
   to dominate alternatives in LP relaxation quality [[UC15]](#ref-UC15).
2. **Chronological stages only**: UC constraints apply where blocks represent
   consecutive time periods; silently skipped for load-duration-curve stages.
3. **Separate element pattern**: A `Commitment` element links to `Generator`
   objects (like `GeneratorProfile`), keeping UC opt-in per generator.
4. **Beyond thermal**: Address battery commitment states, pumped hydro mode
   switching, hydrogen electrolyzer scheduling, and hybrid plant co-optimization.
5. **Frequency security**: Native support for inertia, RoCoF, and frequency
   nadir constraints — a major gap in all open-source tools.
6. **CCGT configurations**: Configuration-based combined-cycle modeling — the
   single biggest gap between open-source and commercial tools.
7. **Emission tracking**: Startup/shutdown emissions separate from steady-state,
   with multi-band (hot/warm/cold) emission rates.

**Competitive positioning**: This implementation would make gtopt the **first
open-source GTEP tool** with:
- Tight MIP UC formulation (vs. PyPSA's basic big-M, GenX's clustering-only)
- Frequency security constraints (vs. none in any open-source tool)
- Combined-cycle configuration tracking (vs. none in any open-source tool)
- Battery degradation-aware commitment (vs. none in any open-source tool)
- Hot/warm/cold startup costs (vs. PyPSA missing, GenX partial)

---

## 2. Background and Motivation

### The Unit Commitment Problem

The **unit commitment (UC)** problem determines the optimal on/off schedule of
generating units over a time horizon, subject to physical and operational
constraints. It extends the economic dispatch problem (which gtopt already
solves) by adding:

- **Binary commitment decisions**: whether each generator is online or offline
  in each time period
- **Inter-temporal coupling constraints**: minimum up/down times prevent rapid
  cycling; ramp constraints limit power output changes between consecutive
  periods
- **Startup/shutdown costs**: fixed costs incurred when changing the
  commitment state of a unit

### Why gtopt Needs UC

Currently, gtopt treats generators as continuously dispatchable within
`[pmin, pmax]` bounds. This is appropriate for long-term capacity planning
(GTEP) where blocks may represent aggregated load duration curves rather than
consecutive hours. However, for **operational planning** studies with
chronological hourly resolution:

1. **Thermal generators cannot be turned on/off instantly** — coal plants may
   need 8+ hours minimum up-time after startup
2. **Minimum stable generation** (`pmin`) should only be enforced when the unit
   is online; an offline unit should produce exactly zero
3. **Ramp rate limits** constrain how quickly a unit can change its output from
   one hour to the next
4. **Startup costs** (\$5,000–\$50,000 per start for large thermal units) are a
   significant component of total operating cost
5. **System stability** requires minimum inertia and fault levels from
   committed synchronous machines — increasingly the binding constraint in
   high-renewable systems

Without UC constraints, the model may produce infeasible or unrealistic
operating schedules that cannot be implemented in practice. Palmintier (2014)
showed that UC detail changes total system costs by 1–5% and renewable
curtailment by 10–30% [[UC12]](#ref-UC12).

### Scope: Chronological Stages Only

UC constraints **only apply to stages where blocks represent consecutive time
periods** (chronological representation). This means:

- A stage represents a specific time span (e.g., one week)
- Its blocks represent consecutive hours within that span (e.g., 168 hourly
  blocks for a week)
- The first block is the first hour of the period, etc.

This is distinct from gtopt's **load-duration curve** representation where
blocks are ordered by magnitude rather than time sequence. UC constraints are
physically meaningless on non-chronological blocks since inter-temporal
constraints (ramps, min up/down times) require temporal adjacency.

---

## 3. PLEXOS UC: Complete Feature Analysis

PLEXOS (Energy Exemplar) is the industry standard for production cost
modeling with comprehensive UC support. This section catalogs its complete
UC feature set as the competitive benchmark. Property names match the PLEXOS
object model as documented in the PLEXOS Wiki/Help and Energy Exemplar
references [[UC26]](#ref-UC26).

### 3.1 Basic Commitment Properties

| Property | Type | Description |
|----------|------|-------------|
| `Units` | Integer | Number of identical units (integer commitment) |
| `Max Capacity` | MW | Maximum rated output per unit |
| `Min Stable Level` | MW or % | Minimum output when committed |
| `Must-Run Units` | Integer | Units that must be committed every interval |
| `Must-Run` | Flag | Forces unit to be committed whenever available |
| `Must Not Run` | Flag | Prevents unit from being committed |
| `Fixed Load` | MW | Unit runs at exactly this output when committed |
| `Commit` | Integer | Externally fixed commitment schedule |
| `UC Optimality` | Enum | `Rounded Relaxation`, `Integer Optimal`, `Linear Relaxation` |

### 3.2 Multi-Band Startup Costs (Hot/Warm/Cold)

PLEXOS supports **time-dependent startup costs** based on offline duration.
This is critical for combined-cycle and large coal units where boiler
temperature decays with time.

| Property | Description |
|----------|-------------|
| `Start Cost` | Single startup cost (\$) — used if no bands defined |
| `Start Cost Time` (band 1..N) | Time threshold (hours offline) per band |
| `Start Cost` (band 1..N) | Cost (\$) per band, increasing with offline time |
| `Hot/Warm/Cold Start Cost` | Named bands for common three-tier structure |
| `Hot/Warm Start Time` | Threshold hours defining hot vs. warm vs. cold |
| `Shutdown Cost` | Cost per shutdown event |

**Formulation**: Auxiliary binary variables for each startup type. If
$d_{g,t}$ is the offline duration before startup at $t$:

$$SC_t = \sum_{s \in \{hot,warm,cold\}} c_s \cdot \delta_{s,t}$$

$$\sum_s \delta_{s,t} = v_{g,t}, \quad
\delta_{hot,t} \leq \sum_{\tau=t-T^{hot}}^{t-1} w_{g,\tau}$$

This follows Silbernagl et al. (2016) [[UC16]](#ref-UC16).

### 3.3 Startup/Shutdown Profiles and Trajectories

| Property | Description |
|----------|-------------|
| `Start Profile` | MW trajectory during startup (multi-period ramp) |
| `Start Profile Time` | Duration of startup profile (hours) |
| `Shutdown Profile` | MW trajectory during shutdown |
| `Shutdown Profile Time` | Duration of shutdown profile |
| `Run Up Rate` | MW/min during startup phase |
| `Run Down Rate` | MW/min during shutdown phase |

These create **multi-period startup constraints** where the unit follows a
prescribed trajectory from 0 MW to Min Stable Level. During these intervals
the unit is committed but cannot be freely dispatched or provide reserves.
Critical for large thermal units (supercritical coal, nuclear) where startup
takes 4–24+ hours.

$$p_{g,\tau} \leq \overline{P}^{profile}(\tau - t^{start}) \quad
\text{for } \tau \in [t^{start}, t^{start} + T^{profile}]$$

### 3.4 Piecewise-Linear Heat Rates

PLEXOS uses incremental heat rate (IHR) with load points:

Given load points $\overline{P}_0 = P^{min}, \overline{P}_1, \ldots,
\overline{P}_K = P^{max}$ and incremental heat rates $h_1, \ldots, h_K$:

$$p_{g,t} = P^{min} \cdot u_{g,t} + \sum_{k=1}^{K} \delta_{k,t}$$

$$0 \leq \delta_{k,t} \leq (\overline{P}_k - \overline{P}_{k-1}) \cdot u_{g,t}$$

$$F_{g,t} = f^{min} \cdot u_{g,t} + \sum_{k} h_k \cdot \delta_{k,t}$$

Each band $\delta_{k,t}$ is bounded by the segment width times the
commitment binary, ensuring zero fuel consumption when offline.

Additional cost properties:

| Property | Description |
|----------|-------------|
| `No-Load Cost` | \$/hr fixed cost when committed (regardless of output) |
| `VO&M Charge` | Variable O&M, \$/MWh |
| `Fuel Offtake at Min Stable Level` | GJ/hr at minimum output |
| `Heat Rate` (band 1..N) | GJ/MWh per piecewise segment |

### 3.5 Combined Cycle Gas Turbine (CCGT) Configurations

This is the **single biggest gap** between open-source and commercial tools.
No open-source UC implementation models CCGT configuration tracking.

PLEXOS models CCGT via a dedicated `CCGT` object:

| Object / Property | Description |
|---|---|
| `CCGT.CT` (1..N) | Combustion turbine components |
| `CCGT.ST` | Steam turbine component |
| `CCGT.Configuration` (1..N) | Valid operating modes (1×1, 2×1, 3×1, etc.) |
| `Configuration.Max Capacity` | MW rating per configuration |
| `Configuration.Min Stable Level` | Per-configuration minimum output |
| `Configuration.Heat Rate` | Per-configuration efficiency |
| `Transition Cost` | Cost of switching configurations |
| `Transition Time` | Time required to switch |
| `Lead CT` | Which CT must start first |

**Mathematical formulation** (Morales-España, Correa-Posada, and Ramos,
2017 [[UC17]](#ref-UC17)):

Binary variables per mode: $z_{m,t}$ (plant in mode $m$ at time $t$),
$y_{m,t}$ (transition into mode $m$), $x_{m,t}$ (transition out):

$$\sum_{m \in \mathcal{M}} z_{m,t} \leq 1 \quad \text{(mode exclusivity)}$$

$$z_{m,t} - z_{m,t-1} = y_{m,t} - x_{m,t} \quad \forall m, t$$

$$y_{m,t} \leq \sum_{m' \in \mathcal{T}_m^{from}} x_{m',t}
 + \left(1 - \sum_{m'} z_{m',t-1}\right) \quad
\text{(allowed transitions)}$$

Power output depends on active configuration:

$$p_t = \sum_m (P_m^{min} \cdot z_{m,t} + \hat{p}_{m,t}), \quad
0 \leq \hat{p}_{m,t} \leq (P_m^{max} - P_m^{min}) \cdot z_{m,t}$$

Startup sequences: CT hot start 0.5–2h, CT warm 2–4h, CT cold 4–8h;
ST requires CT exhaust heat, 2–6h after CT online. Duct firing adds
a steeper cost segment ($c_{duct} > c_2 > c_1$).

### 3.6 Reserve and Ancillary Service Co-optimization

| Property | Description |
|----------|-------------|
| `Spinning Reserve Provision` | MW of spinning reserve capacity |
| `Max Reserve Provision` | Upper limit on reserve from this unit |
| `Reserve Offer Price` | \$/MW price for providing reserve |
| `Reserve Ramp Up` | Ramp rate for reserve deployment |
| `Reserve Category` | Reserve class (Regulation, Contingency, etc.) |
| `Min Generation for Reserve` | Must be at min stable to provide spinning |

Key interaction — a unit must be committed to provide spinning reserve:

$$r_{g,t} \leq (\overline{P}_g - p_{g,t}) \cdot u_{g,t}$$

$$p_{g,t} - r^{dn}_{g,t} \geq \underline{P}_g \cdot u_{g,t}$$

$$\sum_g r^{up}_{g,t} \geq R^{req}_t$$

Reserve requirements can be **dynamically computed** from renewable forecast
error statistics: $R^{req}_t = \alpha \cdot \sigma^{forecast}_t$.

### 3.7 Frequency, Inertia, and System Strength

PLEXOS (v8.x+) supports:

**System inertia constraint:**

$$\sum_g H_g \cdot S_g \cdot u_{g,t} \geq H^{min}_{sys}$$

**RoCoF constraint** (linearized):

$$\sum_g H_g \cdot S_g \cdot u_{g,t}
\geq \frac{f_0 \cdot \Delta P^{max}}{2 \cdot RoCoF^{max}}$$

**Minimum synchronous generation:**

$$\sum_{g \in \mathcal{G}^{sync}} u_{g,t} \geq N^{min}_{sync}$$

**SNSP limit** (System Non-Synchronous Penetration):

$$\frac{\sum_{r \in NSP} p_{r,t} + P_{import,t}}{D_t + P_{export,t}}
\leq SNSP_{max}$$

These are often the **binding constraint** forcing thermal commitment in
high-renewable scenarios (EirGrid SNSP limit, AEMO inertia requirements).

### 3.8 Forced Outage and Maintenance

| Property | Description |
|----------|-------------|
| `Forced Outage Rate` (FOR) | Probability of forced outage |
| `Mean Time to Repair/Fail` | MTTR / MTTF |
| `Forced Outage Rate (Band)` | Partial derating |
| `Maintenance Rate/Weeks` | Scheduled maintenance |
| `Maintenance Class` | Coordinated maintenance groups |
| `Outage Pattern` | External outage schedule |
| `Units Out` | Time-series forced outages |

PLEXOS models outages via Monte Carlo in PASA (Projected Assessment of
System Adequacy), propagating stochastic outages into UC.

### 3.9 Synchronous Condenser Mode

| Property | Description |
|----------|-------------|
| `Synchronous Condenser` | Flag: unit can operate as sync condenser |
| `Synchronous Condenser Cost` | \$/hr operating cost in condenser mode |
| `Synchronous Condenser MVAR` | Reactive power in condenser mode |
| `Synchronous Condenser Inertia` | Inertia contribution in condenser mode |

This introduces a **fourth commitment state**: OFF / GENERATING / PUMPING /
CONDENSER. In condenser mode, the unit provides inertia and reactive power
but no active power, consuming a small parasitic load.

**Formulation** — additional binary $s_{i,t}$:

$$u_{i,t} + s_{i,t} \leq 1 \quad \text{(mutually exclusive)}$$

Inertia constraint includes both generating and condenser modes:

$$\sum_i (u_{i,t} + s_{i,t}) \cdot H_i \cdot S_i \geq E^{min}_{kin}(t)$$

Relevant for: retiring coal plants converted to synchronous condensers,
gas turbines maintaining grid stability during high-renewable periods.

### 3.10 Emission Constraints During Startup

Startups produce disproportionate emissions because SCR catalysts are below
operating temperature, combustion is incomplete, and efficiency is 2–5×
worse than steady-state.

| Pollutant | Steady-State | Per Startup | Reason |
|-----------|-------------|-------------|--------|
| NOx (GT) | 0.1–0.5 lb/MWh | 50–200 lb/start | SCR not at temperature |
| CO (GT) | 0.1–0.3 lb/MWh | 100–500 lb/start | Incomplete combustion |
| SO2 (coal) | 1–5 lb/MWh | 200–1000 lb/start | FGD not operational |
| CO2 (CCGT) | ~0.4 t/MWh | 10–50 t/start | Reduced efficiency |

| Property | Description |
|----------|-------------|
| `Start Fuel Offtake` | GJ consumed during startup |
| `Start Emission` | kg per startup (by pollutant type) |
| `Max Emissions` | Cap on total emissions per period |
| `Emission Price` | Carbon price applied to all emissions |

**Formulation:**

$$E_{total} = \sum_i \sum_t
\left[e_i \cdot p_{i,t} + e_i^{SU} \cdot v_{i,t} + e_i^{SD} \cdot w_{i,t}
\right] \leq E^{max}$$

With hot/warm/cold emission rates:

$$E_i^{SU}(t) = e_i^{hot} \cdot v_{i,t}^{hot}
 + e_i^{warm} \cdot v_{i,t}^{warm}
 + e_i^{cold} \cdot v_{i,t}^{cold}$$

### 3.11 Known Criticisms of PLEXOS UC

Based on academic comparison studies and user community feedback:

**Formulation tightness**: PLEXOS historically used big-M formulations.
Knueven et al. (2020) showed the "Compact" formulation used by many
commercial tools is significantly weaker than state-of-art tight
formulations [[UC5]](#ref-UC5). The Morales-España TaC formulation
dominates alternatives [[UC15]](#ref-UC15).

**Scalability**: For 1000+ generators × 8760 hours × N-1 contingencies,
solve times range from hours to days. Rolling-horizon introduces boundary
effects. Stochastic UC is limited to small systems or few scenarios.

**Transparency**: Closed-source — exact formulation, preprocessing, and
heuristics are not publicly documented. Reproducibility concerns have driven
the movement toward open-source alternatives (PyPSA, GenX, SIENNA)
[[UC20]](#ref-UC20).

**Specific complaints**:
- Startup cost double-counting between `Start Cost` and `No-Load Cost`
- CCGT configuration model extremely difficult to set up correctly
- Reserve prices too high → phantom commitments solely for reserves
- Data-intensive: multi-band startup costs, profiles, outage rates often
  unavailable, especially for developing-country systems
- Memory: large SCUC can require 64–128 GB RAM
- MIP gap tolerance (0.01–0.1%) produces non-deterministic results
- Numerical issues when big-M values are large (phantom commitments at 0.9999)

---

## 4. Open-Source Tool Comparison

### 4.1 PyPSA

PyPSA implements UC via `committable=True` on generators, creating binary
`status`, `start_up`, `shut_down` variables per snapshot.

**Strengths:**
- Network-constrained UC (DC power flow with KVL)
- Sector coupling (heating, transport, hydrogen)
- Transparent, modifiable formulation

**Limitations:**
- **No hot/warm/cold startup costs** — single fixed cost only
- **No piecewise-linear heat rates** natively
- **No CCGT configurations**
- **No frequency/inertia constraints** built-in
- **No native reserve products** (users add custom constraints)
- **No stochastic UC** (no scenario tree)
- **No rolling horizon** built-in
- Performance degrades with many committable units (>100)
- No security-constrained UC (contingency-specific commitment)

### 4.2 GenX

GenX uses clustered integer commitment (Palmintier) [[UC12]](#ref-UC12) —
integer variables count committed units in a cluster, not per-unit binaries.

**Strengths:**
- 10–100× fewer integer variables than full UC
- Capacity expansion + UC simultaneously
- Piecewise fuel consumption curves
- Long-duration storage inter-period linking
- Multi-fuel generators
- Hydrogen electrolyzer flexibility

**Limitations:**
- **Hourly only** (no sub-hourly)
- **No CCGT configurations**
- **No AC/DC power flow** (transport model)
- **No frequency/inertia constraints**
- Clustering assumes identical units (error when units differ)
- Representative-period approach can miss extreme events
- No transmission switching or SCOPF

### 4.3 Feature Comparison Matrix

| Feature | PLEXOS | PyPSA | GenX | **gtopt (proposed)** |
|---------|--------|-------|------|---------------------|
| Binary on/off | ✓ | ✓ | ✓ (integer) | ✓ Phase 1 |
| Startup/shutdown variables | ✓ | ✓ | ✓ | ✓ Phase 1 |
| **Tight MIP formulation** | ? (proprietary) | basic | basic | **✓ Phase 1 (TaC)** |
| Startup cost | ✓ | ✓ | ✓ | ✓ Phase 1 |
| Shutdown cost | ✓ | ✓ | ✗ | ✓ Phase 1 |
| **Hot/warm/cold startup** | ✓ | ✗ | partial | **✓ Phase 3** |
| **Startup profiles** | ✓ | ✗ | ✗ | **✓ Phase 2** |
| Ramp up/down | ✓ | ✓ | ✓ | ✓ Phase 2 |
| **Startup/shutdown ramp** | ✓ | ✗ | ✗ | **✓ Phase 2** |
| Min up time | ✓ | ✓ | ✓ | ✓ Phase 3 |
| Min down time | ✓ | ✓ | ✓ | ✓ Phase 3 |
| No-load cost | ✓ | ✗ | ✗ | ✓ Phase 1 |
| **Piecewise heat rate** | ✓ | ✗ | ✓ | **✓ Phase 3** |
| Initial conditions | ✓ | ✓ | ✓ | ✓ Phase 1 |
| LP relaxation mode | ✓ | ✗ | ✓ | ✓ Phase 1 |
| Must-run / must-not-run | ✓ | partial | ✗ | ✓ Phase 1 |
| **Inertia constraint** | ✓ (v8+) | ✗ | ✗ | **✓ Phase 4** |
| **RoCoF constraint** | ✓ (v8+) | ✗ | ✗ | **✓ Phase 4** |
| **Frequency nadir** | partial | ✗ | ✗ | **✓ Phase 4** |
| **System strength (SCR)** | partial | ✗ | ✗ | **✓ Phase 4** |
| **Sync condenser mode** | ✓ | ✗ | ✗ | **✓ Phase 4** |
| **Grid-forming inverters** | partial | ✗ | ✗ | **✓ Phase 4** |
| **CCGT configurations** | ✓ | ✗ | ✗ | **✓ Phase 5** |
| Reserve co-optimization | ✓ | ✗ (custom) | basic | ✓ Phase 4 |
| **Battery commitment states** | partial | ✗ | ✗ | **✓ Phase 6** |
| **Pumped hydro modes** | ✓ | ✗ | ✗ | **✓ Phase 6** |
| **Hybrid plant commit** | partial | ✗ | ✗ | **✓ Phase 6** |
| **Degradation-aware** | recent | ✗ | ✗ | **✓ Phase 6** |
| **Electrolyzer commit** | partial | ✗ | basic | **✓ Phase 6** |
| **Startup emissions** | ✓ | ✗ | ✗ | **✓ Phase 7** |
| **Emission cost ($/tCO2)** | ✓ | ✓ | ✓ | **✓ Phase 1** |
| **Emission cap (tCO2/year)** | ✓ | ✓ | ✓ | **✓ Phase 1** |
| **Emission factor (tCO2/MWh)** | ✓ | ✓ | ✓ | **✓ Phase 1** |
| Stochastic UC | ✓ | ✗ | ✗ | future |
| Integer clustering | ✓ | ✗ | ✓ | Phase 7 |
| Security-constrained UC | ✓ | ✗ | ✗ | future |
| Integration with GTEP | ✓ | partial | ✓ | **✓ native** |

### 4.4 Competitive Gap Analysis

**Major gaps in ALL open-source tools** (gtopt would be first to close):

1. **Tight MIP formulation** — commercial tools may not use it either, but
   gtopt can gain a measurable advantage in LP relaxation quality and
   solve time by implementing TaC [[UC15]](#ref-UC15)
2. **CCGT configuration tracking** — the most data-intensive gap; highly
   valued by ISO/utility users
3. **Frequency/inertia constraints** — critical for Ireland, Australia, UK,
   Chile, and increasingly all systems >40% renewable penetration
4. **Battery/storage commitment states** — three-mode (idle/charge/discharge)
   with degradation and mode-switching constraints
5. **Startup emissions** — growing regulatory importance (EPA SSM rules,
   EU IED, California SCAQMD)

---

## 5. Literature Review

### 5.1 Classical UC and Tight Formulations

The standard three-variable UC formulation introduces per generator $g$,
time $t$:

| Variable | Type | Description |
|----------|------|-------------|
| $u_{g,t}$ | Binary | 1 if generator ON at time $t$ |
| $v_{g,t}$ | Binary | 1 if generator starts up at $t$ |
| $w_{g,t}$ | Binary | 1 if generator shuts down at $t$ |

**Logical constraint:**

$$u_{g,t} - u_{g,t-1} = v_{g,t} - w_{g,t} \quad \forall g, t > 1$$

**Morales-España et al. (2013, 2015)** developed **tight-and-compact (TaC)**
formulations [[UC1]](#ref-UC1), [[UC2]](#ref-UC2) providing the tightest
known LP relaxation:

Tight ramp constraints:

$$p_{g,t} - p_{g,t-1} \leq RU_g \cdot u_{g,t-1} + SU_g \cdot v_{g,t}$$

$$p_{g,t-1} - p_{g,t} \leq RD_g \cdot u_{g,t} + SD_g \cdot w_{g,t}$$

Tight min up/down time (aggregated form):

$$\sum_{\tau=t}^{t+UT_g-1} u_{g,\tau} \geq UT_g \cdot v_{g,t}$$

$$\sum_{\tau=t}^{t+DT_g-1} (1 - u_{g,\tau}) \geq DT_g \cdot w_{g,t}$$

**Tejada-Arango et al. (2020)** [[UC15]](#ref-UC15) confirmed TaC dominates
all alternatives in a systematic comparison.

**Knueven et al. (2020)** [[UC5]](#ref-UC5) provided the definitive survey
showing that the "Compact" formulation used by many commercial tools is
significantly weaker.

### 5.2 Stochastic and Robust UC

**Two-stage stochastic UC**: commitment (here-and-now) + dispatch (recourse
per scenario). Key advances:

- Dvorkin et al. (2020): distributionally robust chance-constrained UC —
  ambiguity set around forecast distribution
- Bertsimas et al. (2013) [[UC13]](#ref-UC13): adaptive robust UC with
  column-and-constraint generation (C&CG)
- Zhai, Guan, Cheng (2022): distributionally robust UC with Wasserstein
  ambiguity set — less conservative than classical robust
- Lorca & Sun (2021): adaptive robust with uncertainty budget (Γ-robustness)

C&CG has largely displaced Benders decomposition for two-stage robust UC.

### 5.3 Machine-Learning-Assisted UC

The fastest-growing area in UC research:

**Warm-starting MIP:**
- Xavier, Qiu, Ahmed (2021): learn commitment patterns → fix 70–90% of
  binaries → 3–5× speedup
- Bertsimas & Stellato (2022): "voice of optimization" — ML predicts
  optimal solution for warm-start
- Chen & Tong (2023): GNN predicts commitment status → 3–5× speedup

**Variable/constraint screening:**
- Pineda et al. (2020): ML identifies "always on"/"always off" generators
- Falconer & Mones (2023): GNN predicts active constraints in SCUC →
  10–50× speedup at <0.1% optimality gap

**Practical challenges**: distribution shift when fleet changes, feasibility
guarantees require repair heuristics, training data generation is expensive.

### 5.4 Decomposition Methods

**Surrogate Lagrangian Relaxation (SLR)**: Bragin et al. (2015) — updates
multipliers without waiting for all subproblems. Applied to 1000+ unit
SCUC at ISO-NE [[UC14]](#ref-UC14).

**ADMM**: Mhanna, Mancarella (2022) — distributed SCUC by zones, suitable
for multi-area coordination.

**Column Generation**: Ghaddar, Anjos, Lodi (2022) — Dantzig-Wolfe
decomposition where generator subproblems generate commitment schedules.
Strong lower bounds; effective for complex individual constraints.

### 5.5 Frequency-Constrained UC

**Teng, Strbac et al. (2016)** [[UC7]](#ref-UC7): foundational
inertia/nadir-constrained UC formulation.

**Badesa et al. (2019)** [[UC8]](#ref-UC8): simultaneous scheduling of
inertia, PFR, and EFR with polyhedral approximation of the frequency
security region.

**Paturet et al. (2020)**: RoCoF-constrained UC for Irish system.

**Trovato et al. (2023)**: full SFR model in UC including governor deadband,
load damping, and inter-area oscillations.

See [Section 7](#7-system-stability-constraints) for detailed formulations.

### 5.6 Data-Driven Reserve Sizing

Moving beyond fixed reserve rules (3σ, largest contingency):

- Dvorkin (2020): endogenous reserve based on CVaR of imbalance cost
- Mieth & Dvorkin (2023): Wasserstein DRO reserve sizing
- Bienstock et al. (2024): joint optimization of reserve products with
  endogenous sizing — reserve is a variable, not a parameter
- Nosair & Bouffard (2024): operating reserve demand curves — price-
  responsive reserve replaces fixed requirements

---

## 6. Renewable and Storage Challenges

### 6.1 Solar Integration

**Duck curve**: High-solar systems have steep evening ramps (~15 GW in
3 hours for CAISO). UC must commit fast-start units for the ramp and
de-commit baseload during midday trough [[UC18]](#ref-UC18).

**Cloud transients**: 5-minute ramps of 50–70% of installed PV capacity.
Hourly UC misses these; geographic smoothing reduces aggregate variability
but does not eliminate it.

**Minimum generation floor**: committed thermal at min stable + must-run
renewables > demand → negative prices. UC decides whether cycling cost
or curtailment cost is cheaper:

$$\sum_g \underline{P}_g \cdot u_{g,t} + \sum_r p_{r,t}
 + p^{curt}_t = D_t$$

### 6.2 Wind Forecast Uncertainty

Wind forecast errors are **not Gaussian** — heavy-tailed, skewed.
Error magnitude: 4–8% RMSE at 1h ahead, 15–25% at day-ahead.
Spatial correlation means simple reserve summation overestimates needs.

Reserve dimensioning: IEA Wind Task 25 recommends additional reserves of
3–6% of installed wind capacity at day-ahead timescale [[UC19]](#ref-UC19).

### 6.3 Curtailment Optimization

Curtailment is optimal when its marginal cost < de-commitment cost
(including future re-startup). Modern markets increasingly allow
market-based curtailment rather than mandatory priority dispatch.

### 6.4 Battery Degradation-Aware Commitment

**Throughput-based degradation** (most practical for UC):

$$C^{deg}_t = c^{cycle} \cdot \frac{p^{dis}_t \cdot \Delta t}{E^{max}}$$

Added to objective function. Typical degradation adder: \$5–25/MWh
depending on chemistry (LFP lower, NMC higher).

**DoD constraints** (simpler alternative):

$$SOC_{min} \leq SOC_t \leq SOC_{max} \quad \text{(e.g., 10\%–90\%)}$$

$$E^{throughput}_{daily} \leq E^{max}_{daily}$$

Exact cycle counting (rainflow) is path-dependent and non-convex [[UC21]](#ref-UC21).

### 6.5 Hybrid Plant Commitment

Co-located solar+storage or wind+storage behind shared interconnection:

$$p_{solar,t} + p^{dis}_{batt,t} - p^{ch}_{batt,t} \leq P_{POI}$$

ITC/PTC constraints: battery must charge primarily from co-located
renewable. Clipping recovery: battery captures inverter-clipped energy.

Hybrid commitment adds 10–20% revenue vs. independent operation
(O'Shaughnessy et al., 2021, NREL).

### 6.6 Pumped Hydro Mode Switching

Classic **three-mode commitment**:

$$z^{gen}_t + z^{pump}_t + z^{idle}_t = 1
\quad \text{(binary, mutually exclusive)}$$

$$p^{gen}_t \leq P^{max}_{gen} \cdot z^{gen}_t, \quad
p^{pump}_t \leq P^{max}_{pump} \cdot z^{pump}_t$$

$$SOC_t = SOC_{t-1} + \eta^{pump} \cdot p^{pump}_t
 - p^{gen}_t / \eta^{gen}$$

With mode transition constraints: minimum time in each mode, transition
time (5–30 min), synchronous condenser as fourth mode.

Variable-speed pumped hydro can operate at partial load in pumping mode
(unlike fixed-speed), requiring continuous bounds in pump mode.

### 6.7 Hydrogen Electrolyzer Commitment

Electrolyzers have UC-like constraints [[UC22]](#ref-UC22):

$$P^{min}_{elec} \cdot u^{elec}_t \leq p^{elec}_t
 \leq P^{max}_{elec} \cdot u^{elec}_t$$

$$H2_t = H2_{t-1} + \eta_{elec} \cdot p^{elec}_t - D^{H2}_t$$

Technology-dependent constraints:
- **PEM**: fast ramp (seconds), no minimum run time → low commitment complexity
- **Alkaline**: minutes to hours ramp, thermal warm-up → similar to thermal UC
- **SOEC**: hours to start, very high efficiency but fragile → cold start must
  be avoided; min up time 8–24h

IEA (2023) quantifies: flexible operation reduces hydrogen LCOE by 20–40%
vs. baseload.

### 6.8 Long-Duration Storage

100+ hour storage (compressed air, hydrogen caverns, iron-air batteries):
- Very slow ramp rates; commit days in advance
- Limited cycling (few hundred cycles over lifetime for iron-air)
- Inter-temporal coupling: seasonal strategy affects weekly UC
- Typical decomposition: medium-term model provides energy budget → UC
  receives storage constraints as boundary conditions

Sepulveda et al. (2021) [[UC23]](#ref-UC23): long-duration storage reduces
thermal commitment but requires long-horizon UC planning.

### 6.9 Emission Cost and Emission Cap

Emission constraints are fundamental to modern generation planning and interact
directly with unit commitment decisions. The standard approach in all major
tools (PLEXOS, PyPSA, GenX) uses three components:

**1. Emission factor per generator** ($e_g$, tCO2/MWh):

A stage-schedule-dependent optional field on `Generator`. Can be constant
or vary by stage (to model fuel switching, hydrogen blending, or improving
efficiency over time). Heat-rate-based computation: $e_g = HR_g \cdot ef_f$
where $HR_g$ is heat rate (GJ/MWh) and $ef_f$ is fuel emission intensity
(tCO2/GJ). Typical values: natural gas CCGT ≈ 0.36 tCO2/MWh, coal ≈ 0.9,
oil ≈ 0.65. Renewables, nuclear, and storage have $e_g = 0$.

**2. Emission cost** ($\tau_s$, \$/tCO2):

A system-level stage-schedule field representing the carbon price. Added to
each generator's dispatch cost as $\tau_s \cdot e_g$, making the effective
variable cost:

$$c_g^{eff}(s) = c_g^{gen} + \tau_s \cdot e_g$$

This is the approach used by all major tools:
- **PLEXOS**: `Emission.Price` on Emission objects, time-indexed
- **PyPSA**: `GlobalConstraint` with `carrier_attribute = "co2_emissions"`
- **GenX**: `CO2Price` parameter or endogenous via cap dual

Time-varying carbon prices handle ETS forecasts, escalating carbon taxes,
or scenario analysis.

**3. Emission cap** ($E^{cap}_s$, tCO2/year or GtCO2/year):

An optional system-level constraint bounding total emissions per stage (year):

$$\sum_{t \in \mathcal{T}_s} \sum_g
\left[e_{g,s} \cdot p_{g,t} \cdot \Delta_t
 + e^{SU}_{g} \cdot v_{g,t}\right]
\leq E^{cap}_s \quad (\lambda_s)$$

The **dual variable** $\lambda_s$ of this constraint is the **endogenous
carbon price** — the shadow price of the emission budget. At optimality,
the effective carbon price seen by dispatch is $\tau_s + \lambda_s$. If
$\tau_s = 0$ (pure cap-and-trade), $\lambda_s$ alone drives abatement.

**Interaction with UC startup emissions**: When $e^{SU}_g \cdot v_{g,t}$ is
included in the cap constraint, the carbon cost of starting a unit becomes:

$$C^{SU,CO2}_g = (\tau_s + \lambda_s) \cdot e^{SU}_g$$

This makes the commitment optimizer internalize the emission cost of each
startup event, discouraging unnecessary cycling of high-emission units.
Omitting startup emissions underestimates total emissions by 3–8% for gas
peakers [[UC32]](#ref-UC32).

**Key references:**
- Hobbs (1995): foundational LP with emission constraints
- Palmintier & Webster (2016): interaction of caps with investment decisions
- Morales-España et al. (2013): startup emission accounting in tight UC
- Sepulveda et al. (2018): emission cap + capacity expansion (GenX)

---

## 7. System Stability Constraints

These constraints are **the most impactful gap** in open-source UC tools.
They increasingly determine commitment decisions in high-renewable systems.

### 7.1 Inertia-Constrained UC

System inertia (kinetic energy in rotating masses) determines initial
frequency decline after a disturbance. The swing equation:

$$2H_{sys} \frac{df}{dt} = P_{mech} - P_{elec}$$

Total kinetic energy:

$$E_{kin} = \sum_{i \in \mathcal{G}_{on}} H_i \cdot S_i^{nom}$$

**Minimum inertia constraint** (Teng et al., 2016 [[UC7]](#ref-UC7)):

$$\sum_{i \in \mathcal{G}} u_{i,t} \cdot H_i \cdot S_i^{nom}
 \geq E^{min}_{kin}(t) \quad \forall t$$

where $E^{min}_{kin} = \frac{\Delta P_{max} \cdot f_0}{2 \cdot RoCoF_{max}}$.

This is **linear** in the binary commitment variables (product of binary ×
parameter). Key enabler: gtopt's existing LP framework handles this directly.

**Real-world requirements:**
- EirGrid (Ireland): minimum ~23,000 MWs for 460 MW largest contingency
- AEMO (Australia): region-specific, e.g., South Australia 2,150 MVA minimum
  fault level at Adelaide 275 kV

### 7.2 RoCoF Constraints

Rate of Change of Frequency at instant of disturbance:

$$\left.\frac{df}{dt}\right|_{t=0^+}
 = \frac{-\Delta P \cdot f_0}{2 \cdot E_{kin}}$$

Rearranging for a RoCoF limit:

$$E_{kin} \geq \frac{\Delta P_{max} \cdot f_0}{2 \cdot |RoCoF_{max}|}$$

**EirGrid trajectory**: 0.5 Hz/s (2012) → 1.0 Hz/s (2020). At 0.5 Hz/s
with 460 MW contingency: $E_{kin}^{min} = 23{,}000$ MWs — roughly double
the requirement at 1.0 Hz/s.

**SNSP constraint** (proxy for multiple stability concerns):

$$\frac{\sum_{r \in NSP} p_{r,t} + P_{import,t}}{D_t + P_{export,t}}
 \leq SNSP_{max}$$

EirGrid SNSP limit: 50% (2012) → 75% (2021) → target 95%.

### 7.3 Frequency Nadir Constraints

The nadir is the minimum frequency after a disturbance. The SFR (System
Frequency Response) model approximation (Ahmadi & Ghasemi, 2014
[[UC9]](#ref-UC9)):

$$|\Delta f_{nadir}| \approx
\frac{\Delta P_{loss}}{D_{load} + 1/R_{eq}}
\left(1 + \sqrt{\frac{T_R \cdot F_H}{2 H_{sys} \cdot D_{eff}}}\right)$$

This is nonlinear in inertia and PFR. **Linearization via polyhedral
approximation** (Badesa et al., 2019 [[UC8]](#ref-UC8)):

$$\alpha_k \sum_i u_{i,t} H_i S_i
 + \beta_k \sum_i r^{PFR}_{i,t}
 + \gamma_k \sum_j r^{FFR}_{j,t}
 \geq \delta_k \cdot \Delta P_{loss}
\quad \forall k \in \mathcal{K}, \forall t$$

Each linear cut $k$ is a supporting hyperplane of the nonlinear feasibility
region. Pre-computed offline from the SFR model parameters.

**Quasi-steady-state frequency** (linear in $u_{i,t}$):

$$\sum_i \frac{u_{i,t}}{R_i}
 \geq \frac{\Delta P_{loss}}{\Delta f^{max}_{qss}} - D_{load}$$

### 7.4 System Strength (Short-Circuit Ratio)

Minimum fault level at critical buses for IBR stability:

$$SCR_k = \frac{S^{fault}_k}{P^{IBR}_k} \geq SCR^{min}$$

where fault level depends on committed synchronous machines:

$$\sum_{i \in \mathcal{G}^{sync}_k}
\frac{u_{i,t} \cdot S^{nom}_i}{X''_{d,i}} \geq SCR_{min}
\cdot \sum_{j \in IBR_k} p_{j,t}$$

IEEE/NERC guidelines: SCR > 3 (strong), 2–3 (weak), < 2 (very weak —
IBR may fail to operate). ERCOT implemented "Weak Grid Operating
Procedures" after IBR oscillation events (2017–2018).

### 7.5 Grid-Forming Inverter Capabilities

GFM inverters emulate synchronous machine dynamics via Virtual Synchronous
Machine (VSM) control:

$$J_{virtual} \frac{d\omega}{dt}
 = P_{ref} - P_{meas} - D_p(\omega - \omega_0)$$

Equivalent inertia constant:
$H_{virtual} = J_{virtual} \omega_0^2 / (2 S_{rated})$, typically 2–10 s.

The inertia constraint generalizes:

$$\sum_{i \in \mathcal{G}^{sync}} u_{i,t} H_i S_i
 + \sum_{j \in \mathcal{G}^{GFM}} H^{virt}_j \cdot S_j \cdot \alpha_j(t)
 \geq E^{min}_{kin}(t)$$

where $\alpha_j(t) \in [0,1]$ depends on available headroom or SOC for
batteries.

**Operational experience:**
- **ERCOT**: >4 GW battery storage providing Fast Frequency Response (2023)
- **Australia**: Hornsdale "Big Battery" demonstrated sub-second FFR (2017);
  AEMO "Voluntary Spec for Grid-Forming Inverters" (2023)
- **UK**: National Grid "Stability Pathfinder" procured sync condensers and
  GFM BESS (2020–2023)

---

## 8. Current gtopt Architecture

### 8.1 Generator LP Formulation

The current `GeneratorLP` class creates:

1. **Generation variable** $p_{g,b}$ per block with bounds
   $[P^{min}_g, P^{max}_g]$
2. **Bus balance contribution**: $(1 - \lambda_g) \cdot p_{g,b}$ added to
   bus power balance row
3. **Capacity constraint**: $p_{g,b} \leq C^{inst}_g$ with expansion

```
GeneratorLP
  ├── CapacityObjectLP<Generator>
  │     ├── ObjectLP<Generator>
  │     └── CapacityObjectBase
  ├── pmin, pmax schedules
  ├── lossfactor, gcost schedules
  ├── generation_cols  (STBIndexHolder<ColIndex>)
  └── capacity_rows    (STBIndexHolder<RowIndex>)
```

Key: `generation_cols` are indexed by `(Scenario, Stage) → BIndexHolder`,
so each block already has its own LP variable — the exact structure needed
for inter-temporal UC constraints.

### 8.2 Time Structure (Stage/Block)

```
Stage (uid, first_block, count_block, discount_factor)
  └── Block[] (uid, duration in hours)
```

For UC: a stage must have **chronological blocks** where:
- Block durations are uniform (typically 1 hour)
- Blocks are time-consecutive
- Stage represents a specific calendar period

`StageLP` already provides `blocks()` and `duration()`.

### 8.3 MIP Infrastructure

gtopt already supports integer variables:

- `SparseCol::is_integer` flag
- `FlatLinearProblem::colint` collects integer indices
- `CapacityObjectLP` uses integers for expansion modules
- All solver backends (CBC, CPLEX, HiGHS) support MIP; CLP is LP-only
  (usable when UC is relaxed)

### 8.4 Component Patterns (AddToLP concept)

```cpp
template<typename T>
concept AddToLP = requires(T obj,
                           SystemContext& sc,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp,
                           OutputContext& output_context) {
  { obj.add_to_lp(sc, scenario, stage, lp) } -> std::same_as<bool>;
  { obj.add_to_output(output_context) } -> std::same_as<bool>;
};
```

`Commitment` will follow this pattern, placed **after** `GeneratorLP` in the
collections tuple since it references generator columns.

---

## 9. Proposed Implementation Design

### 9.0 Generator Emission Factor and System Emission Cost/Cap

These fields are **independent of UC** — they apply to all planning modes
(monolithic, SDDP, cascade) and do not require commitment variables. They
should be implemented as part of Phase 1 or even before UC.

**New optional field on `Generator`:**

```cpp
// In generator.hpp — new optional field
OptTRealFieldSched emission_factor {};  // tCO2/MWh, stage-schedule
```

**New optional fields on `System` (or `PlanningOptions`):**

```cpp
// System-level emission parameters
OptTRealFieldSched emission_cost {};  // $/tCO2, stage-schedule
OptTRealFieldSched emission_cap {};   // tCO2/year (or GtCO2/year), stage-schedule
```

**How it works in the LP:**

1. **Emission cost**: For each generator with `emission_factor > 0` and
   system `emission_cost > 0`, add $\tau_s \cdot e_g$ to the generation
   variable's objective coefficient. This is a simple modification in
   `GeneratorLP::add_to_lp` — no new variables or constraints needed.

2. **Emission cap**: If `emission_cap` is defined for a stage, add one
   constraint per stage:

   $$\sum_{t \in \mathcal{T}_s} \sum_g
   e_{g,s} \cdot p_{g,t} \cdot \Delta_t \leq E^{cap}_s$$

   With UC enabled, startup emissions are included:

   $$\sum_{t \in \mathcal{T}_s} \sum_g
   \left[e_{g,s} \cdot p_{g,t} \cdot \Delta_t
    + e^{SU}_g \cdot v_{g,t}\right] \leq E^{cap}_s$$

   The dual variable of this constraint is the endogenous carbon price.

3. **No cap → no constraint**: If `emission_cap` is not set, only the
   cost term in the objective applies. Setting both cost and cap gives
   a "tax + cap" hybrid where the effective carbon price is
   $\tau_s + \lambda_s$.

**JSON schema:**

```json
{
  "system": {
    "emission_cost": [10.0, 15.0, 20.0],
    "emission_cap": [50e6, 45e6, 40e6],
    "generator_array": [
      {
        "uid": 1, "name": "coal1", "bus": 1,
        "emission_factor": 0.9,
        "gcost": 25, "capacity": 500
      },
      {
        "uid": 2, "name": "gas_ccgt", "bus": 1,
        "emission_factor": 0.36,
        "gcost": 40, "capacity": 300
      },
      {
        "uid": 3, "name": "wind1", "bus": 1,
        "capacity": 200
      }
    ]
  }
}
```

In this example: wind has no `emission_factor` (defaults to 0), coal pays
$0.9 × \$10 = \$9/MWh$ carbon cost in stage 1, gas pays $0.36 × \$10 =
\$3.6/MWh$. The cap constrains total annual emissions to 50 MtCO2.

### 9.1 Commitment Element

```cpp
struct Commitment
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  // Generator reference
  SingleId generator {unknown_uid};

  // ── Cost parameters ──
  OptTRealFieldSched startup_cost {};    // $/start (default, or "hot")
  OptTRealFieldSched shutdown_cost {};   // $/stop
  OptReal noload_cost {};                // $/hr when committed, output-independent
  OptReal warm_startup_cost {};          // $/start when offline < warm_time
  OptReal cold_startup_cost {};          // $/start when offline > warm_time
  OptReal hot_startup_time {};           // hours: offline < this → hot start
  OptReal warm_startup_time {};          // hours: offline < this → warm start

  // ── Timing constraints ──
  OptReal min_up_time {};        // Minimum hours online after startup
  OptReal min_down_time {};      // Minimum hours offline after shutdown

  // ── Ramp rate constraints (MW/h) ──
  OptTBRealFieldSched ramp_up {};       // Max hourly increase when online
  OptTBRealFieldSched ramp_down {};     // Max hourly decrease when online
  OptReal startup_ramp {};              // Max output in first online period [MW]
  OptReal shutdown_ramp {};             // Max output in last online period [MW]

  // ── Startup profile (multi-period trajectory) ──
  OptTBRealFieldSched startup_profile {};  // MW upper bound trajectory
  OptReal startup_profile_time {};         // Hours of startup trajectory

  // ── Initial conditions ──
  OptReal initial_status {};     // 1.0 = online, 0.0 = offline
  OptReal initial_hours {};      // Hours in current state at stage start

  // ── Relaxation and mode ──
  OptBool relax {};              // true = LP relax (continuous [0,1])
  OptBool sync_condenser {};     // true = unit can operate as sync condenser
  OptReal sync_condenser_cost {}; // $/hr operating cost in condenser mode

  // ── Emission during startup ──
  OptReal startup_emission {};   // kg CO2 per startup event
  OptReal shutdown_emission {};  // kg CO2 per shutdown event

  // ── Frequency response ──
  OptReal inertia_constant {};   // H [seconds] — overrides generator default
  OptReal mva_rating {};         // S [MVA] — overrides generator default
  OptReal droop {};              // Governor droop R [p.u.]
  OptReal pfr_ramp_rate {};      // MW/s for primary frequency response
};
```

### 9.2 Frequency Security Element

```cpp
struct FrequencySecurity
{
  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};

  // ── Inertia and RoCoF ──
  OptReal min_inertia {};          // Minimum system kinetic energy [MWs]
  OptReal rocof_limit {};          // Maximum RoCoF [Hz/s]
  OptReal largest_contingency {};  // Largest credible loss [MW]
  OptReal nominal_frequency {};    // System frequency [Hz], default 50

  // ── Frequency nadir (polyhedral cuts) ──
  OptReal max_frequency_deviation {};  // Maximum allowed Δf [Hz]
  // Nadir cuts: α·H + β·PFR + γ·FFR ≥ δ·ΔP
  // Provided as schedule or computed from SFR model parameters

  // ── SNSP limit ──
  OptReal snsp_limit {};           // Maximum non-synchronous penetration [p.u.]

  // ── System strength ──
  OptReal min_scr {};              // Minimum SCR at critical buses
};
```

### 9.3 Mathematical Formulation

#### Sets and Indices

| Symbol | Description |
|--------|-------------|
| $g \in \mathcal{G}^{uc}$ | Generators with commitment constraints |
| $t \in \mathcal{T}_s$ | Blocks within chronological stage $s$ |
| $m \in \mathcal{M}_g$ | Operating modes for CCGT $g$ |

#### Decision Variables

| Variable | Type | Domain | Description |
|----------|------|--------|-------------|
| $p_{g,t}$ | Continuous | $[0, P^{max}_g]$ | Power output (existing) |
| $u_{g,t}$ | Binary | $\{0,1\}$ | Commitment status (online) |
| $v_{g,t}$ | Binary | $\{0,1\}$ | Startup indicator |
| $w_{g,t}$ | Binary | $\{0,1\}$ | Shutdown indicator |
| $s_{g,t}$ | Binary | $\{0,1\}$ | Sync condenser mode (Phase 4) |
| $v^{hot/warm/cold}_{g,t}$ | Binary | | Startup type indicators (Phase 3) |

#### Core Constraints (Phase 1)

**C1. Logical status transition** (three-bin linking):

$$u_{g,t} - u_{g,t-1} = v_{g,t} - w_{g,t} \quad \forall g, t > 1$$

For $t=1$: $u_{g,1} - u^0_g = v_{g,1} - w_{g,1}$.

**C2. Commitment-dependent generation limits:**

$$P^{min}_g \cdot u_{g,t} \leq p_{g,t} \leq P^{max}_g \cdot u_{g,t}$$

**C3. Startup/shutdown exclusion:**

$$v_{g,t} + w_{g,t} \leq 1$$

#### Ramp Constraints (Phase 2, Tight Formulation)

**C4. Ramp up** (Morales-España tight form):

$$p_{g,t} - p_{g,t-1} \leq RU_g \cdot u_{g,t-1} + SU_g \cdot v_{g,t}$$

**C5. Ramp down:**

$$p_{g,t-1} - p_{g,t} \leq RD_g \cdot u_{g,t} + SD_g \cdot w_{g,t}$$

#### Min Up/Down Time (Phase 3, Tight Formulation)

**C6. Min up time** (aggregated indicator):

$$\sum_{\tau=t}^{\min(t+UT_g-1,T)} u_{g,\tau} \geq UT_g \cdot v_{g,t}$$

**C7. Min down time:**

$$\sum_{\tau=t}^{\min(t+DT_g-1,T)} (1-u_{g,\tau}) \geq DT_g \cdot w_{g,t}$$

With boundary correction at horizon end.

#### Hot/Warm/Cold Startup (Phase 3)

**C8. Startup type decomposition:**

$$v_{g,t} = v^{hot}_{g,t} + v^{warm}_{g,t} + v^{cold}_{g,t}$$

**C9. Hot start indicator** (offline ≤ $T^{hot}$ periods):

$$v^{hot}_{g,t} \leq \sum_{\tau=t-T^{hot}}^{t-1} w_{g,\tau}$$

**C10. Warm start indicator:**

$$v^{warm}_{g,t} \leq \sum_{\tau=t-T^{warm}}^{t-T^{hot}-1} w_{g,\tau}$$

**Objective:**

$$\sum_t \left[c^{hot} v^{hot}_{g,t} + c^{warm} v^{warm}_{g,t}
 + c^{cold} v^{cold}_{g,t}\right]$$

#### Frequency Security (Phase 4)

**C11. Minimum inertia:**

$$\sum_{i \in \mathcal{G}^{sync}} (u_{i,t} + s_{i,t}) H_i S_i
 + \sum_{j \in \mathcal{G}^{GFM}} H^{virt}_j S_j \alpha_j(t)
 \geq E^{min}_{kin}$$

**C12. Frequency nadir cuts:**

$$\alpha_k \sum_i u_{i,t} H_i S_i
 + \beta_k \sum_i r^{PFR}_{i,t}
 + \gamma_k \sum_j r^{FFR}_{j,t}
 \geq \delta_k \cdot \Delta P_{loss}
\quad \forall k$$

**C13. SNSP limit:**

$$\sum_{r \in NSP} p_{r,t} + P_{import,t}
 \leq SNSP_{max} \cdot (D_t + P_{export,t})$$

**C14. System strength:**

$$\sum_{i \in \mathcal{G}^{sync}_k}
\frac{(u_{i,t}+s_{i,t}) S^{nom}_i}{X''_{d,i}}
 \geq SCR_{min} \cdot \sum_{j \in IBR_k} p_{j,t}$$

#### Objective Function

$$\min \sum_{g,t} \delta_s \pi_s \left[
(c^{gen}_g + \tau_s \cdot e_g) \cdot p_{g,t}
+ c^{NL}_g u_{g,t}
+ (c^{SU}_g + \tau_s \cdot e^{SU}_g) \cdot v_{g,t}
+ (c^{SD}_g + \tau_s \cdot e^{SD}_g) \cdot w_{g,t}
+ c^{SC}_g s_{g,t}
\right]$$

where $\tau_s$ is the system emission cost (\$/tCO2) at stage $s$, $e_g$ is
the generator emission factor (tCO2/MWh), and $e^{SU}_g, e^{SD}_g$ are
startup/shutdown emissions (tCO2/event).

#### Emission Cap Constraint

When `emission_cap` is defined for stage $s$:

$$\sum_{t \in \mathcal{T}_s} \sum_g
\left[e_g \cdot p_{g,t} \cdot \Delta_t
 + e^{SU}_g \cdot v_{g,t}
 + e^{SD}_g \cdot w_{g,t}\right]
\leq E^{cap}_s \quad (\lambda_s)$$

The dual variable $\lambda_s$ is the endogenous carbon price.

### 9.4 JSON Input Schema

```json
{
  "system": {
    "emission_cost": [10.0, 15.0, 20.0],
    "emission_cap": [50e6, 45e6, 40e6],
    "generator_array": [
      {
        "uid": 1, "name": "coal_unit", "bus": 1,
        "emission_factor": 0.9,
        "gcost": 25, "pmin": 100, "capacity": 500
      },
      {
        "uid": 2, "name": "gas_peaker", "bus": 1,
        "emission_factor": 0.55,
        "gcost": 60, "capacity": 200
      },
      {
        "uid": 3, "name": "wind1", "bus": 1,
        "capacity": 300
      }
    ],
    "commitment_array": [
      {
        "uid": 1,
        "name": "coal_commit",
        "generator": "coal_unit",
        "startup_cost": 15000,
        "shutdown_cost": 5000,
        "noload_cost": 200,
        "hot_startup_time": 4,
        "warm_startup_time": 12,
        "warm_startup_cost": 25000,
        "cold_startup_cost": 45000,
        "min_up_time": 8,
        "min_down_time": 6,
        "ramp_up": 100,
        "ramp_down": 80,
        "startup_ramp": 150,
        "shutdown_ramp": 150,
        "initial_status": 1.0,
        "initial_hours": 10,
        "inertia_constant": 5.0,
        "mva_rating": 600,
        "droop": 0.04,
        "startup_emission": 50,
        "sync_condenser": true,
        "sync_condenser_cost": 15
      }
    ],
    "frequency_security_array": [
      {
        "uid": 1,
        "name": "system_freq",
        "min_inertia": 23000,
        "rocof_limit": 1.0,
        "largest_contingency": 460,
        "nominal_frequency": 50,
        "max_frequency_deviation": 0.8,
        "snsp_limit": 0.75
      }
    ]
  },
  "simulation": {
    "stage_array": [
      {
        "uid": 1,
        "first_block": 0,
        "count_block": 168,
        "chronological": true
      }
    ]
  }
}
```

In this example: coal at 0.9 tCO2/MWh pays \$9/MWh carbon cost in stage 1
(0.9 × \$10), gas pays \$5.5/MWh, wind pays nothing. Each startup of the
coal unit emits 50 tCO2, costing \$500 at the \$10/tCO2 carbon price.
Annual emissions are capped at 50 MtCO2 (stage 1), declining to 40 MtCO2.

### 9.5 C++ Implementation Plan

| File | Purpose |
|------|---------|
| `include/gtopt/commitment.hpp` | `Commitment` struct |
| `include/gtopt/commitment_lp.hpp` | `CommitmentLP` class |
| `source/commitment_lp.cpp` | `add_to_lp` implementation |
| `include/gtopt/frequency_security.hpp` | `FrequencySecurity` struct |
| `include/gtopt/frequency_security_lp.hpp` | `FrequencySecurityLP` class |
| `source/frequency_security_lp.cpp` | Inertia/RoCoF/nadir constraints |
| `test/source/test_commitment.cpp` | Unit tests |
| `test/source/test_frequency_security.cpp` | Frequency constraint tests |

**System integration:**
1. `generator.hpp`: Add `OptTRealFieldSched emission_factor {}`
2. `system.hpp`: Add `Array<Commitment>`, `Array<FrequencySecurity>`,
   `OptTRealFieldSched emission_cost {}`, `OptTRealFieldSched emission_cap {}`
3. `system_lp.hpp`: Add collections after `GeneratorLP`; add emission cap row
4. `generator_lp.cpp`: Add $\tau_s \cdot e_g$ to generation cost coefficient
5. `stage.hpp`: Add `OptBool chronological {}`
6. JSON parsing for all new fields and arrays

---

## 10. Implementation Phases

### Phase 1: Core Three-Bin UC

**Goal**: Binary commitment with startup/shutdown logic and costs.

**Deliverables:**
- [ ] `Commitment` struct and JSON parsing
- [ ] `CommitmentLP::add_to_lp` — constraints C1, C2, C3
- [ ] Startup/shutdown costs in objective
- [ ] No-load cost
- [ ] Initial conditions handling
- [ ] LP relaxation mode (`relax` flag)
- [ ] Must-run / must-not-run support
- [ ] `Stage::chronological` field
- [ ] `Generator::emission_factor` field (tCO2/MWh, stage-schedule)
- [ ] `System::emission_cost` field ($/tCO2, stage-schedule)
- [ ] `System::emission_cap` field (tCO2/year, stage-schedule)
- [ ] Emission cost adder in generator objective coefficients
- [ ] Emission cap constraint (one per stage, when defined)
- [ ] Output: `status_sol`, `startup_sol`, `shutdown_sol`
- [ ] Unit test: 3-generator UC dispatch
- [ ] Unit test: emission cost shifts dispatch from coal to gas
- [ ] Unit test: emission cap binding → endogenous carbon price

**Constraints per generator per stage** (N blocks):
3N variables (u, v, w) + 4N rows (C1, C2×2, C3) = **3N vars, 4N rows**

### Phase 2: Ramp Constraints and Startup Profiles

**Goal**: Inter-temporal ramp limits with tight formulation; multi-period
startup trajectories.

**Deliverables:**
- [ ] Ramp up/down (C4, C5) — tight form with startup/shutdown ramp
- [ ] Startup profile constraints (prescribed trajectory)
- [ ] Unit test: ramp-constrained dispatch

**Additional**: 2(N−1) ramp rows + startup profile rows

### Phase 3: Min Up/Down Time and Multi-Band Startup Costs

**Goal**: Temporal coupling constraints and cost tiers.

**Deliverables:**
- [ ] Min up/down time (C6, C7) — aggregated indicator form
- [ ] Horizon boundary handling
- [ ] Hot/warm/cold startup cost tiers (C8, C9, C10)
- [ ] Piecewise-linear heat rate integration
- [ ] Unit test: min time enforcement + cold vs. hot start

**Additional**: up to 2N min-time rows + 3 startup-type binaries per period

### Phase 4: Frequency Security and Inertia

**Goal**: System stability constraints — **major differentiator**.

**Deliverables:**
- [ ] `FrequencySecurity` struct and JSON parsing
- [ ] `FrequencySecurityLP::add_to_lp` — C11 (inertia), C12 (nadir), C13
  (SNSP), C14 (system strength)
- [ ] Synchronous condenser mode for generators (C11 extended)
- [ ] Grid-forming inverter inertia contribution
- [ ] Reserve co-optimization (spinning, PFR, FFR headroom)
- [ ] Unit test: inertia-constrained dispatch with renewables

**Additional**: O(1) system-wide constraints per time step + per-generator
reserve rows

### Phase 5: CCGT Configurations

**Goal**: Configuration-based combined-cycle modeling — **major differentiator**.

**Deliverables:**
- [ ] `CCGTConfiguration` struct with mode enumeration
- [ ] Mode exclusivity, transition feasibility matrix
- [ ] Per-configuration min/max, heat rate, ramp
- [ ] Transition costs and times
- [ ] Duct firing as additional cost segment
- [ ] Unit test: 2×1 CCGT with mode switching

### Phase 6: Storage Commitment and Hybrid Plants

**Goal**: UC for non-thermal assets.

**Deliverables:**
- [ ] Battery three-state commitment (idle/charge/discharge)
- [ ] Degradation cost in objective
- [ ] Pumped hydro mode switching (generate/pump/idle/condenser)
- [ ] Hybrid plant POI constraint
- [ ] Hydrogen electrolyzer commitment
- [ ] Unit tests per asset type

### Phase 7: Advanced Features

**Potential deliverables** (prioritize by user needs):
- [ ] Integer clustering (Palmintier — multiple identical units)
- [ ] Startup emission tracking per pollutant
- [ ] Cross-stage commitment state carryover (via `StateVariable`)
- [ ] `element_column_resolver` for user constraints on commitment vars
- [ ] ML warm-start integration hooks
- [ ] Stochastic UC (scenario trees)
- [ ] Rolling horizon support

---

## 11. Testing Strategy

### Unit Tests (per phase)

**Phase 1: Basic UC dispatch**
```
2-generator, 4-block chronological stage
g1: cheap thermal with commitment (pmin=100, pmax=500, startup_cost=10000)
g2: expensive peaker (no commitment, pmax=200)
Demand: [150, 400, 300, 100]
Expected: g1 stays on blocks 1-3, off block 4 (demand < pmin)
Verify: u, v, w variables; objective includes startup costs
```

**Phase 2: Ramp-limited dispatch**
```
1-generator, 6-block, ramp_up=50 MW/h, ramp_down=50 MW/h
Demand: [100, 200, 350, 400, 300, 100]
Verify: consecutive generation changes ≤ 50 MW
```

**Phase 3: Min up/down time + hot/warm/cold**
```
1-generator, 12-block, min_up=4h, min_down=3h
hot_cost=5000, warm_cost=15000, cold_cost=30000
Demand cycling pattern; verify min-time enforcement
Two shutdowns: first restart < hot_time → hot cost; second > warm_time → cold cost
```

**Phase 4: Inertia-constrained dispatch**
```
2 thermal + 1 wind, 24-block
min_inertia=5000 MWs, thermal H=5s, S=200 MVA
Wind covers 80% of demand → at least 1 thermal must stay committed for inertia
Verify: inertia floor forces commitment even when wind is cheaper
```

**Phase 5: CCGT configuration**
```
1 CCGT (2×1), 24-block
Modes: off, 1×0, 2×0, 1×1, 2×1 with different min/max/heat rates
Verify: transitions follow feasibility matrix; cost includes transition costs
```

**Phase 6: Battery commitment states**
```
1 thermal + 1 battery, 12-block
Battery: 100MW/400MWh, degradation cost $15/MWh
Verify: battery charge/discharge modes mutually exclusive; degradation in objective
```

### Integration Tests

- Existing IEEE cases with commitment on thermal generators
- Verify non-chronological stages unaffected
- Juan IPLP case with commitment enabled

### Regression Tests

All existing tests must pass unchanged — commitment is purely additive.

---

## 12. Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| MIP solve time too slow | High | LP relaxation; integer clustering (Phase 7); ML warm-start hooks |
| CBC insufficient for large MIPs | Medium | HiGHS/CPLEX already available; HiGHS has excellent MIP performance |
| Tight formulation complexity | Medium | TaC is well-documented with reference implementations |
| CCGT configuration combinatorics | Medium | Enumerate configurations offline; limit to practical modes |
| Frequency constraint nonlinearity | Medium | Polyhedral cuts pre-computed offline; linear in UC variables |
| Data requirements too heavy | Medium | All new fields optional; graceful degradation to simple UC |
| Integration with SDDP | Medium | Phase 1–6 are monolithic-only; SDDP integration deferred |
| pmin > 0 breaks offline | Low | C2 properly handles: bounds widen to [0, pmax], pmin enforced via constraint |
| Numerical scaling with binaries | Low | `scale_objective` already exists; binaries well-conditioned |
| Startup emission regulation | Low | Optional feature; only activated when emission data provided |

---

## 13. References

<a id="ref-UC1"></a>
**[UC1]** Morales-España, G., Gentile, C., Ramos, A. "Tight MIP formulations
of the power-based unit commitment problem." *OR Spectrum*, 37(4):929–950,
2015. [doi:10.1007/s00291-015-0400-4](https://doi.org/10.1007/s00291-015-0400-4)

<a id="ref-UC2"></a>
**[UC2]** Morales-España, G., Latorre, J.M., Ramos, A. "Tight and compact
MILP formulation for the thermal unit commitment problem." *IEEE Trans.
Power Systems*, 28(4):4897–4908, 2013.
[doi:10.1109/TPWRS.2013.2251373](https://doi.org/10.1109/TPWRS.2013.2251373)

<a id="ref-UC3"></a>
**[UC3]** Gentile, C., Morales-España, G., Ramos, A. "A tight MIP formulation
of the unit commitment problem with start-up and shut-down constraints."
*EURO J. Comp. Optimization*, 5(1–2):177–201, 2017.
[doi:10.1007/s13675-016-0066-y](https://doi.org/10.1007/s13675-016-0066-y)

<a id="ref-UC4"></a>
**[UC4]** Palmintier, B. "Incorporating operational flexibility into electric
generation planning." *PhD Thesis*, MIT, 2013.

<a id="ref-UC5"></a>
**[UC5]** Knueven, B., Ostrowski, J., Watson, J.-P. "On mixed-integer
programming formulations for the unit commitment problem." *INFORMS J.
Computing*, 32(4):857–876, 2020.
[doi:10.1287/ijoc.2019.0944](https://doi.org/10.1287/ijoc.2019.0944)

<a id="ref-UC6"></a>
**[UC6]** Rajan, D., Takriti, S. "Minimum up/down polytopes of the unit
commitment problem with start-up costs." *IBM Research Report RC23628*, 2005.

<a id="ref-UC7"></a>
**[UC7]** Teng, F., Trovato, V., Strbac, G. "Stochastic scheduling with
inertia-dependent fast frequency response requirements." *IEEE Trans.
Power Systems*, 31(2):1557–1566, 2016.
[doi:10.1109/TPWRS.2015.2434837](https://doi.org/10.1109/TPWRS.2015.2434837)

<a id="ref-UC8"></a>
**[UC8]** Badesa, L., Teng, F., Strbac, G. "Simultaneous scheduling of
multiple frequency services in stochastic unit commitment." *IEEE Trans.
Power Systems*, 34(5):3800–3812, 2019.
[doi:10.1109/TPWRS.2019.2905687](https://doi.org/10.1109/TPWRS.2019.2905687)

<a id="ref-UC9"></a>
**[UC9]** Ahmadi, H., Ghasemi, H. "Security-constrained unit commitment with
linearized system frequency limit constraints." *IEEE Trans. Power Systems*,
29(4):1536–1545, 2014.

<a id="ref-UC10"></a>
**[UC10]** Brown, T., Hörsch, J., Schlachtberger, D. "PyPSA: Python for Power
System Analysis." *J. Open Research Software*, 6(4), 2018.
[doi:10.5334/jors.188](https://doi.org/10.5334/jors.188)

<a id="ref-UC11"></a>
**[UC11]** Jenkins, J.D., Sepulveda, N.A. "Enhanced decision support for a
changing electricity landscape: the GenX configurable electricity resource
capacity expansion model." *MIT Energy Initiative Working Paper*, 2017.

<a id="ref-UC12"></a>
**[UC12]** Palmintier, B., Webster, M. "Heterogeneous unit clustering for
efficient operational flexibility modeling." *IEEE Trans. Power Systems*,
29(3):1089–1098, 2014.

<a id="ref-UC13"></a>
**[UC13]** Bertsimas, D., Litvinov, E., Sun, X.A., Zhao, J., Zheng, T.
"Adaptive robust optimization for the security constrained unit commitment
problem." *IEEE Trans. Power Systems*, 28(1):52–63, 2013.

<a id="ref-UC14"></a>
**[UC14]** Bragin, M., Luh, P., Yan, J., Yu, N., Stern, G. "Convergence of
the surrogate Lagrangian relaxation method." *J. Optimization Theory and
Applications*, 164(1):173–201, 2015.

<a id="ref-UC15"></a>
**[UC15]** Tejada-Arango, D.A., Dominguez, R., Wogrin, S., Centeno, E.
"Which unit commitment formulation is best? A comparison framework."
*IEEE Trans. Power Systems*, 35(4):2926–2936, 2020.

<a id="ref-UC16"></a>
**[UC16]** Silbernagl, M., Huber, M., Brandenberg, R. "Improving accuracy
and efficiency of start-up cost formulations in MIP unit commitment models."
*IEEE Trans. Power Systems*, 31(4):2578–2586, 2016.

<a id="ref-UC17"></a>
**[UC17]** Morales-España, G., Correa-Posada, C., Ramos, A. "Tight and
compact MIP formulation of configuration-based combined-cycle units."
*IEEE Trans. Power Systems*, 31(2):1350–1359, 2017.

<a id="ref-UC18"></a>
**[UC18]** Denholm, P., O'Connell, M., Brinkman, G., Jorgenson, J.
"Overgeneration from solar energy in California: a field guide to the duck
chart." *NREL Technical Report*, NREL/TP-6A20-65023, 2015.

<a id="ref-UC19"></a>
**[UC19]** Holttinen, H., et al. "Methodologies to determine operating
reserves due to increased wind power." *IEEE Trans. Sustainable Energy*,
3(4):713–723, 2012.

<a id="ref-UC20"></a>
**[UC20]** Ringkjob, H.-K., Haugan, P.M., Solbrekke, I.M. "A review of
modelling tools for energy and electricity systems with large shares of
variable renewables." *Renewable and Sustainable Energy Reviews*,
96:440–459, 2018.

<a id="ref-UC21"></a>
**[UC21]** Xu, B., et al. "Modeling of lithium-ion battery degradation
for cell life assessment." *Applied Energy*, 211:1096–1104, 2018.

<a id="ref-UC22"></a>
**[UC22]** Bødal, E.F., et al. "Flexible operation of power-to-hydrogen
plants." *Applied Energy*, 275:115377, 2020.

<a id="ref-UC23"></a>
**[UC23]** Sepulveda, N.A., Jenkins, J.D., Edington, A., Mallapragada, D.S.,
Lester, R.K. "The design space for long-duration energy storage in
decarbonized power systems." *Nature Energy*, 6:506–516, 2021.

<a id="ref-UC24"></a>
**[UC24]** Ostrowski, J., Anjos, M.F., Vannelli, A. "Tight mixed integer
linear programming formulations for the unit commitment problem."
*IEEE Trans. Power Systems*, 27(1):39–46, 2012.

<a id="ref-UC25"></a>
**[UC25]** Poncelet, K., et al. "Impact of the level of temporal and
operational detail in energy-system planning models." *Applied Energy*,
162:631–643, 2016.

<a id="ref-UC26"></a>
**[UC26]** Energy Exemplar. "PLEXOS Integrated Energy Model — Generator
Properties." Technical documentation. Available at
[energyexemplar.com](https://www.energyexemplar.com).

<a id="ref-UC27"></a>
**[UC27]** Buitrago Villada, J.D., Matus, M., et al. "FESOP: Fabulous Energy
System Optimizer." *IEEE Kansas Power and Energy Conference (KPEC)*, 2022.

<a id="ref-UC28"></a>
**[UC28]** Deane, J.P., Chiodi, A., Gargiulo, M., O Gallachoir, B.P.
"Soft-linking of a power systems model to an energy systems model."
*Energy*, 42(1):303–312, 2012.

<a id="ref-UC29"></a>
**[UC29]** Xavier, A.S., Qiu, F., Ahmed, S. "Learning to solve large-scale
security-constrained unit commitment problems." *INFORMS J. Computing*,
33(2):739–756, 2021.

<a id="ref-UC30"></a>
**[UC30]** Paturet, M., Markovic, U., Delikaraoglou, S., Vrettos, E.,
Aristidou, P., Hug, G. "Stochastic unit commitment in low-inertia grids."
*IEEE Trans. Power Systems*, 35(5):3448–3458, 2020.

<a id="ref-UC31"></a>
**[UC31]** Morales-España, G., Correa-Posada, C., Ramos, A.
"Tight and compact MIP formulation of configuration-based combined-cycle
units." *IEEE Trans. Power Systems*, 31(2):1350–1359, 2016.

<a id="ref-UC32"></a>
**[UC32]** Benato, A., Stoppato, A., Bracco, S. "Combined cycle power plants
start-up: estimation of NOx and CO emissions." *Applied Energy*,
178:58–68, 2016.

<a id="ref-UC33"></a>
**[UC33]** Matevosyan, J., et al. "Grid-forming inverters: are they the key
for high renewable penetration?" *IEEE Power and Energy Magazine*,
17(6):89–98, 2019.

<a id="ref-UC34"></a>
**[UC34]** Lin, Y., et al. "Research roadmap on grid-forming inverters."
*NREL Technical Report*, NREL/TP-5D00-73476, 2020.

---

## See Also

- [Mathematical Formulation](../formulation/mathematical-formulation.md)
- [Planning Guide](../planning-guide.md)
- [Input Data Reference](../input-data.md)
- [Planning Options](../planning-options.md)
- [Critical Review Report](critical-review-report.md)
