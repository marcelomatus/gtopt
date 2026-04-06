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
