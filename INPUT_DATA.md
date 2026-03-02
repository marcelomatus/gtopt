# gtopt Input Data Structure Documentation

This document describes the input data structure required to define and run
optimization cases with **gtopt** (Generation and Transmission Optimization
Planning Tool).

---

## Overview

A gtopt case is defined by a single **JSON configuration file** (e.g.
`system_c0.json`) that contains three top-level sections:

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

| Field                  | Type     | Description |
|------------------------|----------|-------------|
| `annual_discount_rate` | number   | Annual discount rate for cost calculations |
| `demand_fail_cost`     | number   | Penalty cost for unserved demand |
| `reserve_fail_cost`    | number   | Penalty cost for unserved reserve |
| `scale_objective`      | number   | Scaling factor for the objective function |
| `scale_theta`          | number   | Scaling factor for voltage angles |
| `kirchhoff_threshold`  | number   | Threshold for Kirchhoff constraints |
| `use_line_losses`      | boolean  | Enable line loss modeling |
| `use_kirchhoff`        | boolean  | Enable Kirchhoff's voltage law constraints |
| `use_single_bus`       | boolean  | Collapse system to single bus |
| `use_lp_names`         | boolean  | Use human-readable LP variable names |
| `use_uid_fname`        | boolean  | Use UID-based file names |
| `input_directory`      | string   | Directory for external data files |
| `input_format`         | string   | Format of input files: `"csv"` or `"parquet"` |
| `output_directory`     | string   | Directory for output files |
| `output_format`        | string   | Format of output files: `"csv"` or `"parquet"` |
| `output_compression`   | string   | Compression for output files |

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

A block represents a time period within a stage.

| Field      | Type   | Required | Description |
|------------|--------|----------|-------------|
| `uid`      | integer| Yes      | Unique identifier |
| `name`     | string | No       | Optional name |
| `duration` | number | Yes      | Duration of the block (hours) |

### 2.2 Stage

A stage groups consecutive blocks into a planning period.

| Field            | Type    | Required | Description |
|------------------|---------|----------|-------------|
| `uid`            | integer | Yes      | Unique identifier |
| `name`           | string  | No       | Optional name |
| `first_block`    | integer | Yes      | Index of the first block (0-based) |
| `count_block`    | integer | Yes      | Number of blocks in this stage |
| `discount_factor`| number  | No       | Stage-specific discount factor |
| `active`         | boolean | No       | Whether the stage is active |

### 2.3 Scenario

A scenario represents a possible future realization.

| Field               | Type    | Required | Description |
|---------------------|---------|----------|-------------|
| `uid`               | integer | Yes      | Unique identifier |
| `name`              | string  | No       | Optional name |
| `probability_factor`| number  | No       | Probability weight of this scenario |
| `active`            | boolean | No       | Whether the scenario is active |

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

| Field             | Type    | Required | Description |
|-------------------|---------|----------|-------------|
| `uid`             | integer | Yes      | Unique identifier |
| `name`            | string  | Yes      | Bus name |
| `active`          | boolean | No       | Whether the bus is active |
| `voltage`         | number  | No       | Nominal voltage |
| `reference_theta` | number  | No       | Reference voltage angle |
| `use_kirchhoff`   | boolean | No       | Override global Kirchhoff setting |

### 3.2 Generator

A generation unit connected to a bus.

| Field              | Type           | Required | Description |
|--------------------|----------------|----------|-------------|
| `uid`              | integer        | Yes      | Unique identifier |
| `name`             | string         | Yes      | Generator name |
| `bus`              | string         | Yes      | Name of the connected bus |
| `active`           | boolean        | No       | Whether the generator is active |
| `pmin`             | number/string  | No       | Minimum generation |
| `pmax`             | number/string  | No       | Maximum generation |
| `gcost`            | number/string  | No       | Generation cost ($/MWh) |
| `lossfactor`       | number/string  | No       | Loss factor |
| `capacity`         | number/string  | No       | Installed capacity |
| `expcap`           | number/string  | No       | Expansion capacity per module |
| `expmod`           | number/string  | No       | Number of expansion modules |
| `capmax`           | number/string  | No       | Maximum total capacity |
| `annual_capcost`   | number/string  | No       | Annual cost per unit of capacity |
| `annual_derating`  | number/string  | No       | Annual capacity derating factor |

> **Note:** Fields that accept `number/string` can be either a numeric constant
> or a string referencing an external data file (CSV/Parquet).

### 3.3 Demand

An electrical demand (load) connected to a bus.

| Field              | Type           | Required | Description |
|--------------------|----------------|----------|-------------|
| `uid`              | integer        | Yes      | Unique identifier |
| `name`             | string         | Yes      | Demand name |
| `bus`              | string         | Yes      | Name of the connected bus |
| `active`           | boolean        | No       | Whether the demand is active |
| `lmax`             | number/string  | No       | Maximum load level |
| `lossfactor`       | number/string  | No       | Loss factor |
| `fcost`            | number/string  | No       | Curtailment cost |
| `emin`             | number/string  | No       | Minimum energy requirement |
| `ecost`            | number/string  | No       | Energy cost |
| `capacity`         | number/string  | No       | Installed capacity |
| `expcap`           | number/string  | No       | Expansion capacity per module |
| `expmod`           | number/string  | No       | Number of expansion modules |
| `capmax`           | number/string  | No       | Maximum total capacity |
| `annual_capcost`   | number/string  | No       | Annual cost per unit of capacity |
| `annual_derating`  | number/string  | No       | Annual capacity derating factor |

### 3.4 Line

A transmission line connecting two buses.

| Field              | Type           | Required | Description |
|--------------------|----------------|----------|-------------|
| `uid`              | integer        | Yes      | Unique identifier |
| `name`             | string         | Yes      | Line name |
| `bus_a`            | string         | Yes      | Name of bus A |
| `bus_b`            | string         | Yes      | Name of bus B |
| `active`           | boolean        | No       | Whether the line is active |
| `voltage`          | number/string  | No       | Line voltage |
| `resistance`       | number/string  | No       | Line resistance |
| `reactance`        | number/string  | No       | Line reactance |
| `lossfactor`       | number/string  | No       | Loss factor |
| `tmax_ab`          | number/string  | No       | Max transfer A→B |
| `tmax_ba`          | number/string  | No       | Max transfer B→A |
| `tcost`            | number/string  | No       | Transfer cost |
| `capacity`         | number/string  | No       | Installed capacity |
| `expcap`           | number/string  | No       | Expansion capacity per module |
| `expmod`           | number/string  | No       | Number of expansion modules |
| `capmax`           | number/string  | No       | Maximum total capacity |
| `annual_capcost`   | number/string  | No       | Annual cost per unit of capacity |
| `annual_derating`  | number/string  | No       | Annual capacity derating factor |

### 3.5 Battery

An energy storage device.

| Field               | Type           | Required | Description |
|---------------------|----------------|----------|-------------|
| `uid`               | integer        | Yes      | Unique identifier |
| `name`              | string         | Yes      | Battery name |
| `active`            | boolean        | No       | Whether the battery is active |
| `input_efficiency`  | number/string  | No       | Charging efficiency |
| `output_efficiency` | number/string  | No       | Discharging efficiency |
| `annual_loss`       | number/string  | No       | Annual energy loss rate |
| `vmin`              | number/string  | No       | Minimum stored energy |
| `vmax`              | number/string  | No       | Maximum stored energy |
| `vcost`             | number/string  | No       | Storage cost |
| `vini`              | number         | No       | Initial stored energy |
| `vfin`              | number         | No       | Final stored energy |
| `capacity`          | number/string  | No       | Installed capacity |
| `expcap`            | number/string  | No       | Expansion capacity per module |
| `expmod`            | number/string  | No       | Number of expansion modules |
| `capmax`            | number/string  | No       | Maximum total capacity |
| `annual_capcost`    | number/string  | No       | Annual cost per unit of capacity |
| `annual_derating`   | number/string  | No       | Annual capacity derating factor |

### 3.6 Converter

Links a battery to a generator and demand for charge/discharge.

| Field              | Type           | Required | Description |
|--------------------|----------------|----------|-------------|
| `uid`              | integer        | Yes      | Unique identifier |
| `name`             | string         | Yes      | Converter name |
| `active`           | boolean        | No       | Whether the converter is active |
| `battery`          | string         | Yes      | Name of associated battery |
| `generator`        | string         | Yes      | Name of associated generator |
| `demand`           | string         | Yes      | Name of associated demand |
| `conversion_rate`  | number/string  | No       | Conversion rate |
| `capacity`         | number/string  | No       | Installed capacity |
| `expcap`           | number/string  | No       | Expansion capacity per module |
| `expmod`           | number/string  | No       | Number of expansion modules |
| `capmax`           | number/string  | No       | Maximum total capacity |
| `annual_capcost`   | number/string  | No       | Annual cost per unit of capacity |
| `annual_derating`  | number/string  | No       | Annual capacity derating factor |

### 3.7 Junction

A hydraulic node in the water network.

| Field    | Type    | Required | Description |
|----------|---------|----------|-------------|
| `uid`    | integer | Yes      | Unique identifier |
| `name`   | string  | Yes      | Junction name |
| `active` | boolean | No       | Whether the junction is active |
| `drain`  | boolean | No       | Whether the junction drains water |

### 3.8 Waterway

A water channel connecting two junctions.

| Field        | Type           | Required | Description |
|--------------|----------------|----------|-------------|
| `uid`        | integer        | Yes      | Unique identifier |
| `name`       | string         | Yes      | Waterway name |
| `active`     | boolean        | No       | Whether the waterway is active |
| `junction_a` | string         | Yes      | Name of upstream junction |
| `junction_b` | string         | Yes      | Name of downstream junction |
| `capacity`   | number/string  | No       | Flow capacity |
| `lossfactor` | number/string  | No       | Loss factor |
| `fmin`       | number/string  | No       | Minimum flow |
| `fmax`       | number/string  | No       | Maximum flow |

### 3.9 Reservoir

A water reservoir connected to a junction.

| Field                  | Type           | Required | Description |
|------------------------|----------------|----------|-------------|
| `uid`                  | integer        | Yes      | Unique identifier |
| `name`                 | string         | Yes      | Reservoir name |
| `active`               | boolean        | No       | Whether the reservoir is active |
| `junction`             | string         | Yes      | Name of associated junction |
| `spillway_capacity`    | number         | No       | Spillway capacity |
| `spillway_cost`        | number         | No       | Spillway cost |
| `capacity`             | number/string  | No       | Storage capacity |
| `annual_loss`          | number/string  | No       | Annual loss rate |
| `vmin`                 | number/string  | No       | Minimum volume |
| `vmax`                 | number/string  | No       | Maximum volume |
| `vcost`                | number/string  | No       | Storage cost |
| `vini`                 | number         | No       | Initial volume |
| `vfin`                 | number         | No       | Final volume |
| `fmin`                 | number         | No       | Minimum flow |
| `fmax`                 | number         | No       | Maximum flow |
| `vol_scale`            | number         | No       | Volume scaling factor |
| `flow_conversion_rate` | number         | No       | Flow to volume conversion |

### 3.10 Turbine

A hydro turbine linking a waterway to a generator.

| Field             | Type           | Required | Description |
|-------------------|----------------|----------|-------------|
| `uid`             | integer        | Yes      | Unique identifier |
| `name`            | string         | Yes      | Turbine name |
| `active`          | boolean        | No       | Whether the turbine is active |
| `waterway`        | string         | Yes      | Name of associated waterway |
| `generator`       | string         | Yes      | Name of associated generator |
| `drain`           | boolean        | No       | Whether the turbine drains water |
| `conversion_rate` | number/string  | No       | Water-to-power conversion rate |
| `capacity`        | number/string  | No       | Installed capacity |

### 3.11 Flow (Inflow)

A water inflow at a junction.

| Field       | Type           | Required | Description |
|-------------|----------------|----------|-------------|
| `uid`       | integer        | Yes      | Unique identifier |
| `name`      | string         | Yes      | Flow name |
| `active`    | boolean        | No       | Whether the flow is active |
| `direction` | integer        | No       | Flow direction (1 = inflow, -1 = outflow) |
| `junction`  | string         | Yes      | Name of associated junction |
| `discharge` | number/string  | Yes      | Discharge schedule |

### 3.12 Filtration

Connects a waterway to a reservoir for water filtration.

| Field       | Type    | Required | Description |
|-------------|---------|----------|-------------|
| `uid`       | integer | Yes      | Unique identifier |
| `name`      | string  | Yes      | Filtration name |
| `active`    | boolean | No       | Whether the filtration is active |
| `waterway`  | string  | Yes      | Name of associated waterway |
| `reservoir` | string  | Yes      | Name of associated reservoir |
| `slope`     | number  | No       | Filtration slope coefficient |
| `constant`  | number  | No       | Filtration constant |

### 3.13 Generator Profile

A time-series generation profile for a generator.

| Field       | Type           | Required | Description |
|-------------|----------------|----------|-------------|
| `uid`       | integer        | Yes      | Unique identifier |
| `name`      | string         | Yes      | Profile name |
| `active`    | boolean        | No       | Whether the profile is active |
| `generator` | string         | Yes      | Name of associated generator |
| `profile`   | number/string  | Yes      | Generation profile data |
| `scost`     | number/string  | No       | Profile-specific cost |

### 3.14 Demand Profile

A time-series demand profile.

| Field    | Type           | Required | Description |
|----------|----------------|----------|-------------|
| `uid`    | integer        | Yes      | Unique identifier |
| `name`   | string         | Yes      | Profile name |
| `active` | boolean        | No       | Whether the profile is active |
| `demand` | string         | Yes      | Name of associated demand |
| `profile`| number/string  | Yes      | Demand profile data |
| `scost`  | number/string  | No       | Profile-specific cost |

### 3.15 Reserve Zone

A zone for reserve requirements.

| Field    | Type           | Required | Description |
|----------|----------------|----------|-------------|
| `uid`    | integer        | Yes      | Unique identifier |
| `name`   | string         | Yes      | Zone name |
| `active` | boolean        | No       | Whether the zone is active |
| `urreq`  | number/string  | No       | Up-reserve requirement |
| `drreq`  | number/string  | No       | Down-reserve requirement |
| `urcost` | number/string  | No       | Up-reserve cost |
| `drcost` | number/string  | No       | Down-reserve cost |

### 3.16 Reserve Provision

A reserve provision linking a generator to reserve zones.

| Field                  | Type           | Required | Description |
|------------------------|----------------|----------|-------------|
| `uid`                  | integer        | Yes      | Unique identifier |
| `name`                 | string         | Yes      | Provision name |
| `active`               | boolean        | No       | Whether the provision is active |
| `generator`            | string         | Yes      | Name of associated generator |
| `reserve_zones`        | string         | Yes      | Associated reserve zone(s) |
| `urmax`                | number/string  | No       | Maximum up-reserve |
| `drmax`                | number/string  | No       | Maximum down-reserve |
| `ur_capacity_factor`   | number/string  | No       | Up-reserve capacity factor |
| `dr_capacity_factor`   | number/string  | No       | Down-reserve capacity factor |
| `ur_provision_factor`  | number/string  | No       | Up-reserve provision factor |
| `dr_provision_factor`  | number/string  | No       | Down-reserve provision factor |
| `urcost`               | number/string  | No       | Up-reserve cost |
| `drcost`               | number/string  | No       | Down-reserve cost |

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
