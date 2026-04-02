# Right Junctions (Puntos de Control de Derechos) in PLP Implementations

## Overview

A **Right Junction** is a water rights balance node -- a point in the rights
accounting graph (NOT the physical hydro topology) where water rights claims
are balanced against available supply. This is the rights-domain counterpart
of the physical `Junction` where water mass is balanced.

In gtopt, `RightJunction` creates a balance row per block:
```
sum_{fr : direction=+1} flow(fr,b) - sum_{fr : direction=-1} flow(fr,b)
    + drain(b) >= 0
```

This document catalogs the right junctions defined explicitly or implicitly
in the PLP Fortran implementations of the Maule and Laja irrigation agreements.

---

## 1. Maule Agreement Right Junctions

### 1.1 Armerillo Balance Point

The **Armerillo** point is the most explicit right junction in the PLP code.
It appears as a named balance point in `plpmaulen.dat`:

```
# Descuenta caudales de derechos electricos del balance en Armerillo
T
```

**Physical location**: DGA fluviometric station on the Maule River,
downstream of the Maule-Melado confluence and upstream of the irrigation
canal intakes. In PLP, this is the variable `IQARMR` (variable index 31,
constraint index 28 in `genpdmaule.f`).

**Balance equation**: The Armerillo balance enforces that the total water
flow at the station equals the sum of all contributions:

```
Q_armerillo = Q_intermediate + Q_invernada + Q_maule_release
              - Q_electric_normal - Q_electric_reserve
```

When the flag "Descuenta caudales de derechos electricos del balance en
Armerillo" is TRUE, electrical rights flows are **subtracted** from the
balance, meaning only the irrigation-available portion must satisfy the
irrigation demand constraints downstream.

**gtopt mapping**: A `RightJunction` named "armerillo" with `drain=true`
(excess supply is allowed). `FlowRight` entities connecting to it:

| FlowRight | Direction | Source |
|-----------|-----------|--------|
| Q_intermediate (hoya intermedia) | +1 | Lateral inflows |
| Q_invernada release | +1 | La Invernada discharge |
| Q_maule_release | +1 | Lago Maule controlled release |
| Q_electric_normal | -1 | Normal regime electric rights |
| Q_electric_reserve | -1 | Reserve regime electric rights |
| Irrigation delivery (IQTER) | -1 | Total irrigation withdrawal |

### 1.2 Maule Flow Partition Point

At each generation node (LMAULE, CIPRESES, PEHUENCHE, COLBUN), PLP creates
a **flow partition constraint** that splits turbined flow into rights
categories:

```
-qg_i_j + m_qmne_j + m_qmnr_j + m_qmoe_j + m_qmor_j + m_qmce_j = 0
```

This is an **implicit right junction** at each power plant where total
generation flow is partitioned into:
- `m_qmne`: Normal Electric rights
- `m_qmnr`: Normal Irrigation rights
- `m_qmoe`: Ordinary Electric rights
- `m_qmor`: Ordinary Irrigation rights
- `m_qmce`: ENDESA Compensation

**gtopt mapping**: This could be modeled as a `RightJunction` per
generation plant with `drain=false` (strict equality -- the partition
must be exact).

### 1.3 Delivery Constraint Point

The delivery constraint ensures irrigation delivery does not exceed
available supply:

```
-IQDRAH + IQDRMH + IQINVH <= 0    (delivery <= available)
```

Where:
- `IQDRAH`: Irrigation demand at the delivery point
- `IQDRMH`: Released irrigation flow from Maule
- `IQINVH`: Released irrigation flow from La Invernada

**gtopt mapping**: A `RightJunction` at the irrigation delivery point
with `drain=true`.

### 1.4 La Invernada Winter Balance

The La Invernada winter lagoon balance:

```
m_qidn_j + m_qisd_j + m_qninv_j - m_qhein_j - m_qhnein_j = QAflInvern
```

This balances inflows and storage/release decisions at the Invernada
winter storage reservoir in the rights domain.

### 1.5 Irrigation Distribution Points

Seven irrigation withdrawal points defined in `plpmaulen.dat`:

| Withdrawal Point | Allocation % | Has Slack? |
|-----------------|-------------|-----------|
| RieCMNA (Canal Maule Norte A) | 12.66% | No |
| RieCMNB (Canal Maule Norte B) | 14.70% | No |
| RieMaitenes | 10.49% | No |
| RieMauleSur | 11.97% | No |
| RieMelado | 12.65% | No |
| RieSur123SCDZ (Sectors 1-2-3) | 34.27% | **Yes** |
| RieMolinosOtros | 3.26% | No |

Total: 100.0%

The `Holgura` (slack) flag on RieSur123SCDZ indicates that this district
can receive less than its allocation without penalty -- it has a drain.
All other districts must receive exactly their allocation share.

**gtopt mapping**: These are `FlowRight` entities with `direction=-1`
connected to the Armerillo/delivery `RightJunction`. The percentage
allocation is applied to scale the total irrigation flow into per-district
demands.

### 1.6 Bocatoma Canelon

A specialized infrastructure node with civil works cost:
```
'BCanelon'
# Costo Obra Civil Canelon, por m3/seg
10
```

This represents the Canelon intake structure where irrigation water is
diverted from the main river. It has an associated cost per m3/s.

### 1.7 Resolucion 105 Flow Requirements

Monthly minimum flow requirements at the Maule confluence:

| Month | ConflMaule (m3/s) | Res105 (m3/s) |
|-------|-------------------|---------------|
| Apr | 60 | 80 |
| May | 20 | 40 |
| Jun | 0 | 40 |
| Jul | 0 | 40 |
| Aug | 0 | 40 |
| Sep | 40 | 60 |
| Oct | 100 | 140 |
| Nov | 182 | 180 |
| Dec | 200 | 200 |
| Jan | 200 | 200 |
| Feb | 160 | 180 |
| Mar | 110 | 120 |

These represent legally-mandated flow balance requirements.

**gtopt mapping**: `FlowRight` entities with `direction=-1` at the
confluence right junction, with seasonal `discharge` schedules.

---

## 2. Laja Agreement Right Junctions

### 2.1 El Toro Flow Partition Point

The primary right junction in the Laja system is at the El Toro generation
plant, where total turbined flow is partitioned into rights categories:

```
-qgt_j + qdr_j + qde_j + qdm_j + qga_j = 0    (flow partition)
```

Where:
- `qgt_j`: Total generation turbine flow in block j
- `qdr_j`: Irrigation rights flow (Derechos de Riego)
- `qde_j`: Electrical rights flow (Derechos Electricos)
- `qdm_j`: Mixed rights flow (Derechos Mixtos)
- `qga_j`: Anticipated discharge flow (Gasto Anticipado)

**gtopt mapping**: A `RightJunction` named "laja_partition" with
`drain=false` (strict equality). `FlowRight` entities:

| FlowRight | Direction | Variable |
|-----------|-----------|----------|
| Total generation | +1 | `qgt_j` |
| Irrigation rights | -1 | `qdr_j` |
| Electrical rights | -1 | `qde_j` |
| Mixed rights | -1 | `qdm_j` |
| Anticipated discharge | -1 | `qga_j` |

### 2.2 Irrigation Supply Balance

The irrigation supply is distributed among the demand categories:

```
IQRS - IQPR - IQNR - IQER - IQSR = 0
```

Where:
- `IQRS`: Irrigation rights supply
- `IQPR`: Primary regante (Primeros Regantes) demand
- `IQNR`: Nuevo Riego (new irrigators) demand
- `IQER`: Emergency irrigation demand
- `IQSR`: Saltos del Laja (waterfall) flow

This distributes irrigation water with priorities:
1. Primeros Regantes (80%) -- historical irrigators
2. Nuevo Riego (20%) -- new settlers
3. Within LP: `qrs = 0.8*qpr + 0.2*qnr`

**gtopt mapping**: A `RightJunction` named "laja_irrigation_supply" with
`drain=false`. FlowRight entities for IQRS (+1), IQPR (-1), IQNR (-1),
IQER (-1), IQSR (-1).

### 2.3 Irrigation Withdrawal Points

Three withdrawal points defined in `plplajam.dat`:

| Point | Name | 1oReg % | 2oReg % | Emerg % | Saltos % | Cost Factor |
|-------|------|---------|---------|---------|----------|-------------|
| Zanartu-Collao | RIEGZACO | 37.2% | 0% | 37.2% | 0% | 1.500 |
| Tucapel | RieTucapel | 62.8% | 100% | 62.8% | 0% | 1.000 |
| Saltos del Laja | RieSaltos | 0% | 0% | 0% | 100% | 0.200 |

**Injection names**: RieSaltos injects water back to `'LAJA_I'` (the Laja
river downstream) -- this water re-enters the physical topology.

**Cost factor**: Zanartu-Collao has a 1.5x cost factor (higher penalty
for unserved demand) reflecting its importance.

### 2.4 Total Discharge Identity

```
IQLAJA - IQGTH = 0    (total Laja discharge = total generation)
```

This ensures the total Laja discharge equals the total hourly generation
rate across all turbines in the cascade.

### 2.5 Volume-Dependent Zone Thresholds

The Laja volume zone system defines 4 operational zones:

| Volume Zone | Useful Volume | Irrigation Factor | Electric Factor | Mixed Factor |
|---------|---------------|-------------------|-----------------|-------------|
| Base | -- | 570 m3/s | 0 m3/s | 30 m3/s |
| Zone 1 (1,200 hm3) | 0-1,200 | +0.00 | +0.05 | +1.00 |
| Zone 2 (170 hm3) | 1,200-1,370 | +0.40 | +0.05 | +0.00 |
| Zone 3 (530 hm3) | 1,370-1,900 | +0.40 | +0.40 | +0.00 |
| Zone 4 (3,682 hm3) | 1,900-5,582 | +0.25 | +0.65 | +0.00 |

As volume rises through volume zones, the available rights change.
These create implicit balance points at volume transitions where the
rights partition shifts.

Maximum rights: Irrigation=5000, Electric=1200, Mixed=30, Anticipated=5000

### 2.6 Seasonal Demand Profiles (m3/s)

Default monthly irrigation demands (April-March hydrological year):

| Month | NuevoRiego | RegTucapel | RegAbanico |
|-------|------------|------------|------------|
| Apr | 13.00 | 18.00 | 9.40 |
| May | 0.00 | 0.00 | 0.00 |
| Jun | 0.00 | 0.00 | 0.00 |
| Jul | 0.00 | 0.00 | 0.00 |
| Aug | 0.00 | 0.00 | 0.00 |
| Sep | 19.50 | 27.00 | 14.10 |
| Oct | 42.25 | 58.50 | 30.55 |
| Nov | 55.25 | 76.50 | 39.95 |
| Dec | 65.00 | 90.00 | 47.00 |
| Jan | 65.00 | 90.00 | 47.00 |
| Feb | 52.00 | 72.00 | 37.60 |
| Mar | 32.50 | 45.00 | 23.50 |

### 2.7 State Variables (Inter-Stage Linking)

| Variable | Description |
|----------|-------------|
| `IVDRF` | Irrigation rights remaining volume |
| `IVDEF` | Electrical rights remaining volume |
| `IVDMF` | Mixed rights remaining volume |
| `IVGAF` | Anticipated discharge accumulated volume |
| `IVESF` | ENDESA economy final volume |
| `IVERF` | Reserve economy final volume |
| `IVAPF` | Alto Polcura economy final volume |

---

## 3. Summary: All Right Junctions

### Maule System

| # | Right Junction | Type | drain | Balance Equation |
|---|---------------|------|-------|-----------------|
| 1 | **Armerillo** | Explicit accounting point | true | Q_inter + Q_inv + Q_maule - Q_elec_normal - Q_elec_reserve + drain >= 0 |
| 2 | **Flow partition** (per plant) | Implicit at generation | false | -qg + qmne + qmnr + qmoe + qmor + qmce = 0 |
| 3 | **Delivery** | Implicit delivery | true | -IQDRAH + IQDRMH + IQINVH + drain >= 0 |
| 4 | **Invernada winter** | Implicit seasonal | false | qidn + qisd + qninv - qhein - qhnein = QAflInvern |

### Laja System

| # | Right Junction | Type | drain | Balance Equation |
|---|---------------|------|-------|-----------------|
| 1 | **El Toro partition** | Implicit at generation | false | -qgt + qdr + qde + qdm + qga = 0 |
| 2 | **Irrigation supply** | Implicit distribution | false | IQRS - IQPR - IQNR - IQER - IQSR = 0 |
| 3 | **Laja discharge** | Identity constraint | false | IQLAJA - IQGTH = 0 |

---

## 4. Penalty Costs

### Maule

| Penalty | Value | Description |
|---------|-------|-------------|
| ValorRiego (Maule) | 1100 | Irrigation served value (Conv Maule) |
| ValorRiego (Res105) | 1100 | Irrigation served value (Res 105) |
| CostoRiegoNS (Maule) | 1000 | Unserved irrigation cost (Conv Maule) |
| CostoRiegoNS (Res105) | 1000 | Unserved irrigation cost (Res 105) |
| CostoCanelon | 10 | Canal infrastructure cost per m3/s |
| CostoPenalizadores | 1500, 1000 | Convention penalty costs |

### Laja

| Penalty | Value | Description |
|---------|-------|-------------|
| CRiegoNS | 1100.0 | Irrigation non-served cost |
| CUsoRiego | 0.0 | Cost of using irrigation rights |
| CElectNS | 1150.0 | Electrical rights non-served cost |
| CUsoElect | 0.1 | Cost of using electrical rights |
| CMixto | 1.0 | Mixed rights cost |

---

## 5. Data File Locations

| File | Path |
|------|------|
| Maule config (long term) | `support/plp_long_term/plpmaulen.dat.xz` |
| Maule config (5 year) | `support/plp_5_years/plpmaulen.dat.xz` |
| Maule config (2 year) | `support/plp_2_years/plpmaulen.dat.xz` |
| Laja config (long term) | `support/plp_long_term/plplajam.dat.xz` |
| Laja config (5 year) | `support/plp_5_years/plplajam.dat.xz` |
| Laja config (2 year) | `support/plp_2_years/plplajam.dat.xz` |
| PLP implementation doc | `docs/analysis/irrigation_agreements/plp_implementation.md` |
| PLP Fortran source | `https://github.com/marcelomatus/plp_storage/tree/main/CEN65/src` |
