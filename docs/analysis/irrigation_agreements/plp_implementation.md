# PLP Implementation of Irrigation Agreements (Convenios de Riego)

## Overview

PLP (Programación de Largo Plazo) implements two major irrigation agreements
as hard-coded Fortran modules that add LP constraints to the hydrothermal
scheduling problem. These are **not** generic reservoir constraints -- they
are specific to the Chilean electricity system's historical water-sharing
agreements between ENDESA (now Enel) and the Dirección de Obras Hidráulicas
(DOH).

Both agreements are controlled via environment variables in the PLP run
scripts and can be activated in different modes (0, 1, 2). The typical
production configuration uses mode 2 for both:

```bash
export PLP_CONVLAJA_MODE=2
export PLP_CONVMAULE_MODE=2
```

---

## 1. Convenio del Laja (Laja Irrigation Agreement)

### 1.1 Historical Background

The Laja agreement was signed in 1958 between ENDESA and DOH. It governs
water use from Laguna del Laja -- the largest reservoir in Chile (over
6 billion m3) and the only one with multi-annual regulation capacity.

The agreement balances hydroelectric generation (centrals El Toro, Abanico,
Antuco, Rucue, Quilleco -- collectively >1000 MW) against agricultural
irrigation demands in the Biobio region.

### 1.2 PLP Source Files

| File | Purpose |
|------|---------|
| `laja.fpp` | Preprocessor include: dimension constants and index definitions |
| `parlaja.f` | `PAR_LAJA` derived type: comprehensive parameter structure |
| `parlajac.f` | `PAR_LAJAC` derived type: convention-mode parameters |
| `parlajam.f` | `PAR_LAJAM` derived type: newer monthly-detail model parameters |
| `leelajam.f` | Data reader: parses `plplajam.csv` input file |
| `plp-laja0.f` | `Laja0`: initialization, data loading, default demand setup |
| `plp-laja1.f` | `Laja1`/`Laja1Dual`: per-stage constraint computation and LP bound modification |
| `plp-laja2.f` | `Laja2`: post-solve state update (economics tracking) |
| `genpdlaja.f` | LP constraint matrix assembly (original model) |
| `genpdlajam.f` | LP constraint matrix assembly (newer monthly model) |

### 1.3 PLP Input Files

- **`plplajam.csv`** (or `.dat`): Main configuration file read by `leelajam.f`
  containing reservoir identification, tributary centers, volume parameters,
  rights allocations, cost coefficients, monthly factors, and irrigation
  withdrawal points.

### 1.4 Operating Modes

PLP supports three modes controlled by `PLP_CONVLAJA_MODE`:

| Mode | Description |
|------|-------------|
| 0 | Disabled -- no Laja agreement constraints |
| 1 | Original model (`plp-laja0/1/2.f` + `genpdlaja.f`): dynamic per-simulation bound adjustment |
| 2 | Newer model (`parlajam.f` + `genpdlajam.f`): full LP embedding with volume-zone-based tiers |

### 1.5 Key Concepts

#### 1.5.1 Water Rights Categories

The Laja agreement partitions extraction into distinct categories, each with
its own annual/monthly limits:

| Category | Variable | Description |
|----------|----------|-------------|
| Irrigation rights (Derechos de Riego) | `qdr` / `IQDRH` | Water for agricultural irrigation |
| Electrical rights (Derechos Electricos) | `qde` / `IQDEH` | Water for hydroelectric generation |
| Mixed rights (Derechos Mixtos) | `qdm` / `IQDMH` | Shared allocation |
| Anticipated discharge (Gasto Anticipado) | `qga` / `IQGAH` | Early withdrawal against future rights |

The total turbined flow is partitioned:
```
qgt = qdr + qde + qdm + qga
```

#### 1.5.2 Reservoir Volume Zones (Colchones)

The reservoir volume is divided into operational zones that determine
extraction rights:

```
                        VolMax
    ────────────────────────────
    │     Upper zone            │  Full rights
    ├── VolColchon[upper] ──────┤
    │     Middle zone           │  Reduced rights (factor applied)
    ├── VolColchon[lower] ──────┤
    │     Lower volume zone     │  Minimal extraction
    ├── VolMuerto ──────────────┤
    │     Dead volume           │  No extraction
    ────────────────────────────
```

When useful volume falls into the lower volume zone (`VolUtil <= VolUtilColInf`):
- Economics-based generation is disabled (`qgesh = 0`)
- Reserve economics only (`qgerh` unbounded)
- Annual rights reduced from 57 to 47 m3/s
- No new economy accumulation allowed

#### 1.5.3 The 50cm Rebalse Rule

When reservoir volume exceeds a threshold (`VolAcumA50cmReb`):
- Unrestricted spillage is permitted
- Accounting is capped at 47 m3/s for annual rights
- The `qg50` variable is freed from its upper bound

#### 1.5.4 ENDESA Economics Tracking

The agreement tracks accumulated "economies" -- unused extraction rights
that can be carried forward:

| Variable | Description |
|----------|-------------|
| `vesf` / `IVESF` | ENDESA economy final volume |
| `verf` / `IVERF` | Reserve economy final volume |
| `vapf` / `IVAPF` | Alto Polcura economy final volume |

These are state variables that link consecutive stages in the SDDP
decomposition.

#### 1.5.5 Default Monthly Irrigation Demands (m3/s)

Three demand curves define irrigation requirements by month:

| Month | NuevoRiego | RegTucapel | RegAbanico |
|-------|------------|------------|------------|
| Apr   | 13.00      | 18.00      | 9.40       |
| May   | 0.00       | 0.00       | 0.00       |
| Jun   | 0.00       | 0.00       | 0.00       |
| Jul   | 0.00       | 0.00       | 0.00       |
| Aug   | 0.00       | 0.00       | 0.00       |
| Sep   | 19.50      | 27.00      | 14.10      |
| Oct   | 42.25      | 58.50      | 30.55      |
| Nov   | 55.25      | 76.50      | 39.95      |
| Dec   | 65.00      | 90.00      | 47.00      |
| Jan   | 65.00      | 90.00      | 47.00      |
| Feb   | 52.00      | 72.00      | 37.60      |
| Mar   | 32.50      | 45.00      | 23.50      |

Zero demands during May-August reflect the winter non-irrigation season.

#### 1.5.6 Irrigation Allocation Hierarchy

The agreement establishes a priority system:
1. **Anelo-Reloco** receives minimum guaranteed allocation first
2. **Nuevo Riego** (new irrigators) receives any excess
3. Within the LP, the constraint `qrs = 0.8*qpr + 0.2*qnr` distributes
   supply between historical (80%) and new settlers (20%)

#### 1.5.7 Filtration Constraint

Laguna del Laja has significant natural seepage (filtration):
```
VolFiltLaja = MIN(VoluLaja + VolAflLaja, CauFiltLaja * Cau2Vol(IEtapa))
```

This volume is subtracted from available water before computing extraction
limits, since it represents uncontrollable water loss.

### 1.6 LP Constraint Structure (Mode 2 -- `genpdlajam.f`)

#### Block-Level Constraints

Per block `j` within each stage:
```
-qgt_j + qdr_j + qde_j + qdm_j + qga_j = 0    (flow partition)
```

Block flows are aggregated to hourly rates:
```
qdXh += qdX_j * (blodur_j / etadur)    for X in {r, e, m, a}
```

#### Stage-Level Constraints

Rights accumulation (annual tracking):
```
IVDRF = Previous_IVDRF - (etadur/ScaleVol) * IQDRH    (irrigation rights spent)
IVDEF = Previous_IVDEF - (etadur/ScaleVol) * IQDEH    (electrical rights spent)
IVDMF = Previous_IVDMF - (etadur/ScaleVol) * IQDMH    (mixed rights spent)
IVGAF = Previous_IVGAF + (etadur/ScaleVol) * IQGAH    (anticipated discharge accumulated)
```

Irrigation balance:
```
IQRS - IQPR - IQNR - IQER - IQSR = 0
```

Flow identity:
```
IQLAJA - IQGTH = 0    (total Laja discharge = total generation)
```

#### Objective Function Penalties

```
CRiegoNS * qdr      : irrigation non-served cost
CVertEcon * vrb      : spillage of stored economics
CVolUtilNeg * vun    : lower-zone volume shortfall penalty
CSubEconAnu * qen    : unmet annual economics subsidy
CostoVAP * vapf      : Alto Polcura storage value
```

### 1.7 Dynamic Bound Modification (`FijaLajaMBloA`)

At each stage-simulation pair, the subroutine:
1. Computes the active volume zone layer from current useful volume
2. Applies monthly factors to extraction limits for each rights category
3. Sets irrigation demands based on historical filtration and inter-basin flows
4. Modifies LP bounds via `ModifUpp`, `ModifLow`, and `ModifBrdRhs`
5. Resets annual/monthly accumulators at year/month boundaries

### 1.8 Hydrological Nodes

The Laja model tracks these tributary inflows:
- `AflAbanico`, `AflAntuco`, `AflLaja`, `AflRucue`, `AflTucapel`,
  `AflCaptAltoPolc`

And these operational nodes:
- `CenAbanico`, `CenElToro`, `CenLaja`, `CenTucapel` (generation)
- `RieZaCo`, `RieLaDi`, `RieCCCE` (irrigation districts)
- `FiltLaja` (filtration), `VertLaja` (spillage), `VolLaja` (volume)

---

## 2. Convenio del Maule (Maule Irrigation Agreement)

### 2.1 Historical Background

The Maule agreement was signed in 1947 between ENDESA and DOH. It governs
water allocation from the Maule river system between hydroelectric generation
(centrals Colbun, Machicura, Cipreses) and irrigation, with the Colbun
reservoir playing a central role in storing irrigation rights for farmers.

### 2.2 PLP Source Files

| File | Purpose |
|------|---------|
| `maule.fpp` | Preprocessor include: dimension constants and index definitions |
| `parmaule.f` | `PAR_MAULE` derived type: comprehensive parameter structure |
| `parmaulec.f` | `PAR_MAULEC` derived type: convention-mode parameters |
| `plp-maule0.f` | `Maule0`: initialization, config file parsing, array allocation |
| `plp-maule1.f` | `Maule1`/`Maule1Dual`/`Maule1a`: per-stage constraints and verification |
| `plp-maule2.f` | `Maule2`: post-solve state update (ENDESA/irrigator economics) |
| `genpdmaule.f` | LP constraint matrix assembly and `FijaMaule` bound modification |

### 2.3 PLP Input Files

The Maule module reads its configuration from a file specified by `NArcMaule`,
which contains keyword-value pairs defining:
- Connection names for generation, extraction, and control nodes
- Volume parameters (reserves, ecological requirements, compensation)
- Monthly flow tables (`CauConMau`, `CauRes105`)
- Cost coefficients and operational flags

### 2.4 Operating Modes

| Mode | Description |
|------|-------------|
| 0 | Disabled -- no Maule agreement constraints |
| 1 | Original per-simulation bound adjustment model |
| 2 | Full LP-embedded model with constraint matrix assembly |

### 2.5 Key Concepts

#### 2.5.1 Three-Zone Reservoir Operation

The Maule reservoir operates in three distinct zones:

```
                        VolMax
    ────────────────────────────
    │     Normal Zone           │  Full electric + irrigation rights
    ├── VResOrdMax+VResExtMax ──┤
    │     Ordinary Reserve      │  Reduced electric, reserve irrigation
    ├── VReservaExtraord ───────┤
    │     Extraordinary Reserve │  Minimal flows, restricted reallocation
    ────────────────────────────
```

| Zone | Electric Rights | Irrigation Rights | Compensation |
|------|-----------------|-------------------|--------------|
| Normal | `GastoElecDiaMax` allowed | Full allocation | Fixed annually |
| Ordinary Reserve | Reduced by `PGastoElecDiaMaxMenRes` | Reserve-dependent | Capped at `VCompElecMax` |
| Extraordinary Reserve | Minimal flows | Reserve-only | Restricted |

#### 2.5.2 Water Rights Partition

The Maule agreement uses a different categorization than Laja:

| Category | Block Variable | Description |
|----------|----------------|-------------|
| Normal Electric | `IQMNE` / `m_qmne` | Generation under normal regime |
| Normal Irrigation | `IQMNR` / `m_qmnr` | Irrigation under normal regime |
| Ordinary Electric | `IQMOE` / `m_qmoe` | Generation from ordinary reserve |
| Ordinary Irrigation | `IQMOR` / `m_qmor` | Irrigation from ordinary reserve |
| ENDESA Compensation | `IQMCE` / `m_qmce` | Compensation flows |
| Winter Economy | `IQMEI` | Winter season economic flows |
| Winter Deficit | `IQIDN` | La Invernada deficit level flow |
| Winter No-Deficit | `IQISD` | La Invernada flow without deficit |
| Irrigation Delivery | `IQTER` | Total irrigation delivery |

#### 2.5.3 Multi-Reservoir System

Unlike Laja (single reservoir), Maule involves three interconnected
reservoirs:

- **Lago Maule** (primary): The main regulation reservoir
- **La Invernada** (winter storage): Seasonal storage with special rules
- **Colbun** (downstream): Generation and irrigation delivery point

Each has its own volume, inflow, and spillage tracking variables:
```
IIVolMaule, IIVolInve, IIVolColbun   (volumes)
IIAflMaule, IIAflInve, IIAflColbun   (inflows)
IIVertMaule, IIVertCip, IIVertColbun  (spillages)
```

#### 2.5.4 Extraction Points

The Maule agreement tracks four distinct extraction types:

| Variable | Description |
|----------|-------------|
| `IIExtMauEND` | ENDESA extraction from Maule |
| `IIExtMauRie` | Irrigation extraction from Maule |
| `IIExtInvEND` | ENDESA extraction from La Invernada |
| `IIExtInvRie` | Irrigation extraction from La Invernada |

#### 2.5.5 Irrigation Districts

The model tracks five irrigation districts:
- `RieCMNA` -- Canal Maule Norte A
- `RieCMNB` -- Canal Maule Norte B
- `RieCMel` -- Canal Melado
- `RieS123` -- Sectors 1-2-3
- `RieOReg` -- Other regulations

Plus an optional irrigation allocation (`RieOpcionalMaule`).

#### 2.5.6 Default Monthly Flows (m3/s)

| Month | ConflMaule | Res105 |
|-------|------------|--------|
| Apr   | 60         | 80     |
| May   | 20         | 40     |
| Jun   | 0          | 40     |
| Jul   | 0          | 40     |
| Aug   | 0          | 40     |
| Sep   | 40         | 60     |
| Oct   | 100        | 140    |
| Nov   | 182        | 180    |
| Dec   | 200        | 200    |
| Jan   | 200        | 200    |
| Feb   | 160        | 180    |
| Mar   | 110        | 120    |

Note: `Res105` refers to "Resolucion 105" -- a complementary regulatory
resolution that defines additional flow requirements.

#### 2.5.7 La Invernada Winter Storage Logic

The Invernada reservoir has special storage/disembankment rules:
- `FInvernDesemb`: When irrigation deficit exceeds available inflow,
  disembankment is required (no new storage allowed)
- When storage is allowed: `IQIDNH = 0`, `IQISDH = Infinity`
- When disembankment required: `IQIDNH = Infinity`, `IQISDH = 0`

#### 2.5.8 Seasonal Logic

- `MesRiegoIni` / `MesRiegoFin`: Define the irrigation season
- `PRiegoMaule(12)`: Monthly irrigation percentages
- `QRiego105(12)`: Monthly Resolucion 105 flows
- `EnRiego` flag sets irrigation flow upper bounds to zero outside season

#### 2.5.9 Logical State Flags

Three boolean flags track the reservoir's operational trajectory:
- `FPasoPorResOrd`: Has passed through ordinary reserve
- `FVieneDePorSup`: Coming from upper portion (normal zone)
- `FVieneDeResOrd`: Coming from ordinary reserve

These flags are state variables carried between stages and affect
how reserve rights are allocated.

### 2.6 LP Constraint Structure

#### Block-Level Constraints

Flow partition:
```
-qg_i_j + m_qmne_j + m_qmnr_j + m_qmoe_j + m_qmor_j + m_qmce_j = 0
```

Winter lagoon balance:
```
m_qidn_j + m_qisd_j + m_qninv_j - m_qhein_j - m_qhnein_j = QAflInvern
```

#### Stage-Level Constraints

Volume accumulation:
```
IVMGEMF = Previous_IVMGEMF + (etadur/ScaleVol) * IQMNEH    (monthly electric)
IVMGEAF = Previous_IVMGEAF + IVMGEMF                        (annual electric)
IVMGRTF = Previous_IVMGRTF + (etadur/ScaleVol) * IQMNRH    (seasonal irrigation)
```

Delivery constraint (inequality):
```
-IQDRAH + IQDRMH + IQINVH <= 0    (delivery <= available)
```

Irrigation supply from Resolucion 105:
```
IQA105 = QRiego105(month) * factor
```

#### Objective Function

```
-FCau * ValorRiego105 * QA105           (irrigation benefit from Res105)
-FCau * ValorRiegoMaule * IQTERH        (irrigation benefit from Maule)
+FCau * CostoRiegoNSMaule * IQDRAH      (unmet Maule irrigation penalty)
+FCau * CostoEmbalsar * IQHINV          (storage cost)
+FCau * CostoNoEmbalsar * IQHNEIN       (non-storage penalty)
+CostoCanelon * BloDur/FPhi             (canal infrastructure cost)
```

### 2.7 Post-Solve State Update (`Maule2`)

After each LP solve, `Maule2` updates:
- Reservoir volumes (`VolMau`, `VolInv`, `VolColb`)
- Economic savings (`VolEcoInv`, `VolCompEND`)
- Reserve allocations (`VolResMauEND`, `VolResMauRie`)
- At January/June boundaries: resets compensation quotas, redistributes
  ordinary reserves (20% ENDESA / 80% irrigators)

---

## 3. Comparison: Laja vs Maule

| Feature | Laja | Maule |
|---------|------|-------|
| Year signed | 1958 | 1947 |
| Primary reservoir | Laguna del Laja (6+ billion m3) | Lago Maule |
| Regulation | Multi-annual | Annual/seasonal |
| Number of reservoirs | 1 | 3 (Maule, Invernada, Colbun) |
| Reservoir zones | 2 (upper/lower volume zone) | 3 (normal/ordinary/extraordinary) |
| Rights categories | 4 (irrigation, electric, mixed, anticipated) | 5+ (normal/reserve for each + compensation) |
| Irrigation demand curves | 3 (NuevoRiego, RegTucapel, RegAbanico) | 5 districts + Res105 |
| Irrigation season | Sep-Apr (zero May-Aug) | Configurable (MesRiegoIni/Fin) |
| Economy tracking | ENDESA, reserve, Alto Polcura | ENDESA compensation, winter storage |
| State variables | Volume, economics (3 types) | Volume (3 reservoirs), 3 boolean flags, reserve allocations |
| Special features | 50cm rebalse rule, Alto Polcura option | La Invernada storage/disembankment logic |
| Filtration modeling | Explicit (CauFiltLaja) | Via `QFiltInvern` |

---

## 4. gtopt Current Status

### 4.1 Existing Support

**gtopt does not currently implement irrigation agreement constraints.**

The following evidence was found in the gtopt codebase:

1. **SDDP state variable comment** (`include/gtopt/sddp_method.hpp`, line 12):
   The SDDP method documentation mentions "future irrigation rights" as
   state variables linking consecutive phases, but no implementation of
   irrigation-specific state variables exists in the code.

2. **plp2gtopt converter**: The converter (`scripts/plp2gtopt/`) does not
   have a parser for Laja or Maule agreement input files. There is no
   `laja_parser.py`, `maule_parser.py`, or equivalent.

3. **Ralco parser**: The `RalcoParser` (`scripts/plp2gtopt/ralco_parser.py`)
   handles `plpralco.dat` which contains reservoir drawdown limits -- a
   related but different constraint type.

4. **Run scripts**: The support scripts (`support/plp_2_years/run_plp_cen65_*.sh`)
   set `PLP_CONVLAJA_MODE=2` and `PLP_CONVMAULE_MODE=2`, confirming these
   agreements are active in production PLP runs.

5. **Mathematical formulation**: Reference [5] in the formulation document
   cites the paper "Optimizing Hydrothermal Scheduling with Non-Convex
   Irrigation Constraints" (Pereira-Bonvallet et al., 2016), acknowledging
   the importance of these constraints.

### 4.2 What Would Be Needed for gtopt

To implement irrigation agreements in gtopt, the following components
would be needed:

1. **New data model elements**: A generic "irrigation agreement" or
   "water rights constraint" type that can express:
   - Multi-category extraction partitioning
   - Volume-dependent extraction limits (volume zone logic)
   - Seasonal demand curves
   - Annual/monthly rights accumulation and reset
   - Inter-stage state variables for economics tracking

2. **plp2gtopt parser**: New parsers for `plplajam.csv`/`.dat` and the
   Maule configuration file.

3. **LP assembly**: Constraint generation code in C++ mirroring the
   Fortran `genpdlaja.f` / `genpdmaule.f` logic.

4. **SDDP integration**: The economics/rights accumulation state variables
   must be included in the SDDP forward/backward pass.

---

## 5. PLP Environment Variables

| Variable | Values | Effect |
|----------|--------|--------|
| `PLP_CONVLAJA_MODE` | 0, 1, 2 | Laja agreement mode (0=off, 1=original, 2=full LP) |
| `PLP_CONVMAULE_MODE` | 0, 1, 2 | Maule agreement mode (0=off, 1=original, 2=full LP) |
| `PLP_RESTRALCO_MODE` | si/no | Ralco drawdown restriction |

---

## 6. References

### Academic

- Pereira-Bonvallet, E., Puschel-Lovengreen, S., Matus, M., and Moreno, R.
  (2016). "Optimizing Hydrothermal Scheduling with Non-Convex Irrigation
  Constraints: Case on the Chilean Electricity System." *Energy Procedia*,
  87, pp. 132-138.
  DOI: [10.1016/j.egypro.2015.12.342](https://doi.org/10.1016/j.egypro.2015.12.342)

- Thesis: "Efecto del convenio de riego del sistema hidroelectrico Laja
  sobre la programacion de largo plazo del sistema interconectado central
  de Chile." Universidad de Chile.
  URL: http://repositorio.uchile.cl/handle/2250/139279

- Thesis: "Modelacion para el analisis de la interferencia operacional
  entre hidroelectricidad y riego en la cuenca del Maule, Chile."
  Universidad de Chile.
  URL: https://repositorio.uchile.cl/handle/2250/196765

### PLP Source Code

All Fortran source files referenced above are in:
`https://github.com/marcelomatus/plp_storage/tree/main/CEN65/src`

### Local Documents

The following documents are available in this directory:
- `Actualizacion_Convenio_Laja_PLP.pdf` -- PLP update for Laja agreement
- `Acuerdo_Lago_Laja_2017.pdf` -- 2017 Lago Laja accord
- `CNR_Centrales_Hidroelectricas_Obras_Riego.pdf` -- CNR report on hydro
  plants associated with irrigation works
- `Convenio_Endesa_Riego_Maule.pdf` -- ENDESA-Maule irrigation agreement
- `Convenio_Regulacion_Rio_Maule_1947.pdf` -- Original 1947 regulation
- `Informe_Comision_Investigadora_Maule.pdf` -- Investigation commission
  report on Maule
- `Modelo_PLP_Coordinacion_Hidrotermica.pdf` -- PLP hydrothermal
  coordination model documentation
- `Tesis_Tradeoffs_Hidroelectricidad_Riego.pdf` -- Thesis on hydro-irrigation
  tradeoffs
- `resolucion_dga_105_1983.pdf` -- DGA Resolution 105 (1983)
- `resolucion_dga_105_1983_jvrm.pdf` -- DGA Resolution 105 (annotated)
