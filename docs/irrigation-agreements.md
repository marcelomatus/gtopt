# Irrigation Agreements — Modeling Guide

gtopt models Chilean irrigation agreements (Convenios de Riego) through
three generic, data-driven entity types — **FlowRight**, **VolumeRight**,
and **UserConstraint** — that form a **rights-domain layer** parallel to
the physical hydro topology.  This document describes the legal context,
the LP formulation, the entity mapping, and the PLP-to-gtopt comparison.

---

## Table of Contents

1. [Legal and Historical Context](#1-legal-and-historical-context)
2. [Architecture Overview](#2-architecture-overview)
3. [FlowRight — Flow-Based Water Rights](#3-flowright--flow-based-water-rights)
4. [VolumeRight — Volume-Based Water Rights](#4-volumeright--volume-based-water-rights)
5. [UserConstraint and PAMPL Files](#5-userconstraint-and-pampl-files)
6. [Laja Agreement (Convenio del Laja)](#6-laja-agreement-convenio-del-laja)
7. [Maule Agreement (Convenio del Maule)](#7-maule-agreement-convenio-del-maule)
8. [PLP-to-gtopt Name Mapping](#8-plp-to-gtopt-name-mapping)
9. [PLP vs gtopt LP Comparison](#9-plp-vs-gtopt-lp-comparison)
   - [LP Variable Naming Conventions](#90-lp-variable-naming-conventions)
   - [Coefficient Scaling](#90b-coefficient-scaling)
   - [Reservoir Balance Comparison](#97-reservoir-balance-comparison-stage-1-block-1-dur7h)
   - [Objective Coefficient Comparison](#98-objective-coefficient-comparison-laja-march)
   - [Volume Right Bounds Comparison](#99-volume-right-bounds-comparison-march)
10. [Known Simplifications and Gaps](#10-known-simplifications-and-gaps)
11. [Configuration and Usage](#11-configuration-and-usage)
12. [See Also](#12-see-also)

---

## 1. Legal and Historical Context

### 1.1 Overview

Chile's major hydroelectric systems share water with agricultural
irrigation under legally binding agreements signed decades before the
modern electricity market.  These agreements constrain how much water a
hydro operator can turbine, divert, or store, depending on reservoir
volume, season, and accumulated rights usage.

gtopt currently models two agreements:

| Feature | Laja Agreement | Maule Agreement |
|---------|----------------|-----------------|
| Year signed | 1958 (updated 2017) | 1947 (updated 1983) |
| Parties | DOH + ENDESA (now Enel) | DOH + ENDESA/Colbun |
| Primary reservoir | Laguna del Laja (~6,000 hm³) | Lago Maule + Colbun + Invernada |
| Regulation horizon | Multi-annual (unique in Chile) | Annual / seasonal |
| Irrigation area | ~117,000 ha (Biobio region) | ~90,000+ ha (Maule region) |
| Hydroelectric capacity | ~1,150 MW (El Toro + 4 cascade) | ~800 MW (Colbun + Machicura) |
| Key mechanism | 4-zone piecewise-linear rights | 3-zone proportional allocation |

### 1.2 Laja Agreement — Legal Background

The Convenio del Laja governs the allocation of water from Laguna del
Laja between hydroelectric generation (El Toro, 450 MW) and irrigation
of approximately 117,000 hectares in the Biobio region.

The reservoir volume is divided into **4 zones**, each with different
allocation factors for three rights categories:

- **Derechos de Riego** (irrigation rights) — priority water for
  agriculture, active primarily Sep–Mar
- **Derechos Eléctricos** (electrical rights) — water available for
  hydroelectric generation year-round
- **Derechos Mixtos** (mixed rights) — shared allocation with
  season-dependent usage

The total rights for each category are computed as a piecewise-linear
function of reservoir volume:

```
Rights(V) = Base + Σᵢ [ Factorᵢ × min(Vᵢ, Widthᵢ) ]
```

At the start of each hydrological year (April), volume rights are
**re-provisioned** based on current reservoir volume.  A fourth category,
**Gasto Anticipado** (anticipated discharge), allows early extraction of
future irrigation rights under certain conditions.

Three **economy accumulators** (ENDESA, Reserve, Alto Polcura) track
unused rights that carry forward across years.

### 1.3 Maule Agreement — Legal Background

The Convenio del Maule governs water allocation from the Colbun reservoir
system between ENDESA/Colbun (electric generation) and irrigators
(~90,000+ ha in the Maule region).

The Colbun reservoir volume is divided into **3 operational zones**:

1. **Reserva Extraordinaria** (bottom, 0–129 hm³): No extraction rights
   for either party
2. **Reserva Ordinaria** (middle, 129–581 hm³): Proportional allocation
   — typically 20% electric / 80% irrigation
3. **Zona Normal** (top, above 581 hm³): Full extraction rights for both
   parties

Additional elements include:

- **Resolución 105** (DGA, 1983): Mandatory minimum ecological flow at
  the Melado river junction point, year-round
- **La Invernada**: Auxiliary reservoir with winter storage/bypass
  decisions and an economy accumulator
- **Bocatoma Cañelón**: Physical intake point with associated costs
- **Districts**: Proportional allocation of irrigation flow among
  withdrawal districts (7+ districts with fixed percentage shares)

---

## 2. Architecture Overview

### 2.1 Design Principle

Rights entities are **not part of the physical hydro topology**.  They
create their own LP variables and constraints, and couple to physical
elements (reservoirs, junctions) through explicit references:

```
  ┌─────────────────────────────────────────────────────────┐
  │              PHYSICAL DOMAIN (hydro topology)            │
  │   Reservoir ──► Waterway ──► Turbine ──► Generator      │
  │      │ volume                                            │
  └──────┼──────────────────────────────────────────────────┘
         │ volume drives bound_rule
         ▼
  ┌─────────────────────────────────────────────────────────┐
  │              RIGHTS DOMAIN (water rights accounting)     │
  │                                                          │
  │   FlowRight ────► UserConstraint (flow partition)        │
  │       │                                                  │
  │       ▼                                                  │
  │   VolumeRight (accumulated extraction, state variable)   │
  │       │                                                  │
  │       ▼                                                  │
  │   bound_rule (volume-dependent dynamic bounds)           │
  └─────────────────────────────────────────────────────────┘
```

### 2.2 Entity Summary

| gtopt Entity | Purpose | PLP Equivalent |
|---|---|---|
| **FlowRight** | Flow-based right (m³/s) per block | IQ* variables (IQDR, IQDE, IQMNE, etc.) |
| **VolumeRight** | Volume-based right (hm³) across stages | IV* variables (IVDRF, IVDEF, VMGEMF, etc.) |
| **UserConstraint** | Proportional allocation, identities | Percentage splits, flow partitions |
| **RightBoundRule** | Volume-dependent dynamic bounds | PLP `FijaLaja`/`FijaMaule` bound modification |

---

## 3. FlowRight — Flow-Based Water Rights

### 3.1 Concept

A FlowRight represents a consumptive flow entitlement at a point in the
hydro network.  It creates LP flow variables per block with two operating
modes:

- **Fixed mode** (default): `lowb = uppb = discharge` — the flow is
  fixed to the required extraction rate
- **Variable mode**: When `fmax > 0` and `discharge = 0`, bounds are
  `[0, fmax]` — the optimizer decides the flow

### 3.2 Key Fields

| Field | Type | Description |
|-------|------|-------------|
| `discharge` | STBRealFieldSched | Required flow [m³/s] per (scenario, stage, block) |
| `fmax` | OptTBRealFieldSched | Maximum flow for variable mode [m³/s] |
| `junction` | OptSingleId | Physical junction coupling (consumptive withdrawal) |
| `direction` | OptInt (+1/-1) | Sign in UserConstraint flow balance |
| `use_average` | OptBool | Creates stage-average hourly flow variable (qeh) |
| `fail_cost` | OptTBRealFieldSched | Penalty for unmet demand [$/m³/s·h] |
| `use_value` | OptTBRealFieldSched | Benefit value for flow usage [$/m³/s·h] |
| `bound_rule` | Optional | Volume-dependent dynamic bound on flow |

### 3.3 LP Variables and Constraints

**Per block `b`:**

| Variable | Bounds | Objective | Condition |
|----------|--------|-----------|-----------|
| `frt_flow` | [lowb, uppb] | `-use_value / scale_obj` | Always |
| `frt_fail` | [0, ∞) | `+fail_cost / scale_obj` | If `fail_cost > 0` |

**Deficit coupling** (when `fail_cost > 0` and `discharge > 0`):

```
flow + fail >= discharge
```

This ensures the fail variable absorbs under-delivery when the flow is
constrained by junction balance or bound rules.  Without this constraint,
the fail variable would remain zero (dead variable).

**Stage-average variable** (when `use_average = true`):

```
qeh = Σ_b [ flow(b) × dur(b) / dur_stage ]
```

This mirrors PLP's "H"-suffix variables (IQDRH, IQDEH, etc.) that
compute duration-weighted stage averages.

### 3.4 Junction Coupling

When `junction` is set, the flow is subtracted from the junction
balance row (consumptive withdrawal):

```
junction_balance_row[flow_col] = -1.0
```

### 3.5 Bound Rule (Volume-Dependent Bounds)

The optional `bound_rule` defines a piecewise-linear function mapping
reservoir volume to maximum allowed flow:

```json
{
  "bound_rule": {
    "reservoir": "ELTORO",
    "segments": [
      {"volume": 0,    "slope": 0.00, "constant": 570},
      {"volume": 1200, "slope": 0.40, "constant": 90},
      {"volume": 1900, "slope": 0.25, "constant": 375}
    ],
    "cap": 5000
  }
}
```

During `update_lp()`, the bound is re-evaluated using the current
reservoir volume and flow column upper bounds are updated accordingly.

---

## 4. VolumeRight — Volume-Based Water Rights

### 4.1 Concept

A VolumeRight models accumulated volume entitlements (hm³) from
reservoirs.  It acts as a "dummy reservoir" (StorageLP pattern) for
rights accounting, tracking cumulative extraction across stages with
SDDP state-variable coupling.

### 4.2 Key Fields

| Field | Type | Description |
|-------|------|-------------|
| `reservoir` | OptSingleId | Physical source reservoir |
| `right_reservoir` | OptSingleId | Parent VolumeRight (hierarchical) |
| `emin` / `emax` | OptTRealFieldSched | Volume bounds [hm³] |
| `eini` | OptReal | Initial accumulated volume [hm³] |
| `demand` | OptTRealFieldSched | Required delivery per stage [hm³] |
| `fmax` | OptTBRealFieldSched | Maximum extraction rate [m³/s] |
| `fail_cost` | OptReal | Penalty for unmet volume demand [$/hm³] |
| `reset_month` | Optional | Month when rights are re-provisioned |
| `bound_rule` | Optional | Volume-dependent dynamic bound |
| `saving_rate` | OptTBRealFieldSched | Economy deposit rate [m³/s] |
| `flow_conversion_rate` | OptReal | m³/s·h → hm³ (default 0.0036) |

### 4.3 LP Formulation

**Storage balance per block:**

```
vol(b) = vol(b-1) + fcr × dur(b) × saving(b) / escale
                   - fcr × dur(b) × extraction(b) / escale
```

Where `fcr = 0.0036 hm³/(m³/s·h)` is the flow conversion rate.

**Demand constraint per stage** (if `demand > 0`):

```
Σ_b [ fcr × dur(b) × extraction(b) ] + fail >= demand / escale
```

**Reset at `reset_month`:**
When the stage month matches `reset_month`, the VolumeRight is
re-provisioned.  With a `bound_rule`, the new initial volume is computed
from the piecewise-linear function evaluated at the physical reservoir's
current volume.  Without a bound rule, the volume resets to `emax`.

### 4.4 Coupling

- **Physical reservoir**: Extraction subtracted from reservoir energy
  balance (consumptive outflow)
- **Parent VolumeRight**: When `right_reservoir` is set, extraction
  couples to the parent's energy balance (hierarchical rights)
- **SDDP state variable**: Volume carried across phases via state
  variable linking (backward cuts capture irrigation opportunity cost)

---

## 5. UserConstraint and PAMPL Files

### 5.1 Role in Irrigation Agreements

UserConstraints express the inter-right relationships that cannot be
captured by individual FlowRight or VolumeRight entities:

- **Flow partitions**: Total turbine flow = sum of extraction rights
- **Percentage allocations**: Ordinary reserve split (20% electric / 80%
  irrigation)
- **District proportional allocation**: Each district receives a fixed
  percentage of total irrigation flow
- **Balance equations**: La Invernada inflow/outflow balance

### 5.2 PAMPL File Format

Constraints are defined in `.pampl` files (pseudo-AMPL) with
parameterized Jinja2 templates:

```pampl
# Flow partition: total generation = sum of extractions
constraint laja_particion_derechos "Flow partition":
  flow_right('laja_q_turbinado').flow
    = flow_right('laja_der_riego').flow
    + flow_right('laja_der_electrico').flow
    + flow_right('laja_der_mixto').flow
    + flow_right('laja_gasto_anticipado').flow;

# Parameters
param vol_muerto = 0;
param irr_base = 570;
param irr_factor_1 = 0.00;
```

### 5.3 Multi-File Support

The `user_constraint_files` (plural) field in `System` supports loading
multiple PAMPL files independently:

```json
{
  "system": {
    "user_constraint_files": [
      "laja.pampl",
      "maule.pampl"
    ]
  }
}
```

Each file is parsed separately with auto-incremented UIDs to avoid
collisions.  This keeps each agreement self-contained.

See [User Constraints — Syntax Reference](user-constraints.md) for the
complete constraint expression syntax.

---

## 6. Laja Agreement (Convenio del Laja)

### 6.1 Entity Diagram

```
                     Laguna del Laja (~6,000 hm³)
                              |
                     [tunnel / waterway]
                              |
                              v
                      El Toro (450 MW)
                              |
                 +────────────v─────────────+
                 |    UserConstraint         |
                 | laja_particion_derechos   |
                 |    qgt = qdr+qde+qdm+qga |
                 +──┬───┬───┬───┬───────────+
                    |   |   |   |
        +-----------+   |   |   +----------+
        |               |   |              |
        v               v   v              v
  laja_der_riego  laja_der_  laja_der_  laja_gasto_
    (irrigation)  electrico    mixto    anticipado
     (qdr)          (qde)     (qdm)      (qga)
        |               |   |              |
        v               v   v              v
  [VolumeRight]   [VolumeRight]      [VolumeRight]
  laja_vol_       laja_vol_          laja_vol_
  der_riego       der_electrico      der_mixto
   (IVDRF)          (IVDEF)           (IVDMF)
```

### 6.2 FlowRight Entities

| Name | Purpose | Direction | Use Average | Costs |
|------|---------|-----------|-------------|-------|
| `laja_q_turbinado` | Total turbine flow | +1 (supply) | Yes | — |
| `laja_der_riego` | Irrigation rights | -1 (demand) | Yes | fail: `cost_irr_ns`, value: `cost_irr_uso` |
| `laja_der_electrico` | Electrical rights | -1 (demand) | Yes | fail: `cost_elec_ns`, value: `cost_elec_uso` |
| `laja_der_mixto` | Mixed rights | -1 (demand) | Yes | value: `cost_mixed` |
| `laja_gasto_anticipado` | Anticipated discharge | -1 (demand) | Yes | fail: from `monthly_cost_anticipated` |
| `{district}_{category}` | District withdrawal | -1 (demand) | No | fail: `cost_irr_ns × cost_factor` |

### 6.3 VolumeRight Entities

| Name | Purpose | Reset | Bound Rule | Economy |
|------|---------|-------|------------|---------|
| `laja_vol_der_riego` | Irrigation volume | April | 4-zone piecewise | No |
| `laja_vol_der_electrico` | Electrical volume | April | 4-zone piecewise | No |
| `laja_vol_der_mixto` | Mixed volume | April | 4-zone piecewise | No |
| `laja_vol_gasto_anticipado` | Anticipated volume | April | Same as irrigation | No |
| `laja_vol_econ_endesa` | ENDESA economy | None | None | Yes (saving) |
| `laja_vol_econ_reserva` | Reserve economy | None | None | Yes (saving) |
| `laja_vol_econ_polcura` | Alto Polcura economy | None | None | Yes (saving) |

### 6.4 Volume Zone Formula

```
  Rights
  (m³/s)
  5000 ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ cap
       │                                        ╱
  1125 │ · · · · · · · · · · · · · · · · · · ·╱ (V=3000)
       │                                    ╱
       │                             slope=0.25
       │                           ╱
   638 │ · · · · · · · · · · · ·╱
       │                slope=0.40
   570 │▓▓▓▓▓▓▓▓▓▓▓▓▓▓╱
       │  slope=0.00 ╱
       └────────────┼───────┼────────┼──────────── V (hm³)
       0          1200    1370     1900          5582
              Zone 1  │Zone 2│ Zone 3 │   Zone 4
```

### 6.5 UserConstraints

| Name | Expression |
|------|-----------|
| `laja_particion_derechos` | `flow_right('laja_q_turbinado').flow = flow_right('laja_der_riego').flow + flow_right('laja_der_electrico').flow + flow_right('laja_der_mixto').flow + flow_right('laja_gasto_anticipado').flow` |

---

## 7. Maule Agreement (Convenio del Maule)

### 7.1 Entity Diagram

```
  Laguna del Maule          Rio Maule tributaries
       |                           |
       v                           v
  +---------+  +----------+  +----------+  +-----------+
  | Cipreses |  |Invernada |  | Melado   |  |  Colbun   |
  +---------+  +----------+  +----------+  +-----------+
       |            |              |              |
       v            v              v              v
  maule_gasto    invernada    maule_gasto    maule_
  _normal_elec   _embalsar    _ordinario_   resolucion
    (IQMNE)      (IQHINV)    elec (IQMOE)  _105 (IQR105)
       |            |              |              |
       v            v              v              v
  [VolumeRight]  [VolumeRight]  [VolumeRight]  [fixed]
  monthly/annual  economy       reserve
```

### 7.2 FlowRight Entities

| Name | Purpose | Direction | Use Average | Costs |
|------|---------|-----------|-------------|-------|
| `maule_gasto_normal_elec` | Normal electric | -1 | Yes | fail: `penalizador_1` |
| `maule_gasto_normal_riego` | Normal irrigation | -1 | Yes | fail: `costo_riego_ns`, value: `valor_riego` |
| `maule_gasto_ordinario_elec` | Ordinary reserve electric | -1 | Yes | — |
| `maule_gasto_ordinario_riego` | Ordinary reserve irrigation | -1 | Yes | value: `valor_riego` |
| `maule_compensacion_elec` | ENDESA compensation | -1 | Yes | — |
| `maule_resolucion_105` | Minimum ecological flow | -1 | No | fail: `costo_riego_ns_res105` |
| `invernada_deficit` | Invernada deficit flow | +1 | No | — |
| `invernada_sin_deficit` | Invernada no-deficit flow | +1 | No | — |
| `invernada_caudal_natural` | Invernada natural inflow | +1 | No | — |
| `invernada_embalsar` | Invernada storage | -1 | No | value: `costo_embalsar` |
| `invernada_no_embalsar` | Invernada bypass | -1 | No | value: `costo_no_embalsar` |
| `maule_bocatoma_canelon` | Cañelón intake | -1 | No | value: `costo_canelon` |
| `retiro_*` (per district) | District withdrawal | -1 | No | — |

### 7.3 VolumeRight Entities

| Name | Purpose | Reset | Reservoir |
|------|---------|-------|-----------|
| `maule_vol_gasto_elec_mensual` | Monthly electric accumulator | January | Colbun |
| `maule_vol_gasto_elec_anual` | Annual electric accumulator | June | Colbun |
| `maule_vol_gasto_riego_temp` | Seasonal irrigation accumulator | June | Colbun |
| `maule_vol_compensacion_elec` | ENDESA compensation volume | None | Colbun |
| `maule_vol_reserva_ord_elec` | Extraordinary reserve electric | None | Colbun |
| `maule_vol_reserva_ord_riego` | Extraordinary reserve irrigation | None | Colbun |
| `invernada_vol_econ` | La Invernada winter economy | None | Invernada |

### 7.4 UserConstraints

| Name | Expression | Purpose |
|------|-----------|---------|
| `invernada_balance` | `deficit + sin_deficit + caudal_natural = embalsar + no_embalsar` | Invernada flow balance |
| `maule_pct_ordinario_elec` | `elec_ord <= pct% × (elec_ord + irr_ord)` | Electric share cap in ordinary zone |
| `maule_pct_ordinario_riego` | `irr_ord <= pct% × (elec_ord + irr_ord)` | Irrigation share cap in ordinary zone |
| `dist_retiro_*` | `district.flow {<= or =} pct% × normal_irrigation` | Per-district proportional allocation |

---

## 8. PLP-to-gtopt Name Mapping

### 8.1 Laja Agreement

| PLP Variable | PLP Name | gtopt FlowRight | gtopt VolumeRight |
|---|---|---|---|
| IQGT | Gasto Turbinado | `laja_q_turbinado` | — |
| IQDR | Derecho de Riego | `laja_der_riego` | `laja_vol_der_riego` |
| IQDE | Derecho Eléctrico | `laja_der_electrico` | `laja_vol_der_electrico` |
| IQDM | Derecho Mixto | `laja_der_mixto` | `laja_vol_der_mixto` |
| IQGA | Gasto Anticipado | `laja_gasto_anticipado` | `laja_vol_gasto_anticipado` |
| IVESF | Economía ENDESA | — | `laja_vol_econ_endesa` |
| IVERF | Economía Reserva | — | `laja_vol_econ_reserva` |
| IVAPF | Economía Polcura | — | `laja_vol_econ_polcura` |
| — | Partición Derechos | — | UserConstraint: `laja_particion_derechos` |

### 8.2 Maule Agreement

| PLP Variable | PLP Name | gtopt FlowRight | gtopt VolumeRight |
|---|---|---|---|
| IQMNE | Gasto Normal Eléctrico | `maule_gasto_normal_elec` | `maule_vol_gasto_elec_mensual` |
| IQMNR | Gasto Normal Riego | `maule_gasto_normal_riego` | `maule_vol_gasto_riego_temp` |
| IQMOE | Gasto Ordinario Eléctrico | `maule_gasto_ordinario_elec` | `maule_vol_reserva_ord_elec` |
| IQMOR | Gasto Ordinario Riego | `maule_gasto_ordinario_riego` | `maule_vol_reserva_ord_riego` |
| IQMCE | Compensación Eléctrica | `maule_compensacion_elec` | `maule_vol_compensacion_elec` |
| IQR105 | Resolución 105 | `maule_resolucion_105` | — |
| — | Gasto Eléctrico Anual | — | `maule_vol_gasto_elec_anual` |
| IQHINV | Embalsar (Invernada) | `invernada_embalsar` | — |
| IQHNEIN | No Embalsar (Invernada) | `invernada_no_embalsar` | — |
| IQHSDI | Sin Déficit (Invernada) | `invernada_sin_deficit` | — |
| IQHCNI | Caudal Natural (Invernada) | `invernada_caudal_natural` | — |
| — | Bocatoma Cañelón | `maule_bocatoma_canelon` | — |
| IVMDEIF | Economía Invernada | — | `invernada_vol_econ` |

### 8.3 Maule UserConstraints

| PLP Origin | gtopt Name |
|---|---|
| `genpdmaulen.f` balance | `invernada_balance` |
| `genpdmaulen.f` % ordinario eléctrico | `maule_pct_ordinario_elec` |
| `genpdmaulen.f` % ordinario riego | `maule_pct_ordinario_riego` |
| `genpdmaulen.f` district allocation | `dist_retiro_*` (per district) |

---

## 9. PLP vs gtopt LP Comparison

### 9.0 LP Variable Naming Conventions

#### PLP LP Names (from Fortran `genpdlajam.f` / `genpdmaule.f`)

PLP constructs column names with a **prefix** (`l_` for Laja, `m_` for
Maule) + **base name** + optional **block suffix**:

- **Block-level**: `l_qdr_1`, `l_qdr_2`, ..., `m_qmne_1`, `m_qmne_2`, ...
- **Stage-level**: `l_vdrf`, `l_qdrh`, `m_vmgemf`, `m_qmneh`

Constraint rows use generic indices (`c1`, `c2`, ...) — not named.

| Prefix | Laja Block Vars | Laja Stage Vars |
|--------|----------------|-----------------|
| `l_qdr_j` | Irrigation rights flow (IQDR) | `l_qdrh` (hourly avg) |
| `l_qde_j` | Electric rights flow (IQDE) | `l_qdeh` (hourly avg) |
| `l_qdm_j` | Mixed rights flow (IQDM) | `l_qdmh` (hourly avg) |
| `l_qga_j` | Anticipated flow (IQGA) | `l_qgah` (hourly avg) |
| `l_qriK_j` | Retiro K flow | `l_qrihK` / `l_qrdhK` / `l_qrhrK` |
| — | — | `l_vdrf`/`l_vdef`/`l_vdmf`/`l_vgaf` (volume spent) |
| — | — | `l_qgth` (turbined avg), `l_qlaja` (total) |

| Prefix | Maule Block Vars | Maule Stage Vars |
|--------|-----------------|------------------|
| `m_qmne_j` | Normal electric (IQMNE) | `m_qmneh` (hourly avg) |
| `m_qmnr_j` | Normal irrigation (IQMNR) | `m_qmnrh` (hourly avg) |
| `m_qmoe_j` | Ordinary electric (IQMOE) | `m_qmoeh` (hourly avg) |
| `m_qmor_j` | Ordinary irrigation (IQMOR) | `m_qmorh` (hourly avg) |
| `m_qmce_j` | Compensation (IQMCE) | `m_qmceh` (hourly avg) |
| `m_qmei_j` | Winter savings (IQMEI) | `m_qmeih` (hourly avg) |
| `m_qidn_j` | Invernada deficit (IQIDN) | `m_qidnh` (hourly avg) |
| `m_qisd_j` | Invernada no-deficit (IQISD) | `m_qisdh` (hourly avg) |
| `m_qter_j` | Delivered to irrigation (IQTER) | `m_qterh` (hourly avg) |
| — | — | `m_vmgemf`..`m_vmdeif` (volume accumulators) |
| — | — | `m_qr105`/`m_qa105` (Res 105), `m_qhinv`/`m_qhnein` |

#### gtopt LP Names (with `--lp-names-level 2`)

gtopt uses structured names with entity-type prefix and UID suffix:

```
{entity_prefix}_{uid}_{scenario}_{stage}_{block}
```

| Prefix | Entity | Example |
|--------|--------|---------|
| `frt_flow_` | FlowRight per-block flow | `frt_flow_2001_51_1_1` |
| `frt_fail_` | FlowRight deficit variable | `frt_fail_2001_51_1_1` |
| `frt_qeh_` | FlowRight stage-average | `frt_qeh_2001_51_1` |
| `vrt_vol_` | VolumeRight end volume | `vrt_vol_2005_51_1_1` |
| `vrt_extraction_` | VolumeRight extraction rate | `vrt_extraction_2005_51_1_1` |
| `vrt_sini_` | VolumeRight initial state | `vrt_sini_2005_51_1` |
| `vrt_saving_` | VolumeRight economy deposit | `vrt_saving_2009_51_1_1` |
| `rsv_volumen_` | Reservoir end volume | `rsv_volumen_37_51_1_1` |
| `rsv_extraction_` | Reservoir total extraction | `rsv_extraction_37_51_1_1` |

PAMPL constraints use their defined name with suffixes:
`laja_particion_derechos_s51_t1_b1`

### 9.0b Coefficient Scaling

Both systems convert flow (m³/s) to volume using duration, but with
different unit scaling:

| Factor | PLP | gtopt |
|--------|-----|-------|
| Flow → volume coefficient | `dur_hours × 3.6` (units: 1000 m³) | `dur_hours × 3.6e-3 × flow_scale / energy_scale` (units: scaled hm³) |
| Objective scaling | None (raw $/hm³) | Divided by `scale_objective` (default 1e7) |
| Volume storage units | 1000 m³ | hm³ / `energy_scale` |

**Per-reservoir `variable_scales`** in gtopt provide numerical
conditioning without changing the mathematical formulation:

| Reservoir | energy_scale | flow_scale | Purpose |
|-----------|-------------|------------|---------|
| CIPRESES (6) | 100 | 0.1 | Small reservoir (~175 hm³) |
| COLBUN (28) | 1000 | 1.0 | Medium reservoir (~1500 hm³) |
| EL TORO (37) | 10000 | 10.0 | Large reservoir (~5586 hm³) |

The `rsv_extraction` and `rsv_drain` variables use the reservoir's
`flow_scale`, while `vrt_extraction` variables use native m³/s
(`flow_scale=1`). This creates apparent coefficient ratios in the
reservoir balance:

```
rsv_extraction coefficient = dur_hr × 3.6e-3 × flow_scale / energy_scale
vrt_extraction coeff = dur_hr × 3.6e-3 × 1.0      / energy_scale
ratio = 1 / flow_scale
```

| Reservoir | fext coeff (b1, 7h) | extraction coeff | Ratio | Explanation |
|-----------|-------------------|-----------------|-------|-------------|
| CIPRESES (6) | 2.52e-05 | 2.52e-04 | **10×** | 1/0.1 = 10 |
| COLBUN (28) | 2.52e-05 | 2.52e-05 | **1×** | 1/1.0 = 1 |
| EL TORO (37) | 2.52e-05 | 2.52e-06 | **0.1×** | 1/10.0 = 0.1 |

These ratios are **correct numerical conditioning**, not anomalies.
PLP uses uniform coefficients (`dur × 3.6`) because all flows are in
native m³/s without per-reservoir scaling.

**Verified for block 1 (duration = 7 hours):**
- PLP: `25.2 = 7 × 3.6` (uniform for all flow terms)
- gtopt CIPRESES: `2.52e-05 = 7 × 3.6e-3 × 0.1 / 100`
- gtopt COLBUN: `2.52e-05 = 7 × 3.6e-3 × 1.0 / 1000`
- gtopt EL TORO: `2.52e-05 = 7 × 3.6e-3 × 10.0 / 10000`

### 9.0c Filtration Coefficient Verification

**PLP** (CIPRESES rsv 6): `qf6 = 12.103 + 1.9689e-05 × vf6`
(where vf6 in 1000 m³, so slope per hm³ = 0.019689)

**gtopt**: `wwy_flow_134 = 12.103 + 0.9845 × (eini + vf_last)`
(in scaled units, energy_scale=100, so slope per hm³ =
2 × 0.9845 / 100 = 0.019689)

**Exact match** — gtopt uses average of initial and final volume; the
coefficients are algebraically equivalent.

### 9.1 Laja — Column (Variable) Comparison

| PLP Index | PLP LP Name | Unit | gtopt LP Column | Status |
|---|---|---|---|---|
| IQGT (per block) | `l_qgt_j` | m³/s | `frt_flow_{laja_q_turbinado}` | **Match** |
| IQDR (per block) | `l_qdr_j` | m³/s | `frt_flow_{laja_der_riego}` | **Match** |
| IQDE (per block) | `l_qde_j` | m³/s | `frt_flow_{laja_der_electrico}` | **Match** |
| IQDM (per block) | `l_qdm_j` | m³/s | `frt_flow_{laja_der_mixto}` | **Match** |
| IQGA (per block) | `l_qga_j` | m³/s | `frt_flow_{laja_gasto_anticipado}` | **Match** |
| IQRI (per district) | `l_qriK_j` | m³/s | `frt_flow_{district_category}` | **Match** |
| IQDRH | `l_qdrh` | m³/s | `frt_qeh_{laja_der_riego}` | **Match** |
| IQDEH | `l_qdeh` | m³/s | `frt_qeh_{laja_der_electrico}` | **Match** |
| IQDMH | `l_qdmh` | m³/s | `frt_qeh_{laja_der_mixto}` | **Match** |
| IQGAH | `l_qgah` | m³/s | `frt_qeh_{laja_gasto_anticipado}` | **Match** |
| IQGTH | `l_qgth` | m³/s | `frt_qeh_{laja_q_turbinado}` | **Match** |
| IVDRF | `l_vdrf` | hm³ | `vrt_vol_{laja_vol_der_riego}` | **Match** |
| IVDEF | `l_vdef` | hm³ | `vrt_vol_{laja_vol_der_electrico}` | **Match** |
| IVDMF | `l_vdmf` | hm³ | `vrt_vol_{laja_vol_der_mixto}` | **Match** |
| IVGAF | `l_vgaf` | hm³ | `vrt_vol_{laja_vol_gasto_anticipado}` | **Match** |
| IVESF | `l_vesf` | hm³ | `vrt_vol_{laja_vol_econ_endesa}` | **Match** |
| IVERF | `l_verf` | hm³ | `vrt_vol_{laja_vol_econ_reserva}` | **Match** |
| IVAPF | `l_vapf` | hm³ | `vrt_vol_{laja_vol_econ_polcura}` | **Match** |
| IVESN (saving) | `l_vesn` | hm³ | `vrt_saving_{laja_vol_econ_endesa}` | **Match** |
| IVERN (saving) | `l_vern` | hm³ | `vrt_saving_{laja_vol_econ_reserva}` | **Match** |
| IVAPN (saving) | `l_vapn` | hm³ | `vrt_saving_{laja_vol_econ_polcura}` | **Match** |
| — (extraction) | — | m³/s | `vrt_extraction_{laja_vol_econ_endesa}` | **Match** |
| — (extraction) | — | m³/s | `vrt_extraction_{laja_vol_econ_reserva}` | **Match** |
| — (extraction) | — | m³/s | `vrt_extraction_{laja_vol_econ_polcura}` | **Match** |
| IQRS/IQPR/IQNR/IQER/IQSR | `l_qrs`/`l_qpr`/`l_qnr`/`l_qer`/`l_qsr` | m³/s | — | **Not modeled** ¹ |
| IQLAJA | `l_qlaja` | m³/s | — | **Not modeled** ² |
| IQDEFM / IQHI | `l_qdefm`/`l_qhi` | m³/s | — | **Not modeled** ³ |

**Notes:**
1. PLP decomposes irrigation rights into sub-categories (primary 80%,
   new 20%, emergency, saltos) via a separate balance node.  In gtopt,
   districts withdraw directly — the allocation is preserved through
   fixed discharge schedules.
2. Total Laja discharge is implicitly satisfied by the reservoir balance
   in the hydro model.
3. Minimum turbine flow and intermediate basin flow are handled by the
   hydro model, not the rights module.

### 9.2 Laja — Row (Constraint) Comparison

| PLP Constraint | Equation | gtopt Equivalent | Status |
|---|---|---|---|
| Flow partition (per block) | `qgt = qdr + qde + qdm + qga` | UserConstraint `laja_particion_derechos` | **Match** |
| Hourly accumulation (×5) | `qeh = Σ dur_ratio × flow` | FlowRight `use_average=True` | **Match** |
| Volume accumulation (×4) | `IVDRF = prev - dt × IQDRH` | VolumeRight balance | **Match** |
| Economy balances (×3) | `IVESF = prev + saving - extraction` | VolumeRight balance (with saving) | **Match** |
| Max daily rights (×4) | `fmax × usage_factor` | FlowRight `fmax` schedule | **Match** |
| Deficit coupling | `flow + fail >= discharge` | FlowRight demand row | **Match** |
| Supply partition | `IQRS = IQPR + IQNR + IQER + IQSR` | — | **Not modeled** ¹ |
| Total discharge identity | `IQLAJA = IQGTH - filtration` | — | **Not modeled** ² |

### 9.3 Maule — Column (Variable) Comparison

| PLP Index | PLP LP Name | Unit | gtopt LP Column | Status |
|---|---|---|---|---|
| IQMNE (per block) | `m_qmne_j` | m³/s | `frt_flow_{maule_gasto_normal_elec}` | **Match** |
| IQMNR (per block) | `m_qmnr_j` | m³/s | `frt_flow_{maule_gasto_normal_riego}` | **Match** |
| IQMOE (per block) | `m_qmoe_j` | m³/s | `frt_flow_{maule_gasto_ordinario_elec}` | **Match** |
| IQMOR (per block) | `m_qmor_j` | m³/s | `frt_flow_{maule_gasto_ordinario_riego}` | **Match** |
| IQMCE (per block) | `m_qmce_j` | m³/s | `frt_flow_{maule_compensacion_elec}` | **Match** |
| IQCANELON | `m_qcan_j` | m³/s | `frt_flow_{maule_bocatoma_canelon}` | **Match** |
| IQIDN | `m_qidn_j` | m³/s | `frt_flow_{invernada_deficit}` | **Match** |
| IQISD | `m_qisd_j` | m³/s | `frt_flow_{invernada_sin_deficit}` | **Match** |
| IQRI (per district) | `m_qriK_j` | m³/s | `frt_flow_{retiro_*}` | **Match** |
| IQR105 | `m_qr105` | m³/s | `frt_flow_{maule_resolucion_105}` | **Match** |
| IQNINV | `m_qninv` | m³/s | `frt_flow_{invernada_caudal_natural}` | **Match** |
| IQHINV | `m_qhinv` | m³/s | `frt_flow_{invernada_embalsar}` | **Match** |
| IQHNEIN | `m_qhnein` | m³/s | `frt_flow_{invernada_no_embalsar}` | **Match** |
| IVMGEMF | `m_vmgemf` | hm³ | `vrt_vol_{maule_vol_gasto_elec_mensual}` | **Match** |
| IVMGEAF | `m_vmgeaf` | hm³ | `vrt_vol_{maule_vol_gasto_elec_anual}` | **Match** |
| IVMGRTF | `m_vmgrtf` | hm³ | `vrt_vol_{maule_vol_gasto_riego_temp}` | **Match** |
| IVMDOEF | `m_vmdoef` | hm³ | `vrt_vol_{maule_vol_reserva_ord_elec}` | **Match** |
| IVMDORF | `m_vmdorf` | hm³ | `vrt_vol_{maule_vol_reserva_ord_riego}` | **Match** |
| IVMDCEF | `m_vmdcef` | hm³ | `vrt_vol_{maule_vol_compensacion_elec}` | **Match** |
| IVMDEIF | `m_vmdeif` | hm³ | `vrt_vol_{invernada_vol_econ}` | **Match** |
| IQMEI (extraordinary) | `m_qmei_j` | m³/s | — | **Not modeled** ⁴ |
| IQTER (total irrigation) | `m_qter_j` | m³/s | — | **Not modeled** ⁵ |
| VMGOEF/VMGORF (ordinary) | `m_vmgoef`/`m_vmgorf` | hm³ | — | **Not modeled** ⁶ |
| VMUTIL/VMREB (auxiliary) | `m_vmutil`/`m_vmreb` | hm³ | — | **Not modeled** ⁷ |

**Notes:**
4. Extraordinary zone block-level flow (IQMEI) is not explicitly modeled.
   Only active when reservoir is in the extraordinary zone (rare).
5. Total irrigation delivery is implicit (sum of district flows).
6. PLP tracks ordinary reserve extraction as state variables; gtopt
   enforces the percentage constraint per-block (tighter enforcement).
7. Auxiliary volume accounting variables handled by `bound_rule`.

### 9.4 Maule — Row (Constraint) Comparison

| PLP Constraint | Equation | gtopt Equivalent | Status |
|---|---|---|---|
| Invernada balance | `deficit + no_deficit + natural = storage + bypass` | UserConstraint `invernada_balance` | **Match** |
| % ordinary electric | `elec_ord <= pct × total_ord` | UserConstraint `maule_pct_ordinario_elec` | **Match** |
| % ordinary irrigation | `irr_ord <= pct × total_ord` | UserConstraint `maule_pct_ordinario_riego` | **Match** |
| District allocation (×N) | `district = pct × total_irrigation` | UserConstraint `dist_retiro_*` | **Match** |
| Volume balances (×7) | `vol = prev + dt × flow` | VolumeRight balance rows | **Match** |
| Hourly accumulation (×10+) | `qeh = Σ dur_ratio × flow` | FlowRight `use_average` | **Match** |
| Res 105 minimum | `flow = ecological_requirement` | FlowRight `discharge` | **Match** |
| Per-central flow partition | `central_flow = Σ rights_flows` | — | **Not modeled** ⁸ |
| Delivery constraint | delivery equation | — | **Not modeled** ⁵ |
| Armerillo balance | flow reconstruction | — | **Not modeled** ⁹ |

**Notes:**
8. PLP partitions each central's turbine flow into rights categories.
   gtopt uses aggregated FlowRights across all centrals — the
   optimization allocates extraction optimally anyway.
9. Armerillo flow reconstruction is an accounting identity for
   reporting, not an optimization constraint.

### 9.5 Coverage Summary

| Category | PLP Vars | gtopt Vars | Coverage |
|---|---|---|---|
| Laja block flows | 4 types | 4 FlowRights | 100% |
| Laja stage accumulators | 4 rights + 3 economy | 4 + 3 VolumeRights | 100% |
| Laja hourly accumulators | 5 | 5 (FlowRight averaging) | 100% |
| Laja supply partition | 5 | 0 | 0% — modeled differently |
| Laja districts | N × NBlk | N × NBlk | 100% |
| Maule block flows | 9 types | 7 FlowRights | 78% |
| Maule stage accumulators | 9 state vars | 7 VolumeRights | 78% |
| Maule Invernada | 6 flows | 5 FlowRights | 83% |
| Maule districts | N × NBlk | N × NBlk | 100% |

### 9.6 Key Structural Differences

1. **Per-central vs aggregated flow partition**: PLP partitions each
   central's turbine flow into rights categories.  gtopt uses aggregated
   FlowRights — the optimization allocates extraction across centrals.

2. **Irrigation supply decomposition (Laja)**: PLP has an explicit
   80%/20% split between primary and new irrigators.  gtopt districts
   have fixed demand schedules that implicitly enforce the same split.

3. **Ordinary reserve accumulation (Maule)**: PLP tracks accumulated
   ordinary reserve extraction as state variables.  gtopt enforces the
   percentage constraint per-block (tighter, no temporal smoothing).

4. **Dynamic min turbine bounds**: PLP computes minimum generation
   bounds from irrigation demand and economics.  gtopt relies on
   demand satisfaction penalties (fail_cost) to incentivize generation.

### 9.7 Reservoir Balance Comparison (Stage 1, Block 1, dur=7h)

The reservoir balance is where irrigation variables couple to the
physical hydro model.  Coefficient matching was verified on freshly
generated LP files (April 2026).

#### CIPRESES (rsv 6, energy_scale=100, flow_scale=0.1)

**PLP:**
```
c238: 25.2 qg6_1 + 25.2 qv6_1 - 25.2 qaf6_1 + 25.2 qe6_1 + 25.2 qf6 = 861.84
```
All flow terms: `25.2 = 7 × 3.6` (uniform).

**gtopt:**
```
rsv_volumen_6: +2.52e-05 rsv_extraction_6 + 2.52e-05 rsv_drain_6
               + 0.000252 vrt_extraction_1012 - rsv_sini_6 + rsv_volumen_6 = 0
```
- `rsv_extraction` coeff: `7 × 3.6e-3 × 0.1 / 100 = 2.52e-05` ✓
- `vrt_extraction` coeff: `7 × 3.6e-3 × 1.0 / 100 = 2.52e-04`
  (10× fext — because `1/flow_scale = 1/0.1 = 10`) ✓

#### EL TORO (rsv 37, energy_scale=10000, flow_scale=10)

**PLP:**
```
c241: 25.2 qg37_1 + 25.2 qv37_1 - 25.2 qaf37_1 + 25.2 qe37_1 = 1400.544
```

**gtopt:**
```
rsv_volumen_37: +2.52e-05 rsv_extraction_37 + 2.52e-05 rsv_drain_37
                + 2.52e-06 vrt_extraction_2005 (laja_vol_irr)
                + 2.52e-06 vrt_extraction_2006 (laja_vol_elec)
                + 2.52e-06 vrt_extraction_2007 (laja_vol_mixed)
                + 2.52e-06 vrt_extraction_2008 (laja_vol_anticipated)
                + 2.52e-06 vrt_extraction_2009 (laja_vol_econ_endesa)
                + 2.52e-06 vrt_extraction_2010 (laja_vol_econ_reserve)
                + 2.52e-06 vrt_extraction_2011 (laja_vol_econ_polcura)
                - rsv_sini_37 + rsv_volumen_37 = 0
```
- `rsv_extraction` coeff: `7 × 3.6e-3 × 10 / 10000 = 2.52e-05` ✓
- `vrt_extraction` coeff: `7 × 3.6e-3 × 1.0 / 10000 = 2.52e-06`
  (0.1× fext — because `1/flow_scale = 1/10 = 0.1`) ✓

#### COLBUN (rsv 28, energy_scale=1000, flow_scale=1)

**PLP:**
```
c240: 25.2 qg28_1 + 25.2 qv28_1 - 25.2 qv25_1 - 25.2 qaf28_1
      + 25.2 qe28_1 + 25.2 qx28@1_1 + 25.2 qx28@2_1 + 25.2 qf28 = 1726.2
```
PLP uses 2 aggregate extraction vars: `qx28@1` (0–24 m³/s),
`qx28@2` (0–50 m³/s).

**gtopt:**
```
rsv_volumen_28: +2.52e-05 rsv_extraction_28 + 2.52e-05 rsv_drain_28
                + 2.52e-05 vrt_extraction_1006 (maule_vol_elec_mensual)
                + 2.52e-05 vrt_extraction_1007 (maule_vol_elec_anual)
                + 2.52e-05 vrt_extraction_1008 (maule_vol_riego_temp)
                + 2.52e-05 vrt_extraction_1009 (maule_vol_compensacion)
                + 2.52e-05 vrt_extraction_1010 (maule_vol_rext_elec)
                + 2.52e-05 vrt_extraction_1011 (maule_vol_rext_riego)
                - rsv_sini_28 + rsv_volumen_28 = 0
```
- All coefficients: `7 × 3.6e-3 × 1.0 / 1000 = 2.52e-05` (uniform) ✓
- PLP's 2 aggregate vars → gtopt's 6 granular vars (finer tracking)

### 9.8 Objective Coefficient Comparison (Laja, March)

| Variable | gtopt coeff | × scale_obj (1e7) | PAMPL param | Match |
|---|---|---|---|---|
| `frt_fail` irr rights | 0.00011 | 1100 | `cost_irr_ns=1100` | ✓ |
| `frt_fail` elec rights | 0.0001265 | 1265 | `cost_elec_ns=1150 × month[12]=1.1` | ✓ |
| `frt_flow` elec (usage) | -1.1e-08 | -0.11 | `cost_elec_uso=0.1 × month[12]=1.1` | ✓ |
| `frt_flow` mixed (usage) | -1e-07 | -1.0 | `cost_mixed=1.0` | ✓ |

### 9.9 Volume Right Bounds Comparison (March)

| VolumeRight | PAMPL param | Bound (hm³) | Match |
|---|---|---|---|
| `laja_vol_der_riego` | `max_irr=5000` | ≤ 5000 | ✓ |
| `laja_vol_der_electrico` | `max_elec=1200` | ≤ 1200 | ✓ |
| `laja_vol_der_mixto` | `max_mixed=30` | ≤ 30 | ✓ |
| `laja_vol_gasto_anticipado` | `max_anticipated=5000` | ≤ 5000 | ✓ |
| `maule_vol_gasto_elec_mensual` | `gasto_elec_men_max=25` | ≤ 25 | ✓ |
| `maule_vol_reserva_ord_elec` | `v_der_elect_anu_max=250` | ≤ 250 | ✓ |
| `maule_vol_reserva_ord_riego` | `v_der_riego_temp_max=800` | ≤ 800 | ✓ |
| `maule_vol_compensacion_elec` | `v_comp_elec_max=350` | ≤ 350 | ✓ |

---

## 10. Known Simplifications and Gaps

### 10.1 Economy Reset/Cap Rules (Laja)

| Economy | PLP Behavior | gtopt Behavior | Impact |
|---------|-------------|----------------|--------|
| ENDESA (IVESF) | Caps new provisions on reservoir overflow | Simple accumulator (no cap) | Minor — overflow is rare |
| Reserve (IVERF) | Resets to zero when exiting lower cushion | Simple accumulator (no reset) | Low — reserve economies are emergency-only |
| Alto Polcura (IVAPF) | No reset | No reset | None — equivalent |

### 10.2 La Invernada Economy (Maule)

PLP has conditional reset rules and seasonal usage windows for La
Invernada economy that are not modeled in gtopt.  The current
implementation is a simple accumulator.

### 10.3 Multi-Year Data Overrides

PLP supports annual tables for irrigation percentages (`pct_riego_manual`)
and Resolution 105 flows (`caudal_res105_manual`) that change year by
year.  These multi-year override tables are not consumed by the gtopt
converter.

### 10.4 Colbun 425 Extraction Limit

PLP imposes additional extraction limits when Colbun volume falls below
elevation 425.  This volume-conditional bound is not modeled in gtopt.

### 10.5 Data Coverage

- **Laja**: 43/51 config keys used (84%)
- **Maule**: 31/55 config keys used (56%)

Unused keys are primarily: hydro model concerns, multi-year overrides,
auto-modulation logic, PLP-specific flags, and accumulated volume caps.

---

## 11. Configuration and Usage

Irrigation cases reach gtopt through a three-stage pipeline:

```text
Stage 1 ── PLP .dat files ─────────────▶ canonical JSON
           (plplajam.dat, plpmaulen.dat)  (laja.json, maule.json)
           tool: plp2gtopt

Stage 2 ── canonical JSON ─────────────▶ gtopt entities + PAMPL
           (laja.json, maule.json)        (flow_right_array, volume_right_array,
                                           laja.pampl, maule.pampl)
           tool: gtopt_expand

Stage 3 ── gtopt entities + system ────▶ LP/MIP solution
           (system.json + *.pampl)        (results/*.parquet)
           tool: gtopt
```

Stages 1 and 2 are decoupled so that hand-authored ``laja.json`` /
``maule.json`` can be fed straight into Stage 2, bypassing PLP entirely.
The canonical JSON schema is documented in the module docstrings of
``scripts/gtopt_expand/laja_agreement.py`` and
``scripts/gtopt_expand/maule_agreement.py``.

### 11.1 Stage 1 — PLP-to-gtopt Conversion

```bash
# Convert PLP case including irrigation agreements
plp2gtopt -i /path/to/plp_case -o /path/to/gtopt_case

# With first scenario only (faster)
plp2gtopt -i /path/to/plp_case -o /path/to/gtopt_case --first-scenario
```

The converter automatically detects `plplajam.dat` and `plpmaulen.dat`
in the input directory, writes canonical `laja.json` / `maule.json`
artifacts, and (for backward compatibility) also emits the Stage-2
entity and PAMPL files directly.

### 11.2 Stage 2 — `gtopt_expand` CLI

The `gtopt_expand` package (`scripts/gtopt_expand/`) is the
canonical Stage-2 transform.  It consumes a canonical JSON agreement
description and emits the corresponding gtopt entity arrays plus the
companion PAMPL file.

```bash
# Laja: laja.json → laja_entities.json + laja.pampl
gtopt_expand laja \
    --input  cases/my_case/laja.json \
    --output cases/my_case/irrigation/

# Maule with explicit per-stage metadata (calendar months)
gtopt_expand maule \
    --input  cases/my_case/maule.json \
    --output cases/my_case/irrigation/ \
    --stages cases/my_case/stages.json

# Show the package version
gtopt_expand --version
```

Flags:

| Flag | Default | Description |
|------|---------|-------------|
| `--input PATH` (alias `--in`) | — | Path to `laja.json` or `maule.json`. |
| `--output DIR` (alias `--out`) | — | Directory where entities JSON and `.pampl` are written. |
| `--stages PATH` | `None` | Optional JSON file with per-stage `{number, month}` dicts (or `{"stages": [...]}` wrapper). When omitted, monthly arrays stay in raw hydro-year (Laja) or calendar (Maule) form — convenient for cases that don't need per-stage materialization. |
| `--last-stage N` | `-1` (all) | Truncate the stage list to the first `N` stages. |
| `--blocks-per-stage N` | `1` | Replicate each per-stage value across `N` blocks for block-level schedules. |
| `--version` | — | Print package version and exit. |

**Outputs:**

- `<artifact>_entities.json` — contains `flow_right_array` and
  `volume_right_array` (and, when constraints are inline,
  `user_constraint_array`).
- `<artifact>.pampl` — AMPL constraint definitions and parameters,
  referenced from the entities file via `user_constraint_file`
  (both singular and plural `user_constraint_files` are accepted
  by the C++ `System` parser).

**Exit codes:**

| Code | Meaning |
|------|---------|
| `0` | Success. Writes a one-line `wrote <path>` summary to stderr. |
| `2` | Input/validation/IO error (missing file, malformed JSON, schema key missing, template failure). A one-line `ERROR: ...` is written to stderr. |

**`--stages` file format:** a plain list, or an object with a `stages`
key, of per-stage dicts. Each dict must contain at least `number` (1-based
stage index) and `month` (1 = January .. 12 = December, calendar months):

```json
[
  {"number": 1, "month": 4},
  {"number": 2, "month": 5},
  {"number": 3, "month": 6}
]
```

The Laja agreement translates calendar months back to hydrological-year
indices (April = 0 .. March = 11) internally, so the `--stages` file
should always use **calendar** months regardless of agreement.

### 11.3 Output Files

The full pipeline produces:
- `system.json` — contains `flow_right_array`, `volume_right_array`, and
  `user_constraint_files` pointing to the PAMPL files
- `laja.pampl` — Laja constraint definitions and parameters
- `maule.pampl` — Maule constraint definitions and parameters

### 11.4 Running with LP Debug

To inspect the irrigation LP structure:

```bash
gtopt /path/to/case --lp-names-level 2 \
  -s lp_debug_options.json
```

Where `lp_debug_options.json` contains:

```json
{
  "options": {
    "lp_debug": true,
    "lp_compression": "uncompressed"
  }
}
```

This writes `.lp` files to `results/logs/` where irrigation variables
and constraints can be inspected.

---

## 12. See Also

### Agreement Template Specifications

- **[Laja Agreement Template](../scripts/gtopt_expand/templates/laja.md)** —
  Complete specification of the Laja irrigation agreement: basin topology
  (Mermaid diagrams), piecewise-linear volume zone model (LaTeX formulas),
  FlowRight/VolumeRight/UserConstraint entity definitions (`laja.tson`),
  and AMPL constraint parameters (`laja.tampl`)
- **[Maule Agreement Template](../scripts/gtopt_expand/templates/maule.md)** —
  Complete specification of the Maule irrigation agreement: three-zone
  reservoir operation, Armerillo control point, La Invernada winter
  storage, Resolution 105 ecological flow, 7 irrigation districts,
  entity definitions (`maule.tson`) and constraints (`maule.tampl`)

### Reference Documentation

- **[User Constraints — Syntax Reference](user-constraints.md)** —
  Complete constraint expression syntax and PAMPL format
- **[Input Data Reference](input-data.md)** — JSON schema for
  FlowRight, VolumeRight, and UserConstraint fields
- **[Mathematical Formulation](formulation/mathematical-formulation.md)**
  — LP/MIP formulation including water rights
- **[PLP Implementation Analysis](analysis/irrigation_agreements/plp_implementation.md)**
  — Detailed Fortran source analysis
- **[PLP vs gtopt LP Comparison](lp-comparison.md)** — Full LP
  comparison covering generators, demands, lines, hydro, batteries,
  and SDDP structure (non-irrigation elements)
- **[LP Column & Row Audit](analysis/irrigation_agreements/lp_column_row_audit.md)**
  — Exhaustive PLP vs gtopt variable/constraint comparison
