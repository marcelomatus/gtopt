# gtopt Input Data Structure Documentation

This document describes the input data structure required to define and run
optimization cases with **gtopt** (Generation and Transmission Optimization
Planning Tool).

> For a step-by-step tutorial with worked examples and time-series workflow,
> see **[PLANNING_GUIDE.md](PLANNING_GUIDE.md)**.
> For auto-generated field tables from source code run:
> `python3 scripts/gtopt_field_extractor.py --format html --output field_reference.html`

---

## Overview

A gtopt case is defined by one or more **JSON configuration files** that
contain three top-level sections:

| Section        | Description |
|----------------|-------------|
| `options`      | Global solver and I/O settings |
| `simulation`   | Time structure: blocks, stages, scenarios |
| `system`       | Physical system: buses, generators, demands, lines, etc. |

The JSON file may reference **external data files** (CSV or Parquet) for
time-series or profile data.  These files are stored in a subdirectory
specified by `options.input_directory`.

### Directory Layout

```
case_name/
├── case_name.json            # Main configuration file
└── <input_directory>/        # External data files
    ├── Demand/
    │   └── lmax.parquet      # Demand load profile
    ├── Generator/
    │   └── pmax.parquet      # Generator capacity profile
    └── ...
```

---

## 1. Options

Global settings that control solver behavior and I/O formats.

| Field                  | Type     | Units        | Description |
|------------------------|----------|--------------|-------------|
| `annual_discount_rate` | number   | p.u./year    | Annual discount rate for cost calculations |
| `demand_fail_cost`     | number   | $/MWh        | Penalty cost for unserved demand (value of lost load) |
| `reserve_fail_cost`    | number   | $/MWh        | Penalty cost for unserved spinning reserve |
| `scale_objective`      | number   | dimensionless| Divide objective by this value for numerical stability |
| `scale_theta`          | number   | dimensionless| Scaling factor for voltage-angle variables |
| `kirchhoff_threshold`  | number   | kV           | Minimum bus voltage for Kirchhoff constraint application |
| `use_line_losses`      | boolean  | —            | Enable line loss modeling |
| `use_kirchhoff`        | boolean  | —            | Enable Kirchhoff's voltage law constraints |
| `use_single_bus`       | boolean  | —            | Collapse system to single bus (copper-plate) |
| `use_lp_names`         | boolean  | —            | Use human-readable LP variable names |
| `use_uid_fname`        | boolean  | —            | Use UID-based file names |
| `input_directory`      | string   | —            | Directory for external data files |
| `input_format`         | string   | —            | Format of input files: `"csv"` or `"parquet"` |
| `output_directory`     | string   | —            | Directory for output files |
| `output_format`        | string   | —            | Format of output files: `"csv"` or `"parquet"` |
| `output_compression`   | string   | `"zstd"`     | Compression for output files: `"zstd"` (default), `"gzip"`, `"uncompressed"` |
| `lp_algorithm`        | integer  | —            | LP solver algorithm code (0 = auto) |
| `lp_threads`          | integer  | —            | Number of LP solver threads (0 = auto) |
| `lp_presolve`         | boolean  | —            | Enable LP presolve in the solver |
| `log_directory`       | string   | `"logs"`     | Directory for log and error LP files |
| `lp_debug`            | boolean  | —            | Save LP debug files to `log_directory` before solving. Monolithic solver: one file per `(scene, phase)`; SDDP solver: one file per `(iteration, scene, phase)`. Files are named `gtopt_lp_<scene>_<phase>.lp` (monolithic) or `gtopt_iter_<iter>_<scene>_<phase>.lp` (SDDP). When `output_compression` is set to a codec (default `"zstd"`, or any value other than `"uncompressed"`), files are compressed and the originals removed. |

### Example

```json
{
  "options": {
    "annual_discount_rate": 0.1,
    "use_lp_names": true,
    "output_format": "csv",
    "use_single_bus": false,
    "demand_fail_cost": 1000,
    "scale_objective": 1000,
    "use_kirchhoff": true,
    "input_directory": "system_c0",
    "input_format": "parquet"
  }
}
```

---

## 2. Simulation

Defines the temporal structure of the optimization.

### 2.1 Block

A block is the smallest indivisible time unit. `energy [MWh] = power [MW] × duration [h]`.

| Field      | Type   | Units | Required | Description |
|------------|--------|-------|----------|-------------|
| `uid`      | integer| —     | Yes      | Unique identifier |
| `name`     | string | —     | No       | Optional name |
| `duration` | number | h     | Yes      | Duration of the block |

### 2.2 Stage

A stage groups consecutive blocks into a planning/investment period.

| Field            | Type    | Units | Required | Description |
|------------------|---------|-------|----------|-------------|
| `uid`            | integer | —     | Yes      | Unique identifier |
| `name`           | string  | —     | No       | Optional name |
| `first_block`    | integer | —     | Yes      | 0-based index of the first block in this stage |
| `count_block`    | integer | —     | Yes      | Number of consecutive blocks in this stage |
| `discount_factor`| number  | p.u.  | No       | Present-value cost multiplier for this stage |
| `active`         | boolean | —     | No       | Whether the stage is active |

### 2.3 Scenario

A scenario represents a possible future realization (hydrology, demand level, etc.).

| Field               | Type    | Units | Required | Description |
|---------------------|---------|-------|----------|-------------|
| `uid`               | integer | —     | Yes      | Unique identifier |
| `name`              | string  | —     | No       | Optional name |
| `probability_factor`| number  | p.u.  | No       | Probability weight (values are normalised to sum to 1) |
| `active`            | boolean | —     | No       | Whether the scenario is active |

### Example

```json
{
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 1},
      {"uid": 2, "duration": 2}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "active": 1},
      {"uid": 2, "first_block": 1, "count_block": 1, "active": 1}
    ],
    "scenario_array": [
      {"uid": 1, "probability_factor": 1}
    ]
  }
}
```

---

## 3. System

The system section defines all physical components of the power system.

### 3.1 Bus

An electrical bus (node) in the network.

| Field             | Type    | Units | Required | Description |
|-------------------|---------|-------|----------|-------------|
| `uid`             | integer | —     | Yes      | Unique identifier |
| `name`            | string  | —     | Yes      | Bus name |
| `active`          | boolean | —     | No       | Whether the bus is active |
| `voltage`         | number  | kV    | No       | Nominal voltage level |
| `reference_theta` | number  | rad   | No       | Fixed voltage angle (reference bus: set to 0) |
| `use_kirchhoff`   | boolean | —     | No       | Override global Kirchhoff setting for this bus |

### 3.2 Generator

A generation unit connected to a bus.

| Field              | Type                | Units        | Required | Description |
|--------------------|---------------------|--------------|----------|-------------|
| `uid`              | integer             | —            | Yes      | Unique identifier |
| `name`             | string              | —            | Yes      | Generator name |
| `bus`              | integer\|string     | —            | Yes      | Connected bus UID or name |
| `active`           | boolean             | —            | No       | Whether the generator is active |
| `pmin`             | number\|array\|string| MW          | No       | Minimum active power output |
| `pmax`             | number\|array\|string| MW          | No       | Maximum active power output |
| `gcost`            | number\|array\|string| $/MWh       | No       | Variable generation cost |
| `lossfactor`       | number\|array\|string| p.u.        | No       | Network loss factor |
| `capacity`         | number\|array\|string| MW          | No       | Installed capacity |
| `expcap`           | number\|array\|string| MW          | No       | Capacity added per expansion module |
| `expmod`           | number\|array\|string| —           | No       | Maximum number of expansion modules |
| `capmax`           | number\|array\|string| MW          | No       | Absolute maximum capacity |
| `annual_capcost`   | number\|array\|string| $/MW-year   | No       | Annualized investment cost |
| `annual_derating`  | number\|array\|string| p.u./year   | No       | Annual capacity derating factor |

> **Note:** Fields that accept `number|array|string` can be a numeric constant,
> an inline array (indexed by `[stage][block]`), or a filename referencing an
> external Parquet/CSV file in `input_directory/Generator/`.

### 3.3 Demand

An electrical demand (load) connected to a bus.

| Field              | Type                | Units        | Required | Description |
|--------------------|---------------------|--------------|----------|-------------|
| `uid`              | integer             | —            | Yes      | Unique identifier |
| `name`             | string              | —            | Yes      | Demand name |
| `bus`              | integer\|string     | —            | Yes      | Connected bus UID or name |
| `active`           | boolean             | —            | No       | Whether the demand is active |
| `lmax`             | number\|array\|string| MW          | No       | Maximum served load |
| `lossfactor`       | number\|array\|string| p.u.        | No       | Network loss factor |
| `fcost`            | number\|array\|string| $/MWh       | No       | Demand curtailment cost |
| `emin`             | number\|array\|string| MWh         | No       | Minimum energy that must be served per stage |
| `ecost`            | number\|array\|string| $/MWh       | No       | Energy-shortage cost |
| `capacity`         | number\|array\|string| MW          | No       | Installed capacity |
| `expcap`           | number\|array\|string| MW          | No       | Capacity added per expansion module |
| `expmod`           | number\|array\|string| —           | No       | Maximum number of expansion modules |
| `capmax`           | number\|array\|string| MW          | No       | Absolute maximum capacity |
| `annual_capcost`   | number\|array\|string| $/MW-year   | No       | Annualized investment cost |
| `annual_derating`  | number\|array\|string| p.u./year   | No       | Annual capacity derating factor |

### 3.4 Line

A transmission line connecting two buses.

| Field              | Type                | Units        | Required | Description |
|--------------------|---------------------|--------------|----------|-------------|
| `uid`              | integer             | —            | Yes      | Unique identifier |
| `name`             | string              | —            | Yes      | Line name |
| `bus_a`            | integer\|string     | —            | Yes      | Sending-end (from) bus |
| `bus_b`            | integer\|string     | —            | Yes      | Receiving-end (to) bus |
| `active`           | boolean             | —            | No       | Whether the line is active |
| `voltage`          | number\|array\|string| kV          | No       | Nominal voltage level |
| `resistance`       | number\|array\|string| p.u.        | No       | Series resistance |
| `reactance`        | number\|array\|string| p.u.        | No       | Series reactance (DC power flow) |
| `lossfactor`       | number\|array\|string| p.u.        | No       | Lumped loss factor |
| `type`             | string              | —            | No       | Element type tag; use `"transformer"` for transformers |
| `tap_ratio`        | number\|array\|string| p.u.        | No       | Off-nominal tap ratio τ (default 1.0). A tap-changing transformer with τ ≠ 1 has effective susceptance $B/\tau$. Supports per-stage schedule. |
| `phase_shift_deg`  | number\|array\|string| degrees     | No       | Phase-shift angle φ in degrees (default 0). Models a Phase-Shifting Transformer (PST); shifts the Kirchhoff constraint RHS by $-\sigma_\theta \cdot \phi_{\text{rad}}$. |
| `tmax_ab`          | number\|array\|string| MW          | No       | Max flow in A→B direction |
| `tmax_ba`          | number\|array\|string| MW          | No       | Max flow in B→A direction |
| `tcost`            | number\|array\|string| $/MWh       | No       | Variable transmission cost |
| `capacity`         | number\|array\|string| MW          | No       | Installed capacity |
| `expcap`           | number\|array\|string| MW          | No       | Capacity added per expansion module |
| `expmod`           | number\|array\|string| —           | No       | Maximum number of expansion modules |
| `capmax`           | number\|array\|string| MW          | No       | Absolute maximum capacity |
| `annual_capcost`   | number\|array\|string| $/MW-year   | No       | Annualized investment cost |
| `annual_derating`  | number\|array\|string| p.u./year   | No       | Annual capacity derating factor |

### 3.5 Battery

A battery energy storage system (BESS).

#### Standalone battery (unified definition)

When the optional `bus` field is set (without `source_generator`), the system
auto-generates a discharge Generator, a charge Demand, and a linking Converter
during preprocessing. Both charge and discharge connect to the same external
bus. Only a single Battery element is needed — no separate Converter,
Generator, or Demand definitions required.

```json
{
  "uid": 1, "name": "bess1",
  "bus": 3,
  "input_efficiency": 0.95, "output_efficiency": 0.95,
  "emin": 0, "emax": 200,
  "pmax_charge": 60, "pmax_discharge": 60,
  "gcost": 0,
  "capacity": 200
}
```

#### Generation-coupled battery (hybrid / behind-the-meter)

When both `bus` and `source_generator` are set, the battery operates in
*generation-coupled* mode: the referenced generator directly charges the
battery through an auto-created internal bus. This models a solar or wind
plant that sits "behind" the battery (e.g. a DC-coupled or AC-coupled
hybrid system).

`System::expand_batteries()` will:
- Create an internal bus (name = `<battery.name>_int_bus`)
- Connect the **discharge Generator** to the external `bus`
- Connect the **charge Demand** to the internal bus
- Rewire the **source generator** to the internal bus

The `source_generator` value is the UID or name of a Generator element
already defined in `generator_array`. Its own `bus` field is overwritten
with the internal bus.

```json
{
  "uid": 1, "name": "bess1",
  "bus": 3,
  "source_generator": "solar1",
  "input_efficiency": 0.95, "output_efficiency": 0.95,
  "emin": 0, "emax": 200,
  "pmax_charge": 60, "pmax_discharge": 60,
  "gcost": 0,
  "capacity": 200
}
```

#### Traditional definition

Without the `bus` field, a separate Converter, Generator, and Demand must
be defined manually (see §3.6 Converter).

| Field               | Type                | Units        | Required | Description |
|---------------------|---------------------|--------------|----------|-------------|
| `uid`               | integer             | —            | Yes      | Unique identifier |
| `name`              | string              | —            | Yes      | Battery name |
| `active`            | boolean             | —            | No       | Whether the battery is active |
| `bus`               | integer\|string     | —            | No       | Bus connection (enables unified / coupled definition) |
| `source_generator`  | integer\|string     | —            | No       | Source generator for generation-coupled mode (battery name or UID) |
| `input_efficiency`  | number\|array\|string| p.u.        | No       | Charging efficiency |
| `output_efficiency` | number\|array\|string| p.u.        | No       | Discharging efficiency |
| `annual_loss`       | number\|array\|string| p.u./year   | No       | Annual self-discharge rate |
| `emin`              | number\|array\|string| MWh         | No       | Minimum state of charge |
| `emax`              | number\|array\|string| MWh         | No       | Maximum state of charge |
| `ecost`             | number\|array\|string| $/MWh       | No       | Storage usage cost (penalty) |
| `eini`              | number              | MWh          | No       | Initial state of charge |
| `efin`              | number              | MWh          | No       | Terminal state of charge |
| `pmax_charge`       | number\|array\|string| MW          | No       | Max charging power (unified definition) |
| `pmax_discharge`    | number\|array\|string| MW          | No       | Max discharging power (unified definition) |
| `gcost`             | number\|array\|string| $/MWh       | No       | Discharge generation cost (unified definition) |
| `capacity`          | number\|array\|string| MWh         | No       | Installed energy capacity |
| `expcap`            | number\|array\|string| MWh         | No       | Energy capacity per expansion module |
| `expmod`            | number\|array\|string| —           | No       | Maximum number of expansion modules |
| `capmax`            | number\|array\|string| MWh         | No       | Absolute maximum energy capacity |
| `annual_capcost`    | number\|array\|string| $/MWh-year  | No       | Annualized investment cost |
| `annual_derating`   | number\|array\|string| p.u./year   | No       | Annual capacity derating factor |

### 3.6 Converter

Links a battery to a discharge generator and a charge demand.

| Field              | Type                | Units           | Required | Description |
|--------------------|---------------------|-----------------|----------|-------------|
| `uid`              | integer             | —               | Yes      | Unique identifier |
| `name`             | string              | —               | Yes      | Converter name |
| `active`           | boolean             | —               | No       | Whether the converter is active |
| `battery`          | integer\|string     | —               | Yes      | Associated battery UID or name |
| `generator`        | integer\|string     | —               | Yes      | Discharge generator UID or name |
| `demand`           | integer\|string     | —               | Yes      | Charge demand UID or name |
| `conversion_rate`  | number\|array\|string| MW/(MWh/h)     | No       | Electrical output per unit stored energy withdrawn |
| `capacity`         | number\|array\|string| MW              | No       | Installed power capacity |
| `expcap`           | number\|array\|string| MW              | No       | Power capacity per expansion module |
| `expmod`           | number\|array\|string| —               | No       | Maximum number of expansion modules |
| `capmax`           | number\|array\|string| MW              | No       | Absolute maximum power capacity |
| `annual_capcost`   | number\|array\|string| $/MW-year       | No       | Annualized investment cost |
| `annual_derating`  | number\|array\|string| p.u./year       | No       | Annual capacity derating factor |

### 3.7 Junction

A hydraulic node in the water network.

| Field    | Type    | Units | Required | Description |
|----------|---------|-------|----------|-------------|
| `uid`    | integer | —     | Yes      | Unique identifier |
| `name`   | string  | —     | Yes      | Junction name |
| `active` | boolean | —     | No       | Whether the junction is active |
| `drain`  | boolean | —     | No       | If true, excess water can leave the system freely |

### 3.8 Waterway

A water channel connecting two junctions.

| Field        | Type                | Units  | Required | Description |
|--------------|---------------------|--------|----------|-------------|
| `uid`        | integer             | —      | Yes      | Unique identifier |
| `name`       | string              | —      | Yes      | Waterway name |
| `active`     | boolean             | —      | No       | Whether the waterway is active |
| `junction_a` | integer\|string     | —      | Yes      | Upstream junction UID or name |
| `junction_b` | integer\|string     | —      | Yes      | Downstream junction UID or name |
| `capacity`   | number\|array\|string| m³/s  | No       | Maximum flow capacity |
| `lossfactor` | number\|array\|string| p.u.  | No       | Transit loss coefficient |
| `fmin`       | number\|array\|string| m³/s  | No       | Minimum required water flow |
| `fmax`       | number\|array\|string| m³/s  | No       | Maximum allowed water flow |

### 3.9 Reservoir

A water reservoir connected to a junction.  Volume units: **dam³** (1 dam³ = 1 000 m³).

| Field                  | Type                | Units       | Required | Description |
|------------------------|---------------------|-------------|----------|-------------|
| `uid`                  | integer             | —           | Yes      | Unique identifier |
| `name`                 | string              | —           | Yes      | Reservoir name |
| `active`               | boolean             | —           | No       | Whether the reservoir is active |
| `junction`             | integer\|string     | —           | Yes      | Associated junction UID or name |
| `spillway_capacity`    | number              | m³/s        | No       | Maximum uncontrolled spill capacity |
| `spillway_cost`        | number              | $/dam³      | No       | Penalty per unit of spilled water |
| `capacity`             | number\|array\|string| dam³       | No       | Total usable storage capacity |
| `annual_loss`          | number\|array\|string| p.u./year  | No       | Annual evaporation/seepage loss rate |
| `emin`                 | number\|array\|string| dam³       | No       | Minimum allowed stored volume |
| `emax`                 | number\|array\|string| dam³       | No       | Maximum allowed stored volume |
| `ecost`                | number\|array\|string| $/dam³     | No       | Water value (shadow cost of stored water) |
| `eini`                 | number              | dam³        | No       | Initial stored volume |
| `efin`                 | number              | dam³        | No       | Target final stored volume |
| `fmin`                 | number              | m³/s        | No       | Minimum net inflow |
| `fmax`                 | number              | m³/s        | No       | Maximum net inflow |
| `vol_scale`            | number              | —           | No       | Multiplicative scaling factor for volume |
| `flow_conversion_rate` | number              | dam³/(m³/s·h)| No     | Converts m³/s × hours to dam³ (default: 0.0036) |

### 3.10 Turbine

A hydro turbine linking a waterway to a generator.

| Field             | Type                | Units      | Required | Description |
|-------------------|---------------------|------------|----------|-------------|
| `uid`             | integer             | —          | Yes      | Unique identifier |
| `name`            | string              | —          | Yes      | Turbine name |
| `active`          | boolean             | —          | No       | Whether the turbine is active |
| `waterway`        | integer\|string     | —          | Yes      | Associated waterway UID or name |
| `generator`       | integer\|string     | —          | Yes      | Associated generator UID or name |
| `drain`           | boolean             | —          | No       | If true, turbine can spill water without generating |
| `conversion_rate` | number\|array\|string| MW·s/m³   | No       | Water-to-power conversion factor |
| `capacity`        | number\|array\|string| MW        | No       | Maximum turbine power output |
| `main_reservoir`  | integer\|string     | —          | No       | Reservoir whose volume drives the efficiency curve |

### 3.11 Flow (Inflow)

A water inflow or outflow at a junction.

| Field       | Type                | Units | Required | Description |
|-------------|---------------------|-------|----------|-------------|
| `uid`       | integer             | —     | Yes      | Unique identifier |
| `name`      | string              | —     | Yes      | Flow name |
| `active`    | boolean             | —     | No       | Whether the flow is active |
| `direction` | integer             | —     | No       | +1 = inflow, −1 = outflow |
| `junction`  | integer\|string     | —     | Yes      | Associated junction UID or name |
| `discharge` | number\|array\|string| m³/s | Yes      | Water discharge schedule |

### 3.12 Filtration

Piecewise-linear seepage model from a waterway to an adjacent reservoir,
representing water losses due to soil permeability (Darcy's law
approximation).  The seepage flow through the filtration waterway is
constrained to:

```
seepage [m³/s] = slope [m³/s/dam³] × avg_reservoir_volume [dam³] + constant [m³/s]
```

where `avg_reservoir_volume = (eini + efin) / 2`.  This captures the
hydrostatic-head dependence of seepage.  Corresponds to PLP **plpcenfi.dat**
(*Centrales Filtración*).

When piecewise-linear `segments` are provided, the active segment is
selected based on the reservoir's current volume (vini from the previous
phase).  The LP constraint coefficients (slope and RHS) are updated
dynamically — the same mechanism used by Reservoir Efficiency for turbine
conversion rates.  If `segments` is empty, the static `slope` and
`constant` fields are used as a simple linear model.

| Field       | Type    | Units        | Required | Description |
|-------------|---------|--------------|----------|-------------|
| `uid`       | integer | —            | Yes      | Unique identifier |
| `name`      | string  | —            | Yes      | Filtration name |
| `active`    | boolean | —            | No       | Whether the filtration is active |
| `waterway`  | integer\|string | —    | Yes      | Source waterway UID or name |
| `reservoir` | integer\|string | —    | Yes      | Receiving reservoir UID or name |
| `slope`     | number  | m³/s / dam³  | No       | Default seepage slope (used when `segments` is empty) |
| `constant`  | number  | m³/s         | No       | Default constant seepage rate (used when `segments` is empty) |
| `segments`  | array   | —            | No       | Piecewise-linear concave segments (see below) |

Each segment in the `segments` array describes one piece of the concave
filtration envelope:

| Field      | Type   | Units          | Description |
|------------|--------|----------------|-------------|
| `volume`   | number | dam³           | Volume breakpoint |
| `slope`    | number | m³/s / dam³    | Seepage slope at this breakpoint |
| `constant` | number | m³/s           | Seepage rate at this breakpoint |

The filtration rate at volume *V* is computed as the minimum over all
segments: `filtration(V) = min_i { constant_i + slope_i × (V − volume_i) }`.
This is analogous to the piecewise-linear efficiency evaluation used in
[Reservoir Efficiency](#313-reservoir-efficiency).

**Example with segments:**

```json
{
  "uid": 1,
  "name": "filt1",
  "waterway": "w1_2",
  "reservoir": "r1",
  "slope": 0.001,
  "constant": 1.0,
  "segments": [
    { "volume": 0.0, "slope": 0.0003, "constant": 0.5 },
    { "volume": 5000.0, "slope": 0.0001, "constant": 2.0 }
  ]
}
```

### 3.13 Reservoir Efficiency

Piecewise-linear turbine efficiency as a function of reservoir volume
(hydraulic head).  Models the PLP "rendimiento" concept: the turbine
conversion rate varies with the current water level in the reservoir.
Corresponds to PLP **plpcenre.dat** (*Archivo de Rendimiento de Embalses*).

When the SDDP solver runs, it updates the turbine's conversion-rate LP
coefficient at each forward-pass iteration using the current reservoir
volume.  For the first iteration, the reservoir's `eini` value is used.
If the LP solver does not support in-place coefficient modification
(`supports_set_coeff()`), the static `conversion_rate` from the Turbine
element is used unchanged.

| Field                        | Type    | Units          | Required | Description |
|------------------------------|---------|----------------|----------|-------------|
| `uid`                        | integer | —              | Yes      | Unique identifier |
| `name`                       | string  | —              | Yes      | Efficiency element name |
| `active`                     | boolean | —              | No       | Whether the element is active |
| `turbine`                    | integer\|string | —      | Yes      | Associated turbine UID or name |
| `reservoir`                  | integer\|string | —      | Yes      | Associated reservoir UID or name |
| `mean_efficiency`            | number  | MW·s/m³        | No       | Fallback efficiency (default: 1.0) |
| `segments`                   | array   | —              | No       | Piecewise-linear concave segments |
| `sddp_efficiency_update_skip`| integer | —              | No       | SDDP iterations to skip between updates |

Each segment in the `segments` array has the following fields:

| Field      | Type   | Units           | Description |
|------------|--------|-----------------|-------------|
| `volume`   | number | dam³            | Volume breakpoint |
| `slope`    | number | efficiency/dam³ | Slope at this breakpoint |
| `constant` | number | MW·s/m³         | Intercept (efficiency) at this breakpoint |

The efficiency is computed as the **minimum** over all segments:

```
efficiency(V) = min_i { constant_i + slope_i × (V − volume_i) }
```

Segments should have slopes in **decreasing** order so the function forms
a concave envelope.  The result is clamped to zero (efficiency cannot be
negative).

**Example:**

```json
{
  "reservoir_efficiency_array": [
    {
      "uid": 1,
      "name": "eff_colbun",
      "turbine": "COLBUN",
      "reservoir": "COLBUN",
      "mean_efficiency": 1.53,
      "segments": [
        { "volume": 0.0, "slope": 0.0002294, "constant": 1.2558 },
        { "volume": 500.0, "slope": 0.0001, "constant": 1.53 }
      ]
    }
  ]
}
```

### 3.14 Generator Profile

A time-varying capacity-factor profile for a generator.

| Field       | Type                | Units | Required | Description |
|-------------|---------------------|-------|----------|-------------|
| `uid`       | integer             | —     | Yes      | Unique identifier |
| `name`      | string              | —     | Yes      | Profile name |
| `active`    | boolean             | —     | No       | Whether the profile is active |
| `generator` | integer\|string     | —     | Yes      | Associated generator UID or name |
| `profile`   | number\|array\|string| p.u. | Yes      | Capacity-factor profile (0–1) |
| `scost`     | number\|array\|string| $/MWh| No       | Short-run generation cost override |

### 3.15 Demand Profile

A time-varying load-shape profile for a demand element.

| Field    | Type                | Units | Required | Description |
|----------|---------------------|-------|----------|-------------|
| `uid`    | integer             | —     | Yes      | Unique identifier |
| `name`   | string              | —     | Yes      | Profile name |
| `active` | boolean             | —     | No       | Whether the profile is active |
| `demand` | integer\|string     | —     | Yes      | Associated demand UID or name |
| `profile`| number\|array\|string| p.u. | Yes      | Load-scaling profile (0–1) |
| `scost`  | number\|array\|string| $/MWh| No       | Short-run load-shedding cost override |

### 3.16 Reserve Zone

A spinning-reserve requirement zone.

| Field    | Type                | Units  | Required | Description |
|----------|---------------------|--------|----------|-------------|
| `uid`    | integer             | —      | Yes      | Unique identifier |
| `name`   | string              | —      | Yes      | Zone name |
| `active` | boolean             | —      | No       | Whether the zone is active |
| `urreq`  | number\|array\|string| MW    | No       | Up-reserve requirement |
| `drreq`  | number\|array\|string| MW    | No       | Down-reserve requirement |
| `urcost` | number\|array\|string| $/MW  | No       | Up-reserve shortage penalty |
| `drcost` | number\|array\|string| $/MW  | No       | Down-reserve shortage penalty |

### 3.17 Reserve Provision

A generator's contribution to reserve zones.

| Field                  | Type                | Units  | Required | Description |
|------------------------|---------------------|--------|----------|-------------|
| `uid`                  | integer             | —      | Yes      | Unique identifier |
| `name`                 | string              | —      | Yes      | Provision name |
| `active`               | boolean             | —      | No       | Whether the provision is active |
| `generator`            | integer\|string     | —      | Yes      | Associated generator UID or name |
| `reserve_zones`        | string              | —      | Yes      | Comma-separated reserve zone UIDs or names |
| `urmax`                | number\|array\|string| MW    | No       | Maximum up-reserve offer |
| `drmax`                | number\|array\|string| MW    | No       | Maximum down-reserve offer |
| `ur_capacity_factor`   | number\|array\|string| p.u.  | No       | Up-reserve capacity factor |
| `dr_capacity_factor`   | number\|array\|string| p.u.  | No       | Down-reserve capacity factor |
| `ur_provision_factor`  | number\|array\|string| p.u.  | No       | Up-reserve provision factor |
| `dr_provision_factor`  | number\|array\|string| p.u.  | No       | Down-reserve provision factor |
| `urcost`               | number\|array\|string| $/MW  | No       | Up-reserve bid cost |
| `drcost`               | number\|array\|string| $/MW  | No       | Down-reserve bid cost |

---

### 3.18 User Constraint

User-defined linear constraints applied to the LP formulation.
See **[USER_CONSTRAINTS.md](USER_CONSTRAINTS.md)** for the full syntax
reference and examples.

| Field         | Type            | Unit  | Required | Description |
|---------------|-----------------|-------|----------|-------------|
| `uid`         | integer         | —     | Yes      | Unique identifier |
| `name`        | string          | —     | Yes      | Human-readable constraint name |
| `active`      | boolean         | —     | No       | Activation flag (default: true) |
| `expression`  | string          | —     | Yes      | Constraint expression in AMPL-inspired syntax |

**System-level fields:**

| Field                    | Type            | Description |
|--------------------------|-----------------|-------------|
| `user_constraint_array`  | array           | Inline array of UserConstraint objects |
| `user_constraint_file`   | string          | Path to external JSON file with constraint array |

---

## 4. External Data Files

When a field value is a **string** instead of a number, it refers to an
external data file in the `input_directory`.  Files are organized by
component type:

```
<input_directory>/
├── Bus/
├── Generator/
│   └── pmax.parquet
├── Demand/
│   └── lmax.parquet
├── Line/
├── Battery/
└── ...
```

### File Format

External data files (CSV or Parquet) follow a tabular format with columns:

| Column     | Description |
|------------|-------------|
| `scenario` | Scenario UID |
| `stage`    | Stage UID |
| `block`    | Block UID |
| `uid:<N>`  | Value for element with UID N |

### Example CSV

```csv
"scenario","stage","block","uid:1"
1,1,1,10
1,2,2,15
1,3,3,8
```

---

## 5. Output Structure

After running gtopt, results are written to the `output_directory`:

```
output/
├── solution.csv              # Objective value, status
├── Bus/
│   └── balance_dual.csv      # Marginal costs per bus
├── Generator/
│   ├── generation_sol.csv    # Generation dispatch
│   └── generation_cost.csv   # Generation costs
└── Demand/
    ├── load_sol.csv           # Served load
    ├── fail_sol.csv           # Unserved demand
    ├── capacity_dual.csv      # Capacity dual values
    └── ...
```

Output files use the same tabular format as input files, with
`scenario`, `stage`, `block`, and `uid:<N>` columns.

---

## 6. Complete Example

See `cases/c0/system_c0.json` for a minimal working example with:
- 1 bus, 1 generator, 1 demand
- 5 time blocks, 5 stages, 1 scenario
- Demand expansion planning

---

## See also

- **[Mathematical Formulation](docs/formulation/MATHEMATICAL_FORMULATION.md)**
  — Full LP/MIP optimization formulation with academic references
- **[PLANNING_GUIDE.md](PLANNING_GUIDE.md)** — Step-by-step planning guide
  with worked examples
- **[USAGE.md](USAGE.md)** — Command-line options and advanced usage
- **[SCRIPTS.md](SCRIPTS.md)** — Python conversion utilities
