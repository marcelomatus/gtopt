# Naming Convention Analysis for gtopt

**Date**: April 2026
**Status**: Draft — for review
**Scope**: Element names, field names, option names, JSON keys, LP column names

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Current gtopt Naming Inventory](#2-current-gtopt-naming-inventory)
3. [Cross-Tool Comparison](#3-cross-tool-comparison)
4. [Academic and IEEE Standard Terminology](#4-academic-and-ieee-standard-terminology)
5. [Analysis of Issues](#5-analysis-of-issues)
6. [Naming Update Proposal](#6-naming-update-proposal)
7. [Migration Strategy](#7-migration-strategy)
8. [Implementation Prompt](#8-implementation-prompt)
9. [Phase 1 Implementation Plan — 2026-05](#9-phase-1-implementation-plan--2026-05)
10. [Runtime Dialect Switching — 2026-05](#10-runtime-dialect-switching--2026-05)
11. [Canonical Option-Name Compatibility Review — 2026-05](#11-canonical-option-name-compatibility-review)
12. [Appendix C — Full gtopt ↔ PLEXOS ↔ SDDP Dictionary](#appendix-c-full-gtopt--plexos--sddp-dictionary) *(2026-05-17)*
13. [Appendix D — PLP ↔ gtopt Field Dictionary](#appendix-d-plp--gtopt-field-dictionary) *(2026-05-17)*
14. [Appendix E — Planning / Simulation / Options Dictionary](#appendix-e--planning--simulation--options-dictionary) *(2026-05-17)*

---

## 1. Executive Summary

This document analyses the naming conventions used in gtopt — a C++26 solver
for Generation and Transmission Expansion Planning (GTEP) — and compares them
against industry-standard tools (pandapower, PyPSA, PowerModels.jl, PLEXOS,
GenX) and academic literature. The goal is to identify field names, class names,
and option names that could be improved for clarity, discoverability, and
alignment with community expectations.

### Key Findings

1. **`_array` suffix on JSON collection keys** (`generator_array`,
   `demand_array`, …) is unique to gtopt. Every comparable tool uses simple
   plurals (`generators`, `demands`, `buses`). This adds verbosity without
   information.

2. **Abbreviated field names** like `lmax`, `gcost`, `pmin`, `pmax`,
   `tmax_ab`, `fcost`, `expcap`, `expmod` are terse to the point of
   ambiguity. No other major tool uses these abbreviations. pandapower uses
   `max_p_mw`, PyPSA uses `p_nom`, PowerModels.jl uses `pmax`.

3. **"Demand" class with "load" output variable** creates a confusing duality.
   The JSON input says `demand_array[i].lmax` but the LP output file is
   `Demand/load_sol.csv`. The field `lmax` means "maximum load" but lives on
   a `Demand` object.

4. **Expansion planning fields** (`expcap`, `expmod`, `annual_capcost`,
   `annual_derating`, `capmax`) use ad-hoc abbreviations that don't match
   any other tool. PyPSA uses `p_nom_extendable`, `capital_cost`. GenX uses
   `Cap_Size`, `Inv_Cost_per_MWyr`.

5. **Option name inconsistency**: some options use `snake_case`
   (`use_kirchhoff`, `demand_fail_cost`) while others nest into sub-objects
   (`solver_options.algorithm`, `sddp_options.convergence_tol`). The flat vs.
   nested boundary is not obvious.

6. **Cost field proliferation**: `gcost` (generator), `fcost` (demand
   curtailment), `tcost` (line transfer), `ecost` (energy shortfall),
   `scost` (state/storage), `urcost`/`drcost` (reserve). Each uses a
   different prefix with no unifying pattern.

### Recommendation

Adopt a **descriptive, unit-inclusive naming convention** inspired by
pandapower's `<quantity>_<unit>` pattern and PyPSA's `<physical_quantity>`
pattern, while keeping gtopt's LP-oriented semantics. Provide backward-
compatible JSON aliases during a transition period.

---

## 2. Current gtopt Naming Inventory

### 2.1 Element Class Names

| Class | JSON Key | LP Short | Output Dir | Notes |
|-------|----------|----------|------------|-------|
| Bus | `bus_array` | `bus` | `Bus/` | Standard ✓ |
| Generator | `generator_array` | `gen` | `Generator/` | Standard ✓ |
| Demand | `demand_array` | `dem` | `Demand/` | PyPSA/pandapower: "Load" |
| Line | `line_array` | `lin` | `Line/` | PyPSA: "Line"; pandapower: "line" |
| Battery | `battery_array` | `bat` | `Battery/` | PyPSA: "StorageUnit"/"Store" |
| Converter | `converter_array` | `con` | `Converter/` | PyPSA: "Link" |
| Reservoir | `reservoir_array` | `rsv` | `Reservoir/` | Domain-specific ✓ |
| Junction | `junction_array` | `jun` | `Junction/` | Domain-specific ✓ |
| Waterway | `waterway_array` | `wwy` | `Waterway/` | Domain-specific ✓ |
| Turbine | `turbine_array` | `tur` | `Turbine/` | Domain-specific ✓ |
| Flow | `flow_array` | `flw` | `Flow/` | Domain-specific ✓ |
| GeneratorProfile | `generator_profile_array` | `gpr` | `GeneratorProfile/` | PyPSA: `p_max_pu` series |
| DemandProfile | `demand_profile_array` | `dpr` | `DemandProfile/` | PyPSA: `p_set` series |
| ReserveZone | `reserve_zone_array` | `rzn` | `ReserveZone/` | Domain-specific ✓ |
| ReserveProvision | `reserve_provision_array` | `rpr` | `ReserveProvision/` | Domain-specific ✓ |
| FlowRight | `flow_right_array` | `frt` | `FlowRight/` | Domain-specific ✓ |
| VolumeRight | `volume_right_array` | `vrt` | `VolumeRight/` | Domain-specific ✓ |
| ReservoirSeepage | `reservoir_seepage_array` | `fil` | `ReservoirSeepage/` | Inline or standalone |
| ReservoirDischargeLimit | `reservoir_discharge_limit_array` | `rdl` | — | Constraint element |
| ReservoirProductionFactor | `reservoir_production_factor_array` | `ref` | — | Coefficient element |
| UserConstraint | `user_constraint_array` | `uc` | `UserConstraint/` | User-defined LP rows |

### 2.2 Generator Fields

| gtopt Field | Type | Meaning | pandapower | PyPSA | PowerModels |
|-------------|------|---------|------------|-------|-------------|
| `uid` | Uid | Unique ID | (index) | (index) | `index` |
| `name` | Name | Label | `name` | (index name) | `name` |
| `bus` | SingleId | Connected bus | `bus` | `bus` | `gen_bus` |
| `pmin` | Sched | Min output MW | `min_p_mw` | `p_min_pu` × `p_nom` | `pmin` |
| `pmax` | Sched | Max output MW | `max_p_mw` | `p_max_pu` × `p_nom` | `pmax` |
| `gcost` | Sched | Variable cost $/MWh | (poly_cost) | `marginal_cost` | `cost` |
| `capacity` | Sched | Installed cap MW | `max_p_mw` | `p_nom` | `pmax` |
| `lossfactor` | Sched | Loss factor p.u. | — | `efficiency` | — |
| `expcap` | Sched | MW per expansion module | — | (`p_nom_max − p_nom`) | — |
| `expmod` | Sched | Max modules | — | `p_nom_extendable` | — |
| `capmax` | Sched | Absolute max capacity | — | `p_nom_max` | — |
| `annual_capcost` | Sched | $/MW-year | — | `capital_cost` | — |
| `annual_derating` | Sched | Derating p.u./yr | — | — | — |

### 2.3 Demand Fields

| gtopt Field | Type | Meaning | pandapower | PyPSA | PowerModels |
|-------------|------|---------|------------|-------|-------------|
| `uid` | Uid | Unique ID | (index) | (index) | `index` |
| `name` | Name | Label | `name` | (index name) | — |
| `bus` | SingleId | Connected bus | `bus` | `bus` | `load_bus` |
| `lmax` | Sched | Max served load MW | `p_mw` | `p_set` | `pd` |
| `fcost` | Sched | Curtailment cost $/MWh | — | — | — |
| `emin` | Sched | Min energy MWh | — | — | — |
| `ecost` | Sched | Energy shortfall $/MWh | — | — | — |
| `capacity` | Sched | Installed cap MW | — | `p_nom` | — |
| `expcap` | Sched | MW per module | — | — | — |
| `expmod` | Sched | Max modules | — | `p_nom_extendable` | — |
| `annual_capcost` | Sched | $/MW-year | — | `capital_cost` | — |

### 2.4 Line Fields

| gtopt Field | Type | Meaning | pandapower | PyPSA | PowerModels |
|-------------|------|---------|------------|-------|-------------|
| `bus_a` | SingleId | From bus | `from_bus` | `bus0` | `f_bus` |
| `bus_b` | SingleId | To bus | `to_bus` | `bus1` | `t_bus` |
| `reactance` | Sched | Ω or p.u. | `x_ohm_per_km` | `x` | `br_x` |
| `resistance` | Sched | Ω or p.u. | `r_ohm_per_km` | `r` | `br_r` |
| `tmax_ab` | Sched | MW limit A→B | `max_i_ka` → MVA | `s_nom` | `rate_a` |
| `tmax_ba` | Sched | MW limit B→A | (same) | (same) | `rate_a` |
| `tcost` | Sched | Transfer cost $/MWh | — | — | — |

### 2.5 Battery / Storage Fields

| gtopt Field | Type | Meaning | pandapower | PyPSA (Store) | PyPSA (StorageUnit) |
|-------------|------|---------|------------|---------------|---------------------|
| `emin` | Sched | Min energy MWh | — | `e_min_pu` × `e_nom` | `state_of_charge_min` |
| `emax` | Sched | Max energy MWh | — | `e_nom` | `max_hours × p_nom` |
| `eini` | Real | Initial SoC MWh | — | `e_initial` | `state_of_charge_initial` |
| `efin` | Real | Final SoC MWh | — | — | — |
| `input_efficiency` | Sched | Charge eff. | — | `efficiency_store` | `efficiency_store` |
| `output_efficiency` | Sched | Discharge eff. | — | `efficiency_dispatch` | `efficiency_dispatch` |
| `pmax_charge` | Sched | Max charge MW | — | (via Link) | `p_nom` |
| `pmax_discharge` | Sched | Max discharge MW | — | (via Link) | `p_nom` |

### 2.6 Option Names

| gtopt Option | Meaning | Comparable |
|-------------|---------|------------|
| `use_kirchhoff` | Enable DC OPF angles | PowerModels: model type |
| `use_single_bus` | Copper-plate mode | PyPSA: no network constraints |
| `demand_fail_cost` | VOLL $/MWh | PyPSA: load shedding cost |
| `reserve_fail_cost` | Reserve penalty $/MWh | — |
| `scale_objective` | Obj. coefficient divisor | — |
| `scale_theta` | Angle scaling | — |
| `input_directory` | Input path | Standard ✓ |
| `output_directory` | Output path | Standard ✓ |
| `input_format` | "parquet"/"csv" | Standard ✓ |
| `output_format` | "parquet"/"csv" | Standard ✓ |
| `method` | "monolithic"/"sddp"/"cascade" | — |
| `annual_discount_rate` | Yearly discount | Standard ✓ |

### 2.7 Simulation / Time Structure

| gtopt Field | Meaning | PyPSA | PLEXOS |
|-------------|---------|-------|--------|
| `block_array` | Time blocks (hours) | `snapshots` | Intervals |
| `stage_array` | Investment periods | — | Horizons |
| `scenario_array` | Stochastic scenarios | (scenario loop) | Samples |
| `phase_array` | Super-stages | — | — |
| `scene_array` | Super-scenarios | — | — |

---

## 3. Cross-Tool Comparison

### 3.1 Element Naming

| Concept | gtopt | pandapower | PyPSA | PowerModels | PLEXOS | GenX |
|---------|-------|------------|-------|-------------|--------|------|
| Electrical node | Bus | bus | Bus | bus | Node | Zone |
| Thermal/conventional generator | Generator | gen | Generator | gen | Generator | Resource |
| Renewable generator | Generator (+ profile) | sgen | Generator (carrier) | gen | Generator | Resource |
| Electrical load | **Demand** | **load** | **Load** | **load** | **Demand** / Region | Demand |
| Transmission branch | Line | line | Line / Link | branch | Line | Network |
| Battery storage | Battery | storage | Store / StorageUnit | storage | Battery | Storage |
| Hydro reservoir | Reservoir | — | (custom) | — | Storage | — |

**Observation**: gtopt's "Demand" aligns with PLEXOS but differs from
pandapower ("load") and PyPSA ("Load"). The academic literature uses both
terms: "demand" for the requirement and "load" for the consumption. IEEE
standards tend to use "load" as the electrical term and "demand" for the
economic/planning concept. The primary issue is not the class name itself,
but the mismatch between the class name ("Demand") and its main field
(`lmax`, where "l" stands for "load"). See §5.3 for the recommendation
to rename the field (`lmax` → `max_demand`) rather than the class.

### 3.2 Field Naming Pattern

| Convention | Tool | Example | Notes |
|-----------|------|---------|-------|
| `quantity_unit` | pandapower | `p_mw`, `max_p_mw`, `vn_kv` | Most explicit; unit always present |
| `quantity` | PyPSA | `p_nom`, `marginal_cost`, `efficiency` | Descriptive; units in docs |
| `abbreviation` | PowerModels | `pmin`, `pmax`, `pd`, `qd` | Matpower legacy; terse |
| `abbreviation` | **gtopt** | `lmax`, `gcost`, `pmin`, `pmax` | Mixed; some clear, some cryptic |
| `FULL_NAME` | GenX | `Cap_Size`, `Inv_Cost_per_MWyr` | Verbose CSV headers |
| `FullName` | PLEXOS | `Max Capacity`, `Fuel Price` | GUI-oriented; spaces |

### 3.3 Expansion Planning Fields

| Concept | gtopt | PyPSA | GenX | PLEXOS |
|---------|-------|-------|------|--------|
| Installed capacity | `capacity` | `p_nom` | `Existing_Cap_MW` | `Max Capacity` |
| Expandable? | `expmod > 0` | `p_nom_extendable=True` | `New_Build=1` | `Build=True` |
| Module size | `expcap` | — (continuous) | `Cap_Size` | `Unit Size` |
| Max modules | `expmod` | — | `Max_Units_Built` | `Max Units Built` |
| Max capacity | `capmax` | `p_nom_max` | `Max_Cap_MW` | `Max Capacity` |
| Investment cost | `annual_capcost` | `capital_cost` | `Inv_Cost_per_MWyr` | `WACC * Build Cost` |
| Variable cost | `gcost` | `marginal_cost` | `Var_OM_Cost_per_MWh` | `VO&M Charge` |

### 3.4 Cost Field Naming

| Cost Purpose | gtopt | PyPSA | pandapower | Literature |
|-------------|-------|-------|------------|------------|
| Generation variable cost | `gcost` | `marginal_cost` | `cost_per_mw` (OPF) | marginal cost, Cg |
| Demand curtailment cost | `fcost` | (load shedding cost) | — | VOLL, Cfail |
| Line transfer cost | `tcost` | — | — | transfer cost |
| Energy shortfall cost | `ecost` | — | — | — |
| Reserve penalty | `urcost` / `drcost` | — | — | reserve penalty |
| Investment cost | `annual_capcost` | `capital_cost` | — | annualized CAPEX |
| State/storage cost | `scost` | — | — | water value, storage value |

---

## 4. Academic and IEEE Standard Terminology

### 4.1 IEEE PES Standard Terms

From IEEE Std 100 and IEEE PES transaction papers:

| IEEE/Academic Term | Definition | gtopt Equivalent |
|-------------------|-----------|------------------|
| **Load** | Power consumed by an electrical device | Demand.lmax (output: load_sol) |
| **Demand** | Electric load at the receiving terminals, averaged over a specified interval | Demand element |
| **Generation** | Process of producing electric energy | Generator (output: generation_sol) |
| **Capacity** | Rated continuous load-carrying ability of generation equipment | Generator.capacity |
| **Transmission capacity** | Maximum power transfer capability of a line | Line.tmax_ab / tmax_ba |
| **Loss factor** | Ratio of losses to total power | Generator.lossfactor, Line.lossfactor |
| **Bus** | A conductor serving as a common connection | Bus |
| **Branch** | An element of a network (line, transformer) | Line |
| **Reactance** | Imaginary part of impedance | Line.reactance |
| **Dispatch** | Allocation of generation to meet demand | generation_sol output |
| **Unit commitment** | Decision to start/stop a generator | (planned feature) |
| **Reserve** | Capacity available above current demand | ReserveZone / ReserveProvision |
| **State of charge** | Stored energy as fraction of capacity | Battery.eini / emax |
| **Value of lost load (VOLL)** | Economic cost of unserved energy | demand_fail_cost |

### 4.2 Common Mathematical Notation (from GTEP literature)

| Symbol | Meaning | gtopt Field |
|--------|---------|-------------|
| $P_g^{min}$, $P_g^{max}$ | Generator output bounds | `pmin`, `pmax` |
| $P_d$ or $D_d$ | Demand/load at bus d | `lmax` |
| $f_{ij}$ or $F_{ij}$ | Power flow on line i→j | `flowp_sol` |
| $\bar{f}_{ij}$ | Flow capacity limit | `tmax_ab` |
| $\theta_i$ | Voltage angle at bus i | `theta_sol` |
| $x_{ij}$ | Reactance of line i→j | `reactance` |
| $c_g$ | Variable generation cost | `gcost` |
| $c^{inv}_g$ | Investment cost | `annual_capcost` |
| $\eta_{ch}$, $\eta_{dis}$ | Charge/discharge efficiency | `input_efficiency`, `output_efficiency` |
| $E_{min}$, $E_{max}$ | Storage energy bounds | `emin`, `emax` |
| $E_0$ | Initial state of charge | `eini` |
| $\delta_t$ | Discount factor | `annual_discount_rate` |

---

## 5. Analysis of Issues

### 5.1 `_array` Suffix on JSON Collection Keys

**Problem**: Every collection in gtopt JSON uses `_array` suffix
(`generator_array`, `demand_array`, `bus_array`, …). No other tool does this.
PyPSA uses simple plural component names (`generators`, `buses`, `loads`).
pandapower uses singular (`gen`, `load`, `bus`). PowerModels.jl uses singular
(`gen`, `load`, `branch`).

**Impact**: Adds 6 characters per key, makes JSON harder to read, and creates
a learning barrier for users coming from other tools.

**Recommendation**: **Change to simple plurals** (`generators`, `demands`,
`buses`, `lines`, `batteries`, …). Accept `_array` as a deprecated alias.

**Severity**: Medium — affects every JSON file and every user.

### 5.2 Abbreviated Field Names

**Problem**: Fields like `lmax`, `gcost`, `pmin`, `pmax`, `tmax_ab`,
`fcost`, `expcap`, `expmod` are cryptic abbreviations.

| Field | What it means | Why it's confusing |
|-------|--------------|-------------------|
| `lmax` | Maximum load | "l" could mean "line", "loss", "length" |
| `gcost` | Generation cost | "g" could mean "grid", "global", "green" |
| `fcost` | Failure/curtailment cost | "f" could mean "flow", "fuel", "fixed" |
| `tcost` | Transfer/transmission cost | "t" could mean "time", "total", "thermal" |
| `ecost` | Energy shortfall cost | "e" could mean "efficiency", "expansion" |
| `scost` | State/storage cost | "s" could mean "spinning", "solar", "stage" |
| `tmax_ab` | Transmission max A→B | Why not `flow_max_ab` or `capacity_ab`? |
| `expcap` | Expansion capacity | Not a standard abbreviation |
| `expmod` | Expansion modules | "mod" could mean "model", "modular", "mode" |
| `eini` | Initial energy | Why not `initial_energy`? |
| `efin` | Final energy | Why not `final_energy`? |

**Recommendation**: **Introduce descriptive aliases** while keeping the short
names for backward compatibility. Prioritize renaming the most user-facing
fields.

**Severity**: High — these are the fields users interact with most.

### 5.3 Demand vs. Load Duality

**Problem**: The element is named "Demand" (class, JSON key, output dir) but
its main field is `lmax` ("maximum **load**") and its LP solution variable is
`load_sol`. This creates cognitive dissonance: am I working with demands or
loads?

- pandapower: element = `load`, field = `p_mw` → consistent
- PyPSA: element = `Load`, field = `p_set` → consistent
- PowerModels: element = `load`, field = `pd` → consistent
- **gtopt**: element = `Demand`, field = `lmax` ("l" for load?) → inconsistent

**Recommendation**: Either rename the class to "Load" or rename the field to
`dmax`/`demand_max`. Since the academic GTEP literature predominantly uses
"demand" for the planning context, keeping "Demand" as the class name and
renaming `lmax` → `demand_max` (or `max_demand`) is the best path.

**Severity**: Medium — causes confusion in documentation and onboarding.

### 5.4 Cost Field Prefix Proliferation

**Problem**: Cost fields use inconsistent single-letter prefixes: `gcost`,
`fcost`, `tcost`, `ecost`, `scost`, `urcost`, `drcost`. The prefix is the
first letter of the associated concept, but this is not self-documenting.

**Recommendation**: Use descriptive names:

| Current | Proposed | Meaning |
|---------|----------|---------|
| `gcost` | `marginal_cost` | Variable generation cost (aligns with PyPSA) |
| `fcost` | `curtailment_cost` | Demand curtailment penalty |
| `tcost` | `transfer_cost` | Line transfer cost |
| `ecost` | `energy_shortfall_cost` | Energy minimum violation cost |
| `scost` | `storage_cost` | State/water value cost |
| `urcost` | `up_reserve_cost` | Up-reserve cost |
| `drcost` | `down_reserve_cost` | Down-reserve cost |

**Severity**: High — cost fields are critical for economic interpretation.

### 5.5 Expansion Planning Field Names

**Problem**: `expcap` and `expmod` are non-standard abbreviations that don't
appear in any other tool or academic paper.

**Recommendation**:

| Current | Proposed | Rationale |
|---------|----------|-----------|
| `expcap` | `expansion_capacity` | Self-documenting |
| `expmod` | `expansion_modules` | Self-documenting |
| `capmax` | `max_capacity` | Aligns with PyPSA `p_nom_max` |
| `annual_capcost` | `capital_cost` | Aligns with PyPSA |
| `annual_derating` | `derating_rate` | Simpler |

**Severity**: Medium — affects expansion planning users.

### 5.6 Storage/Energy Field Abbreviations

**Problem**: `emin`, `emax`, `eini`, `efin` use single-letter prefix "e" for
energy. While common in mathematical notation, these are opaque in JSON.

**Recommendation**:

| Current | Proposed | Notes |
|---------|----------|-------|
| `emin` | `energy_min` | or `min_energy` |
| `emax` | `energy_max` | or `max_energy` |
| `eini` | `initial_energy` | PyPSA: `e_initial` |
| `efin` | `final_energy` | No equivalent in other tools |

**Severity**: Low-Medium — mathematical users understand `emin/emax`.

### 5.7 Line Endpoint Naming

**Problem**: gtopt uses `bus_a` / `bus_b` for line endpoints. Other tools:

| Tool | From | To |
|------|------|----|
| pandapower | `from_bus` | `to_bus` |
| PyPSA | `bus0` | `bus1` |
| PowerModels | `f_bus` | `t_bus` |
| **gtopt** | `bus_a` | `bus_b` |

**Recommendation**: **Keep `bus_a` / `bus_b`**. While `from_bus` / `to_bus`
is more common across tools, the gtopt convention emphasizes that in DC
power flow the flow direction is determined by the solver, not by the
user's endpoint labeling. The neutral naming avoids implying a "from→to"
direction that does not exist in the input specification. That said, this
is a matter of preference — `from_bus` / `to_bus` would also be acceptable
and more familiar to users of pandapower and PowerModels.jl. We recommend
keeping the current convention but documenting the rationale clearly.

**Severity**: Low — acceptable as-is.

### 5.8 Reserve Fields

**Problem**: `urreq`, `drreq`, `urmax`, `drmax` use two-letter prefixes
(`ur` = up reserve, `dr` = down reserve) followed by abbreviated suffixes.

**Recommendation**:

| Current | Proposed |
|---------|----------|
| `urreq` | `up_reserve_requirement` |
| `drreq` | `down_reserve_requirement` |
| `urmax` | `up_reserve_max` |
| `drmax` | `down_reserve_max` |

**Severity**: Low — reserve modeling is used by advanced users.

### 5.9 Option Name `demand_fail_cost`

**Problem**: The option `demand_fail_cost` controls the Value of Lost Load
(VOLL). The name is unclear: "fail cost" could mean "cost of failure" or
"default cost when demand fails".

**Recommendation**: Rename to `value_of_lost_load` or `lost_load_cost`.
The term "Value of Lost Load" (VOLL) is standard in IEEE literature.

**Severity**: Medium — this is a key economic parameter.

---

## 6. Naming Update Proposal

### 6.1 Priority Tiers

Changes are grouped by impact and difficulty:

#### Tier 1 — High Impact, JSON-only (no LP/output changes)

These changes only affect JSON input parsing and can be done with backward-
compatible aliases:

| Category | Current | Proposed | Alias? |
|----------|---------|----------|--------|
| **Collections** | `generator_array` | `generators` | Keep `_array` as alias |
| | `demand_array` | `demands` | Keep `_array` as alias |
| | `bus_array` | `buses` | Keep `_array` as alias |
| | `line_array` | `lines` | Keep `_array` as alias |
| | `battery_array` | `batteries` | Keep `_array` as alias |
| | `converter_array` | `converters` | Keep `_array` as alias |
| | `junction_array` | `junctions` | Keep `_array` as alias |
| | `waterway_array` | `waterways` | Keep `_array` as alias |
| | `reservoir_array` | `reservoirs` | Keep `_array` as alias |
| | `turbine_array` | `turbines` | Keep `_array` as alias |
| | `flow_array` | `flows` | Keep `_array` as alias |
| | `generator_profile_array` | `generator_profiles` | Keep `_array` as alias |
| | `demand_profile_array` | `demand_profiles` | Keep `_array` as alias |
| | `reserve_zone_array` | `reserve_zones` | Keep `_array` as alias |
| | `reserve_provision_array` | `reserve_provisions` | Keep `_array` as alias |
| | `flow_right_array` | `flow_rights` | Keep `_array` as alias |
| | `volume_right_array` | `volume_rights` | Keep `_array` as alias |
| | `block_array` | `blocks` | Keep `_array` as alias |
| | `stage_array` | `stages` | Keep `_array` as alias |
| | `scenario_array` | `scenarios` | Keep `_array` as alias |
| | `phase_array` | `phases` | Keep `_array` as alias |
| | `scene_array` | `scenes` | Keep `_array` as alias |

#### Tier 2 — High Impact, Field Renames (need LP/output aliases too)

| Element | Current | Proposed | Backward Compat |
|---------|---------|----------|-----------------|
| Generator | `gcost` | `marginal_cost` | Accept both in JSON |
| Generator | `pmin` | `min_power` | Accept both |
| Generator | `pmax` | `max_power` | Accept both |
| Demand | `lmax` | `max_demand` | Accept both |
| Demand | `fcost` | `curtailment_cost` | Accept both |
| Line | `tmax_ab` | `max_flow_ab` | Accept both |
| Line | `tmax_ba` | `max_flow_ba` | Accept both |
| Line | `tcost` | `transfer_cost` | Accept both |
| Battery | `emin` | `energy_min` | Accept both |
| Battery | `emax` | `energy_max` | Accept both |
| Battery | `eini` | `initial_energy` | Accept both |
| Battery | `efin` | `final_energy` | Accept both |

#### Tier 3 — Medium Impact, Expansion & Cost Fields

| Element | Current | Proposed | Backward Compat |
|---------|---------|----------|-----------------|
| All expandable | `expcap` | `expansion_capacity` | Accept both |
| All expandable | `expmod` | `expansion_modules` | Accept both |
| All expandable | `capmax` | `max_capacity` | Accept both |
| All expandable | `annual_capcost` | `capital_cost` | Accept both |
| All expandable | `annual_derating` | `derating_rate` | Accept both |
| Demand | `ecost` | `energy_shortfall_cost` | Accept both |
| Battery/Reservoir | `ecost` | `storage_value_cost` | Accept both |
| ReserveZone | `urreq` | `up_reserve_requirement` | Accept both |
| ReserveZone | `drreq` | `down_reserve_requirement` | Accept both |
| ReserveZone | `urcost` | `up_reserve_cost` | Accept both |
| ReserveZone | `drcost` | `down_reserve_cost` | Accept both |
| ReserveProvision | `urmax` | `up_reserve_max` | Accept both |
| ReserveProvision | `drmax` | `down_reserve_max` | Accept both |

#### Tier 4 — Low Impact, Option Renames

| Current | Proposed | Notes |
|---------|----------|-------|
| `demand_fail_cost` | `lost_load_cost` | VOLL standard term |
| `reserve_fail_cost` | `reserve_shortfall_cost` | More descriptive |
| `hydro_fail_cost` | `hydro_shortfall_cost` | More descriptive |
| `use_kirchhoff` | `use_kirchhoff` | **Keep** — more precise than `use_dc_opf` (see §5 note) |
| `use_single_bus` | `use_copper_plate` | Common alternative name |
| `scale_objective` | `objective_scale_factor` | More descriptive |
| `scale_theta` | `angle_scale_factor` | More descriptive |
| `use_uid_fname` | `use_uid_filenames` | More descriptive |

> **Note on `use_kirchhoff`**: An earlier draft proposed renaming to
> `use_dc_opf`, but `use_kirchhoff` is actually more precise — it
> refers specifically to enforcing Kirchhoff's Voltage Law (angle
> variables and flow = Δθ/x constraints), not the broader concept of
> DC optimal power flow. We recommend keeping this name.

### 6.2 What NOT to Change

The following names are **already good** and should not be changed:

| Name | Why it's fine |
|------|---------------|
| `Bus` | Universal standard |
| `Generator` | Universal standard |
| `Line` | Universal standard (vs. "Branch") |
| `Battery` | Clear and standard |
| `Reservoir`, `Junction`, `Waterway`, `Turbine`, `Flow` | Domain-specific, self-documenting |
| `Demand` (class name) | Correct for GTEP context (vs. "Load" for power flow) |
| `bus_a` / `bus_b` | Neutral bidirectional naming ✓ |
| `reactance`, `resistance` | Standard electrical terms |
| `input_efficiency`, `output_efficiency` | Clear and descriptive |
| `pmax_charge`, `pmax_discharge` | Clear and descriptive |
| `annual_discount_rate` | Standard financial term |
| `capacity` | Universal |
| `lossfactor` | Standard |
| `input_directory`, `output_directory` | Standard |
| `input_format`, `output_format` | Standard |

### 6.3 Proposed Naming Convention Rules

Going forward, new fields should follow these rules:

1. **Use `snake_case`** for all JSON field names (already the convention).
2. **Use descriptive English words**, not abbreviations — `marginal_cost`
   not `gcost`, `max_power` not `pmax`.
3. **Include the physical quantity** in the name — `max_flow_ab` not
   `tmax_ab`.
4. **Use simple plurals** for collections — `generators` not
   `generator_array`.
5. **Cost fields end in `_cost`** — `marginal_cost`, `curtailment_cost`,
   `capital_cost`.
6. **Bounds use `min_`/`max_` or `_min`/`_max` prefix/suffix** —
   `min_power`, `max_demand`, `energy_min`.
7. **Initial/final state use `initial_`/`final_`** — `initial_energy`,
   `final_energy`.
8. **Expansion fields use `expansion_`** — `expansion_capacity`,
   `expansion_modules`.
9. **Options use descriptive phrases** — `lost_load_cost` not
   `demand_fail_cost`.

---

## 7. Migration Strategy

### 7.1 Backward Compatibility

All changes should be **backward compatible** via JSON alias support:

1. **JSON deserialization**: Accept both old and new names. When both are
   present, new name takes precedence with a deprecation warning.
2. **JSON serialization**: Output uses new names by default. Add a
   `--legacy-json` flag to produce old names.
3. **LP column names**: Keep the short `gen_1_generation` format (these
   are internal and not user-facing).
4. **Output directories**: Keep `Generator/`, `Demand/`, etc. (these
   match the class names which are not changing).
5. **Output file names**: Keep `generation_sol.csv`, `load_sol.csv`, etc.
   (these match the LP variable names).

### 7.2 Implementation Phases

**Phase 1**: Add JSON aliases for collection keys (Tier 1). Update
documentation and examples to use new names.

**Phase 2**: Add JSON aliases for field names (Tier 2). Update case files.
Add deprecation warnings for old names.

**Phase 3**: Add JSON aliases for remaining fields (Tier 3, 4). Update
all documentation.

**Phase 4** (future): Remove deprecated aliases after 2 major versions.

### 7.3 Files That Need Updates

| Category | Files | Count |
|----------|-------|-------|
| JSON contracts | `include/gtopt/json_*.hpp` | ~10 |
| C++ headers | `include/gtopt/*.hpp` (struct members) | ~20 |
| Test cases | `test/source/*.hpp`, `test/source/*.cpp` | ~30 |
| Example cases | `cases/*/` JSON files | ~12 |
| Documentation | `docs/*.md` | ~8 |
| Python scripts | `scripts/*/` writers and tests | ~20 |
| Web service | `webservice/` | ~5 |
| GUI service | `guiservice/` | ~5 |

---

## 8. Implementation Prompt

The following prompt can be used with Claude to implement the naming changes
described in this proposal. The implementation should be done in phases.

### Phase 1 Prompt: Collection Key Aliases

```
You are implementing naming convention improvements for the gtopt C++ project
(Generation and Transmission Expansion Planning solver).

## Task: Add plural collection key aliases in JSON deserialization

### Context
Currently, all JSON collection keys use the `_array` suffix pattern:
- `generator_array`, `demand_array`, `bus_array`, `line_array`, etc.
- `block_array`, `stage_array`, `scenario_array`, etc.

We want to also accept simple plural names as aliases:
- `generators`, `demands`, `buses`, `lines`, etc.
- `blocks`, `stages`, `scenarios`, etc.

### Requirements

1. **Backward compatible**: Both old (`_array`) and new (plural) names must
   work. If both are present in the same JSON, the plural name takes precedence.

2. **Files to modify**:
   - `include/gtopt/json_system.hpp` — System struct JSON mapping
   - `include/gtopt/json_simulation.hpp` — Simulation struct JSON mapping
   - `include/gtopt/json_planning.hpp` — Planning struct JSON mapping
   
3. **Implementation approach**: The DAW JSON library used by gtopt supports
   alternative member names via `json_link_no_name` or additional name aliases.
   Look at how the existing json_data_contract mappings work and add alias
   support.

4. **Update all example case JSON files** in `cases/` to use the new plural
   names (keeping backward compat means old files still work).

5. **Update documentation** in `docs/input-data.md` to show the new names
   as primary, with a note that `_array` suffix is accepted for backward
   compatibility.

6. **Add a test** in `test/source/` that verifies both old and new JSON
   key names deserialize correctly.

### Mapping Table

| Old Key | New Key (alias) |
|---------|----------------|
| `bus_array` | `buses` |
| `generator_array` | `generators` |
| `demand_array` | `demands` |
| `line_array` | `lines` |
| `battery_array` | `batteries` |
| `converter_array` | `converters` |
| `junction_array` | `junctions` |
| `waterway_array` | `waterways` |
| `reservoir_array` | `reservoirs` |
| `turbine_array` | `turbines` |
| `flow_array` | `flows` |
| `generator_profile_array` | `generator_profiles` |
| `demand_profile_array` | `demand_profiles` |
| `reserve_zone_array` | `reserve_zones` |
| `reserve_provision_array` | `reserve_provisions` |
| `flow_right_array` | `flow_rights` |
| `volume_right_array` | `volume_rights` |
| `reservoir_seepage_array` | `reservoir_seepages` |
| `reservoir_discharge_limit_array` | `reservoir_discharge_limits` |
| `reservoir_production_factor_array` | `reservoir_production_factors` |
| `user_constraint_array` | `user_constraints` |
| `user_param_array` | `user_params` |
| `block_array` | `blocks` |
| `stage_array` | `stages` |
| `scenario_array` | `scenarios` |
| `phase_array` | `phases` |
| `scene_array` | `scenes` |
| `aperture_array` | `apertures` |
| `iteration_array` | `iterations` |
```

### Phase 2 Prompt: Field Name Aliases

```
You are implementing naming convention improvements for the gtopt C++ project.

## Task: Add descriptive field name aliases for Generator, Demand, Line, Battery

### Context
gtopt uses abbreviated field names inherited from mathematical notation.
We want to add descriptive aliases that are more user-friendly, while keeping
the old names for backward compatibility.

### Requirements

1. **Backward compatible**: Both old and new names must work in JSON.
   The C++ struct member names remain unchanged (old names). The JSON
   deserialization accepts both.

2. **Priority field renames** (Generator):
   - `gcost` → also accept `marginal_cost`
   - `pmin` → also accept `min_power`
   - `pmax` → also accept `max_power`

3. **Priority field renames** (Demand):
   - `lmax` → also accept `max_demand`
   - `fcost` → also accept `curtailment_cost`

4. **Priority field renames** (Line):
   - `tmax_ab` → also accept `max_flow_ab`
   - `tmax_ba` → also accept `max_flow_ba`
   - `tcost` → also accept `transfer_cost`

5. **Priority field renames** (Battery & Reservoir):
   - `emin` → also accept `energy_min`
   - `emax` → also accept `energy_max`
   - `eini` → also accept `initial_energy`
   - `efin` → also accept `final_energy`

6. **Priority field renames** (Expansion, all elements):
   - `expcap` → also accept `expansion_capacity`
   - `expmod` → also accept `expansion_modules`
   - `capmax` → also accept `max_capacity`
   - `annual_capcost` → also accept `capital_cost`
   - `annual_derating` → also accept `derating_rate`

7. **Priority field renames** (Reserve):
   - `urreq` → also accept `up_reserve_requirement`
   - `drreq` → also accept `down_reserve_requirement`
   - `urcost` → also accept `up_reserve_cost`
   - `drcost` → also accept `down_reserve_cost`
   - `urmax` → also accept `up_reserve_max`
   - `drmax` → also accept `down_reserve_max`

8. **Priority field renames** (Options):
   - `demand_fail_cost` → also accept `lost_load_cost`
   - `reserve_fail_cost` → also accept `reserve_shortfall_cost`
   - `use_single_bus` → also accept `use_copper_plate`
   - Note: keep `use_kirchhoff` as-is (more precise than `use_dc_opf`)

9. **Files to modify**:
   - `include/gtopt/json_generator.hpp` and related `json_*.hpp` files
   - All JSON data contract mappings that reference these fields
   - Example case files in `cases/`
   - Documentation in `docs/input-data.md`

10. **Add tests** verifying both old and new field names parse correctly
    and produce the same results.

### Implementation Notes
- C++ struct member names do NOT change (avoid massive refactoring)
- Only JSON deserialization layer adds aliases
- The DAW JSON library supports alternative names via json_link configurations
- Update docs to show new names as primary, old names as "also accepted"
```

### Phase 3 Prompt: Documentation & Examples Update

```
You are updating documentation and example cases for the gtopt project
after naming convention improvements.

## Task: Update all documentation and examples to use new naming

### Requirements

1. Update `docs/input-data.md` to use new field names as primary, with
   "(also: old_name)" notes for backward compatibility.

2. Update `docs/usage.md`, `docs/planning-guide.md`, and
   `docs/formulation/mathematical-formulation.md` to use new names.

3. Update all JSON files in `cases/` to use new collection and field names.

4. Update `README.md` examples.

5. Update `.github/copilot-instructions.md` and `CLAUDE.md` to reflect
   new naming.

6. Update Python scripts in `scripts/` to output new names.

7. Add a "Naming Migration Guide" section to `docs/input-data.md` that
   lists all old → new name mappings and explains the deprecation timeline.
```

---

## Appendix A: Full Cross-Tool Comparison Table

### Generator / Generation Unit

| Attribute | gtopt | pandapower | PyPSA | PowerModels | PLEXOS | GenX |
|-----------|-------|------------|-------|-------------|--------|------|
| Class name | Generator | gen | Generator | gen | Generator | Resource |
| Connected bus | `bus` | `bus` | `bus` | `gen_bus` | Node | Zone |
| Min output | `pmin` | `min_p_mw` | `p_min_pu` | `pmin` | `Min Stable Level` | `Min_Power` |
| Max output | `pmax` | `max_p_mw` | `p_max_pu` | `pmax` | `Max Capacity` | `Max_Power` |
| Installed capacity | `capacity` | `max_p_mw` | `p_nom` | `pmax` | `Max Capacity` | `Existing_Cap_MW` |
| Variable cost | `gcost` | (poly_cost) | `marginal_cost` | `cost` | `VO&M Charge` | `Var_OM_Cost_per_MWh` |
| Investment cost | `annual_capcost` | — | `capital_cost` | — | `Build Cost` | `Inv_Cost_per_MWyr` |
| Expandable | `expmod > 0` | — | `p_nom_extendable` | — | `Build = True` | `New_Build = 1` |
| Max capacity | `capmax` | — | `p_nom_max` | — | `Max Units Built` | `Max_Cap_MW` |
| Module size | `expcap` | — | — (continuous) | — | `Unit Size` | `Cap_Size` |
| Efficiency | `lossfactor` | — | `efficiency` | — | `Heat Rate` | `Heat_Rate_MMBTU_per_MWh` |
| Name | `name` | `name` | (index) | `name` | Name | `Resource` |
| ID | `uid` | (index) | (index) | `index` | — | `Resource_ID` |

### Load / Demand

| Attribute | gtopt | pandapower | PyPSA | PowerModels | PLEXOS | GenX |
|-----------|-------|------------|-------|-------------|--------|------|
| Class name | **Demand** | **load** | **Load** | **load** | **Demand** | Demand |
| Connected bus | `bus` | `bus` | `bus` | `load_bus` | Node | Zone |
| Max/set load | **`lmax`** | **`p_mw`** | **`p_set`** | **`pd`** | `Load` | `Demand_MW` |
| Curtailment cost | `fcost` | — | — | — | `VOLL` | `Lost_Load_Cost` |

### Transmission Line / Branch

| Attribute | gtopt | pandapower | PyPSA | PowerModels | PLEXOS |
|-----------|-------|------------|-------|-------------|--------|
| Class name | Line | line | Line | branch | Line |
| From bus | `bus_a` | `from_bus` | `bus0` | `f_bus` | `Node From` |
| To bus | `bus_b` | `to_bus` | `bus1` | `t_bus` | `Node To` |
| Reactance | `reactance` | `x_ohm_per_km` | `x` | `br_x` | `Reactance` |
| Resistance | `resistance` | `r_ohm_per_km` | `r` | `br_r` | `Resistance` |
| Flow limit | `tmax_ab`/`tmax_ba` | `max_i_ka` | `s_nom` | `rate_a` | `Max Flow` |
| Transfer cost | `tcost` | — | — | — | `Wheeling Charge` |

### Storage / Battery

| Attribute | gtopt | PyPSA (Store) | PyPSA (StorageUnit) | PLEXOS |
|-----------|-------|---------------|---------------------|--------|
| Class name | Battery | Store | StorageUnit | Battery |
| Energy min | `emin` | `e_min_pu × e_nom` | — | `Min Volume` |
| Energy max | `emax` | `e_nom` | `max_hours × p_nom` | `Max Volume` |
| Initial energy | `eini` | `e_initial` | `state_of_charge_initial` | `Initial Volume` |
| Final energy | `efin` | — | — | `End Volume` |
| Charge eff. | `input_efficiency` | `efficiency_store` | `efficiency_store` | `Charge Efficiency` |
| Discharge eff. | `output_efficiency` | `efficiency_dispatch` | `efficiency_dispatch` | `Discharge Efficiency` |
| Max charge | `pmax_charge` | (via Link) | `p_nom` | `Max Power In` |
| Max discharge | `pmax_discharge` | (via Link) | `p_nom` | `Max Power Out` |

---

## Appendix B: References

1. **pandapower**: Thurner, L. et al. (2018). "pandapower — An Open-Source
   Python Tool for Convenient Modeling, Analysis, and Optimization of
   Electric Power Systems." IEEE Trans. Power Systems, 33(6), 6510-6521.
   DOI: [10.1109/TPWRS.2018.2829021](https://doi.org/10.1109/TPWRS.2018.2829021).
   https://pandapower.readthedocs.io/

2. **PyPSA**: Brown, T. et al. (2018). "PyPSA: Python for Power System
   Analysis." Journal of Open Research Software, 6(4).
   DOI: [10.5334/jors.188](https://doi.org/10.5334/jors.188).
   https://pypsa.org/

3. **PowerModels.jl**: Coffrin, C. et al. (2018). "PowerModels.jl: An
   Open-Source Framework for Exploring Power Flow Formulations." PSCC 2018.
   DOI: [10.23919/PSCC.2018.8442948](https://doi.org/10.23919/PSCC.2018.8442948).
   https://github.com/lanl-ansi/PowerModels.jl

4. **GenX**: Jenkins, J.D., Sepulveda, N.A. (2017). "Enhanced Decision
   Support for a Changing Electricity Landscape." MIT Energy Initiative.
   https://github.com/GenXProject/GenX.jl

5. **PLEXOS**: Energy Exemplar. "PLEXOS Integrated Energy Model."
   https://www.energyexemplar.com/plexos

6. **IEEE Std 100**: IEEE Standard Dictionary of Electrical and Electronics
   Terms. IEEE, 2000.

7. **Romero, R. & Monticelli, A.** (1994). "A hierarchical decomposition
   approach for transmission network expansion planning." IEEE Trans.
   Power Systems, 9(1), 373-380.
   DOI: [10.1109/59.317600](https://doi.org/10.1109/59.317600).

8. **Lumbreras, S. & Ramos, A.** (2016). "The new challenges to
   transmission expansion planning. Survey of recent practice and
   literature review." Electric Power Systems Research, 134, 19-29.
   DOI: [10.1016/j.epsr.2015.10.013](https://doi.org/10.1016/j.epsr.2015.10.013).

9. **Stott, B., Jardim, J. & Alsaç, O.** (2009). "DC Power Flow
   Revisited." IEEE Trans. Power Systems, 24(3), 1290-1300.
   DOI: [10.1109/TPWRS.2009.2021235](https://doi.org/10.1109/TPWRS.2009.2021235).

10. **Anderson, P.M. & Fouad, A.A.** (2002). "Power Systems Control
    and Stability." 2nd ed., Wiley-IEEE Press.
    ISBN: 978-0-471-23862-1.

---

## Appendix C: Full gtopt ↔ PLEXOS ↔ SDDP Dictionary

Compiled 2026-05-17 to support the naming-decision discussion.

**Scope.** Covers every element class currently mapped in
`include/gtopt/json/json_*.hpp` (25 element types + the time-axis
classes). For each, the table lists the **gtopt JSON key**, the
**PLEXOS property** (from Energy Exemplar's class-model
documentation), the **PSR SDDP** input field (from the SDDP
data-file schema; SDDP property names are Portuguese-derived and
often abbreviated), and the **IEEE / literature symbol** where one
is standard.

**Caveats.**
- "SDDP" here means **PSR's SDDP commercial product** (Brazilian
  hydro-thermal dispatch tool), not the algorithm of the same name.
- PLEXOS property names taken from the v9.x object model; some
  scenario-specific or band-specific variants are omitted.
- SDDP field names taken from `.dat` and JSON-export schemas; older
  versions used different abbreviations. PSR SDDP recently moved to
  English property names in newer JSON exports — both are listed
  where applicable as `Portuguese / English`.
- Empty cells mean "no direct equivalent in that tool"; a row whose
  PLEXOS and SDDP cells are both empty is a gtopt-specific concept
  (typically irrigation or domain-specific).

### C.1 Bus / Node

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Bus` (class) | `Node` | `Sistema` / `Bus` | bus *i* | |
| `uid` | (auto-ID) | `no_barr` / `BusNumber` | *i* | |
| `name` | `Name` | `Nome` / `Name` | — | |
| `voltage` | `Voltage` | `Tensao` / `Voltage` (kV) | $V_i$ | |
| `reference_theta` | `Reference Node` (bool) + slack | `Ref` flag | $\theta_{ref}$ | gtopt holds an angle value; PLEXOS/SDDP just flag the slack. |
| `use_kirchhoff` | (per-region setting) | `Tipo_Rede` (`DC`/`Transp`) | — | |

### C.2 Generator (thermal/conventional)

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Generator` (class) | `Generator` | `Termoeletrica` / `Thermal` | unit *g* | |
| `bus` | `Nodes From` | `no_barr` | bus($g$) | |
| `pmin` | `Min Stable Level` | `GerMin` / `MinGen` | $P_g^{\min}$ | |
| `pmax` | `Max Capacity` | `GerMax` / `MaxGen` (or `PotInst`) | $P_g^{\max}$ | |
| `capacity` | `Max Capacity` (rated) | `PotInst` / `InstalledCapacity` | $\bar P_g$ | |
| `gcost` | `VO&M Charge` + `Fuel Price × Heat Rate` | `CVU` / `VariableCost` | $c_g$ | gtopt rolls fuel × heat-rate into `gcost` unless a `Fuel`/`Commitment` is bound. |
| `lossfactor` | (per-bus loss factor) | `FatorPerda` | $\eta_g$ | |
| `heat_rate` | `Heat Rate Base` / `Heat Rate Incr` | `ConsumoEspecifico` / `HeatRate` | $H_g$ | |
| `fuel` | `Fuel(s)` (membership) | `Combustivel` / `Fuel` | — | |
| `expmod` | `Max Units Built` | (newer expansion module) | — | |
| `expcap` | `Unit Size` / `Build Cost (per MW)` | — | $\Delta\bar P$ | |
| `capmax` | `Max Capacity Built` | `PotMax` | $P_g^{\max,total}$ | |
| `annual_capcost` | `Build Cost` × `WACC` | `CFix` / `FixedCost` | $c_g^{inv}$ | |
| `annual_derating` | (Outage rate / De-rating Factor) | `TaxaIndispoForcada` / `FOR` | — | |
| `emission_rate` | `CO2 Production Rate` | `FatorEmissao` / `EmissionFactor` | — | |

### C.3 Demand / Load

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Demand` (class) | `Load` (Region property) | `Carga` / `Load` (per-bus) | demand *d* | |
| `bus` | (Region → Node) | `no_barr` | bus($d$) | |
| `lmax` | `Load` (timeseries) | `Demanda` / `Demand` (per block) | $D_d$ | |
| `fcost` | `VoLL` | `CustoDeficit` / `DeficitCost` | VOLL | |
| `lossfactor` | (per-zone loss) | `FatorPerda` | $\eta_d$ | |
| `emin` | `Min Energy` constraint | `EnergiaMin` | — | Energy floor across blocks. |
| `ecost` | `Min Energy Penalty` | `CustoEnergiaMin` | — | |
| `capacity` | `Max Load` | — | $\bar D_d$ | |
| `expmod` / `expcap` | — | — | — | Demand growth modeled via expansion modules. |

### C.4 Line / Branch

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Line` (class) | `Line` | `Circuito` / `Circuit` | branch *ℓ* (or *i,j*) | |
| `bus_a` | `Node From` | `De` / `BusFrom` | *i* | |
| `bus_b` | `Node To` | `Para` / `BusTo` | *j* | |
| `reactance` | `Reactance` (Ω or p.u.) | `Reat` / `Reactance` | $x_{ij}$ | |
| `resistance` | `Resistance` | `Resist` / `Resistance` | $r_{ij}$ | |
| `voltage` | `Voltage` | `Tensao` | $V$ | |
| `tmax_ab` | `Max Flow` | `Capac` / `Capacity` | $\bar f_{ij}$ | |
| `tmax_ba` | `Min Flow` (negated) or `Max Flow (Reverse)` | `CapacRev` / `CapacityReverse` | $\underline f_{ij}$ | |
| `tcost` | `Wheeling Charge` | `CustoTransmissao` | — | |
| `lossfactor` | `Loss Coefficient` | `FatorPerdas` | $\rho_{ij}$ | |
| `use_line_losses` | `Allow Line Losses` | `ConsiderarPerdas` | — | |
| `line_losses_mode` / `loss_segments` | `Loss Segments` | `NumSegPerdas` | — | |
| `tap_ratio` | `Units Out` × `Tap` | `Tap` | $t_{ij}$ | |
| `phase_shift_deg` | `Phase Shift` | `DefasagemAngular` | $\phi_{ij}$ | |
| `expcap` / `expmod` / `capmax` | `Max Units Built` / `Build Cost` | (expansion via newer schema) | — | |
| `annual_capcost` | `Build Cost × WACC` | `CFix` | — | |

### C.5 Battery / Storage Unit

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Battery` (class) | `Battery` | `Bateria` / `Battery` | storage *s* | |
| `bus` | `Node` | `no_barr` | bus($s$) | |
| `source_generator` | (Linked Generator) | — | — | gtopt-specific (in-bus modeling). |
| `pmax_charge` | `Max Power Flow` (charge) | `PotCarga` / `MaxChargePower` | $P_s^{ch,\max}$ | |
| `pmax_discharge` | `Max Power Flow` (discharge) | `PotDescarga` / `MaxDischargePower` | $P_s^{dis,\max}$ | |
| `input_efficiency` | `Charge Efficiency` | `EfCarga` / `ChargeEff` | $\eta^{ch}$ | |
| `output_efficiency` | `Discharge Efficiency` | `EfDescarga` / `DischargeEff` | $\eta^{dis}$ | |
| `emin` | `Min SoC` | `E_Min` / `MinEnergy` | $E_s^{\min}$ | |
| `emax` | `Max SoC` / `Capacity` | `E_Max` / `MaxEnergy` | $E_s^{\max}$ | |
| `eini` | `Initial SoC` | `E_Inicial` / `InitialEnergy` | $E_s^0$ | |
| `efin` | `End SoC` | `E_Final` / `FinalEnergy` | $E_s^T$ | |
| `efin_cost` | (End-SoC penalty constraint) | — | — | gtopt soft end-of-horizon penalty. |
| `soft_emin` / `soft_emin_cost` | `Min SoC Penalty` | `CustoVioMin` | — | |
| `ecost` / `scost` | `Energy Storage Cost` | `CustoEnergia` | — | Water-value-like coefficient. |
| `gcost` | `VO&M Charge` | `CVU` | — | Throughput cost. |
| `annual_loss` | `Self-Discharge Rate` | `TaxaAutoDescarga` | $\lambda^{loss}$ | |

### C.6 Reservoir (hydro)

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Reservoir` (class) | `Storage` (Hydro) | `Reservatorio` / `Reservoir` (part of `Hidreletrica`) | reservoir *r* | |
| `junction` | (linked via Waterway/Generator) | (implicit topology) | — | gtopt requires an explicit Junction. |
| `spill_junction` | (Spillway connection) | `JusanteVertimento` | — | |
| `spillway_capacity` | `Max Spill` | `VertedorMaximo` / `MaxSpill` | $\bar S_r$ | |
| `spillway_cost` | `Spill Penalty` | `CustoVertimento` | — | |
| `emin` | `Min Volume` | `VolMin` / `MinVolume` (hm³) | $V_r^{\min}$ | |
| `emax` | `Max Volume` | `VolMax` / `MaxVolume` | $V_r^{\max}$ | |
| `eini` | `Initial Volume` | `V_Inicial` / `InitialVolume` | $V_r^0$ | |
| `efin` | `End Volume` | `V_Final` / `FinalVolume` | $V_r^T$ | |
| `soft_emin` / `soft_emin_cost` | `Vol Penalty` | `CustoVioVolMin` | — | |
| `scost` | `Energy Coefficient` × `Discount Rate` | (water-value via FCF) | $\pi_r$ | gtopt explicit; SDDP derives from Bellman value functions. |
| `mean_production_factor` | `Energy Coefficient` (MWh/hm³) | `FProd` / `ProductionFactor` | $\rho_r$ | |
| `fmin` / `fmax` | `Min Release` / `Max Release` | `VazaoMin` / `VazaoMax` | $\underline Q, \bar Q$ | |
| `flow_conversion_rate` | (Unit conversion) | `FatorConversao` | — | hm³ ↔ MWh. |
| `annual_loss` | `Evaporation Rate` | `Evaporacao` | — | |
| `use_state_variable` | (always state in PLEXOS) | (always state in SDDP) | — | gtopt opt-in for cuts. |
| `daily_cycle` | `Cycle = Day` | (Reservatorio Diario) | — | |

### C.7 Waterway

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Waterway` (class) | `Waterway` (or `Pipeline`) | (implicit in Hidreletrica `Jusante` topology, or `Canal`) | arc $(r_a \to r_b)$ | |
| `junction_a` / `junction_b` | `Storage From` / `Storage To` | `JusanteUsina` / `MontanteUsina` | — | |
| `capacity` | `Max Flow` | `VazaoMax` | $\bar Q$ | |
| `fmin` / `fmax` | `Min Flow` / `Max Flow` | `VazaoMinDefluencia` / `VazaoMaxDefluencia` | — | |
| `lossfactor` | `Loss Coefficient` | `FatorPerdas` | — | Filtration loss. |
| `fcost` | `Flow Cost` | `CustoVazao` | — | gtopt soft-flow penalty. |

### C.8 Turbine

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Turbine` (class) | (`Hydro Generator` properties) | (folded into `Hidreletrica` unit) | turbine *τ* | |
| `waterway` / `flow` | `Storage Discharge` | (implicit via Usina topology) | — | |
| `generator` | `Generator` | `UsinaGeradora` | $g(\tau)$ | |
| `production_factor` | `Production Coefficient` | `FProd` / `ProductionFactor` | $\rho_\tau$ (MW per m³/s) | |
| `efficiency` | `Efficiency` | `Eficiencia` | $\eta_\tau$ | |
| `capacity` | `Max Capacity` | `TurbinamentoMax` / `MaxTurbining` | $\bar Q_\tau$ | |
| `drain` (bool) | — | (Reversao / by-pass) | — | |
| `main_reservoir` | `Head Storage` | `Reservatorio` | — | For head-dependent factor. |

### C.9 Flow

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Flow` (class) | `Hydro Inflow` (timeseries) | `Vazao` / `Inflow` (per stage/scenario) | $w_t$ | gtopt's Flow = exogenous inflow injection. |
| `junction` | (Storage `Natural Inflow`) | `Posto` / `GaugeStation` | — | |
| `direction` | (sign) | (sign) | ±1 | |
| `discharge` | `Inflow` (m³/s × hours) | `Vazao` | $w_t$ (m³/s) | |

### C.10 Junction

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Junction` (class) | (implicit via Storage/Waterway endpoints) | (implicit via `Jusante`/`Montante` IDs) | hydraulic node | gtopt makes junctions first-class for graph clarity. |
| `drain` (bool) | (Storage with `Drain` flag) | (terminal/oceano) | — | |

### C.11 Fuel

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Fuel` (class) | `Fuel` | `Combustivel` / `Fuel` | fuel *f* | |
| `price` | `Price` (timeseries) | `Preco` / `Price` | $c_f$ | |
| `heat_content` | `Heat Content` (e.g. GJ/tonne) | `PoderCalorifico` / `HeatingValue` | $\beta_f$ | |
| `combustion_emission_factor` | `CO2 Content` | `FatorEmissaoCombustao` | $\epsilon_f^{comb}$ | |
| `upstream_emission_factor` | `Upstream Emissions` | `FatorEmissaoUpstream` | $\epsilon_f^{up}$ | |

### C.12 Converter

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Converter` (class) | `Link` / `Transformer` | `Conversor` / `DCLink` | — | gtopt's Converter is a 3-way coupler (battery↔generator↔demand). |
| `battery` / `generator` / `demand` | `Storage` / `Gen` / `Load` membership | — | — | |
| `conversion_rate` | `Efficiency` | `Eficiencia` | $\eta_c$ | |
| `capacity` / `expcap` / `expmod` / `capmax` | `Max Flow` / `Max Units Built` | `Capac` / `CapacInv` | — | |
| `annual_capcost` | `Build Cost × WACC` | `CFix` | — | |

### C.13 Pump

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Pump` (class) | `Generator` with `Pump Load`, or `Pump` class | `Bombeio` / `PumpedStorage` | pump *p* | |
| `waterway` / `demand` | `Storage From → To` / `Load` | (linked via Reversao) | — | |
| `pump_factor` | `Pump Efficiency` | `EficienciaBombeio` | $\eta^{pump}$ | |
| `efficiency` | (same) | `EfBomba` | — | |
| `capacity` | `Max Pump Load` | `PotBombeio` / `PumpCapacity` | $\bar P^{pump}$ | |
| `main_reservoir` | `Head Storage` | `ReservatorioMontante` | — | |

### C.14 LNG Terminal

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `LngTerminal` (class) | `Gas Storage` / `Gas Field` | `TerminalGNL` / `GasTerminal` (rare) | — | |
| `emin` / `emax` / `eini` / `efin` | `Min/Max/Initial/End Storage` | `Volume*` | — | Reuses storage semantics. |
| `sendout_max` / `sendout_min` | `Max/Min Send Out` | `EnvioMax` / `EnvioMin` | — | |
| `delivery` | `Delivery` (timeseries) | `EntregaContratual` | — | |
| `spillway_capacity` / `spillway_cost` | `Max Vent` / `Vent Penalty` | `Boil-Off` | — | |
| `mean_production_factor` | `Heat Rate` (gas→energy) | `FatorConversao` | $\rho_{gnl}$ | |
| `annual_loss` | `Boil-Off Rate` | `TaxaBoilOff` | — | |

### C.15 Commitment / SimpleCommitment

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Commitment` (class) | (Generator UC properties) | (Termoeletrica UC properties) | unit-commitment for *g* | |
| `generator` | (parent) | (parent) | $g$ | |
| `fuel` | `Fuel(s)` | `Combustivel` | — | |
| `fuel_cost` | `Fuel Price` | `PrecoCombustivel` | $c_f$ | |
| `startup_cost` | `Start Cost` | `CustoPartida` | $c_g^{SU}$ | |
| `shutdown_cost` | `Shutdown Cost` | `CustoDesligamento` | $c_g^{SD}$ | |
| `noload_cost` | `No-Load Cost` | `CustoSemCarga` | $c_g^{NL}$ | |
| `min_up_time` | `Min Up Time` | `TempoMinOperacao` / `MinUpTime` | $T_g^{up}$ | |
| `min_down_time` | `Min Down Time` | `TempoMinParado` / `MinDownTime` | $T_g^{down}$ | |
| `ramp_up` / `ramp_down` | `Max Ramp Up` / `Max Ramp Down` | `RampaSubida` / `RampaDescida` | $R_g^+, R_g^-$ | |
| `startup_ramp` / `shutdown_ramp` | `Start Up Ramp` / `Shut Down Ramp` | `RampaPartida` / `RampaParada` | — | |
| `initial_status` / `initial_hours` | `Initial On/Off` / `Hours Up/Down` | `EstadoInicial` / `HorasInicial` | $u_g^0, T_g^0$ | |
| `must_run` | `Commit = -1` / `Must Run` | `MustRun` | — | |
| `relax` | `Unit Commitment Optimality = Relax` | `Tipo = Linear` | — | |
| `hot_start_cost` / `warm_start_cost` / `cold_start_cost` | `Start Cost (Hot/Warm/Cold)` | `Custo*Start` | — | |
| `hot_start_time` / `cold_start_time` | `Hot/Cold Start Time` | `Tempo*Start` | — | |
| `commitment_period` | `Commitment Period` | (block discretization) | — | |
| `fuel_emission_factor` | `Emission Production Rate` | `FatorEmissao` | $\epsilon_g$ | |

### C.16 Reserve Zone

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `ReserveZone` (class) | `Reserve` (Region / Zone) | `Reserva` / `Reserve` | reserve region *z* | |
| `urreq` | `Min Provision` (Up) | `ReservaSubir` / `UpReserve` | $R_z^{up,req}$ | |
| `drreq` | `Min Provision` (Down) | `ReservaDescer` / `DownReserve` | $R_z^{dn,req}$ | |
| `urcost` | `VoRS` (Up) | `CustoFaltaReservaSubir` | $c^{up,fail}$ | |
| `drcost` | `VoRS` (Down) | `CustoFaltaReservaDescer` | $c^{dn,fail}$ | |

### C.17 Reserve Provision

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `ReserveProvision` (class) | (Generator → Reserve membership) | (Termoeletrica → Reserva) | provision *p* | |
| `generator` | (parent) | (parent) | $g$ | |
| `urmax` / `drmax` | `Max Provision` (Up/Down) | `MaxReservaSubir` / `MaxReservaDescer` | $\bar R_g^{up}, \bar R_g^{dn}$ | |
| `ur_capacity_factor` / `dr_capacity_factor` | `Reserve / Capacity` | `FatorCapacReserva` | — | |
| `ur_provision_factor` / `dr_provision_factor` | `Risk` / `Provision Factor` | `FatorProvisao` | — | |
| `urcost` / `drcost` | `Provision Cost` | `CustoReserva` | $c_g^{R}$ | |

### C.18 Inertia Zone / Inertia Provision

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `InertiaZone.requirement` | `Inertia Requirement` (newer PLEXOS) | (custom constraint) | $H_z^{req}$ | Synthetic inertia / RoCoF. |
| `InertiaZone.cost` | `Inertia VoRS` | (penalty in custom constraint) | — | |
| `InertiaProvision.inertia_constant` | `Inertia` (Generator) | `ConstanteInercia` | $H_g$ (s) | |
| `InertiaProvision.rated_power` | `Rated Capacity` | `PotRated` | $S_g$ (MVA) | |
| `InertiaProvision.provision_max` | `Max Inertia` | `MaxInercia` | — | |
| `InertiaProvision.provision_factor` | (Status-dependent) | `FatorInercia` | — | |
| `InertiaProvision.cost` | `Inertia Cost` | `CustoInercia` | — | |

### C.19 Generator Profile / Demand Profile

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `GeneratorProfile` (class) | `Rating Factor` (timeseries on Generator) | `Perfil` / `Profile` per Renovavel | $\bar p_g(t)$ | gtopt cap as fraction × `pmax`. |
| `DemandProfile` (class) | `Load` (timeseries pattern) | `Patamar` / `LoadCurve` | $D_d(t)$ | |

### C.20 Flow Right / Volume Right (irrigation, gtopt-specific)

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `FlowRight` / `VolumeRight` | `Constraint` (user-defined) | `RestricaoOutorga` (water-rights, newer) | — | gtopt domain-specific for the Chilean Maule/Laja agreements. |
| `fmin` / `fmax` | `Min/Max Flow` (constraint) | `VazaoMin` / `VazaoMax` | $\underline Q, \bar Q$ | |
| `target` / `priority` / `purpose` | (constraint metadata) | — | — | |
| `emin` / `emax` / `eini` / `efin` (VolumeRight) | `Min/Max Storage` (constraint) | `VolMin` / `VolMax` | — | |
| `saving_rate` / `demand` (VolumeRight) | — | — | — | gtopt-only economy/extraction tracker. |
| `reset_month` | — | — | — | gtopt-only annual reset for water-year accounting. |
| `fail_cost` / `fcost` | `Penalty` | `CustoFalta` | — | |

### C.21 Reservoir Seepage / Discharge Limit / Production Factor

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `ReservoirSeepage.*` | `Storage Inflow Loss` | `Filtracao` | — | Volume → flow leakage. |
| `ReservoirDischargeLimit.*` | `Max Release Limit` (volume-dependent) | `VazaoMaxAfluencia` | — | Outflow cap as function of volume. |
| `ReservoirProductionFactor.*` | `Energy Coefficient` (volume-dep) | `FProd(vol)` (table) | $\rho_r(V)$ | Volume-dependent MWh/hm³. |

### C.22 Converter / User Constraint / User Param

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `UserConstraint.expression` | `Constraint Equation` (LHS) | `RestricaoUsuario.Equacao` | — | gtopt uses AMPL-like syntax. |
| `UserConstraint.constraint_type` | `Sense` (`<=`, `=`, `>=`) | `Sinal` | — | |
| `UserConstraint.penalty` | `Penalty` | `CustoViolacao` | — | |
| `UserConstraint.penalty_class` | (per-band) | — | — | |
| `UserParam.value` | `Parameter` (named scalar) | `ParametroUsuario` | — | |

### C.23 Time Hierarchy

| gtopt | PLEXOS | SDDP | Literature | Notes |
|-------|--------|------|------------|-------|
| `Block` | `Period` / `Time Interval` | `Bloco` / `Patamar` / `LoadBlock` | block *b* | gtopt: hours. PLEXOS: minutes-to-hours. SDDP: typical 3-5 load blocks per stage. |
| `Stage` | `Horizon` step | `Estagio` / `Stage` | stage *t* | gtopt: weekly/monthly. SDDP: monthly typical. |
| `Scenario` | `Sample` | `Cenario` / `Scenario` | scenario *ω* | |
| `Phase` | (Multi-Horizon set) | `Fase` / `Phase` (newer SDDP) | — | gtopt's super-stage / SDDP phase. |
| `Scene` | `Scenario Group` | `GrupoCenarios` | — | gtopt super-scenario. |
| `Aperture` | (sample of inflow trajectory) | `Abertura` / `Aperture` | aperture *a* | SDDP-native term — gtopt borrows it. |
| `Iteration` | `Outer Iteration` | `Iteracao` / `Iteration` | $k$ | SDDP cut-generation iterations. |

### C.24 Sub-Options (model / SDDP / cascade / solver / monolithic / LP)

These are configuration sub-structs, not network elements. Listed
for completeness; mapping is approximate because each tool's
option surface is shaped by its solver pipeline.

| gtopt | PLEXOS analog | SDDP analog |
|-------|--------------|-------------|
| `model_options.use_kirchhoff` | `Transmission Detail = DC` | `Tipo_Rede = DC` |
| `model_options.use_single_bus` | `Transmission Detail = Copper Plate` | `Tipo_Rede = Transp / Uninodal` |
| `model_options.demand_fail_cost` | `VoLL` (global) | `CustoDeficitGlobal` |
| `model_options.reserve_fail_cost` | `VoRS` (global) | `CustoFaltaReserva` |
| `model_options.scale_objective` | `Scale Factor` (numerics) | (internal) |
| `model_options.annual_discount_rate` | `Discount Rate` (Project) | `TaxaDesconto` |
| `sddp_options.num_apertures` | (Stochastic Sample Count) | `NumeroAberturas` |
| `sddp_options.convergence_tol` | (Convergence Tolerance) | `TolConvergencia` |
| `cascade_options.level_array` | (Hierarchical Decomposition) | (Cascade levels, newer) |
| `solver_options.algorithm` | `Solver Method` (Simplex/IPM) | `MetodoSolver` |
| `monolithic_options.*` | (Monolithic vs decomposed) | `Tipo = Determinístico` |

### C.25 Coverage Summary

| Element type | gtopt fields | PLEXOS coverage | SDDP coverage |
|--------------|--------------|-----------------|---------------|
| Bus | 6 | ✅ full | ✅ full |
| Generator | 14 | ✅ full | ✅ full |
| Demand | 9 | ✅ full | ✅ full |
| Line | 17 | ✅ full | ✅ full |
| Battery | 17 | ✅ full | ✅ full |
| Reservoir | 21 | ✅ near-full | ✅ near-full (terms in PT) |
| Waterway | 6 | 🟡 partial (modeled differently) | 🟡 implicit in Hidreletrica |
| Turbine | 8 | 🟡 partial (folded into Generator) | 🟡 folded into Hidreletrica |
| Flow | 4 | ✅ as Hydro Inflow | ✅ as Vazao |
| Junction | 4 | ⚪ implicit only | ⚪ implicit only |
| Fuel | 5 | ✅ full | ✅ full |
| Converter | 8 | 🟡 partial (Link/Transformer) | 🟡 partial (DCLink) |
| Pump | 7 | ✅ full | ✅ full |
| LngTerminal | 12 | 🟡 partial (Gas Storage) | ⚪ rarely modeled |
| Commitment | 22 | ✅ full | ✅ full |
| ReserveZone / ReserveProvision | 11 | ✅ full | ✅ full |
| InertiaZone / InertiaProvision | 8 | 🟡 newer feature | ⚪ not standard |
| GeneratorProfile / DemandProfile | 2 | ✅ as Rating Factor | ✅ as Patamar / Perfil |
| FlowRight / VolumeRight | 25 | ⚪ user-constraint only | 🟡 newer water-rights schema |
| ReservoirSeepage / DischargeLimit / ProductionFactor | 9 | 🟡 partial | 🟡 partial |
| UserConstraint / UserParam | 7 | ✅ as Constraint | ✅ as RestricaoUsuario |
| Time hierarchy (Block/Stage/Scenario/Phase/Scene/Aperture) | 7 | 🟡 partial (different time model) | ✅ near-full |

**Legend**: ✅ direct equivalent, 🟡 partial or different shape,
⚪ no direct equivalent (gtopt-specific or implicit in the other tool).

### C.26 Naming-Pattern Observations

Three patterns emerge across the dictionary that should inform the
final naming-convention choice (§9):

1. **PLEXOS uses spaces and title-case** (`Max Capacity`, `Heat
   Rate Incr`, `Min Stable Level`). Verbose, GUI-oriented, but the
   semantics are always explicit. **PLEXOS never abbreviates.**
2. **SDDP mixes Portuguese abbreviations** (`CVU`, `GerMax`,
   `PotInst`) **with newer English equivalents** (`VariableCost`,
   `MaxGen`, `InstalledCapacity`) — the newer JSON schema is
   moving towards full English words but the legacy `.dat` field
   names remain as canonical.
3. **gtopt is closest to the legacy SDDP/Matpower style** —
   abbreviated lowercase (`gcost`, `pmax`, `lmax`) — which is the
   most terse but the least self-documenting. Modern tools
   (pandapower, PyPSA, newer SDDP/PLEXOS schemas) all moved away
   from this style.

**Implication for §6 / §9**: the proposed gtopt aliases
(`marginal_cost`, `max_demand`, `max_flow_ab`, `expansion_capacity`)
already sit in the same lexical neighborhood as the newer SDDP
English schema and PyPSA / pandapower, which is the right direction
for cross-tool interoperability and onboarding. The §9 registry
mechanism can serve as the bridge layer that lets the same gtopt
code accept either dialect.

### C.27 Collection / Container Names

Top-level JSON keys that hold arrays of elements. PLEXOS doesn't
have flat collections (every object lives in the class hierarchy),
so the "PLEXOS" column lists the **class container** used in the
XML export. SDDP organises one `.dat` file per element type, so the
"SDDP" column lists the historical file stem and the newer JSON
array key (where present).

| gtopt (current) | gtopt (proposed) | PLEXOS XML | SDDP `.dat` / JSON | pandapower | PyPSA | PowerModels.jl |
|-----------------|------------------|-----------|---------------------|------------|-------|----------------|
| `bus_array` | `buses` | `<Nodes>` | `sistema.dat` / `buses` | `bus` | `buses` | `bus` |
| `generator_array` | `generators` | `<Generators>` | `term.dat` / `thermals` (+ `renovavel.dat`) | `gen` / `sgen` | `generators` | `gen` |
| `demand_array` | `demands` | (per-Region `Load` property) | `carga.dat` / `loads` | `load` | `loads` | `load` |
| `line_array` | `lines` | `<Lines>` | `circuito.dat` / `circuits` | `line` | `lines` | `branch` |
| `battery_array` | `batteries` | `<Batteries>` | `bateria.dat` / `batteries` | `storage` | `storage_units` (+ `stores`) | `storage` |
| `converter_array` | `converters` | `<Links>` / `<Transformers>` | `dclink.dat` / `dc_links` | `trafo` | `links` | `dcline` |
| `reservoir_array` | `reservoirs` | `<Storages>` (Hydro) | `hidr.dat` / `hydros` (reservoir part) | — | (custom) | — |
| `junction_array` | `junctions` | (implicit on Storage) | (implicit `jusante` IDs) | — | — | — |
| `waterway_array` | `waterways` | `<Waterways>` | (implicit topology) | — | — | — |
| `turbine_array` | `turbines` | (Hydro `Generators`) | (folded into `hidr.dat`) | — | — | — |
| `flow_array` | `flows` | `<HydroInflows>` | `vazoes.dat` / `inflows` | — | — | — |
| `fuel_array` | `fuels` | `<Fuels>` | `combust.dat` / `fuels` | — | (per-`carrier` settings) | — |
| `pump_array` | `pumps` | `<Pumps>` | (reversao part of `hidr.dat`) | — | — | — |
| `lng_terminal_array` | `lng_terminals` | `<GasStorages>` | (rare; `terminal_gnl.dat`) | — | — | — |
| `commitment_array` | `commitments` | (per-Generator UC properties) | (per-Termoeletrica UC fields) | — | (`Generator` UC fields) | — |
| `simple_commitment_array` | `simple_commitments` | (Generator `Commit`) | (Termoeletrica `MustRun`) | — | — | — |
| `reserve_zone_array` | `reserve_zones` | `<Reserves>` | `reserva.dat` / `reserves` | — | — | — |
| `reserve_provision_array` | `reserve_provisions` | (Generator ↔ Reserve membership) | (per-Termoeletrica reserve cols) | — | — | — |
| `inertia_zone_array` | `inertia_zones` | `<Inertia>` (v9+) | (custom) | — | — | — |
| `inertia_provision_array` | `inertia_provisions` | (Generator `Inertia`) | (custom) | — | — | — |
| `generator_profile_array` | `generator_profiles` | (Generator `Rating Factor`) | `perfil_renovavel.dat` / `profiles_renewable` | — | (per-`Generator` `p_max_pu` series) | — |
| `demand_profile_array` | `demand_profiles` | (Region `Load` timeseries) | `patamar.dat` / `load_curves` | — | (per-`Load` `p_set` series) | — |
| `flow_right_array` | `flow_rights` | `<Constraints>` (user) | `outorga_vazao.dat` / `flow_rights` (newer) | — | — | — |
| `volume_right_array` | `volume_rights` | `<Constraints>` (user) | `outorga_volume.dat` / `volume_rights` (newer) | — | — | — |
| `reservoir_seepage_array` | `reservoir_seepages` | (Storage `Inflow Loss`) | (filtration entries in `hidr.dat`) | — | — | — |
| `reservoir_discharge_limit_array` | `reservoir_discharge_limits` | (Storage `Max Release` table) | (max-defluencia table) | — | — | — |
| `reservoir_production_factor_array` | `reservoir_production_factors` | (Storage `Energy Coef` table) | (FProd table) | — | — | — |
| `user_constraint_array` | `user_constraints` | `<Constraints>` | `restricao.dat` / `user_constraints` | — | (`global_constraints`) | — |
| `user_param_array` | `user_params` | (named scalar `Parameters`) | (`parametros.dat`) | — | — | — |
| `block_array` | `blocks` | `<Periods>` / `<Timeslices>` | `bloco.dat` / `blocks` | (timestep index) | (`snapshots`) | — |
| `stage_array` | `stages` | (Horizon `Steps`) | `estagio.dat` / `stages` | — | (`investment_periods`) | — |
| `scenario_array` | `scenarios` | (Sample set) | `cenario.dat` / `scenarios` | — | (scenario loop) | — |
| `phase_array` | `phases` | (Horizon group) | `fase.dat` / `phases` (newer) | — | — | — |
| `scene_array` | `scenes` | (Scenario group) | (newer scene grouping) | — | — | — |
| `aperture_array` | `apertures` | (Sample of inflow trajectory) | (per-Estagio aperture set) | — | — | — |
| `iteration_array` | `iterations` | (Outer-iter logs) | `iteracao.dat` / `iterations` | — | — | — |

#### C.27.1 Naming-pattern observations

- **gtopt is the outlier** with `<element>_array` everywhere. No
  comparator tool uses this pattern. PLEXOS uses class container
  tags (`<Generators>` plural, no suffix), SDDP uses plural English
  words in the newer JSON schema (`thermals`, `circuits`,
  `reserves`), and every Python tool (pandapower, PyPSA,
  PowerModels.jl) uses either singular (`gen`, `load`) or plural
  (`generators`, `loads`) but never an `_array` suffix.
- **Plural vs singular** splits the comparators roughly in half:
  PyPSA / PLEXOS / SDDP-new go plural, pandapower /
  PowerModels.jl / SDDP-old go singular. Plural reads more
  naturally for "this key holds many of these things" and is what
  the §6.1 Tier 1 proposal already picked.
- **Domain-specific collections** (`flow_right_array`,
  `volume_right_array`, `reservoir_seepage_array`,
  `reservoir_discharge_limit_array`,
  `reservoir_production_factor_array`) have no equivalent in the
  general-purpose tools. They map to **SDDP's newer water-rights
  schema** (`outorga_*`) and to **PLEXOS user-defined Constraint
  classes**, but the gtopt model is the most expressive of the
  three.
- **Time-axis collections** (`block_array`, `stage_array`,
  `scenario_array`, `phase_array`, `scene_array`, `aperture_array`,
  `iteration_array`) align well with SDDP's vocabulary
  (`bloco`, `estagio`, `cenario`, `fase`, `iteracao`, `abertura`)
  — unsurprising since gtopt's time model is closer to SDDP's
  than to PyPSA's flat snapshot list.

#### C.27.2 Registry integration

The collection renames fit the §9.4 registry as a fourth
sub-namespace alongside `names::demand::`, `names::generator::`,
`names::line::`:

```cpp
namespace gtopt::names::collections
{
  inline constexpr std::string_view kBusArray        = "bus_array";
  inline constexpr std::string_view kGeneratorArray  = "generator_array";
  // …
  inline constexpr std::array kBusArrayAliases       = {std::string_view {"buses"}};
  inline constexpr std::array kGeneratorArrayAliases = {std::string_view {"generators"}};
  // …
  inline constexpr FieldName Buses      {kBusArray,       kBusArrayAliases};
  inline constexpr FieldName Generators {kGeneratorArray, kGeneratorArrayAliases};
  // …
}
```

Then in `include/gtopt/json/json_system.hpp` and
`json_planning.hpp`, the daw::json contract uses
`names::collections::kBusArray` for the canonical entry and
`names::collections::kBusArrayAliases[0]` for the alias entry —
identical pattern to the per-element field aliases in §9.4.2.

**Phase ordering recommendation**: defer collection-key renames to
**Phase 2** (after Generator + Demand + Line field aliases land in
Phase 1) because:

1. Collection renames touch the `System` / `Planning` contracts —
   architecturally orthogonal to per-element field contracts.
2. Every case JSON file in `cases/` references collections by name
   — the alias coverage there is large enough that a separate PR
   gives cleaner review.
3. The §9.4 registry mechanism is proven by Phase 1 before being
   reused for collections.

---

## 9. Phase 1 Implementation Plan — 2026-05

This section supersedes §8 with a concrete, scoped plan ratified
2026-05-17. It picks one specific aliasing technique compatible with
gtopt's strict daw::json policy and lists the exact edits required.

### 9.1 Scoped Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| **Scope of change** | JSON-alias only | Zero C++ struct refactor. Old member names stay. Output dirs/files unchanged. Smallest safe step before a future C++ rename phase. |
| **Backward-compatibility window** | Indefinite | Old short names parse forever. No deprecation warnings, no removal release. |
| **Element families in Phase 1** | Generator + Demand + Line | Highest-traffic surface, hits all the cryptic prefix patterns (`g*`, `l*`, `t*_ab`, `f*`, `exp*`) in one pass. Battery / Reservoir / Reserve / Options postponed. |
| **Collection-key renames (`_array` → plural)** | Deferred | Touches every case JSON and every script writer. Worth doing separately under its own PR so review diffs stay reviewable. |
| **AMPL constexpr name constants** | Out of scope | Tracked under `feedback_no_magic_strings`. Phase 1 only touches the JSON wire format, not LP-side `*Name` constants. |

### 9.2 Technical Constraint — daw::json + StrictParsePolicy

gtopt parses with `StrictParsePolicy`
(`UseExactMappingsByDefault::yes`, see
`include/gtopt/json/json_parse_policy.hpp:33`).  Two facts force a
specific implementation shape:

1. **Strict rejection of unknown keys.** A user JSON containing only
   the new alias `max_demand` (and no `lmax`) will *fail* parsing
   unless the contract explicitly lists `max_demand`.

2. **daw::json has no multi-name member feature.** Each
   `json_*<"name", …>` entry in a `json_member_list` binds exactly
   one JSON key. The header
   `/usr/local/include/daw/json/daw_json_link_types.h:497` stores
   the name as a single `static constexpr daw::string_view name = Name;`.

This rules out the "just add an alias attribute to the member" approach
sketched in §7.  Three viable techniques remain:

| Option | How it works | Cost | Verdict |
|--------|--------------|------|---------|
| **A. Dual contract members** | Add a second optional member to the C++ struct (e.g. `OptTBRealFieldSched max_demand` alongside `lmax`) and list both in `json_data_contract`. Collapse aliases → canonical in a post-parse hook (or in `flatten()`). | Adds one `Opt*` field per alias to the host struct. Touches struct + JSON contract + collapse routine. | ✅ **Chosen** — minimally invasive, type-safe, no extra parse pass. |
| **B. JSON DOM rewrite pass** | Parse to `daw::json::json_value`, rewrite alias keys to canonical, re-serialize, parse again with the existing contract. | 1.5× parse cost on every input. No struct churn. | ❌ Performance hit unacceptable for IPLP-scale runs. |
| **C. Wrapper-view struct** | Define a `DemandJsonView` with all old + new fields, parse into it, then `collapse()` into `Demand`. | Doubles the type surface; new keys live in wrapper, not in `Demand`. Clean separation but every consumer of the JSON layer must learn the wrapper convention. | ❌ More architectural change than scope allows. |

**Phase 1 commits to Option A.** It localizes the alias logic to the
JSON-binding layer and the host struct's `merge()` (which already
exists for stage / scenario sched overlays).

### 9.3 Concrete Alias Surface — Phase 1

#### 9.3.1 Generator

Target: `Generator` and `GeneratorAttrs` in `include/gtopt/json/json_generator.hpp`.

| Canonical (kept) | Alias (new key) | Reference tool |
|------------------|-----------------|---------------|
| `pmin` | `min_power` | PyPSA `p_min_pu × p_nom` |
| `pmax` | `max_power` | PyPSA `p_max_pu × p_nom` |
| `gcost` | `marginal_cost` | PyPSA `marginal_cost` |
| `expcap` | `expansion_capacity` | GenX `Cap_Size` |
| `expmod` | `expansion_modules` | GenX `Max_Units_Built` |
| `capmax` | `max_capacity` | PyPSA `p_nom_max` |
| `annual_capcost` | `capital_cost` | PyPSA `capital_cost` |
| `annual_derating` | `derating_rate` | — |
| `integer_expmod` | `integer_expansion_modules` | — |

#### 9.3.2 Demand

Target: `Demand` and `DemandAttrs` in `include/gtopt/json/json_demand.hpp`.

| Canonical (kept) | Alias (new key) | Reference tool |
|------------------|-----------------|---------------|
| `lmax` | `max_demand` | pandapower `p_mw`, PyPSA `p_set` |
| `fcost` | `curtailment_cost` | PLEXOS `VOLL`, GenX `Lost_Load_Cost` |
| `emin` | `energy_min` | — |
| `ecost` | `energy_shortfall_cost` | — |
| `expcap` | `expansion_capacity` | (shared w/ Generator) |
| `expmod` | `expansion_modules` | (shared) |
| `capmax` | `max_capacity` | (shared) |
| `annual_capcost` | `capital_cost` | (shared) |
| `annual_derating` | `derating_rate` | (shared) |
| `integer_expmod` | `integer_expansion_modules` | (shared) |

#### 9.3.3 Line

Target: `Line` and `LineAttrs` in `include/gtopt/json/json_line.hpp`.

| Canonical (kept) | Alias (new key) | Reference tool |
|------------------|-----------------|---------------|
| `tmax_ab` | `max_flow_ab` | PyPSA `s_nom`, PowerModels `rate_a` |
| `tmax_ba` | `max_flow_ba` | (same) |
| `tcost` | `transfer_cost` | PLEXOS `Wheeling Charge` |
| `expcap` | `expansion_capacity` | (shared) |
| `expmod` | `expansion_modules` | (shared) |
| `capmax` | `max_capacity` | (shared) |
| `annual_capcost` | `capital_cost` | (shared) |
| `annual_derating` | `derating_rate` | (shared) |
| `integer_expmod` | `integer_expansion_modules` | (shared) |

Endpoint naming (`bus_a` / `bus_b`) stays per §5.7.

### 9.4 Shared Name Registry (JSON ↔ AMPL)

A pain point with the §9.3 alias surface is **double maintenance**:
adding the alias `max_demand` requires touching the daw::json
contract in `json_demand.hpp` *and* the AMPL resolver in
`source/element_column_resolver.cpp`, which today reads:

```cpp
if (ref.element_type == "demand") {
  if (ref.attribute == "lmax")  return dem.param_lmax(suid, buid);
  if (ref.attribute == "fcost") return dem.param_fcost(suid);
  …
}
```

Note that line 377 already carries an ad-hoc alias chain
`tmax || tmax_ab` — proof that this surface needs aliases today, and
that the current solution scales poorly. Combined with the
[`no-magic-strings`](#) feedback memory (AMPL variable/attribute
names should never appear as string literals), the right move is a
**single compile-time registry** that both the JSON layer and the
AMPL resolver consult.

#### 9.4.1 Registry Header

New header `include/gtopt/element_names.hpp`:

```cpp
namespace gtopt::names
{

/// One canonical name + zero or more JSON / AMPL aliases.
/// All strings are program-lifetime; the spans never own storage.
struct FieldName
{
  std::string_view canonical;
  std::span<const std::string_view> aliases {};
};

/// Helper: does `key` match the canonical name or any alias?
[[nodiscard]] constexpr bool matches(
    const FieldName& f, std::string_view key) noexcept
{
  if (f.canonical == key) return true;
  for (auto a : f.aliases) if (a == key) return true;
  return false;
}

namespace demand
{
  inline constexpr std::string_view kLmax  = "lmax";
  inline constexpr std::string_view kFcost = "fcost";
  inline constexpr std::string_view kEmin  = "emin";
  inline constexpr std::string_view kEcost = "ecost";
  // … expansion fields shared via gtopt::names::capacity::*

  inline constexpr std::array kLmaxAliases  = {std::string_view {"max_demand"}};
  inline constexpr std::array kFcostAliases = {std::string_view {"curtailment_cost"}};
  inline constexpr std::array kEminAliases  = {std::string_view {"energy_min"}};
  inline constexpr std::array kEcostAliases = {std::string_view {"energy_shortfall_cost"}};

  inline constexpr FieldName Lmax  {kLmax,  kLmaxAliases};
  inline constexpr FieldName Fcost {kFcost, kFcostAliases};
  inline constexpr FieldName Emin  {kEmin,  kEminAliases};
  inline constexpr FieldName Ecost {kEcost, kEcostAliases};
}  // namespace demand

namespace generator { … }   // pmin/min_power, pmax/max_power,
                            // gcost/marginal_cost, …
namespace line      { … }   // tmax_ab/max_flow_ab, tmax_ba/max_flow_ba,
                            // tcost/transfer_cost, …
namespace capacity  { … }   // expcap, expmod, capmax, annual_capcost,
                            // annual_derating, integer_expmod
                            // (shared by every expandable element)

}  // namespace gtopt::names
```

All names live in a single TU-free header. Adding an alias is one
edit (the `kFooAliases` array). Removing the §9.3 ad-hoc `tmax ||
tmax_ab` chain becomes a registry lookup.

#### 9.4.2 JSON Side Uses the Registry

The daw::json contracts replace raw string literals with the
constexpr names from the registry. `JSONNAMETYPE` accepts a literal
`std::string_view` NTTP in the daw-json-link version vendored at
`/usr/local/include/daw/json/`, so:

```cpp
// include/gtopt/json/json_demand.hpp
#include <gtopt/element_names.hpp>

template<>
struct json_data_contract<DemandAttrs>
{
  using type = json_member_list<
      json_variant<"bus", SingleId>,
      // canonical
      json_variant_null<names::demand::kLmax,
                        OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      // alias — same target type, different JSON key
      json_variant_null<names::demand::kLmaxAliases[0],
                        OptTBRealFieldSched, jvtl_TBRealFieldSched>,
      …>;
  …
};
```

A static assertion in `element_names.hpp` enforces single-alias-per-
field for Phase 1, so `[0]` is unambiguous. (Multi-alias support, if
ever needed, would generate the contract entries via a macro fan-out.)

If `JSONNAMETYPE` turns out *not* to accept a constexpr `string_view`
under the vendored daw-json version (worth a 10-line compile probe
before committing), fall back to a hand-written `#define`:

```cpp
#define GTOPT_NAME_DEMAND_LMAX        "lmax"
#define GTOPT_NAME_DEMAND_MAX_DEMAND  "max_demand"
```

and have the registry header derive `kLmax` from the same macro.
The macro is then the single source of truth.

#### 9.4.3 AMPL Side Uses the Registry

The resolver chain in `element_column_resolver.cpp` collapses from a
per-attribute `if`-ladder to a registry-driven lookup. Sketch:

```cpp
// One entry per (canonical, getter) pair, defined alongside the
// per-element resolver block.  Order: most-frequent first.
namespace {
struct DemandAttr { FieldName name; DemandLP::ParamGetter get; };

constexpr std::array kDemandAttrs = {
  DemandAttr{names::demand::Lmax,  &DemandLP::param_lmax_stage_block},
  DemandAttr{names::demand::Fcost, &DemandLP::param_fcost_stage},
  DemandAttr{names::demand::Emin,  &DemandLP::param_emin_stage},
  DemandAttr{names::demand::Ecost, &DemandLP::param_ecost_stage},
};
}  // namespace

if (ref.element_type == "demand") {
  const auto& dem = sc.get_element(ObjectSingleId<DemandLP>{single_id});
  for (const auto& a : kDemandAttrs) {
    if (matches(a.name, ref.attribute)) {
      return (dem.*a.get)(suid, buid);  // signature normalized
    }
  }
  return std::nullopt;
}
```

Two consequences:

1. **Alias acceptance is automatic.** `ref.attribute == "max_demand"`
   matches `names::demand::Lmax` because the registry entry holds the
   alias. The hardcoded `tmax || tmax_ab` chain in the current
   resolver goes away — `Line.Tmax_ab` carries `max_flow_ab` *and*
   the historical `tmax` as aliases in one place.

2. **No more bare `"lmax"` literals.** Every site that referenced an
   attribute by string now references the constexpr constant. Future
   refactors (per the `no-magic-strings` memory) only edit the
   registry.

#### 9.4.4 Compile-Time Cross-Check

A single `static_assert` block at the end of `element_names.hpp`
encodes the cross-side invariant:

```cpp
// Every JSON contract member key must come from the registry.
// Enforced by linting `json_*.hpp` for raw string literals in
// `json_*<"…", …>` template args — a 30-line check in
// `tools/check_json_names.py`, registered as a ctest entry.
```

The lint check (script) walks `include/gtopt/json/*.hpp`, extracts
every quoted name in a `json_*<"…", …>` template, and asserts each
appears either as a canonical or alias entry in the registry header.
Same script also walks `element_column_resolver.cpp` for bare
`ref.attribute == "…"` chains and flags any that don't come from a
`names::*` constant.

#### 9.4.5 Why a Compile-Time Registry (Not a Runtime Map)

- **No allocation, no init-order hazards.** The registry is a header
  of `constexpr` `std::string_view` arrays — usable in template
  parameters and `constexpr` contexts.
- **daw::json needs NTTP-friendly strings.** Runtime `std::map` lookups
  cannot drive `json_*<NAME, …>` template parameters; constexpr can.
- **The AMPL resolver is on the hot path** for any user constraint —
  a `std::array` linear scan over ≤10 entries beats a `flat_map` lookup
  by a wide margin and stays in icache.

### 9.5 Implementation Recipe (Option A in detail)

For each canonical field `X` that gains alias `Y`:

1. **Add an alias field to the host C++ struct** in
   `include/gtopt/<element>.hpp` (and the matching `<element>_attrs`):
   ```cpp
   OptTBRealFieldSched lmax;       // canonical (unchanged)
   OptTBRealFieldSched max_demand; // alias — folded into lmax at merge() time
   ```
   Mark the alias members with a brief `// alias` comment so future
   readers know they are not first-class fields.

2. **Extend the daw::json contract** in
   `include/gtopt/json/json_<element>.hpp`, sourcing every key from
   the registry (no raw string literals):
   ```cpp
   #include <gtopt/element_names.hpp>
   …
   json_variant_null<names::demand::kLmax,
                     OptTBRealFieldSched, jvtl_TBRealFieldSched>,
   json_variant_null<names::demand::kLmaxAliases[0],
                     OptTBRealFieldSched, jvtl_TBRealFieldSched>,
   ```
   Update `to_json_data(...)` to forward both members in order. The
   collapse step (#3) guarantees `max_demand` is empty before
   serialization, so output JSON always uses the canonical key.

3. **Collapse aliases → canonical** in the struct's existing
   `merge(other)` method (or, if no merge exists, in a new
   `canonicalize()` method called once after `from_json`):
   ```cpp
   if (max_demand.has_value()) {
     if (lmax.has_value()) {
       throw std::runtime_error(
         std::format("Demand uid={} sets both 'lmax' and 'max_demand' — pick one", uid));
     }
     lmax = std::move(max_demand);
     max_demand.reset();
   }
   ```
   Throwing on dual-set matches the existing strict-parse philosophy
   and prevents silent precedence bugs.

4. **Pick the canonicalize call site.** The natural place is the
   end of `parse_planning_files()` in
   `source/gtopt_json_io_parse.cpp` (or wherever the per-element
   `merge()` chain finishes). Walk `planning.system.demand_array`,
   `.generator_array`, `.line_array` and call `canonicalize()` on
   each element + its profiled `*Attrs`.

5. **No change** to LP code, output files, output column names, case
   JSON files, scripts, or docs other than `docs/input-data.md`.

### 9.6 Files Touched (Phase 1)

| File | Change |
|------|--------|
| `include/gtopt/element_names.hpp` | **New.** Registry of canonical names + aliases (§9.4.1). |
| `include/gtopt/generator.hpp` | Add 9 alias fields to `Generator` + `GeneratorAttrs`. |
| `include/gtopt/demand.hpp` | Add 10 alias fields to `Demand` + `DemandAttrs`. |
| `include/gtopt/line.hpp` | Add 9 alias fields to `Line` + `LineAttrs`. |
| `include/gtopt/json/json_generator.hpp` | Replace raw `"pmin"`/`"pmax"`/`"gcost"`/… with `names::generator::*` constants + alias entries. |
| `include/gtopt/json/json_demand.hpp` | Replace raw `"lmax"`/`"fcost"`/… with `names::demand::*` constants + alias entries. |
| `include/gtopt/json/json_line.hpp` | Replace raw `"tmax_ab"`/`"tcost"`/… with `names::line::*` constants + alias entries. |
| `source/generator.cpp` / `source/demand.cpp` / `source/line.cpp` | Add `canonicalize()` (or extend `merge()`) with the alias-fold logic. |
| `source/gtopt_json_io_parse.cpp` | Call `canonicalize()` on every element after parse. |
| `source/element_column_resolver.cpp` | Replace per-attribute `if`-ladder for generator/demand/line with the registry-driven lookup of §9.4.3. Drops the hardcoded `tmax || tmax_ab` chain. |
| `tools/check_json_names.py` | **New.** Lint script (§9.4.4) registered as a ctest entry. |
| `test/source/test_json_aliases.cpp` | New test (see §9.7). |
| `test/source/test_element_names_registry.cpp` | New unit test for `matches()` and registry round-trip. |
| `docs/input-data.md` | Add "Field-name aliases" subsection per element. |

Estimated diff size: ~450 lines added (registry header is ~120),
~80 removed (resolver `if`-ladders collapse). No case JSON or
script writer touched.

### 9.7 Test Plan

#### 9.7.1 Registry self-test (`test_element_names_registry.cpp`)

- `matches(names::demand::Lmax, "lmax")` → `true`.
- `matches(names::demand::Lmax, "max_demand")` → `true`.
- `matches(names::demand::Lmax, "Lmax")` → `false` (case-sensitive).
- `matches(names::line::Tmax_ab, "tmax_ab")` and `…, "max_flow_ab"`
  both `true`; `…, "tmax_ba"` is `false`.
- Compile-time check: `static_assert(matches(names::demand::Lmax, "lmax"))`.
- Compile-time uniqueness: `static_assert` that the union of
  canonical + alias strings across all `names::demand::*` entries
  has no duplicates (prevents `"lmax"` accidentally appearing as
  both canonical of one field and alias of another).

#### 9.7.2 JSON aliases (`test_json_aliases.cpp`)

One `TEST_CASE` per element:

1. **`"Generator JSON aliases — marginal_cost ↔ gcost"`**
   - Parse a minimal JSON with `marginal_cost: 50` (and no `gcost`).
     `CHECK(gen.gcost.value().value(0) == doctest::Approx(50.0))`.
   - Round-trip: serialize the same `Generator` and confirm output
     uses `gcost`, not `marginal_cost`.
   - Negative test: JSON sets both → expect `from_json` to throw with
     a message mentioning both keys.
   - Repeat the pattern for `min_power`, `max_power`,
     `expansion_capacity`, `expansion_modules`, `max_capacity`,
     `capital_cost`, `derating_rate`, `integer_expansion_modules`.

2. **`"Demand JSON aliases — max_demand ↔ lmax"`**
   - Same triple (alias-only / canonical-only / both → throw) for the
     10 demand fields.

3. **`"Line JSON aliases — max_flow_ab ↔ tmax_ab"`**
   - Same triple for the 9 line fields. Add one extra subcase that
     parses a full `Planning` with a single line written in alias
     form and confirms the LP build (`gtopt_main` with
     `use_single_bus=true`, `--lp-only`-equivalent path) succeeds
     and that the resulting `line.tmax_ab` schedule matches the
     equivalent canonical JSON.

4. **`"Strict-policy unknown key is still rejected"`**
   - A JSON with `random_nonsense_key: 0` must still fail with the
     existing strict-parse error. This guards against a regression
     where someone weakens the parse policy to "fix" alias support.

Coverage target: 100% line coverage on the new `canonicalize()`
methods (every alias branch executed) and on the alias-rejection
arm of `from_json`.

#### 9.7.3 AMPL resolver aliases (`test_element_column_resolver_aliases.cpp`)

Drive `resolve_param` (or equivalent entry point of
`source/element_column_resolver.cpp`) with hand-built `ElementRef`s:

1. **Resolver accepts alias attribute names.**
   `resolve_param({"demand", uid=1, attribute="max_demand", scen, stage, block})`
   returns the same value as `attribute="lmax"`. Repeat for
   `marginal_cost ↔ gcost`, `max_flow_ab ↔ tmax_ab`,
   `transfer_cost ↔ tcost`.
2. **Unknown attribute returns nullopt** (not a hard throw — current
   resolver returns `std::nullopt` for unknown attributes; the new
   registry-driven version must preserve that contract).
3. **Historical aliases preserved.** `attribute="tmax"` still resolves
   to `Line.tmax_ab` (the old hardcoded chain). Listed explicitly in
   the registry so this is now data, not code.

#### 9.7.4 JSON-name lint (`check_json_names.py`)

Registered as a `script`-labelled ctest target. Scans:

- `include/gtopt/json/*.hpp` for `json_*<"…", …>` raw literal names.
  Fails if any literal is not listed as a `kFoo` / `kFooAliases[*]`
  in `element_names.hpp`.
- `source/element_column_resolver.cpp` for `ref.attribute == "…"`
  literal compares (the lint only flags the *generator / demand /
  line* blocks for Phase 1; other element blocks remain on raw
  strings until their phase lands).

### 9.8 Out of Phase 1 — Tracked for Phase 2+

The registry mechanism (§9.4) is reusable as-is — each future
phase only adds new `names::<element>::` sub-namespaces and an extra
`json_data_contract` block + resolver arm:

| Item | Section reference | Registry impact |
|------|-------------------|----------------|
| Plural collection keys (`generator_array` → `generators`) | §6.1 Tier 1 | `names::collections::` sub-namespace; same Option-A technique applied at the `System` / `Planning` contract level. Touches every case JSON. Schedule as its own PR. |
| Battery / Reservoir storage fields (`emin`, `emax`, `eini`, `efin`) | §6.1 Tier 2 | Reuse `names::storage::` shared between Battery and Reservoir. Small surface. |
| Reserve fields (`urreq`, `drreq`, `urmax`, `drmax`, `urcost`, `drcost`) | §6.1 Tier 3 | `names::reserve::` sub-namespace. Resolver arm joins the registry-driven list. |
| Option renames (`demand_fail_cost` → `lost_load_cost`) | §6.1 Tier 4 | Lives on `Options`, not on elements — `names::options::` sub-namespace; the same Option-A canonicalize step in `Options::merge`. |
| C++ struct-member renames | §5.2, §7 | Future phase, requires touching LP code that reads `demand.lmax`. Registry remains the source of truth; only the `param_*` accessor names change. |
| Output CSV column renames | §7.1 | Future phase, breaking change for downstream tooling. Same registry can drive output-header generation. |
| AMPL `*Name` constexpr renames (`DemandLP::LoadName` etc.) | `feedback_no_magic_strings` | **Subsumed by Phase 1 registry.** Move the LP-side `*Name` constants into `names::<element>::lp::*` and have `DemandLP::LoadName` alias the registry constant — single source of truth across input JSON, AMPL resolver, and LP variable registration. |

### 9.9 Acceptance Criteria

Phase 1 is done when:

- [ ] `test_element_names_registry.cpp` passes (registry self-test).
- [ ] `test_json_aliases.cpp` passes under `ctest -j20`.
- [ ] `test_element_column_resolver_aliases.cpp` passes — both alias
      and canonical attribute names resolve to the same parameter
      value.
- [ ] `check_json_names.py` lint target passes — no raw string
      literal in `json_{generator,demand,line}.hpp` or in the
      generator/demand/line arms of `element_column_resolver.cpp`.
- [ ] The hardcoded `if (ref.attribute == "tmax" || ref.attribute ==
      "tmax_ab")` chain in `element_column_resolver.cpp:377` is
      replaced by a registry lookup (proves the design eliminates
      ad-hoc alias chains).
- [ ] An IPLP-scale case (`support/juan/IPLP/`) parses identically
      whether the JSON uses canonical or alias keys (gold-file
      comparison on the planning struct).
- [ ] A PAMPL fragment that references `demand.max_demand[…]`
      resolves to the same column as one referencing
      `demand.lmax[…]` — end-to-end JSON↔AMPL consistency check.
- [ ] `clang-tidy` clean on the modified `*.cpp` files.
- [ ] `docs/input-data.md` lists every Phase 1 alias and points to
      `element_names.hpp` as the source of truth.
- [ ] No existing test required modification.
- [ ] No measurable regression in `parse_planning_files()` wall time
      on the juan IPLP case (±2% noise band).

---

## 10. Runtime Dialect Switching — 2026-05

This section designs the **multi-dialect** extension on top of the
§9 registry: lets the user pick the **naming vocabulary** for both
input and output at runtime, so the same gtopt binary can ingest a
PyPSA case, a PLP `.dat`-derived JSON, or a PLEXOS export — and emit
results in whichever vocabulary the downstream tool expects.

### 10.1 Decisions (user-confirmed 2026-05-17)

| Decision | Choice |
|----------|--------|
| Scope | **Full bidirectional** — `--input-dialect` and `--output-dialect`, **plus per-file `"dialect": "..."` header** so cases self-describe. |
| Dialects in scope | `gtopt` (canonical), `pypsa`, `pandapower`, `plexos`, `sddp`, **`plp`** (PLP Fortran lineage; mapping derived from `scripts/plp2gtopt/`). |
| Orphan policy (fields with no equivalent in the chosen dialect) | **Fall back to canonical gtopt name**. Output JSON is mixed-dialect in that case; round-trip is preserved. |
| Default `--input-dialect` | `auto` — accept any known dialect name (§9 behavior, no breaking change). |
| Default `--output-dialect` | `gtopt` — canonical names (matches today's output). |

### 10.2 Per-File Dialect Header

A JSON case file may declare its dialect at the top:

```json
{
  "dialect": "pypsa",
  "buses":      [{"name": "b1", …}],
  "generators": [{"p_nom": 100, "marginal_cost": 25, …}]
}
```

The dialect header has two effects on input:

1. **Tightens** key acceptance to that dialect's vocabulary plus
   canonical `gtopt` (orphans fall back to canonical, so a PyPSA
   case can still set the gtopt-only `saving_rate` if it needs to).
2. **Validates** that no other dialect's keys appear — catches
   accidental mix-and-match (`p_nom` next to `max_power`).

If `"dialect"` is absent, the parser uses `--input-dialect`
(default `auto`). CLI flag overrides the file header only when
explicitly passed.

### 10.3 Registry Extension

The §9.4 `FieldName` struct grows from `{canonical, aliases[]}` to
`{canonical, dialect_names[]}`:

```cpp
namespace gtopt::names
{

enum class Dialect : uint8_t {
  gtopt = 0,    ///< canonical (default; always accepted)
  pypsa,
  pandapower,
  plexos,
  sddp,
  plp,
  count_        ///< sentinel for array sizing
};

struct DialectName {
  Dialect dialect;
  std::string_view name;
};

struct FieldName {
  std::string_view canonical;          ///< gtopt-dialect name (always)
  std::span<const DialectName> alts;   ///< per-dialect alternates
};

/// True when `key` matches the canonical name or any alternate
/// whose dialect is in the allowed set.
[[nodiscard]] constexpr bool matches(
    const FieldName& f, std::string_view key,
    DialectMask allowed) noexcept;
}
```

Where `DialectMask` is a `std::bitset<count_>` selecting which
dialects' alternates are honored on input. `--input-dialect=auto`
sets every bit; `--input-dialect=plp` sets only `gtopt | plp`.

Adding a dialect for an existing field is one line:

```cpp
inline constexpr std::array kPmaxAlts = {
  DialectName{Dialect::pypsa,      "p_nom"},
  DialectName{Dialect::pandapower, "max_p_mw"},
  DialectName{Dialect::plexos,     "Max Capacity"},
  DialectName{Dialect::sddp,       "PotInst"},
  DialectName{Dialect::plp,        "PotMax"},
};
inline constexpr FieldName Pmax{"pmax", kPmaxAlts};
```

### 10.4 Input-Side Implementation

The §9.4 daw::json contract is built once with **every** canonical
name listed; alternate keys are not part of the contract. Instead,
a pre-parse **key-normalization pass** rewrites alternate keys to
canonical *before* handing the JSON to daw::json:

```cpp
// 1. Parse JSON into a daw::json::json_value DOM (cheap; no
//    contract validation yet).
auto dom = daw::json::json_value{json_text};

// 2. Read top-level "dialect" if present; combine with CLI mask.
auto mask = compute_mask(cli_input_dialect, dom["dialect"]);

// 3. Walk dom; for every object key that's a known alternate
//    under `mask`, rewrite to canonical.  Unknown keys are left
//    alone — daw::json's strict mode will reject them next.
walk_and_canonicalize(dom, mask, gtopt::names::registry);

// 4. Re-emit the dom and parse with the existing strict contract.
auto canonical_json = serialize(dom);
auto planning = daw::json::from_json<Planning>(
    canonical_json, gtopt::StrictParsePolicy);
```

Performance: one extra DOM-walk pass over the JSON. On the juan
IPLP case (~12 MB JSON), expected overhead is ~50–100 ms — small
relative to the LP-build time.

The DOM step also reports a structured error for *strict-dialect*
violations: "key `gcost` appears under `\"dialect\": \"pypsa\"` —
did you mean `marginal_cost`?". The error mentions the canonical
name and every dialect's alternate, so the diagnostic is self-
documenting.

### 10.5 Output-Side Implementation

Same idea, mirrored: serialize via the existing daw::json contract
(produces canonical-name JSON), then walk the DOM and rewrite
canonical keys to the requested `--output-dialect` alternate where
one exists. Orphans (no alternate in that dialect) stay on the
canonical key per the fallback decision.

The top-level `"dialect": "<output-dialect>"` is injected so the
file can be re-read without a CLI flag.

```cpp
auto canonical_json = daw::json::to_json(planning);
auto dom = daw::json::json_value{canonical_json};
inject_dialect_header(dom, cli_output_dialect);
rewrite_keys_to_dialect(dom, cli_output_dialect, registry);
return serialize(dom);
```

### 10.6 CLI Surface

```text
--input-dialect=auto|gtopt|pypsa|pandapower|plexos|sddp|plp
                       (default: auto)
--output-dialect=gtopt|pypsa|pandapower|plexos|sddp|plp
                       (default: gtopt)
--allow-orphans=true|false
                       (default: true)
                       false → refuse to serialize fields with no
                       equivalent in --output-dialect; refuse to
                       parse fields under a strict --input-dialect
                       that have no entry in that dialect.
--list-dialects        prints the registry per dialect and exits
```

CLI overrides the JSON header. When CLI is unset and the header is
absent, `auto` is used. `--allow-orphans` has no JSON-header
counterpart — it is a pure runtime policy choice.

### 10.7 Orphan Handling

A field is an "orphan" for dialect `D` when its `FieldName.alts`
has no `DialectName{D, …}` entry. Examples:

- `VolumeRight.saving_rate` is gtopt-only — no alternate in any
  dialect.
- `Battery.input_efficiency` has `pypsa: efficiency_store` but no
  `pandapower` alternate (pandapower doesn't model storage
  efficiency).

**Default (`--allow-orphans=true`, recommended).** Output retains
the canonical gtopt name for orphan fields. The output JSON is
**mixed-dialect** in the strict sense, but round-trip is
guaranteed: re-reading the file with `--input-dialect=auto` (or
with the header's declared dialect plus `--allow-orphans=true`)
yields the same `Planning` object. This is the right default
because:

1. It preserves information (lossless round-trip) for every field
   gtopt models, regardless of dialect coverage.
2. It matches §9's "indefinite back-compat aliases" stance — the
   tool should accept and emit valid data first, validate
   vocabulary second.
3. Many gtopt option fields (~60 of ~170 in Appendix E.14) have
   no equivalent in any other tool. With orphan strictness
   defaulting to error, every non-trivial gtopt case would fail
   to serialize under any non-gtopt dialect — defeating the
   feature.

**Strict mode (`--allow-orphans=false`, opt-in).** Useful when a
downstream tool ingests the JSON and would silently misinterpret a
canonical-gtopt key it doesn't recognize. Behavior:

- On **output**: serializer refuses to emit an orphan; reports the
  list of orphan fields with their canonical names and exits
  non-zero. The user must either change `--output-dialect`, accept
  `--allow-orphans=true`, or remove the offending fields from
  the input.
- On **input**: parser rejects any orphan key under a strict
  `--input-dialect=X` (X ≠ `auto`); reports the list of unknown-
  to-dialect keys with the canonical name they would map to under
  `--input-dialect=auto`. Same exit-non-zero behavior.

Strict mode ships in §10-C (not deferred), but the default value
ensures `--allow-orphans` is invisible to users who don't ask for
it.

### 10.8 Implementation Phases

This section's design is **deferred until §9 Phase 1 lands**. Once
the registry exists and the JSON-alias plumbing is proven,
multi-dialect is incremental:

| Phase | Scope | Depends on |
|-------|-------|-----------|
| **§10-A** | Registry refactor: `aliases[]` → `alts[DialectName]`. CLI flag `--list-dialects`. No behavior change beyond §9. | §9 Phase 1 |
| **§10-B** | Input-side DOM-rewrite pass + `--input-dialect` + per-file `"dialect"` header. `auto` default preserves §9 behavior. | §10-A |
| **§10-C** | Output-side rewrite pass + `--output-dialect` flag + `--allow-orphans=true\|false` (default `true`). Default `--output-dialect=gtopt` preserves today's output. Strict mode (`--allow-orphans=false`) and its diagnostics ship in this phase, not deferred. | §10-B |
| **§10-D** | Verbose mismatch diagnostics polish: "did you mean…?" suggestions across dialects, structured JSON-pointer error paths, machine-readable error format for CI gates. | §10-C |
| **§10-E** | Verify PLEXOS and SDDP name lists against authoritative current docs (see §10.10) before they ship as production dialects. The `plp` and `pypsa` and `pandapower` lists are derived from code in this repo (and from well-documented stable Python packages) and can ship in §10-B without an `--experimental` gate. | §10-A |

### 10.9 Test Plan Additions

On top of §9.7:

1. **`test_dialect_input_pypsa`** — Parse a fixture JSON written
   in PyPSA vocabulary (`p_nom`, `marginal_cost`, `bus0`/`bus1`,
   `p_set`, `s_nom`). Assert the resulting `Planning` matches the
   gtopt-canonical equivalent fixture byte-for-byte after
   `to_json`.
2. **`test_dialect_input_plp`** — Parse a JSON with
   `"dialect": "plp"` that uses PLP names (`PotMax`, `CVar`,
   `BusA`/`BusB`, `Demanda`, `BatNom`). Assert same equivalence.
3. **`test_dialect_input_strict_mismatch`** — JSON with
   `"dialect": "pypsa"` but containing `gcost` (a non-PyPSA name)
   must fail with a diagnostic naming both `gcost` and
   `marginal_cost`.
4. **`test_dialect_output_round_trip`** — For each dialect *D*,
   serialize with `--output-dialect=D`, re-parse with
   `--input-dialect=D`, confirm `Planning` equality.
5. **`test_dialect_orphan_fallback`** — Output with
   `--output-dialect=pandapower` on a case that uses
   `VolumeRight.saving_rate`. Confirm `saving_rate` appears under
   its canonical name and the file still round-trips.
6. **`test_dialect_cli_overrides_header`** — File declares
   `"dialect": "plexos"`, CLI passes `--input-dialect=pypsa`. CLI
   wins; PLEXOS-only names in the file fail validation.

### 10.10a Scope: JSON-Visible vs CLI-Only Options

gtopt already exposes a **dialect-independent override mechanism**:
`--set <canonical-path>=<value>` accepts any option using its
canonical C++/JSON struct path (e.g.
`--set model_options.demand_fail_cost=5000`,
`--set sddp_options.num_apertures=10`,
`--set solver_options.threads=8`). Because `--set` parses the
dotted path against the canonical struct hierarchy, the value is
applied **regardless of what dialect the JSON case file uses**.

Implication for the dialect registry: only options that **actually
appear in user-edited JSON case files** need a dialect alias.
Options that users typically only set via `--set` (numerical
tuning, debug flags, low-memory knobs) do not — `--set` covers
their interop need already.

This splits the option surface into two tiers:

**Tier J — JSON-visible options** (need dialect aliases):

- Per-element properties: every gtopt field already covered by §9
  (`pmax`, `lmax`, `tmax_ab`, …).
- Top-level economics & physics: `annual_discount_rate`, `method`,
  `model_options.use_kirchhoff`, `use_single_bus`,
  `demand_fail_cost`, `reserve_fail_cost`, `hydro_fail_cost`,
  `hydro_use_value`, `emission_cost`, `emission_cap`.
- I/O paths: `input_directory`, `output_directory`,
  `input_format`, `output_format`, `output_compression`.
- High-level method controls: `sddp_options.num_apertures`,
  `max_iterations`, `convergence_tol`, `cut_directory`,
  `boundary_cuts_file`; `cascade_options.level_array`,
  `inherit_optimality_cuts`; `solver_options.algorithm`,
  `threads`, `time_limit`.
- Stage / Scenario / Aperture fields: `month`, `discount_factor`,
  `probability_factor`, `hydrology`, `source_scenario`.

**Tier C — CLI-only options** (no dialect alias needed — `--set`
handles cross-tool tuning):

- All numerical tolerances and thresholds:
  `kirchhoff_threshold`, `dc_line_reactance_threshold`,
  `solver_options.{optimal_eps,feasible_eps,barrier_eps}`,
  `sddp_options.{convergence_tol,stationary_*,prune_dual_threshold,
  cut_coeff_eps}`, `kappa_threshold`.
- Scaling factors: `scale_objective`, `scale_theta`,
  `scale_loss_link`, `theta_max`, `auto_scale`,
  `variable_scales[]`.
- LP-debug filters: `lp_debug`, `lp_only`, `lp_fingerprint`,
  `lp_debug_scene_min`/`max`, `lp_debug_phase_min`/`max`,
  `lp_debug_passes`, `lp_compression`.
- LP-validation / matrix-diagnostic flags: every option under
  `lp_matrix_options` and `lp_validation_options`.
- Low-memory & codec knobs: `low_memory_mode`, `memory_codec`,
  `memory_emphasis`, `compute_stats`.
- SDDP iteration mechanics: `update_lp_skip`, `multi_cut_threshold`,
  `max_cuts_per_phase`, `max_stored_cuts`, `cut_prune_interval`,
  `terminal_failure_threshold`, `forward_max_fallbacks`,
  `state_variable_lookup_mode`, `recovery_mode`,
  `cut_recovery_mode`, `single_cut_storage`,
  `missing_cut_var_mode`, `convergence_confidence`,
  `convergence_mode`, `cut_drain_mode`.
- Aperture mechanics: `aperture_use_manual_clone`,
  `aperture_drop_fcuts`, `aperture_chunk_size`,
  `aperture_selection_mode`, `aperture_timeout`,
  `aperture_directory`, `save_aperture_lp`.
- HPC / sentinel: `sentinel_file`, `api_enabled`,
  `save_per_iteration`, `simulation_mode`.
- Build / output mechanics: `build_mode`, `write_out`,
  `constraint_mode`, `use_uid_fname`.

**Registry-size impact**: this scoping shrinks the options-side
dialect registry from the ~170 entries estimated in §E.14 to
roughly **~30–40 entries** — the Tier-J list above. The
remaining ~130 Tier-C options live on the canonical gtopt name
forever; `--set` is their dialect-independent override path.

### 10.10 Verification Caveats

The dialect mappings in Appendix C (PLEXOS, PSR SDDP) and the
field-name tables for those two tools are **best-effort from
training data**. They are not safe to ship as production dialects
until a verification pass against authoritative current
documentation:

- **PLEXOS**: Energy Exemplar object-model reference for the
  installed PLEXOS version (8.x vs 9.x property names differ).
- **PSR SDDP**: PSR SDDP user manual / latest JSON schema export.
  The Portuguese↔English transition is ongoing; both vocabularies
  may need separate sub-dialects (`sddp-pt`, `sddp-en`).

In contrast, the following dialects are derived from
**code already in this repo** and can ship without external
verification:

- **`gtopt`**: by definition, the canonical names in the registry.
- **`plp`**: derived from `scripts/plp2gtopt/` parser/writer pairs
  (Appendix D). The PLP field names are authoritative because they
  are what the converter actually reads from the Fortran-formatted
  `.dat` files.
- **`pypsa`** / **`pandapower`**: well-documented and stable
  schemas; field names can be cross-checked against the pinned
  versions of `pypsa` / `pandapower` Python packages.

§10-E reflects this: PLEXOS and SDDP dialects ship under an
`--experimental` flag until verification.

---

## Appendix D — PLP ↔ gtopt Field Dictionary

Compiled 2026-05-17 from `scripts/plp2gtopt/plp2gtopt/` parsers
and writers. PLP is the Chilean dispatch-model legacy format
(Fortran-fixed-format `.dat` files); the authoritative source for
"what PLP calls field X" is the converter script, not external
documentation.

**Caveats**:

- PLP packs multiple gtopt concepts into single files (e.g. one
  `plpcnfce.dat` row encodes a Generator, a Reservoir, or a
  Turbine depending on the central's `type`).
- Several gtopt fields are **derived** during conversion, not
  read verbatim — those rows are flagged with "derived".
- Time-varying parameters (maintenance schedules, cost schedules)
  land in **parquet files** rather than JSON. PLP's
  `plpmance.dat` / `plpmanli.dat` / `plpmanbat.dat` /
  `plpcosce.dat` feed those parquet outputs.

### D.1 Bus — `plpbar.dat`

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | `plpbar.dat` | `number` | Fortran 1-based |
| `name` | `plpbar.dat` | `name` | Quoted string, trimmed |
| `voltage` | `plpbar.dat` | `voltage` (kV) | Extracted from name pattern or default 1.0 |
| `reference_theta` | derived | — | 0.0 for first bus (PLP slack convention) |

### D.2 Generator (thermal/renewable) — `plpcnfce.dat` + `plpcosce.dat` + `plpmance.dat`

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | `plpcnfce.dat` | `number` | Central number, Fortran 1-based |
| `name` | `plpcnfce.dat` | `name` | Quoted string |
| `bus` | `plpcnfce.dat` | `bus` | Bus number where generator connects |
| `pmin` | `plpcnfce.dat` | `PotMin` | MW |
| `pmax` | `plpcnfce.dat` *or* `plpmance.dat` | `PotMax` / `tmax_ij` | Maintenance overrides static |
| `capacity` | `plpcnfce.dat` | `pmax` | Design capacity, MW |
| `gcost` | `plpcnfce.dat` *or* `plpcosce.dat` | `CVar` / `cost` | $/MWh; schedule may override |
| `lossfactor` (efficiency) | `plpcnfce.dat` | `Rendimiento` | p.u. (used as thermal/hydro efficiency) |
| `type` | derived from `plpcnfce.dat` | (auto-classifier) | One of: `embalse`, `serie`, `pasada`, `termica`, `bateria`, `falla` (see `central_parser._central_type`) |

### D.3 Demand — `plpdem.dat` + `plpcosce.dat`

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | `plpdem.dat` | `bus number` | Demand uid = bus number |
| `bus` | `plpdem.dat` | `bus number` | Target bus |
| `lmax` | `plpdem.dat` (→ parquet) | block-wise demands | Becomes `Demand/lmax.parquet` |
| `emin` | `plpdem.dat` (→ parquet) | per-stage energies | Only if `management_factor > 0` |
| `fcost` | `plpcosce.dat` (→ parquet) | falla cost schedule | From "falla" central on same bus |

### D.4 Line — `plpcnfli.dat` + `plpmanli.dat`

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | `plpcnfli.dat` | `number` | |
| `name` | `plpcnfli.dat` | `name` | Quoted, trimmed |
| `active` | `plpcnfli.dat` *or* `plpmanli.dat` | `Operativa` (T/F) | Maintenance overrides static |
| `bus_a` | `plpcnfli.dat` | `BusA` | From bus |
| `bus_b` | `plpcnfli.dat` | `BusB` | To bus |
| `resistance` | `plpcnfli.dat` | `R(Ohm)` | Ω |
| `reactance` | `plpcnfli.dat` | `X(ohm)` | Ω; omitted for HVDC or DC lines (X=0) |
| `tmax_ab` | `plpcnfli.dat` *or* `plpmanli.dat` | `F.Max.A-B` | Forward rating, MW |
| `tmax_ba` | `plpcnfli.dat` *or* `plpmanli.dat` | `F.Max.B-A` | Reverse rating, MW |
| `voltage` | `plpcnfli.dat` | `Voltage` | kV |
| `loss_segments` | `plpcnfli.dat` | `Num.Tramos` | PWL segments; omit if = 1 |
| `use_line_losses` | `plpcnfli.dat` | `Mod.Perd.` | Per-line loss model toggle |
| `loss_allocation_mode` | `plpcnfli.dat` | `FPerdLin` (E/R/M) | E=sender, R=receiver, M=split |
| `hvdc` (gtopt extension) | `plpcnfli.dat` | derived | Flag, omitted when False |

### D.5 Battery — `plpcenbat.dat` + `plpcnfce.dat` (BAT type) + `plpess.dat` + `plpmanbat.dat` + `plpmaness.dat`

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | `plpcnfce.dat` | BAT central `number` | Battery UID = its BAT central |
| `name` | `plpcenbat.dat` | `BatNom` | |
| `active` | `plpmanbat.dat` *or* `plpmaness.dat` | per-block | Schedule |
| `input_efficiency` | `plpcenbat.dat` | `FPC` | p.u. (default 0.95 if file missing) |
| `output_efficiency` | `plpcenbat.dat` | `FPD` | p.u. (default 0.95 if file missing) |
| `emin` | `plpcenbat.dat` *or* `plpmanbat.dat` *or* `plpmaness.dat` | `BatEMin` / `EMin` / `Emin` | MWh |
| `emax` | `plpcenbat.dat` *or* `plpmanbat.dat` *or* `plpmaness.dat` | `BatEMax` / `EMax` / `Emax` | MWh |
| `eini` | derived | — | From `(emin + emax)/2` if not specified |
| `capacity` | `plpess.dat` | `dcmax × hrs_reg` | MWh; only when ESS model present |

The Battery generates **three linked gtopt elements** at write time:

1. `Battery` itself (storage state).
2. A discharge `Generator` (uid = BAT central number).
3. A charge `Demand` (uid = BAT central number).
4. A `Converter` binding the three together.

### D.6 Converter (Battery linkage) — derived

| gtopt JSON | PLP source | Notes |
|-----------|------------|-------|
| `uid` | `plpcnfce.dat` BAT central `number` | |
| `name` | `plpcenbat.dat` `BatNom` | |
| `battery` / `generator` / `demand` | All three reference the same BAT central uid | Three-way link |
| `capacity` | `plpess.dat` `dcmax` *or* `plpcnfce.dat` `pmax` | MW |

### D.7 Reservoir — `plpcnfce.dat` (embalse type) + `plpmanem.dat` + `plpplem2.dat`

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | `plpcnfce.dat` | central `number` (embalse) | |
| `name` | `plpcnfce.dat` | `name` | |
| `emin` | `plpcnfce.dat` *or* `plpmanem.dat` | `emin` | hm³ |
| `emax` | `plpcnfce.dat` *or* `plpmanem.dat` | `emax` | hm³ |
| `eini` | `plpcnfce.dat` | `vol_ini` | hm³ |
| `efin` | `plpcnfce.dat` | `vol_fin` | hm³ |
| `efin_cost` | `plpplem2.dat` | `GradX_i` (avg, scaled) | $/hm³ — boundary-cut water value |
| `vert_min` / `vert_max` | `plpcnfce.dat` | `vert_min` / `vert_max` | Optional physical bounds |
| `energy_scale` | `plpcnfce.dat` | `energy_scale` | Volume rescale for ≥1 Mm³ units |

### D.8 Waterway — `plpaflce.dat` + `plpcnfli.dat` + junction topology

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | derived | — | Generated by `junction_writer` |
| `name` | `plpaflce.dat` | central name | |
| `efficiency` | `plpcenre.dat` | `segments[0].constant` | MW·s/m³; PWL segments supported |
| `turbine_pmax` | `plpcenpmax.dat` | volume-Pmax `segments` | Optional volume-dependent Pmax |

### D.9 Junction — derived (no PLP file)

PLP encodes hydro topology **implicitly** through central types
(`embalse`/`serie`/`pasada`) and cascade routing fields
(`Hid_Indep`, `ser_hid`, `ser_ver`). `junction_writer.py` reifies
the implicit graph into explicit `Junction` nodes named `<central>_jnc`.

### D.10 Turbine — derived from Generator + cascade routing

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | `plpcnfce.dat` | central `number` | Same as parent Generator |
| `name` | `plpcnfce.dat` | `name` | |
| `efficiency` | `plpcenre.dat` | `mean_production_factor` | MW·s/m³ |
| `pmin` / `pmax` | `plpcnfce.dat` *or* `plpmance.dat` *or* `plpcenpmax.dat` | `PotMin` / `PotMax` / volume-curve constant | |

### D.11 Flow (stochastic inflow) — `plpaflce.dat`

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | derived | — | Scenario set ID |
| `waterway` | `plpaflce.dat` | central name | Pasada or serie |
| `blocks` | `plpaflce.dat` | block numbers | |
| `discharge` (per-scenario) | `plpaflce.dat` | columns 2..N | m³/s per stochastic scenario |
| `num_hydrologies` | `plpaflce.dat` | header | Scenario count |

Pasada centrals in **profile mode** are normalized to capacity
factors; in **hydro mode** (`pasada_hydro=True`) flows stay
physical [m³/s].

### D.12 Reservoir Seepage — `plpcenfi.dat`

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | derived | — | |
| `reservoir` | `plpcenfi.dat` | `EMBALSE_NAME` | |
| `slope` | `plpcenfi.dat` | `slope` (or first segment) | m³/s per hm³ |
| `constant` | `plpcenfi.dat` | `constant` (or first segment) | m³/s |
| `segments` | `plpcenfi.dat` | PWL `segments` | Piecewise-linear model |

Seepage formula: `q_seep = slope × (eini + efin)/2 + constant` [m³/s].

### D.13 LNG Terminal — `plpcnfgnl.dat`

| gtopt JSON | PLP file | PLP field | Notes |
|-----------|----------|-----------|-------|
| `uid` | `plpcnfgnl.dat` | `Id` | |
| `name` | `plpcnfgnl.dat` | `Name` | |
| `emax` (`vmax`) | `plpcnfgnl.dat` | `VMax` | m³ tank capacity |
| `eini` (`vini`) | `plpcnfgnl.dat` | `Vini` | m³ initial |
| `fuel_cost` | `plpcnfgnl.dat` | `CGnl` | $/m³ |
| `spillway_cost` (`vent_cost`) | `plpcnfgnl.dat` | `CVer` | $/m³ |
| `regasification_cost` | `plpcnfgnl.dat` | `CReg` | $/m³ |
| `scost` (storage holding) | `plpcnfgnl.dat` | `CAlm` | $/m³/day |
| `conversion_efficiency` | `plpcnfgnl.dat` | `GnlRen` | LNG → gas p.u. |
| `generators[]` | `plpcnfgnl.dat` | `LinkedGenerators` | Per-gen heat rate computed as `1.0 / (GnlRen × gen_efficiency × 3.6)` |
| `deliveries[]` | `plpcnfgnl.dat` | `DeliveryEntries` | Per-stage delivery schedule |

### D.14 Water Rights (Laja / Maule) — `plplajam.dat` / `plpmaulen.dat`

The two PLP files encode the **Laja** (Laguna del Laja) and **Maule**
irrigation/electric conventions. Both materialize as gtopt
`VolumeRight` / `FlowRight` / `UserConstraint` graphs that have no
single-row mapping — see `laja_parser.py` / `maule_parser.py` and
`docs/irrigation-agreements.md` for the full reduction.

Key field families (Laja example):

| Concept | PLP field | Maps to |
|---------|-----------|---------|
| Right category name | `name` | `VolumeRight.name` |
| Right type | (derived: riego/electrico/mixto/anticipado) | `VolumeRight.purpose` |
| Max annual volume | `max_irr`, `max_elec`, … | `VolumeRight.emax` |
| Max flow | `qmax_irr`, `qmax_elec`, … | `FlowRight.fmax` |
| Base cost | `cost_irr_ns`, `cost_elec_ns`, … | `VolumeRight.ecost` / `FlowRight.fcost` |
| Monthly usage factor | `monthly_usage_irr[m]` | Schedule on `VolumeRight.demand` |
| Per-zone allocation | `irr_factors[]`, `elec_factors[]` | `UserConstraint` coefficients |

### D.15 Cost Schedule — `plpcosce.dat`

| gtopt destination | PLP field | Notes |
|-------------------|-----------|-------|
| `Generator.gcost` (→ parquet) | (name, stage, cost) | Thermal / hydro variable cost |
| `Demand.fcost` (→ parquet) | (name, stage, cost) | Falla / curtailment per-bus |

### D.16 Stage — `plpeta.dat`

| gtopt JSON | PLP field | Notes |
|-----------|-----------|-------|
| `Stage.number` | `Etapa` | |
| `Stage.month` | `Mes` | Calendar month 1-12 |
| `Stage.duration` | `NHoras` | Hours |
| `discount_factor` | `FactTasa` | 1 / discount rate |

### D.17 Block — `plpblo.dat`

| gtopt JSON | PLP field | Notes |
|-----------|-----------|-------|
| `Block.number` | `block_idx` | |
| `Block.stage` | `stage_idx` | |
| `Block.duration` | `duration` | Hours |

### D.18 PLP Files With No gtopt Analog

| PLP file | Purpose | Reason |
|----------|---------|--------|
| `plpmat.dat` | PLP solver numerical parameters | gtopt has its own solver config |
| `plpvrebemb.dat` | Per-reservoir discretization hints | Informational; not used |
| `plpind.dat` | Demand index schedules | Merged into `lmax` / `emin` parquet |
| `plpaplef.dat` | Aperture / stochastic-scenario config | Handled by `aperture_writer`, not in `Planning` |

### D.19 gtopt Elements With No PLP Analog

| gtopt element | Why missing in PLP |
|---------------|---------------------|
| `InertiaZone` / `InertiaProvision` | PLP has no synthetic-inertia constraints |
| `VolumeRight` / `FlowRight` (generic) | PLP only has the specialized Laja/Maule encodings |
| `Commitment` / `SimpleCommitment` | PLP does not model unit-commitment ramp / min-up-down |
| `ReserveZone` / `ReserveProvision` (generic) | PLP reserve handling is per-central, not zonal |

### D.20 Unit Conventions

| Quantity | PLP | gtopt | Conversion in `gtopt_writer` |
|----------|-----|-------|------------------------------|
| Reservoir volume | Mm³ (10⁶ m³) | hm³ (10⁵ m³) | × 100 (via `energy_scale`) |
| Flow | m³/s | m³/s | identity (timing differs: block-avg vs stage-avg) |
| Turbine efficiency | MW·s/m³ | MW·s/m³ | identity |
| Cost | $/MWh | $/MWh | identity |
| Power | MW | MW | identity |
| Electrical R/X | Ω | Ω | identity (X omitted for DC lines) |

### D.21 Parquet Schedule Outputs

Time-varying parameters land in parquet files, not JSON. PLP
sources feeding each:

| Parquet output | PLP sources |
|----------------|-------------|
| `Generator/{gcost,pmin,pmax}.parquet` | `plpcosce.dat`, `plpmance.dat`, `plpmaness.dat` |
| `Demand/{lmax,emin,fcost}.parquet` | `plpdem.dat`, `plpmanbat.dat`, `plpcosce.dat` |
| `Line/{tmax_ab,tmax_ba,active}.parquet` | `plpmanli.dat` |
| `Battery/{emin,emax}.parquet` | `plpmanbat.dat`, `plpmaness.dat` |

---

## Appendix E — Planning / Simulation / Options Dictionary

Companion to Appendices C and D. Covers gtopt's **non-element**
surface — top-level `Planning`, the `Simulation` time-axis,
configuration sub-structs (`Options`, `ModelOptions`,
`SddpOptions`, `CascadeOptions`, `SolverOptions`,
`MonolithicOptions`, `LpMatrixOptions`, `LpValidationOptions`).

The same caveats as Appendix C apply: PLEXOS / PSR SDDP cells are
best-effort from training data; `PLP` cells are derived from
`scripts/plp2gtopt/` and from PLP's `.dat` headers, so they are
authoritative.

### E.1 Planning (top-level)

| gtopt | PLEXOS | PSR SDDP | PyPSA | pandapower | PLP |
|-------|--------|----------|-------|------------|-----|
| `Planning` (root struct) | `Model` | `Caso` / `Case` | `pypsa.Network()` | `pp.create_empty_network()` | `plp.in` (driver) |
| `system` (sub-object) | `<System>` membership | `Sistema` block | (flat on Network) | (flat on net) | per-`.dat` files |
| `simulation` (sub-object) | `<Horizon>` + `<Sample>` | `Estagio` + `Cenario` blocks | `snapshots` + `investment_periods` | (per-time series) | `plpeta.dat` + `plpblo.dat` + `cenarios` |
| `options` (sub-object) | `<Settings>` / `<Diagnostic>` | `sddp.dat` global block | (kwargs on `lopf()`) | (kwargs on `runopp()`) | `plpmat.dat` + flags |
| `model_options` | `<Model>` properties | `Tipo_Rede`, `Tipo_Custo` | (kwargs) | (kwargs) | flags in `plpmat.dat` |
| `sddp_options` | (3rd-party SDDP plug-in) | (the whole `sddp.dat`) | — | — | `plpaperturas.*` |
| `cascade_options` | `<Multi-Horizon>` | `Cascata` (newer SDDP) | — | — | — |
| `solver_options` | `<Performance>` / `<Diagnostic>` | `solver.dat` | `solver_options` | (kwargs) | (handled by external solver) |
| `monolithic_options` | (default monolithic run) | `Tipo = Deterministico` | `lopf()` default | `runopp()` default | (default plp run) |

### E.2 Simulation (time axis)

| gtopt | PLEXOS | PSR SDDP | PyPSA | pandapower | PLP |
|-------|--------|----------|-------|------------|-----|
| `block_array` | `<Periods>` / `<Timeslices>` | `Bloco` / `Patamar` | `snapshots` | (each timestep) | `plpblo.dat` |
| `stage_array` | `<Horizon>` `Steps` | `Estagio` / `Stage` | `investment_periods` | — | `plpeta.dat` |
| `scenario_array` | `<Sample>` set | `Cenario` / `Scenario` | (scenario loop) | (manual) | `plpaflce.dat` columns |
| `phase_array` | `<Multi-Horizon>` group | `Fase` (newer) | — | — | (cascade levels) |
| `scene_array` | `<Scenario Group>` | (super-scenario) | — | — | (manual) |
| `aperture_array` | (sample point) | `Abertura` | — | — | (per-stage aperture set) |
| `iteration_array` | (outer-iter logs) | `Iteracao` | — | — | (PLP iteration log) |
| `annual_discount_rate` | `Discount Rate (Long Term)` | `TaxaDesconto` | (discounting outside) | — | `FactTasa` (per-stage) |
| `boundary_cuts_file` | `<Cut File>` | `cortes.dat` / `cuts.json` | — | — | `plpplem2.dat` |
| `boundary_cuts_valuation` | (cut interp setting) | `Tipo_Avaliacao` | — | — | — |
| `probability_rescale` | `Sample Probabilities = Renormalize` | `RenormalizarProb` | — | — | — |
| `kappa_warning` / `kappa_threshold` | `Diagnostic.Condition` | (newer diagnostic) | — | — | — |
| `block_hour_map` | (Timeslice → Hour map) | (Bloco → Hora map) | (snapshot tz) | — | derived |

### E.3 Stage / Block / Scenario / Phase / Scene / Aperture / Iteration

| gtopt field | PLEXOS | PSR SDDP | PLP |
|-------------|--------|----------|-----|
| `Stage.uid` | `Horizon Step ID` | `Estagio.Id` | `plpeta.dat` `Etapa` |
| `Stage.first_block` / `count_block` | (Step → Period range) | (Estagio → Bloco range) | derived from `plpblo.dat` |
| `Stage.discount_factor` | `Discount Factor` | `FactDesconto` | `plpeta.dat` `FactTasa` |
| `Stage.month` | `Month` (Calendar) | `Mes` | `plpeta.dat` `Mes` |
| `Stage.chronological` | `Chronological = True` | `Cronologico` | derived |
| `Block.duration` | `Period.Duration` | `Bloco.Duracao` | `plpblo.dat` `duration` |
| `Scenario.probability_factor` | `Sample.Probability` | `Cenario.Probabilidade` | per-aperture in `plpaflce.dat` |
| `Scenario.hydrology` | `Sample.Hydrology ID` | `Cenario.IdHidrologia` | hydrology column in `plpaflce.dat` |
| `Phase.first_stage` / `count_stage` | `Multi-Horizon range` | `Fase.IntervaloEstagio` | (cascade-level config) |
| `Phase.continuous` (`true`/`false`) | `Linked = True` | `Continuo` | — |
| `Phase.apertures[]` | `Sample subset` | `AperturasFase` | per-level apertures |
| `Scene.first_scenario` / `count_scenario` | `Scenario Group range` | (super-scenario) | — |
| `Aperture.source_scenario` | `Sample.SourceHydrology` | `Abertura.CenarioOrigem` | inflow scenario ID |
| `Aperture.probability_factor` | `Sample.SubProbability` | `Abertura.Probabilidade` | uniform 1/N typical |
| `Iteration.index` / `update_lp` | `Outer Iter Index` / `Update Required` | `Iteracao.Indice` / `AtualizarLp` | per-iter log |

### E.4 General Options (`options`)

Most general options have direct PLEXOS / SDDP analogs. PLP
options live in `plpmat.dat` and CLI flags to the PLP driver.

| gtopt | PLEXOS | PSR SDDP | PyPSA | pandapower | PLP |
|-------|--------|----------|-------|------------|-----|
| `input_directory` | `Input Folder` | `DiretorioEntrada` | (constructor path) | — | working dir of `plp.in` |
| `input_format` (`parquet`/`csv`) | (Input Format setting) | (CSV / JSON / DAT) | — | — | `dat` (always) |
| `output_directory` | `Output Folder` | `DiretorioSaida` | — | — | `output/` |
| `output_format` | `Output Format` (CSV/XML/h5) | `FormatoSaida` | — | — | `.csv` reports |
| `output_compression` (`snappy`/`zstd`) | (Parquet compression) | (Parquet compression) | — | — | — |
| `use_uid_fname` | (Object ID in filename) | (Use ID in name) | — | — | — |
| `method` (`monolithic`/`sddp`/`cascade`) | `<Model.Run Type>` | `Tipo_Modelo` | (`lopf` vs MILP) | — | (set in driver) |
| `build_mode` | `<Compilation Mode>` | (internal) | — | — | — |
| `log_directory` | `Log Folder` | `DirLogs` | (Python logging) | — | `logs/` |
| `lp_debug` | `Diagnostic.Dump LP` | `DumpLp` | — | — | `iplp_debug` |
| `lp_compression` (`lz4`/`zstd`) | — | — | — | — | — |
| `lp_only` | `Run to LP write` | `SoLp` | — | — | `solo_lp` flag |
| `lp_fingerprint` | (LP hash diagnostic) | — | — | — | — |
| `lp_debug_scene_min` / `lp_debug_scene_max` / `lp_debug_phase_min` / `lp_debug_phase_max` | (LP-dump filters) | — | — | — | — |
| `variable_scales` | `<Scaling Overrides>` | (manual scaling) | — | — | — |
| `constraint_mode` | `<Constraint Handling>` | — | — | — | — |
| `write_out` (`serial`/`parallel`/…) | `Output Write Mode` | `EscritaModo` | — | — | — |
| `demand_fail_cost` | `VoLL` (global) | `CustoDeficitGlobal` | (`load_shedding`) | — | falla central CVU |
| `reserve_fail_cost` | `VoRS` (global) | `CustoFaltaReserva` | — | — | per-reserve cost |
| `hydro_fail_cost` | `Hydro Spill Penalty` | `CustoFaltaHidro` | — | — | — |
| `hydro_use_value` | `Hydro Use Value` | `ValorUsoHidro` (newer) | — | — | (`pcost` per hydro) |
| `use_line_losses` | `Allow Line Losses` | `ConsiderarPerdas` | (linear pf) | (`opf_init`) | `Mod.Perd.` |
| `loss_segments` | `Loss Segments` | `NumSegPerdas` | — | (`num_steps`) | `Num.Tramos` |
| `use_kirchhoff` | `Transmission Detail = DC` | `Tipo_Rede = DC` | (PF `mode`) | (`opf_alg`) | `Mod.DC` |
| `use_single_bus` | `Transmission Detail = Copper Plate` | `Tipo_Rede = Transp` | (no network) | — | `Mod.Uninodal` |
| `kirchhoff_threshold` | (PTDF cut-off) | — | — | — | — |
| `scale_objective` | `Scale Factor.Obj` | (internal) | — | — | — |
| `scale_theta` | `Scale Factor.Angle` | — | — | — | — |
| `annual_discount_rate` | `Discount Rate` | `TaxaDesconto` | — | — | `FactTasa` (per-stage) |

### E.5 Model Options (`model_options`)

Physics, scaling, and per-class penalty controls.

| gtopt | PLEXOS | PSR SDDP | PLP |
|-------|--------|----------|-----|
| `use_single_bus` | `Transmission Detail = Copper Plate` | `Tipo_Rede = Transp` | `Mod.Uninodal` |
| `use_kirchhoff` | `Transmission Detail = DC` | `Tipo_Rede = DC` | `Mod.DC` |
| `kirchhoff_mode` | `Angle Constraint Mode` | — | — |
| `use_line_losses` | `Allow Line Losses` | `ConsiderarPerdas` | `Mod.Perd.` |
| `line_losses_mode` | `Loss Approximation` (segmented / quadratic) | `TipoPerdas` | — |
| `kirchhoff_threshold` | (numerical cut-off) | — | — |
| `dc_line_reactance_threshold` | `DC Line X tol` | — | — |
| `loss_segments` | `Loss Segments` | `NumSegPerdas` | `Num.Tramos` |
| `scale_objective` | `Scale Factor.Obj` | (internal) | — |
| `scale_theta` | `Scale Factor.Angle` | — | — |
| `scale_loss_link` | (Loss-link scaling) | — | — |
| `theta_max` | `Theta Bound` | (angle bound) | — |
| `auto_scale` | `Auto Scaling = True` | `EscalaAutomatica` | — |
| `demand_fail_cost` | `VoLL` (global) | `CustoDeficitGlobal` | falla CVU |
| `reserve_fail_cost` | `VoRS` (global) | `CustoFaltaReserva` | per-reserve cost |
| `hydro_fail_cost` | `Hydro Spill Penalty` | `CustoFaltaHidro` | — |
| `hydro_use_value` | `Hydro Use Value` | `ValorUsoHidro` | — |
| `state_fail_cost` | `State Constraint Penalty` | `CustoFalhaEstado` | — |
| `demand_option_c` | (substitution-based demand) | — | — |
| `emission_cost` | `Emission Cost` | `CustoEmissao` | — |
| `emission_cap` | `Emission Cap` | `LimiteEmissao` | — |
| `continuous_phases` | `Linked Phases` | `FasesContinuas` | — |
| `strict_storage_emin` | `Strict Storage Min` | (newer) | — |

### E.6 SDDP Options (`sddp_options`)

PLEXOS does not implement SDDP natively — its multi-stage runs use
deterministic + scenario sampling. The closest equivalent is the
**third-party SDDP plug-in** or PSR's SDDP. Most rows are therefore
gtopt ↔ PSR SDDP. PyPSA / pandapower have no SDDP layer.

| gtopt | PSR SDDP | PLP |
|-------|----------|-----|
| `cut_sharing_mode` (`none`/…) | `CompartilharCortes` | — |
| `cut_drain_mode` | `EsgotarCortes` | — |
| `cut_directory` | `DiretorioCortes` | `cortes/` |
| `api_enabled` | (web-API access) | — |
| `update_lp_skip` | (LP-update stride) | — |
| `max_iterations` / `min_iterations` | `MaxIter` / `MinIter` | `iter_max` |
| `convergence_tol` | `TolConvergencia` | `tol_conv` |
| `elastic_penalty` | `PenalidadeElastica` | — |
| `scale_alpha` | `EscalaAlpha` | — |
| `cut_recovery_mode` | `ModoRecuperacaoCortes` | — |
| `recovery_mode` | `ModoRecuperacao` | — |
| `save_per_iteration` | `SalvarPorIteracao` | — |
| `cuts_input_file` | `ArquivoCortesEntrada` | `cortes_in.dat` |
| `sentinel_file` | (run-status sentinel) | — |
| `elastic_mode` | `ModoElastico` | — |
| `multi_cut_threshold` | `LimiarMultCorte` | — |
| `apertures[]` / `num_apertures` | `NumeroAberturas` | per-Estagio aperture count |
| `aperture_selection_mode` | `ModoSelecaoAberturas` | — |
| `aperture_directory` | `DiretorioAberturas` | `aberturas/` |
| `aperture_timeout` | `TimeoutAbertura` | — |
| `save_aperture_lp` | `SalvarLpAbertura` | — |
| `lp_debug_passes` | `DebugLpPasses` | — |
| `aperture_use_manual_clone` | (LP-clone strategy) | — |
| `aperture_drop_fcuts` | `DescartarCortesFwd` | — |
| `aperture_chunk_size` | (chunked aperture batch) | — |
| `boundary_cuts_file` | `ArquivoCortesFronteira` | `plpplem2.dat` |
| `boundary_cuts_mode` | `ModoCortesFronteira` | — |
| `boundary_cuts_mean_shift` | `DeslocamentoMedio` | — |
| `boundary_max_iterations` | `MaxIterFronteira` | — |
| `missing_cut_var_mode` | `VarCorteAusenteModo` | — |
| `max_cuts_per_phase` | `MaxCortesPorFase` | `max_cortes` |
| `cut_prune_interval` | `IntervaloPodaCortes` | — |
| `prune_dual_threshold` | `LimiarPodaDual` | — |
| `single_cut_storage` | `ArmazenamentoUnicoCorte` | — |
| `max_stored_cuts` | `MaxCortesArmazenados` | `max_cortes` |
| `simulation_mode` | `ModoSimulacao` | `tipo_sim` |
| `low_memory_mode` | `BaixaMemoria` | — |
| `memory_codec` (`lz4`/`zstd`) | `CodecMemoria` | — |
| `cut_coeff_eps` | `EpsCoefCorte` | — |
| `convergence_mode` | `ModoConvergencia` | — |
| `state_variable_lookup_mode` | `BuscaVarEstado` | — |
| `stationary_tol` / `stationary_window` | `TolEstacionaria` / `JanelaEstacionaria` | — |
| `convergence_confidence` | `ConfiancaConvergencia` | — |
| `stationary_gap_ceiling` | `LimiteSupGapEstac` | — |
| `terminal_failure_threshold` | `LimiarFalhasTerminal` | — |
| `forward_max_fallbacks` | `MaxFallbackForward` | — |

### E.7 Cascade Options (`cascade_options`)

gtopt's `Cascade` is a hierarchical multi-fidelity decomposition.
PSR SDDP added a similar **"Cascade"** mode in newer versions;
PLEXOS handles it via `Multi-Horizon` linkage.

| gtopt | PLEXOS | PSR SDDP | PLP |
|-------|--------|----------|-----|
| `inherit_optimality_cuts` | `Inherit Cuts` (Multi-Horizon) | `HerdarCortes` | — |
| `CascadeLevel.system_file` | (per-level Model file) | (per-level case path) | — |
| `CascadeLevel.level_array` | `Multi-Horizon levels` | `NiveisCascata` | — |
| (per-level overrides of SDDP options) | Multi-Horizon per-step overrides | per-Fase override | — |

### E.8 Solver Options (`solver_options`)

Pass-through to the LP/MIP solver. Same flag set every backend
supports, plus a few gtopt extensions.

| gtopt | CPLEX param | HiGHS option | CBC option | PLEXOS | PSR SDDP |
|-------|-------------|--------------|------------|--------|----------|
| `algorithm` (`primal`/`dual`/`barrier`/`network`/`automatic`) | `LPMETHOD` | `solver` | `-presolve` + `-solve` mode | `LP Solver` | `Algoritmo` |
| `threads` | `Threads` | `parallel` + `threads` | `-threads` | `Threads` | `NumThreads` |
| `presolve` | `PreInd` | `presolve` | `-presolve on` | `Presolve` | `Presolve` |
| `optimal_eps` | `EpOpt` | `dual_feasibility_tolerance` | `-tolopt` | `Optimal Tol` | `TolOtimo` |
| `feasible_eps` | `EpRHS` | `primal_feasibility_tolerance` | `-tolpriminf` | `Feasibility Tol` | `TolViab` |
| `barrier_eps` | `BarEpComp` | `ipm_optimality_tolerance` | (n/a) | `Barrier Tol` | `TolBarr` |
| `log_level` | `MIPDisplay` / `BarDisplay` | `log_dev_level` | `-log` | `Log Level` | `NivelLog` |
| `log_mode` | (log-file routing) | (log-file routing) | (file vs stdout) | `Log Mode` | — |
| `time_limit` | `TiLim` | `time_limit` | `-sec` | `Time Limit` | `TempoMaximo` |
| `scaling` | `ScaInd` | `scaling` | `-scaling` | `Scaling` | `Escalamento` |
| `crossover` | `BarCrossAlg` | `run_crossover` | (n/a) | `Crossover` | `Crossover` |
| `max_fallbacks` | (gtopt-driven retry loop) | (same) | (same) | (manual) | — |
| `memory_emphasis` | `MemoryEmphasis` | (n/a) | (n/a) | `Memory Emphasis` | — |

### E.9 Monolithic Options (`monolithic_options`)

Used when `method = "monolithic"`.

| gtopt | PLEXOS | PSR SDDP | PLP |
|-------|--------|----------|-----|
| `solve_mode` | `Solve Mode` | `ModoResolucao` | — |
| `boundary_cuts_file` | `<Cut File>` | `cortes.dat` | `plpplem2.dat` |
| `boundary_cuts_mode` | (Cut-load policy) | `ModoCortesFronteira` | — |
| `boundary_max_iterations` | `Cut Refinement Iter Max` | `MaxIterCortes` | — |

### E.10 LP Matrix Options (`lp_matrix_options`)

Numerical-quality tooling — gtopt-specific surface; no direct
equivalents in PLEXOS / PSR SDDP (they expose only the solver's
own scaling option).

| gtopt | Notes |
|-------|-------|
| `lp_coeff_ratio_threshold` | Warning threshold on `max_coef / min_coef` per row. |
| `equilibration_method` | Row/col equilibration strategy. |
| `fast_sqrt_method` | Fast inverse-sqrt for equilibration. |
| `compute_stats` | Compute and log LP shape stats. |

### E.11 LP Validation Options (`lp_validation_options`)

Bounds / coefficient / RHS / objective sanity checks. Closest
PLEXOS analog is `<Diagnostic>` warnings; PSR SDDP has scattered
similar checks.

| gtopt | PLEXOS | PSR SDDP |
|-------|--------|----------|
| `enable` | `Diagnostic Enabled` | `DiagnosticoAtivo` |
| `coeff_warn_max` / `coeff_warn_min` | `Coefficient Warning Bounds` | (kappa diagnostic) |
| `bound_warn_max` | `Bound Warning Max` | — |
| `rhs_warn_max` | `RHS Warning Max` | — |
| `obj_warn_max` | `Objective Coefficient Warning` | — |
| `max_warnings_per_kind` | `Max Warnings Per Class` | — |

### E.12 Coverage Summary

| Surface | gtopt fields | PLEXOS | PSR SDDP | PyPSA | pandapower | PLP |
|---------|--------------|--------|----------|-------|------------|-----|
| Planning / Simulation root | 9 | ✅ full | ✅ full | 🟡 partial | 🟡 partial | ✅ full |
| Time axis (Block/Stage/Scenario/Phase/Scene/Aperture/Iteration) | 25 | ✅ full | ✅ full | 🟡 partial (no Phase/Aperture) | ⚪ none | 🟡 partial (no Phase/Scene) |
| General options | 33 | ✅ full | ✅ full | 🟡 partial | 🟡 partial | 🟡 partial |
| ModelOptions | 22 | ✅ full | ✅ full | 🟡 partial | 🟡 partial | 🟡 partial |
| SddpOptions | 47 | 🟡 plug-in only | ✅ full | ⚪ none | ⚪ none | 🟡 partial |
| CascadeOptions | 5 | 🟡 (Multi-Horizon) | 🟡 (newer Cascade) | ⚪ none | ⚪ none | ⚪ none |
| SolverOptions | 13 | ✅ full | ✅ full | 🟡 partial | 🟡 partial | (delegated to solver) |
| MonolithicOptions | 4 | ✅ full | ✅ full | ✅ default | ✅ default | ✅ default |
| LpMatrixOptions | 4 | ⚪ none (solver-internal) | ⚪ none | ⚪ none | ⚪ none | ⚪ none |
| LpValidationOptions | 7 | 🟡 partial | 🟡 partial | ⚪ none | ⚪ none | ⚪ none |

### E.13 Naming-Pattern Observations (options)

- **gtopt is already close to the PSR SDDP English-schema style**
  for SDDP-specific options (`max_iterations`, `convergence_tol`,
  `cut_sharing_mode`). The gap to a PSR-SDDP dialect is mostly
  Portuguese ↔ English translation, not structural.
- **PLEXOS option names are GUI-centric** (`<Settings>`,
  `<Diagnostic>`, `<Performance>`) — they would feel verbose as
  JSON keys but are easy to discover in a properties browser.
- **PyPSA / pandapower don't have a unified options object**;
  configuration is passed as `**kwargs` to solver entry points.
  This means a `pypsa` or `pandapower` dialect for *options* would
  be partial at best — mainly mapping `demand_fail_cost`,
  `use_kirchhoff`, `solver`-family names.
- **gtopt has option families with no analog in any tool**:
  `lp_matrix_options.*` (numerical diagnostics),
  `variable_scales[]` (per-variable scaling overrides),
  `write_out` (parallel-output strategy), `low_memory_mode`,
  `memory_codec`. These will always be orphans under the §10
  dialect mechanism and will fall back to canonical gtopt names.

### E.14 Registry Implications

For §10 (runtime dialect switching), the options surface adds
roughly **170 new entries** to the registry. Distribution:

- ~30 with clean equivalents in every dialect (PyPSA / pandapower / PLEXOS / SDDP / PLP).
- ~80 with PLEXOS + SDDP equivalents only (no PyPSA / pandapower analog).
- ~60 gtopt-orphan (no equivalent anywhere; always serialized
  under the canonical name per §10.7).

This roughly doubles the §9.4 registry surface but the recipe is
identical: each option gets a `FieldName` entry with its `alts[]`
populated for whichever dialects have an equivalent. The orphan
fallback policy (§10.7) keeps output round-tripping even when
60% of options have no chosen-dialect equivalent.

---

## 11. Canonical Option-Name Compatibility Review

**Status (2026-05-17)**: soft recommendations only. The §10 dialect
mechanism gives users the choice of vocabulary at runtime *without*
changing canonical names. This section asks a different question:
**should some of gtopt's canonical option names themselves be
renamed** to land closer to industry-standard terminology by
default?

The benefit is purely cosmetic-UX: a user reading a gtopt JSON
without setting any `--input-dialect` flag sees option names that
already match what they know from PLEXOS / PSR SDDP / PyPSA. The
cost is a wave of search-and-replace across cases, scripts, tests,
and docs.

Recommendations are **opt-in**, not part of the §9 Phase 1 scope,
and would happen *after* the §10 dialect mechanism lands so the
rename can ship with the old name automatically aliased into the
new canonical via the registry.

### 11.1 Methodology

For each gtopt canonical option name, I check whether at least
three of the five comparator tools (PLEXOS, PSR SDDP, PyPSA,
pandapower, PLP) share a recognizable term. Where they do, the
shared term is the rename candidate.

Three categories:

- **✅ Keep** — gtopt's canonical name is already the
  industry-aligned choice.
- **🔄 Strong rename candidate** — at least 3 comparator tools
  use a clearly better name; the rename pays for itself in
  reduced cognitive load.
- **🤔 Weak rename candidate** — comparator tools split, or
  gtopt's name is fine but a more descriptive form exists.

### 11.2 General Options (`options`)

| gtopt canonical | Category | Recommendation | Rationale |
|-----------------|----------|----------------|-----------|
| `input_directory` / `output_directory` | ✅ Keep | — | Universal. |
| `input_format` / `output_format` | ✅ Keep | — | Universal. |
| `output_compression` | ✅ Keep | — | Pandas / Arrow convention. |
| `annual_discount_rate` | ✅ Keep | — | Standard financial term. |
| `method` | ✅ Keep | — | Values `monolithic`/`sddp`/`cascade` are gtopt-specific; the *key* is fine. |
| `log_directory` | ✅ Keep | — | Universal. |
| `demand_fail_cost` | 🔄 Strong | **→ `value_of_lost_load`** (alias `voll_cost`, `lost_load_cost`) | IEEE-standard "VOLL". PLEXOS: `VoLL`. PSR SDDP: `CustoDeficit`. PyPSA: `load_shedding`. Already noted in §5.9. |
| `reserve_fail_cost` | 🔄 Strong | **→ `reserve_shortage_cost`** | PLEXOS: `VoRS` ("Value of Reserve Shortage"). PSR SDDP: `CustoFaltaReserva`. Aligns with "shortage" used everywhere except gtopt. |
| `hydro_fail_cost` | 🔄 Strong | **→ `hydro_spill_cost`** (or `hydro_shortage_cost` if it covers both) | "fail" is ambiguous. PLEXOS: `Hydro Spill Penalty`. PSR SDDP: `CustoVertimento`. |
| `hydro_use_value` | ✅ Keep | — | gtopt-coined but self-documenting. PSR SDDP equivalent `ValorUsoHidro` is a calque of this. |
| `use_kirchhoff` | ✅ Keep | — | Precise. §5 already settled this — `use_dc_opf` was rejected as less precise. |
| `use_single_bus` | 🔄 Strong | **→ `copper_plate`** (boolean) | PLEXOS's term is "Copper Plate"; PSR SDDP uses `Tipo_Rede = Transp`/`Uninodal`. Either `copper_plate=true` or repurpose as a mode enum `network_mode = copper_plate \| dc_opf \| ac_opf`. The latter is cleaner. |
| `kirchhoff_threshold` | 🤔 Weak | (keep, or → `dc_opf_threshold`) | Tied to `use_kirchhoff` semantics. |
| `scale_objective` / `scale_theta` | ✅ Keep | — | gtopt-specific numerical tooling; no industry analog. |
| `loss_segments` | ✅ Keep | — | PLEXOS / PSR SDDP / pandapower all use "segments". |
| `use_line_losses` | ✅ Keep | — | Clear. |
| `lp_only` | 🤔 Weak | (keep, or → `build_only` / `dry_run`) | "dry_run" is the universal CLI convention; "build_only" is more descriptive of what gtopt does. |
| `lp_debug` | 🤔 Weak | (keep, or → `dump_lp`) | PLEXOS uses `Dump LP`. Most LP tools call it "dump LP". |
| `lp_compression` | ✅ Keep | — | Internal-format hint; gtopt-specific. |
| `lp_fingerprint` | ✅ Keep | — | gtopt-specific reproducibility tooling. |
| `lp_debug_scene_min` / `..._max` / `..._phase_min` / `..._phase_max` | 🤔 Weak | (keep, or collapse into `lp_debug_filter = "scene=[2-5],phase=[1-3]"`) | Four flags vs one expression. The expression form scales if more axes are added. |
| `use_uid_fname` | 🔄 Strong | **→ `use_uid_filenames`** | Already noted in §6.1 Tier 4. "fname" is the only abbreviation in the options surface. |
| `variable_scales` | ✅ Keep | — | gtopt-specific. |
| `constraint_mode` | ✅ Keep | — | gtopt-specific. |
| `write_out` | 🤔 Weak | (keep, or → `output_write_mode`) | "write_out" reads as a verb; "output_write_mode" matches the rest of the *_mode suffix convention used in `cut_sharing_mode`, `low_memory_mode`, etc. |
| `build_mode` | ✅ Keep | — | Matches the `*_mode` convention. |

### 11.3 Model Options (`model_options`)

| gtopt canonical | Category | Recommendation |
|-----------------|----------|----------------|
| `state_fail_cost` | 🔄 Strong | **→ `state_violation_cost`** ("fail" is too vague for a soft-constraint penalty). |
| `demand_option_c` | 🔄 Strong | **→ `demand_fail_rhs_shift`**. Both Options A and C are fail-substitutions; what Option C *adds* is moving the `+fail_cost·ecost·lmax` baseline out of `obj_constant` into RHS shifts on the bus-balance and capacity rows. The name should describe *how* the substitution is done, not whether. "option_c" is a code reference, not user-meaningful. |
| `emission_cost` / `emission_cap` | ✅ Keep | — Universal. |
| `continuous_phases` | ✅ Keep | — Self-documenting. |
| `strict_storage_emin` | ✅ Keep | — Self-documenting. |
| `kirchhoff_mode` / `line_losses_mode` | ✅ Keep | — `*_mode` convention. |
| `dc_line_reactance_threshold` | ✅ Keep | — Self-documenting. |
| `theta_max` | ✅ Keep | — Standard. |
| `auto_scale` | ✅ Keep | — Universal. |
| `scale_loss_link` | ✅ Keep | — gtopt-specific. |

### 11.4 SDDP Options (`sddp_options`)

The SDDP option surface is gtopt's largest. Most names already
align with PSR SDDP English schema. A handful would benefit from
clarifying:

| gtopt canonical | Category | Recommendation |
|-----------------|----------|----------------|
| `convergence_tol` | 🔄 Strong | **→ `convergence_tolerance`** (full word; matches the rest of the option surface where abbreviations like "fname" / "eps" are the outliers). |
| `feasible_eps` / `optimal_eps` / `barrier_eps` | 🔄 Strong | **→ `feasibility_tolerance` / `optimality_tolerance` / `barrier_tolerance`** (CPLEX / HiGHS / PSR SDDP all use the long form in their public option names). |
| `cut_coeff_eps` | 🔄 Strong | **→ `cut_coefficient_tolerance`**. |
| `update_lp_skip` | 🔄 Strong | **→ `lp_update_interval`** (positive framing; matches `cut_prune_interval`). |
| `aperture_drop_fcuts` | 🤔 Weak | (keep, or → `drop_forward_cuts_per_aperture`). |
| `multi_cut_threshold` | ✅ Keep | — Standard. |
| `single_cut_storage` | ✅ Keep | — Standard. |
| `max_cuts_per_phase` / `max_stored_cuts` / `cut_prune_interval` / `prune_dual_threshold` | ✅ Keep | — All self-documenting. |
| `stationary_tol` / `stationary_window` / `stationary_gap_ceiling` | 🤔 Weak | (extend to `stationarity_*` for grammatical consistency, or keep — comparator tools don't have an equivalent). |
| `low_memory_mode` / `memory_codec` | ✅ Keep | — gtopt-specific. |
| `boundary_cuts_*` | ✅ Keep | — Standard SDDP / PSR vocabulary. |
| `missing_cut_var_mode` | ✅ Keep | — Self-documenting. |
| `cut_sharing_mode` / `cut_drain_mode` | ✅ Keep | — Match the `*_mode` convention. |
| `recovery_mode` / `cut_recovery_mode` | ✅ Keep | — Match the convention. |
| `terminal_failure_threshold` | ✅ Keep | — Self-documenting. |
| `forward_max_fallbacks` | ✅ Keep | — Self-documenting. |
| `convergence_confidence` | ✅ Keep | — Standard. |
| `simulation_mode` | ✅ Keep | — Standard. |
| `api_enabled` | ✅ Keep | — Universal. |
| `sentinel_file` | ✅ Keep | — Universal HPC convention. |
| `save_per_iteration` / `save_aperture_lp` | ✅ Keep | — Verb-adjective pattern reads cleanly. |
| `state_variable_lookup_mode` | ✅ Keep | — Self-documenting. |
| `aperture_*` family | ✅ Keep | — "aperture" is PSR SDDP's native term. |

### 11.5 Cascade Options (`cascade_options`)

| gtopt canonical | Category | Recommendation |
|-----------------|----------|----------------|
| `inherit_optimality_cuts` | ✅ Keep | — Self-documenting. |
| `level_array` | (covered by §6.1 Tier 1) | → `levels` (drop `_array`). |

### 11.6 Solver Options (`solver_options`)

The solver options surface is already well-aligned with CPLEX /
HiGHS / CBC / PLEXOS / PSR SDDP conventions. Two minor candidates:

| gtopt canonical | Category | Recommendation |
|-----------------|----------|----------------|
| `optimal_eps` / `feasible_eps` / `barrier_eps` | 🔄 Strong | Same as §11.4 — **→ `optimality_tolerance` / `feasibility_tolerance` / `barrier_tolerance`**. |
| `log_mode` | ✅ Keep | — `*_mode` convention. |
| `memory_emphasis` | ✅ Keep | — CPLEX term, well known. |
| `max_fallbacks` | ✅ Keep | — gtopt's solver-retry shim; self-documenting. |
| `algorithm` / `threads` / `presolve` / `time_limit` / `scaling` / `crossover` / `log_level` | ✅ Keep | — Universal across every solver and meta-tool. |

### 11.7 Monolithic / LP-Matrix / LP-Validation Options

| gtopt canonical | Category | Recommendation |
|-----------------|----------|----------------|
| `solve_mode` | ✅ Keep | — `*_mode` convention. |
| `boundary_cuts_*` | ✅ Keep | — Standard SDDP vocabulary. |
| `boundary_max_iterations` | ✅ Keep | — Self-documenting. |
| `lp_coeff_ratio_threshold` | ✅ Keep | — Numerical-diagnostic term, gtopt-specific. |
| `equilibration_method` / `fast_sqrt_method` | ✅ Keep | — gtopt-specific. |
| `compute_stats` | ✅ Keep | — Universal. |
| `enable` (on `LpValidation`) | ✅ Keep | — Match the universal "enable a feature" convention. |
| `coeff_warn_max` / `coeff_warn_min` / `bound_warn_max` / `rhs_warn_max` / `obj_warn_max` | 🤔 Weak | (consistent already; or extend to `coefficient_warning_max` etc. for full clarity — five flags, modest churn). |
| `max_warnings_per_kind` | ✅ Keep | — Self-documenting. |

### 11.8 Simulation Options (on `simulation`)

| gtopt canonical | Category | Recommendation |
|-----------------|----------|----------------|
| `annual_discount_rate` | ✅ Keep | — Standard. |
| `boundary_cuts_file` / `boundary_cuts_valuation` | ✅ Keep | — Standard SDDP. |
| `probability_rescale` | ✅ Keep | — Standard SDDP / PLEXOS. |
| `kappa_warning` / `kappa_threshold` | ✅ Keep | — Numerical-conditioning standard. |
| `block_hour_map` | ✅ Keep | — gtopt-specific timing helper. |

### 11.9 Time-Axis Field Names

| gtopt canonical | Category | Recommendation |
|-----------------|----------|----------------|
| `Stage.first_block` / `count_block` | 🤔 Weak | (keep, or → `first_block_index` / `block_count` — but the current pair is concise and consistent). |
| `Phase.first_stage` / `count_stage` | 🤔 Weak | (same comment). |
| `Scene.first_scenario` / `count_scenario` | 🤔 Weak | (same comment). |
| `Stage.chronological` (bool) | ✅ Keep | — Self-documenting. |
| `Stage.month` | ✅ Keep | — Universal. |
| `Stage.discount_factor` | ✅ Keep | — Standard financial term. |
| `Scenario.probability_factor` | ✅ Keep | — Standard. |
| `Scenario.hydrology` | ✅ Keep | — PSR SDDP convention. |
| `Aperture.source_scenario` / `probability_factor` | ✅ Keep | — Standard. |
| `Iteration.index` / `update_lp` | ✅ Keep | — Self-documenting. |

### 11.10 Consolidated Rename Candidates

The "🔄 Strong" rows above, gathered for easy review:

| Current canonical | Proposed canonical | Reason |
|-------------------|---------------------|--------|
| `demand_fail_cost` | `value_of_lost_load` | IEEE-standard "VOLL". |
| `reserve_fail_cost` | `reserve_shortage_cost` | Aligns with PLEXOS `VoRS`, PSR `CustoFalta`. |
| `hydro_fail_cost` | `hydro_spill_cost` | "fail" too vague; matches PLEXOS / PSR. |
| `use_single_bus` | `copper_plate` (bool) **or** `network_mode = copper_plate \| dc_opf \| ac_opf` | PLEXOS standard term; the mode-enum form is the cleaner future-proof choice if AC OPF lands later. |
| `state_fail_cost` | `state_violation_cost` | Clearer semantics. |
| `demand_option_c` | `demand_fail_rhs_shift` | Replace internal code reference with semantic name. Both Options A and C substitute `fail = lmax − load`; Option C additionally absorbs the baseline into RHS shifts (bus-balance row `+(1+loss)·lmax`, capacity row `+lmax`) instead of `obj_constant`. The new name describes that distinguishing behaviour. |
| `convergence_tol` | `convergence_tolerance` | Full word; consistent across the rest of the option surface. |
| `feasible_eps` | `feasibility_tolerance` | Industry-standard solver-tolerance form. |
| `optimal_eps` | `optimality_tolerance` | Same. |
| `barrier_eps` | `barrier_tolerance` | Same. |
| `cut_coeff_eps` | `cut_coefficient_tolerance` | Same. |
| `update_lp_skip` | `lp_update_interval` | Positive framing; matches `cut_prune_interval`. |
| `use_uid_fname` | `use_uid_filenames` | Drop "fname" abbreviation. |

### 11.11 Implementation Note

If §10 (runtime dialects) lands first, every rename in §11.10
becomes a **single-line edit** in `element_names.hpp`:

```cpp
// Old canonical kept as gtopt-dialect alternate ⇒ all existing
// JSON files continue to parse with no dialect specified.
inline constexpr std::array kVollAlts = {
  DialectName{Dialect::gtopt,      "demand_fail_cost"},  // legacy
  DialectName{Dialect::plexos,     "VoLL"},
  DialectName{Dialect::sddp,       "CustoDeficitGlobal"},
  DialectName{Dialect::pypsa,      "load_shedding"},
};
inline constexpr FieldName Voll{"value_of_lost_load", kVollAlts};
```

The §10 DOM-rewrite pass then accepts both names on input and
emits the new canonical on output. **Zero case JSON edits
required** for the rename itself. This is a strong argument for
landing §10 before tackling §11.

### 11.12 Out of Scope

Things this review explicitly does *not* touch:

- The values of enum-typed options (`method = monolithic/sddp/...`,
  `cut_sharing_mode = none/...`). Those have their own
  conventions and would need separate review.
- Field names on physical elements — covered by §9 and the dialect
  mechanism in §10.
- Top-level structural keys (`generators` etc.) — covered by §6.1
  Tier 1.

### 11.13 Trimming Renames Through `--set` Awareness

§10.10a observes that **`--set canonical.path=value` works
dialect-independently** for any option. The corollary for this
review: options that users typically only touch via `--set`
(numerical tuning, debug flags) **don't benefit from a canonical
rename** — the user already types the canonical name on the CLI,
and the rename forces every existing shell script and CI pipeline
to migrate.

Re-classifying §11.10's "Strong rename" candidates against the
Tier-J / Tier-C split in §10.10a:

| Rename candidate | Tier | Re-classified verdict |
|------------------|------|------------------------|
| `demand_fail_cost` → `value_of_lost_load` | **J** (case-JSON) | **🔄 Keep rename** — user-edited; UX wins are real. |
| `reserve_fail_cost` → `reserve_shortage_cost` | **J** | **🔄 Keep rename** — case-JSON. |
| `hydro_fail_cost` → `hydro_spill_cost` | **J** | **🔄 Keep rename**. |
| `use_single_bus` → `copper_plate` (or mode enum) | **J** | **🔄 Keep rename** — case-JSON; high-visibility. |
| `state_fail_cost` → `state_violation_cost` | **J** | **🔄 Keep rename** — case-JSON penalty. |
| `demand_option_c` → `demand_fail_rhs_shift` | **J** | **🔄 Keep rename** — case-JSON behavior toggle. |
| `convergence_tol` → `convergence_tolerance` | **C** (CLI-tuned) | **⏸ Defer / drop** — almost always set via `--set sddp_options.convergence_tol=…`. Renaming breaks every existing CI script for marginal docs win. |
| `feasible_eps` / `optimal_eps` / `barrier_eps` → `*_tolerance` | **C** | **⏸ Defer / drop** — solver-tuning knobs; `--set solver_options.feasible_eps=…` is the canonical access path. |
| `cut_coeff_eps` → `cut_coefficient_tolerance` | **C** | **⏸ Defer / drop**. |
| `update_lp_skip` → `lp_update_interval` | **C** | **⏸ Defer / drop** — SDDP-tuning. |
| `use_uid_fname` → `use_uid_filenames` | **C** (build flag) | **🤔 Borderline** — typically `--set use_uid_fname=true`, but reads badly in docs. Keep on the rename list, low priority. |

**Effective canonical-rename shortlist** (after `--set` filtering):

1. `demand_fail_cost` → `value_of_lost_load`
2. `reserve_fail_cost` → `reserve_shortage_cost`
3. `hydro_fail_cost` → `hydro_spill_cost`
4. `use_single_bus` → `copper_plate` (or evolve to `network_mode` enum)
5. `state_fail_cost` → `state_violation_cost`
6. `demand_option_c` → `demand_fail_rhs_shift`
7. *(optional, low-priority)* `use_uid_fname` → `use_uid_filenames`

**Six high-confidence canonical renames** vs the original 13 in
§11.10. The dropped seven are all numerical tolerances that live
on the CLI tuning surface — `--set` is doing the cross-tool
ergonomics work that a rename would otherwise have to.

### 11.14 Principle

> A canonical option name only deserves a rename if the option is
> typically read or written **inside JSON case files**. Options that
> live primarily on the CLI tuning surface get cross-tool ergonomics
> for free via `--set <canonical.path>=<value>` — renaming them is
> all churn, no signal.

Apply this principle when considering any future option-rename
proposal: classify Tier-J vs Tier-C first; reject Tier-C renames
unless the existing name is actively misleading.

---

## 12. Implementation Status — 2026-05 (post §10 Phase 1)

What's actually shipping versus what §10 originally proposed.

### 12.1 `--naming-dialect <name>` (CLI + JSON, IMPLEMENTED)

`model_options.naming_dialect` (canonical) / `--naming-dialect` (CLI).
Recognised values are the `dialect` tags in
`share/gtopt/naming_dialects.json` — currently `gtopt`,
`gtopt-legacy`, `plp`, `sddp`, `plexos`, `pypsa`, `pandapower`.

**Input warn** — when set, the alias canonicalization at JSON parse
time consults `NamesRegistry::dialect_for(alias)` for every alias it
rewrites.  When the alias's source dialect differs from the
enforced dialect, a once-per-alias warning fires through
`spdlog::warn`.  Reduces silent dialect drift — e.g. a PLEXOS
`Max Capacity` slipping into a `--naming-dialect=gtopt` run.

**Output rename** — at `planning.json` write time the JSON
canonicalize pass runs in reverse:
`decanonicalize_json_keys(text, dialect)` rewrites each canonical
key to the chosen dialect's alias from
`NamesRegistry::alias_for(canonical, dialect)`.  Canonicals without
a matching alias in the requested dialect pass through unchanged
(partial coverage degrades gracefully).

**NOT YET WIRED** — parquet column rename on element output files
(`Generator/generation_sol.parquet`, …).  The JSON write path is the
only output channel currently dialect-aware.

### 12.2 Unit dictionary — `share/gtopt/unit_dialects.json` (IMPLEMENTED)

Sibling registry to `naming_dialects.json`, loaded via
`UnitRegistry::instance()`.  Each entry annotates the canonical
attribute of a class with the physical unit expected by a given
dialect:

    {"class": "reservoir", "canonical": "emax", "dialect": "gtopt", "unit": "Mm3"},
    {"class": "reservoir", "canonical": "emax", "dialect": "plp",   "unit": "hm3"}

The `--naming-dialect` input warn pass consults
`UnitRegistry::class_agnostic_unit_for(canonical, dialect)` for the
alias's source dialect and the enforced dialect.  When both lookups
succeed and the units differ, the warning escalates to
`spdlog::error` with both units named:

    UNIT MISMATCH on input alias 'VolMax' (dialect 'plp', unit 'hm3')
    vs --naming-dialect 'gtopt' (unit 'Mm3') for canonical 'emax'

**Known limitation** — the unit lookup is class-blind because
`canonicalize_json_keys` operates on the JSON token stream before
the element class is known.  When the same canonical name lives on
two classes with different units (e.g. `cost` is USD/MWh on
generator, USD/MMBtu on fuel), `class_agnostic_unit_for` returns
nullopt and the warn falls back to the dialect-only line.

**NOT YET WIRED** — auto-conversion at parse time (the "phase 3"
mentioned in the original proposal).  The unit table is purely
diagnostic for now; numeric values are passed through verbatim.

### 12.3 `--list-dialects [<dialect>]` (IMPLEMENTED)

Diagnostic dump of the merged naming + unit registries.  Without an
argument, prints every `(canonical, dialect, alias, unit)` row,
tab-separated.  With an argument, restricts to that dialect:

    $ gtopt --list-dialects plp
    # canonical    dialect    alias       unit
    pmax           plp        PotMax      MW
    emax           plp        VolMax      hm3
    fmax           plp        QMax        m3/s
    ...

Errors with exit code 1 and lists the registered dialects when the
filter does not match any known dialect.  The footer line names the
source file each registry loaded from
(`naming_dialects.json` / `unit_dialects.json`), making the dump
self-documenting under environment overrides.

### 12.4 igtopt template integration (IMPLEMENTED)

`scripts/igtopt/_options_meta.py` exposes the three new fields in
the Excel template:

* `naming_dialect` and `continuous_phases` in `MODEL_OPTION_KEYS`
  (so they nest correctly into `model_options.*`).
* `mip_gap` in `SOLVER_OPTION_KEYS` (so `solver_mip_gap` nests into
  `solver_options.mip_gap`).

### 12.5 Deferred to future commits

* **Parquet column rename** under `--naming-dialect` (the second half
  of "output rename" — currently only `planning.json` is renamed).
* **Auto-conversion** at parse time (phase 3) — needs a unit algebra
  engine before a one-line conversion factor can be safely applied.
* **`--list-dialects --format json`** machine-readable variant of
  the tab-separated dump.
* **Fill remaining missing element classes** in
  `naming_dialects.json` (Junction, Pump, VolumeRight, LngTerminal,
  GeneratorProfile, DemandProfile, reservoir sub-records).  The
  external-tool mappings for those are best-effort-from-memory
  and have been left out of the registry until verified against
  real exports.
