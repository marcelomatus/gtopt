# Mathematical Formulation of the GTEP Optimization Problem

> **gtopt** solves a multi-stage, multi-scenario **Generation and Transmission
> Expansion Planning (GTEP)** problem formulated as a sparse linear program
> (LP/MIP). This document describes the complete mathematical formulation in
> detail.

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
   - [5.5 Transmission Line Constraints](#55-transmission-line-constraints)
   - [5.6 Kirchhoff Voltage Law (DC OPF)](#56-kirchhoff-voltage-law-dc-opf)
   - [5.7 Battery / Energy Storage Constraints](#57-battery--energy-storage-constraints)
   - [5.8 Converter Constraints](#58-converter-constraints)
   - [5.9 Reserve Constraints](#59-reserve-constraints)
   - [5.10 Hydro Cascade Constraints](#510-hydro-cascade-constraints)
   - [5.11 Capacity Expansion Constraints](#511-capacity-expansion-constraints)
6. [Scaling and Solver Options](#6-scaling-and-solver-options)
7. [Mapping: JSON Fields → Mathematical Symbols](#7-mapping-json-fields--mathematical-symbols)
8. [Cross-References](#8-cross-references)

---

## 1. Overview

The GTEP problem finds the minimum-cost combination of:

- **Operational decisions (OPEX)** — how much each generator produces in every
  time block, which loads to curtail if capacity is insufficient, and how much
  power flows on each transmission line.
- **Investment decisions (CAPEX)** — how many capacity expansion modules to
  build for generators, demands, lines, and batteries across multi-year
  planning stages.

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
| $X_l$ | `reactance` | Line reactance | p.u. |
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
| $\overline{V}_r$ | `vmax` | Reservoir max volume | hm³ |
| $\underline{V}_r$ | `vmin` | Reservoir min volume | hm³ |
| $\kappa_u$ | `conversion_rate` (turbine) | Turbine water-to-power factor | MW/(m³/s) |
| $\overline{R}_z^{\text{up}}$ | `urreq` | Up-reserve requirement | MW |
| $\overline{R}_z^{\text{dn}}$ | `drreq` | Down-reserve requirement | MW |
| $K_g^{\text{cap}}$ | `annual_capcost` | Annual expansion cost per module | \$/year |
| $M_g$ | `expcap` | Capacity per expansion module | MW |
| $\overline{m}_g$ | `expmod` | Maximum expansion modules | — |
| $\bar{C}_g^0$ | `capacity` | Initial installed capacity | MW |

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
| $r_{p,s,t,b}^{\text{up}}$ | Up-reserve provision from gen $g$ | $\geq 0$ |
| $r_{p,s,t,b}^{\text{dn}}$ | Down-reserve provision from gen $g$ | $\geq 0$ |
| $q_z^{\text{up}}$ | Unserved up-reserve at zone $z$ | $\geq 0$ |
| $q_z^{\text{dn}}$ | Unserved down-reserve at zone $z$ | $\geq 0$ |
| $\varphi_{w,s,t,b}$ | Waterway $w$ water flow | $[\underline{Q}_w,\; \overline{Q}_w]$ |
| $v_{r,s,t,b}$ | Reservoir $r$ volume | $[\underline{V}_r,\; \overline{V}_r]$ |
| $\text{spill}_{r,s,t,b}$ | Reservoir $r$ spillway discharge | $\geq 0$ |

> **Note on line flows**: When `use_line_losses` is enabled and
> $\lambda_l > 0$, the solver creates separate forward ($f^+$) and reverse
> ($f^-$) flow variables. When $\lambda_l = 0$, a single bidirectional
> variable $f_l \in [-\overline{F}_l^{ba},\; \overline{F}_l^{ab}]$ is used.

---

## 3. Compact Formulation

$$
\boxed{
\min_{p, \ell, q, f, \theta, e, \bar{C}, m, \ldots}
\;\;
\underbrace{
\sum_{s \in \mathcal{S}} \sum_{t \in \mathcal{T}} \sum_{b \in \mathcal{B}_t}
\omega_{s,t,b}
\left[
  \sum_{g} c_g \, p_{g,s,t,b}
  + \sum_{d} c_d^{\text{fail}} q_{d,s,t,b}
  + \sum_{l} c_l \left( f_{l,s,t,b}^+ + f_{l,s,t,b}^- \right)
\right]
}_{\text{OPEX}}
\;+\;
\underbrace{
\sum_{g} \sum_{t} \omega_t^{\text{cap}} \, K_g^{\text{cap}} \, m_{g,t}
}_{\text{CAPEX}}
}
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
  \underbrace{\sum_{g \in \mathcal{G}} c_{g,t} \; p_{g,s,t,b}}_{\text{Generation cost}}
  + \underbrace{\sum_{d \in \mathcal{D}} c_{d,t}^{\text{fail}} \; q_{d,s,t,b}}_{\text{Curtailment cost}}
  + \underbrace{\sum_{l \in \mathcal{L}} c_{l,t} \left( f_{l,s,t,b}^{+} + f_{l,s,t,b}^{-} \right)}_{\text{Transfer cost}}
\Bigg]
$$

Additional OPEX terms may include:

- **Reserve failure cost**: $\sum_{z} c_z^{\text{rfail}} (q_{z,s,t,b}^{\text{up}} + q_{z,s,t,b}^{\text{dn}})$
- **Reserve provision cost**: $\sum_{p} c_{p,t}^{\text{ur}} \; r_{p,s,t,b}^{\text{up}} + c_{p,t}^{\text{dr}} \; r_{p,s,t,b}^{\text{dn}}$
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
withdrawal (Kirchhoff's Current Law):

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
\overline{P}_{g,t,b} = \min\!\big(\overline{P}_{g,t}(b),\; \bar{C}_{g,t}\big)
\qquad
\underline{P}_{g,t,b} = \max\!\big(\underline{P}_{g,t}(b),\; 0\big)
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

### 5.5 Transmission Line Constraints

#### Without Losses ($\lambda_l = 0$)

A single bidirectional flow variable $f_l$ is created:

$$
-\overline{F}_l^{ba} \;\leq\; f_{l,s,t,b} \;\leq\; \overline{F}_l^{ab}
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
\text{At bus } a: \quad -(1 + \lambda_l) \, f_{l,s,t,b}^{+}
\qquad
\text{At bus } b: \quad +f_{l,s,t,b}^{+}
$$

Bus balance contributions for reverse flow ($b \to a$):

$$
\text{At bus } b: \quad -(1 + \lambda_l) \, f_{l,s,t,b}^{-}
\qquad
\text{At bus } a: \quad +f_{l,s,t,b}^{-}
$$

The loss is proportional to the flow: loss $= \lambda_l \cdot |f|$.

#### Transfer Cost

$$
\text{cost}(f_{l,s,t,b}^{+}) = \text{cost}(f_{l,s,t,b}^{-}) = c_{l,t} \cdot \omega_{s,t,b}
$$

### 5.6 Kirchhoff Voltage Law (DC OPF)

When `use_kirchhoff = true` and `use_single_bus = false`, voltage angle
variables $\theta_n$ are introduced and DC power flow constraints enforce
Kirchhoff's Voltage Law on each line $l$ connecting bus $a$ to bus $b$:

$$
\frac{\theta_{a,s,t,b} - \theta_{b,s,t,b}}{X_l / V^2} = f_{l,s,t,b}^{+} - f_{l,s,t,b}^{-}
$$

In the implementation, this is scaled by $\sigma_\theta$ for numerical
stability. Defining the scaled susceptance:

$$
\chi_l = \sigma_\theta \cdot \frac{X_l}{V^2}
$$

the constraint becomes:

$$
-\theta_{a,s,t,b} + \theta_{b,s,t,b} + \chi_l \, f_{l,s,t,b}^{+} - \chi_l \, f_{l,s,t,b}^{-} = 0
\qquad \forall \; l, s, t, b
$$

#### Reference Bus

One bus is designated as the **reference bus** with a fixed voltage angle:

$$
\theta_{n^{\text{ref}},s,t,b} = \theta^{\text{ref}} \cdot \sigma_\theta
\qquad \forall \; s, t, b
$$

typically $\theta^{\text{ref}} = 0$.

#### Voltage Angle Bounds

For non-reference buses:

$$
-2\pi \, \sigma_\theta \;\leq\; \theta_{n,s,t,b} \;\leq\; 2\pi \, \sigma_\theta
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
self-discharge losses.

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
\sum_{p \in \mathcal{P}_z} \alpha_{p,t} \; r_{p,s,t,b}^{\text{up}} + q_{z,s,t,b}^{\text{up}} \;\geq\; \overline{R}_{z,t,b}^{\text{up}}
\qquad \forall \; z, s, t, b
$$

$$
\sum_{p \in \mathcal{P}_z} \alpha_{p,t} \; r_{p,s,t,b}^{\text{dn}} + q_{z,s,t,b}^{\text{dn}} \;\geq\; \overline{R}_{z,t,b}^{\text{dn}}
\qquad \forall \; z, s, t, b
$$

where:
- $\alpha_{p,t}$ is the provision factor at stage $t$ (typically 1.0)
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

- **Reserve provision cost**: $c_{p,t}^{\text{ur}} \cdot \omega_{s,t,b}$
  per MW of up-reserve provided
- **Reserve failure cost**: $c_z^{\text{rfail}} \cdot \omega_{s,t,b}$
  per MW of unserved reserve

### 5.10 Hydro Cascade Constraints

The hydro cascade models a network of junctions (hydraulic nodes) connected
by waterways, with reservoirs for water storage.

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

#### Filtration (Water Seepage)

Filtration models seepage from a waterway $w$ into a reservoir $r$ as a
function of the reservoir's average volume:

$$
\varphi_{i,s,t,b} = a_i + b_i \cdot \frac{v_{r,s,t,b-1} + v_{r,s,t,b}}{2}
\qquad \forall \; i, s, t, b
$$

where $a_i$ is the constant term and $b_i$ is the slope (seepage rate per
unit volume).

### 5.11 Capacity Expansion Constraints

Capacity expansion applies uniformly to generators, demands, lines,
batteries, and converters. For each expandable component $g$ at stage $t$:

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

### 6.3 Key Options Affecting the Formulation

| Option | JSON field | Default | Effect on formulation |
|--------|-----------|---------|----------------------|
| Kirchhoff mode | `use_kirchhoff` | `true` | Enables DC OPF constraints (§5.6) |
| Single bus | `use_single_bus` | `false` | Disables all network constraints (copper plate) |
| Line losses | `use_line_losses` | `true` | Enables loss modeling (§5.5) |
| Kirchhoff threshold | `kirchhoff_threshold` | `0` | Minimum bus count for Kirchhoff activation |
| Objective scale | `scale_objective` | `1000` | Divides all cost coefficients |
| Theta scale | `scale_theta` | `1000` | Scales voltage angle variables |
| Annual discount rate | `annual_discount_rate` | `0.0` | Multi-year cost discounting |
| Demand fail cost | `demand_fail_cost` | *(none)* | Enables load curtailment variables |
| Reserve fail cost | `reserve_fail_cost` | *(none)* | Enables reserve shortage variables |
| Input format | `input_format` | `"parquet"` | Time-series input format |
| Output format | `output_format` | `"parquet"` | Solution output format |
| Output compression | `output_compression` | `"gzip"` | Parquet compression codec |

### 6.4 Modeling Modes Summary

| Mode | Conditions | Variables | Constraints |
|------|-----------|-----------|-------------|
| **Copper plate** | `use_single_bus = true` | $p, \ell, q$ | Global balance only |
| **Transport model** | `use_kirchhoff = false`, multi-bus | $p, \ell, q, f$ | Bus balance + line capacity |
| **DC OPF** | `use_kirchhoff = true`, multi-bus | $p, \ell, q, f, \theta$ | Bus balance + Kirchhoff VL + line capacity |

---

## 7. Mapping: JSON Fields → Mathematical Symbols

This table maps the JSON input fields (used in planning files) to the
mathematical symbols used in this formulation.

### System Options

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `options.scale_objective` | $\sigma$ | Objective scaling |
| `options.scale_theta` | $\sigma_\theta$ | Angle scaling |
| `options.annual_discount_rate` | $r$ | Discount rate |
| `options.use_kirchhoff` | — | Enable DC OPF |
| `options.use_single_bus` | — | Copper-plate mode |
| `options.use_line_losses` | — | Enable line losses |
| `options.demand_fail_cost` | $c^{\text{fail}}_d$ | Curtailment penalty |
| `options.reserve_fail_cost` | $c^{\text{rfail}}$ | Reserve penalty |

### Simulation Structure

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `simulation.scenario_array[].probability_factor` | $\pi_s$ | Scenario weight |
| `simulation.stage_array[].discount_factor` | $\delta_t^{\text{user}}$ | Stage discount |
| `simulation.block_array[].duration` | $\Delta_b$ | Block duration (h) |

### Generator

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `generator_array[].gcost` | $c_g$ | Variable cost (\$/MWh) |
| `generator_array[].pmin` | $\underline{P}_g$ | Min output (MW) |
| `generator_array[].pmax` | $\overline{P}_g$ | Max output (MW) |
| `generator_array[].capacity` | $\bar{C}_g^0$ | Installed capacity (MW) |
| `generator_array[].lossfactor` | $\lambda_g$ | Injection loss |
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

### Hydro Components

| JSON Path | Symbol | Description |
|-----------|--------|-------------|
| `waterway_array[].fmin` | $\underline{Q}_w$ | Min water flow (m³/s) |
| `waterway_array[].fmax` | $\overline{Q}_w$ | Max water flow (m³/s) |
| `waterway_array[].lossfactor` | $\lambda_w$ | Transport loss |
| `reservoir_array[].vmin` | $\underline{V}_r$ | Min volume (hm³) |
| `reservoir_array[].vmax` | $\overline{V}_r$ | Max volume (hm³) |
| `turbine_array[].conversion_rate` | $\kappa_u$ | Water-to-power factor |
| `filtration_array[].constant` | $a_i$ | Seepage constant |
| `filtration_array[].slope` | $b_i$ | Seepage slope |

---

## 8. Cross-References

- **[Planning Guide](../../PLANNING_GUIDE.md)** — How to set up planning
  problems with stages, scenarios, and expansion options.
- **[Input Data Reference](../../INPUT_DATA.md)** — Complete specification of
  JSON and Parquet input file formats with all supported fields.
- **[Usage Guide](../../USAGE.md)** — How to run the `gtopt` solver, CLI
  flags, and output interpretation.

---

## Appendix A: LP Problem Size Estimates

For a system with $N$ buses, $G$ generators, $D$ demands, $L$ lines,
$E$ batteries, and $B$ blocks per stage over $T$ stages and $S$ scenarios:

| Quantity | Approximate count |
|----------|------------------|
| **Operational variables** | $(G + 2D + 2L + 3E + N) \times S \times T \times B$ |
| **Investment variables** | $(G + D + L + E) \times T$ |
| **Bus balance constraints** | $N \times S \times T \times B$ |
| **Kirchhoff constraints** | $L \times S \times T \times B$ (if DC OPF) |
| **SoC balance constraints** | $E \times S \times T \times B$ |
| **Capacity constraints** | $(G + D + L + E) \times T$ |

The LP is assembled in compressed sparse column (CSC) format via the
`FlatLinearProblem` class and passed to the COIN-OR solver (CBC/CLP).

---

*Document generated from the gtopt source code. See the C++ implementation
in `source/*_lp.cpp` and `include/gtopt/*_lp.hpp` for the canonical
formulation.*
