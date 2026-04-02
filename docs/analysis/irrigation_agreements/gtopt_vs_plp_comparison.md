# Irrigation Agreements in gtopt: Description and PLP Comparison

## 1. Overview

gtopt implements Chilean irrigation agreements (Convenios de Riego) through
three generic, data-driven entities that form a **rights-domain** layer
parallel to the physical hydro topology:

| gtopt Entity | Purpose | PLP Equivalent |
|---|---|---|
| **RightJunction** | Flow balance node in rights domain | Armerillo balance, Laja partition |
| **FlowRight** | Flow-based right (m3/s) per block | IQDR, IQDE, IQDM, IQGA, IQMNE, etc. |
| **VolumeRight** | Volume-based right (hm3) across stages | IVDRF, IVDEF, IVDMF, IVGAF, etc. |
| **UserConstraint** | Proportional allocation, identities | Percentage splits, IQLAJA identity |

The key design principle: rights entities are **not part of the physical
topology**. They create their own LP variables and constraints, and couple
to physical elements through the `consumptive` flag when needed.

### 1.1 Historical Context

Both agreements predate the modern Chilean electricity system:

| Feature | Laja Agreement | Maule Agreement |
|---|---|---|
| Year signed | 1958 | 1947 |
| Parties | DOH + ENDESA | DOH + ENDESA |
| Primary reservoir | Laguna del Laja (~6,000 hm3) | Lago Maule + Colbun + Invernada |
| Regulation | Multi-annual (unique in Chile) | Annual / seasonal |
| Irrigation area | ~117,000 ha (Biobio region) | ~90,000+ ha (Maule region) |
| Hydroelectric capacity | ~1,150 MW (El Toro + 4 cascade) | ~800 MW (Colbun + Machicura + Cipreses) |
| Supreme Court priority | Shared (contested) | Irrigation priority (confirmed 2021) |
| 2017 update | Permanent convention signed | Resolucion 105 still in force |

### 1.2 PLP Source Code Structure

PLP (Programacion de Largo Plazo) implements each agreement as a monolithic
Fortran module.  Authoritative source:
`https://github.com/marcelomatus/plp_storage/tree/main/CEN65/src`

**Laja files:**

| File | Purpose |
|------|---------|
| `laja.fpp` | Preprocessor include: dimension constants and index definitions |
| `parlajam.f` | `PAR_LAJAM` derived type: comprehensive parameter structure |
| `leelajam.f` | Data reader: parses `plplajam.dat` input file |
| `plp-laja0.f` | Initialization, data loading, default demand setup |
| `plp-laja1.f` | Per-stage constraint computation, LP bound modification |
| `plp-laja2.f` | Post-solve state update (economics tracking) |
| `genpdlajam.f` | LP constraint matrix assembly (variable/constraint indices) |

**Maule files:**

| File | Purpose |
|------|---------|
| `maule.fpp` | Preprocessor include: dimension constants and index definitions |
| `parmaule.f` | `PAR_MAULE` derived type: comprehensive parameter structure |
| `plp-maule0.f` | Initialization, config file parsing |
| `plp-maule1.f` | Per-stage constraints and verification |
| `plp-maule2.f` | Post-solve state update (ENDESA/irrigator economics) |
| `genpdmaule.f` | LP constraint matrix assembly, `FijaMaule` bound modification |

**Operating modes** (both agreements, via environment variables):

| Mode | Description |
|------|-------------|
| 0 | Disabled — no agreement constraints |
| 1 | Original per-simulation bound adjustment model |
| 2 | Full LP-embedded model with constraint matrix assembly |

Production runs use mode 2 for both:
```bash
export PLP_CONVLAJA_MODE=2
export PLP_CONVMAULE_MODE=2
```

### 1.3 plp2gtopt Converter

The `scripts/plp2gtopt/` directory contains Python parsers and writers that
convert PLP configuration files to gtopt JSON:

| File | Purpose |
|------|---------|
| `laja_parser.py` | Parses `plplajam.dat` — all 4 volume zones, costs, districts |
| `laja_writer.py` | Converts parsed config → gtopt JSON entities (UID range 2000+) |
| `maule_parser.py` | Parses `plpmaulen.dat` — 3-zone system, districts, Res105 |
| `maule_writer.py` | Converts parsed config → gtopt JSON entities (UID range 1000+) |
| `tests/test_laja.py` | 68 tests: parser, writer, schedule helpers, minimal configs |
| `tests/test_maule.py` | 56 tests: parser, writer, schedule helpers, minimal configs |

---

### 1.4 System-Level Architecture Diagram

```
  ┌─────────────────────────────────────────────────────────────────┐
  │                    PHYSICAL DOMAIN (hydro topology)              │
  │                                                                 │
  │   ┌──────────┐     ┌──────────┐     ┌──────────┐               │
  │   │ Reservoir │────►│ Waterway │────►│Generator │               │
  │   │ (Laja /  │     │ (tunnel) │     │(El Toro/ │               │
  │   │  Colbun) │     │          │     │ Colbun)  │               │
  │   └────┬─────┘     └──────────┘     └──────────┘               │
  │        │ volume                                                 │
  │        ▼                                                        │
  │   ┌──────────┐                                                  │
  │   │ Seepage  │  (ReservoirSeepage: volume-dependent filtration) │
  │   └──────────┘                                                  │
  └────────┼────────────────────────────────────────────────────────┘
           │ volume drives bound_rule
           ▼
  ┌─────────────────────────────────────────────────────────────────┐
  │                    RIGHTS DOMAIN (water rights accounting)      │
  │                                                                 │
  │   ┌──────────────┐         ┌──────────────┐                    │
  │   │ RightJunction│◄────────│  FlowRight   │                    │
  │   │ (partition / │  dir=+1 │ (total gen)  │                    │
  │   │  armerillo)  │         └──────────────┘                    │
  │   │              │                                              │
  │   │  balance row │◄────────┬──────────────┬──────────────┐     │
  │   │  per block   │  dir=-1 │  FlowRight   │  FlowRight   │ ... │
  │   └──────────────┘         │  (irr_rights)│  (elec_rights)│    │
  │                            │  bound_rule  │  bound_rule   │    │
  │                            └──────┬───────┘──────┬────────┘    │
  │                                   │              │              │
  │                                   ▼              ▼              │
  │                            ┌──────────────┐──────────────┐     │
  │                            │ VolumeRight  │ VolumeRight  │     │
  │                            │ (vol_irr)    │ (vol_elec)   │     │
  │                            │ state_var    │ state_var    │     │
  │                            │ reset: april │ reset: april │     │
  │                            └──────────────┘──────────────┘     │
  │                                                                 │
  │   ┌──────────────┐         ┌──────────────┐                    │
  │   │UserConstraint│────────►│  FlowRight   │                    │
  │   │ (pct split)  │  refs   │  (districts) │                    │
  │   │ flow_right   │         │              │─ ─ ─► physical    │
  │   │ ('a')<=20%   │         │              │  Junction balance │
  │   └──────────────┘         └──────────────┘                    │
  └─────────────────────────────────────────────────────────────────┘
```

### 1.5 Laja Flow Partition Diagram

```
                     ┌─────────────────────┐
                     │   RightJunction      │
                     │   "laja_partition"   │
                     │   drain = false      │
                     │                     │
        direction=+1 │   balance row:      │ direction=-1
     ───────────────►│   +qgt - qdr - qde  │◄──────────────────
                     │   - qdm - qga = 0   │
                     └─────────┬───────────┘
                               │
              ┌────────────────┼────────────────┐
              │                │                │
   ┌──────────▼──┐  ┌─────────▼───┐  ┌─────────▼───┐
   │ FlowRight   │  │ FlowRight   │  │ FlowRight   │  ...
   │ total_gen   │  │ irr_rights  │  │ elec_rights │
   │ dir=+1      │  │ dir=-1      │  │ dir=-1      │
   │ fmax=5582   │  │ bound_rule  │  │ bound_rule  │
   │             │  │ fail=1100   │  │ fail=1150   │
   │  ┌───────┐  │  │  ┌───────┐  │  │  ┌───────┐  │
   │  │qeh col│  │  │  │qeh col│  │  │  │qeh col│  │
   │  └───┬───┘  │  │  └───┬───┘  │  │  └───┬───┘  │
   └──────┼──────┘  └──────┼──────┘  └──────┼──────┘
          │                │                │
          ▼                ▼                ▼
   ┌──────────────────────────────────────────────┐
   │           VolumeRight entities               │
   │  vol_irr    vol_elec    vol_mixed   vol_antic│
   │  emax=5000  emax=1200   emax=30    emax=5000│
   │  reset=apr  reset=apr   reset=apr  reset=apr│
   │  state_var  state_var   state_var  state_var│
   └──────────────────────────────────────────────┘
```

### 1.6 Maule Armerillo Balance Diagram

```
   Supplies (+1)                    Withdrawals (-1)
   ─────────────                    ──────────────────

   Q_intermediate ──►┌────────────────────┐◄── maule_elec_normal
   Q_invernada   ──►│   RightJunction    │◄── maule_irr_normal
   Q_maule       ──►│   "armerillo"      │◄── maule_elec_ordinary
                     │   drain = true     │◄── maule_irr_ordinary
                     │                    │◄── maule_compensation
                     │  balance row:      │
                     │  +supplies         │    ┌──────────────────┐
                     │  -withdrawals      │    │  UserConstraint  │
                     │  -drain >= 0       │    │  20/80 split:    │
                     └────────┬───────────┘    │  elec<=20%*total │
                              │                │  irr <=80%*total │
                              ▼                └──────────────────┘
                     ┌────────────────────┐
                     │  Irrigation        │    ┌──────────────────┐
                     │  Districts (7)     │    │  UserConstraint  │
                     │                    │◄───│  dist allocation │
                     │                    │    │  RieCMNA = 12.66%│
                     │  RieCMNA  12.66%   │    │  RieCMNB = 14.70%│
                     │  RieCMNB  14.70%   │    │  ...             │
                     │  ...               │    │  RieMelado<=12.65│
                     │  Total = 100%      │    │  (slack for some)│
                     └────────────────────┘    └──────────────────┘
```

---

## 2. gtopt Irrigation Entities

### 2.1 RightJunction

A balance node in the rights accounting graph. Creates one balance row per
block:

```
sum_{fr : direction=+1} flow(fr,b) - sum_{fr : direction=-1} flow(fr,b)
    [- drain(b)] >= 0
```

**Fields:**
- `junction` (OptSingleId): Reference to physical Junction (documentation only)
- `drain` (OptBool, default: true): When true, adds a free drain variable
  (excess supply allowed, `>=` constraint). When false, exact equality.

**LP columns:** `drain` (one per block, only if drain=true)
**LP rows:** `balance` (one per block)

### 2.2 FlowRight

A flow-based water right. Creates flow columns per block with two operating
modes:

- **Fixed mode** (default): `lowb = uppb = discharge` — fixed extraction
- **Variable mode**: When `fmax > 0` and `discharge == 0`, bounds are
  `[0, fmax]` — optimizer decides the flow

**Fields:**
- `discharge` (STBRealFieldSched): Required flow [m3/s], indexed [scenario][stage][block]
- `fmax` (OptTBRealFieldSched): Maximum flow for variable mode [m3/s]
- `right_junction` (OptSingleId): Links to RightJunction balance row
- `direction` (OptInt, default: -1): Sign in RightJunction balance (+1 supply, -1 withdrawal)
- `junction` (OptSingleId): Reference to physical Junction
- `use_average` (OptBool, default: false): Creates stage-average hourly flow variable `qeh`
- `fail_cost` (OptReal): Penalty for unmet demand [$/m3/s*h]

**LP columns per block:**
- `flow` — extraction flow
- `fail` — deficit variable (only if fail_cost > 0)

**LP columns per stage (if use_average=true):**
- `qeh` — stage-average hourly flow: `qeh = sum_b [flow(b) * dur(b) / dur_stage]`

**LP rows per stage (if use_average=true):**
- `qavg` — averaging constraint: `qeh - sum_b [dur_ratio(b) * flow(b)] = 0`

**Coupling:**
1. **RightJunction:** `lp.row_at(rj_balance[b])[flow_col[b]] = direction`
2. **Physical Junction:** `lp.row_at(junction_balance[b])[flow_col[b]] = -1.0`

### 2.3 VolumeRight

A volume-based right modeled as a "dummy reservoir" (StorageLP pattern).
Tracks accumulated volume across stages with state-variable coupling for SDDP.

**Fields:**
- `reservoir` (OptSingleId): Reference to physical source Reservoir
- `right_reservoir` (OptSingleId): Reference to parent VolumeRight for volume balance
- `direction` (OptInt, default: -1): Sign in parent VolumeRight balance
- `emin/emax` (OptTRealFieldSched): Volume bounds [hm3]
- `eini` (OptReal): Initial volume [hm3]
- `efin` (OptReal): Required final volume [hm3]
- `demand` (OptTRealFieldSched): Required delivery per stage [hm3]
- `fmax` (OptTBRealFieldSched): Maximum extraction rate [m3/s]
- `fail_cost` (OptReal): Penalty for unmet volume demand [$/hm3]
- `use_state_variable` (OptBool, default: true): SDDP state coupling
- `flow_conversion_rate` (OptReal, default: 0.0036 hm3/(m3/s*h))
- `energy_scale` (OptReal, default: 1.0)
- `reset_month` (OptMonthType): When set, forces `eini=0` at each stage
  matching this month — automatic seasonal/annual reset of accumulated volume

**LP columns per block:**
- `finp` — input flow (extraction rate)
- Energy state variables (from StorageLP)

**LP columns per stage:**
- `fail` — demand deficit (only if demand > 0)

**LP rows per block:**
- Energy balance row (from StorageLP)

**LP rows per stage:**
- Demand satisfaction: `sum_b [fcr * dur(b) * finp(b)] + fail >= demand / energy_scale`

**Coupling:**
1. **RightReservoir:** `lp.row_at(parent.energy_rows[b])[finp[b]] = fcr * dur * dir / escale`
2. **Consumptive:** `lp.row_at(reservoir.energy_rows[b])[finp[b]] = -fcr * dur / r_escale`

### 2.4 RightBoundRule (Volume-Dependent Dynamic Bounds)

An optional `bound_rule` field on FlowRight and VolumeRight defines a
piecewise-linear function that maps a referenced reservoir's volume to
the maximum allowed flow/extraction bound.

**Fields (`RightBoundRule`):**
- `reservoir` (SingleId): Source reservoir whose volume drives the bound
- `segments` (vector of `RightBoundSegment`): Piecewise-linear breakpoints
  - `volume` (Real): Breakpoint — segment active when V >= volume
  - `slope` (Real): d(bound)/d(V)
  - `constant` (Real): Y-intercept at V=0 for this segment
- `cap` (OptReal): Maximum cap on the computed bound
- `floor` (OptReal): Minimum floor (default: 0)

**Evaluation:** `bound = constant_i + slope_i * V` for the active segment
(highest breakpoint <= V), clamped to [floor, cap].

**LP integration:**
- During `add_to_lp`: initial bound evaluated from reservoir `eini`,
  applied as `uppb = min(fmax, rule_value)` on flow/finp columns
- During `update_lp` (auto-dispatched via `HasUpdateLP` concept):
  reservoir volume retrieved via `physical_eini/physical_efin`,
  piecewise function re-evaluated, column bounds updated via
  `set_col_upp` when the bound changes
- No orchestrator entity needed — each right independently evaluates
  its own bound from its referenced reservoir

### 2.5 UserConstraint (Proportional Allocation)

gtopt's `UserConstraint` entity expresses proportional flow splits and
identity constraints using an AMPL-inspired expression syntax.

**Expression syntax:**
```
flow_right('name').flow  — FlowRight extraction flow per block
flow_right('name').fail  — FlowRight deficit variable per block
volume_right('name').flow — VolumeRight input flow per block
generator('name').generation — Generator output per block
reservoir('name').volume — Reservoir volume
= / <= / >=              — Constraint direction
sum(flow_right('a','b').flow) — Sum over named elements
2.5 * flow_right('x').flow — Coefficient multiplication
```

---

## 3. PLP Laja Agreement: Complete Row and Column Mapping

### 3.1 LP Columns (Variables) — `genpdlajam.f`

The Laja agreement in mode 2 creates the following LP variables.  Block-level
variables are created per block `j`; stage-level variables once per stage.

#### 3.1.1 Block-Level Flow Variables (per block j)

| # | PLP Variable | Description | Bounds | gtopt Entity | gtopt Name |
|---|---|---|---|---|---|
| 1 | `qgt_j` | Total turbine flow | [0, VolMax] | FlowRight | `laja_total_gen` |
| 2 | `qdr_j` | Irrigation rights flow | [0, fmax_irr×usage] | FlowRight | `laja_irr_rights` |
| 3 | `qde_j` | Electrical rights flow | [0, fmax_elec×usage] | FlowRight | `laja_elec_rights` |
| 4 | `qdm_j` | Mixed rights flow | [0, fmax_mixed×usage] | FlowRight | `laja_mixed_rights` |
| 5 | `qga_j` | Anticipated discharge | [0, fmax_antic×usage] | FlowRight | `laja_anticipated` |

All FlowRights use variable mode (`discharge=0`, `fmax>0`) with
`direction` signs connecting them to the `laja_partition` RightJunction.
The `qgt_j` column has `direction=+1` (supply); all others have
`direction=-1` (withdrawal).

**Monthly usage modulation:**

| Category | Apr | May | Jun | Jul | Aug | Sep | Oct | Nov | Dec | Jan | Feb | Mar |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Irrigation | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 1 | 1 |
| Electrical | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| Mixed | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| Anticipated | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 1 | 0 | 0 | 0 | 0 |

In gtopt, these are pre-multiplied into the `fmax` schedule by
`laja_writer.py`: `fmax = qmax * usage_factor` per stage.

#### 3.1.2 Stage-Level Hourly Aggregate Variables

PLP aggregates block flows to stage-average hourly rates:
```
IQDRH = sum_j [qdr_j * blodur_j / etadur]
```

| # | PLP Variable | Description | gtopt Mapping |
|---|---|---|---|
| 6 | `IQDRH` | Irrigation hourly rate [m3/s] | FlowRight `laja_irr_rights` → `qeh` column |
| 7 | `IQDEH` | Electrical hourly rate [m3/s] | FlowRight `laja_elec_rights` → `qeh` column |
| 8 | `IQDMH` | Mixed hourly rate [m3/s] | FlowRight `laja_mixed_rights` → `qeh` column |
| 9 | `IQGAH` | Anticipated hourly rate [m3/s] | FlowRight `laja_anticipated` → `qeh` column |
| 10 | `IQGTH` | Total turbine hourly rate [m3/s] | FlowRight `laja_total_gen` → `qeh` column |

Each FlowRight with `use_average=true` creates a `qeh` column and
averaging constraint: `qeh - sum_b [dur_ratio(b) * flow(b)] = 0`.
This mirrors PLP's duration-weighted aggregation exactly.

#### 3.1.3 Volume State Variables (Annual Rights Accumulation)

PLP tracks cumulative rights usage across stages:
```
IVDRF = Previous_IVDRF - (etadur/ScaleVol) * IQDRH
```

| # | PLP Variable | Description | Reset | gtopt Entity | gtopt Name |
|---|---|---|---|---|---|
| 11 | `IVDRF` | Irrigation volume remaining [hm3] | April | VolumeRight | `laja_vol_irr` |
| 12 | `IVDEF` | Electrical volume remaining [hm3] | April | VolumeRight | `laja_vol_elec` |
| 13 | `IVDMF` | Mixed volume remaining [hm3] | April | VolumeRight | `laja_vol_mixed` |
| 14 | `IVGAF` | Anticipated accumulated [hm3] | April | VolumeRight | `laja_vol_anticipated` |

All use `use_state_variable=true` and `reset_month=april`.

**Initial values** (from parsed `plplajam.dat`):

| Variable | `eini` | `emax` |
|---|---|---|
| `laja_vol_irr` | 266.0 hm3 | 5,000 hm3 |
| `laja_vol_elec` | 67.0 hm3 | 1,200 hm3 |
| `laja_vol_mixed` | 0.0 hm3 | 30 hm3 |
| `laja_vol_anticipated` | 0.0 hm3 | 5,000 hm3 |

#### 3.1.4 Economy State Variables (Not Yet Emitted by Writer)

| # | PLP Variable | Description | gtopt Status |
|---|---|---|---|
| 15 | `IVESF` | ENDESA economy final volume | Mappable as VolumeRight |
| 16 | `IVERF` | Reserve economy final volume | Mappable as VolumeRight |
| 17 | `IVAPF` | Alto Polcura economy final volume | Mappable as VolumeRight |

These are additional VolumeRight entities that could be emitted by the
converter.  They carry across the April reset boundary (no reset_month).

#### 3.1.5 Irrigation Withdrawal Variables

PLP models 3 districts × 4 categories = up to 12 withdrawal variables:

| District | 1oReg % | 2oReg % | Emerg % | Saltos % | Cost Factor | Injection |
|---|---|---|---|---|---|---|
| RIEGZACO (Zanartu-Collao) | 37.2% | 0% | 37.2% | 0% | 1.500 | — |
| RieTucapel | 62.8% | 100% | 62.8% | 0% | 1.000 | — |
| RieSaltos (Waterfall) | 0% | 0% | 0% | 100% | 0.200 | `LAJA_I` |

Default demand bases: 1oReg=90, 2oReg=53, Emerg=0, Saltos=7 m3/s.

Each non-zero combination generates a FlowRight named
`{district}_{category}` (e.g., `RIEGZACO_1o_reg`) with:
- `purpose = "irrigation"`, `direction = -1`
- `discharge` = base_demand × percentage × seasonal_factor (STBRealFieldSched)
- `fail_cost` = cost_irr_ns × cost_factor

**Seasonal demand factors** (Apr-Mar hydrological year):

| Category | Apr | May-Aug | Sep | Oct | Nov | Dec-Jan | Feb | Mar |
|---|---|---|---|---|---|---|---|---|
| 1oReg | 1.00 | 0.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| 2oReg | 0.20 | 0.00 | 0.30 | 0.65 | 0.85 | 1.00 | 0.80 | 0.50 |
| Emergencia | 0.20 | 0.00 | 0.30 | 0.65 | 0.85 | 1.00 | 0.80 | 0.50 |
| Saltos | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.50-1.00 | 1.00 | 0.00 |

#### 3.1.6 Deficit Variables

PLP creates deficit (fail) variables for unserved demand:

| # | PLP Variable | Description | Penalty | gtopt Mapping |
|---|---|---|---|---|
| 18 | `qdr_fail` | Irrigation non-served | 1,100 $/m3/s | FlowRight `laja_irr_rights` → `fail` col |
| 19 | `qde_fail` | Electrical non-served | 1,150 $/m3/s | FlowRight `laja_elec_rights` → `fail` col |
| 20 | `vrb` | Spillage of stored economics | varies | VolumeRight drain |
| 21 | `vun` | Lower-zone volume shortfall | varies | FlowRight/VolumeRight with `bound_rule` |
| 22 | `qen` | Unmet annual economics subsidy | varies | VolumeRight `fail` col |

**Monthly modulation of irrigation non-served cost:**
```
Apr:1.0  May:0.0  Jun-Aug:0.0  Sep:0.1  Oct:0.2  Nov:0.5
Dec-Jan:1.5  Feb:1.2  Mar:1.0
```

#### 3.1.7 Special Variables

| # | PLP Variable | Description | gtopt Status |
|---|---|---|---|
| 23 | `qgesh` | Economics-based generation | Mappable via FlowRight `bound_rule` |
| 24 | `qgerh` | Reserve economics generation | Mappable via FlowRight `bound_rule` |
| 25 | `qg50` | 50cm rebalse overflow | **Not modeled** (level-triggered rule) |
| 26 | `IQLAJA` | Total Laja discharge identity | UserConstraint |

### 3.2 LP Rows (Constraints) — `genpdlajam.f`

#### 3.2.1 Block-Level Constraints (per block j)

| # | PLP Constraint | Equation | Sense | gtopt Mapping |
|---|---|---|---|---|
| R1 | Flow partition | `-qgt_j + qdr_j + qde_j + qdm_j + qga_j = 0` | = | RightJunction `laja_partition` balance row |
| R2 | Irr demand satisfaction | `qdr_j + fail_irr_j >= demand_irr_j` | >= | FlowRight `laja_irr_rights` fail row |
| R3 | Elec demand satisfaction | `qde_j + fail_elec_j >= demand_elec_j` | >= | FlowRight `laja_elec_rights` fail row |

#### 3.2.2 Stage-Level Constraints

| # | PLP Constraint | Equation | Sense | gtopt Mapping |
|---|---|---|---|---|
| R4 | Irr averaging | `IQDRH - sum_j [dur_ratio_j * qdr_j] = 0` | = | FlowRight `laja_irr_rights` qavg row |
| R5 | Elec averaging | `IQDEH - sum_j [dur_ratio_j * qde_j] = 0` | = | FlowRight `laja_elec_rights` qavg row |
| R6 | Mixed averaging | `IQDMH - sum_j [dur_ratio_j * qdm_j] = 0` | = | FlowRight `laja_mixed_rights` qavg row |
| R7 | Antic averaging | `IQGAH - sum_j [dur_ratio_j * qga_j] = 0` | = | FlowRight `laja_anticipated` qavg row |
| R8 | Total averaging | `IQGTH - sum_j [dur_ratio_j * qgt_j] = 0` | = | FlowRight `laja_total_gen` qavg row |
| R9 | Irr vol accum | `IVDRF = Prev_IVDRF - (etadur/ScaleVol) * IQDRH` | = | VolumeRight `laja_vol_irr` energy balance |
| R10 | Elec vol accum | `IVDEF = Prev_IVDEF - (etadur/ScaleVol) * IQDEH` | = | VolumeRight `laja_vol_elec` energy balance |
| R11 | Mixed vol accum | `IVDMF = Prev_IVDMF - (etadur/ScaleVol) * IQDMH` | = | VolumeRight `laja_vol_mixed` energy balance |
| R12 | Antic vol accum | `IVGAF = Prev_IVGAF + (etadur/ScaleVol) * IQGAH` | = | VolumeRight `laja_vol_anticipated` energy balance |
| R13 | Irrigation supply | `IQRS - IQPR - IQNR - IQER - IQSR = 0` | = | RightJunction `laja_irrigation_supply` |
| R14 | Total discharge | `IQLAJA - IQGTH = 0` | = | UserConstraint identity |

**PLP-gtopt volume conversion equivalence:**
```
PLP:  IVDRF = Prev_IVDRF - (etadur / ScaleVol) * IQDRH
gtopt: efin = eini - fcr * sum_b[dur(b) * finp(b)] / energy_scale
```
Where `fcr = 0.0036 hm3/(m3/s*h)` is the flow-conversion-rate, and
`etadur/ScaleVol` in PLP serves the same conversion.

### 3.3 Volume Zones — Dynamic Bound Modification

PLP computes volume-dependent rights bounds via `FijaLajaMBloA`:
```
Rights_irr  = 570 + 0.00*V1 + 0.40*V2 + 0.40*V3 + 0.25*V4
Rights_elec = 0   + 0.05*V1 + 0.05*V2 + 0.40*V3 + 0.65*V4
Rights_mix  = 30  + 1.00*V1 + 0.00*V2 + 0.00*V3 + 0.00*V4
```

Where V_i is the volume within each zone (clamped to zone width):

| Zone | Width (hm3) | Cumulative Top | Irr Factor | Elec Factor | Mixed Factor |
|---|---|---|---|---|---|
| 1 | 1,200 | 1,200 | 0.00 | 0.05 | 1.00 |
| 2 | 170 | 1,370 | 0.40 | 0.05 | 0.00 |
| 3 | 530 | 1,900 | 0.40 | 0.40 | 0.00 |
| 4 | 3,682 | 5,582 | 0.25 | 0.65 | 0.00 |

Base values: Irrigation=570, Electrical=0, Mixed=30 m3/s.
Caps: Irrigation=5,000, Electrical=1,200, Mixed=30 m3/s.

**gtopt mapping — `bound_rule` segments for irrigation rights:**

The volume zone formula `Rights = Base + Σ(Factor_i × min(Vi, Width_i))`
is piecewise-linear in total volume V.  The converter
(`_zones_to_bound_rule_segments()`) transforms this to segments:

```json
{
  "bound_rule": {
    "reservoir": "ELTORO",
    "segments": [
      {"volume": 0,    "slope": 0.00, "constant": 570},
      {"volume": 1200, "slope": 0.40, "constant": 90},
      {"volume": 1370, "slope": 0.40, "constant": 90},
      {"volume": 1900, "slope": 0.25, "constant": 375}
    ],
    "cap": 5000
  }
}
```

Segment derivation for irrigation: at V=1200 the slope changes from 0.00
to 0.40, and `constant = rights(V) - slope*V = 570 - 0.40*1200 = 90`.
At V=1900 the slope changes to 0.25, and
`constant = (570+0+68+212) - 0.25*1900 = 375`.

**Example evaluations:**

| Volume (hm3) | PLP Calculation | bound_rule Evaluation |
|---|---|---|
| 500 | 570 + 0×500 = 570 | 570 + 0.00×500 = 570 |
| 1,300 | 570 + 0×1200 + 0.40×100 = 610 | 90 + 0.40×1300 = 610 |
| 3,000 | 570 + 0 + 68 + 212 + 0.25×1100 = 1,125 | 375 + 0.25×3000 = 1,125 |

**Identical results** — the piecewise-linear representation is
mathematically equivalent to PLP's volume zone formula.

#### Laja Volume Zone Diagram

```
  PLP "Colchones" (Laja)              gtopt bound_rule segments
  ============================         ============================

  VolMax = 5,582 hm3                   segment[3]: V >= 1900
  ┌────────────────────────┐           slope=0.25, constant=375
  │  Zone 4  (3,682 hm3)  │           Irr = 375 + 0.25*V
  │  Irr +0.25/hm3        │           Elec = -954.5 + 0.65*V
  │  Elec +0.65/hm3       │           cap: Irr=5000, Elec=1200
  ├── 1,900 hm3 ──────────┤         ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
  │  Zone 3  (530 hm3)    │           segment[2]: V >= 1370
  │  Irr +0.40/hm3        │           slope=0.40, constant=90
  │  Elec +0.40/hm3       │           (same slope as zone 2)
  ├── 1,370 hm3 ──────────┤         ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
  │  Zone 2  (170 hm3)    │           segment[1]: V >= 1200
  │  Irr +0.40/hm3        │           slope=0.40, constant=90
  │  Elec +0.05/hm3       │
  ├── 1,200 hm3 ──────────┤         ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
  │  Zone 1  (1,200 hm3)  │           segment[0]: V >= 0
  │  Irr +0.00/hm3        │           slope=0.00, constant=570
  │  Elec +0.05/hm3       │           (flat: Irr = 570 always)
  │  Mixed +1.00/hm3      │
  ├── VolMuerto = 0 ──────┤
  └────────────────────────┘
  Base: Irr=570, Elec=0, Mix=30
```

**Irrigation rights as a function of volume:**

```
  Rights
  (m3/s)
  5000 ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ cap ─ ─ ─
       │                                          ╱
  1125 │ · · · · · · · · · · · · · · · · · · · ·╱·(V=3000)
       │                                      ╱
   850 │ · · · · · · · · · · · · · · · · · ·╱
       │                               slope=0.25
       │                             ╱
   638 │ · · · · · · · · · · · · ·╱·
       │                  slope=0.40
   570 │▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓╱·
       │  slope=0.00  ╱
       │ (flat)     ╱
       └──────────┼───────┼────────┼────────────────────── V (hm3)
       0        1200    1370     1900                5582
                Zone 1 │Zone 2│  Zone 3 │    Zone 4
```

**Electrical rights as a function of volume:**

```
  Rights
  (m3/s)
  1200 ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ cap ─ ─ ─ ─ ─ ─
       │                                  ╱
       │                              ╱
   996 │ · · · · · · · · · · · · ·╱·(V=3000)
       │                       ╱  slope=0.65
       │                    ╱
   280 │ · · · · · · · ·╱·
       │             ╱  slope=0.40
    69 │ · · · · ·╱·
       │     ╱▓▓▓╱  slope=0.05
    60 │▓▓╱▓▓▓
       │╱ slope=0.05
       └──────────┼───────┼────────┼────────────────────── V (hm3)
       0        1200    1370     1900                5582
```

### 3.4 Laja Seepage Interaction

Laguna del Laja has significant natural seepage:
- **PLP parameter:** `CauFiltLaja = 47.0 m3/s` (fixed average)
- **Formula:** `VolFiltLaja = MIN(VoluLaja + VolAflLaja, CauFiltLaja * Cau2Vol)`
- Seepage reduces available volume before computing extraction limits
- **Network node:** `FiltLaja`

In gtopt, seepage is modeled by `ReservoirSeepage` entities on the
physical reservoir, which are separate from (but interact with) the
rights entities.  Seepage lowers reservoir volume → triggers `bound_rule`
re-evaluation → adjusts rights bounds.

### 3.5 Special Rules

**Lower volume zone (VolUtil <= VolUtilColInf):**
- Economics-based generation disabled (`qgesh = 0`)
- Annual rights reduced from 570 to 47 m3/s
- No new economy accumulation allowed
- gtopt: modeled via `bound_rule` threshold segment with constant=0

**50cm Rebalse Rule (above VolAcumA50cmReb):**
- Unrestricted spillage permitted
- Accounting capped at 47 m3/s for annual rights
- `qg50` variable freed from upper bound
- gtopt: **Not modeled** (level-triggered rule, not piecewise-linear)

### 3.6 Laja Summary: Entity Count

| gtopt Entity | Count | PLP Equivalent |
|---|---|---|
| RightJunction | 1 | `laja_partition` (drain=false) |
| FlowRight (partition) | 5 | qgt, qdr, qde, qdm, qga |
| FlowRight (districts) | 11 | 3 districts × 4 categories (zero-pct skipped) |
| VolumeRight | 4 | IVDRF, IVDEF, IVDMF, IVGAF |
| UserConstraint | 0 | (no percentage splits needed) |
| **Total entities** | **21** | |

---

## 4. PLP Maule Agreement: Complete Row and Column Mapping

### 4.1 LP Columns (Variables) — `genpdmaule.f`

#### 4.1.1 Block-Level Flow Variables (per block j, per central)

PLP creates a flow partition at each of 4 generation plants (LMAULE,
CIPRESES, PEHUENCHE, COLBUN):

```
-qg_i_j + m_qmne_j + m_qmnr_j + m_qmoe_j + m_qmor_j + m_qmce_j = 0
```

| # | PLP Variable | Description | gtopt Entity | gtopt Name |
|---|---|---|---|---|
| 1 | `m_qmne_j` | Normal electric rights flow | FlowRight | `maule_elec_normal` |
| 2 | `m_qmnr_j` | Normal irrigation rights flow | FlowRight | `maule_irr_normal` |
| 3 | `m_qmoe_j` | Ordinary reserve electric flow | FlowRight | `maule_elec_ordinary` |
| 4 | `m_qmor_j` | Ordinary reserve irrigation flow | FlowRight | `maule_irr_ordinary` |
| 5 | `m_qmce_j` | ENDESA compensation flow | FlowRight | `maule_compensation` |
| 6 | `qg_i_j` | Total generation flow (per central) | (partition junction) | `partition_{central}` |

All rights FlowRights connect to the `armerillo` RightJunction with
`direction=-1`.  The partition junctions per central use `drain=false`.

**Monthly irrigation percentage schedule** (PLP `PRiegoMaule`):

| Month | Apr | May | Jun | Jul | Aug | Sep | Oct | Nov | Dec | Jan | Feb | Mar |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Standard % | 30 | 20 | 0 | 0 | 0 | 30 | 70 | 90 | 100 | 100 | 80 | 55 |

Applied to `fmax`: `fmax = percentage/100 × gasto_riego_max (200 m3/s)`.

#### 4.1.2 Stage-Level Hourly Aggregate Variables

| # | PLP Variable | Description | gtopt Mapping |
|---|---|---|---|
| 7 | `IQMNEH` | Normal electric hourly rate | FlowRight `maule_elec_normal` → `qeh` |
| 8 | `IQMNRH` | Normal irrigation hourly rate | FlowRight `maule_irr_normal` → `qeh` |
| 9 | `IQMOEH` | Ordinary electric hourly rate | FlowRight `maule_elec_ordinary` → `qeh` |
| 10 | `IQMORH` | Ordinary irrigation hourly rate | FlowRight `maule_irr_ordinary` → `qeh` |
| 11 | `IQMCEH` | Compensation hourly rate | FlowRight `maule_compensation` → `qeh` |
| 12 | `IQTERH` | Total irrigation delivery rate | FlowRight `maule_irr_normal` → `qeh` |

#### 4.1.3 Resolucion 105 Variable

| # | PLP Variable | Description | Mode | gtopt Entity | gtopt Name |
|---|---|---|---|---|---|
| 13 | `IQA105` / `IQARMR` | Resolution 105 minimum flows | Fixed | FlowRight | `maule_res105` |

Variable index 31, constraint index 28 in `genpdmaule.f`.  Monthly
schedule from `QRiego105(12)`:

| Month | Apr | May | Jun | Jul | Aug | Sep | Oct | Nov | Dec | Jan | Feb | Mar |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Res105 (m3/s) | 80 | 40 | 40 | 40 | 40 | 60 | 140 | 180 | 200 | 200 | 180 | 120 |

In gtopt: FlowRight in fixed mode with `discharge` set to the monthly
schedule (STBRealFieldSched), `fail_cost=1000`.

#### 4.1.4 Volume State Variables

| # | PLP Variable | Description | Reset | gtopt Entity | gtopt Name |
|---|---|---|---|---|---|
| 14 | `IVMGEMF` | Monthly electric accumulated [hm3] | Every month | VolumeRight | `maule_vol_elec_monthly` |
| 15 | `IVMGEAF` | Annual electric accumulated [hm3] | June | VolumeRight | `maule_vol_elec_annual` |
| 16 | `IVMGRTF` | Seasonal irrigation accumulated [hm3] | June | VolumeRight | `maule_vol_irr_seasonal` |
| 17 | `VolCompEND` | ENDESA compensation volume [hm3] | — | VolumeRight | `maule_vol_compensation` |
| 18 | `VolEcoInv` | La Invernada winter economy [hm3] | — | VolumeRight | `maule_vol_econ_invernada` |

**Initial values and bounds** (from parsed `plpmaulen.dat`):

| gtopt Name | Reservoir | `eini` | `emax` | `reset_month` |
|---|---|---|---|---|
| `maule_vol_elec_monthly` | COLBUN | 0.0 | 25 hm3 | january |
| `maule_vol_elec_annual` | COLBUN | 49.183 | 250 hm3 | june |
| `maule_vol_irr_seasonal` | COLBUN | 163.363 | 800 hm3 | june |
| `maule_vol_compensation` | COLBUN | 10.5893 | 350 hm3 | — |
| `maule_vol_econ_invernada` | CIPRESES | 0.0 | — | — |

**Dual reset boundaries in Maule:**
- **January**: monthly electric (`IVMGEMF`) resets → `reset_month=january`
- **June**: annual electric (`IVMGEAF`) and seasonal irrigation (`IVMGRTF`)
  reset → `reset_month=june`
- Compensation and Invernada economy carry forward indefinitely (no reset)

#### 4.1.5 La Invernada Winter Storage Variables

| # | PLP Variable | Description | gtopt Status |
|---|---|---|---|
| 19 | `m_qidn_j` | La Invernada deficit discharge | Mappable via FlowRight `bound_rule` |
| 20 | `m_qisd_j` | La Invernada no-deficit storage | Mappable via FlowRight `bound_rule` |
| 21 | `m_qninv_j` | La Invernada natural inflow | FlowRight |
| 22 | `m_qhein_j` | Invernada storage to reservoir | FlowRight |
| 23 | `m_qhnein_j` | Invernada non-storage bypass | FlowRight |

Conditional logic:
- When deficit > inflow: `IQIDNH = ∞`, `IQISDH = 0` (must discharge)
- When deficit < inflow: `IQIDNH = 0`, `IQISDH = ∞` (can store)

#### 4.1.6 Multi-Reservoir Physical Variables

PLP tracks volumes, inflows, and spillages across three reservoirs:

| # | PLP Variable | Description | gtopt Mapping |
|---|---|---|---|
| 24 | `IIVolMaule` | Lago Maule volume | Reservoir entity (physical) |
| 25 | `IIVolInve` | La Invernada volume | Reservoir entity (physical) |
| 26 | `IIVolColbun` | Colbun volume | Reservoir entity (physical) |
| 27 | `IIAflMaule` | Maule inflows | Inflow entity (physical) |
| 28 | `IIAflInve` | Invernada inflows | Inflow entity (physical) |
| 29 | `IIAflColbun` | Colbun inflows | Inflow entity (physical) |
| 30 | `IIVertMaule` | Maule spillage | Reservoir spillage (physical) |
| 31 | `IIVertCip` | Cipreses spillage | Reservoir spillage (physical) |
| 32 | `IIVertColbun` | Colbun spillage | Reservoir spillage (physical) |

These are part of the physical hydro topology, NOT the rights domain.
They already exist in gtopt's core Reservoir/Waterway entities.

#### 4.1.7 Extraction Point Variables

| # | PLP Variable | Description | gtopt Status |
|---|---|---|---|
| 33 | `IIExtMauEND` | ENDESA extraction from Maule | FlowRight / physical |
| 34 | `IIExtMauRie` | Irrigation extraction from Maule | FlowRight / physical |
| 35 | `IIExtInvEND` | ENDESA extraction from Invernada | FlowRight / physical |
| 36 | `IIExtInvRie` | Irrigation extraction from Invernada | FlowRight / physical |

#### 4.1.8 Boolean State Flags (Non-LP)

| # | PLP Variable | Description | gtopt Status |
|---|---|---|---|
| 37 | `FPasoPorResOrd` | Has passed through ordinary reserve | **Not modeled** |
| 38 | `FVieneDePorSup` | Coming from upper portion (normal) | **Not modeled** |
| 39 | `FVieneDeResOrd` | Coming from ordinary reserve | **Not modeled** |

These track zone-transition hysteresis and are inherently non-LP
constructs.  They would require integer variables (MIP) or
pre-computation logic in the converter.

### 4.2 LP Rows (Constraints) — `genpdmaule.f`

#### 4.2.1 Block-Level Constraints (per block j)

| # | PLP Constraint | Equation | Sense | gtopt Mapping |
|---|---|---|---|---|
| R1 | Flow partition (per central) | `-qg_i_j + m_qmne_j + m_qmnr_j + m_qmoe_j + m_qmor_j + m_qmce_j = 0` | = | RightJunction `partition_{central}` |
| R2 | Armerillo balance | `Q_inter + Q_inv + Q_maule - Q_elec_n - Q_elec_r + drain >= 0` | >= | RightJunction `armerillo` (drain=true) |
| R3 | Invernada winter balance | `m_qidn_j + m_qisd_j + m_qninv_j - m_qhein_j - m_qhnein_j = QAflInvern` | = | FlowRight / RightJunction |

The Armerillo balance is the most explicit right junction in PLP.
When flag "Descuenta caudales de derechos electricos" = TRUE, electrical
rights flows are subtracted (`direction=-1`) from the balance.

#### 4.2.2 Stage-Level Constraints

| # | PLP Constraint | Equation | Sense | gtopt Mapping |
|---|---|---|---|---|
| R4 | Normal elec averaging | `IQMNEH - sum_j [dur_ratio_j * m_qmne_j] = 0` | = | FlowRight `maule_elec_normal` qavg row |
| R5 | Normal irr averaging | `IQMNRH - sum_j [dur_ratio_j * m_qmnr_j] = 0` | = | FlowRight `maule_irr_normal` qavg row |
| R6 | Ordinary elec averaging | `IQMOEH - sum_j [dur_ratio_j * m_qmoe_j] = 0` | = | FlowRight `maule_elec_ordinary` qavg row |
| R7 | Ordinary irr averaging | `IQMORH - sum_j [dur_ratio_j * m_qmor_j] = 0` | = | FlowRight `maule_irr_ordinary` qavg row |
| R8 | Compensation averaging | `IQMCEH - sum_j [dur_ratio_j * m_qmce_j] = 0` | = | FlowRight `maule_compensation` qavg row |
| R9 | Monthly elec vol accum | `IVMGEMF = Prev + (etadur/ScaleVol) * IQMNEH` | = | VolumeRight `maule_vol_elec_monthly` energy |
| R10 | Annual elec vol accum | `IVMGEAF = Prev + IVMGEMF` | = | VolumeRight `maule_vol_elec_annual` energy |
| R11 | Seasonal irr vol accum | `IVMGRTF = Prev + (etadur/ScaleVol) * IQMNRH` | = | VolumeRight `maule_vol_irr_seasonal` energy |
| R12 | Delivery constraint | `-IQDRAH + IQDRMH + IQINVH <= 0` | <= | RightJunction (delivery) |
| R13 | Res 105 supply | `IQA105 = QRiego105(month) * factor` | = | FlowRight `maule_res105` fixed discharge |

#### 4.2.3 Percentage Allocation Constraints (via UserConstraint)

PLP implements the 20/80 ordinary reserve split and district allocation
percentages as LP constraints.  In gtopt, these are `UserConstraint`
entities with single-quoted flow references:

| # | PLP Constraint | gtopt UserConstraint Expression |
|---|---|---|
| R14 | Electric ≤ 20% of ordinary | `flow_right('maule_elec_ordinary').flow <= 0.2 * flow_right('maule_elec_ordinary').flow + 0.2 * flow_right('maule_irr_ordinary').flow` |
| R15 | Irrigation ≤ 80% of ordinary | `flow_right('maule_irr_ordinary').flow <= 0.8 * flow_right('maule_elec_ordinary').flow + 0.8 * flow_right('maule_irr_ordinary').flow` |
| R16 | RieCMNA = 12.66% | `flow_right('RieCMNA').flow = 0.1266 * flow_right('maule_irr_normal').flow` |
| R17 | RieCMNB = 14.70% | `flow_right('RieCMNB').flow = 0.147 * flow_right('maule_irr_normal').flow` |
| R18 | RieMaitenes = 10.49% | `flow_right('RieMaitenes').flow = 0.1049 * flow_right('maule_irr_normal').flow` |
| R19 | RieMauleSur = 11.97% | `flow_right('RieMauleSur').flow = 0.1197 * flow_right('maule_irr_normal').flow` |
| R20 | RieMelado = 12.65% (slack) | `flow_right('RieMelado').flow <= 0.1265 * flow_right('maule_irr_normal').flow` |
| R21 | RieSur123SCDZ = 34.27% | `flow_right('RieSur123SCDZ').flow = 0.3427 * flow_right('maule_irr_normal').flow` |
| R22 | RieMolinosOtros = 3.26% | `flow_right('RieMolinosOtros').flow = 0.0326 * flow_right('maule_irr_normal').flow` |

Note: RieMelado uses `<=` (slack district); all others use `=` (exact).
Total percentages: 12.66+14.70+10.49+11.97+12.65+34.27+3.26 = 100%.

### 4.3 Three-Zone Reservoir Operation — Dynamic Bound Modification

PLP's `FijaMaule` callback applies zone-dependent bounds based on
Colbun reservoir volume:

```
                      VolMax
  ─────────────────────────────────────
  │  Normal Zone                       │  > 581 hm3
  ├── VResOrd + VResExt = 581 hm3 ────┤
  │  Ordinary Reserve (452 hm3)        │  129 – 581 hm3
  ├── VReservaExtraord = 129 hm3 ─────┤
  │  Extraordinary Reserve (129 hm3)   │  0 – 129 hm3
  ─────────────────────────────────────
```

**gtopt mapping — `bound_rule` step-function segments:**

Normal electric rights (active only in normal zone):
```json
{"segments": [
  {"volume": 0,   "slope": 0, "constant": 0},
  {"volume": 581, "slope": 0, "constant": 30}
]}
```

Ordinary reserve electric (active only in ordinary zone):
```json
{"segments": [
  {"volume": 0,   "slope": 0, "constant": 0},
  {"volume": 129, "slope": 0, "constant": 30},
  {"volume": 581, "slope": 0, "constant": 0}
]}
```

Normal irrigation (active only in normal zone):
```json
{"segments": [
  {"volume": 0,   "slope": 0, "constant": 0},
  {"volume": 581, "slope": 0, "constant": 200}
]}
```

The three-segment pattern for ordinary reserve achieves zone exclusivity:
- V < 129: constant=0 → deactivated
- 129 ≤ V < 581: constant=30 → active in ordinary zone
- V ≥ 581: constant=0 → deactivated (normal zone takes over)

#### Maule Three-Zone Diagram

```
  PLP "Colchones" (Maule)             gtopt bound_rule segments
  ============================         ============================

       VolMax (Colbun)
  ┌────────────────────────┐
  │                        │           Normal electric bound_rule:
  │   Normal Zone          │           seg[0]: V>=0,   const=0   (OFF)
  │                        │           seg[1]: V>=581, const=30  (ON)
  │   Elec: 30 m3/s daily  │
  │   Irr:  200 m3/s       │           Normal irrigation bound_rule:
  │   Comp: allowed         │           seg[0]: V>=0,   const=0   (OFF)
  │                        │           seg[1]: V>=581, const=200 (ON)
  ├── 581 hm3 ─────────────┤         ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
  │                        │           Ordinary electric bound_rule:
  │   Ordinary Reserve     │           seg[0]: V>=0,   const=0   (OFF)
  │   (452 hm3)            │           seg[1]: V>=129, const=30  (ON)
  │                        │           seg[2]: V>=581, const=0   (OFF)
  │   20% → ENDESA         │
  │   80% → Irrigators     │           Ordinary irrigation bound_rule:
  │                        │           seg[0]: V>=0,   const=0   (OFF)
  │   UserConstraint:      │           seg[1]: V>=129, const=200 (ON)
  │   flow_right('elec')   │           seg[2]: V>=581, const=0   (OFF)
  │    <= 20% of total     │
  ├── 129 hm3 ─────────────┤         ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
  │                        │
  │   Extraordinary        │           All rights: const=0 (OFF)
  │   Reserve (129 hm3)    │           No extraction allowed
  │                        │
  │   Minimal flows only   │
  ├── 0 hm3 ───────────────┤
  └────────────────────────┘
```

**How zone exclusivity works with bound_rule:**

PLP uses `FijaMaule` with if/else logic to activate one zone at a time.
gtopt achieves the same effect with overlapping bound_rule segments:

```
  Electric
  Rights     Normal elec         Ordinary elec
  (m3/s)     bound_rule:          bound_rule:

   30  ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┌──────────────┐         ┌──────
       │                         │  ON (30)     │         │ON(30)
       │                         │              │         │
       │ Ordinary electric       │              │  Normal │
       │ is active HERE          │              │  elec   │
       │                         │              │  active │
    0  │▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓┘   OFF (0)   └─────────┘OFF(0)
       └──────────┼──────────────┼────────────────────────── V (hm3)
       0         129            581

       ◄── Extraordinary ──►◄── Ordinary ──────►◄── Normal ──────►
            (no rights)       (20/80 split)       (full rights)
```

**Seepage-rights compounding effect:**

```
  ┌─────────────────────────────────────────────────┐
  │             Physical Reservoir (Colbun)          │
  │                                                 │
  │  Volume ──► ReservoirSeepage ──► seepage flow   │
  │    │                                            │
  │    │        (reduces volume)                    │
  │    ▼                                            │
  │  Lower volume ──► bound_rule re-evaluation      │
  │    │                                            │
  │    ▼                                            │
  │  Zone transition ──► rights bounds change       │
  │    │                                            │
  │    ▼                                            │
  │  Reduced extraction ──► different dispatch      │
  └─────────────────────────────────────────────────┘
```

### 4.4 Maule Seepage (Volume-Dependent)

Three reservoirs have piecewise-linear seepage curves (from `plpfilemb.dat`):

| Reservoir | Avg Seepage | Segments | Downstream |
|---|---|---|---|
| ELTORO (Laguna del Laja) | 30.80 m3/s | 3 segments | ABANICO |
| CIPRESES (La Invernada) | 14.20 m3/s | 4 segments | FILT_CIPRESES |
| COLBUN | 6.10 m3/s | 3 segments | SAN_CLEMENTE |

Seepage reduces reservoir volume, which may trigger zone transitions
and further reduce extraction rights (compounding effect).

### 4.5 La Invernada Winter Storage Model

La Invernada is a seasonal reservoir used for winter water banking.
PLP models it with five block-level flow variables forming a balance:

```
  m_qidn_j + m_qisd_j + m_qninv_j - m_qhein_j - m_qhnein_j = QAflInvern
```

Where:
- `m_qidn_j` — deficit-mode discharge (must release when irrigators need
  more than inflow provides)
- `m_qisd_j` — no-deficit storage flow (can store when irrigators are
  satisfied)
- `m_qninv_j` — natural inflow to La Invernada
- `m_qhein_j` — storage into reservoir (winter banking)
- `m_qhnein_j` — bypass (water not stored, passes through)

**Conditional bound flipping** (computed in `plp-maule1.f`):

| Condition | `IQIDNH` uppb | `IQISDH` uppb | Interpretation |
|---|---|---|---|
| Irrigation deficit > inflow | `∞` | `0` | Must discharge to cover deficit |
| Irrigation deficit < inflow | `0` | `∞` | May store excess for later use |

This is a **simulation-level conditional**, not a pure LP constraint:
the upper bounds flip based on whether current-stage irrigation demand
exceeds available surface inflow, computed before the LP solve.

**Objective terms:**
- `+FCau * CostoEmbalsar * IQHINV` — storage cost (0.1 $/m3)
- `+FCau * CostoNoEmbalsar * IQHNEIN` — bypass penalty (varies)

**gtopt status:** The writer emits a single VolumeRight
(`maule_vol_econ_invernada`) tracking economy state, but does **not**
emit the five balance variables, the conditional bound flipping, or
the storage/bypass objective terms.

**Possible mapping:** The five balance variables map to FlowRight entities
connected through a dedicated RightJunction (drain=false). The conditional
bound flipping could be implemented as a `bound_rule` on the `m_qidn` and
`m_qisd` FlowRights where the reference volume is computed from the
irrigation demand gap, or as an `update_lp` rule that evaluates net
demand vs. inflow at each stage.

### 4.6 Bocatoma Canelon

A specialized infrastructure node for the Canelon irrigation intake:

| Parameter | Value |
|---|---|
| Name | `BCanelon` |
| Cost | 10 $/m3/s per block-hour |
| Type | Infrastructure diversion cost |
| PLP variable | Part of objective function: `CostoCanelon * BloDur/FPhi` |

In PLP, this cost appears in the objective function proportional to
block duration. It penalizes the use of the Canelon intake when water
is diverted through it, reflecting civil engineering costs.

**gtopt status:** Not yet emitted by the writer. Mappable as a FlowRight
with `use_cost = 10.0` at the diversion point.

### 4.7 Colbun Cota 425

Colbun reservoir has a volume-dependent operational mode switch:

| Parameter | Value |
|---|---|
| Volume threshold (Cota 425) | 1,047.9 hm3 |
| Extraction index above | `nExtColb = 1` (normal extraction) |
| Extraction index below | `nExtColb = 2` (restricted extraction) |

When Colbun volume drops below 1,047.9 hm3, the extraction point
index changes, which affects how water is allocated between ENDESA and
irrigation extractions (`IIExtMauEND`, `IIExtMauRie`).

In PLP, this is a **per-simulation conditional** checked in
`genpdmaule.f` before LP assembly. It is not a continuous LP constraint
but a discrete mode switch.

**gtopt status:** Not modeled. Could potentially be approximated by a
`bound_rule` on extraction FlowRights with a step function at 1,047.9 hm3,
but the extraction-point reassignment logic is inherently non-LP.

### 4.8 Maule Summary: Entity Count

**Currently emitted by `maule_writer.py`:**

| gtopt Entity | Count | PLP Equivalent |
|---|---|---|
| RightJunction | 5 | `armerillo` (drain=true) + 4 plant partitions |
| FlowRight (rights) | 5 | IQMNE, IQMNR, IQMOE, IQMOR, IQMCE |
| FlowRight (Res105) | 1 | IQA105 |
| FlowRight (districts) | 7 | 7 irrigation withdrawal districts |
| VolumeRight | 5 | IVMGEMF, IVMGEAF, IVMGRTF, VolCompEND, VolEcoInv |
| UserConstraint | 9 | 2 percentage splits + 7 district allocations |
| **Total emitted** | **32** | |

**Not yet emitted (would add ~12 entities):**

| gtopt Entity | Count | PLP Equivalent |
|---|---|---|
| RightJunction (Invernada) | 1 | La Invernada winter balance |
| FlowRight (Invernada) | 5 | IQIDN, IQISD, IQNINV, IQHEIN, IQHNEIN |
| FlowRight (extractions) | 4 | ExtMauEND/Rie, ExtInvEND/Rie |
| FlowRight (Canelon) | 1 | BCanelon intake |
| **Total not emitted** | **~11** | |

---

## 5. Objective Function Comparison

### 5.1 Laja Objective Terms

| PLP Term | Expression | $/unit | gtopt Mapping | Emitted? |
|---|---|---|---|---|
| Irr non-served | `CRiegoNS * qdr_fail` | 1,100 | FlowRight `fail_cost=1100` | **Yes** |
| Elec non-served | `CElectNS * qde_fail` | 1,150 | FlowRight `fail_cost=1150` | **Yes** |
| Irr usage cost | `CUsoRiego * qdr` | 0.0 | (zero, no cost needed) | N/A |
| Elec usage cost | `CUsoElect * qde` | 0.1 | FlowRight `use_cost=0.1` | No (*) |
| Mixed cost | `CMixto * qdm` | 1.0 | FlowRight `use_cost=1.0` | No (*) |
| Econ spillage | `CVertEcon * vrb` | varies | VolumeRight drain cost | No |
| Lower-zone shortfall | `CVolUtilNeg * vun` | varies | VolumeRight fail cost | No |
| Econ subsidy unmet | `CSubEconAnu * qen` | varies | VolumeRight fail cost | No |
| Alto Polcura value | `CostoVAP * vapf` | varies | VolumeRight cost | No |

(*) Requires `use_cost` field on FlowRight (not yet in gtopt C++).

**District cost factors** multiply the base `CRiegoNS`:

| District | Cost Factor | Effective fail_cost | Emitted? |
|---|---|---|---|
| RIEGZACO | 1.500 | 1,650 | **Yes** |
| RieTucapel | 1.000 | 1,100 | **Yes** |
| RieSaltos | 0.200 | 220 | **Yes** |

**Emitted: 4/9 terms** (the two core deficit penalties + district
factors). Missing terms are all related to economy tracking or usage
costs not yet emitted by the writer.

### 5.2 Maule Objective Terms

| PLP Term | Expression | $/unit | gtopt Mapping | Emitted? |
|---|---|---|---|---|
| Irr benefit (Res105) | `-FCau * ValorRiego105 * QA105` | -1,100 | FlowRight `fail_cost=1000` | **Yes** |
| Irr benefit (Maule) | `-FCau * ValorRiegoMaule * IQTERH` | -1,100 | FlowRight `fail_cost=1000` | **Yes** |
| Irr unserved (Maule) | `+FCau * CostoRiegoNSMaule * IQDRAH` | 1,000 | FlowRight `fail_cost=1000` | **Yes** |
| Storage cost | `+FCau * CostoEmbalsar * IQHINV` | 0.1 | FlowRight `use_cost=0.1` | No (*) |
| Non-storage penalty | `+FCau * CostoNoEmbalsar * IQHNEIN` | varies | FlowRight `use_cost` | No (*) |
| Canal infrastructure | `+CostoCanelon * BloDur/FPhi` | 10 | FlowRight `use_cost=10` | No (*) |
| Conv penalty 1 | `+Penalizador_1` | 1,500 | FlowRight `fail_cost=1500` | **Yes** |
| Conv penalty 2 | `+Penalizador_2` | 1,000 | FlowRight `fail_cost=1000` | **Yes** |

(*) Requires `use_cost` field on FlowRight (not yet in gtopt C++).

**Emitted: 5/8 terms.** Missing terms are all `use_cost`-dependent
(small steering costs and infrastructure costs).

---

## 6. Calendar Year and Seasonal Reset Rules

PLP irrigation agreements do **not** use the calendar year
(January-December) as their annual cycle.

### 6.1 Laja — Hydrological Year (April-March)

- Annual reset at **April 1**: volume counters (IVDRF, IVDEF, IVDMF,
  IVGAF) re-initialized
- Irrigation season: **September-April** (zero extraction May-August)
- Economy volumes (IVESF, IVERF) carry across calendar year (no reset)
- Monthly factors are 12-element vectors indexed by hydrological month
  (Apr=0, Mar=11)

**gtopt mapping:**
- VolumeRight `reset_month = "april"` for all 4 rights accumulators
- FlowRight `fmax` schedule has zeros for May-August stages
- `laja_writer.py` converts hydrological indices to calendar months
  via `_hydro_to_stage_schedule()`

### 6.2 Maule — Dual Reset Boundaries (January and June)

- **January 1**: monthly electric accumulator (`IVMGEMF`) resets,
  compensation quotas reset, ordinary reserve redistribution
- **June 1**: annual electric (`IVMGEAF`) and seasonal irrigation
  (`IVMGRTF`) reset
- Compensation and Invernada economy carry forward indefinitely

**gtopt mapping:**
- `maule_vol_elec_monthly`: `reset_month = "january"`
- `maule_vol_elec_annual`: `reset_month = "june"`
- `maule_vol_irr_seasonal`: `reset_month = "june"`
- `maule_vol_compensation`: no reset_month (carries forward)
- `maule_vol_econ_invernada`: no reset_month (carries forward)

### 6.3 Cumulative Volume Targets (Maule)

Accumulated irrigation rights volume remaining to end season (hm3):

| Month | Apr | May | Jun-Aug | Sep | Oct | Nov | Dec | Jan | Feb | Mar |
|---|---|---|---|---|---|---|---|---|---|---|
| Volume | 262.7 | 107.1 | 0 | 3,012.8 | 2,857.2 | 2,482.3 | 2,015.7 | 1,480.0 | 944.4 | 557.3 |
| Days remaining | 61 | 31 | 0 | 273 | 243 | 212 | 182 | 151 | 120 | 92 |

---

## 7. Coverage Summary

### 7.1 Fully Covered by gtopt Generic Entities

| Feature | PLP Construct | gtopt Construct |
|---|---|---|
| Flow partition | `-qgt + qdr + qde + qdm + qga = 0` | RightJunction (drain=false) + FlowRight (variable mode) |
| Stage-average flows | `IQDRH = sum_j [qdr_j * dur_j / etadur]` | FlowRight `use_average=true` → `qeh` |
| Volume accumulation | `IVDRF = prev - dur * IQDRH` | VolumeRight with `use_state_variable=true` |
| Rights balance | Armerillo balance equation | RightJunction (drain=true) with FlowRight direction signs |
| Physical coupling | Withdrawal from physical balance | FlowRight coupled to Junction |
| Deficit penalties | CRiegoNS, CElectNS | FlowRight/VolumeRight `fail_cost` |
| Seasonal schedules | Monthly factor tables | FlowRight/VolumeRight schedule fields |
| Withdrawal districts | Percentage-based distribution | FlowRight + UserConstraint proportional splits |
| Percentage allocation | 20/80 reserve split | UserConstraint with coefficient expressions |
| Res 105 minimum flows | Fixed monthly requirements | FlowRight fixed mode |
| Volume zones | Volume-dependent rights bounds | `bound_rule` piecewise-linear segments |
| Dynamic per-sim bounds | FijaLajaMBloA/FijaMaule | `update_lp` with `bound_rule` |
| 3-zone operation | FijaMaule zone logic | `bound_rule` step-function segments |
| Annual/seasonal reset | Reset accumulators Apr/Jun/Jan | VolumeRight `reset_month` field |
| Seepage-rights interaction | VolFilt reduces available volume | ReservoirSeepage → volume → bound_rule |
| La Invernada conditional | IQIDN/IQISD | FlowRight with `bound_rule` |
| Volume-to-volume coupling | Inter-VolumeRight balance | VolumeRight `right_reservoir` + `direction` |

### 7.2 Not Yet Emitted by Writers (Mappable with Existing Entities)

These PLP features can be represented using existing gtopt entity types
but the plp2gtopt writers do not yet emit them:

| Feature | PLP Construct | gtopt Mapping | Complexity |
|---|---|---|---|
| Economy accumulators (Laja) | IVESF, IVERF, IVAPF | 3 VolumeRight entities (no reset_month) | Low |
| Lower-zone economy bounds | qgesh/qgerh capped when vol < threshold | FlowRight `bound_rule` with step at VolUtilColInf | Low |
| Elec/Mixed usage cost | CUsoElect=0.1, CMixto=1.0 | FlowRight `use_cost` field | Low |
| La Invernada balance | 5 flow variables + RJ balance | 5 FlowRight + 1 RightJunction | Medium |
| Storage/bypass costs | CostoEmbalsar, CostoNoEmbalsar | FlowRight `use_cost` on storage/bypass | Low |
| Bocatoma Canelon | CostoCanelon=10 $/m3/s | FlowRight `use_cost=10` | Low |
| Extraction point variables | IIExtMauEND/Rie, IIExtInvEND/Rie | FlowRight per extraction point | Medium |
| Monthly fail_cost | CRiegoNS varies by month | FlowRight `fail_cost` as schedule | Medium (*) |

(*) `fail_cost` is currently OptReal; making it a schedule (OptTRealFieldSched)
requires a gtopt C++ change.

### 7.3 Not Covered (Requires New gtopt Features or Non-LP Logic)

These PLP features use simulation-level conditional logic that cannot
be represented in a pure LP formulation:

| Feature | PLP Construct | Why It's Hard | Possible Approach |
|---|---|---|---|
| 50cm rebalse rule | `qg50` freed above `VolAcumA50cmReb` | Threshold-triggered bound relaxation, not piecewise-linear | `bound_rule` with large step at threshold |
| Boolean state flags | FPasoPorResOrd, FVieneDePorSup, FVieneDeResOrd | Binary state carried across SDDP stages | Integer variables (MIP) or pre-computation |
| Zone-transition hysteresis | Direction-dependent zone behavior | State depends on trajectory, not just current volume | Simulation-level state machine |
| Invernada conditional bounds | IQIDNH/IQISDH flip based on demand vs. inflow | Pre-LP bound computation (not volume-dependent) | Custom `update_lp` rule comparing demand vs. inflow |
| Colbun Cota 425 mode switch | Extraction index changes below 1,047.9 hm3 | Discrete mode switch, not continuous | Step `bound_rule` at threshold |
| Post-solve economics | Maule2 redistributes reserve quotas at Jan/Jun | Multi-stage callback modifying state between solves | Converter pre-computation or custom post-solve hook |

#### 7.3.1 The 50cm Rebalse Rule (Laja)

When Laguna del Laja volume exceeds `VolAcumA50cmReb` (~5,074 hm3):
- The spillage variable `qg50` loses its upper bound (unrestricted)
- Annual rights accounting is capped at 47 m3/s regardless of volume
- This creates an operational "overflow mode" where water is spilled
  without consuming rights quotas

In PLP, this is implemented in `FijaLajaMBloA` as a conditional bound
modification. In gtopt, this could be approximated with a `bound_rule`
on a spillage FlowRight where the slope changes from 0 to infinity
at the threshold, but the accounting cap (47 m3/s) is harder to model
since it requires modifying the *value* of rights consumption, not just
the bound on flow.

#### 7.3.2 Boolean State Flags (Maule)

PLP carries three boolean flags across SDDP stages:

```
  Stage t-1                     Stage t
  ┌──────────┐                  ┌──────────────────────────┐
  │ Solve LP │──► Maule2 ──►   │ Read flags:              │
  │          │    updates:      │  if FPasoPorResOrd:      │
  └──────────┘    FPasoPor...   │    apply reserve rules   │
                  FVieneDe...   │  if FVieneDePorSup:      │
                  FVieneDe...   │    allow normal alloc    │
                                │  Set LP bounds based on  │
                                │  flag combination        │
                                └──────────────────────────┘
```

These flags gate which reserve allocation rules apply at the 20/80
split boundaries. For example:
- If `FPasoPorResOrd` is true and `FVieneDePorSup` is true, the
  reservoir is transitioning from normal to reserve, and special
  reallocation applies
- If `FVieneDeResOrd` is true, the system was already in reserve
  mode and different bounds are used

These are inherently non-LP: they represent discrete operational modes
that depend on the trajectory through the zone structure, not just the
current volume level.

**Impact assessment:** The boolean flags primarily affect how the 20/80
ordinary reserve split is managed during zone transitions. For most
operational scenarios (volume well above or below zone boundaries),
the flags have no effect. They matter mainly during rare mid-season
transitions between zones.

#### 7.3.3 La Invernada Conditional Bound Flipping

The most complex missing feature. PLP computes the irrigation demand gap
*before* the LP solve:

```
  gap = irrigation_demand - surface_inflow
  if gap > 0:
    # Deficit: must discharge from Invernada
    set IQIDNH_uppb = infinity
    set IQISDH_uppb = 0
  else:
    # Surplus: may store in Invernada
    set IQIDNH_uppb = 0
    set IQISDH_uppb = infinity
```

This cannot be modeled as a volume-dependent `bound_rule` because the
condition depends on **demand vs. inflow**, not on reservoir volume.
Options:
1. **Pre-computation:** The converter computes the seasonal pattern
   and sets bounds statically (works for deterministic runs)
2. **Custom update_lp:** A new `update_lp` variant that reads demand
   and inflow values from the LP solution and flips bounds accordingly
3. **Big-M formulation:** Use a MIP indicator constraint to select
   between discharge and storage modes

### 7.4 Quantitative Coverage Assessment

**Laja Agreement:**

| Category | Emitted | Total | Status |
|---|---|---|---|
| Block flow variables | 5 | 5 | **Emitted** |
| Hourly aggregates (qeh) | 5 | 5 | **Emitted** |
| Core volume state (IVDRF etc.) | 4 | 4 | **Emitted** |
| Economy state (IVESF, IVERF, IVAPF) | 0 | 3 | Mappable, not emitted |
| Volume zone bound_rule | 3 categories | 3 | **Emitted** |
| Withdrawal districts | 3 × 4 categories | 12 max | **Emitted** (non-zero only) |
| Deficit penalties (CRiegoNS, CElectNS) | 2 | 2 | **Emitted** |
| Usage costs (CUsoElect, CMixto) | 0 | 2 | Not emitted (needs `use_cost`) |
| Monthly factors | 4 categories | 4 | **Emitted** |
| Seepage | Physical layer | — | **Emitted** (ReservoirSeepage) |
| 50cm rebalse rule | 0 | 1 | Not modeled |
| Lower-zone economy bounds | 0 | 2 | Mappable via bound_rule |
| **LP variables covered** | **14/17** | | **82% emitted, 100% mappable** |

**Maule Agreement:**

| Category | Emitted | Total | Status |
|---|---|---|---|
| Flow partition (per central) | 5+1 | 6 | **Emitted** |
| Hourly aggregates | 6 | 6 | **Emitted** |
| Volume state (IVMGEMF etc.) | 5 | 5 | **Emitted** |
| Armerillo balance | 1 | 1 | **Emitted** |
| Withdrawal districts | 7 | 7 | **Emitted** |
| Res 105 flows | 1 | 1 | **Emitted** |
| 3-zone bound_rule | 4 rights | 4 | **Emitted** |
| Percentage allocation | 9 constraints | 9 | **Emitted** |
| La Invernada balance (5 vars) | 0 | 5 | Mappable, not emitted |
| La Invernada conditional bounds | 0 | 2 | Needs custom logic |
| Storage/bypass costs | 0 | 2 | Mappable, not emitted |
| Extraction points (4 vars) | 0 | 4 | Mappable, not emitted |
| Boolean state flags | 0 | 3 | Not modeled (non-LP) |
| Bocatoma Canelon | 0 | 1 | Mappable, not emitted |
| Colbun Cota 425 | 0 | 1 | Needs conditional logic |
| **LP variables covered** | **32/50** | | **64% emitted, 84% mappable** |

**Overall assessment:** The core rights accounting structure (flow
partition, volume accumulation, zone-dependent bounds, percentage
allocation) is fully implemented. The remaining gaps are:
- **Easy wins** (mappable now): economy accumulators, usage costs,
  extraction points, Bocatoma Canelon, storage/bypass costs
- **Medium** (needs `update_lp` or schedule extension): La Invernada
  balance, monthly fail_cost
- **Hard** (non-LP): boolean flags, zone hysteresis, Cota 425 mode
  switch

---

## 8. Architectural Comparison

### 8.1 PLP Approach: Monolithic Procedural

PLP implements each agreement as a ~2000-line Fortran module with:
- **Fixed-dimension arrays** allocated per agreement type
- **Per-simulation callbacks** (`FijaLajaMBloA`, `FijaMaule`) modifying LP
  bounds based on current reservoir volume
- **Hardcoded subroutines** (`genpdlajam.f`, `genpdmaule.f`) building the
  LP constraint matrix
- **Post-solve state updates** (`Laja2`, `Maule2`) tracking zone transitions

**Advantages:** Arbitrary conditional logic (zones, transitions, boolean flags).
**Disadvantages:** Each agreement is a separate Fortran module; adding a new
agreement requires writing a new module from scratch.

### 8.2 gtopt Approach: Generic Data-Driven

gtopt uses three generic entity types composed via JSON:
- **RightJunction** for flow balance nodes
- **FlowRight** for flow-based rights with variable/fixed modes
- **VolumeRight** for volume-based rights with state coupling
- **UserConstraint** for proportional allocation and identity constraints

**Advantages:**
- New agreements configured via JSON, no C++ changes
- Reusable entities compose arbitrarily
- Consistent LP formulation across all agreements
- plp2gtopt converter automates PLP → gtopt translation

**Disadvantages:**
- Boolean state tracking not possible in pure LP
- Level-triggered rules (50cm rebalse) not yet modeled

### 8.3 Key Design Difference

| Aspect | PLP | gtopt |
|---|---|---|
| Bound modification | Fortran callback per agreement | `bound_rule` JSON + generic `update_lp` |
| Constraint generation | Hardcoded matrix assembly | Auto-generated from entity fields |
| Percentage allocation | LP constraint in Fortran | UserConstraint expression in JSON |
| Monthly scheduling | Factor arrays in Fortran | Schedule fields on entities |
| New agreement cost | ~2000 lines of Fortran | JSON configuration only |

Both approaches re-evaluate bounds during the solve loop:
- PLP calls `FijaLajaMBloA`/`FijaMaule` at each simulation step
- gtopt calls `FlowRightLP::update_lp`/`VolumeRightLP::update_lp` via
  the `HasUpdateLP` concept auto-dispatch

---

## 9. Remaining Differences and Roadmap

### 9.1 Converter-Only Changes (No C++ Required)

These can be implemented entirely in `laja_writer.py` / `maule_writer.py`
using existing gtopt entity types:

| # | Feature | Writer | Entities to Emit |
|---|---|---|---|
| 1 | Laja economy accumulators | `laja_writer.py` | 3 VolumeRight: IVESF, IVERF, IVAPF (no reset_month) |
| 2 | Laja lower-zone bounds | `laja_writer.py` | Add `bound_rule` step at VolUtilColInf for qgesh/qgerh |
| 3 | La Invernada balance | `maule_writer.py` | 5 FlowRight + 1 RightJunction for winter storage |
| 4 | Extraction points | `maule_writer.py` | 4 FlowRight: ExtMauEND/Rie, ExtInvEND/Rie |
| 5 | Bocatoma Canelon cost | `maule_writer.py` | 1 FlowRight with `use_cost=10` |
| 6 | Storage/bypass costs | `maule_writer.py` | FlowRight `use_cost=0.1` on IQHINV, penalty on IQHNEIN |

**Estimated effort:** Each is 20-50 lines of Python in the writer +
corresponding test cases.

### 9.2 Requires New gtopt C++ Fields

| # | Feature | Change | Impact |
|---|---|---|---|
| 1 | Usage cost on FlowRight | Add `use_cost` (OptReal) field | Objective coefficient on flow column |
| 2 | Schedule-valued fail_cost | Change `fail_cost` from OptReal to OptTRealFieldSched | Per-stage penalty modulation |
| 3 | Negative objective (benefit) | FlowRight with negative `use_cost` for ValorRiego105 | Incentivizes meeting Res105 delivery |

Usage cost is the most impactful: PLP applies `CUsoElect=0.1`,
`CMixto=1.0`, `CostoEmbalsar=0.1`, `CostoCanelon=10.0` as LP
objective coefficients on flow variables. Without `use_cost`, these
small steering costs are lost, potentially changing optimal dispatch.

### 9.3 Requires New gtopt Logic (Non-Trivial)

| # | Feature | Difficulty | Approach |
|---|---|---|---|
| 1 | Invernada conditional bounds | Medium | Custom `update_lp` comparing demand vs. inflow |
| 2 | Colbun Cota 425 switch | Medium | Step `bound_rule` at 1,047.9 hm3 (approximate) |
| 3 | 50cm rebalse | Medium | `bound_rule` step at VolAcumA50cmReb + cap |
| 4 | Boolean state flags | Hard | Integer variables (MIP) or pre-computation |
| 5 | Zone-transition hysteresis | Hard | Simulation-level state machine |
| 6 | Post-solve reserve redistribution | Hard | Custom post-solve callback |

### 9.4 Feature-by-Feature Priority Assessment

```
  Priority 1 (Core LP accuracy)         Priority 2 (Refinements)
  ─────────────────────────────          ────────────────────────────
  ■ Economy accumulators (Laja)          ■ Monthly fail_cost schedule
  ■ Usage costs (CUsoElect, CMixto)      ■ Bocatoma Canelon cost
  ■ La Invernada balance (Maule)         ■ Extraction point tracking
  ■ Lower-zone economy bounds            ■ Colbun Cota 425

  Priority 3 (PLP parity)               Not Recommended
  ─────────────────────────────          ────────────────────────────
  ■ 50cm rebalse rule                    ■ Boolean state flags (MIP)
  ■ Invernada conditional bounds         ■ Zone hysteresis (non-LP)
  ■ Storage/bypass costs                 ■ Post-solve redistribution
```

**Priority 1** items affect LP objective or constraint accuracy and
should be implemented first. **Priority 2** items are correctness
refinements with smaller numerical impact. **Priority 3** items bring
closer PLP parity but require C++ changes. **Not Recommended** items
represent PLP simulation-level logic that is inherently incompatible
with LP formulation; the practical impact on planning results is
expected to be small since they primarily affect rare zone-transition
scenarios.

---

## 10. Data File Locations

### PLP Configuration Files

| File | Content |
|------|---------|
| `support/plp_long_term/plplajam.dat.xz` | Laja agreement parameters |
| `support/plp_5_years/plplajam.dat.xz` | Laja (5-year horizon) |
| `support/plp_2_years/plplajam.dat.xz` | Laja (2-year horizon) |
| `support/plp_long_term/plpmaulen.dat.xz` | Maule agreement parameters |
| `support/plp_5_years/plpmaulen.dat.xz` | Maule (5-year horizon) |
| `support/plp_2_years/plpmaulen.dat.xz` | Maule (2-year horizon) |
| `support/plp_long_term/plpfilemb.dat.xz` | Seepage curves (3 reservoirs) |

### gtopt Converter Files

| File | Content |
|------|---------|
| `scripts/plp2gtopt/laja_parser.py` | PLP Laja config parser |
| `scripts/plp2gtopt/laja_writer.py` | Laja → gtopt JSON writer |
| `scripts/plp2gtopt/maule_parser.py` | PLP Maule config parser |
| `scripts/plp2gtopt/maule_writer.py` | Maule → gtopt JSON writer |
| `scripts/plp2gtopt/tests/test_laja.py` | Laja parser/writer tests (68) |
| `scripts/plp2gtopt/tests/test_maule.py` | Maule parser/writer tests (56) |

### Analysis Documents

| File | Content |
|------|---------|
| `docs/analysis/irrigation_agreements/plp_implementation.md` | Full PLP Fortran documentation |
| `docs/analysis/irrigation_agreements/right_junctions_analysis.md` | Right junction catalog |
| `docs/analysis/irrigation_agreements/seepage_and_colchones_analysis.md` | Seepage and volume zones |
| `docs/analysis/irrigation_agreements/laja_agreement_research.md` | Historical research |

### PLP Fortran Source

Authoritative source:
`https://github.com/marcelomatus/plp_storage/tree/main/CEN65/src`

### References

- Pereira-Bonvallet, E., Puschel-Lovengreen, S., Matus, M., and Moreno, R.
  (2016). "Optimizing Hydrothermal Scheduling with Non-Convex Irrigation
  Constraints." *Energy Procedia*, 87, pp. 132-138.
  DOI: [10.1016/j.egypro.2015.12.342](https://doi.org/10.1016/j.egypro.2015.12.342)

- Thesis: "Efecto del convenio de riego del sistema hidroelectrico Laja."
  Universidad de Chile.
  URL: http://repositorio.uchile.cl/handle/2250/139279

- Thesis: "Modelacion para el analisis de la interferencia operacional
  entre hidroelectricidad y riego en la cuenca del Maule."
  Universidad de Chile.
  URL: https://repositorio.uchile.cl/handle/2250/196765
