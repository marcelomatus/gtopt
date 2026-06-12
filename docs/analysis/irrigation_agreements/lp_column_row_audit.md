# LP Column & Row Audit: PLP vs gtopt Irrigation Agreements

> **Date**: 2026-04-02
> **Purpose**: Exhaustive comparison of every LP column (variable) and row
> (constraint) created by PLP Fortran and gtopt C++ for the Laja and Maule
> irrigation agreements.  Validates that all data from `plplajam.dat` and
> `plpmaulen.dat` is consumed.

---

## 1. Laja Agreement

### 1.1 PLP Block-Level Columns (per block `j`)

These are created in `GenPDLajaMBloA` (genpdlajam.f:4-124).

| PLP variable | Name pattern | Description | Unit | gtopt equivalent | Status |
|---|---|---|---|---|---|
| `IQDR` (1..NBlk) | `l_qdr_j` | Irrigation rights flow | m³/s | `frt_flow_{laja_irr_rights}` | **DONE** |
| `IQDE` (1..NBlk) | `l_qde_j` | Electrical rights flow | m³/s | `frt_flow_{laja_elec_rights}` | **DONE** |
| `IQDM` (1..NBlk) | `l_qdm_j` | Mixed rights flow | m³/s | `frt_flow_{laja_mixed_rights}` | **DONE** |
| `IQGA` (1..NBlk) | `l_qga_j` | Anticipated discharge flow | m³/s | `frt_flow_{laja_anticipated}` | **DONE** |
| `IQRI` (1..NBlk, per district) | `l_qri_j` | Irrigation withdrawal per district | m³/s | `frt_flow_{district_name_category}` | **DONE** |

**Total PLP block columns**: 4 × NBlk (rights) + NDistrict × NBlk (withdrawals)

**gtopt block columns**: Each FlowRight creates one `frt_flow_` column per block.
Additionally, FlowRight with `use_average=True` creates `frt_qeh_` (stage-average hourly)
which replaces PLP's explicit hourly accumulators.

### 1.2 PLP Stage-Level Columns

Created in `GenPDLajaMEtaA` (genpdlajam.f:179-313) and `GenPDLajaMEtaFO` (316-366).

| PLP variable | Name | Description | Unit | gtopt equivalent | Status |
|---|---|---|---|---|---|
| `IQDRH` | qdrh | Hourly irrigation accumulator | m³/s | `frt_qeh_{laja_irr_rights}` | **DONE** (FlowRight average) |
| `IQDEH` | qdeh | Hourly electrical accumulator | m³/s | `frt_qeh_{laja_elec_rights}` | **DONE** |
| `IQDMH` | qdmh | Hourly mixed accumulator | m³/s | `frt_qeh_{laja_mixed_rights}` | **DONE** |
| `IQGAH` | qgah | Hourly anticipated accumulator | m³/s | `frt_qeh_{laja_anticipated}` | **DONE** |
| `IQGTH` | qgth | Total generation hourly flow | m³/s | `frt_qeh_{laja_total_gen}` | **DONE** |
| `IVDRF` | ivdrf | Irrigation rights volume (state) | hm³ | `vrt_efin_{laja_vol_irr}` | **DONE** |
| `IVDEF` | ivdef | Electrical rights volume (state) | hm³ | `vrt_efin_{laja_vol_elec}` | **DONE** |
| `IVDMF` | ivdmf | Mixed rights volume (state) | hm³ | `vrt_efin_{laja_vol_mixed}` | **DONE** |
| `IVGAF` | ivgaf | Anticipated volume (state) | hm³ | `vrt_efin_{laja_vol_anticipated}` | **DONE** |
| `IQRS` | iqrs | Irrigation supply flow | m³/s | — | **MISSING** (1) |
| `IQPR` | iqpr | Primary irrigators flow | m³/s | — | **MISSING** (1) |
| `IQNR` | iqnr | New irrigators flow | m³/s | — | **MISSING** (1) |
| `IQER` | iqer | Emergency irrigators flow | m³/s | — | **MISSING** (1) |
| `IQSR` | iqsr | Saltos del Laja flow | m³/s | — | **MISSING** (1) |
| `IQLAJA` | iqlaja | Total Laja discharge | m³/s | — | **MISSING** (2) |
| `IQDEFM` | iqdefm | Minimum turbine flow (fixed) | m³/s | — | **MISSING** (3) |
| `IQHI` | iqhi | Intermediate basin flow (fixed) | m³/s | — | **MISSING** (3) |
| `IQRIH` | iqrih | District hourly irrigation (ZaCo) | m³/s | implicit in FlowRight | **PARTIAL** |
| `IQRFH` | iqrfh | District hourly irrigation (Tucapel) | m³/s | implicit in FlowRight | **PARTIAL** |
| `IQRDH` | iqrdh | District hourly irrigation (Saltos) | m³/s | implicit in FlowRight | **PARTIAL** |
| `IVESF` | ivesf | ENDESA economy volume (state) | hm³ | `vrt_efin_{laja_vol_econ_endesa}` | **DONE** |
| `IVERF` | iverf | Reserve economy volume (state) | hm³ | `vrt_efin_{laja_vol_econ_reserve}` | **DONE** |
| `IVAPF` | ivapf | Alto Polcura economy vol (state) | hm³ | `vrt_efin_{laja_vol_econ_polcura}` | **DONE** |
| `IVESN` | ivesn | ENDESA economy new saving | hm³ | `vrt_saving_{laja_vol_econ_endesa}` | **DONE** |
| `IVERN` | ivern | Reserve economy new saving | hm³ | `vrt_saving_{laja_vol_econ_reserve}` | **DONE** |
| `IVAPN` | ivapn | Alto Polcura economy new saving | hm³ | `vrt_saving_{laja_vol_econ_polcura}` | **DONE** |
| `IQGESH` | iqgesh | ENDESA economy spending | m³/s | `vrt_extraction_{laja_vol_econ_endesa}` | **DONE** |
| `IQGERH` | iqgerh | Reserve economy spending | m³/s | `vrt_extraction_{laja_vol_econ_reserve}` | **DONE** |
| `IQGAPH` | iqgaph | Alto Polcura economy spending | m³/s | `vrt_extraction_{laja_vol_econ_polcura}` | **DONE** |

**Notes**:
1. **MISSING: Irrigation supply partition** (IQRS/IQPR/IQNR/IQER/IQSR).  PLP decomposes
   the total irrigation right into sub-categories (primary 80%, new 20%, emergency, saltos)
   via a separate balance node `IQRS = IQPR + IQNR + IQER + IQSR`.  In gtopt, districts
   withdraw directly from the partition FlowRight — the intermediate supply partition is
   **not modeled**.  The district FlowRights already carry the correct demand via
   `discharge = base_demand × pct × seasonal`, so the functional allocation is preserved
   but the supply/demand decomposition constraint is absent.

2. **MISSING: Total Laja discharge identity** (`IQLAJA = IQGTH - filtration`).  In gtopt,
   the reservoir's physical extraction is handled by the hydro model; the FlowRight
   `laja_total_gen` represents the same quantity.  Filtration (seepage) is modeled via
   ReservoirSeepage in the hydro layer, not in the rights module.  The constraint is
   **implicitly satisfied** by the reservoir balance, not explicitly modeled.

3. **MISSING: Fixed variables** (IQDEFM, IQHI).  PLP creates explicit LP columns with
   `lowb = uppb = value` (fixed variables) for minimum turbine flow and intermediate
   basin flow.  In gtopt, these are captured as external flows or reservoir constraints
   in the hydro model, not in the rights module.

### 1.3 PLP Block-Level Rows (Constraints)

From `GenPDLajaMBloA`.

| PLP constraint | Equation | gtopt equivalent | Status |
|---|---|---|---|
| Flow partition | `l_qgt_j = l_qdr_j + l_qde_j + l_qdm_j + l_qga_j` | UserConstraint `laja_partition` | **DONE** |
| Hourly accumulation (×4) | `IQDRH += l_qdr_j × (BloDur/EtaDur)` | FlowRight `use_average=True` (internal) | **DONE** |
| Total gen accumulation | `IQGTH += l_qgt_j × (BloDur/EtaDur)` | FlowRight `use_average=True` (internal) | **DONE** |
| Irrigation assignment | `iqri_j = gen_extraction_j × fraction` | FlowRight discharge (district) | **PARTIAL** (4) |

**Note 4**: PLP assigns district withdrawals as fractions of the generation
extraction at each central.  In gtopt, district FlowRights have fixed `discharge`
schedules (base × pct × seasonal) rather than being proportional to actual
generation.  The result is the same when demand is deterministic, but PLP's
approach allows the optimizer to endogenously determine withdrawal proportions.

### 1.4 PLP Stage-Level Rows (Constraints)

From `GenPDLajaMEtaA`.

| PLP constraint | Equation | Type | gtopt equivalent | Status |
|---|---|---|---|---|
| Irrigation rights accum | `IVDRF = prev + IQDRH × (EtaDur/ScaleVol)` | Equality | VolumeRight `laja_vol_irr` balance | **DONE** |
| Electrical rights accum | `IVDEF = prev + IQDEH × (EtaDur/ScaleVol)` | Equality | VolumeRight `laja_vol_elec` balance | **DONE** |
| Mixed rights accum | `IVDMF = prev + IQDMH × (EtaDur/ScaleVol)` | Equality | VolumeRight `laja_vol_mixed` balance | **DONE** |
| Anticipated accum | `IVGAF = prev + IQGAH × (EtaDur/ScaleVol)` | Equality | VolumeRight `laja_vol_anticipated` balance | **DONE** |
| Irrigation supply balance | `IQRS - IQPR - IQNR - IQER - IQSR = 0` | Equality | — | **MISSING** (1) |
| Total Laja flow | `IQLAJA = IQGTH - filtration` | Equality | — | **MISSING** (2) |
| Max daily irrigation | `IQDRT ≤ QMaxDia` (after initial stages) | ≤ | FlowRight `fmax` | **DONE** |
| District hourly accum (×3) | `IQRIH = Σ l_qri_j × BloDur/EtaDur` | Equality | FlowRight averaging | **DONE** |
| ENDESA economy balance | `IVESF = prev + IVESN - IQGESH×dt` | Equality | VolumeRight `laja_vol_econ_endesa` balance | **DONE** |
| Reserve economy balance | `IVERF = prev + IVERN - IQGERH×dt` | Equality | VolumeRight `laja_vol_econ_reserve` balance | **DONE** |
| Polcura economy balance | `IVAPF = prev + IVAPN - IQGAPH×dt` | Equality | VolumeRight `laja_vol_econ_polcura` balance | **DONE** |

### 1.5 PLP Objective Function Coefficients

From `GenPDLajaMBloFO` and `GenPDLajaMEtaFO`.

| PLP cost term | Variable | Formula | gtopt equivalent | Status |
|---|---|---|---|---|
| Irrigation usage | `l_qdr_j` | `CQVar(IQDR) × BloDur/FPhi` | FlowRight `use_value` (cost_irr_uso) | **DONE** (if > 0) |
| Electrical usage | `l_qde_j` | `CQVar(IQDE) × BloDur/FPhi` | FlowRight `use_value` (cost_elec_uso) | **DONE** (if > 0) |
| Mixed usage | `l_qdm_j` | `CQVar(IQDM) × BloDur/FPhi` | FlowRight `use_value` (cost_mixed) | **DONE** (if > 0) |
| Anticipated usage | `l_qga_j` | `CQVar(IQGA) × BloDur/FPhi` | FlowRight `use_value` (on anticipated) | **MISSING** (5) |
| Irrigation non-served | district fail vars | `CRiegoNS × FCau × FRiegoCost(Idx)` | FlowRight `fail_cost` × cost_factor | **DONE** |
| Electrical non-served | IQDEH fail | `CElectNS × monthly` | FlowRight `fail_cost` (cost_elec_ns) | **DONE** |

**Note 5**: The anticipated discharge FlowRight has a `fail_cost` (from
`monthly_cost_anticipated`) but no `use_value`.  PLP applies `CQVar(IQGA)`
as a usage cost.  If this cost is zero in practice (common), the omission
has no LP impact.

### 1.6 PLP Bounds Set in `FijaLajaM` (per-stage dynamic)

| PLP bound | Variable | Formula | gtopt equivalent | Status |
|---|---|---|---|---|
| Max irrigation hourly | `IQDRH` | `CaudalMaxRiego × FactMenMaxRiego(Mes)` | FlowRight `fmax` (qmax × usage) | **DONE** |
| Max electrical hourly | `IQDEH` | `CaudalMaxElect × FactMenMaxElect(Mes)` | FlowRight `fmax` (qmax × usage) | **DONE** |
| Max mixed hourly | `IQDMH` | `CaudalMaxMixto × FactMenMaxMixto(Mes)` | FlowRight `fmax` (qmax × usage) | **DONE** |
| Max anticipated hourly | `IQGAH` | `CaudalMaxAntic × FactMenMaxAntic(Mes)` | FlowRight `fmax` (qmax × usage) | **DONE** |
| Fixed total gen flow | `IQGTH` | `CaudalToro(IEta)` (first N stages) | — | **MISSING** (6) |
| Reservoir-level cap | all rights | `min(fmax, bound_rule(volume))` | VolumeRight `bound_rule` (on extraction) | **DONE** |
| INICIOTEMP reset | `IVDRF_F` etc. | `DerRiego = Base + Σ(Factor × Zone_Vol)` | VolumeRight `reset_month` + `bound_rule` | **DONE** |
| INICIOANTIC reset | `IVGAF_F` | `IVGAF = 0` | VolumeRight `reset_month` (april) | **DONE** |
| Fixed district flows | `IQPR`, etc. | `lowb = uppb = QPRiego` | FlowRight `discharge` (fixed schedule) | **PARTIAL** (7) |
| Min turbine flow | gen vars | `CauExtrMin` (from Laja1) | — | **MISSING** (8) |
| Max turbine flow | gen vars | `CauExtrMax` (from Laja1) | — | **MISSING** (8) |

**Note 6**: `CaudalToro` fixes the total generation flow for the first N stages
(historical/planned data).  gtopt has no equivalent — the FlowRight is always
free within its bounds.  This only affects initial stages (typically the first
few months of a planning run) where actual turbine schedules are known.

**Note 7**: PLP fixes district irrigation flows as exact values (`lowb = uppb`)
computed by `GetQsLajaM`.  In gtopt, districts have fixed `discharge` schedules
computed from `base_demand × pct × seasonal_factor`.  The values should match
when the PLP monthly factors and base demands are correctly transferred.

**Note 8**: `Laja1` computes min/max turbine bounds from a complex function
of remaining rights, reservoir volume, filtration, and intermediate basin
flows.  These bounds are applied to the generation central in PLP via
`ModifUpp`/`ModifLow` in `Laja1Dual`.  In gtopt, these are partially captured
by the VolumeRight `bound_rule` (volume-dependent extraction cap), but the
dynamic min bound (ensuring irrigation demand is met) and the cross-check
against generation capacity are **not modeled**.

### 1.7 PLP State Variables (SDDP Cut Variables)

From `IsVarCorteLajaM` (genpdlajam.f:1019-1052).

| PLP state var | Reset behavior | gtopt state var | Status |
|---|---|---|---|
| `IVDRF` | State at INICIOTEMP; reset to bound_rule value | `vrt_sini_{laja_vol_irr}` + `skip_state_link` | **DONE** |
| `IVDEF` | State at INICIOTEMP; reset | `vrt_sini_{laja_vol_elec}` + `skip_state_link` | **DONE** |
| `IVDMF` | State at INICIOTEMP; reset | `vrt_sini_{laja_vol_mixed}` + `skip_state_link` | **DONE** |
| `IVGAF` | State at INICIOANTIC; reset to 0 | `vrt_sini_{laja_vol_anticipated}` + `skip_state_link` | **DONE** |
| `IVESF` | Continuous (no reset) | `vrt_sini_{laja_vol_econ_endesa}` | **DONE** |
| `IVERF` | PLP: reset on cushion exit; gtopt: no reset | `vrt_sini_{laja_vol_econ_reserve}` | **SIMPLIFIED** |
| `IVAPF` | Continuous (no reset) | `vrt_sini_{laja_vol_econ_polcura}` | **DONE** |

### 1.8 Data Usage Validation: `plplajam.dat`

Every field parsed by `LajaParser` → checked against `LajaWriter` usage.

| Parser config key | Used by writer? | How used | Notes |
|---|---|---|---|
| `central_laja` | **YES** | Reservoir ref for bound_rule, VolumeRight | |
| `intermediate_basins` | **NO** | — | Used by hydro model, not rights module |
| `vol_max` | **YES** | FlowRight `laja_total_gen` fmax | |
| `zone_widths` | **YES** | `_zones_to_bound_rule_segments()` | |
| `irr_base` | **YES** | bound_rule segments | |
| `irr_factors` | **YES** | bound_rule segments | |
| `elec_base` | **YES** | bound_rule segments | |
| `elec_factors` | **YES** | bound_rule segments | |
| `mixed_base` | **YES** | bound_rule segments | |
| `mixed_factors` | **YES** | bound_rule segments | |
| `max_irr` | **YES** | VolumeRight emax + bound_rule cap | |
| `max_elec` | **YES** | VolumeRight emax + bound_rule cap | |
| `max_mixed` | **YES** | VolumeRight emax + bound_rule cap | |
| `max_anticipated` | **YES** | VolumeRight emax + bound_rule cap | |
| `mes_inicio_riego` | **NO** | — | PLP uses to determine INICIOTEMP; gtopt uses `reset_month: april` (hardcoded) |
| `mes_inicio_anticipos` | **NO** | — | PLP uses for INICIOANTIC; gtopt uses `reset_month: april` |
| `qmax_irr` | **YES** | FlowRight fmax (× monthly usage) | |
| `qmax_elec` | **YES** | FlowRight fmax (× monthly usage) | Also used as `saving_rate` for economies |
| `qmax_mixed` | **YES** | FlowRight fmax (× monthly usage) | |
| `qmax_anticipated` | **YES** | FlowRight fmax (× monthly usage) | |
| `cost_irr_ns` | **YES** | FlowRight fail_cost | |
| `cost_irr_uso` | **YES** | FlowRight use_value (if > 0) | |
| `cost_elec_ns` | **YES** | FlowRight fail_cost | |
| `cost_elec_uso` | **YES** | FlowRight use_value (if > 0) | |
| `cost_mixed` | **YES** | FlowRight use_value (if > 0) | |
| `monthly_cost_irr_ns` | **YES** | FlowRight fail_cost modulation | |
| `monthly_cost_irr` | **NO** | — | **(GAP)**: Irrigation rights monthly cost factor not applied |
| `monthly_cost_elec` | **YES** | FlowRight fail_cost modulation | |
| `monthly_cost_mixed` | **NO** | — | **(GAP)**: Mixed rights monthly cost factor not applied |
| `monthly_cost_anticipated` | **YES** | FlowRight fail_cost modulation | |
| `monthly_usage_irr` | **YES** | fmax schedule | |
| `monthly_usage_elec` | **YES** | fmax schedule | |
| `monthly_usage_mixed` | **YES** | fmax schedule | |
| `monthly_usage_anticipated` | **YES** | fmax schedule | |
| `ini_irr` | **YES** | VolumeRight eini | |
| `ini_elec` | **YES** | VolumeRight eini | |
| `ini_mixed` | **YES** | VolumeRight eini | |
| `ini_anticipated` | **YES** | VolumeRight eini | |
| `districts` | **YES** | FlowRight per district/category | |
| `filtration` | **NO** | — | Handled by hydro model (ReservoirSeepage) |
| `demand_1o_reg` | **YES** | District discharge computation | |
| `demand_2o_reg` | **YES** | District discharge computation | |
| `demand_emergencia` | **YES** | District discharge computation | |
| `demand_saltos` | **YES** | District discharge computation | |
| `seasonal_1o_reg` | **YES** | District discharge modulation | |
| `seasonal_2o_reg` | **YES** | District discharge modulation | |
| `seasonal_emergencia` | **YES** | District discharge modulation | |
| `seasonal_saltos` | **YES** | District discharge modulation | |
| `vol_muerto` | **YES** | bound_rule segment start volume | |
| `manual_withdrawals` | **NO** | — | **(GAP)**: PLP uses for first N stages with known flows |
| `forced_flows` | **NO** | — | **(GAP)**: PLP's `CaudalToro` (fixed gen flows) |
| `ini_econ_endesa` | **YES** | VolumeRight eini (economy) | |
| `ini_econ_reserve` | **YES** | VolumeRight eini (economy) | |
| `ini_econ_polcura` | **YES** | VolumeRight eini (economy) | |

**Laja data coverage**: 43/51 config keys used (**84%**).
Missing keys are either handled by the hydro model (`intermediate_basins`,
`filtration`), represent PLP-specific stage-type logic (`mes_inicio_riego`,
`mes_inicio_anticipos`), or are gaps to address (`monthly_cost_irr`,
`monthly_cost_mixed`, `manual_withdrawals`, `forced_flows`).

---

## 2. Maule Agreement

### 2.1 PLP Block-Level Columns (per block `j`)

Created in `GenPDMauleBloA` (genpdmaule.f:3-157).

| PLP variable | Name pattern | Description | Unit | gtopt equivalent | Status |
|---|---|---|---|---|---|
| `IQMNE` (1..NBlk) | `m_qmne_j` | Normal electric flow | m³/s | `frt_flow_{maule_elec_normal}` | **DONE** |
| `IQMNR` (1..NBlk) | `m_qmnr_j` | Normal irrigation flow | m³/s | `frt_flow_{maule_irr_normal}` | **DONE** |
| `IQMOE` (1..NBlk) | `m_qmoe_j` | Ordinary reserve electric | m³/s | `frt_flow_{maule_elec_ordinary}` | **DONE** |
| `IQMOR` (1..NBlk) | `m_qmor_j` | Ordinary reserve irrigation | m³/s | `frt_flow_{maule_irr_ordinary}` | **DONE** |
| `IQMCE` (1..NBlk) | `m_qmce_j` | ENDESA compensation | m³/s | `frt_flow_{maule_compensation}` | **DONE** |
| `IQMEI` (1..NBlk) | `m_qmei_j` | Extraordinary reserve flow | m³/s | — | **MISSING** (9) |
| `IQIDN` (1..NBlk) | `m_qidn_j` | Invernada deficit discharge | m³/s | `frt_flow_{invernada_deficit}` | **DONE** |
| `IQISD` (1..NBlk) | `m_qisd_j` | Invernada no-deficit storage | m³/s | `frt_flow_{invernada_no_deficit}` | **DONE** |
| `IQTER` (1..NBlk) | `m_qter_j` | Irrigation delivery total | m³/s | — | **MISSING** (10) |
| `IQCANELON` (1..NBlk) | `m_qcan_j` | Bocatoma Canelon intake | m³/s | `frt_flow_{bocatoma_canelon}` | **DONE** |
| `IQRI` (1..NBlk, per district) | `m_qri_j` | District withdrawal flow | m³/s | `frt_flow_{district_name}` | **DONE** |

**Note 9**: `IQMEI` is the extraordinary reserve extraction flow.  When the
reservoir is in the extraordinary zone, both electric and irrigation
extractions are tracked through separate accumulators (VMDOEF/VMDORF) but
the block-level flow `IQMEI` is a single combined variable.  In gtopt, the
extraordinary reserve VolumeRights (`maule_vol_rext_elec`, `maule_vol_rext_riego`)
exist as accumulators but there is no explicit block-level FlowRight for
extraordinary zone extraction.  This flow is **only active** when the
reservoir is in the extraordinary zone, which is rare in practice.

**Note 10**: `IQTER` is the total irrigation delivery flow (sum of all
district withdrawals).  In gtopt, this is implicit — the sum of all district
FlowRight flows.  The total is not needed as an explicit variable because
district constraints reference `maule_irr_normal` directly.

### 2.2 PLP Stage-Level Columns

Created in `GenPDMauleEtaA` (genpdmaule.f:210-488).

| PLP variable | Name | Description | Unit | gtopt equivalent | Status |
|---|---|---|---|---|---|
| `VMGEMF` | vmgemf | Monthly electric accumulator (state) | hm³ | `vrt_efin_{maule_vol_elec_monthly}` | **DONE** |
| `VMGEAF` | vmgeaf | Annual electric accumulator (state) | hm³ | `vrt_efin_{maule_vol_elec_annual}` | **DONE** |
| `VMGRTF` | vmgrtf | Seasonal irrigation accumulator (state) | hm³ | `vrt_efin_{maule_vol_irr_seasonal}` | **DONE** |
| `VMGOEF` | vmgoef | Ordinary reserve electric accum (state) | hm³ | — | **MISSING** (11) |
| `VMGORF` | vmgorf | Ordinary reserve irrigation accum (state) | hm³ | — | **MISSING** (11) |
| `VMDOEF` | vmdoef | Extraordinary reserve electric accum (state) | hm³ | `vrt_efin_{maule_vol_rext_elec}` | **DONE** |
| `VMDORF` | vmdorf | Extraordinary reserve irrigation accum (state) | hm³ | `vrt_efin_{maule_vol_rext_riego}` | **DONE** |
| `VMDCEF` | vmdcef | Compensation accumulator (state) | hm³ | `vrt_efin_{maule_vol_compensation}` | **DONE** |
| `VMDEIF` | vmdeif | Invernada economy volume (state) | hm³ | `vrt_efin_{maule_vol_econ_invernada}` | **DONE** |
| `VMDCEN` | vmdcen | Compensation new (fixed) | hm³ | — | **MISSING** (12) |
| `VMDEIN` | vmdein | Invernada economy initial (fixed) | hm³ | VolumeRight eini | **DONE** |
| `VMDEMT` | vmdemt | Electric monthly threshold (fixed) | hm³ | VolumeRight emax | **DONE** |
| `VMDEAT` | vmdeat | Electric annual threshold (fixed) | hm³ | VolumeRight emax | **DONE** |
| `VMDRTT` | vmdrtt | Irrigation seasonal threshold (fixed) | hm³ | VolumeRight emax | **DONE** |
| `VMUTIL` | vmutil | Useful volume (Colbun - reserves) | hm³ | — | **MISSING** (13) |
| `VMREB` | vmreb | Rebasing volume | hm³ | — | **MISSING** (13) |
| `IQMNEH` | qmneh | Normal electric hourly accum | m³/s | `frt_qeh_{maule_elec_normal}` | **DONE** |
| `IQMNRH` | qmnrh | Normal irrigation hourly accum | m³/s | `frt_qeh_{maule_irr_normal}` | **DONE** |
| `IQMOEH` | qmoeh | Ordinary reserve electric hourly | m³/s | `frt_qeh_{maule_elec_ordinary}` | **DONE** |
| `IQMORH` | qmorh | Ordinary reserve irrigation hourly | m³/s | `frt_qeh_{maule_irr_ordinary}` | **DONE** |
| `IQMCEH` | qmceh | Compensation hourly accum | m³/s | `frt_qeh_{maule_compensation}` | **DONE** |
| `IQMEIH` | qmeih | Extraordinary hourly accum | m³/s | — | **MISSING** (9) |
| `IQIDNH` | qidnh | Invernada deficit hourly | m³/s | `frt_qeh_{invernada_deficit}` | **DONE** |
| `IQISDH` | qisdh | Invernada no-deficit hourly | m³/s | `frt_qeh_{invernada_no_deficit}` | **DONE** |
| `IQTERH` | qterh | Total irrigation delivery hourly | m³/s | — | **MISSING** (10) |
| `QDRMH` | qdrmh | Maule delivery flow (fixed) | m³/s | — | **MISSING** (14) |
| `QDRAH` | qdrah | Irrigation non-served flow | m³/s | FlowRight fail variable | **DONE** |
| `IQHI` | qhi | Intermediate basin flow (fixed) | m³/s | — | **MISSING** (3) |
| `QARMR` | qarmr | Armerillo reconstruction flow | m³/s | — | **MISSING** (15) |
| `QMAULEH` | qmauleh | Maule total inflow hourly | m³/s | — | **MISSING** (3) |
| `QINVERH` | qinverh | Invernada total inflow hourly | m³/s | — | **MISSING** (3) |
| `QR105` | qr105 | Resolution 105 flow (fixed) | m³/s | `frt_flow_{maule_res105}` (as discharge) | **DONE** |
| `QA105` | qa105 | Res 105 non-served | m³/s | FlowRight fail variable | **DONE** |
| `QNINV` | qninv | Invernada natural inflow | m³/s | `frt_flow_{invernada_natural_inflow}` | **DONE** |
| `QHINV` | qhinv | Invernada storage (to reservoir) | m³/s | `frt_flow_{invernada_storage}` | **DONE** |
| `QHNEIN` | qhnein | Invernada bypass (not stored) | m³/s | `frt_flow_{invernada_bypass}` | **DONE** |
| `QHEIN` | qhein | Invernada total to reservoir | m³/s | — | **MISSING** (16) |

**Note 11**: `VMGOEF`/`VMGORF` track ordinary reserve extraction separately.
In PLP, these are used to enforce the percentage allocation constraint
(20%/80% split) on an accumulated-volume basis.  In gtopt, the percentage
constraint is enforced per-block via UserConstraint `maule_ord_elec_pct`/
`maule_ord_irr_pct`, which is a tighter enforcement (per-block vs accumulated).

**Note 12**: `VMDCEN` is a fixed variable representing the new compensation
amount for the current stage.  In gtopt, this is implicit in the VolumeRight
balance.

**Note 13**: `VMUTIL` and `VMREB` are auxiliary volume accounting variables
used by PLP for internal zone tracking.  Not needed in gtopt where zone
logic is handled by `bound_rule`.

**Note 14**: `QDRMH` is the Maule delivery flow, fixed to the computed
demand value.  In gtopt, irrigation demand is represented via FlowRight
`discharge` schedules.

**Note 15**: `QARMR` is the Armerillo flow reconstruction — the flow at
the DGA fluviometric station downstream of the Maule-Melado confluence.
It is an accounting identity, not a decision variable.  PLP uses it for
verification and reporting; gtopt does not need it for optimization.

**Note 16**: `QHEIN` is total Invernada flow to reservoir.  In gtopt,
this is captured by the Invernada balance UserConstraint.

### 2.3 PLP Block-Level Rows (Maule)

From `GenPDMauleBloA`.

| PLP constraint | Equation | gtopt equivalent | Status |
|---|---|---|---|
| Flow partition (per central) | `qg_j = qmne_j + qmnr_j + qmoe_j + qmor_j + qmce_j + qmei_j` | — | **MISSING** (17) |
| Invernada balance | `qidn_j + qisd_j + qninv_j = qhein_j` | UserConstraint `invernada_balance` | **DONE** |
| Hourly accumulation (×N) | averaging constraints | FlowRight `use_average=True` | **DONE** |
| District withdrawal assignment | `qri_j = fraction × qter_j` | UserConstraint `dist_*` | **DONE** |

**Note 17**: PLP partitions each central's turbine flow into rights
categories.  The flow partition is per-central (Maule, Cipreses, Pehuenche,
Colbun).  In gtopt, the FlowRights are global (not per-central) — they
represent total rights across all centrals.  The partitioning is not
modeled explicitly because gtopt tracks the aggregate rights balance.
This is a **structural simplification**: PLP can distribute extraction
across multiple centrals; gtopt treats it as a single aggregated flow.

### 2.4 PLP Stage-Level Rows (Maule)

From `GenPDMauleEtaA`.

| PLP constraint | Equation | Type | gtopt equivalent | Status |
|---|---|---|---|---|
| Monthly elec accum | `VMGEMF = prev + IQMNEH×dt/scale` | Eq | VolumeRight balance | **DONE** |
| Annual elec accum | `VMGEAF = prev + IQMNEH×dt/scale` | Eq | VolumeRight balance | **DONE** |
| Seasonal irr accum | `VMGRTF = prev + IQMNRH×dt/scale` | Eq | VolumeRight balance | **DONE** |
| Ordinary elec accum | `VMGOEF = prev + IQMOEH×dt/scale` | Eq | — | **MISSING** (11) |
| Ordinary irr accum | `VMGORF = prev + IQMORH×dt/scale` | Eq | — | **MISSING** (11) |
| Extraordinary elec accum | `VMDOEF = prev + IQMEIH×pct×dt` | Eq | VolumeRight balance | **DONE** |
| Extraordinary irr accum | `VMDORF = prev + IQMEIH×pct×dt` | Eq | VolumeRight balance | **DONE** |
| Compensation accum | `VMDCEF = prev + IQMCEH×dt/scale` | Eq | VolumeRight balance | **DONE** |
| Economy accum | `VMDEIF = prev + saving - extraction` | Eq | VolumeRight balance (with saving) | **DONE** |
| Delivery constraint | `-QDRAH + QDRMH + IQINVH ≤ 0` | ≤ | — | **MISSING** (14) |
| Armerillo balance | `QARMR = Σ centrals - electric` | Eq | — | **MISSING** (15) |
| % allocation ordinary | `IQMOEH / (IQMOEH + IQMORH) ≤ pct` | ≤ | UserConstraint `maule_ord_elec_pct` | **DONE** |
| % allocation irrigation | `IQMORH / (IQMOEH + IQMORH) ≤ pct` | ≤ | UserConstraint `maule_ord_irr_pct` | **DONE** |
| Res 105 minimum | `QR105 = QRiego105(mes)` | Fixed | FlowRight `discharge` | **DONE** |
| District proportional | `qri = pct × qter` | Eq/≤ | UserConstraint `dist_*` | **DONE** |

### 2.5 PLP Objective Function Coefficients (Maule)

From `GenPDMauleEtaFO` (genpdmaule.f:491-551).

| PLP cost term | Variable | Formula | gtopt equivalent | Status |
|---|---|---|---|---|
| Irrigation value (Res105) | `qr105` | `-FCau × ValorRiego105` | FlowRight `use_value` | **DONE** |
| Irrigation value (Maule) | `qterh` | `-FCau × ValorRiegoMaule` | FlowRight `use_value` | **DONE** |
| Irrigation non-served (Maule) | `qdrah` | `FCau × CostoRiegoNSMaule` | FlowRight `fail_cost` | **DONE** |
| Irrigation non-served (Res105) | `qa105` | `FCau × CostoRiegoNS105` | FlowRight `fail_cost` | **DONE** |
| Economy storage cost | `vmdeif` | `FVol × EconInvernCosto` | FlowRight `use_value` on invernada_storage | **DONE** |
| Invernada storage cost | `qhinv` | `FCau × CostoEmbalsar` | — | **MISSING** (18) |
| Invernada no-storage cost | `qhnein` | `FCau × CostoNoEmbalsar` | — | **MISSING** (18) |
| Canelon civil cost | `qcanelon` | `FCau × CostoCanelon` | FlowRight `use_value` | **DONE** |
| Penalty 1 (electric excess) | gen excess | `Penalizador_1` | FlowRight `fail_cost` | **DONE** |
| Penalty 2 (irrigation excess) | irr excess | `Penalizador_2` | — | **PARTIAL** |
| District non-served | district fail | `FCau × CostoRiegoNS*` | FlowRight `fail_cost` | **DONE** |

**Note 18**: `CostoEmbalsar` and `CostoNoEmbalsar` are costs applied to
the Invernada storage and bypass FlowRights respectively.  The writer
only applies `econ_inver_costo` as `use_value` on `invernada_storage`
but does not set costs on `invernada_bypass`.

### 2.6 PLP Bounds Set in `FijaMaule` (per-stage dynamic)

| PLP bound | Variable | Condition | gtopt equivalent | Status |
|---|---|---|---|---|
| Normal electric max | `IQMNEH` | `GastoElecDiaMax × Mod(Mes)` | FlowRight `fmax` | **DONE** |
| Normal electric: zone gate | `IQMNEH` | 0 if not in normal zone | FlowRight `bound_rule` | **DONE** |
| Normal irrigation max | `IQMNRH` | `PRiegoMaule(Mes) × GastoRiegoMax` | FlowRight `fmax` schedule | **DONE** |
| Normal irrigation: zone gate | `IQMNRH` | 0 if not in normal zone | FlowRight `bound_rule` | **DONE** |
| Ordinary elec max | `IQMOEH` | 0 if not in ordinary zone | FlowRight `bound_rule` | **DONE** |
| Ordinary irr max | `IQMORH` | 0 if not in ordinary zone | FlowRight `bound_rule` | **DONE** |
| Compensation max | `IQMCEH` | `GastoElecDiaMax` | FlowRight `fmax` | **DONE** |
| Extraordinary max | `IQMEIH` | 0 if not in extraordinary zone | — | **MISSING** (9) |
| Invernada deficit | `IQIDNH` | based on deficit/surplus logic | FlowRight fmax | **PARTIAL** |
| Invernada no-deficit | `IQISDH` | based on deficit/surplus logic | FlowRight fmax | **PARTIAL** |
| Monthly elec reset | `VMGEMF` | Reset to 0 at month boundary | VolumeRight `reset_month: january` | **DONE** |
| Annual elec reset | `VMGEAF` | Reset to 0 at June boundary | VolumeRight `reset_month: june` | **DONE** |
| Seasonal irr reset | `VMGRTF` | Reset to 0 at June boundary | VolumeRight `reset_month: june` | **DONE** |
| Colbun 425 extraction | extraction vars | `ExtrMax425` if vol < Vol425 | — | **MISSING** (19) |

**Note 19**: PLP imposes additional extraction limits when Colbun volume
falls below elevation 425 (Vol425).  This is a volume-conditional bound
that is not modeled in gtopt.

### 2.7 PLP State Variables (Maule)

| PLP state var | Reset behavior | gtopt state var | Status |
|---|---|---|---|
| `VMGEMF` | Reset monthly (January) | `vrt_sini_{maule_vol_elec_monthly}` | **DONE** |
| `VMGEAF` | Reset yearly (June) | `vrt_sini_{maule_vol_elec_annual}` | **DONE** |
| `VMGRTF` | Reset yearly (June) | `vrt_sini_{maule_vol_irr_seasonal}` | **DONE** |
| `VMGOEF` | Regime-dependent | — | **MISSING** (11) |
| `VMGORF` | Regime-dependent | — | **MISSING** (11) |
| `VMDOEF` | Continuous | `vrt_sini_{maule_vol_rext_elec}` | **DONE** |
| `VMDORF` | Continuous | `vrt_sini_{maule_vol_rext_riego}` | **DONE** |
| `VMDCEF` | Continuous | `vrt_sini_{maule_vol_compensation}` | **DONE** |
| `VMDEIF` | Continuous | `vrt_sini_{maule_vol_econ_invernada}` | **DONE** |

### 2.8 Data Usage Validation: `plpmaulen.dat`

Every field parsed by `MauleParser` → checked against `MauleWriter` usage.

| Parser config key | Used by writer? | How used | Notes |
|---|---|---|---|
| `central_maule` | **NO** | — | Only used in template rendering |
| `central_invernada` | **YES** | Economy VolumeRight reservoir ref | |
| `central_melado` | **NO** | — | Only in template |
| `central_colbun` | **YES** | bound_rule reservoir for all FlowRights | |
| `intermediate_basins` | **NO** | — | Used by hydro model |
| `v_util_min` | **NO** | — | **(GAP)**: PLP uses for min useful volume |
| `v_reserva_extraord` | **YES** | Zone threshold for bound_rule segments | |
| `v_reserva_ordinaria` | **YES** | Zone threshold for bound_rule segments | |
| `v_der_riego_temp_max` | **YES** | VolumeRight emax | |
| `v_der_elect_anu_max` | **YES** | VolumeRight emax | |
| `v_comp_elec_max` | **YES** | VolumeRight emax | |
| `v_gasto_elec_men_ini` | **YES** | VolumeRight eini | |
| `v_gasto_elec_anu_ini` | **YES** | VolumeRight eini | |
| `v_gasto_riego_ini` | **YES** | VolumeRight eini | |
| `v_gasto_rext_elec_ini` | **YES** | VolumeRight eini | |
| `v_gasto_rext_riego_ini` | **YES** | VolumeRight eini | |
| `v_der_rext_elec_ini` | **NO** | — | **(GAP)**: Initial ext reserve electric rights |
| `v_der_rext_riego_ini` | **NO** | — | **(GAP)**: Initial ext reserve irrigation rights |
| `v_comp_elec_ini` | **YES** | VolumeRight eini | |
| `v_econ_inver_ini` | **YES** | VolumeRight eini | |
| `gasto_elec_men_max` | **YES** | VolumeRight emax (monthly elec) | |
| `gasto_elec_dia_max` | **YES** | FlowRight fmax | |
| `mod_elec_reserva` | **YES** | Monthly fmax schedule | |
| `descuenta_elec_armerillo` | **NO** | — | **(GAP)**: Armerillo balance not modeled |
| `gasto_riego_max` | **YES** | FlowRight fmax | |
| `caudal_min_embalsa` | **NO** | — | **(GAP)**: Min Maule flow for storage |
| `flag_embalsa_1` | **NO** | — | PLP storage logic flags |
| `flag_embalsa_2` | **NO** | — | PLP storage logic flags |
| `valor_riego_maule` | **YES** | FlowRight use_value | |
| `valor_riego_res105` | **YES** | FlowRight use_value | |
| `costo_riego_ns_maule` | **YES** | FlowRight fail_cost | |
| `costo_riego_ns_res105` | **YES** | FlowRight fail_cost | |
| `pct_riego_mensual` | **YES** | FlowRight fmax schedule | |
| `n_agnos_pct_manual` | **NO** | — | **(GAP)**: Multi-year overrides |
| `pct_riego_manual` | **NO** | — | **(GAP)**: Multi-year overrides |
| `ano_mod_auto` | **NO** | — | **(GAP)**: Auto-modulation year |
| `factor_caudales_futuros` | **NO** | — | **(GAP)**: Future flow modulation |
| `mod_auto_res105` | **NO** | — | **(GAP)**: Res105 auto-modulation |
| `vol_acum_riego_temp` | **NO** | — | **(GAP)**: Accumulated irrigation caps |
| `dias_acum_temp` | **NO** | — | **(GAP)**: Season day accumulation |
| `pct_elec_reserva` | **YES** | UserConstraint % allocation | |
| `pct_riego_reserva` | **YES** | UserConstraint % allocation | |
| `caudal_res105` | **YES** | FlowRight discharge | |
| `n_agnos_res105_manual` | **NO** | — | **(GAP)**: Multi-year overrides |
| `caudal_res105_manual` | **NO** | — | **(GAP)**: Multi-year overrides |
| `districts` | **YES** | FlowRight + UserConstraint | |
| `bocatoma_canelon` | **YES** | FlowRight name | |
| `costo_canelon` | **YES** | FlowRight use_value | |
| `idx_extrac_colbun_425` | **NO** | — | **(GAP)**: Colbun 425 extraction |
| `v_cota_425` | **NO** | — | **(GAP)**: Colbun 425 volume |
| `econ_inver_uso_reserva` | **NO** | — | **(GAP)**: Economy in reserve flag |
| `econ_inver_acumulables` | **NO** | — | **(GAP)**: Accumulation flag |
| `econ_inver_costo` | **YES** | FlowRight use_value | |
| `penalizador_1` | **YES** | FlowRight fail_cost | |
| `penalizador_2` | **YES** | — | In template only |

**Maule data coverage**: 31/55 config keys used (**56%**).
The unused keys fall into several categories:
- Hydro model concerns (`intermediate_basins`, `central_maule`, `central_melado`)
- Multi-year override tables (`n_agnos_pct_manual`, `pct_riego_manual`, etc.)
- Auto-modulation logic (`ano_mod_auto`, `factor_caudales_futuros`, `mod_auto_res105`)
- Accumulated volume caps (`vol_acum_riego_temp`, `dias_acum_temp`)
- PLP-specific flags (`flag_embalsa_*`, `descuenta_elec_armerillo`)
- Colbun 425 extraction limit (`idx_extrac_colbun_425`, `v_cota_425`)
- Economy behavioral flags (`econ_inver_uso_reserva`, `econ_inver_acumulables`)

---

## 3. Summary: Coverage Assessment

### 3.1 Column (Variable) Coverage

| Category | PLP vars | gtopt vars | Coverage |
|---|---|---|---|
| **Laja block flows** | 4 types | 4 FlowRights | **100%** |
| **Laja stage accumulators** | 4 rights + 3 economy | 4 VolumeRights + 3 economy | **100%** |
| **Laja hourly accumulators** | 5 (qdrh..qgth) | 5 (FlowRight averaging) | **100%** |
| **Laja supply partition** | 5 (iqrs/pr/nr/er/sr) | 0 | **0%** — modeled differently |
| **Laja auxiliary** | 3 (iqlaja, iqdefm, iqhi) | 0 | **0%** — handled by hydro model |
| **Laja districts** | N × NBlk | N × NBlk (FlowRights) | **100%** |
| **Maule block flows** | 9 types | 7 FlowRights | **78%** (IQMEI, IQTER missing) |
| **Maule stage accumulators** | 9 state vars | 7 VolumeRights | **78%** (VMGOEF, VMGORF missing) |
| **Maule hourly accumulators** | ~15 | ~10 (FlowRight averaging) | **67%** |
| **Maule auxiliary** | ~8 (vmutil, vmreb, etc.) | 0 | **0%** — handled differently |
| **Maule Invernada** | 6 flows | 5 FlowRights | **83%** (QHEIN missing) |
| **Maule districts** | N × NBlk | N × NBlk | **100%** |

### 3.2 Row (Constraint) Coverage

| Category | PLP rows | gtopt rows | Coverage |
|---|---|---|---|
| **Laja flow partition** | 1 (per block) | 1 UserConstraint | **100%** |
| **Laja volume balances** | 4 + 3 economy | 4 + 3 VolumeRight balances | **100%** |
| **Laja supply partition** | 1 | 0 | **0%** |
| **Laja total discharge** | 1 | 0 (hydro model) | **0%** |
| **Laja hourly averaging** | 5+ | 5+ (FlowRight internal) | **100%** |
| **Maule flow partition** | 1 per central | 0 | **0%** (structural diff) |
| **Maule volume balances** | 9 | 7 VolumeRight balances | **78%** |
| **Maule % allocation** | 2 | 2 UserConstraints | **100%** |
| **Maule Invernada balance** | 1 | 1 UserConstraint | **100%** |
| **Maule district allocation** | N | N UserConstraints | **100%** |
| **Maule delivery constraint** | 1 | 0 | **0%** |
| **Maule Armerillo balance** | 1 | 0 | **0%** |
| **Maule Res 105** | 1 (fixed flow) | 1 (FlowRight discharge) | **100%** |

### 3.3 Key Structural Differences

1. **Per-central vs aggregated flow partition**: PLP partitions each
   central's turbine flow (Maule, Cipreses, Pehuenche, Colbun) into
   rights categories.  gtopt uses aggregated FlowRights that are not
   tied to specific centrals.  Impact: minor — the optimization allocates
   extraction optimally across centrals anyway.

2. **Irrigation supply decomposition (Laja)**: PLP has an explicit
   80%/20% split between primary and new irrigators.  gtopt models
   districts with fixed demand schedules, which implicitly enforces the
   same split if the base demands and percentages are correctly computed.

3. **Dynamic min turbine bounds**: PLP's `Laja1` computes minimum
   generation bounds from irrigation demand, economics, and reservoir
   state.  gtopt does not impose a minimum generation constraint —
   it relies on demand satisfaction (fail_cost penalty) to incentivize
   generation.

4. **Ordinary reserve accumulation (Maule)**: PLP tracks accumulated
   ordinary reserve extraction (VMGOEF/VMGORF) as state variables.
   gtopt enforces the percentage constraint per-block, which is
   tighter (no temporal smoothing) but simpler.

5. **Multi-year data overrides**: PLP supports annual tables for
   irrigation percentages and Res 105 flows that change year by year.
   gtopt uses a single monthly schedule.  Impact: only for long-horizon
   runs where different years have different irrigation policies.

### 3.4 Priority Gaps to Address

**High priority** (affects LP equivalence):

| Gap | PLP feature | Impact | Effort |
|---|---|---|---|
| Extraordinary flow (IQMEI) | Block-level extraction in extreme zone | Low probability but needed for correctness | Medium |
| Ordinary accum (VMGOEF/VMGORF) | Accumulated % tracking | Affects long-horizon allocation | Medium |
| CostoEmbalsar / CostoNoEmbalsar | Invernada storage/bypass costs | Missing objective terms | Easy (add use_value) |
| `monthly_cost_irr` / `monthly_cost_mixed` | Usage cost modulation (Laja) | Affects dispatch cost | Easy (apply in writer) |

**Medium priority** (affects specific scenarios):

| Gap | PLP feature | Impact | Effort |
|---|---|---|---|
| `manual_withdrawals` / `forced_flows` | Known initial-stage flows | First-stage accuracy | Easy (conditional fmax) |
| `v_util_min` | Min useful volume | Reservoir constraint | Easy |
| Colbun 425 extraction | Volume-conditional bound | Extreme drought only | Medium |
| `v_der_rext_elec/riego_ini` | Ext reserve rights initial | Initialization accuracy | Easy |

**Low priority** (PLP-specific or rare):

| Gap | PLP feature | Impact | Effort |
|---|---|---|---|
| Armerillo balance | Accounting identity | Verification only | N/A |
| Multi-year overrides | Annual tables | Long-horizon only | Medium |
| Auto-modulation | Year-dependent demand | Planning tool feature | Hard |
| Economy flags | Conditional reset/usage | Rare conditions | Hard |
| Supply partition (Laja) | 80%/20% decomposition | Implicitly modeled | N/A |

---

## 4. Parsed but LP-Inert Data

This section identifies data that `plp2gtopt` extracts from the `.dat` files and
passes through to the `.pampl` template or the writer, but that **does not
create any LP column, row, or bound** in the final gtopt LP.

### 4.1 How PAMPL `param` Declarations Work

PAMPL files support two declaration types:
- **`param`**: Named scalar or monthly-indexed values.  Stored in
  `user_param_array`.  **Do NOT create LP variables or rows.**  They
  only affect the LP when *referenced by a `constraint` expression*,
  where they are substituted as constants on the RHS.
- **`constraint`**: Creates actual LP rows (one per block).

Therefore, any `param` line that is **not referenced by any constraint**
is purely documentary — it has zero effect on the LP.

### 4.2 Laja: `param` Declarations in `laja.pampl`

| Template `param` | Referenced by any constraint? | LP effect |
|---|---|---|
| `vol_muerto` | **NO** | **NONE** — documentary only |
| `vol_max` | **NO** | **NONE** — documentary only |
| `irr_base` | **NO** | **NONE** — documentary only |
| `irr_factor_1..N` | **NO** | **NONE** — documentary only |
| `max_irr` | **NO** | **NONE** — documentary only |
| `qmax_irr` | **NO** | **NONE** — documentary only |
| `cost_irr_ns` | **NO** | **NONE** — documentary only |
| `cost_irr_uso` | **NO** | **NONE** — documentary only |
| `irr_usage[month]` | **NO** | **NONE** — documentary only |
| `elec_base` | **NO** | **NONE** — documentary only |
| `elec_factor_1..N` | **NO** | **NONE** — documentary only |
| `max_elec` | **NO** | **NONE** — documentary only |
| `qmax_elec` | **NO** | **NONE** — documentary only |
| `cost_elec_ns` | **NO** | **NONE** — documentary only |
| `cost_elec_uso` | **NO** | **NONE** — documentary only |
| `elec_usage[month]` | **NO** | **NONE** — documentary only |
| `mixed_base` | **NO** | **NONE** — documentary only |
| `mixed_factor_1..N` | **NO** | **NONE** — documentary only |
| `max_mixed` | **NO** | **NONE** — documentary only |
| `qmax_mixed` | **NO** | **NONE** — documentary only |
| `cost_mixed` | **NO** | **NONE** — documentary only |
| `mixed_usage[month]` | **NO** | **NONE** — documentary only |
| `max_anticipated` | **NO** | **NONE** — documentary only |
| `qmax_anticipated` | **NO** | **NONE** — documentary only |
| `antic_usage[month]` | **NO** | **NONE** — documentary only |
| `ini_irr` | **NO** | **NONE** — documentary only |
| `ini_elec` | **NO** | **NONE** — documentary only |
| `ini_mixed` | **NO** | **NONE** — documentary only |
| `ini_anticipated` | **NO** | **NONE** — documentary only |
| `ini_econ_endesa` | **NO** | **NONE** — documentary only |
| `ini_econ_reserve` | **NO** | **NONE** — documentary only |
| `ini_econ_polcura` | **NO** | **NONE** — documentary only |
| `filtration` | **NO** | **NONE** — documentary only |
| `demand_1o_reg` | **NO** | **NONE** — documentary only |
| `demand_2o_reg` | **NO** | **NONE** — documentary only |
| `demand_emergencia` | **NO** | **NONE** — documentary only |
| `demand_saltos` | **NO** | **NONE** — documentary only |
| `seasonal_*[month]` | **NO** | **NONE** — documentary only |
| `monthly_cost_*[month]` | **NO** | **NONE** — documentary only |

**Laja constraint** `laja_partition` references only `flow_right(...).flow`
entities, not any `param`.

**Conclusion**: In the current Laja `.pampl`, **ALL `param` declarations
are LP-inert**.  Every parameter is purely documentary.  The actual LP
structure comes entirely from:
1. **FlowRight** entities (JSON) → columns + averaging rows
2. **VolumeRight** entities (JSON) → state columns + balance rows + demand rows
3. **`constraint laja_partition`** (PAMPL) → flow partition row

### 4.3 Maule: `param` Declarations in `maule.pampl`

Same pattern — the Maule template has ~40 `param` declarations, none of
which are referenced by the constraints.

The Maule constraints reference only `flow_right(...).flow`:
- `invernada_balance`: references 5 FlowRights
- `maule_ord_elec_pct`: references 2 FlowRights + inline numeric constants
- `maule_ord_irr_pct`: references 2 FlowRights + inline numeric constants
- `dist_*`: references 2 FlowRights + inline numeric constants

The percentage constants in the constraint expressions (e.g.,
`0.2 * flow_right('maule_elec_ordinary').flow`) are **hardcoded inline**
from `cfg["pct_elec_reserva"]` during template rendering, **not**
referenced via `param pct_elec_reserva`.

**Conclusion**: In the current Maule `.pampl`, **ALL `param` declarations
are LP-inert**.

### 4.4 Data Flow: Where `.dat` Data Actually Enters the LP

The `.dat` file data enters the LP through **two separate paths**:

```
plplajam.dat / plpmaulen.dat
        │
        ├──→ Parser (laja_parser.py / maule_parser.py)
        │         │
        │         ├──→ Writer (laja_writer.py / maule_writer.py)
        │         │         │
        │         │         ├──→ FlowRight JSON  ──→ LP columns + rows  ✓ LP EFFECT
        │         │         ├──→ VolumeRight JSON ──→ LP columns + rows  ✓ LP EFFECT
        │         │         └──→ UserConstraint JSON ──→ LP rows         ✓ LP EFFECT
        │         │
        │         └──→ Template rendering (.tampl → .pampl)
        │                   │
        │                   ├──→ param declarations ──→ user_param_array  ✗ NO LP EFFECT
        │                   └──→ constraint declarations ──→ LP rows       ✓ LP EFFECT
        │
        └──→ (some fields never leave the parser — not used by writer or template)
```

### 4.5 Complete Inventory: Data That Reaches Parser but NOT the LP

**Laja — parsed, never affects LP:**

| Config key | In writer? | In template? | Reason unused |
|---|---|---|---|
| `intermediate_basins` | NO | NO | Hydro model concern |
| `mes_inicio_riego` | NO | NO | PLP stage-type logic; gtopt uses `reset_month` |
| `mes_inicio_anticipos` | NO | NO | Same |
| `monthly_cost_irr` | NO | YES (param) | **BUG/GAP**: parsed, in template as param, but not used by writer for FlowRight costs and not referenced by any constraint |
| `monthly_cost_mixed` | NO | YES (param) | **BUG/GAP**: same |
| `manual_withdrawals` | NO | NO | PLP initial-stage known flows |
| `forced_flows` | NO | NO | PLP CaudalToro for first N stages |
| `filtration` | NO | YES (param) | Hydro model; param is documentary |
| `vol_muerto` | YES (bound_rule) | YES (param) | Used by writer, param is documentary |
| `vol_max` | YES (fmax) | YES (param) | Used by writer, param is documentary |

**Maule — parsed, never affects LP:**

| Config key | In writer? | In template? | Reason unused |
|---|---|---|---|
| `central_maule` | NO | YES (comment) | Only in template header comment |
| `central_melado` | NO | YES (comment) | Only in template header comment |
| `intermediate_basins` | NO | NO | Hydro model |
| `v_util_min` | NO | NO | PLP min useful volume, not modeled |
| `descuenta_elec_armerillo` | NO | NO | Armerillo not modeled |
| `caudal_min_embalsa` | NO | NO | PLP storage logic |
| `flag_embalsa_1` | NO | NO | PLP storage logic |
| `flag_embalsa_2` | NO | NO | PLP storage logic |
| `n_agnos_pct_manual` | NO | NO | Multi-year overrides |
| `pct_riego_manual` | NO | NO | Multi-year overrides |
| `ano_mod_auto` | NO | NO | Auto-modulation |
| `factor_caudales_futuros` | NO | NO | Auto-modulation |
| `mod_auto_res105` | NO | NO | Auto-modulation |
| `vol_acum_riego_temp` | NO | NO | Accumulated volume caps |
| `dias_acum_temp` | NO | NO | Season day accumulation |
| `n_agnos_res105_manual` | NO | NO | Multi-year overrides |
| `caudal_res105_manual` | NO | NO | Multi-year overrides |
| `idx_extrac_colbun_425` | NO | NO | Colbun 425 extraction |
| `v_cota_425` | NO | NO | Colbun 425 volume |
| `econ_inver_uso_reserva` | NO | NO | Economy behavioral flag |
| `econ_inver_acumulables` | NO | NO | Economy behavioral flag |
| `v_der_rext_elec_ini` | NO | YES (param) | **BUG**: parsed, in template, but not used as VolumeRight eini (uses `v_gasto_rext_elec_ini` instead) |
| `v_der_rext_riego_ini` | NO | YES (param) | **BUG**: same — these are *rights* initials, distinct from *extraction* initials |

### 4.6 Specific Bugs Found

1. **`v_der_rext_elec_ini` vs `v_gasto_rext_elec_ini` (Maule)**:
   The parser produces both.  `v_gasto_rext_*_ini` is the accumulated
   extraction from the extraordinary reserve; `v_der_rext_*_ini` is
   the remaining rights in the extraordinary reserve zone.  The writer
   uses `v_gasto_rext_*_ini` as `eini` for the VolumeRight, which is
   correct (tracking accumulated extraction).  But `v_der_rext_*_ini`
   (remaining rights) is never used — it could serve as the VolumeRight
   `emax` or as a constraint on the maximum additional extraction.

2. **`monthly_cost_irr` and `monthly_cost_mixed` (Laja)**:
   The parser produces 12-month cost factor arrays.  The writer uses
   `monthly_cost_irr_ns` (non-served) and `monthly_cost_elec` for
   FlowRight `fail_cost` modulation, but ignores `monthly_cost_irr`
   (usage cost modulation) and `monthly_cost_mixed` (mixed usage cost
   modulation).  In PLP, these modulate the objective function
   coefficients `CQVar(IQDR)` and `CQVar(IQDM)` via
   `CQVarEta(I, IEta) = CQVar(I) × FactMenCQVar(I, Mes)`.
   If `cost_irr_uso > 0` or `cost_mixed > 0`, the monthly modulation
   should be applied.

3. **Template params as false documentation**: All `param` declarations
   in both templates suggest that the values are "parameters" of the
   model, but they have zero LP effect.  This is misleading — a reader
   might assume changing a `param` value would change the optimization
   result.  The actual LP-affecting values are in the JSON entities
   (FlowRight/VolumeRight fields).

---

## 5. Appendix: PLP Fortran Source Cross-Reference

| Fortran file | Subroutine | Purpose | Lines |
|---|---|---|---|
| `parlajam.f` | (type definition) | PAR_LAJAM structure | 183 |
| `leelajam.f` | `LeeLajaM` | Read plplajam.dat | 462 |
| `genpdlajam.f` | `GenPDLajaMBloA` | Block constraints | 4-124 |
| `genpdlajam.f` | `GenPDLajaMBloFO` | Block objective/bounds | 130-174 |
| `genpdlajam.f` | `GenPDLajaMEtaA` | Stage constraints | 179-313 |
| `genpdlajam.f` | `GenPDLajaMEtaFO` | Stage objective | 316-366 |
| `genpdlajam.f` | `FijaLajaM` | Dynamic bounds/RHS | 477-799 |
| `genpdlajam.f` | `GetVarLajaMPrev` | Previous stage state | 803-836 |
| `genpdlajam.f` | `AgrFactLajaM` | SDDP cut aggregation | 841-907 |
| `genpdlajam.f` | `IsVarCorteLajaM` | State variable classification | 1019-1052 |
| `plp-laja1.f` | `Laja1` | Stage bounds computation | 1-484 |
| `plp-laja1.f` | `Laja1Dual` | Block bounds modification | 486-558 |
| `genpdmaule.f` | `GenPDMauleBloA` | Block constraints | 3-157 |
| `genpdmaule.f` | `GenPDMauleBloFO` | Block objective/bounds | 163-205 |
| `genpdmaule.f` | `GenPDMauleEtaA` | Stage constraints | 210-488 |
| `genpdmaule.f` | `GenPDMauleEtaFO` | Stage objective | 491-551 |
| `genpdmaule.f` | `FijaMaule` | Dynamic bounds/RHS | 654-1133 |
| `genpdmaule.f` | `LeeMaule` | Read plpmaulen.dat | 1300-1930 |
| `genpdmaule.f` | `IsVarCorteMaule` | State variable classification | 2038-2127 |
| `plp-maule1.f` | `Maule1` | Stage bounds computation | 1-470 |
| `plp-maule1.f` | `Maule1Dual` | Block bounds modification | 472-566 |
| `plp-maule1.f` | `Maule1a` | Feasibility verification | 568-652 |
