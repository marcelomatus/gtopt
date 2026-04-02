# Seepage and Colchones (Volume Zones) in Irrigation Agreements

## Overview

This document details how seepage (filtration) interacts with the volume zone
system and how generation/irrigation rights are defined for each reservoir in
the Laja and Maule irrigation agreements.

---

## 1. Seepage (Filtration) in Irrigation Agreements

### 1.1 Laja: Fixed Average Model

**Parameter:** `CauFiltLaja = 47.0 m3/s` (from `plplajam.dat`)

**Formula:**
```
VolFiltLaja = MIN(VoluLaja + VolAflLaja, CauFiltLaja * Cau2Vol(IEtapa))
```

Where:
- `VoluLaja`: Current reservoir useful volume [hm3]
- `VolAflLaja`: Total inflow in the stage [hm3]
- `CauFiltLaja`: Historical average filtration rate [m3/s]
- `Cau2Vol(IEtapa)`: Flow-to-volume conversion for stage duration

The MIN ensures seepage cannot exceed either the available water or the
maximum seepage capacity for the stage.

**Effect on rights:** Filtration is subtracted from available water BEFORE
computing extraction limits. It represents uncontrollable water loss that
reduces availability for both irrigation and generation rights.

**Network node:** `FiltLaja` — explicit filtration node in PLP network.

### 1.2 Maule: Piecewise-Linear Volume-Dependent Model

The Maule system uses volume-dependent seepage defined in `plpfilemb.dat`.
Three reservoirs have seepage curves:

#### ELTORO (Laguna del Laja in Maule context)

**Average filtration:** 30.80 m3/s

| Segment | Volume Threshold (hm3) | Slope (m3/s/hm3) | Constant (m3/s) |
|---------|----------------------|-------------------|-----------------|
| 1 | 0.0 | 0.058532 | 0.0 |
| 2 | 280.0 | 0.005520 | 14.843218 |
| 3 | 2700.0 | 0.007149 | 10.444110 |

**Downstream outlet:** `'ABANICO'` (generation plant)

**Examples:**
- V = 100 hm3: Seepage = 0.058532 x 100 = 5.85 m3/s
- V = 1000 hm3: Seepage = 14.843 + 0.00552 x 1000 = 20.36 m3/s
- V = 5000 hm3: Seepage = 10.444 + 0.00715 x 5000 = 46.19 m3/s

#### CIPRESES (La Invernada)

**Average filtration:** 14.20 m3/s

| Segment | Volume Threshold (hm3) | Slope (m3/s/hm3) | Constant (m3/s) |
|---------|----------------------|-------------------|-----------------|
| 1 | 0.0 | 0.000000 | 0.0 |
| 2 | 4.716 | 1.337199 | -6.306230 |
| 3 | 13.0 | 0.037851 | 10.584810 |
| 4 | 98.0 | 0.074571 | 6.986530 |

**Downstream outlet:** `'FILT_CIPRESES'`

#### COLBUN

**Average filtration:** 6.10 m3/s

| Segment | Volume Threshold (hm3) | Slope (m3/s/hm3) | Constant (m3/s) |
|---------|----------------------|-------------------|-----------------|
| 1 | 0.0 | 0.0000 | 0.0 |
| 2 | 381.6 | 0.0000 | 0.0 |
| 3 | 660.0 | 0.0118 | -7.7963 |

**Downstream outlet:** `'SAN_CLEMENTE'`

Colbun only has significant seepage above 660 hm3.

### 1.3 Seepage Formula (gtopt `ReservoirSeepage`)

For each block, the active segment is selected based on average volume:
```
V_avg = (eini + efin) / 2

Seepage(V_avg) = constant_i + slope_i * V_avg
```

In the LP matrix:
```
filt_flow - slope*0.5*eini - slope*0.5*efin = constant
```

### 1.4 Seepage-Rights Interaction

| Aspect | Laja | Maule |
|--------|------|-------|
| Model type | Fixed average | Piecewise-linear, volume-dependent |
| Formula | MIN(available, CauFilt * duration) | constant + slope * V |
| Average rate | 47.0 m3/s | 30.8 / 14.2 / 6.1 m3/s per reservoir |
| Volume-dependent? | No | Yes (positive slope) |
| Rights effect | Reduces available water pre-rights calc | Reduces volume, may trigger zone transition |
| Network integration | `FiltLaja` node | `FILT_CIPRESES`, `ABANICO`, `SAN_CLEMENTE` |

**Key insight:** Seepage reduces reservoir volume, which can trigger transitions
between volume zones, further reducing extraction rights. This creates a
compounding effect: more seepage -> lower volume -> lower zone -> fewer rights.

---

## 2. Laja Colchones (Volume Zones)

### 2.1 Zone Structure

**Source:** `plplajam.dat` — 4 volume zones

```
                     VolMax = 5,582 hm3
    ─────────────────────────────────────
    │  Zone 4 (3,682 hm3)               │  1,900 - 5,582 hm3
    ├── 1,900 hm3 ──────────────────────┤
    │  Zone 3 (530 hm3)                 │  1,370 - 1,900 hm3
    ├── 1,370 hm3 ──────────────────────┤
    │  Zone 2 (170 hm3)                 │  1,200 - 1,370 hm3
    ├── 1,200 hm3 ──────────────────────┤
    │  Zone 1 (1,200 hm3)               │  0 - 1,200 hm3
    ├── VolMuerto = 0 hm3 ─────────────┤
    ─────────────────────────────────────
```

### 2.2 Rights by Zone

Rights are computed as: `Rights = Base + sum(Factor_i * V_zone_i)`

where `V_zone_i` is the volume within each zone (clamped to zone width).

#### Irrigation Rights (Derechos de Riego)

| Parameter | Zone 1 | Zone 2 | Zone 3 | Zone 4 |
|-----------|--------|--------|--------|--------|
| Width (hm3) | 1,200 | 170 | 530 | 3,682 |
| Factor | 0.00 | 0.40 | 0.40 | 0.25 |
| **Base:** 570 m3/s | **Max:** 5,000 m3/s |

**Example at V = 3,000 hm3 (in Zone 4):**
```
Irrigation = 570 + 0.00*1200 + 0.40*170 + 0.40*530 + 0.25*(3000-1900)
           = 570 + 0 + 68 + 212 + 275
           = 1,125 m3/s
```

#### Electrical Rights (Derechos Electricos)

| Parameter | Zone 1 | Zone 2 | Zone 3 | Zone 4 |
|-----------|--------|--------|--------|--------|
| Width (hm3) | 1,200 | 170 | 530 | 3,682 |
| Factor | 0.05 | 0.05 | 0.40 | 0.65 |
| **Base:** 0 m3/s | **Max:** 1,200 m3/s |

**Example at V = 3,000 hm3:**
```
Electric = 0 + 0.05*1200 + 0.05*170 + 0.40*530 + 0.65*(3000-1900)
         = 60 + 8.5 + 212 + 715
         = 995.5 m3/s
```

#### Mixed Rights (Derechos Mixtos)

| Parameter | Zone 1 | Zone 2 | Zone 3 | Zone 4 |
|-----------|--------|--------|--------|--------|
| Width (hm3) | 1,200 | 170 | 530 | 3,682 |
| Factor | 1.00 | 0.00 | 0.00 | 0.00 |
| **Base:** 30 m3/s | **Max:** 30 m3/s |

Mixed rights are capped at 30 m3/s and only depend on Zone 1. At any
volume > 0, the factor saturates at the maximum.

#### Anticipated Discharge (Gasto Anticipado)

- **Base:** 0 m3/s, **Max:** 5,000 m3/s
- Available **only** during winter months (June-August in hydrological year)
- No volume-dependent factors

### 2.3 Monthly Usage Factors

Rights are further modulated by monthly factors (April-March hydrological year):

| Category | Apr | May | Jun | Jul | Aug | Sep | Oct | Nov | Dec | Jan | Feb | Mar |
|----------|-----|-----|-----|-----|-----|-----|-----|-----|-----|-----|-----|-----|
| Irrigation max factor | 1.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| Electrical max factor | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| Mixed max factor | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| Anticipated max factor | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 1.00 | 1.00 | 1.00 | 0.00 | 0.00 | 0.00 | 0.00 |

### 2.4 Cost Parameters

| Category | Non-Served Cost | Usage Cost |
|----------|----------------|------------|
| Irrigation | 1,100.0 $/m3/s | 0.0 |
| Electrical | 1,150.0 $/m3/s | 0.1 $/m3/s |
| Mixed | 1.0 $/m3/s | -- |

**Monthly modulation of irrigation non-served cost:**
```
Apr: 1.0, May: 0.0, Jun-Aug: 0.0, Sep: 0.1, Oct: 0.2, Nov: 0.5,
Dec-Jan: 1.5, Feb: 1.2, Mar: 1.0
```

### 2.5 Special Rules

**Lower volume zone (VolUtil <= VolUtilColInf):**
- Economics-based generation disabled (`qgesh = 0`)
- Annual rights reduced from 570 to 47 m3/s
- No new economy accumulation allowed

**50cm Rebalse Rule (above VolAcumA50cmReb):**
- Unrestricted spillage permitted
- Accounting capped at 47 m3/s for annual rights
- `qg50` variable freed from upper bound

### 2.6 Withdrawal Points

| Point | Name | 1oReg % | 2oReg % | Emerg % | Saltos % | Cost Factor |
|-------|------|---------|---------|---------|----------|-------------|
| 1 | RIEGZACO (Zanartu-Collao) | 37.2% | 0% | 37.2% | 0% | 1.500 |
| 2 | RieTucapel | 62.8% | 100% | 62.8% | 0% | 1.000 |
| 3 | RieSaltos (Waterfall) | 0% | 0% | 0% | 100% | 0.200 |

**RieSaltos** injects water back to `'LAJA_I'` (downstream river).

### 2.7 Default Irrigation Demands (m3/s)

4 demand curves: 1oRegantes (90.0), 2oRegantes (53.0), Emergencias (0.0),
Saltos (7.0).

Seasonal variation curves:

| Month | 1oReg | 2oReg | Emerg | Saltos |
|-------|-------|-------|-------|--------|
| Apr | 1.00 | 0.20 | 0.20 | 0.00 |
| May-Aug | 0.00 | 0.00 | 0.00 | 0.00 |
| Sep | 1.00 | 0.30 | 0.30 | 0.00 |
| Oct | 1.00 | 0.65 | 0.65 | 0.00 |
| Nov | 1.00 | 0.85 | 0.85 | 0.00 |
| Dec-Jan | 1.00 | 1.00 | 1.00 | 0.50-1.00 |
| Feb | 1.00 | 0.80 | 0.80 | 1.00 |
| Mar | 1.00 | 0.50 | 0.50 | 0.00 |

---

## 3. Maule Colchones (Three-Zone System)

### 3.1 Zone Structure

**Source:** `plpmaulen.dat` — 3-zone system for Lago Maule

```
                          VolMax
    ─────────────────────────────────────
    │  Normal Zone                       │  > 581,000 x10^3 m3
    ├── VResOrd + VResExt = 581,000 ────┤
    │  Ordinary Reserve (452,000)        │  129,000 - 581,000 x10^3 m3
    ├── VReservaExtraord = 129,000 ─────┤
    │  Extraordinary Reserve (129,000)   │  0 - 129,000 x10^3 m3
    ├── VEmbalseUtilMin = 0 ────────────┤
    ─────────────────────────────────────
```

**Volume thresholds (10^3 m3):**
- `VEmbalseUtilMin`: 0
- `VReservaExtraord`: 129,000 (129 hm3)
- `VReservaOrdinaria`: 452,000 (452 hm3)

### 3.2 Rights by Zone

#### Normal Zone (above 581,000 x10^3 m3)

| Parameter | Value |
|-----------|-------|
| GastoElecDiaMax (daily max electric) | 30 m3/s |
| GastoElecMenMax (monthly max electric) | 25 m3/s |
| VDerElectAnuMax (annual max electric volume) | 250,000 x10^3 m3 (250 hm3) |
| Gasto Riego Maximo (max irrigation flow) | 200 m3/s |
| VDerRiegoTempMax (seasonal max irrigation volume) | 800,000 x10^3 m3 (800 hm3) |
| VCompElecMax (max compensation volume) | 350,000 x10^3 m3 (350 hm3) |

#### Ordinary Reserve (129,000 - 581,000 x10^3 m3)

The 20/80 split applies:
- **20%** of incoming rights allocated to ENDESA (electric)
- **80%** of incoming rights allocated to irrigators

Monthly electric modulation in reserve:
```
100% all months (no additional restriction)
```

#### Extraordinary Reserve (0 - 129,000 x10^3 m3)

Minimal flows, restricted reallocation.

### 3.3 Monthly Irrigation Percentages

| Month | Apr | May | Jun | Jul | Aug | Sep | Oct | Nov | Dec | Jan | Feb | Mar |
|-------|-----|-----|-----|-----|-----|-----|-----|-----|-----|-----|-----|-----|
| Standard % | 30 | 20 | 0 | 0 | 0 | 30 | 70 | 90 | 100 | 100 | 80 | 55 |
| Manual % | 30 | 20 | 0 | 0 | 0 | 30 | 70 | 90 | 100 | 65 | 60 | 50 |

### 3.4 Cumulative Volume Targets

Accumulated irrigation rights volume remaining to end season (10^3 m3):

| Month | Apr | May | Jun-Aug | Sep | Oct | Nov | Dec | Jan | Feb | Mar |
|-------|-----|-----|---------|-----|-----|-----|-----|-----|-----|-----|
| Volume | 262,656 | 107,136 | 0 | 3,012,768 | 2,857,248 | 2,482,272 | 2,015,712 | 1,480,032 | 944,352 | 557,280 |
| Days | 61 | 31 | 0 | 273 | 243 | 212 | 182 | 151 | 120 | 92 |

### 3.5 Resolucion 105 Minimum Flows (m3/s)

| Month | Apr | May | Jun | Jul | Aug | Sep | Oct | Nov | Dec | Jan | Feb | Mar |
|-------|-----|-----|-----|-----|-----|-----|-----|-----|-----|-----|-----|-----|
| Standard | 80 | 40 | 40 | 40 | 40 | 60 | 140 | 180 | 200 | 200 | 180 | 120 |
| Manual | 60 | 40 | 40 | 40 | 40 | 60 | 140 | 180 | 200 | 130 | 120 | 100 |

### 3.6 Armerillo Balance

Flag: "Descuenta caudales de derechos electricos del balance en Armerillo" = **T**

When TRUE: electrical rights flows are subtracted from the Armerillo balance,
leaving only irrigation-available water to satisfy downstream demands.

### 3.7 Withdrawal Points (7 districts)

| District | % | Slack |
|----------|-----|-------|
| RieCMNA | 12.66 | N |
| RieCMNB | 14.70 | N |
| RieMaitenes | 10.49 | N |
| RieMauleSur | 11.97 | N |
| RieMelado | 12.65 | N |
| RieSur123SCDZ | 34.27 | **Y** |
| RieMolinosOtros | 3.26 | N |

### 3.8 Bocatoma Canelon

Infrastructure intake: `BCanelon`, cost 10 $/m3/s.

### 3.9 Colbun Parameters

- **Cota Disponibilidad 425:** Volume = 1,047,900 x10^3 m3 (1,047.9 hm3)
- **Extraction Index at Cota 425:** 2

### 3.10 Cost Parameters

| Parameter | Conv Maule | Res 105 |
|-----------|-----------|---------|
| Valor Riego Servido | 1,100 | 1,100 |
| Costo Riego No Servido | 1,000 | 1,000 |
| Penalizadores Convenio | 1,500 / 1,000 | -- |
| Caudal Maule Minimo para embalsar | 250 m3/s | -- |

---

## 4. La Invernada (Winter Storage)

### 4.1 Operating Logic

The Invernada reservoir has conditional storage/discharge rules:

**When irrigation deficit > available inflow (Disembankment required):**
- `IQIDNH = Infinity` — must discharge
- `IQISDH = 0` — cannot store

**When deficit < inflow (Storage permitted):**
- `IQIDNH = 0` — no forced discharge
- `IQISDH = Infinity` — can store freely

### 4.2 Balance Equation

```
m_qidn_j + m_qisd_j + m_qninv_j - m_qhein_j - m_qhnein_j = QAflInvern
```

### 4.3 Economy Parameters

- **Initial economy volume:** 0.0 x10^3 m3
- **Uso en Reserva Ordinaria:** F (not used in ordinary reserve)
- **Son acumulables:** F (economies are not cumulative)
- **Costo Almacenamiento:** 0.1 $/m3

---

## 5. State Variables Summary

### Laja

| Variable | Description | Type |
|----------|-------------|------|
| IVDRF | Irrigation rights remaining volume | Volume (10^3 m3) |
| IVDEF | Electrical rights remaining volume | Volume |
| IVDMF | Mixed rights remaining volume | Volume |
| IVGAF | Anticipated discharge accumulated volume | Volume |
| IVESF | ENDESA economy final volume | Volume |
| IVERF | Reserve economy final volume | Volume |
| IVAPF | Alto Polcura economy final volume | Volume |

### Maule

| Variable | Description | Type |
|----------|-------------|------|
| IVMGEMF | Monthly electric rights accumulated | Volume |
| IVMGEAF | Annual electric rights accumulated | Volume |
| IVMGRTF | Seasonal irrigation rights accumulated | Volume |
| VolEcoInv | Invernada storage economy | Volume |
| VolCompEND | ENDESA compensation allocation | Volume |
| FPasoPorResOrd | Has passed through ordinary reserve | Boolean |
| FVieneDePorSup | Coming from upper portion | Boolean |
| FVieneDeResOrd | Coming from ordinary reserve | Boolean |

---

## 6. Data File Locations

| File | Content |
|------|---------|
| `support/plp_long_term/plplajam.dat.xz` | Laja agreement parameters |
| `support/plp_long_term/plpmaulen.dat.xz` | Maule agreement parameters |
| `support/plp_long_term/plpfilemb.dat.xz` | Seepage curves (ELTORO, CIPRESES, COLBUN) |
| `docs/analysis/irrigation_agreements/plp_implementation.md` | Full PLP documentation |
| `docs/analysis/irrigation_agreements/right_junctions_analysis.md` | Right junction catalog |
