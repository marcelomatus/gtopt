# gtopt Planning Guide

A step-by-step guide to building, running, and understanding gtopt optimization
cases. This guide walks through progressively more complex examples: from a
simple single-bus dispatch to a multi-bus DC power-flow case with batteries and
external time-series data.

---

## Table of Contents

1. [Concepts](#1-concepts)
   - [Time structure: Blocks, Stages, Scenarios](#11-time-structure-blocks-stages-scenarios)
   - [Phases and Scenes](#12-phases-and-scenes)
   - [System elements](#13-system-elements)
2. [Anatomy of a gtopt JSON file](#2-anatomy-of-a-gtopt-json-file)
3. [Example 1 – Single-bus dispatch (one block)](#3-example-1--single-bus-dispatch-one-block)
4. [Example 2 – Multi-bus DC power flow (IEEE 9-bus)](#4-example-2--multi-bus-dc-power-flow-ieee-9-bus)
5. [Example 3 – Multi-stage capacity expansion](#5-example-3--multi-stage-capacity-expansion)
6. [Example 4 – Battery storage (4-bus, 4 blocks)](#6-example-4--battery-storage-4-bus-4-blocks)
7. [Working with time-series schedules](#7-working-with-time-series-schedules)
   - [Inline schedules in JSON](#71-inline-schedules-in-json)
   - [External CSV files](#72-external-csv-files)
   - [External Parquet files](#73-external-parquet-files)
   - [Directory layout and file-field naming convention](#74-directory-layout-and-file-field-naming-convention)
8. [Complete JSON element reference](#8-complete-json-element-reference)
   - [Options](#81-options)
   - [Simulation (time structure)](#82-simulation-time-structure)
   - [System – Electrical network](#83-system--electrical-network)
   - [System – Profiles](#84-system--profiles)
   - [System – Energy storage](#85-system--energy-storage)
   - [System – Reserves](#86-system--reserves)
   - [System – Hydro cascade](#87-system--hydro-cascade)
9. [Field reference and auto-generated docs](#9-field-reference-and-auto-generated-docs)
10. [Output files](#10-output-files)

---

## 1. Concepts

### 1.1 Time structure: Blocks, Stages, Scenarios

The **planning data model** defines how time and uncertainty are represented.
The diagram below shows the class hierarchy:

```mermaid
---
title: gtopt Planning Data Model
---
classDiagram
    direction TB

    class Planning {
        +string name
        +Options options
    }
    class Simulation {
        +Scenario[] scenario_array
        +Stage[]    stage_array
        +Phase[]    phase_array
        +Block[]    block_array
        +Scene[]    scene_array
    }
    class Scenario {
        +Uid  uid
        +Name name
        +Real probability_factor [p.u.]
        +Bool active
    }
    class Phase {
        +Uid  uid
        +Name name
        +Size first_stage
        +Size count_stage
        +Bool active
    }
    class Stage {
        +Uid  uid
        +Name name
        +Size first_block
        +Size count_block
        +Real discount_factor [p.u.]
        +Bool active
    }
    class Block {
        +Uid  uid
        +Name name
        +Real duration [h]
    }
    class Scene {
        +Uid  uid
        +Name name
        +Size first_scenario
        +Size count_scenario
        +Bool active
    }
    class System {
        +Bus[]              bus_array
        +Generator[]        generator_array
        +Demand[]           demand_array
        +Line[]             line_array
        +Battery[]          battery_array
        +Converter[]        converter_array
        +GeneratorProfile[] generator_profile_array
        +DemandProfile[]    demand_profile_array
        +ReserveZone[]      reserve_zone_array
        +ReserveProvision[] reserve_provision_array
        +Junction[]         junction_array
        +Waterway[]         waterway_array
        +Flow[]             flow_array
        +Reservoir[]        reservoir_array
        +Filtration[]       filtration_array
        +Turbine[]          turbine_array
    }

    Planning  *--  Simulation : contains
    Planning  *--  System     : contains
    Simulation *-- Scenario   : has many
    Simulation *-- Stage      : has many
    Simulation *-- Phase      : has many
    Simulation *-- Block      : has many
    Simulation *-- Scene      : has many
    Phase      o-- Stage      : groups
    Stage      o-- Block      : references
    Scene      --> Scenario   : indexes
    Scene      ..> Phase      : paired with
```

> 💾 **Auto-generated SVG**: `docs/diagrams/planning_structure.svg`
> (regenerate with `python3 scripts/gtopt_diagram.py --diagram-type planning -o docs/diagrams/planning_structure.svg`)

| Element | Role |
|---------|------|
| **Block** | Smallest time unit. `energy [MWh] = power [MW] × duration [h]`. |
| **Stage** | Investment period. Capacity built in a stage is available in all later stages. Costs are multiplied by `discount_factor` for present-value accounting. |
| **Scenario** | One realisation of uncertain inputs (e.g. dry/wet hydrology). All scenarios are solved simultaneously; their costs are weighted by `probability_factor`. |
| **Phase** | Groups consecutive stages into a higher-level period (e.g. seasons, construction vs. operation). Default: single phase covering all stages. See [Section 1.2](#12-phases-and-scenes). |
| **Scene** | Combines a subset of scenarios for LP solving. Default: single scene covering all scenarios. See [Section 1.2](#12-phases-and-scenes). |

A single-snapshot operational study uses **one block, one stage, one scenario**
(the defaults if you omit `simulation` entirely):

```json
{
  "simulation": {
    "block_array":    [{"uid": 1, "duration": 1}],
    "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  }
}
```

A 24-hour operational study uses **24 blocks, one stage, one scenario**:

```json
{
  "simulation": {
    "block_array": [
      {"uid":  1, "duration": 1},
      {"uid":  2, "duration": 1},
      ...
      {"uid": 24, "duration": 1}
    ],
    "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 24}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  }
}
```

A 5-year investment study with annual stages uses **five 1-block stages** (or
more blocks per stage for seasonal detail):

```json
{
  "simulation": {
    "block_array": [
      {"uid": 1, "duration": 8760},
      {"uid": 2, "duration": 8760},
      {"uid": 3, "duration": 8760},
      {"uid": 4, "duration": 8760},
      {"uid": 5, "duration": 8760}
    ],
    "stage_array": [
      {"uid": 1, "first_block": 0, "count_block": 1, "discount_factor": 1.0},
      {"uid": 2, "first_block": 1, "count_block": 1, "discount_factor": 0.909},
      {"uid": 3, "first_block": 2, "count_block": 1, "discount_factor": 0.826},
      {"uid": 4, "first_block": 3, "count_block": 1, "discount_factor": 0.751},
      {"uid": 5, "first_block": 4, "count_block": 1, "discount_factor": 0.683}
    ],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  }
}
```

> **Tip**: set `annual_discount_rate` in `options` and let gtopt compute
> discount factors automatically instead of providing them explicitly.

### 1.2 Phases and Scenes

#### Phase – Grouping stages into higher-level periods

A **Phase** groups consecutive **stages** into a higher-level planning period.
Common use cases:

| Use case | Phases | Stages per phase |
|----------|--------|-----------------|
| **Seasonal analysis** | 4 phases (summer, autumn, winter, spring) | 3 monthly stages each |
| **Construction vs. operation** | 2 phases (build, operate) | Variable |
| **Single-period study** | 1 phase (default) | All stages |

When no `phase_array` is provided in the JSON, gtopt automatically creates a
single default phase that covers all stages.

**JSON example – 4 seasonal phases (12 monthly stages)**:

```json
{
  "simulation": {
    "phase_array": [
      {"uid": 1, "name": "summer",  "first_stage": 0, "count_stage": 3},
      {"uid": 2, "name": "autumn",  "first_stage": 3, "count_stage": 3},
      {"uid": 3, "name": "winter",  "first_stage": 6, "count_stage": 3},
      {"uid": 4, "name": "spring",  "first_stage": 9, "count_stage": 3}
    ],
    "stage_array": [
      {"uid": 1,  "first_block": 0,  "count_block": 3},
      {"uid": 2,  "first_block": 3,  "count_block": 3},
      {"uid": 3,  "first_block": 6,  "count_block": 3},
      {"uid": 4,  "first_block": 9,  "count_block": 3},
      {"uid": 5,  "first_block": 12, "count_block": 3},
      {"uid": 6,  "first_block": 15, "count_block": 3},
      {"uid": 7,  "first_block": 18, "count_block": 3},
      {"uid": 8,  "first_block": 21, "count_block": 3},
      {"uid": 9,  "first_block": 24, "count_block": 3},
      {"uid": 10, "first_block": 27, "count_block": 3},
      {"uid": 11, "first_block": 30, "count_block": 3},
      {"uid": 12, "first_block": 33, "count_block": 3}
    ],
    "block_array": [
      {"uid": 1,  "duration": 217, "name": "night"},
      {"uid": 2,  "duration": 372, "name": "solar"},
      {"uid": 3,  "duration": 155, "name": "evening"}
    ]
  }
}
```

**Phase fields**:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `uid` | integer | **Yes** | Unique identifier |
| `name` | string | No | Human-readable label (e.g. `"summer"`) |
| `active` | boolean | No | Activation status (default: `true`) |
| `first_stage` | integer | No | 0-based index of the first stage (default: `0`) |
| `count_stage` | integer | No | Number of stages (default: all remaining) |

#### Scene – Cross-product of scenarios and phases

A **Scene** combines a set of **scenarios** with a **phase**.  In the LP
formulation, each scene defines which scenarios are solved together within
which phase.  This is an advanced feature used for complex multi-scenario,
multi-phase studies.

For most cases the default single scene (covering all scenarios across one
phase) is sufficient.  You only need explicit `scene_array` when combining
multiple scenarios with multiple phases to control which scenario groups
apply to which phase.

**JSON example – default (implicit)**:

```json
{
  "simulation": {
    "scene_array": [{"uid": 1, "first_scenario": 0, "count_scenario": 1}]
  }
}
```

**Scene fields**:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `uid` | integer | **Yes** | Unique identifier |
| `name` | string | No | Human-readable label |
| `active` | boolean | No | Activation status (default: `true`) |
| `first_scenario` | integer | No | 0-based index of the first scenario (default: `0`) |
| `count_scenario` | integer | No | Number of scenarios (default: all remaining) |

#### Time hierarchy diagram

The complete time hierarchy in gtopt is:

```
Planning
 └─ Scene (cross-product of scenarios × phases)
     ├─ Scenario (probability-weighted future realization)
     └─ Phase (higher-level grouping)
         └─ Stage (investment period, discount factor)
             └─ Block (smallest time unit, duration in hours)
```

For a typical seasonal study with 2 scenarios (dry/wet hydrology):

```
Scene 1 ─── Scenario: "dry year" (prob=0.3)
         └─ Phase: "summer" → Stages 1-3 (Jan, Feb, Mar)
                              Each stage: 3 blocks (night, solar, evening)
Scene 2 ─── Scenario: "wet year" (prob=0.7)
         └─ Phase: "summer" → Stages 1-3 (Jan, Feb, Mar)
                              Each stage: 3 blocks (night, solar, evening)
```

### 1.3 System elements

| Category | Elements | Description |
|----------|---------|-------------|
| Electrical network | Bus, Generator, Demand, Line | Core grid model |
| Time-varying profiles | GeneratorProfile, DemandProfile | Capacity-factor / load-shape scaling |
| Energy storage | Battery, Converter | BESS modelling |
| Reserve | ReserveZone, ReserveProvision | Spinning-reserve requirements |
| Hydro cascade | Junction, Waterway, Flow, Reservoir, Filtration, Turbine | Hydrothermal systems |

---

## 2. Anatomy of a gtopt JSON file

A gtopt case is defined by **one or more JSON files** passed on the command
line. Multiple files are merged in order, so you can split options, simulation,
and system across files.

```
gtopt base_options.json simulation.json system.json
```

The top-level structure is always:

```json
{
  "options":    { ... },
  "simulation": { ... },
  "system":     { ... }
}
```

All three sections are **optional** — omitted sections use defaults.

### Options (commonly used fields)

| Field | Units | Description |
|-------|-------|-------------|
| `demand_fail_cost` | $/MWh | Penalty for unserved load (value of lost load) |
| `use_kirchhoff` | — | Enable DC power-flow constraints (`true`/`false`) |
| `use_single_bus` | — | Collapse network to copper plate (`true`/`false`) |
| `scale_objective` | — | Divide objective by this value (improves solver numerics) |
| `annual_discount_rate` | p.u./year | Compute stage discount factors automatically |
| `input_directory` | — | Root directory for external time-series files |
| `input_format` | — | `"parquet"` (default) or `"csv"` |
| `output_directory` | — | Directory for result files (default: `"output"`) |
| `output_format` | — | `"parquet"` (default) or `"csv"` |

---

## 3. Example 1 – Single-bus dispatch (one block)

A minimal case: one bus, one cheap generator, one load, one hour.

### Network diagram

```mermaid
flowchart LR
    gen_1[/"⚡ g1\n100 MW · $20/MWh"\]
    bus_1["🔌 b1"]
    dem_1[\"📊 d1\n80 MW load"/]

    gen_1 --> bus_1
    bus_1 --> dem_1
    classDef cls_bus fill:#D6EAF8,stroke:#1A5276,color:#1C2833
    classDef cls_gen fill:#FEF9E7,stroke:#E67E22,color:#1C2833
    classDef cls_demand fill:#FADBD8,stroke:#C0392B,color:#1C2833
    class bus_1 cls_bus
    class gen_1 cls_gen
    class dem_1 cls_demand
```

### JSON

```json
{
  "options": {
    "use_single_bus": true,
    "demand_fail_cost": 500,
    "scale_objective": 1000,
    "output_format": "csv"
  },
  "simulation": {
    "block_array":    [{"uid": 1, "duration": 1}],
    "stage_array":    [{"uid": 1, "first_block": 0, "count_block": 1}],
    "scenario_array": [{"uid": 1, "probability_factor": 1}]
  },
  "system": {
    "name": "example1",
    "bus_array": [
      {"uid": 1, "name": "b1"}
    ],
    "generator_array": [
      {"uid": 1, "name": "g1", "bus": "b1", "pmax": 100, "gcost": 20, "capacity": 100}
    ],
    "demand_array": [
      {"uid": 1, "name": "d1", "bus": "b1", "lmax": 80}
    ]
  }
}
```

### Expected result

- g1 dispatches 80 MW to serve d1 exactly.
- Objective = 80 MW × 1 h × $20/MWh / 1000 = **$1.60** (scaled).
- `output/solution.csv`: `status=0` (optimal).
- `output/Generator/generation_sol.csv`: `uid:1 = 80`.

---

## 4. Example 2 – Multi-bus DC power flow (IEEE 9-bus)

The classic Anderson–Fouad 9-bus benchmark. Three generators, three loads, nine
transmission lines. DC power flow (Kirchhoff's voltage law) is enabled.

### Network diagram

The IEEE 9-bus test system (Anderson & Fouad, 1994). Three generators,
three loads, nine transmission lines with DC power flow:

```mermaid
---
title: IEEE 9-bus test system
---
flowchart LR

    bus_1["🔌 b1"]
    bus_2["🔌 b2"]
    bus_3["🔌 b3"]
    bus_4["🔌 b4"]
    bus_5["🔌 b5"]
    bus_6["🔌 b6"]
    bus_7["🔌 b7"]
    bus_8["🔌 b8"]
    bus_9["🔌 b9"]
    gen_1[/"⚡ g1\n250 MW · $20/MWh"\]
    gen_2[/"⚡ g2\n300 MW · $35/MWh"\]
    gen_3[/"☀️ g3\n270 MW · $0/MWh"\]
    dem_1[\"📊 d1\n125 MW"/]
    dem_2[\"📊 d2\n100 MW"/]
    dem_3[\"📊 d3\n90 MW"/]

    gen_1 --> bus_1
    gen_2 --> bus_2
    gen_3 --> bus_3
    bus_5 --> dem_1
    bus_7 --> dem_2
    bus_9 --> dem_3
    bus_1 ---|l1_4 x=0.058| bus_4
    bus_2 ---|l2_7 x=0.063| bus_7
    bus_3 ---|l3_9 x=0.059| bus_9
    bus_4 ---|l4_5 x=0.085| bus_5
    bus_4 ---|l4_6 x=0.092| bus_6
    bus_5 ---|l5_7 x=0.161| bus_7
    bus_6 ---|l6_9 x=0.170| bus_9
    bus_7 ---|l7_8 x=0.072| bus_8
    bus_8 ---|l8_9 x=0.101| bus_9

    classDef cls_bus fill:#D6EAF8,stroke:#1A5276,color:#1C2833
    class bus_1,bus_2,bus_3,bus_4,bus_5,bus_6,bus_7,bus_8,bus_9 cls_bus
    classDef cls_gen fill:#FEF9E7,stroke:#E67E22,color:#1C2833
    class gen_1,gen_2 cls_gen
    classDef cls_solar fill:#FDEBD0,stroke:#F39C12,color:#1C2833
    class gen_3 cls_solar
    classDef cls_demand fill:#FADBD8,stroke:#C0392B,color:#1C2833
    class dem_1,dem_2,dem_3 cls_demand
```

> 🛠 Regenerate the full SVG: `python3 scripts/gtopt_diagram.py cases/ieee_9b/ieee_9b.json --subsystem electrical -o docs/diagrams/ieee9b_electrical.svg`
> 
> See also: `docs/diagrams/ieee9b_electrical.svg`

### Run the bundled case

```bash
cd cases/ieee_9b_ori
gtopt ieee_9b_ori.json
cat output/solution.csv          # status=0, obj_value=5.0
cat output/Generator/generation_sol.csv
```

Expected: g1 dispatches ~250 MW (cheapest at $20/MWh), g3 serves the rest,
g2 (most expensive at $35/MWh) is at or near minimum.

### Key JSON excerpt

```json
{
  "options": {
    "use_single_bus": false,
    "use_kirchhoff": true,
    "demand_fail_cost": 1000,
    "scale_objective": 1000
  },
  "system": {
    "line_array": [
      {
        "uid": 1, "name": "l1_4",
        "bus_a": "b1", "bus_b": "b4",
        "reactance": 0.0576,
        "tmax_ab": 250, "tmax_ba": 250
      }
    ]
  }
}
```

> **Note on reactance units**: line reactance values in the bundled IEEE cases
> are in **per-unit (p.u.)** on a common system base (typically 100 MVA).
> When `use_kirchhoff = true`, gtopt uses the p.u. reactance to compute the
> voltage-angle difference: `flow [MW] = (θ_a − θ_b) / reactance [p.u.]`.

---

## 5. Example 3 – Multi-stage capacity expansion

The `cases/c0/` case demonstrates **demand-side capacity expansion** over five
years. The demand `d1` starts at zero installed capacity and the solver decides
how many 20 MW modules to build each year.

### One-line diagram

```
g1 (20 MW, $100/MWh)
     │
b1 ──┤── d1 (expandable: 0→200 MW, $8 760/MW-year)
```

### Time structure

Five stages × one block each, annual durations (1/2/3/4/5 h in the simplified
case; full annual = 8 760 h in production cases).

```json
"stage_array": [
  {"uid": 1, "first_block": 0, "count_block": 1},
  {"uid": 2, "first_block": 1, "count_block": 1},
  {"uid": 3, "first_block": 2, "count_block": 1},
  {"uid": 4, "first_block": 3, "count_block": 1},
  {"uid": 5, "first_block": 4, "count_block": 1}
]
```

### Expandable demand definition

```json
{
  "uid": 1, "name": "d1", "bus": "b1",
  "lmax": "lmax",
  "capacity": 0,
  "expcap": 20,
  "expmod": 10,
  "annual_capcost": 8760
}
```

- `capacity = 0`: no initial capacity
- `expcap = 20 MW`: each module adds 20 MW
- `expmod = 10`: solver may build at most 10 modules
- `annual_capcost = 8760 $/MW-year`: annualised investment cost
- `lmax = "lmax"`: refers to `system_c0/Demand/lmax.parquet`

### Run

```bash
cd cases/c0
gtopt system_c0.json
cat output/Demand/capacost_sol.csv   # expansion cost per stage
```

---

## 6. Example 4 – Battery storage (4-bus, 4 blocks)

The `cases/bat_4b/` case adds a battery energy storage system (BESS) to a
4-bus network. The battery charges at low-cost periods and discharges during
high-demand periods.

### One-line diagram

```
g1 ($20/MWh, 250 MW) ── b1 ──l1_2── b2 ── g2 ($40/MWh, 150 MW)
                         │                  │
                        l1_3               l2_3
                         │                  │
  g_solar (0 $/MWh) ──  b3 ──────l2_4───── b4 (no generator)
  BESS (bat1, 60 MW)    d3 (120→200 MW varying load)
```

**Battery** (`bat1`) uses the **unified definition**: the `bus` field
connects it to b3 and `pmax_charge`/`pmax_discharge` set the charge/discharge
power rating.  The system auto-generates the discharge generator, charge
demand, and converter at LP construction time.

### Battery definition (unified)

```json
"battery_array": [
  {
    "uid": 1, "name": "bat1",
    "bus": "b3",
    "input_efficiency":  0.95,
    "output_efficiency": 0.95,
    "emin": 0, "emax": 200,
    "eini": 0,
    "pmax_charge": 60,
    "pmax_discharge": 60,
    "gcost": 0,
    "capacity": 200
  }
]
```

> **Note:** No `converter_array`, `g_bat_out` generator, or `d_bat_in`
> demand is needed — all three are auto-generated by `expand_batteries()`.

The BESS charges when solar is cheap (block 3) and discharges during the
high-demand block 4 (200 MW load).

### Run

```bash
cd cases/bat_4b
gtopt bat_4b.json
cat output/Generator/generation_sol.csv
cat output/Battery/storage_sol.csv
```

---

## 7. Working with time-series schedules

Many fields — `pmax`, `lmax`, `gcost`, `profile`, `discharge` — can hold:

| Value type | Meaning |
|------------|---------|
| `100` (scalar) | Constant value in every block |
| `[[80, 90, 100]]` (inline array) | Per-`[stage][block]` values |
| `[[[70, 80, 90], [60, 70, 80]]]` | Per-`[scenario][stage][block]` values |
| `"lmax"` (string) | Filename in `input_directory/<ClassName>/` |

### 7.1 Inline schedules in JSON

The array dimensions depend on the field type:

| C++ type | Dimensions | Example |
|----------|-----------|---------|
| `OptTRealFieldSched` | `[stage]` or scalar | `[100, 90, 80, 70, 60]` |
| `OptTBRealFieldSched` | `[stage][block]` or scalar | `[[100, 95], [90, 85]]` |
| `STBRealFieldSched` | `[scenario][stage][block]` | `[[[1.0, 0.8, 0.5]]]` |

**Example**: 24-hour solar generation profile for a generator with 270 MW capacity:

```json
{
  "uid": 1, "name": "gp_solar",
  "generator": "g_solar",
  "profile": [[[0, 0, 0, 0, 0, 0.05,
                0.2, 0.45, 0.7, 0.88, 0.97, 1.0,
                0.98, 0.95, 0.88, 0.72, 0.5, 0.25,
                0.08, 0.01, 0, 0, 0, 0]]]
}
```

> Inline arrays are fine for tens of blocks. For hundreds or thousands of
> time steps, use external files.

### 7.2 External CSV files

A CSV schedule file uses these columns:

| Column | Description |
|--------|-------------|
| `scenario` | Scenario UID |
| `stage` | Stage UID |
| `block` | Block UID |
| `uid:<N>` | Value for element with UID N |

Multiple elements can appear as additional `uid:<N>` columns in the same file.

**Example**: `input/Demand/lmax.csv` with per-block demand for `d1` (uid=1)
and `d2` (uid=2):

```csv
"scenario","stage","block","uid:1","uid:2"
1,1,1,125.0,100.0
1,1,2,130.0,105.0
1,1,3,120.0,95.0
```

Rows with missing (scenario, stage, block) combinations inherit the last value
or zero depending on context.

**Using a CSV schedule from JSON**:

```json
{
  "uid": 1, "name": "d1", "bus": "b5",
  "lmax": "lmax"
}
```

When `lmax = "lmax"`, gtopt reads
`<input_directory>/Demand/lmax.csv` (or `lmax.parquet`) and uses the
`uid:1` column for this demand.

**Convert existing CSV files to Parquet**:

```bash
# Using the bundled utility
cvs2parquet input/Demand/lmax.csv input/Demand/lmax.parquet
```

### 7.3 External Parquet files

Parquet is the preferred format (faster reading, smaller files, typed columns).
The schema is identical to CSV: columns `scenario`, `stage`, `block`, and
`uid:<N>` for each element.

**Create a Parquet file with Python**:

```python
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

# 24-hour demand profile for uid=1 and uid=2
records = [
    {"scenario": 1, "stage": 1, "block": b,
     "uid:1": 125.0 + 20 * (1 if 8 <= b <= 20 else 0),
     "uid:2":  80.0 + 15 * (1 if 7 <= b <= 21 else 0)}
    for b in range(1, 25)
]
df = pd.DataFrame(records)

# Cast index columns to int32, values to float64
for col in ("scenario", "stage", "block"):
    df[col] = df[col].astype("int32")

table = pa.Table.from_pandas(df)
pq.write_table(table, "input/Demand/lmax.parquet")
```

**Read a Parquet file with Python** (for validation):

```python
import pyarrow.parquet as pq
table = pq.read_table("input/Demand/lmax.parquet")
print(table.to_pandas().head())
```

**Use `cvs2parquet` for existing CSV files**:

```bash
# Single file
cvs2parquet input/Demand/lmax.csv input/Demand/lmax.parquet

# Batch conversion with optional schema enforcement
cvs2parquet --schema input/Generator/pmax.csv input/Generator/pmax.parquet
```

### 7.4 Directory layout and file-field naming convention

When a JSON field value is a **string** it is treated as a filename (without
extension).  The file is looked up in:

```
<input_directory>/<ClassName>/<field_name>.<format>
```

where:
- `<ClassName>` is the element's class name (e.g. `Generator`, `Demand`,
  `Battery`, `GeneratorProfile`)
- `<field_name>` is the string value from JSON (e.g. `"lmax"`, `"pmax"`,
  `"profile"`)
- `<format>` is `parquet` or `csv` depending on `input_format` option

**Full directory example**:

```
my_case/
├── my_case.json              # Main planning file
└── input/                    # input_directory = "input"
    ├── Demand/
    │   └── lmax.parquet      # lmax schedule for all demands
    ├── Generator/
    │   ├── pmax.parquet      # pmax schedule for all generators
    │   └── gcost.parquet     # time-varying generation cost
    ├── GeneratorProfile/
    │   └── profile.parquet   # capacity-factor profiles
    ├── Battery/
    │   ├── emin.parquet      # minimum SoC schedule
    │   └── emax.parquet      # maximum SoC schedule
    └── Reservoir/
        └── emax.parquet      # seasonal reservoir limits
```

**JSON linking a demand to an external file**:

```json
{
  "uid": 1, "name": "d1", "bus": "b5",
  "lmax": "lmax"
}
```

This tells gtopt: read `input/Demand/lmax.parquet`, column `uid:1`.

**JSON linking a generator to multiple external fields**:

```json
{
  "uid": 2, "name": "g_wind", "bus": "b3",
  "capacity": 500,
  "pmax": "pmax",
  "gcost": "gcost"
}
```

- `pmax` → column `uid:2` in `input/Generator/pmax.parquet`
- `gcost` → column `uid:2` in `input/Generator/gcost.parquet`

**Profile files** use the `GeneratorProfile` class name:

```json
{
  "uid": 1, "name": "wind_profile",
  "generator": "g_wind",
  "profile": "profile"
}
```

→ `input/GeneratorProfile/profile.parquet`, column `uid:1`.

> **Tip**: When `input_format = "parquet"`, gtopt first looks for the
> `.parquet` file.  If it is absent it falls back to the `.csv` file.

---

## 8. Complete JSON element reference

This section documents **every element and class** in the gtopt JSON input file.
Values can be specified as:

| JSON representation | C++ type pattern | Description |
|---------------------|-----------------|-------------|
| `100` (number) | scalar | Constant in every block/stage |
| `[80, 90]` | `[stage]` | Per-stage values |
| `[[80, 90], [70, 85]]` | `[stage][block]` | Per-stage, per-block |
| `[[[1.0, 0.8]]]` | `[scenario][stage][block]` | Full 3-D indexing |
| `"filename"` (string) | `FileSched` | External Parquet/CSV file in `input_directory/<Class>/` |

### 8.1 Options

Global solver and model configuration.  All fields are optional.

| Field | JSON type | Units | Default | Description |
|-------|-----------|-------|---------|-------------|
| `demand_fail_cost` | number | $/MWh | — | Penalty for unserved load (value of lost load) |
| `reserve_fail_cost` | number | $/MWh | — | Penalty for unserved spinning reserve |
| `use_kirchhoff` | boolean | — | `true` | Enable DC power-flow (Kirchhoff's voltage law) constraints |
| `use_single_bus` | boolean | — | `false` | Collapse all buses to a single copper-plate node |
| `use_line_losses` | boolean | — | `true` | Model resistive transmission losses |
| `kirchhoff_threshold` | number | kV | — | Minimum voltage level for Kirchhoff constraints |
| `scale_objective` | number | — | `1000` | Divide all objective coefficients by this value (improves solver numerics) |
| `scale_theta` | number | — | `1000` | Scale voltage angle variables |
| `annual_discount_rate` | number | p.u./year | — | Yearly rate for automatic stage discount factor computation |
| `input_directory` | string | — | `"input"` | Root directory for external schedule files |
| `input_format` | string | — | `"parquet"` | Preferred input format (`"parquet"` or `"csv"`); falls back to the other |
| `output_directory` | string | — | `"output"` | Root directory for result files |
| `output_format` | string | — | `"parquet"` | Output file format (`"parquet"` or `"csv"`) |
| `output_compression` | string | — | `"gzip"` | Parquet compression codec (`"gzip"`, `"zstd"`, `"lzo"`, `"uncompressed"`) |
| `use_lp_names` | boolean | — | — | Use descriptive names for LP variables/constraints (debugging) |
| `use_uid_fname` | boolean | — | — | Use UIDs in output filenames |
| `lp_algorithm` | integer | — | `0` | LP algorithm: 0=auto, 1=primal simplex, 2=dual simplex, 3=barrier |
| `lp_threads` | integer | — | `0` | Number of solver threads (0 = automatic) |
| `lp_presolve` | boolean | — | `true` | Enable LP presolve |

### 8.2 Simulation (time structure)

#### Block

The smallest time unit.  `energy [MWh] = power [MW] × duration [h]`.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | No | Optional label (e.g. `"night"`, `"peak"`) |
| `duration` | number | h | **Yes** | Time duration of this block in hours |

#### Stage

Investment period grouping consecutive blocks.  Capacity decisions made in a
stage persist into all subsequent stages.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | No | Optional label (e.g. `"January"`, `"Year 1"`) |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `first_block` | integer | — | No | 0-based index of the first block (default: `0`) |
| `count_block` | integer | — | No | Number of consecutive blocks (default: all remaining) |
| `discount_factor` | number | p.u. | No | Present-value cost multiplier (default: `1.0`; auto-computed if `annual_discount_rate` is set in options) |

#### Scenario

One realisation of uncertain inputs (e.g. dry/wet hydrology, high/low demand).
All scenarios are solved simultaneously; costs are weighted by `probability_factor`.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | No | Optional label (e.g. `"dry year"`) |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `probability_factor` | number | p.u. | No | Probability weight (default: `1.0`; normalised internally) |

#### Phase

Groups consecutive stages into a higher-level period (see [Section 1.2](#12-phases-and-scenes)).

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | No | Optional label (e.g. `"summer"`, `"construction"`) |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `first_stage` | integer | — | No | 0-based index of the first stage (default: `0`) |
| `count_stage` | integer | — | No | Number of stages (default: all remaining) |

#### Scene

Cross-products scenarios with phases (see [Section 1.2](#12-phases-and-scenes)).

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | No | Optional label |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `first_scenario` | integer | — | No | 0-based index of the first scenario (default: `0`) |
| `count_scenario` | integer | — | No | Number of scenarios (default: all remaining) |

### 8.3 System – Electrical network

#### Bus

Electrical node in the network.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Bus name (used for cross-references) |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `voltage` | number | kV | No | Nominal voltage level |
| `reference_theta` | number | rad | No | Fixed voltage angle (reference bus) |
| `use_kirchhoff` | boolean | — | No | Override the global `use_kirchhoff` setting for this bus |

#### Generator

Thermal, renewable, or hydro generation unit.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Generator name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `bus` | integer\|string | — | **Yes** | Connected bus (UID or name) |
| `pmin` | number\|array\|string | MW | No | Minimum power output `[stage][block]` |
| `pmax` | number\|array\|string | MW | No | Maximum power output `[stage][block]` |
| `gcost` | number\|array\|string | $/MWh | No | Variable generation cost `[stage]` |
| `lossfactor` | number\|array\|string | p.u. | No | Network loss factor `[stage]` |
| `capacity` | number\|array\|string | MW | No | Installed generation capacity `[stage]` |
| `expcap` | number\|array\|string | MW | No | Capacity per expansion module `[stage]` |
| `expmod` | number\|array\|string | — | No | Maximum expansion modules `[stage]` |
| `capmax` | number\|array\|string | MW | No | Absolute maximum capacity `[stage]` |
| `annual_capcost` | number\|array\|string | $/MW-year | No | Annualised investment cost `[stage]` |
| `annual_derating` | number\|array\|string | p.u./year | No | Annual capacity derating `[stage]` |

#### Demand

Electrical load (fixed or flexible).

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Demand name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `bus` | integer\|string | — | **Yes** | Connected bus (UID or name) |
| `lmax` | number\|array\|string | MW | No | Maximum served load `[stage][block]` |
| `lossfactor` | number\|array\|string | p.u. | No | Network loss factor `[stage]` |
| `fcost` | number\|array\|string | $/MWh | No | Curtailment cost override `[stage]` |
| `emin` | number\|array\|string | MWh | No | Minimum energy served per stage `[stage]` |
| `ecost` | number\|array\|string | $/MWh | No | Energy shortage cost `[stage]` |
| `capacity` | number\|array\|string | MW | No | Installed demand capacity `[stage]` |
| `expcap` | number\|array\|string | MW | No | Expansion unit `[stage]` |
| `expmod` | number\|array\|string | — | No | Maximum expansion modules `[stage]` |
| `capmax` | number\|array\|string | MW | No | Absolute maximum capacity `[stage]` |
| `annual_capcost` | number\|array\|string | $/MW-year | No | Annualised investment cost `[stage]` |
| `annual_derating` | number\|array\|string | p.u./year | No | Annual capacity derating `[stage]` |

#### Line

Transmission branch connecting two buses.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Line name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `bus_a` | integer\|string | — | **Yes** | "From" bus (UID or name) |
| `bus_b` | integer\|string | — | **Yes** | "To" bus (UID or name) |
| `voltage` | number\|array\|string | kV | No | Nominal voltage `[stage]` |
| `resistance` | number\|array\|string | p.u. | No | Series resistance (for loss modelling) `[stage]` |
| `reactance` | number\|array\|string | p.u. | No | Series reactance (for DC power flow) `[stage]` |
| `lossfactor` | number\|array\|string | p.u. | No | Loss factor `[stage]` |
| `tmax_ab` | number\|array\|string | MW | No | Maximum flow A→B `[stage][block]` |
| `tmax_ba` | number\|array\|string | MW | No | Maximum flow B→A `[stage][block]` |
| `tcost` | number\|array\|string | $/MWh | No | Variable transfer cost `[stage]` |
| `capacity` | number\|array\|string | MW | No | Installed line capacity `[stage]` |
| `expcap` | number\|array\|string | MW | No | Expansion unit `[stage]` |
| `expmod` | number\|array\|string | — | No | Maximum expansion modules `[stage]` |
| `capmax` | number\|array\|string | MW | No | Absolute maximum capacity `[stage]` |
| `annual_capcost` | number\|array\|string | $/MW-year | No | Annualised investment cost `[stage]` |
| `annual_derating` | number\|array\|string | p.u./year | No | Annual capacity derating `[stage]` |

### 8.4 System – Profiles

#### GeneratorProfile

Time-varying capacity factor for a generator (e.g. solar irradiance curve,
wind profile).  Multiplies the generator's `capacity` to yield available power.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Profile name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `generator` | integer\|string | — | **Yes** | Associated generator (UID or name) |
| `profile` | number\|array\|string | p.u. | **Yes** | Capacity factor `[scenario][stage][block]` (0.0–1.0) |
| `scost` | number\|array\|string | $/MWh | No | Override generation cost `[stage]` |

#### DemandProfile

Time-varying load scaling factor for a demand.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Profile name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `demand` | integer\|string | — | **Yes** | Associated demand (UID or name) |
| `profile` | number\|array\|string | p.u. | **Yes** | Load scaling factor `[scenario][stage][block]` (0.0–1.0) |
| `scost` | number\|array\|string | $/MWh | No | Override curtailment cost `[stage]` |

### 8.5 System – Energy storage

#### Battery

Energy storage system with charge/discharge efficiencies and SoC bounds.

**Unified definition (recommended):** When the optional `bus` field is
set, the system auto-generates a discharge Generator (`name_gen`), a charge
Demand (`name_dem`), and a linking Converter (`name_conv`). Only a single
Battery element is needed — no separate Converter, Generator, or Demand
definitions are required. This follows conventions used by
[PyPSA StorageUnit](https://pypsa.readthedocs.io/en/latest/components.html#storage-unit)
and [pandapower storage](https://pandapower.readthedocs.io/en/latest/elements/storage.html).

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

**Traditional definition:** Without the `bus` field, a separate Converter,
Generator, and Demand must be defined manually (see Converter below).

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Battery name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `bus` | integer\|string | — | No | Bus connection (enables unified definition) |
| `input_efficiency` | number\|array\|string | p.u. | No | Charging efficiency `[stage]` (0.0–1.0) |
| `output_efficiency` | number\|array\|string | p.u. | No | Discharging efficiency `[stage]` (0.0–1.0) |
| `annual_loss` | number\|array\|string | p.u./year | No | Self-discharge rate `[stage]` |
| `emin` | number\|array\|string | MWh | No | Minimum state of charge `[stage]` |
| `emax` | number\|array\|string | MWh | No | Maximum state of charge `[stage]` |
| `vcost` | number\|array\|string | $/MWh | No | Storage usage cost `[stage]` |
| `eini` | number | MWh | No | Initial state of charge |
| `efin` | number | MWh | No | Terminal state of charge |
| `pmax_charge` | number\|array\|string | MW | No | Max charging power `[stage]` (unified) |
| `pmax_discharge` | number\|array\|string | MW | No | Max discharging power `[stage]` (unified) |
| `gcost` | number\|array\|string | $/MWh | No | Discharge generation cost `[stage]` (unified) |
| `capacity` | number\|array\|string | MWh | No | Energy storage capacity `[stage]` |
| `expcap` | number\|array\|string | MWh | No | Expansion unit `[stage]` |
| `expmod` | number\|array\|string | — | No | Maximum expansion modules `[stage]` |
| `capmax` | number\|array\|string | MWh | No | Absolute maximum capacity `[stage]` |
| `annual_capcost` | number\|array\|string | $/MWh-year | No | Annualised investment cost `[stage]` |
| `annual_derating` | number\|array\|string | p.u./year | No | Annual capacity derating `[stage]` |

#### Converter

Couples a Battery to an electrical Generator (discharge path) and Demand
(charge path).

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Converter name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `battery` | integer\|string | — | **Yes** | Battery (UID or name) |
| `generator` | integer\|string | — | **Yes** | Discharge generator (UID or name) |
| `demand` | integer\|string | — | **Yes** | Charge demand (UID or name) |
| `conversion_rate` | number\|array\|string | MW/(MWh/h) | No | Power-to-energy rate `[stage]` |
| `capacity` | number\|array\|string | MW | No | Power capacity `[stage]` |
| `expcap` | number\|array\|string | MW | No | Expansion unit `[stage]` |
| `expmod` | number\|array\|string | — | No | Maximum expansion modules `[stage]` |
| `capmax` | number\|array\|string | MW | No | Absolute maximum capacity `[stage]` |
| `annual_capcost` | number\|array\|string | $/MW-year | No | Annualised investment cost `[stage]` |
| `annual_derating` | number\|array\|string | p.u./year | No | Annual capacity derating `[stage]` |

### 8.6 System – Reserves

#### ReserveZone

Spinning-reserve requirement for a group of generators.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Zone name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `urreq` | number\|array\|string | MW | No | Up-reserve requirement `[stage][block]` |
| `drreq` | number\|array\|string | MW | No | Down-reserve requirement `[stage][block]` |
| `urcost` | number\|array\|string | $/MW | No | Up-reserve shortage cost `[stage]` |
| `drcost` | number\|array\|string | $/MW | No | Down-reserve shortage cost `[stage]` |

#### ReserveProvision

Links a generator to one or more reserve zones.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Provision name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `generator` | integer\|string | — | **Yes** | Provider generator (UID or name) |
| `reserve_zones` | string | — | **Yes** | Comma-separated zone UIDs or names |
| `urmax` | number\|array\|string | MW | No | Maximum up-reserve contribution `[stage][block]` |
| `drmax` | number\|array\|string | MW | No | Maximum down-reserve contribution `[stage][block]` |
| `ur_capacity_factor` | number\|array\|string | p.u. | No | Up-reserve capacity factor `[stage]` |
| `dr_capacity_factor` | number\|array\|string | p.u. | No | Down-reserve capacity factor `[stage]` |
| `ur_provision_factor` | number\|array\|string | p.u. | No | Up-reserve provision factor `[stage]` |
| `dr_provision_factor` | number\|array\|string | p.u. | No | Down-reserve provision factor `[stage]` |
| `urcost` | number\|array\|string | $/MW | No | Up-reserve bid cost `[stage]` |
| `drcost` | number\|array\|string | $/MW | No | Down-reserve bid cost `[stage]` |

### 8.7 System – Hydro cascade

#### Junction

Hydraulic node where waterways and reservoirs meet.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Junction name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `drain` | boolean | — | No | Allow excess water to leave system freely |

#### Waterway

Water channel connecting two junctions.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Waterway name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `junction_a` | integer\|string | — | **Yes** | Upstream junction (UID or name) |
| `junction_b` | integer\|string | — | **Yes** | Downstream junction (UID or name) |
| `capacity` | number\|array\|string | m³/s | No | Maximum flow capacity `[stage]` |
| `lossfactor` | number\|array\|string | p.u. | No | Transit water loss coefficient `[stage]` (default: `0.0`) |
| `fmin` | number\|array\|string | m³/s | No | Minimum flow `[stage][block]` (default: `0.0`) |
| `fmax` | number\|array\|string | m³/s | No | Maximum flow `[stage][block]` (default: `300000.0`) |

#### Flow

Exogenous inflow or outflow at a junction (e.g. river inflow, evaporation).

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Flow name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `direction` | integer | — | No | `+1` = inflow, `-1` = outflow (default: `+1`) |
| `junction` | integer\|string | — | **Yes** | Connected junction (UID or name) |
| `discharge` | number\|array\|string | m³/s | **Yes** | Discharge schedule `[scenario][stage][block]` |

#### Reservoir

Water storage (lake, dam).  Volume balance is maintained across blocks.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Reservoir name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `junction` | integer\|string | — | **Yes** | Connected junction (UID or name) |
| `spillway_capacity` | number | m³/s | No | Uncontrolled spill capacity (default: `6000.0`) |
| `spillway_cost` | number | $/dam³ | No | Cost of spilling water |
| `capacity` | number\|array\|string | dam³ | No | Usable storage capacity `[stage]` |
| `annual_loss` | number\|array\|string | p.u./year | No | Evaporation/seepage loss rate `[stage]` |
| `emin` | number\|array\|string | dam³ | No | Minimum volume `[stage]` |
| `emax` | number\|array\|string | dam³ | No | Maximum volume `[stage]` |
| `vcost` | number\|array\|string | $/dam³ | No | Water value cost `[stage]` |
| `eini` | number | dam³ | No | Initial volume |
| `efin` | number | dam³ | No | Terminal volume |
| `fmin` | number | m³/s | No | Minimum net inflow (default: `-10000.0`) |
| `fmax` | number | m³/s | No | Maximum net inflow (default: `+10000.0`) |
| `vol_scale` | number | — | No | Volume unit scaling factor (default: `1.0`) |
| `flow_conversion_rate` | number | dam³/(m³/s·h) | No | Flow-to-volume conversion (default: `0.0036`) |

#### Filtration

Water seepage from a waterway into a reservoir (linear model:
`seepage = slope × flow + constant`).

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Filtration name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `waterway` | integer\|string | — | **Yes** | Source waterway (UID or name) |
| `reservoir` | integer\|string | — | **Yes** | Destination reservoir (UID or name) |
| `slope` | number | — | No | Seepage rate coefficient (default: `0.0`) |
| `constant` | number | m³/s | No | Constant seepage rate (default: `0.0`) |

#### Turbine

Hydro turbine: converts water flow into electrical power via a generator.

| Field | JSON type | Units | Required | Description |
|-------|-----------|-------|----------|-------------|
| `uid` | integer | — | **Yes** | Unique identifier |
| `name` | string | — | **Yes** | Turbine name |
| `active` | boolean | — | No | Activation status (default: `true`) |
| `waterway` | integer\|string | — | **Yes** | Source waterway (UID or name) |
| `generator` | integer\|string | — | **Yes** | Discharge generator (UID or name) |
| `drain` | boolean | — | No | Allow spill without power generation |
| `conversion_rate` | number\|array\|string | MW·s/m³ | No | Water-to-power conversion rate `[stage]` |
| `capacity` | number\|array\|string | MW | No | Maximum turbine output `[stage]` |

---

## 9. Field reference and auto-generated docs

The `scripts/gtopt_field_extractor.py` utility parses the C++ header files and
generates documentation tables directly from the source code. This ensures the
documentation stays in sync with the implementation.

### Generate a Markdown field reference

```bash
# All elements (printed to stdout)
python3 scripts/gtopt_field_extractor.py

# Specific elements
python3 scripts/gtopt_field_extractor.py --elements Generator Demand Line Battery

# Write to a file
python3 scripts/gtopt_field_extractor.py --output docs/field_reference.md
```

### Generate an HTML field reference

```bash
python3 scripts/gtopt_field_extractor.py --format html --output docs/field_reference.html
```

The generated HTML includes:
- A hyperlinked table of contents
- Per-element tables with Field / C++ Type / JSON Type / Units / Required / Description
- Internal anchor links (e.g. `#generator`, `#battery`)

### Example output (Generator)

| Field | C++ Type | JSON Type | Units | Required | Description |
|-------|----------|-----------|-------|----------|-------------|
| `uid` | `Uid` | integer | — | **Yes** | Unique identifier |
| `name` | `Name` | string | — | **Yes** | Generator name |
| `bus` | `SingleId` | integer\|string | — | **Yes** | Bus ID where the generator is connected |
| `pmin` | `OptTBRealFieldSched` | number\|array\|string | `MW` | No | Minimum active power output |
| `pmax` | `OptTBRealFieldSched` | number\|array\|string | `MW` | No | Maximum active power output |
| `gcost` | `OptTRealFieldSched` | number\|array\|string | `$/MWh` | No | Variable generation cost |
| `capacity` | `OptTRealFieldSched` | number\|array\|string | `MW` | No | Installed generation capacity |
| `expcap` | `OptTRealFieldSched` | number\|array\|string | `MW` | No | Capacity added per expansion module |
| `expmod` | `OptTRealFieldSched` | number\|array\|string | — | No | Maximum number of expansion modules |
| `annual_capcost` | `OptTRealFieldSched` | number\|array\|string | `$/MW-year` | No | Annualized investment cost |

---

## 10. Output files

After a successful run, gtopt writes result files in `output_directory`
(default: `output/`) using the same tabular format as input files.

```
output/
├── solution.csv                    # Objective, status, iterations
├── Bus/
│   ├── balance_dual.csv            # LMP (Locational Marginal Price) [$/MWh]
│   └── theta_sol.csv               # Voltage angle θ [rad]
├── Generator/
│   ├── generation_sol.csv          # Dispatch [MW]
│   └── generation_cost.csv         # Dispatch cost contribution [$/h]
├── Demand/
│   ├── load_sol.csv                # Served load [MW]
│   ├── fail_sol.csv                # Unserved demand [MW]
│   ├── fail_cost.csv               # Curtailment cost [$/h]
│   └── capacity_dual.csv           # Shadow price of capacity constraint [$/MW]
├── Line/
│   ├── flowp_sol.csv               # Active power flow [MW]
│   └── theta_dual.csv              # Dual of Kirchhoff constraint
├── Battery/
│   └── storage_sol.csv             # State of charge [MWh]
└── Reservoir/
    └── volume_sol.csv              # Reservoir volume [dam³]
```

### solution.csv

| Column | Description |
|--------|-------------|
| `obj_value` | Scaled total cost (divide by `scale_objective` for $/h) |
| `kappa` | Solver iterations |
| `status` | 0 = optimal; 1 = infeasible; 2 = unbounded; 5 = not solved |

### Interpreting results

```bash
# Check status
grep status output/solution.csv

# Locational Marginal Prices
cat output/Bus/balance_dual.csv

# Verify no load shedding
grep -v "^scenario" output/Demand/fail_sol.csv | awk -F, '{print $NF}' | sort -n | tail -5
```

---

## Quick-start checklist

1. ☐ Define `options` (at minimum: `demand_fail_cost`, `output_format`)
2. ☐ Define `simulation` (blocks, stages, scenarios)
3. ☐ Define `system` (buses, generators, demands)
4. ☐ Set `input_directory` and create input files if using external schedules
5. ☐ Run: `gtopt my_case.json`
6. ☐ Check `output/solution.csv` for `status=0`
7. ☐ Inspect `output/Generator/generation_sol.csv` and
   `output/Demand/fail_sol.csv`

---

## See also

- **[Mathematical Formulation](docs/formulation/MATHEMATICAL_FORMULATION.md)**
  — Full LP/MIP optimization formulation with LaTeX notation, JSON-to-symbol
  mapping, and academic references
- **[INPUT_DATA.md](INPUT_DATA.md)** — Complete field reference for all JSON
  elements
- **[USAGE.md](USAGE.md)** — Command-line options and advanced usage
- **[SCRIPTS.md](SCRIPTS.md)** — `plp2gtopt`, `igtopt`, `cvs2parquet`
  conversion utilities
- **[BUILDING.md](BUILDING.md)** — Build and installation instructions
- **[DIAGRAM_TOOL.md](DIAGRAM_TOOL.md)** — `gtopt-diagram` network and
  planning diagram tool: aggregation, voltage reduction, large-case workflows
- `scripts/gtopt_field_extractor.py` — Auto-generate field-reference tables
  from C++ headers
