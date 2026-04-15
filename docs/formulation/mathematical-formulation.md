# Mathematical Formulation of the GTEP Optimization Problem

> **gtopt** solves a multi-stage, multi-scenario **Generation and Transmission
> Expansion Planning (GTEP)** problem formulated as a sparse linear program
> (LP/MIP). This document describes the complete mathematical formulation in
> detail.

> **Rendering note:** This document uses [GitHub Flavored Markdown LaTeX math](https://docs.github.com/en/get-started/writing-on-github/working-with-advanced-formatting/writing-mathematical-expressions).
> View it on GitHub or in a Markdown renderer with math support.
> In VS Code, install the **Markdown Preview Enhanced** extension and enable `markdown-preview-enhanced.enableEHTML`.
> All display formulas use `$$...$$` blocks and all inline symbols use `$...$` notation.

## Table of Contents

1. [Overview](#1-overview)
2. [Notation and Sets](#2-notation-and-sets)
3. [Compact Formulation](#3-compact-formulation)
4. [Objective Function](#4-objective-function)
5. [Constraints by Component](#5-constraints-by-component)
   - [5.1 Bus Power Balance](#51-bus-power-balance)
   - [5.2 Generator Constraints](#52-generator-constraints)
   - [5.3 Generator Profile Constraints](#53-generator-profile-constraints)
   - [5.4 Demand Constraints](#54-demand-constraints)
   - [5.4 bis Demand Profile Constraints](#54-bis-demand-profile-constraints)
   - [5.5 Transmission Line Constraints](#55-transmission-line-constraints)
   - [5.6 Kirchhoff Voltage Law (DC OPF)](#56-kirchhoff-voltage-law-dc-opf)
   - [5.7 Battery / Energy Storage Constraints](#57-battery--energy-storage-constraints)
   - [5.8 Converter Constraints](#58-converter-constraints)
   - [5.9 Reserve Constraints](#59-reserve-constraints)
   - [5.10 Hydro Cascade Constraints](#510-hydro-cascade-constraints)
   - [5.10 bis Hydro Pump](#510-bis-hydro-pump)
   - [5.10 ter Reservoir Discharge Limit](#510-ter-reservoir-discharge-limit)
   - [5.11 Capacity Expansion Constraints](#511-capacity-expansion-constraints)
   - [5.12 User Constraints](#512-user-constraints)
   - [5.13 Water Rights (FlowRight and VolumeRight)](#513-water-rights-flowright-and-volumeright)
   - [5.14 LNG Terminal](#514-lng-terminal)
   - [5.15 Unit Commitment (3-bin)](#515-unit-commitment-3-bin)
   - [5.16 Simple Commitment](#516-simple-commitment)
   - [5.17 Inertia Zone and Provision](#517-inertia-zone-and-provision)
6. [Scaling and Solver Options](#6-scaling-and-solver-options)
   - [6.1 Objective Scaling](#61-objective-scaling)
   - [6.2 Voltage Angle Scaling](#62-voltage-angle-scaling)
   - [6.3 Variable Scaling](#63-variable-scaling)
   - [6.4 Key Options Affecting the Formulation](#64-key-options-affecting-the-formulation)
   - [6.5 Modeling Modes Summary](#65-modeling-modes-summary)
   - [6.6 Stochastic Dual Dynamic Programming (SDDP)](#66-stochastic-dual-dynamic-programming-sddp)
   - [6.7 Apertures — Backward-Pass Scenario Sampling](#67-apertures--backward-pass-scenario-sampling)
   - [6.8 SDDP Benders Cuts](#68-sddp-benders-cuts)
   - [6.9 Boundary Cuts and Future Cost](#69-boundary-cuts-and-future-cost)
   - [6.10 Hot Start](#610-hot-start)
   - [6.11 Solver Timeouts](#611-solver-timeouts)
   - [6.12 Monolithic vs SDDP Equivalence](#612-monolithic-vs-sddp-equivalence)
   - [6.13 Elastic Filter and Feasibility](#613-elastic-filter-and-feasibility)
   - [6.14 Cascade Solver — Multi-Level Decomposition](#614-cascade-solver--multi-level-decomposition)
7. [Mapping: JSON Fields → Mathematical Symbols](#7-mapping-json-fields--mathematical-symbols)
8. [Cross-References](#8-cross-references)
9. [References](#9-references)

---

## 1. Overview

The GTEP problem finds the minimum-cost combination of:

- **Operational decisions (OPEX)** — how much each generator produces in every
  time block, which loads to curtail if capacity is insufficient, and how much
  power flows on each transmission line.
- **Investment decisions (CAPEX)** — how many capacity expansion modules to
  build for generators, demands, lines, and batteries across multi-year
  planning stages.

This is a classical problem in power system planning, extensively studied in
the literature [[1]](#ref1) [[2]](#ref2) [[3]](#ref3) [[16]](#ref16). The gtopt formulation
builds on the FESOP (Fabulous Energy System Optimizer) framework
[[4]](#ref4), which extended traditional long-term operational planning
(as implemented in PLP-type hydrothermal coordination tools [[5]](#ref5)
[[6]](#ref6)) to include capacity expansion, variable renewable energy
representation, and modern flexibility mechanisms such as battery storage
and spinning reserve constraints.

The problem is formulated as a **sparse linear program** (LP) over a
three-level time hierarchy:

$$
\text{Scenario} \;\supset\; \text{Stage} \;\supset\; \text{Block}
$$

- **Block** $b \in \mathcal{B}$ — smallest time unit (typically 1 hour) with
  duration $\Delta_b$ in hours.
- **Stage** $t \in \mathcal{T}$ — investment period grouping consecutive
  blocks. Capacity decisions at stage $t$ persist to subsequent stages.
- **Scenario** $s \in \mathcal{S}$ — a possible future realization (e.g.,
  hydrology, demand level) weighted by probability $\pi_s$.

The solver minimizes the total **expected discounted cost** across all
scenarios, stages, and blocks.

---

## 2. Notation and Sets

### Sets

| Symbol | Description |
|--------|-------------|
| $\mathcal{S}$ | Set of scenarios, indexed by $s$ |
| $\mathcal{T}$ | Set of stages (investment periods), indexed by $t$ |
| $\mathcal{B}_t$ | Set of blocks in stage $t$, indexed by $b$ |
| $\mathcal{N}$ | Set of buses (electrical nodes), indexed by $n$ |
| $\mathcal{G}$ | Set of generators, indexed by $g$ |
| $\mathcal{D}$ | Set of demands (loads), indexed by $d$ |
| $\mathcal{L}$ | Set of transmission lines, indexed by $l$ |
| $\mathcal{E}$ | Set of batteries (energy storage), indexed by $e$ |
| $\mathcal{V}$ | Set of converters, indexed by $v$ |
| $\mathcal{Z}$ | Set of reserve zones, indexed by $z$ |
| $\mathcal{P}$ | Set of reserve provisions, indexed by $p$ |
| $\mathcal{J}$ | Set of junctions (hydraulic nodes), indexed by $j$ |
| $\mathcal{W}$ | Set of waterways, indexed by $w$ |
| $\mathcal{R}$ | Set of reservoirs, indexed by $r$ |
| $\mathcal{U}$ | Set of turbines, indexed by $u$ |
| $\mathcal{F}$ | Set of fixed flows (inflows/releases), indexed by $f$ |
| $\mathcal{I}$ | Set of filtrations (seepage), indexed by $i$ |
| $\mathcal{H}$ | Set of hydro pumps, indexed by $h$ |
| $\mathcal{DL}$ | Set of reservoir discharge-limit elements, indexed by $\ell$ |
| $\mathcal{FR}$ | Set of flow rights (flow-based water rights), indexed by $i$ |
| $\mathcal{VR}$ | Set of volume rights (volume-based water rights), indexed by $o$ |
| $\mathcal{LNG}$ | Set of LNG terminals, indexed by $\tau$ |
| $\mathcal{UC}$ | Set of generators with 3-bin unit commitment, indexed by $g$ |
| $\mathcal{SC}$ | Set of generators with simple commitment, indexed by $g$ |
| $\mathcal{IZ}$ | Set of inertia zones, indexed by $z$ |
| $\mathcal{IP}$ | Set of inertia provisions, indexed by $p$ |
| $\mathcal{G}_n$ | Generators connected to bus $n$ |
| $\mathcal{D}_n$ | Demands connected to bus $n$ |
| $\mathcal{L}_n^+$ | Lines where bus $n$ is the receiving end |
| $\mathcal{L}_n^-$ | Lines where bus $n$ is the sending end |

### Parameters

| Symbol | JSON field | Description | Unit |
|--------|-----------|-------------|------|
| $\pi_s$ | `probability_factor` | Scenario probability weight | — |
| $\delta_t$ | `discount_factor` | Stage discount factor | — |
| $\Delta_b$ | `duration` | Block duration | hours |
| $r$ | `annual_discount_rate` | Annual discount rate | — |
| $\sigma$ | `scale_objective` | Objective scaling divisor (default 1000) | — |
| $\sigma_\theta$ | `scale_theta` | Voltage angle scaling (default 1000) | — |
| $c_g$ | `gcost` | Generator variable cost | \$/MWh |
| $c_d^{\text{fail}}$ | `demand_fail_cost` | Load curtailment penalty | \$/MWh |
| $c_l$ | `tcost` | Line transfer cost | \$/MWh |
| $\overline{P}_g$ | `pmax` / `capacity` | Generator max output | MW |
| $\underline{P}_g$ | `pmin` | Generator min output | MW |
| $\overline{L}_d$ | `lmax` | Maximum demand (load) | MW |
| $\overline{F}_l^{ab}$ | `tmax_ab` | Line capacity $a \to b$ | MW |
| $\overline{F}_l^{ba}$ | `tmax_ba` | Line capacity $b \to a$ | MW |
| $X_l$ | `reactance` | Line reactance | Ω (p.u. when $V = 1$) |
| $\tau_l$ | `tap_ratio` | Off-nominal tap ratio (transformer) | p.u. |
| $\varphi_l$ | `phase_shift_deg` | Phase-shift angle (PST) | degrees |
| $\lambda_g$ | `lossfactor` (gen) | Generator injection loss fraction | — |
| $\lambda_d$ | `lossfactor` (demand) | Demand withdrawal loss fraction | — |
| $\lambda_l$ | `lossfactor` (line) | Line transmission loss fraction | — |
| $\phi_g(b)$ | `generator_profile` | Generator capacity profile factor | — |
| $\eta_e^{\text{in}}$ | `input_efficiency` | Battery charge efficiency | — |
| $\eta_e^{\text{out}}$ | `output_efficiency` | Battery discharge efficiency | — |
| $\mu_e$ | `annual_loss` | Battery annual self-discharge rate | 1/year |
| $\overline{E}_e$ | `emax` | Battery max energy | MWh |
| $\underline{E}_e$ | `emin` | Battery min energy | MWh |
| $E_e^0$ | `eini` | Battery initial energy | MWh |
| $E_e^{\text{fin}}$ | `efin` | Battery final energy | MWh |
| $\rho_v$ | `conversion_rate` | Converter power conversion rate | — |
| $\overline{Q}_w$ | `fmax` | Waterway max flow | m³/s |
| $\underline{Q}_w$ | `fmin` | Waterway min flow | m³/s |
| $\overline{V}_r$ | `emax` | Reservoir max volume | hm³ |
| $\underline{V}_r$ | `emin` | Reservoir min volume | hm³ |
| $\kappa_u$ | `conversion_rate` (turbine) | Turbine water-to-power factor | MW/(m³/s) |
| $\overline{R}_{z,t,b}^{\text{up}}$ | `urreq` | Up-reserve requirement | MW |
| $\overline{R}_{z,t,b}^{\text{dn}}$ | `drreq` | Down-reserve requirement | MW |
| $K_g^{\text{cap}}$ | `annual_capcost` | Annual expansion cost per module | \$/year |
| $M_g$ | `expcap` | Capacity per expansion module | MW |
| $\overline{m}_g$ | `expmod` | Maximum expansion modules | — |
| $\bar{C}_g^0$ | `capacity` | Initial installed capacity | MW |
| $\phi_h$ | `pump_factor` | Pump MW consumed per m³/s lifted | MW/(m³/s) |
| $\eta_h$ | `efficiency` | Pump efficiency | — |
| $\overline{Q}_h$ | `capacity` (pump) | Pump waterway capacity | m³/s |
| $(V_k^\ell, m_k^\ell, c_k^\ell)$ | `segments` (discharge limit) | Piecewise breakpoint $k$ of reservoir discharge limit | (hm³, m³/s/hm³, m³/s) |
| $\bar{\xi}_{i,s,t,b}$ | `discharge` (flow right) | Scheduled flow-right discharge | m³/s |
| $\overline{F}_i$ | `fmax` (flow right) | Flow-right maximum flow (variable mode) | m³/s |
| $c_i^f$ | `fail_cost` (flow right) | Flow-right deficit penalty | \$/(m³/s) |
| $c^{hf}$ | `hydro_fail_cost` | Global hydro fail cost | \$/m³ |
| $\overline{F}_o$ | `fmax` (volume right) | Volume-right per-block extraction cap | m³/s |
| $D_o$ | `demand` (volume right) | Stage demand target | m³ |
| $c_o^f$ | `fail_cost` (volume right) | Volume-right deficit penalty | \$/m³ |
| $\alpha_o$ | `flow_conversion_rate` | Flow-to-volume conversion (= $0.0036$) | hm³/((m³/s)·h) |
| $\rho_\tau$ | `fcr` (LNG) | Flow conversion rate (tank volume) | — |
| $\lambda_\tau$ | `annual_loss` (LNG) | Boil-off rate | 1/year |
| $\eta_g^{hr}$ | `heat_rate` (LNG link) | Generator fuel-burn rate (m³ LNG / MWh) | m³/MWh |
| $\text{SU}_g$ | `startup_cost` | Startup cost | \$ |
| $\text{SD}_g$ | `shutdown_cost` | Shutdown cost | \$ |
| $\text{NL}_g$ | `noload_cost` | No-load cost (paid while $u=1$) | \$/h |
| $R_g^{\text{up}}$ | `ramp_up` | Hourly ramp-up rate | MW/h |
| $R_g^{\text{dn}}$ | `ramp_down` | Hourly ramp-down rate | MW/h |
| $\text{SU}_g^{\text{ramp}}$ | `startup_ramp` | Ramp allowance at startup | MW |
| $\text{SD}_g^{\text{ramp}}$ | `shutdown_ramp` | Ramp allowance at shutdown | MW |
| $T_g^{\text{up}}$ | `min_up_time` | Minimum up time | h |
| $T_g^{\text{dn}}$ | `min_down_time` | Minimum down time | h |
| $\underline{P}_g^d$ | `dispatch_pmin` | Simple-commitment dispatch floor | MW |
| $H_g$ | `inertia_constant` | Synchronous-machine inertia constant | s |
| $S_g$ | `rated_power` | Rated apparent power | MVA |
| $\Phi_p$ | `provision_factor` | Inertia conversion factor (MWs delivered per MW provided) | s |
| $\overline{R}_{z,t,b}^{H}$ | `requirement` (inertia) | Zone inertia requirement | MWs |

### Decision Variables

| Symbol | Description | Bounds |
|--------|-------------|--------|
| $p_{g,s,t,b}$ | Generator $g$ output | $[\underline{P}_g,\; \overline{P}_g]$ |
| $\ell_{d,s,t,b}$ | Served load at demand $d$ | $[0,\; \overline{L}_d]$ |
| $q_{d,s,t,b}$ | Unserved load (curtailment) at demand $d$ | $\geq 0$ |
| $f_{l,s,t,b}^+$ | Line $l$ forward power flow ($a \to b$) | $[0,\; \overline{F}_l^{ab}]$ |
| $f_{l,s,t,b}^-$ | Line $l$ reverse power flow ($b \to a$) | $[0,\; \overline{F}_l^{ba}]$ |
| $\theta_{n,s,t,b}$ | Voltage angle at bus $n$ (Kirchhoff mode) | $[-2\pi \sigma_\theta,\; 2\pi \sigma_\theta]$ |
| $e_{e,s,t,b}$ | Battery $e$ energy (state of charge) | $[\underline{E}_e,\; \overline{E}_e]$ |
| $p_{e,s,t,b}^{\text{in}}$ | Battery $e$ charging power | $\geq 0$ |
| $p_{e,s,t,b}^{\text{out}}$ | Battery $e$ discharging power | $\geq 0$ |
| $\bar{C}_{g,t}$ | Installed capacity of generator $g$ at stage $t$ | $[\bar{C}_g^0,\; \bar{C}_g^{\max}]$ |
| $m_{g,t}$ | Expansion modules built at stage $t$ | $[0,\; \overline{m}_g]$ |
| $r_{p,s,t,b}^{\text{up}}$ | Up-reserve provision $p$ | $\geq 0$ |
| $r_{p,s,t,b}^{\text{dn}}$ | Down-reserve provision $p$ | $\geq 0$ |
| $q_z^{\text{up}}$ | Unserved up-reserve at zone $z$ | $\geq 0$ |
| $q_z^{\text{dn}}$ | Unserved down-reserve at zone $z$ | $\geq 0$ |
| $\varphi_{w,s,t,b}$ | Waterway $w$ water flow | $[\underline{Q}_w,\; \overline{Q}_w]$ |
| $v_{r,s,t,b}$ | Reservoir $r$ volume | $[\underline{V}_r,\; \overline{V}_r]$ |
| $\text{spill}_{r,s,t,b}$ | Reservoir $r$ spillway discharge | $\geq 0$ |
| $\ell_{d,s,t,b}^{\text{man}}$ | Mandatory served load at demand $d$ (min-energy) | $[0,\; E_d^{\min}/\Delta_b]$ |
| $\text{spill}_{g,s,t,b}$ | Generator $g$ profile curtailment | $\geq 0$ |
| $q_{d,s,t,b}^{\text{prof}}$ | Demand-profile unserved (spillover) slack | $\geq 0$ |
| $q^{\ell}_{s,t}$ | Reservoir-discharge-limit stage-average flow (`qeh`) | free |
| $\xi_{i,s,t,b}$ | FlowRight extraction flow | $[\underline{\xi}_i,\; \overline{\xi}_i]$ |
| $\delta_{i,s,t,b}$ | FlowRight per-block deficit | $\geq 0$ |
| $\bar{\xi}_{i,s,t}^{h}$ | FlowRight stage-average hourly flow (`qeh`) | free |
| $\xi_{o,s,t,b}^{\text{in}}$ | VolumeRight extraction rate | $[0,\; \overline{F}_o]$ |
| $\xi_{o,s,t,b}^{\text{sav}}$ | VolumeRight saving (deposit) rate | $\geq 0$ |
| $\nu_{o,s,t,b}$ | VolumeRight remaining rights volume (state) | $[0,\; \overline{V}_o]$ |
| $q_{o,s,t}^{\text{fail}}$ | VolumeRight demand deficit | $\geq 0$ |
| $e_{\tau,s,t,b}^{\text{lng}}$ | LNG tank volume | $[\underline{V}_\tau,\; \overline{V}_\tau]$ |
| $d_{\tau,s,t,b}^{\text{lng}}$ | LNG delivery (inflow to tank) | $\geq 0$ |
| $u_{g,s,t,p}$ | UC binary status of generator $g$ over period $p$ | $\{0,1\}$ |
| $v_{g,s,t,p}$ | UC binary startup of generator $g$ | $\{0,1\}$ |
| $w_{g,s,t,p}$ | UC binary shutdown of generator $g$ | $\{0,1\}$ |
| $u_{g,s,t,b}^{\text{sc}}$ | Simple-commitment binary status | $\{0,1\}$ |
| $r_{p,s,t,b}^{H}$ | Inertia provision (MW dedicated for inertia) | $[0,\; \overline{R}_p^{H}]$ |
| $q_{z,s,t,b}^{H}$ | Inertia-zone requirement slack | $[0,\; \overline{R}_{z,t,b}^{H}]$ |

> **Note on line flows**: When `use_line_losses` is enabled and
> $\lambda_l > 0$, the solver creates separate forward ($f^+$) and reverse
> ($f^-$) flow variables. When $\lambda_l = 0$, a single bidirectional
> variable $f_l \in [-\overline{F}_l^{ba},\; \overline{F}_l^{ab}]$ is used.

---

## 3. Compact Formulation

The following compact formulation follows the standard GTEP LP structure
[[1]](#ref1) [[3]](#ref3) [[10]](#ref10), extended with the FESOP
enhancements for renewable integration and storage [[4]](#ref4):

$$
\min_{p, \ell, q, f, \theta, e, \bar{C}, m, \ldots}
\quad
\underbrace{
\sum_{s \in \mathcal{S}} \sum_{t \in \mathcal{T}} \sum_{b \in \mathcal{B}_t}
\omega_{s,t,b}
\left[
  \sum_{g} c_g \, p_{g,s,t,b} +
  \sum_{d} c_d^{\text{fail}} q_{d,s,t,b} +
  \sum_{l} c_l \left( f_{l,s,t,b}^+ + f_{l,s,t,b}^- \right)
\right]
}_{\text{OPEX}}
\;+\;
\underbrace{
\sum_{g} \sum_{t} \omega_t^{\text{cap}} \, K_g^{\text{cap}} \, m_{g,t}
}_{\text{CAPEX}}
$$

where the combined weighting factor is:

$$
\omega_{s,t,b} \;=\; \frac{\pi_s \cdot \delta_t \cdot \Delta_b}{\sigma}
$$

subject to:

| # | Constraint | $\forall$ |
|---|-----------|-----------|
| (C1) | Bus power balance | $n, s, t, b$ |
| (C2) | Kirchhoff voltage law | $l, s, t, b$ |
| (C3) | Generator output bounds | $g, s, t, b$ |
| (C4) | Generator capacity expansion | $g, t$ |
| (C5) | Demand balance | $d, s, t, b$ |
| (C6) | Line capacity | $l, s, t, b$ |
| (C7) | Battery state-of-charge | $e, s, t, b$ |
| (C8) | Reserve requirements | $z, s, t, b$ |
| (C9) | Reserve–generator coupling | $p, s, t, b$ |
| (C10) | Converter coupling | $v, s, t, b$ |
| (C11) | Junction water balance | $j, s, t, b$ |
| (C12) | Reservoir volume balance | $r, s, t, b$ |
| (C13) | Turbine power conversion | $u, s, t, b$ |
| (C14) | User constraints (optional) | varies |

---

## 4. Objective Function

The objective minimizes the total expected discounted system cost.

### 4.1 Cost Weighting

Every operational cost coefficient is multiplied by the universal weighting
factor:

$$
\omega_{s,t,b} = \frac{\pi_s \cdot \delta_t \cdot \Delta_b}{\sigma}
$$

| Factor | Formula | Description |
|--------|---------|-------------|
| $\pi_s$ | `scenario.probability_factor` | Scenario probability (sum to 1) |
| $\delta_t$ | $(1 + r)^{-\tau_t / 8760}$ | Continuous discounting from stage start time $\tau_t$ (hours) |
| $\Delta_b$ | `block.duration` | Block duration in hours |
| $\sigma$ | `scale_objective` (default 1000) | Numerical scaling divisor |

The discount factor uses continuous compounding based on the stage start
time $\tau_t$ measured in hours from the planning horizon origin:

$$
\delta_t = (1 + r)^{-\tau_t / 8760} \cdot \delta_t^{\text{user}}
$$

where $\delta_t^{\text{user}}$ is an optional user-supplied per-stage
discount factor (default 1.0).

### 4.2 Operational Costs (OPEX)

$$
z_{\text{OPEX}} = \sum_{s \in \mathcal{S}} \sum_{t \in \mathcal{T}}
\sum_{b \in \mathcal{B}_t} \omega_{s,t,b}
\Bigg[
  \underbrace{\sum_{g \in \mathcal{G}} c_{g,t} \; p_{g,s,t,b}}_{\text{Generation cost}} +
  \underbrace{\sum_{d \in \mathcal{D}} c_{d,t}^{\text{fail}} \; q_{d,s,t,b}}_{\text{Curtailment cost}} +
  \underbrace{\sum_{l \in \mathcal{L}} c_{l,t} \left( f_{l,s,t,b}^{+} + f_{l,s,t,b}^{-} \right)}_{\text{Transfer cost}}
\Bigg]
$$

Additional OPEX terms may include:

- **Reserve failure cost**: $\sum_{z} c_z^{\text{rfail}} (q_{z,s,t,b}^{\text{up}} + q_{z,s,t,b}^{\text{dn}})$
- **Reserve provision cost**: $\sum_{p} \bigl( c_{p,t}^{\text{ur}} \; r_{p,s,t,b}^{\text{up}} + c_{p,t}^{\text{dr}} \; r_{p,s,t,b}^{\text{dn}} \bigr)$
- **Spillway cost**: $\sum_{r} c_{r,t}^{\text{spill}} \; \text{spill}_{r,s,t,b}$
- **Generator profile spillover cost**: $\sum_{g} c_{g,t}^{\text{spill}} \; \text{spill}_{g,s,t,b}$

### 4.3 Investment Costs (CAPEX)

$$
z_{\text{CAPEX}} = \sum_{g} \sum_{t \in \mathcal{T}}
\omega_t^{\text{cap}} \; K_g^{\text{cap}} \; m_{g,t}
$$

where the investment cost weight is:

$$
\omega_t^{\text{cap}} = \frac{\delta_t \cdot T_t}{\sigma}
$$

with $T_t$ the stage duration in hours and the annualized cost converted to
an hourly rate:

$$
K_g^{\text{hour}} = \frac{K_g^{\text{cap}}}{8760}
$$

The same expansion structure applies to demands, lines, batteries, and
converters. The CAPEX term generalizes to all expandable components.

### 4.4 Complete Objective

$$
\min \quad z = z_{\text{OPEX}} + z_{\text{CAPEX}}
$$

---

## 5. Constraints by Component

### 5.1 Bus Power Balance

The power balance at each bus $n$ ensures that total injection equals total
withdrawal (Kirchhoff's Current Law) [[11]](#ref11):

$$
\sum_{g \in \mathcal{G}_n} (1 - \lambda_g) \; p_{g,s,t,b}
\;-\; \sum_{d \in \mathcal{D}_n} (1 + \lambda_d) \; \ell_{d,s,t,b}
\;+\; \sum_{l \in \mathcal{L}_n^+} f_{l,s,t,b}^{\text{net,in}}
\;-\; \sum_{l \in \mathcal{L}_n^-} f_{l,s,t,b}^{\text{net,out}}
\;=\; 0
\qquad \forall \; n, s, t, b
$$

where:
- $(1 - \lambda_g)$ accounts for generator injection losses
- $(1 + \lambda_d)$ accounts for demand-side losses (load + losses must be served)
- The line flow contributions depend on loss modeling (see
  [Section 5.5](#55-transmission-line-constraints))

The **dual variable** of this constraint is the **Locational Marginal Price
(LMP)** at bus $n$, reported as `balance_dual` in the output.

### 5.2 Generator Constraints

#### Output Bounds

$$
\underline{P}_{g,t,b} \;\leq\; p_{g,s,t,b} \;\leq\; \overline{P}_{g,t,b}
\qquad \forall \; g, s, t, b
$$

The effective bounds at each block depend on stage-specific and
block-specific parameters:

$$
\overline{P}_{g,t,b} = \min\bigl(\overline{P}_{g,t}(b),\; \bar{C}_{g,t}\bigr)
\qquad
\underline{P}_{g,t,b} = \max\bigl(\underline{P}_{g,t}(b),\; 0\bigr)
$$

where $\overline{P}_{g,t}(b)$ and $\underline{P}_{g,t}(b)$ are the
block-level schedule values of `pmax` and `pmin` respectively, and
$\bar{C}_{g,t}$ is the installed capacity at stage $t$.

#### Cost Coefficient

The LP column cost for $p_{g,s,t,b}$ is:

$$
\text{cost}(p_{g,s,t,b}) = c_{g,t} \cdot \omega_{s,t,b}
= \frac{c_{g,t} \cdot \pi_s \cdot \delta_t \cdot \Delta_b}{\sigma}
$$

#### Capacity Linking

When capacity expansion is enabled for generator $g$:

$$
p_{g,s,t,b} \;\leq\; \bar{C}_{g,t}
\qquad \forall \; s, t, b
$$

This constraint links the operational variable to the investment variable
(see [Section 5.11](#511-capacity-expansion-constraints)).

### 5.3 Generator Profile Constraints

When a generator profile $\phi_g(s,t,b)$ is defined (e.g., for solar or
wind plants), the profile constrains the maximum output at each block:

$$
p_{g,s,t,b} + \text{spill}_{g,s,t,b} = \phi_g(s,t,b) \cdot \bar{C}_{g,t}
\qquad \forall \; s, t, b
$$

where:
- $\phi_g(s,t,b) \in [0, 1]$ is the capacity factor at each block
- $\text{spill}_{g,s,t,b} \geq 0$ is curtailed generation (spillover)
- The spillover variable has cost $c_g^{\text{spill}} \cdot \omega_{s,t,b}$
  (may be zero to allow free curtailment of renewables)

### 5.4 Demand Constraints

#### Demand Balance

When curtailment is allowed (i.e., `demand_fail_cost` is set):

$$
\ell_{d,s,t,b} + q_{d,s,t,b} = \overline{L}_{d,t,b}
\qquad \forall \; d, s, t, b
$$

When curtailment is **not** allowed, the load is fixed:

$$
\ell_{d,s,t,b} = \overline{L}_{d,t,b}
\qquad \forall \; d, s, t, b
$$

where $\overline{L}_{d,t,b}$ is the scheduled demand at stage $t$, block $b$,
incorporating any demand profile scaling.

#### Curtailment Cost

$$
\text{cost}(q_{d,s,t,b}) = c_{d,t}^{\text{fail}} \cdot \omega_{s,t,b}
$$

#### Bus Balance Contribution

Demand contributes to the bus balance with a loss factor:

$$
\text{bus balance coefficient of } \ell_{d,s,t,b}: \quad -(1 + \lambda_d)
$$

where $\lambda_d$ is the demand-side loss factor.

#### Minimum Energy Constraint (Optional)

When a minimum energy requirement $E_d^{\min}$ is specified:

$$
\sum_{b \in \mathcal{B}_t} \ell_{d,s,t,b}^{\text{man}} \cdot \Delta_b \;\geq\; E_d^{\min}
\qquad \forall \; d, s, t
$$

### 5.4 bis Demand Profile Constraints

When a `DemandProfile` element is attached to a demand $d$, the served load
is tied to a time-varying profile factor $\phi_d(s,t,b) \in [0,1]$ with an
auxiliary spillover (unserved) variable $q_{d,s,t,b}^{\text{prof}} \geq 0$.

This mirrors §5.3 GeneratorProfile: one LP column per block and one
equality row per block.

#### Profile Row — With Capacity Column

When the parent `Demand` carries an explicit capacity column $\bar{C}_{d,t}$
(either a constant capacity or an expansion variable), the profile ties
served load plus spillover to the scaled capacity:

$$
\begin{aligned}
\ell_{d,s,t,b} \;+\; q_{d,s,t,b}^{\text{prof}} \;-\; \phi_d(s,t,b)\,\bar{C}_{d,t} \;=\; 0
\qquad \forall \; s,t,b
\end{aligned}
$$
<!-- source: include/gtopt/profile_object_lp.hpp:118-123 -->

#### Profile Row — Without Capacity Column

When no capacity column exists, the profile row uses the stage capacity
$\overline{L}_{d,t}$ on the RHS:

$$
\begin{aligned}
\ell_{d,s,t,b} \;+\; q_{d,s,t,b}^{\text{prof}} \;=\; \phi_d(s,t,b) \cdot \overline{L}_{d,t}
\qquad \forall \; s,t,b
\end{aligned}
$$
<!-- source: include/gtopt/profile_object_lp.hpp:125-126 -->

#### Spillover Cost

The spillover column carries the `scost` coefficient, weighted by the usual
block cost factor:

$$
\text{cost}(q_{d,s,t,b}^{\text{prof}}) = c_{d,t}^{\text{spill}} \cdot \omega_{s,t,b}
$$
<!-- source: include/gtopt/profile_object_lp.hpp:97-108 -->

> **Activation.** The `DemandProfile` element requires the parent `Demand`
> to define either a `capacity` value or a capacity expansion; otherwise
> the profile is rejected during LP assembly.
> <!-- source: source/demand_profile_lp.cpp:40-47 -->

### 5.5 Transmission Line Constraints

#### Without Losses ($\lambda_l = 0$)

A single bidirectional flow variable $f_l$ is created:

$$ -\overline{F}_l^{ba} \;\leq\; f_{l,s,t,b} \;\leq\; \overline{F}_l^{ab}
\qquad \forall \; l, s, t, b
$$

Bus balance contributions (line from bus $a$ to bus $b$):

$$
\text{At bus } a: \quad -f_{l,s,t,b}
\qquad\qquad
\text{At bus } b: \quad +f_{l,s,t,b}
$$

#### With Losses ($\lambda_l > 0$)

Separate forward and reverse flow variables are created:

$$
0 \;\leq\; f_{l,s,t,b}^{+} \;\leq\; \overline{F}_l^{ab}
\qquad
0 \;\leq\; f_{l,s,t,b}^{-} \;\leq\; \overline{F}_l^{ba}
$$

Bus balance contributions for forward flow ($a \to b$):

$$
\text{At bus } a: \quad -f_{l,s,t,b}^{+}
\qquad
\text{At bus } b: \quad +(1 - \lambda_l) \, f_{l,s,t,b}^{+}
$$

Bus balance contributions for reverse flow ($b \to a$):

$$
\text{At bus } b: \quad -f_{l,s,t,b}^{-}
\qquad
\text{At bus } a: \quad +(1 - \lambda_l) \, f_{l,s,t,b}^{-}
$$

Losses are allocated at the receiving bus: the power arriving is
$(1 - \lambda_l)$ times the power sent.

#### Transfer Cost

$$
\text{cost}(f_{l,s,t,b}^{+}) = \text{cost}(f_{l,s,t,b}^{-}) = c_{l,t} \cdot \omega_{s,t,b}
$$

### 5.6 Kirchhoff Voltage Law (DC OPF)

When `use_kirchhoff = true` and `use_single_bus = false`, voltage angle
variables $\theta_n$ are introduced and DC power flow constraints enforce
Kirchhoff's Voltage Law on each line $l$ connecting bus $a$ to bus $b$
[[11]](#ref11) [[12]](#ref12):

$$
\frac{\theta_{a,s,t,b} - \theta_{b,s,t,b}}{X_l / V^2} = f_{l,s,t,b}^{+} - f_{l,s,t,b}^{-}
$$

> **Unit convention:** $V$ is the line's `voltage` field (defaults to 1.0)
> and $X_l$ is the `reactance`. The two must use a consistent unit system:
>
> | Mode | $V$ | $X_l$ | $B_l = V^2/X_l$ |
> |------|-----|-------|------------------|
> | **Per-unit** (default, `voltage` omitted) | 1.0 | p.u. | p.u. |
> | **Physical** (`voltage` in kV) | kV | Ω | MW/rad |
>
> Mixing conventions (e.g. $V = 220$ kV with $X = 0.01$ p.u.) produces
> an enormous susceptance and will cause numerical issues.
> See [[17]](#ref17) for an example of automatic per-unit conversion.

In the implementation, this is scaled by $\sigma_\theta$ for numerical
stability. Defining the scaled susceptance:

$$
\chi_l = \sigma_\theta \cdot \frac{X_l}{V^2}
$$

the constraint becomes:

$$ -\theta_{a,s,t,b} + \theta_{b,s,t,b} + \chi_l \, f_{l,s,t,b}^{+} - \chi_l \, f_{l,s,t,b}^{-} = 0
\qquad \forall \; l, s, t, b
$$

#### Transformer and Phase-Shifting Transformer Model

A **tap-changing transformer** or **phase-shifting transformer (PST)** extends
the standard Kirchhoff constraint with two additional parameters:

- **Off-nominal tap ratio** $\tau_l$ (JSON field `tap_ratio`, default 1.0):
  scales the effective susceptance by $1/\tau_l$, so the line carries less
  (or more) power for the same angle difference.
- **Phase-shift angle** $\varphi_l$ (JSON field `phase_shift_deg`, default
  0°, converted internally to radians): shifts the right-hand side of the
  equality constraint, enabling direct control of power flow through the PST.

The generalized Kirchhoff constraint for a transformer branch is:

$$ -\theta_{a,s,t,b} + \theta_{b,s,t,b} +
(\tau_l \, \chi_l) \, f_{l,s,t,b}^{+} -
(\tau_l \, \chi_l) \, f_{l,s,t,b}^{-}
= -\sigma_\theta \cdot \varphi_{l,\text{rad}}
\qquad \forall \; l, s, t, b
$$

where $\varphi_{l,\text{rad}} = \varphi_l \cdot \pi / 180$.

When $\tau_l = 1$ and $\varphi_l = 0$ the expression reduces to the standard
line constraint above. The parameters support per-stage schedules (scalar,
inline array, or Parquet filename) — useful for OLTC (on-load tap changer)
models where the tap position can vary across planning stages.

To model a transformer element in the JSON input, set `"type": "transformer"`
on the line entry (optional tag used by the `pp2gtopt` converter and for
documentation purposes):

```json
{
  "uid": 5, "name": "T1_2_5",
  "type": "transformer",
  "bus_a": 2, "bus_b": 5,
  "voltage": 220.0,
  "reactance": 0.0625,
  "tmax_ab": 150.0, "tmax_ba": 150.0,
  "tap_ratio": 1.05
}
```

#### Reference Bus

One bus is designated as the **reference bus** with a fixed voltage angle:

$$
\theta_{n^{\text{ref}},s,t,b} = \theta^{\text{ref}} \cdot \sigma_\theta
\qquad \forall \; s, t, b
$$

typically $\theta^{\text{ref}} = 0$.

#### Voltage Angle Bounds

For non-reference buses:

$$ -2\pi \, \sigma_\theta \;\leq\; \theta_{n,s,t,b} \;\leq\; 2\pi \, \sigma_\theta
\qquad \forall \; n \neq n^{\text{ref}},\; s, t, b
$$

#### Enabling Conditions

Kirchhoff constraints are added when **all** of:
- `use_kirchhoff = true` (default)
- `use_single_bus = false` (default)
- The system has more than `kirchhoff_threshold` buses (default 0)
- Line has a defined `reactance` value

### 5.7 Battery / Energy Storage Constraints

Batteries model energy storage with charge/discharge efficiencies and
self-discharge losses. The formulation follows the standard linear storage
model used in capacity expansion tools [[4]](#ref4) [[13]](#ref13)
[[14]](#ref14).

#### State-of-Charge (SoC) Balance

For each battery $e$ at each block $b$ within a stage:

$$
e_{e,s,t,b} = e_{e,s,t,b-1} \cdot (1 - \mu_e^h \Delta_b)
\;+\; p_{e,s,t,b}^{\text{in}} \cdot \eta_e^{\text{in}} \cdot \Delta_b
\;-\; p_{e,s,t,b}^{\text{out}} \cdot \frac{\Delta_b}{\eta_e^{\text{out}}}
$$

where:
- $\mu_e^h = \mu_e / 8760$ is the hourly self-discharge rate
- $\eta_e^{\text{in}}$ is the charging efficiency (fraction of input power
  stored)
- $\eta_e^{\text{out}}$ is the discharging efficiency (fraction of stored
  energy delivered)
- $\Delta_b$ is the block duration in hours

#### Energy Bounds

$$
\underline{E}_{e,t} \;\leq\; e_{e,s,t,b} \;\leq\; \overline{E}_{e,t}
\qquad \forall \; e, s, t, b
$$

#### Initial and Final Conditions

For the **first block** of the first stage:

$$
e_{e,s,t_0,b_0} \in [\underline{E}_e^{\text{ini}},\; \overline{E}_e^{\text{ini}}]
$$

where $E_e^{\text{ini}}$ defaults to the stage bounds if not specified.

For the **last block** of each stage, a final SoC target may be enforced:

$$
e_{e,s,t,b_{\text{last}}} \;\geq\; E_e^{\text{fin}}
$$

where $E_e^{\text{fin}}$ defaults to $\underline{E}_e$ if not specified.

#### Inter-Stage Linking

The final SoC of stage $t$ links to the initial SoC of stage $t+1$:

$$
e_{e,s,t+1,b_0} = e_{e,s,t,b_{\text{last}}}
$$

### 5.8 Converter Constraints

A converter $v$ couples a battery $e$ to a generator $g$ (discharge path) and
a demand $d$ (charge path) via a conversion rate $\rho_v$:

#### Discharge Coupling (Battery → Generator)

$$
p_{g,s,t,b} = \rho_v \cdot p_{e,s,t,b}^{\text{out}}
\qquad \forall \; s, t, b
$$

#### Charge Coupling (Demand → Battery)

$$
\ell_{d,s,t,b} = \rho_v \cdot p_{e,s,t,b}^{\text{in}}
\qquad \forall \; s, t, b
$$

#### Converter Capacity

When the converter has a capacity limit:

$$
p_{g,s,t,b} + \ell_{d,s,t,b} \;\leq\; \bar{C}_{v,t}
\qquad \forall \; s, t, b
$$

### 5.9 Reserve Constraints

#### Reserve Zone Requirements

For each reserve zone $z$, the total up-reserve provided must meet the
requirement:

$$
\sum_{p \in \mathcal{P}_z} \gamma_{p,t} \; r_{p,s,t,b}^{\text{up}} + q_{z,s,t,b}^{\text{up}} \;\geq\; \overline{R}_{z,t,b}^{\text{up}}
\qquad \forall \; z, s, t, b
$$

$$
\sum_{p \in \mathcal{P}_z} \gamma_{p,t} \; r_{p,s,t,b}^{\text{dn}} + q_{z,s,t,b}^{\text{dn}} \;\geq\; \overline{R}_{z,t,b}^{\text{dn}}
\qquad \forall \; z, s, t, b
$$

where:
- $\gamma_{p,t}$ is the provision factor at stage $t$ (typically 1.0)
- $q_z^{\text{up}}, q_z^{\text{dn}} \geq 0$ are reserve shortage
  (curtailment) variables
- $\overline{R}_{z,t,b}^{\text{up/dn}}$ is the reserve requirement (MW)

#### Reserve–Generator Coupling

Up-reserve provision is limited by the generator's headroom:

$$
p_{g,s,t,b} + r_{p,s,t,b}^{\text{up}} \;\leq\; \bar{C}_{g,t}
\qquad \forall \; p \in \mathcal{P},\; s, t, b
$$

Down-reserve provision is limited by the generator's operating range:

$$
p_{g,s,t,b} - r_{p,s,t,b}^{\text{dn}} \;\geq\; \underline{P}_{g,t,b}
\qquad \forall \; p \in \mathcal{P},\; s, t, b
$$

#### Reserve Costs

- **Up-reserve provision cost**: $c_{p,t}^{\text{ur}} \cdot \omega_{s,t,b}$
  per MW of up-reserve provided
- **Down-reserve provision cost**: $c_{p,t}^{\text{dr}} \cdot \omega_{s,t,b}$
  per MW of down-reserve provided
- **Reserve failure cost**: $c_z^{\text{rfail}} \cdot \omega_{s,t,b}$
  per MW of unserved reserve (applies to both up and down shortages)

### 5.10 Hydro Cascade Constraints

The hydro cascade models a network of junctions (hydraulic nodes) connected
by waterways, with reservoirs for water storage. This formulation extends
classical hydrothermal coordination models [[5]](#ref5) [[6]](#ref6)
[[7]](#ref7) to support multi-scenario expansion planning.

#### Junction Water Balance

At each junction $j$, the water balance ensures conservation:

$$
\sum_{w \in \mathcal{W}_j^{\text{in}}} (1 - \lambda_w) \, \varphi_{w,s,t,b}
\;-\; \sum_{w \in \mathcal{W}_j^{\text{out}}} \varphi_{w,s,t,b}
\;+\; \sum_{f \in \mathcal{F}_j^+} Q_f
\;-\; \sum_{f \in \mathcal{F}_j^-} Q_f
\;+\; \sum_{r \in \mathcal{R}_j} d_{r,s,t,b}
\;=\; 0
\qquad \forall \; j, s, t, b
$$

where:
- $\lambda_w$ is the waterway transport loss factor
- $Q_f$ is a fixed exogenous inflow or outflow
- $d_{r,s,t,b}$ is the net extraction/injection from reservoir $r$

#### Waterway Flow Bounds

$$
\underline{Q}_{w,t} \;\leq\; \varphi_{w,s,t,b} \;\leq\; \overline{Q}_{w,t}
\qquad \forall \; w, s, t, b
$$

#### Reservoir Volume Balance

Reservoir volume dynamics follow the same storage template as batteries:

$$
v_{r,s,t,b} = v_{r,s,t,b-1} \cdot (1 - \mu_r^h \, \Delta_b)
\;-\; d_{r,s,t,b} \cdot \Delta_b
\;-\; \text{spill}_{r,s,t,b} \cdot \Delta_b
$$

where:
- $\mu_r^h$ is the hourly evaporation/seepage loss rate
- $d_{r,s,t,b}$ is the net extraction from the reservoir (positive = water
  leaving for turbines)
- $\text{spill}_{r,s,t,b}$ is the spillway discharge

#### Volume Bounds

$$
\underline{V}_{r,t} \;\leq\; v_{r,s,t,b} \;\leq\; \overline{V}_{r,t}
\qquad \forall \; r, s, t, b
$$

with initial volume $v_{r,s,t_0,b_0} = V_r^0$ and optional final volume
target.

#### Turbine Power Conversion

Each turbine $u$ links a waterway $w$ to a generator $g$ via a
water-to-power conversion:

$$
p_{g,s,t,b} = \kappa_u \cdot \varphi_{w,s,t,b}
\qquad \forall \; u, s, t, b
$$

or, when the turbine allows partial water bypass (drain mode):

$$
p_{g,s,t,b} \;\leq\; \kappa_u \cdot \varphi_{w,s,t,b}
\qquad \forall \; u, s, t, b
$$

##### Volume-Dependent Efficiency

In practice, the conversion rate $\kappa_u$ is not constant — it depends
on the hydraulic head, which is a function of the reservoir volume.  When
a `ReservoirEfficiency` element is defined for a turbine, the solver
replaces the static $\kappa_u$ with a volume-dependent function.

The efficiency is modelled as a **concave piecewise-linear envelope**
parameterised by $N$ segments:

$$
\kappa_u(v) = \min_{i=1}^{N} \lbrace c_i + m_i \cdot (v - v_i) \rbrace
$$

where for each segment $i$:

| Symbol | JSON field  | Meaning |
|--------|-----------|---------|
| $v_i$  | `volume`  | Volume breakpoint [dam³] |
| $m_i$  | `slope`   | Marginal efficiency change per dam³ |
| $c_i$  | `constant`| Efficiency intercept at breakpoint $v_i$ |

The result is clamped to zero: $\kappa_u(v) \ge 0$.  The fallback
`mean_production_factor` $\bar{\kappa}_u$ is used when the solver cannot modify
LP matrix coefficients in-place.

**SDDP integration**: during the forward pass, the solver evaluates
$\kappa_u(v_{r}^{(k)})$ using the reservoir volume $v_{r}^{(k)}$ from
iteration $k$ and updates the LP matrix coefficient via
`set_coeff(row, col, -kappa_u)` (i.e., $-\kappa_u$).  For the **first iteration**
($k{=}1$), the initial volume $v_{\text{ini}}$ is used for all phases
and scenes.

#### Filtration (Water Seepage)

Filtration models seepage from a waterway $w$ into a reservoir $r$ as a
function of the reservoir's average volume:

$$
\varphi_{i,s,t,b} = a_i + b_i \cdot \frac{v_{r,s,t,b-1} + v_{r,s,t,b}}{2}
\qquad \forall \; i, s, t, b
$$

where $a_i$ is the constant term (RHS) and $b_i$ is the slope (seepage rate
per unit volume).

**Piecewise-linear filtration (plpfilemb.dat / plpcenfi.dat segments).**
When a set of segments $\{(V_k, b_k, c_k)\}_{k=1}^{K}$ is provided, the
filtration function is defined by a concave envelope:

$$
\varphi(V) = \min_{k} \lbrace c_k + b_k \cdot (V - V_k) \rbrace
$$

At each phase, the active segment $k^*$ is selected based on the last known
reservoir volume $V^{\text{ini}}$ (from the previous phase or the initial
condition).  The LP constraint is then updated directly in the LP matrix:

- Coefficient on storage variables: $-b_{k^*} / 2$ on both $v_{b-1}$ and $v_b$
- Right-hand side: $a_{k^*} = c_{k^*} - b_{k^*} \cdot V_{k^*}$

To minimise overhead, `set_coeff` / `set_rhs` calls are dispatched only when
the new values differ from the previously applied ones.  This mechanism is
analogous to the piecewise-linear efficiency update for turbines (§5.10).

The primary PLP source for this model is `plpfilemb.dat` (Fortran subroutine
`LeeFilEmb` in `leefilemb.f`), parsed by `FilembParser` in
`scripts/plp2gtopt/filemb_parser.py`.  Volume breakpoints are stored in dam³
(convert from PLP Mm³ by multiplying by 1000); slopes are in m³/s per dam³
(convert from PLP /Mm³ by dividing by 1000); constants are in m³/s (no
conversion).  The legacy `plpcenfi.dat` file is also supported via
`CenfiParser`; `plpfilemb.dat` takes precedence when both are present.

### 5.10 bis Hydro Pump

A `Pump` element $h \in \mathcal{H}$ couples a waterway flow variable
$\varphi_{w,s,t,b}$ [m³/s] to the served load of a demand
$\ell_{d,s,t,b}$ [MW] representing the pump motor. Physically, the pump
must consume at least $\phi_h / \eta_h$ MW of electrical energy per m³/s
of water raised, where $\phi_h$ is the nominal pump factor and
$\eta_h \in (0,1]$ is the stage efficiency.

#### Conversion Constraint

$$
\begin{aligned}
\frac{\phi_{h,t}}{\eta_{h,t}} \; \varphi_{w,s,t,b} \;-\; \ell_{d,s,t,b} \;\leq\; 0
\qquad \forall \; h \in \mathcal{H},\; s,t,b
\end{aligned}
$$
<!-- source: source/pump_lp.cpp:86-97 -->

Equivalently, $\ell_{d,s,t,b} \geq (\phi_{h,t}/\eta_{h,t})\,\varphi_{w,s,t,b}$:
the optimizer must supply enough electrical power through demand $d$ to
sustain the water flow pumped through waterway $w$.

#### Waterway Capacity (Optional)

When a pump capacity $\overline{Q}_{h,t}$ is specified for the stage, an
additional per-block cap is added on the waterway flow:

$$
\begin{aligned}
\varphi_{w,s,t,b} \;\leq\; \overline{Q}_{h,t}
\qquad \forall \; h \in \mathcal{H},\; s,t,b
\end{aligned}
$$
<!-- source: source/pump_lp.cpp:99-112 -->

The waterway and demand referenced by the pump retain their native
contributions to the junction water balance (§5.10) and the bus power
balance (§5.1) respectively; the pump element adds only the coupling row
(and the optional capacity row).

### 5.10 ter Reservoir Discharge Limit

A `ReservoirDischargeLimit` element $\ell \in \mathcal{DL}$ imposes a
**volume-dependent cap** on the stage-average hourly turbine discharge of
a waterway $w$, selected from a piecewise-linear concave envelope indexed
by reservoir volume.

#### Stage-Average Flow Variable

A free auxiliary variable $q^{\ell}_{s,t}$ represents the stage-average
hourly flow through $w$:

$$
\begin{aligned}
q^{\ell}_{s,t} \;-\; \sum_{b \in \mathcal{B}_t} \frac{\Delta_b}{T_t} \, \varphi_{w,s,t,b} \;=\; 0
\qquad \forall \; \ell,\; s,t
\end{aligned}
$$
<!-- source: source/reservoir_discharge_limit_lp.cpp:76-92 -->

where $T_t = \sum_{b \in \mathcal{B}_t} \Delta_b$ is the stage duration.

#### Volume-Dependent Cap

Given piecewise segments $\{(V_k^\ell, m_k^\ell, c_k^\ell)\}_{k=1}^{K}$
selected by the current reservoir volume $V$ (average of initial and
final stage volumes), the active segment $k^*$ yields slope
$m_{k^*}^\ell$ and intercept $c_{k^*}^\ell$. The discharge cap then
reads:

$$
\begin{aligned}
q^{\ell}_{s,t} \;-\; \tfrac{1}{2} m_{k^*}^\ell\, v_{r,s,t,b_0} \;-\; \tfrac{1}{2} m_{k^*}^\ell\, v_{r,s,t,b_{\text{last}}} \;\leq\; c_{k^*}^\ell
\qquad \forall \; \ell,\; s,t
\end{aligned}
$$
<!-- source: source/reservoir_discharge_limit_lp.cpp:94-109 -->

where $v_{r,s,t,b_0}$ / $v_{r,s,t,b_{\text{last}}}$ are the initial/final
reservoir volume columns (`eini`, `efin`) of the associated reservoir $r$.

Between SDDP iterations the solver re-selects $k^*$ from the updated
forward-pass volume and patches both storage coefficients
(`-m_{k^*}/2` on `eini` and `efin`) and the RHS ($c_{k^*}^\ell$)
in place.
<!-- source: source/reservoir_discharge_limit_lp.cpp:133-188 -->

### 5.11 Capacity Expansion Constraints

Capacity expansion applies uniformly to generators, demands, lines,
batteries, and converters. The modular expansion structure follows the
standard GTEP approach [[1]](#ref1) [[2]](#ref2) [[3]](#ref3) [[10]](#ref10).
For each expandable component $g$ at stage $t$:

#### Installation Balance

$$
\bar{C}_{g,t} = \bar{C}_{g,t-1} \cdot (1 - \xi_{g,t})
\;+\; M_g \cdot m_{g,t}
\;+\; \Delta C_{g,t}
$$

where:
- $\bar{C}_{g,t}$ is the installed capacity at stage $t$
- $\xi_{g,t}$ is the stage derating factor (capacity degradation)
- $M_g$ (`expcap`) is the capacity per expansion module
- $m_{g,t}$ is the number of modules installed at stage $t$
- $\Delta C_{g,t}$ accounts for any exogenous capacity changes between stages

#### Expansion Bounds

$$
0 \;\leq\; m_{g,t} \;\leq\; \overline{m}_{g,t}
\qquad \forall \; g, t
$$

where $\overline{m}_{g,t}$ (`expmod`) is the maximum number of modules that
can be installed at stage $t$.

#### Maximum Capacity

$$
\bar{C}_{g,t} \;\leq\; \bar{C}_g^{\max}
\qquad \forall \; g, t
$$

where $\bar{C}_g^{\max}$ is derived from `capmax` or
$\bar{C}_g^0 + M_g \cdot \overline{m}_g$.

#### Investment Cost Tracking

The annual investment cost is converted to an hourly rate and weighted by
the stage discount factor:

$$
\text{cost}(m_{g,t}) = \frac{K_g^{\text{cap}}}{8760} \cdot M_g \cdot \frac{\delta_t \cdot T_t}{\sigma}
$$

where $T_t$ is the stage duration in hours.

#### Variable Type

By default, $m_{g,t}$ is a **continuous** variable (LP relaxation). When
`colint` is set on the component, $m_{g,t}$ becomes an **integer** variable
(MIP formulation).

### 5.12 User Constraints

In addition to the built-in constraints above, gtopt supports
**user-defined linear constraints** that allow arbitrary linear
relationships between LP variables. User constraints are specified via
the `user_constraint_array` (inline) or `user_constraint_file`
(external JSON) fields in the planning input.

Each user constraint defines:
- A **sense** (`<=`, `>=`, or `=`)
- A **right-hand side** value
- A set of **coefficient entries** mapping (component class, element
  UID, variable name) triples to scalar coefficients

User constraints are added to the LP after all built-in constraints and
can reference any LP variable (generator output, line flow, battery SoC,
etc.).

> **See also**: [`INPUT_DATA.md`](../input-data.md) for the JSON
> format specification of user constraints.

### 5.13 Water Rights (FlowRight and VolumeRight)

Hydro-dominated systems often operate under legally-binding irrigation
agreements that partition turbine discharge into distinct rights
categories with volume-dependent allocations. gtopt implements this via
two generic LP elements that live **in parallel** to the physical hydro
topology: `FlowRight` ($i \in \mathcal{FR}$) and `VolumeRight`
($o \in \mathcal{VR}$). They are optionally *consumptive* (coupled to
the physical junction/reservoir balances) or purely virtual (a separate
ledger).

See the companion white paper §5 (`sec:rights`) for the full domain
narrative; the equations below port the formal statements and tie each
to the LP assembly code.

#### 5.13.1 Flow Rights

A `FlowRight` creates one extraction flow column $\xi_{i,s,t,b}$
[m³/s] per block. Two operating modes are supported:

$$
\begin{aligned}
\underline{\xi}_{i,s,t,b} = \begin{cases}
\bar{\xi}_{i,s,t,b} & \text{fixed mode (default)} \\
0 & \text{variable mode: } \overline{F}_i > 0 \text{ and } \bar{\xi}_{i,s,t,b} = 0
\end{cases}
\end{aligned}
$$

$$
\begin{aligned}
\overline{\xi}_{i,s,t,b} = \begin{cases}
\bar{\xi}_{i,s,t,b} & \text{fixed mode} \\
\overline{F}_{i,t,b} & \text{variable mode}
\end{cases}
\end{aligned}
$$

Optionally capped further by a `bound_rule` (e.g. a volume-zone lookup):

$$
\begin{aligned}
\overline{\xi}_{i,s,t,b} \;\leftarrow\; \min\!\bigl(\overline{\xi}_{i,s,t,b},\; B_i(x)\bigr)
\end{aligned}
$$

where $B_i(x)$ is the piecewise-linear rule evaluated on an axis $x$
(reservoir volume, calendar month, etc.).
<!-- source: source/flow_right_lp.cpp:46-69 -->

**Deficit / soft discharge.** When the per-block fail cost
$c_{i,s,t,b}^f > 0$ (either direct schedule or derived from the global
$c^{hf}$ via $c^{hf} \cdot \Delta_b \cdot 3600$) and a nonzero
discharge is scheduled, a deficit variable $\delta_{i,s,t,b} \geq 0$ is
created with lower bound on $\xi$ relaxed to $0$, and the **demand
coupling** row is added:

$$
\begin{aligned}
\xi_{i,s,t,b} \;+\; \delta_{i,s,t,b} \;\geq\; \bar{\xi}_{i,s,t,b}
\qquad \forall \; i \in \mathcal{FR},\; s,t,b
\end{aligned}
$$
<!-- source: source/flow_right_lp.cpp:198-214 -->

**Use-value benefit.** A per-block `use_value`
$u_{i,s,t,b}$ ($\$/(m^3/s)$, inherited from $c^{hu}$ when unset)
contributes a *negative* coefficient $-u_{i,s,t,b}$ on the flow column —
a benefit that incentivizes non-zero flow in variable mode.
<!-- source: source/flow_right_lp.cpp:163-185 -->

**Stage-average auxiliary.** When `use_average = true`, a stage-level
variable $\bar{\xi}_{i,s,t}^{h}$ plus an equality row:

$$
\begin{aligned}
\bar{\xi}_{i,s,t}^{h} \;-\; \sum_{b \in \mathcal{B}_t} \frac{\Delta_b}{T_t} \, \xi_{i,s,t,b} \;=\; 0
\qquad \forall \; i,\; s,t
\end{aligned}
$$
<!-- source: source/flow_right_lp.cpp:246-274 -->

**Consumptive coupling.** When a `junction` reference is set, each
block's flow column is injected with coefficient $-1$ into the junction
balance row of §5.10, subtracting the extraction from the physical
water balance.
<!-- source: source/flow_right_lp.cpp:278-293 -->

#### 5.13.2 Volume Rights

A `VolumeRight` is a virtual storage (built on the `StorageLP` base)
that tracks the remaining rights volume $\nu_{o,s,t,b}$ [hm³]. The
conversion factor $\alpha_o = \text{fcr}_o$ has default value
$0.0036$ hm³/((m³/s)·h) so that an extraction rate in m³/s integrated
over one hour produces a volume decrement in hm³.

**Balance (StorageLP template specialised for rights).**

$$
\begin{aligned}
\nu_{o,s,t,b} \;=\; \nu_{o,s,t,b-1} \cdot (1 - \mu_o^h \Delta_b) \;+\; \alpha_o\,\Delta_b\,\xi_{o,s,t,b}^{\text{sav}} \;-\; \alpha_o\,\Delta_b\,\xi_{o,s,t,b}^{\text{in}}
\qquad \forall \; o \in \mathcal{VR},\; s,t,b
\end{aligned}
$$
<!-- source: source/volume_right_lp.cpp:161-214 (StorageBase::add_to_lp) -->

where $\xi_{o,s,t,b}^{\text{sav}}$ is an optional *saving* (deposit)
column created when a `saving_rate` schedule is present
(economy/reserved rights).

**Extraction bounds.**

$$
\begin{aligned}
0 \;\leq\; \xi_{o,s,t,b}^{\text{in}} \;\leq\; \min\!\bigl(\overline{F}_{o,t,b},\; B_o(x)\bigr)
\qquad \forall \; o,\; s,t,b
\end{aligned}
$$
<!-- source: source/volume_right_lp.cpp:117-137 -->

**Optional stage demand.** When $D_o > 0$, a deficit
$q_{o,s,t}^{\text{fail}} \geq 0$ with cost $c_o^f$ is added with the
stage-integrated satisfaction row:

$$
\begin{aligned}
\sum_{b \in \mathcal{B}_t} \alpha_o\,\Delta_b\,\xi_{o,s,t,b}^{\text{in}} \;+\; q_{o,s,t}^{\text{fail}} \;\geq\; D_{o,t}
\qquad \forall \; o,\; s,t
\end{aligned}
$$
<!-- source: source/volume_right_lp.cpp:313-346 -->

**Seasonal reset.** When a `reset_month` is configured and the current
stage's calendar month matches, the initial-volume column is clamped to
the provisioned value for the new hydrological period:

$$
\begin{aligned}
\nu_{o,s,t,b_0} \;=\; \begin{cases}
B_o(x) & \text{if } \texttt{bound\_rule} \text{ is set} \\
\overline{V}_{o,t} & \text{otherwise}
\end{cases}
\end{aligned}
$$
<!-- source: source/volume_right_lp.cpp:230-237 -->

**Consumptive coupling.** When a `reservoir` reference is set, the
extraction column is injected with coefficient $+\alpha_o\,\Delta_b$
into the energy balance row of the linked reservoir (§5.10), debiting
the physical water storage. A `right_reservoir` link analogously
couples a child right to a parent rights ledger.
<!-- source: source/volume_right_lp.cpp:278-307 -->

### 5.14 LNG Terminal

An `LngTerminal` element $\tau \in \mathcal{LNG}$ models an LNG storage
tank whose inventory is coupled to gas-fired generators through their
heat rates. The formulation reuses the `StorageLP` balance template
(§5.7) with the following specialisations:

- `finp` = delivery inflow column $d_{\tau,s,t,b}^{\text{lng}}$
  (fixed-rate across the stage: bounds set to $D_\tau/T_t$);
- `fout` = 0 (no direct outflow — generator consumption is injected
  into the balance row below);
- `drain` = venting column with penalty `spillway_cost`;
- `annual_loss` = boil-off rate $\lambda_\tau$.

<!-- source: source/lng_terminal_lp.cpp:62-132 -->

#### Tank Volume Balance

$$
\begin{aligned}
e_{\tau,s,t,b}^{\text{lng}} \;=\;& e_{\tau,s,t,b-1}^{\text{lng}} \cdot (1 - \lambda_\tau^h\,\Delta_b) \\
& \;+\; \rho_\tau\,\Delta_b\,d_{\tau,s,t,b}^{\text{lng}} \\
& \;-\; \rho_\tau\,\Delta_b\,\text{vent}_{\tau,s,t,b} \\
& \;-\; \rho_\tau\,\Delta_b\,\sum_{g \in \mathcal{G}_\tau} \eta_g^{hr}\, p_{g,s,t,b}
\qquad \forall \; \tau,\; s,t,b
\end{aligned}
$$
<!-- source: source/lng_terminal_lp.cpp:134-164 -->

where $\mathcal{G}_\tau$ is the set of generators linked to terminal
$\tau$, $\eta_g^{hr}$ is the generator's heat rate in m³/MWh, and
$\lambda_\tau^h = \lambda_\tau / 8760$.

The last term is added directly to the existing energy balance row by
setting

$$
\text{coeff}(\text{balance}_b, \; p_{g,s,t,b}) \;=\; \rho_\tau \cdot \eta_g^{hr} \cdot \Delta_b
$$

for each linked generator and each block.

#### Delivery Schedule

$$
\begin{aligned}
d_{\tau,s,t,b}^{\text{lng}} \;=\; \frac{D_{\tau,t}}{T_t}
\qquad \forall \; \tau,\; s,t,b
\end{aligned}
$$
<!-- source: source/lng_terminal_lp.cpp:71-88 -->

Volume bounds $\underline{V}_\tau \leq e_{\tau,s,t,b}^{\text{lng}} \leq \overline{V}_\tau$
and initial/final conditions follow the generic storage template (§5.7).

### 5.15 Unit Commitment (3-bin)

A `Commitment` element attaches to a generator $g \in \mathcal{UC}$ and
introduces the classical three-binary (status / startup / shutdown)
unit-commitment formulation. Status, startup and shutdown variables
live on the **commitment period** grid $p \in \mathcal{P}_t^g$ (a
coarsening of the block grid so that startup decisions align with
enforcement periods); each block within the period references the same
status column $u_{g,s,t,p(b)}$.

#### Variables

$$
\begin{aligned}
u_{g,s,t,p}, \; v_{g,s,t,p}, \; w_{g,s,t,p} \;\in\; \{0,1\}
\qquad \forall \; g \in \mathcal{UC},\; s,t,p
\end{aligned}
$$
<!-- source: source/commitment_lp.cpp:219-268 -->

(Binary is relaxed to $[0,1]$ continuous when `relax = true` or in SDDP
continuous phases.)

#### C1 — Status Transition Logic

$$
\begin{aligned}
u_{g,s,t,p} \;-\; u_{g,s,t,p-1} \;-\; v_{g,s,t,p} \;+\; w_{g,s,t,p} \;=\; 0
\qquad \forall \; g,\; s,t,p > 0
\end{aligned}
$$

For the first period the RHS becomes the initial status:

$$
\begin{aligned}
u_{g,s,t,0} \;-\; v_{g,s,t,0} \;+\; w_{g,s,t,0} \;=\; u_g^{\text{init}}
\qquad \forall \; g,\; s,t
\end{aligned}
$$
<!-- source: source/commitment_lp.cpp:272-289 -->

#### C2 — Startup/Shutdown Exclusion

$$
\begin{aligned}
v_{g,s,t,p} \;+\; w_{g,s,t,p} \;\leq\; 1
\qquad \forall \; g,\; s,t,p
\end{aligned}
$$
<!-- source: source/commitment_lp.cpp:291-304 -->

#### C3 — Generation Bounds Coupled to Status

$$
\begin{aligned}
p_{g,s,t,b} \;-\; \overline{P}_{g,t,b}\,u_{g,s,t,p(b)} \;\leq\; 0
\qquad \forall \; g,\; s,t,b
\end{aligned}
$$
<!-- source: source/commitment_lp.cpp:346-362 -->

$$
\begin{aligned}
p_{g,s,t,b} \;-\; \underline{P}_{g,t,b}\,u_{g,s,t,p(b)} \;\geq\; 0
\qquad \forall \; g,\; s,t,b
\end{aligned}
$$
<!-- source: source/commitment_lp.cpp:364-377 -->

#### C4 — Ramp-Up

$$
\begin{aligned}
p_{g,s,t,b} \;-\; p_{g,s,t,b-1} \;-\; R_g^{\text{up}}\Delta_b\,u_{g,s,t,p(b-1)} \;-\; \text{SU}_g^{\text{ramp}}\,v_{g,s,t,p(b)} \;\leq\; 0
\qquad \forall \; g,\; s,t,b > 0
\end{aligned}
$$
<!-- source: source/commitment_lp.cpp:379-404 -->

#### C5 — Ramp-Down

$$
\begin{aligned}
p_{g,s,t,b-1} \;-\; p_{g,s,t,b} \;-\; R_g^{\text{dn}}\Delta_b\,u_{g,s,t,p(b)} \;-\; \text{SD}_g^{\text{ramp}}\,w_{g,s,t,p(b)} \;\leq\; 0
\qquad \forall \; g,\; s,t,b > 0
\end{aligned}
$$
<!-- source: source/commitment_lp.cpp:406-430 -->

#### C6 — Minimum Up/Down Time (Optional)

When $T_g^{\text{up}} > 0$, for each period $p$ let $\mathcal{Q}(p)$
be the set of periods covering the next $T_g^{\text{up}}$ hours:

$$
\begin{aligned}
\sum_{q \in \mathcal{Q}(p)} v_{g,s,t,q} \;-\; u_{g,s,t,p} \;\leq\; 0
\qquad \forall \; g,\; s,t,p
\end{aligned}
$$
<!-- source: source/commitment_lp.cpp:562-618 -->

Symmetrically for minimum down time with $w$ and $1-u$.
<!-- source: source/commitment_lp.cpp:626-681 -->

#### Costs

The objective contribution of a committed unit is:

$$
\omega_{s,t,b}\,\Bigl[
  \text{NL}_g \cdot T_p \cdot u_{g,s,t,p}
  \;+\; \text{SU}_g \cdot v_{g,s,t,p}
  \;+\; \text{SD}_g \cdot w_{g,s,t,p}
\Bigr]
$$

(no-load cost scales with period duration $T_p$; startup/shutdown are
per-event).
<!-- source: source/commitment_lp.cpp:216-267 -->

### 5.16 Simple Commitment

A `SimpleCommitment` element attaches to a generator $g \in \mathcal{SC}$
and provides a **stripped-down** commitment variant: one binary status
variable per block, no startup/shutdown bookkeeping, no ramp
constraints, no min up/down time.

#### Variable

$$
\begin{aligned}
u_{g,s,t,b}^{\text{sc}} \;\in\; \{0,1\}
\qquad \forall \; g \in \mathcal{SC},\; s,t,b
\end{aligned}
$$
<!-- source: source/simple_commitment_lp.cpp:86-96 -->

When `must_run = true`, the column is clamped to $1$ via
$\underline{u}^{\text{sc}} = 1$ (i.e. $u_{g,s,t,b}^{\text{sc}} = 1$).
When `relax = true` or in SDDP continuous phases the integrality is
dropped.

#### Generation Bounds

$$
\begin{aligned}
p_{g,s,t,b} \;-\; \overline{P}_{g,t,b}\,u_{g,s,t,b}^{\text{sc}} \;\leq\; 0
\qquad \forall \; g,\; s,t,b
\end{aligned}
$$
<!-- source: source/simple_commitment_lp.cpp:102-115 -->

$$
\begin{aligned}
p_{g,s,t,b} \;-\; \underline{P}_{g,t,b}^{d}\,u_{g,s,t,b}^{\text{sc}} \;\geq\; 0
\qquad \forall \; g,\; s,t,b
\end{aligned}
$$
<!-- source: source/simple_commitment_lp.cpp:117-130 -->

where $\underline{P}_{g,t,b}^{d}$ (`dispatch_pmin`) defaults to the
generator's own $\underline{P}_g$ when unset.

### 5.17 Inertia Zone and Provision

Mirroring the reserve-zone formulation (§5.9), gtopt models
**synchronous inertia** as two coupled elements: `InertiaZone`
($z \in \mathcal{IZ}$) defines a per-block inertia requirement
$\overline{R}_{z,t,b}^{H}$ in MWs, and `InertiaProvision`
($p \in \mathcal{IP}$) links a generator's MW production to a fraction
of this MWs requirement.

#### Zone Requirement (InertiaZone)

For each block a slack column $q_{z,s,t,b}^{H}$ is created, and the
requirement row is written as a `>= 0` row with the slack contributing
with coefficient $-1$ on the LHS. InertiaProvisions add their
contributions through the zone requirement row.

When the reserve-fail cost is finite, the slack has bounds
$[0,\overline{R}_{z,t,b}^{H}]$ and a negative objective coefficient
(penalty); when it is infinite, the slack is clamped
($\overline{R}^{H} \leq q^{H} \leq \overline{R}^{H}$) forcing
feasibility via provisions only.

$$
\begin{aligned}
\sum_{p \in \mathcal{P}_z} \Phi_{p,t}\,r_{p,s,t,b}^{H} \;-\; q_{z,s,t,b}^{H} \;\geq\; 0
\qquad \forall \; z \in \mathcal{IZ},\; s,t,b
\end{aligned}
$$
<!-- source: source/inertia_zone_lp.cpp:56-86 -->
<!-- source: source/inertia_provision_lp.cpp:175-191 (factor injection) -->

Because the slack enforces $q_{z,s,t,b}^{H} \geq \overline{R}_{z,t,b}^{H}$
(via its lower bound when penalised, or equality otherwise) and enters
the row with $-1$, the net effect is
$\sum_p \Phi_{p,t} r_{p,s,t,b}^{H} \geq \overline{R}_{z,t,b}^{H}$ —
unserved inertia is absorbed by the priced slack.

#### Provision Coupling (InertiaProvision)

Each provision creates a column $r_{p,s,t,b}^{H}$ with upper bound
`provision_max` (falling back to the generator's own $\underline{P}_g$
when unset) and an explicit cost:

$$
\begin{aligned}
0 \;\leq\; r_{p,s,t,b}^{H} \;\leq\; \overline{R}_{p,t,b}^{H}
\qquad \forall \; p \in \mathcal{IP},\; s,t,b
\end{aligned}
$$
<!-- source: source/inertia_provision_lp.cpp:143-159 -->

A **downward coupling** row ties the generator output to the reserved
MW:

$$
\begin{aligned}
p_{g,s,t,b} \;-\; r_{p,s,t,b}^{H} \;\geq\; 0
\qquad \forall \; p \in \mathcal{IP},\; s,t,b
\end{aligned}
$$
<!-- source: source/inertia_provision_lp.cpp:161-173 -->

#### Provision Factor

$r_{p,s,t,b}^{H}$ is in MW; the zone requirement in MWs. The
conversion factor

$$
\Phi_{p,t} \;=\; \begin{cases}
\text{provision\_factor}_{p,t} & \text{(explicit schedule)} \\[2pt]
H_p \cdot S_p / \underline{P}_g & \text{if } H_p,\,S_p \text{ set and } \underline{P}_g > 0 \\[2pt]
1 & \text{otherwise}
\end{cases}
$$

is injected as the coefficient on $r_{p,s,t,b}^{H}$ in every linked
zone's requirement row.
<!-- source: source/inertia_provision_lp.cpp:127-138,175-191 -->

> **Note on renumbering.** Capacity Expansion (§5.11) and User
> Constraints (§5.12) retain their numbering even though §5.13–§5.17
> add new content: §5.12 continues to reference *all* of the above
> element-specific variables and rows, so it remains the natural final
> subsection of §5.

---

## 6. Scaling and Solver Options

### 6.1 Objective Scaling

All cost coefficients are divided by `scale_objective` ($\sigma$, default
1000) to improve solver numerical conditioning. The reported objective value
in `solution.csv` is the **scaled** value:

$$
z_{\text{reported}} = \frac{z_{\text{actual}}}{\sigma}
$$

Multiply by $\sigma$ to recover the cost in original monetary units.

### 6.2 Voltage Angle Scaling

Voltage angles are scaled by `scale_theta` ($\sigma_\theta$, default 1000)
to keep angle variables and susceptance coefficients in a numerically
well-conditioned range.

### 6.3 Variable Scaling

Beyond the global objective and angle scaling factors, gtopt supports
**per-variable scale factors** that improve LP solver numerics when
physical quantities span very different magnitudes (e.g., reservoir
volumes in dam$^3$ vs. power flows in MW).

#### Convention

All variable scales follow a single convention:

$$
x_{\text{physical}} = x_{\text{LP}} \times \sigma_x
$$

where $\sigma_x$ is the scale factor and $x_{\text{LP}}$ is the LP
decision variable. Bounds, coefficients, and right-hand sides are
adjusted consistently so that the **optimal physical solution is
invariant to the choice of scale**.

#### Component-Specific Scales

| Component | Field | Symbol | Default | Effect |
|-----------|-------|--------|---------|--------|
| Battery | `variable_scales` (energy) | $\sigma_E$ | 1.0 | SoC variable: $E_{\text{phys}} = E_{\text{LP}} \times \sigma_E$ |
| Reservoir | `variable_scales` (energy) | $\sigma_V$ | 1.0 | Volume variable: $V_{\text{phys}} = V_{\text{LP}} \times \sigma_V$ |
| Bus | `scale_theta` | $\sigma_\theta$ | 1000 | Angle variable: $\theta_{\text{phys}} = \theta_{\text{LP}} / \sigma_\theta$ |

When a scale factor $\sigma_x$ is applied to a storage variable, the LP
formulation adjusts:

- **Variable bounds**: $x_{\text{LP}} \in [\underline{x}/\sigma_x,\; \overline{x}/\sigma_x]$
- **Objective coefficients**: multiplied by $\sigma_x$ so that
  $c \cdot x_{\text{LP}} \cdot \sigma_x = c \cdot x_{\text{phys}}$
- **Constraint coefficients**: adjusted so that physical-unit
  relationships are preserved (e.g., the SoC balance row coefficients
  absorb the ratio $\sigma_{\text{flow}} / \sigma_E$)

#### Generic Variable Scales (`variable_scales`)

The `options.variable_scales` array provides a uniform mechanism for
scaling any LP variable by element class, variable name, and optional
element UID:

```json
{
  "options": {
    "variable_scales": [
      {"class_name": "Reservoir", "variable": "volume", "scale": 1000.0},
      {"class_name": "Battery", "variable": "energy", "uid": 3, "scale": 10.0}
    ]
  }
}
```

Resolution priority:

1. Per-element override (matching class + variable + UID)
2. Per-class default (matching class + variable, no UID)
3. Fallback: 1.0 (no scaling)

Global options (`scale_theta`) take precedence over entries in
`variable_scales`.

> **⚠️ Important: Reservoir scaling for large hydro systems.**
> The default scale `1.0` for reservoirs is adequate for small
> systems, but will cause severe LP numerical ill-conditioning for
> large-scale hydrothermal systems. A reservoir with maximum volume
> $V_{\max} = 6 \times 10^6$ dam³ (Lake Laja scale) combined with a
> generator cost coefficient of $\sim 0.1$ $/MWh produces an LP coefficient
> ratio exceeding $10^8$ — well outside the safe range of $10^5$–$10^7$.
>
> **Best practice:** Set the energy scale to `max(1, emax / 1000)` via
> `variable_scales` so that LP volume variables are normalised to the
> $[0, 1000]$ range:
>
> ```json
> {
>   "options": {
>     "variable_scales": [
>       {"class_name": "Reservoir", "variable": "energy", "uid": 1, "scale": 6000.0},
>       {"class_name": "Reservoir", "variable": "energy", "uid": 2, "scale": 1500.0}
>     ]
>   }
> }
> ```
>
> Or use a uniform default with `uid = -1` for all reservoirs:
>
> ```json
> {
>   "options": {
>     "variable_scales": [
>       {"class_name": "Reservoir", "variable": "energy", "uid": -1, "scale": 1000.0}
>     ]
>   }
> }
> ```
>
> This matches the PLP `ScaleVol` convention (`ScaleVol = max(1, Vmax/1000)`).
> Run with `--stats` and check the LP coefficient ratio in the log output;
> a ratio above $10^7$ is a warning sign that scaling should be tuned.

### 6.4 Key Options Affecting the Formulation

| Option | JSON field | Default | Effect on formulation |
|--------|-----------|---------|----------------------|
| Kirchhoff mode | `use_kirchhoff` | `true` | Enables DC OPF constraints (§5.6) |
| Single bus | `use_single_bus` | `false` | Disables all network constraints (copper plate) |
| Line losses | `use_line_losses` | `true` | Enables loss modeling (§5.5) |
| Kirchhoff threshold | `kirchhoff_threshold` | `0` | Minimum bus count for Kirchhoff activation |
| Objective scale | `scale_objective` | `1000` | Divides all cost coefficients |
| Theta scale | `scale_theta` | `1000` | Scales voltage angle variables |
| Annual discount rate | `annual_discount_rate` | `0.0` | Multi-year cost discounting (now in `simulation`) |
| Demand fail cost | `demand_fail_cost` | *(none)* | Enables load curtailment variables |
| Reserve fail cost | `reserve_fail_cost` | *(none)* | Enables reserve shortage variables |
| Input format | `input_format` | `"parquet"` | Time-series input format |
| Output format | `output_format` | `"parquet"` | Solution output format |
| Output compression | `output_compression` | `"zstd"` | Parquet compression codec |

### 6.5 Modeling Modes Summary

The three network modeling modes correspond to standard formulations in the
power systems literature [[11]](#ref11) [[12]](#ref12):

| Mode | Conditions | Variables | Constraints |
|------|-----------|-----------|-------------|
| **Copper plate** | `use_single_bus = true` | $p, \ell, q$ | Global balance only |
| **Transport model** | `use_kirchhoff = false`, multi-bus | $p, \ell, q, f$ | Bus balance + line capacity |
| **DC OPF** | `use_kirchhoff = true`, multi-bus | $p, \ell, q, f, \theta$ | Bus balance + Kirchhoff VL + line capacity |

### 6.6 Stochastic Dual Dynamic Programming (SDDP)

When solving multi-stage problems with many scenarios, gtopt supports the
**Stochastic Dual Dynamic Programming (SDDP)** algorithm
[[4]](#ref4) [[5]](#ref5) [[6]](#ref6). SDDP decomposes the full problem
into a series of smaller LP sub-problems linked by **Benders cuts** that
approximate the expected future cost.

#### Problem Decomposition

For a planning horizon of $T$ stages and $S$ scenarios, the SDDP algorithm
alternates between:

1. **Forward pass** — solve each stage LP sequentially, using available
   cuts to approximate future costs:

$$
\underset{x_t}{\min} \;\; c_t^\top x_t + \alpha_{t+1}
\quad \text{s.t.} \quad A_t x_t = b_t(s),\; x_t \ge 0,\; \alpha_{t+1} \ge \hat{\alpha}_{t+1}
$$

   where $\alpha_{t+1}$ is the future-cost approximation (Benders value
   function) and $\hat{\alpha}_{t+1}$ are the cuts accumulated across
   iterations.

2. **Backward pass** — starting from the last stage, compute a new Benders
   cut from the dual solution of the stage LP and add it to the previous
   stage:

$$
\hat{\alpha}_t^{(k)} : \alpha_t \ge z_t^{(k)} +
\sum_i \bar{\pi}_i^{(k)} \bigl( x_{t-1,i} - \hat{x}_{t-1,i}^{(k)} \bigr)
$$

   where $z_t^{(k)}$ is the optimal value of the stage-$t$ LP in iteration
   $k$, $\bar{\pi}_i^{(k)}$ are the dual prices (reduced costs) of the
   linking constraints, and $\hat{x}_{t-1,i}^{(k)}$ is the trial solution
   from the forward pass.

#### Convergence

The algorithm uses two convergence criteria, evaluated at the **end of each
training iteration** (after a paired forward + backward pass).  The final
simulation pass (a forward-only evaluation) does **not** determine
convergence — it inherits the converged status from the last training
iteration.

##### Primary criterion — gap tolerance

The solver declares convergence when the **relative optimality gap** falls
below a tolerance $\varepsilon$:

$$
\text{gap}^{(k)} = \frac{\text{UB}^{(k)} - \text{LB}^{(k)}}{\max\bigl(1, \lvert \text{UB}^{(k)} \rvert\bigr)} < \varepsilon
\quad \text{and} \quad k \ge k_{\min}
$$

where:
- $\text{LB}^{(k)}$ = average phase-0 objective across scenes (lower bound
  on the expected cost);
- $\text{UB}^{(k)}$ = average total forward-pass cost across scenes (upper
  bound, feasible but not optimal);
- $\varepsilon$ = `convergence_tol` (default $10^{-4}$);
- $k_{\min}$ = `min_iterations` (default 2).

##### Secondary criterion — stationary gap

Some SDDP/Benders problems converge to a **non-zero stationary gap** due
to stochastic noise, problem structure, or numerical conditioning.  In
these cases the primary gap tolerance $\varepsilon$ is never reached, but
the gap stops improving.  The **stationary-gap criterion** detects this
plateau and declares convergence when the gap has not changed significantly
over a look-back window:

$$
\text{gap\_change}^{(k)} =
\frac{\lvert \text{gap}^{(k)} - \text{gap}^{(k - w)} \rvert}
     {\max\bigl(10^{-10},\; \text{gap}^{(k - w)}\bigr)}
< \varepsilon_s
\quad \text{and} \quad k \ge k_{\min}
\quad \text{and} \quad k \ge w
$$

where:
- $w$ = `stationary_window` (default 10) — number of iterations to look
  back;
- $\varepsilon_s$ = `stationary_tol` (default 0.0 = disabled) — relative
  gap-change tolerance;
- the denominator uses $\text{gap}^{(k-w)}$ (the earlier gap value) as the
  reference, with $10^{-10}$ as a floor to prevent division by zero when the
  gap is near zero.

The secondary criterion only fires when $\varepsilon_s > 0$ and the
primary criterion has **not** been met.  When triggered, the solver sets
both `converged = true` and `stationary_converged = true` in the
iteration result.

##### Convergence output

The `solution.csv` output file includes two convergence columns:

| Column | Description |
|--------|-------------|
| `gap` | Final relative gap $(UB - LB) / \max(1, \lvert UB \rvert)$ |
| `gap_change` | Final gap-change metric (1.0 if stationary criterion disabled) |

| JSON field | Symbol | Default | Description |
|------------|--------|---------|-------------|
| `convergence_tol` | $\varepsilon$ | $10^{-4}$ | Relative gap tolerance (primary) |
| `min_iterations` | $k_{\min}$ | 2 | Minimum training iterations before convergence |
| `max_iterations` | $k_{\max}$ | 100 | Maximum training iterations (hard stop) |
| `stationary_tol` | $\varepsilon_s$ | 0.0 | Stationary gap-change tolerance (0 = disabled) |
| `stationary_window` | $w$ | 10 | Look-back window for stationary gap check |

#### Cut Sharing

Cuts generated in one scenario can be shared across scenarios to accelerate
convergence. Three modes are supported:

| Mode | Description |
|------|-------------|
| `none` | No sharing — each scene uses only its own cuts |
| `expected` | Compute probability-weighted average cut; share to all scenes |
| `max` | Share every cut from every scene to all other scenes |

### 6.7 Apertures — Backward-Pass Scenario Sampling

An **aperture** (from the Portuguese *abertura hidrológica* in PLP) is a
hydrological or stochastic realization used during the SDDP backward pass
to compute the expected future-cost cut. Apertures provide a Monte Carlo
sample of uncertain parameters (typically river inflows / affluents) that
are different from the forward-pass scenarios.

#### Aperture Set $\mathcal{A}$

Let $\mathcal{A} = \{1, \ldots, A\}$ be the set of apertures with
probability weights $\{\rho_a\}_{a \in \mathcal{A}}$ normalised so that

$$
\sum_{a \in \mathcal{A}} \rho_a = 1.
$$

Each aperture $a$ references a **source scenario** $s_a$ whose affluent
data (flow column bounds) are applied to the cloned backward-pass LP.

#### Expected Benders Cut

Instead of a single backward-pass solve, the solver generates one cut per
aperture and computes the **probability-weighted average** cut:

$$
\hat{\alpha}_t^{(k)} : \alpha_t \;\ge\;
\sum_{a \in \mathcal{A}} \rho_a \Bigl[
  z_{t,a}^{(k)} +
  \sum_i \bar{\pi}_{i,a}^{(k)} \bigl( x_{t-1,i} - \hat{x}_{t-1,i,a}^{(k)} \bigr)
\Bigr]
$$

where $z_{t,a}^{(k)}$ and $\bar{\pi}_{i,a}^{(k)}$ are the objective value
and dual prices from solving the stage-$t$ LP with aperture $a$'s parameter
values.

#### JSON Configuration

```json
{
  "simulation": {
    "aperture_array": [
      {"uid": 1, "source_scenario": 1, "probability_factor": 0.5},
      {"uid": 2, "source_scenario": 5, "probability_factor": 0.3},
      {"uid": 3, "source_scenario": 10, "probability_factor": 0.2}
    ]
  }
}
```

| Field | Symbol | Description |
|-------|--------|-------------|
| `source_scenario` | $s_a$ | UID of the scenario providing affluent data |
| `probability_factor` | $\rho_a$ (pre-normalisation) | Weight of this aperture in the expected cut |

When `aperture_array` is absent, the solver falls back to the legacy
`sddp_num_apertures` option: `N > 0` uses the first $N$ scenarios
(equal weights), `N = -1` uses all scenarios, `N = 0` disables apertures.

#### Boundary Cuts

Boundary cuts approximate the expected future cost **beyond the planning
horizon** — i.e. the value function at the terminal stage.  They are
analogous to PLP's "planos de embalse" (reservoir future-cost function).
Each cut constrains the future-cost variable $\alpha$ at the last phase:

$$
\alpha_{T} \;\ge\; \beta_0^{(k)} \;+\; \sum_{i} \rho_i^{(k)} \cdot x_{i,T}
$$

where $x_{i,T}$ are the state variables (reservoir volumes, battery SoC)
at the last phase $T$, $\rho_i^{(k)}$ are gradient coefficients, and
$\beta_0^{(k)}$ is the intercept.  Index $k$ runs over the set of
external cuts loaded from the boundary cuts CSV file.

The boundary cuts are loaded according to three modes:

- **`"noload"`** — skip loading (no terminal value function).
- **`"separated"`** (default) — each cut is assigned to the scene whose
  UID matches the `scene` column in the CSV.  This corresponds to PLP's
  per-simulation future-cost planes.
- **`"combined"`** — all cuts are broadcast to all scenes.

An optional `sddp_boundary_max_iterations` parameter limits loading to
the last $N$ distinct SDDP iterations (by the `iteration` column in the
CSV, corresponding to PLP's `IPDNumIte`).

> **See also**: [`docs/SDDP_SOLVER.md`](../methods/sddp.md) §4.11 for
> the CSV format specification and configuration details.

> **See also**: [`docs/SDDP_SOLVER.md`](../methods/sddp.md) for the
> complete SDDP algorithm description, convergence criteria, and
> implementation notes.

### 6.8 SDDP Benders Cuts

The SDDP algorithm builds a piecewise-linear outer approximation of the
future cost function $\alpha_t$ by accumulating **Benders cuts** across
iterations. Each cut is a linear inequality added to the LP of phase
$t{-}1$.

#### Optimality Cuts

An **optimality cut** is generated from the dual solution of a feasible
phase-$t$ LP. At iteration $k$:

$$
\alpha_{t-1} \;\ge\; z_t^{(k)} + \sum_{i \in \mathcal{S}_t}
\text{rc}_i^{(k)} \cdot \bigl( x_{t-1,i} - \hat{v}_i^{(k)} \bigr)
\qquad \forall \; k \in \mathcal{K}_{\text{opt}}
$$

where:
- $z_t^{(k)}$ is the optimal objective of the phase-$t$ LP (including
  its own $\alpha_t$ term)
- $\mathcal{S}_t$ is the set of state-variable links between phases
  $t{-}1$ and $t$
- $\text{rc}_i^{(k)}$ is the reduced cost of the dependent column for
  state variable $i$ in phase $t$
- $x_{t-1,i}$ is the source column (decision variable) in phase $t{-}1$
- $\hat{v}_i^{(k)}$ is the trial value from the forward pass

Expanding and rearranging, the cut takes the canonical LP row form:

$$
\alpha_{t-1} - \sum_{i} \text{rc}_i^{(k)} \cdot x_{t-1,i}
\;\ge\; z_t^{(k)} - \sum_{i} \text{rc}_i^{(k)} \cdot \hat{v}_i^{(k)}
$$

The right-hand side is stored as the cut's `rhs` field and the
coefficients $\text{rc}_i^{(k)}$ are stored as `(column_index, coefficient)`
pairs in the `StoredCut` structure.

#### Feasibility Cuts

When the forward-pass subproblem at phase $t$ is **infeasible** (the
trial values from phase $t{-}1$ violate constraints), the solver applies
an elastic filter (see [Section 6.13](#613-elastic-filter-and-feasibility))
to obtain dual information. The resulting **feasibility cut** has the
same algebraic form as an optimality cut but is derived from the
elastic-clone's dual solution rather than the original LP.

In gtopt, the `CutType` enum distinguishes the two:

| `CutType` | Origin | Shared across scenes? |
|-----------|--------|----------------------|
| `Optimality` | Feasible backward-pass solve | Yes (per `cut_sharing_mode`) |
| `Feasibility` | Elastic filter clone | Never |

### 6.9 Boundary Cuts and Future Cost

Boundary cuts approximate the expected future cost **beyond the planning
horizon** $T$ -- the terminal value function $\alpha_T$. They are
loaded from an external CSV file and added to the last phase of each
scene's LP before solving begins.

Each boundary cut $k$ constrains the future-cost variable at the last
phase:

$$
\alpha_T \;\ge\; \beta_0^{(k)} + \sum_{i} \rho_i^{(k)} \cdot x_{i,T}
\qquad \forall \; k \in \mathcal{K}_{\text{boundary}}
$$

where $x_{i,T}$ are the state variables (reservoir volumes, battery SoC)
at phase $T$, $\rho_i^{(k)}$ are gradient coefficients, and
$\beta_0^{(k)}$ is the intercept (RHS).

Boundary cuts are analogous to PLP's "planos de embalse" (reservoir
future-cost function). They are supported by both the SDDP and
monolithic solvers. When used with the monolithic solver, an $\alpha$
variable is added to the last phase and the cuts are applied identically.

> **See also**: [`docs/SDDP_SOLVER.md`](../methods/sddp.md) Section 4.11
> for the CSV format specification, load modes, and iteration filtering.

### 6.10 Hot Start

The SDDP solver supports **hot-starting** from previously saved cuts to
accelerate convergence. When `hot_start` is enabled:

1. The solver loads all valid cut files from the cut directory
   (files matching `scene_<N>.csv` or `sddp_cuts.csv`)
2. Error files (prefixed with `error_`) from infeasible scenes in
   previous runs are automatically **skipped**
3. Loaded cuts are injected into the per-phase LPs before the first
   forward pass, providing an initial outer approximation of the
   future cost function
4. The SDDP iterations then refine this approximation further

Hot start is particularly useful for:
- **Interrupted runs**: if the solver was stopped (sentinel file, time
  limit, or external signal), the saved cuts allow resumption without
  losing progress
- **Parameter sensitivity**: re-solving with slightly modified parameters
  while retaining the bulk of the cut approximation
- **Incremental refinement**: running additional iterations on top of a
  previous solution

Configuration:
```json
{
  "options": {
    "sddp_options": {
      "hot_start": true,
      "cut_directory": "cuts"
    }
  }
}
```

### 6.11 Solver Timeouts

LP subproblem solves have configurable time limits to prevent the solver
from stalling on difficult instances.

| Option | Scope | Default | Description |
|--------|-------|---------|-------------|
| `solve_timeout` | SDDP forward pass | 0 (none) | Time limit per LP solve (seconds) |
| `aperture_timeout` | SDDP backward pass | 0 (none) | Time limit per aperture LP solve (seconds) |
| `monolithic_solve_timeout` | Monolithic solver | 0 (none) | Time limit per scene LP solve (seconds) |

When a timeout is exceeded:
- The LP is saved to a debug file in `log_directory`
- A CRITICAL message is logged with the phase, scene, and elapsed time
- For SDDP: the scene is marked as failed for that iteration
- For apertures: the timed-out aperture is skipped and the solver
  continues with remaining apertures

### 6.12 Monolithic vs SDDP Equivalence

Under specific conditions, the monolithic and SDDP solvers produce
**identical optimal solutions**.

**Theorem (Finite convergence).**
For a deterministic problem (single scenario $|\mathcal{S}| = 1$) with
$T$ phases, linear cost-to-go functions, and no integer variables, the
SDDP algorithm converges finitely to the exact monolithic optimum
[[3]](#ref3)[[4]](#ref4).

**Formal statement.** Let $z^*_{\text{mono}}$ be the monolithic optimal
value:

$$
z^*_{\text{mono}} = \min \sum_{t=1}^{T} c_t^\top x_t
\quad \text{s.t.} \quad A_t x_t + B_t x_{t-1} \ge b_t,\;
x_t \ge 0 \;\; \forall \; t
$$

and let $z^{(k)}_{\text{SDDP}}$ be the SDDP lower bound at iteration
$k$. Then $z^{(k)}_{\text{SDDP}} \to z^*_{\text{mono}}$ in a finite
number of iterations, provided:

1. **Single scenario** (or all scenarios evaluated) -- the backward
   pass sees the same data as the forward pass
2. **No aperture sampling** -- apertures use the full scenario set
3. **No cut sharing** (`cut_sharing_mode = "none"`) -- cuts are not
   broadcast across scenes
4. **Convergence achieved** -- the gap
   $(UB - LB) / \max(1, |UB|) < \varepsilon$

When any of these conditions is violated, the SDDP solution may differ
from the monolithic optimum (typically providing a lower bound).

> **See also**: [`docs/MONOLITHIC_SOLVER.md`](../methods/monolithic.md)
> Section 3 for additional details on equivalence conditions.

### 6.13 Elastic Filter and Feasibility

When a forward-pass subproblem at phase $t$ is infeasible (the trial
values $\hat{v}_i$ from phase $t{-}1$ violate the current phase's
constraints), the SDDP solver applies an **elastic filter** to recover
dual information for cut generation.

The elastic filter procedure:

1. **Clone** the phase-$t$ LP (deep copy via `OsiSolverInterface::clone()`)
2. **Relax** fixed state-variable columns to their physical bounds
3. **Add penalized slack variables** $s_i^+, s_i^-$ for each state
   variable link:

$$
x_i + s_i^+ - s_i^- = \hat{v}_i, \qquad s_i^+, s_i^- \ge 0
$$

4. **Modify the objective** to include elastic penalties:

$$
\min \quad c^\top x + M \sum_i (s_i^+ + s_i^-)
$$

   where $M$ is the `elastic_penalty` parameter (default $10^6$)

5. **Solve** the cloned LP with the elastic objective
6. **Extract** dual information from the clone for cut generation
7. **Discard** the clone -- the original LP is never modified

Two elastic filter modes control what happens with the extracted
information:

| Mode | JSON value | Behavior |
|------|-----------|----------|
| Feasibility cut | `"cut"` | Add a Benders feasibility cut to phase $t{-}1$ |
| Backpropagate bounds | `"backpropagate"` | Tighten source column bounds in phase $t{-}1$ to the elastic-clone solution values |

The `"cut"` mode is the standard Nested Benders Decomposition approach
with theoretical convergence guarantees. The `"backpropagate"` mode is
a heuristic from the PLP hydrothermal scheduler that can converge faster
in practice for problems with tight physical bounds.

> **See also**: [`docs/SDDP_SOLVER.md`](../methods/sddp.md) Section 5.4
> for a detailed comparison of elastic filter modes.

### 6.14 Cascade Solver — Multi-Level Decomposition

The **Cascade solver** (`method = "cascade"`) extends the SDDP
algorithm with an outer loop over multiple **levels**, each with its own LP
formulation and solver parameters.  The key idea is progressive refinement:
start from a simplified model (e.g. single-bus / copper-plate) that
converges quickly, then warm-start subsequent levels with increasingly
detailed network models.

> **See also**: [`docs/CASCADE_SOLVER.md`](../methods/cascade.md) for the
> complete configuration reference, implementation details, and worked
> examples.

#### Multi-Level Structure

A cascade consists of $L$ ordered levels $\ell = 0, 1, \ldots, L-1$.
Each level defines:

- **LP formulation** $\mathcal{M}_\ell$: network topology, Kirchhoff
  constraints, line losses, scaling (via `model_options`)
- **Solver parameters** $\Theta_\ell$: iteration limits, apertures,
  convergence tolerance (via `sddp_options`)
- **Transition rules** $\mathcal{T}_\ell$: cut inheritance, target
  inheritance, dual thresholds (via `transition`)

At each level, the cascade solver internally runs an SDDP solver (§6.6)
with the specified LP and parameters.

#### Cut Inheritance

When level $\ell$ receives cuts from level $\ell{-}1$, the transfer uses
LP **column names** (not indices) for cross-LP resolution.  This allows
cuts generated with one LP structure (e.g. single-bus, where theta columns
do not exist) to be applied to a different structure (e.g. multi-bus with
Kirchhoff).

Formally, a stored optimality cut from level $\ell{-}1$:

$$
\alpha_t \;\ge\; z_t^{(k)} + \sum_{i \in \mathcal{S}}
\bar{\pi}_i^{(k)} (x_{t{-}1,i} - \hat{x}_{t{-}1,i}^{(k)})
$$

is serialized with column names $\{\text{name}(i)\}$ and deserialized at
level $\ell$ by resolving each name to the corresponding column index in
the new LP $\mathcal{M}_\ell$.  Columns that do not resolve (e.g. a theta
variable in a single-bus LP) are skipped.

**Cut forgetting**: when `inherit_optimality_cuts = N` (positive integer),
the inherited cuts are kept for at most $N$ training iterations, then
discarded.  The solver re-solves with only self-generated cuts for the
remaining iteration budget.

#### Target Inheritance (Elastic Constraints)

When level $\ell$ inherits targets from level $\ell{-}1$, state variable
values (reservoir volumes, battery SoC) from the previous level's
forward-pass solution are used as **elastic penalty constraints**:

$$
v_{\ell{-}1} - \delta \;\le\; v + s^- - s^+ \;\le\; v_{\ell{-}1} + \delta
\qquad s^+, s^- \ge 0
$$

where:
- $v_{\ell{-}1}$ = state variable value from previous level's solution
- $\delta = \max(\rho \cdot |v_{\ell{-}1}|, \;\delta_{\min})$
- $\rho$ = `target_rtol` (default 0.05 = 5%)
- $\delta_{\min}$ = `target_min_atol` (default 1.0)
- $s^+, s^-$ have objective cost `target_penalty` (default 500) per unit

The target constraints guide the optimizer towards the previous level's
solution trajectory without creating hard infeasibility.

#### Iteration Budget

The cascade supports two levels of iteration control:

- **Per-level budget**: $k^{\ell}_{\max}$ = `CascadeLevelMethod::max_iterations`
- **Global budget**: $K_{\max}$ = `CascadeOptions::sddp_options::max_iterations`

The effective per-level limit is:

$$
k^{\ell}_{\text{eff}} = \min\bigl(k^{\ell}_{\max},\; K_{\max} - \sum_{j < \ell} k^{j}_{\text{used}}\bigr)
$$

When the global budget is exhausted, the cascade stops regardless of which
level is active.

#### Convergence Behavior

- If level $\ell$ converges ($\text{gap}_\ell < \varepsilon_\ell$) at an
  intermediate position ($\ell < L-1$), the cascade **continues** to the
  next level for further refinement.
- If the **final** level ($\ell = L-1$) converges, the solver returns
  immediately.
- If the global iteration budget is exhausted, the solver stops and
  reports non-convergence.
- The overall convergence flag is taken from the last iteration result
  across all levels.

#### Typical Cascade Patterns

```
Pattern 1: Uninodal → Full Network (2 levels)
  Level 0: use_single_bus=true, Benders (no apertures), fast convergence
  Level 1: use_kirchhoff=true, SDDP with apertures, inherits targets

Pattern 2: Training → Refinement (2 levels, same LP)
  Level 0: N iterations, generate cuts
  Level 1: reuse LP, inherit cuts → converge faster

Pattern 3: Progressive (3 levels)
  Level 0: uninodal Benders → rough trajectory
  Level 1: full network + targets from L0 → guided convergence
  Level 2: reuse L1's LP + inherit cuts from L1 → fast final refinement
```

---

## 7. Mapping: JSON Fields → Mathematical Symbols

This table maps the JSON input fields (used in planning files) to the
mathematical symbols used in this formulation.

### System Options

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `options.scale_objective` | $\sigma$ | Objective scaling |
| `options.scale_theta` | $\sigma_\theta$ | Angle scaling |
| `simulation.annual_discount_rate` | $r$ | Discount rate (also accepted in `options`) |
| `options.use_kirchhoff` | — | Enable DC OPF |
| `options.use_single_bus` | — | Copper-plate mode |
| `options.use_line_losses` | — | Enable line losses |
| `options.demand_fail_cost` | $c^{\text{fail}}_d$ | Curtailment penalty |
| `options.reserve_fail_cost` | $c^{\text{rfail}}$ | Reserve penalty |
| `options.model_options.emission_cost` | $c^{\text{em}}$ | CO₂ emission price (\$/tCO₂); adds $c^{\text{em}} \times f_g$ to each generator's cost |
| `options.model_options.emission_cap` | — | Per-stage CO₂ emission cap (tCO₂/year); dual = endogenous carbon price |
| `options.method` | — | Solver: `"monolithic"` or `"sddp"` |
| `options.variable_scales` | $\sigma_x$ | Per-variable scale factors (Section 6.3) |
| `simulation.boundary_cuts_file` | — | CSV with boundary cuts for last phase (Section 6.9) |
| `simulation.boundary_cuts_valuation` | — | Valuation mode: `"end_of_horizon"` or `"present_value"` |
| `options.sddp_options.boundary_cuts_mode` | — | Load mode: `"noload"`, `"separated"`, `"combined"` |
| `options.sddp_options.boundary_max_iterations` | — | Max iterations to load from boundary cuts |
| `options.sddp_options.hot_start` | — | Enable hot-start from saved cuts (Section 6.10) |
| `options.sddp_options.save_per_iteration` | — | Save cuts after each iteration (default: true) |
| `options.sddp_options.solve_timeout` | — | LP solve time limit in seconds (Section 6.11) |
| `options.sddp_options.aperture_timeout` | — | Aperture LP time limit in seconds (Section 6.11) |

### Simulation Structure

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `simulation.scenario_array[].probability_factor` | $\pi_s$ | Scenario weight |
| `simulation.stage_array[].discount_factor` | $\delta_t^{\text{user}}$ | Stage discount |
| `simulation.block_array[].duration` | $\Delta_b$ | Block duration (h) |
| `simulation.aperture_array[].source_scenario` | $s_a$ | Source scenario UID for aperture |
| `simulation.aperture_array[].probability_factor` | $\rho_a$ | Aperture probability weight |

### Generator

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `generator_array[].gcost` | $c_g$ | Variable cost (\$/MWh) |
| `generator_array[].pmin` | $\underline{P}_g$ | Min output (MW) |
| `generator_array[].pmax` | $\overline{P}_g$ | Max output (MW) |
| `generator_array[].capacity` | $\bar{C}_g^0$ | Installed capacity (MW) |
| `generator_array[].lossfactor` | $\lambda_g$ | Injection loss |
| `generator_array[].emission_factor` | $f_g$ | CO₂ emission intensity (tCO₂/MWh) |
| `generator_array[].expcap` | $M_g$ | MW per module |
| `generator_array[].expmod` | $\overline{m}_g$ | Max modules |
| `generator_array[].annual_capcost` | $K_g^{\text{cap}}$ | \$/year per module |

### Demand

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `demand_array[].lmax` | $\overline{L}_d$ | Max demand (MW) |
| `demand_array[].lossfactor` | $\lambda_d$ | Withdrawal loss |

### Line

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `line_array[].tmax_ab` | $\overline{F}_l^{ab}$ | Capacity $a \to b$ (MW) |
| `line_array[].tmax_ba` | $\overline{F}_l^{ba}$ | Capacity $b \to a$ (MW) |
| `line_array[].reactance` | $X_l$ | Reactance (p.u.) |
| `line_array[].tcost` | $c_l$ | Transfer cost (\$/MWh) |
| `line_array[].lossfactor` | $\lambda_l$ | Transmission loss |

### Battery

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `battery_array[].emin` | $\underline{E}_e$ | Min energy (MWh) |
| `battery_array[].emax` | $\overline{E}_e$ | Max energy (MWh) |
| `battery_array[].eini` | $E_e^0$ | Initial energy (MWh) |
| `battery_array[].efin` | $E_e^{\text{fin}}$ | Final energy (MWh) |
| `battery_array[].input_efficiency` | $\eta_e^{\text{in}}$ | Charge efficiency |
| `battery_array[].output_efficiency` | $\eta_e^{\text{out}}$ | Discharge efficiency |
| `battery_array[].annual_loss` | $\mu_e$ | Annual self-discharge |
| `variable_scales` (Battery, energy) | $\sigma_E$ | Energy variable scale factor (Section 6.3) |

### Converter

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `converter_array[].conversion_rate` | $\rho_v$ | Power conversion factor |

### Reserve Zone

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `reserve_zone_array[].urreq` | $\overline{R}_z^{\text{up}}$ | Up-reserve req (MW) |
| `reserve_zone_array[].drreq` | $\overline{R}_z^{\text{dn}}$ | Down-reserve req (MW) |

### Reserve Provision

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `reserve_provision_array[].urmax` | — | Max up-reserve (MW) |
| `reserve_provision_array[].urcost` | $c_p^{\text{ur}}$ | Up-reserve cost |
| `reserve_provision_array[].drmax` | — | Max down-reserve (MW) |
| `reserve_provision_array[].drcost` | $c_p^{\text{dr}}$ | Down-reserve cost |
| `reserve_provision_array[].factor` | $\gamma_{p,t}$ | Provision factor |

### Hydro Components

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `waterway_array[].fmin` | $\underline{Q}_w$ | Min water flow (m³/s) |
| `waterway_array[].fmax` | $\overline{Q}_w$ | Max water flow (m³/s) |
| `waterway_array[].lossfactor` | $\lambda_w$ | Transport loss |
| `reservoir_array[].emin` | $\underline{V}_r$ | Min volume (hm³) |
| `reservoir_array[].emax` | $\overline{V}_r$ | Max volume (hm³) |
| `variable_scales` (Reservoir, energy) | $\sigma_V$ | Volume variable scale factor (Section 6.3) |
| `turbine_array[].conversion_rate` | $\kappa_u$ | Water-to-power factor |
| `turbine_array[].main_reservoir` | — | Reservoir for efficiency lookup |
| `reservoir_production_factor_array[].mean_production_factor` | $\bar{\kappa}_u$ | Fallback production factor |
| `reservoir_production_factor_array[].segments[].volume` | $v_i$ | Volume breakpoint (hm³) |
| `reservoir_production_factor_array[].segments[].slope` | $m_i$ | Marginal production factor per hm³ |
| `reservoir_production_factor_array[].segments[].constant` | $c_i$ | Production factor intercept |
| `reservoir_seepage_array[].constant` | $a_i$ | Default seepage constant |
| `reservoir_seepage_array[].slope` | $b_i$ | Default seepage slope |
| `reservoir_seepage_array[].segments[].volume` | $V_k$ | Volume breakpoint (hm³) |
| `reservoir_seepage_array[].segments[].slope` | $b_k$ | Seepage slope at breakpoint |
| `reservoir_seepage_array[].segments[].constant` | $c_k$ | Seepage rate at breakpoint |

### Hydro Pump

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `pump_array[].waterway` | — | FK to the pumping waterway |
| `pump_array[].demand` | — | FK to the electrical demand (pump motor load) |
| `pump_array[].pump_factor` | $\phi_h$ | Power consumed per unit flow (MW/(m³/s)) |
| `pump_array[].efficiency` | $\eta_h$ | Pump efficiency (p.u., default 1.0) |
| `pump_array[].capacity` | $\overline{Q}_h$ | Maximum pump flow (m³/s) |
| `pump_array[].main_reservoir` | — | Reservoir whose volume drives variable pump factor (SDDP) |

### LNG Terminal

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `lng_terminal_array[].emin` | $\underline{V}_\tau$ | Minimum tank volume (m³) |
| `lng_terminal_array[].emax` | $\overline{V}_\tau$ | Maximum tank volume (m³) |
| `lng_terminal_array[].eini` | $e_\tau^0$ | Initial tank volume (m³) |
| `lng_terminal_array[].efin` | $e_\tau^{\text{fin}}$ | Target final tank volume (m³) |
| `lng_terminal_array[].ecost` | — | Holding cost per m³ stored (\$/m³) |
| `lng_terminal_array[].annual_loss` | $\lambda_\tau$ | Annual boil-off rate (p.u./year) |
| `lng_terminal_array[].delivery` | $D_{\tau,t}$ | Scheduled LNG arrival volume per stage (m³) |
| `lng_terminal_array[].sendout_max` | — | Max regasification rate (m³/h) |
| `lng_terminal_array[].sendout_min` | — | Min regasification rate (m³/h) |
| `lng_terminal_array[].spillway_cost` | — | Penalty for venting LNG (\$/m³) |
| `lng_terminal_array[].spillway_capacity` | — | Maximum venting rate (m³/h) |
| `lng_terminal_array[].flow_conversion_rate` | $\rho_\tau$ | Unit-conversion factor m³/(m³/h·h) (default 1.0) |
| `lng_terminal_array[].mean_production_factor` | — | Energy content of LNG (MWh/m³, for SDDP state valuation) |
| `lng_terminal_array[].soft_emin` | — | Soft minimum tank volume (m³, penalised) |
| `lng_terminal_array[].soft_emin_cost` | — | Penalty for crossing `soft_emin` (\$/m³) |
| `lng_terminal_array[].generators[].generator` | — | FK to the consuming generator |
| `lng_terminal_array[].generators[].heat_rate` | $\eta_g^{hr}$ | Fuel consumption per MWh (m³\_LNG/MWh, default 1.0) |

### Unit Commitment

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `commitment_array[].generator` | — | FK to the committed generator |
| `commitment_array[].startup_cost` | $\text{SU}_g$ | Startup cost (\$/start) |
| `commitment_array[].shutdown_cost` | $\text{SD}_g$ | Shutdown cost (\$/stop) |
| `commitment_array[].noload_cost` | $\text{NL}_g$ | No-load cost while committed (\$/hr) |
| `commitment_array[].min_up_time` | $T_g^{\text{up}}$ | Minimum up time (hours) |
| `commitment_array[].min_down_time` | $T_g^{\text{dn}}$ | Minimum down time (hours) |
| `commitment_array[].ramp_up` | — | Ramp-up limit while online (MW/hr) |
| `commitment_array[].ramp_down` | — | Ramp-down limit while online (MW/hr) |
| `commitment_array[].startup_ramp` | — | Max output in startup block (MW) |
| `commitment_array[].shutdown_ramp` | — | Max output in shutdown block (MW) |
| `commitment_array[].initial_status` | $u_g^{\text{init}}$ | Initial on/off state (1 = online) |
| `commitment_array[].initial_hours` | — | Hours in current state at t=0 |
| `commitment_array[].relax` | — | LP relaxation: binary → continuous in [0,1] |
| `commitment_array[].must_run` | — | Force committed: $u = 1$ always |
| `commitment_array[].commitment_period` | — | Binary variable resolution (hours) |
| `commitment_array[].pmax_segments` | $\overline{P}_{g,k}$ | Piecewise heat-rate power breakpoints (MW) |
| `commitment_array[].heat_rate_segments` | $h_{g,k}$ | Heat rate per segment (GJ/MWh) |
| `commitment_array[].fuel_cost` | — | Fuel cost (\$/GJ) |
| `commitment_array[].fuel_emission_factor` | — | Emission factor for piecewise fuel (tCO₂/GJ) |
| `commitment_array[].hot_start_cost` | — | Startup cost when recently offline (\$/start) |
| `commitment_array[].warm_start_cost` | — | Startup cost at medium offline (\$/start) |
| `commitment_array[].cold_start_cost` | — | Startup cost when long offline (\$/start) |
| `commitment_array[].hot_start_time` | — | Max offline hours for hot start (h) |
| `commitment_array[].cold_start_time` | — | Min offline hours for cold start (h) |
| `simple_commitment_array[].generator` | — | FK to the committed generator |
| `simple_commitment_array[].pmin_dispatch` | $\underline{P}_g^d$ | Dispatch minimum when committed (MW) |
| `simple_commitment_array[].relax` | — | LP relaxation: binary → continuous |
| `simple_commitment_array[].must_run` | — | Force committed: $u = 1$ always |
| `stage_array[].chronological` | — | `true` when blocks are hourly-consecutive (enables UC) |

### Inertia Zone and Provision

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `inertia_zone_array[].requirement` | $\overline{R}_{z,t,b}^{H}$ | Min inertia requirement (MWs) |
| `inertia_zone_array[].cost` | — | Shortage penalty (\$/MWs) |
| `inertia_provision_array[].generator` | — | FK to the providing generator |
| `inertia_provision_array[].inertia_zones` | — | Colon-separated list of InertiaZone IDs/names |
| `inertia_provision_array[].inertia_constant` | $H_p$ | Machine inertia constant (seconds) |
| `inertia_provision_array[].rated_power` | $S_p$ | Rated apparent power (MVA) |
| `inertia_provision_array[].provision_max` | $\overline{R}_{p,t,b}^{H}$ | Max inertia provision (MW) |
| `inertia_provision_array[].provision_factor` | $\Phi_{p,t}$ | Effectiveness factor (MWs/MW) |
| `inertia_provision_array[].cost` | — | Provision cost (\$/MW) |

---

## 8. Cross-References

- **[Planning Guide](../planning-guide.md)** — How to set up planning
  problems with stages, scenarios, and expansion options.
- **[Input Data Reference](../input-data.md)** — Complete specification of
  JSON and Parquet input file formats with all supported fields.
- **[Usage Guide](../usage.md)** — How to run the `gtopt` solver, CLI
  flags, and output interpretation.
- **[Contributing Guide](../../CONTRIBUTING.md)** — Code style, testing, and
  contribution guidelines.
- **[Building Guide](../../BUILDING.md)** — Detailed build instructions,
  dependencies, and troubleshooting.
- **[Scripts Guide](../scripts-guide.md)** — Python conversion utilities
  (plp2gtopt, igtopt, pp2gtopt, ts2gtopt, cvs2parquet).
- **[SDDP Solver](../methods/sddp.md)** — Complete SDDP algorithm
  description, convergence criteria, cut sharing, and configuration.
- **[Monolithic Solver](../methods/monolithic.md)** — Default solver
  description, boundary cuts, and equivalence with SDDP.
- **[Cascade Solver](../methods/cascade.md)** — Multi-level hybrid SDDP
  solver with cut inheritance, target inheritance, and progressive
  LP refinement.

---

## 9. References

### FESOP and gtopt Publications

<a id="ref4"></a>
**[4]** Buitrago Villada, M.P., García Bujanda, C.E., Baeza, E., and
Matus, A.M. (2022). "Optimal Expansion and Reliable Renewable Energy
Integration in Long-Term Planning Using FESOP." *2022 IEEE Kansas Power
and Energy Conference (KPEC)*, pp. 1–6.
DOI: [10.1109/KPEC54747.2022.9814781](https://doi.org/10.1109/KPEC54747.2022.9814781).

> The foundational paper for the FESOP framework on which gtopt is based.
> Presents results of applying FESOP to analyze optimal expansion of the
> Aysén electric system over a 30-year horizon with four demand growth
> scenarios, demonstrating renewable energy integration with spinning
> reserve requirements and battery storage systems.

<a id="ref5"></a>
**[5]** Pereira-Bonvallet, E., Puschel-Lovengreen, S., Matus, M., and
Moreno, R. (2016). "Optimizing Hydrothermal Scheduling with Non-Convex
Irrigation Constraints: Case on the Chilean Electricity System." *Energy
Procedia*, 87, pp. 132–138.
DOI: [10.1016/j.egypro.2015.12.342](https://doi.org/10.1016/j.egypro.2015.12.342).

> Presents the hydrothermal coordination approach that forms the
> operational planning core of the PLP/FESOP lineage. Demonstrates SDDP-
> based scheduling with non-convex irrigation constraints in the Chilean
> Central Interconnected System (SIC).

<a id="ref6"></a>
**[6]** Benavides, C., Matus, M., Sierra, E., Sepúlveda, R., Ruz, A.M.,
and Gallardo, F. (2019). "Value contribution of solar plants to the
Chilean electric system." *AIP Conference Proceedings* 2126, 120008
(SolarPACES 2018).
DOI: [10.1063/1.5117671](https://doi.org/10.1063/1.5117671).

> Quantifies the value of solar energy in the Chilean National Electric
> System using long-term planning, hydrothermal coordination, and short-
> term operation models — demonstrating the multi-model planning workflow
> that motivated gtopt's unified approach.

<a id="ref7"></a>
**[7]** Benavides, C., Alvarez, R., Torres, R., Moreno, R., Matus, M.,
Muñoz, D., Gonzalez, J.M., Jiménez-Estévez, G., and Palma-Behnke, R.
(2019). "Capacity payment allocation in hydrothermal power systems with
high shares of renewable energies." *E3S Web of Conferences*, 140, 11008.
DOI: [10.1051/e3sconf/201914011008](https://doi.org/10.1051/e3sconf/201914011008).

> Proposes a capacity valuation framework for variable renewable generation
> in hydrothermal systems, relevant to gtopt's reserve and capacity
> expansion modeling.

<a id="ref8"></a>
**[8]** Matus, M., Cáceres, N., Puschel-Lovengreen, S., and Moreno, R.
(2015). "Chebyshev based continuous time power system operation approach."
*2015 IEEE Power & Energy Society General Meeting*, pp. 1–5.
DOI: [10.1109/PESGM.2015.7286570](https://doi.org/10.1109/PESGM.2015.7286570).

> Introduces a continuous-time representation for power system operations
> using Chebyshev polynomials, addressing the time-discretization
> challenges that gtopt handles via its block/stage/scenario hierarchy.

<a id="ref9"></a>
**[9]** Matus, M., Sáez, D., Favley, M., Suazo-Martinez, C., Moya, J.,
Jiménez-Estévez, G., Palma-Behnke, R., Olguín, G., and Jorquera, P.
(2012). "Identification of Critical Spans for Monitoring Systems in
Dynamic Thermal Rating." *IEEE Transactions on Power Delivery*, 27(2),
pp. 1002–1009.
DOI: [10.1109/TPWRD.2012.2185254](https://doi.org/10.1109/TPWRD.2012.2185254).

> Addresses dynamic thermal rating of transmission lines — the real-time
> capacity assessment problem complementary to the long-term transmission
> expansion planning solved by gtopt.

### Transmission Expansion Planning

<a id="ref1"></a>
**[1]** Romero, R., Monticelli, A., Garcia, A.V., and Haffner, S. (2002).
"Test systems and mathematical models for transmission network expansion
planning." *IEE Proceedings – Generation, Transmission and Distribution*,
149(1), pp. 27–36.
DOI: [10.1049/ip-gtd:20020026](https://doi.org/10.1049/ip-gtd:20020026).

> Defines the standard test systems and LP/MIP mathematical models for
> transmission expansion planning. The DC power flow and transport models
> in gtopt follow the formulations described in this seminal reference.

<a id="ref2"></a>
**[2]** Romero, R. and Monticelli, A. (1994). "A hierarchical
decomposition approach for transmission network expansion planning."
*IEEE Transactions on Power Systems*, 9(1), pp. 373–380.
DOI: [10.1109/59.317588](https://doi.org/10.1109/59.317588).

> Introduces hierarchical decomposition for large-scale transmission
> expansion, which influenced the multi-stage structure used in gtopt.

<a id="ref3"></a>
**[3]** Lumbreras, S. and Ramos, A. (2016). "The new challenges to
transmission expansion planning. Survey of recent practice and literature
review." *Electric Power Systems Research*, 134, pp. 19–29.
DOI: [10.1016/j.epsr.2015.10.013](https://doi.org/10.1016/j.epsr.2015.10.013).

> Comprehensive survey of TEP methods covering DC models, transport models,
> multi-stage formulations, and uncertainty representation — all approaches
> implemented in gtopt.

<a id="ref10"></a>
**[10]** Gonzalez-Romero, I.C., Wogrin, S., and Román, T. (2020).
"Review on generation and transmission expansion co-planning models under
a market environment." *IET Generation, Transmission & Distribution*,
14(6), pp. 931–944.
DOI: [10.1049/iet-gtd.2019.0123](https://doi.org/10.1049/iet-gtd.2019.0123).

> Reviews co-optimization of generation and transmission expansion (the
> core GTEP problem solved by gtopt), covering LP/MIP formulations,
> decomposition methods, and renewable integration.

### DC Optimal Power Flow

<a id="ref11"></a>
**[11]** Stott, B., Jardim, J., and Alsaç, O. (2009). "DC Power Flow
Revisited." *IEEE Transactions on Power Systems*, 24(3), pp. 1290–1300.
DOI: [10.1109/TPWRS.2009.2021235](https://doi.org/10.1109/TPWRS.2009.2021235).

> The definitive reference on the DC power flow approximation. The
> linearized power flow equations in gtopt's Kirchhoff constraints
> (§5.6) follow the standard DC model: $f_l = B_l (\theta_a - \theta_b)$
> where $B_l = V^2 / X_l$ is the line susceptance ($V$ and $X_l$ must
> share a consistent unit system — see §5.6).

<a id="ref12"></a>
**[12]** Anderson, P.M. and Fouad, A.A. (2002). *Power Systems Control
and Stability*. 2nd ed. Wiley-IEEE Press. ISBN: 978-0471238621.

> Standard textbook for power system dynamics and stability. The IEEE
> 9-bus and 14-bus test systems used in gtopt's benchmark cases originate
> from this reference.

### Similar Tools and Comparable Formulations

<a id="ref13"></a>
**[13]** Brown, T., Hörsch, J., and Schlachtberger, D. (2018). "PyPSA:
Python for Power System Analysis." *Journal of Open Research Software*,
6(1), p. 4.
DOI: [10.5334/jors.188](https://doi.org/10.5334/jors.188).

> PyPSA implements a similar LOPF (linear optimal power flow) formulation
> with multi-period investment planning. gtopt's bus balance, Kirchhoff,
> and storage constraints follow the same mathematical structure, with
> gtopt additionally supporting hydro cascades and multi-scenario
> stochastic optimization.

<a id="ref14"></a>
**[14]** Jenkins, J.D. and Sepulveda, N.A. (2017). "Enhanced Decision
Support for a Changing Electricity Landscape: The GenX Configurable
Electricity Resource Capacity Expansion Model." MIT Energy Initiative
Working Paper.

> GenX implements a similar capacity expansion formulation in Julia/JuMP.
> Both GenX and gtopt use LP/MIP with modular capacity additions,
> storage SoC tracking, and representative time periods, though gtopt
> uses a C++ sparse-matrix assembly for performance.

<a id="ref17"></a>
**[17]** Thurner, L., Scheidler, A., Schäfer, F., Menke, J-H., Dollichon,
J., Meier, F., Meinecke, S., and Braun, M. (2018). "pandapower — An
Open-Source Python Tool for Convenient Modeling, Analysis, and
Optimization of Electric Power Systems." *IEEE Transactions on Power
Systems*, 33(6), pp. 6510–6521.
DOI: [10.1109/TPWRS.2018.2829021](https://doi.org/10.1109/TPWRS.2018.2829021).

> pandapower is a Python-based power system analysis tool supporting AC and
> DC power flow, optimal power flow (OPF), and short-circuit analysis. It
> is used by gtopt's `pp2gtopt` converter to import pandapower network
> models into gtopt format, and by the `gtopt_compare` validation script to
> provide a reference DC OPF solution for benchmarking gtopt results on
> standard IEEE test cases (4-bus, 9-bus, 14-bus, 30-bus, 57-bus).

### Solvers

<a id="ref15"></a>
**[15]** Forrest, J.J. and Lougee-Heimer, R. (2005). "CBC User Guide."
In *Emerging Theory, Methods, and Applications*, INFORMS, pp. 257–277.
DOI: [10.1287/educ.1053.0020](https://doi.org/10.1287/educ.1053.0020).

> Documents the COIN-OR CBC (Coin-or Branch and Cut) solver used as
> gtopt's default MIP solver. CLP (COIN-OR Linear Programming) is used
> for pure LP problems.

### Classification and Surveys

<a id="ref16"></a>
**[16]** Mahdavi, M., Antunez, C.S., Ajalli, M., and Romero, R. (2019).
"Transmission Expansion Planning: Literature Review and Classification."
*IEEE Systems Journal*, 13(3), pp. 3129–3140.
DOI: [10.1109/JSYST.2018.2871793](https://doi.org/10.1109/JSYST.2018.2871793).

> Systematic classification of TEP literature covering mathematical
> models, solution methods, and uncertainty handling — provides context
> for gtopt's position in the GTEP landscape.

---

## Appendix A: LP Problem Size Estimates

For a system with $N$ buses, $G$ generators, $D$ demands, $L$ lines,
$E$ batteries, $P$ pumps, $\tau$ LNG terminals, $Z^R$ reserve zones,
$Z^H$ inertia zones, and $B$ blocks per stage over $T$ stages and $S$ scenarios:

| Quantity | Approximate count |
|----------|------------------|
| **Operational variables** | $(G + 2D + 2L + 3E + N) \times S \times T \times B$ |
| **Pump variables** | $P \times S \times T \times B$ (pump flow) |
| **LNG terminal variables** | $3\tau \times S \times T \times B$ (volume, delivery, vent) |
| **Reserve variables** | $Z^R \times S \times T \times B$ (shortage slack) |
| **Inertia variables** | $Z^H \times S \times T \times B$ (shortage slack) |
| **UC variables** | $3 \times \lvert\mathcal{UC}\rvert \times S \times T \times B$ (u/v/w) |
| **Investment variables** | $(G + D + L + E) \times T$ |
| **Bus balance constraints** | $N \times S \times T \times B$ |
| **Kirchhoff constraints** | $L \times S \times T \times B$ (if DC OPF) |
| **SoC balance constraints** | $E \times S \times T \times B$ |
| **LNG tank balance constraints** | $\tau \times S \times T \times B$ |
| **Pump coupling constraints** | $P \times S \times T \times B$ |
| **Reserve constraints** | $Z^R \times S \times T \times B$ |
| **Inertia constraints** | $Z^H \times S \times T \times B$ |
| **Capacity constraints** | $(G + D + L + E) \times T$ |

The LP is assembled in compressed sparse column (CSC) format via the
`FlatLinearProblem` class and passed to the configured solver backend
(CBC/CLP/HiGHS/CPLEX) [[15]](#ref15).

---

*Document generated from the gtopt source code. See the C++ implementation
in `source/*_lp.cpp` and `include/gtopt/*_lp.hpp` for the canonical
formulation. For the theoretical background, see [Section 9: References](#9-references).*
