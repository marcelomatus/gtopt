# Understanding gtopt: A Global Overview

> **New to gtopt?** Start here. This page explains what gtopt is, how the
> codebase is structured, the key technologies used, and how the different
> pieces fit together — all in one place.

## Table of Contents

1. [What gtopt Does](#what-gtopt-does)
2. [Repository Layout](#repository-layout)
3. [Core Library Architecture](#core-library-architecture)
   - [Domain Model](#1-domain-model)
   - [LP Assembly](#2-lp-assembly)
   - [Solver Interface and Plugins](#3-solver-interface-and-plugins)
   - [Solution Methods](#4-solution-methods)
   - [I/O Layer](#5-io-layer)
4. [Data Flow](#data-flow)
5. [Key Technologies](#key-technologies)
6. [Power System Domain Quick Reference](#power-system-domain-quick-reference)
   - [Time Structure](#time-structure)
   - [System Components](#system-components)
   - [Modeling Modes](#modeling-modes)
7. [Python Ecosystem](#python-ecosystem)
8. [Web Service and GUI Service](#web-service-and-gui-service)
9. [CI/CD Pipelines](#cicd-pipelines)
10. [Where to Go Next](#where-to-go-next)

---

## What gtopt Does

**gtopt** (**G**eneration and **T**ransmission **O**ptimal **P**lanning
**T**ool) is a high-performance C++ solver for
**Generation and Transmission Expansion Planning (GTEP)**.

Given a description of an electrical power system — its generators, demands,
transmission lines, storage, and hydro cascades — gtopt finds the **minimum-cost
combination of operational decisions and capacity investments** over a
multi-stage planning horizon under uncertainty.

The two cost components it minimizes are:

| Component | What it represents |
|-----------|-------------------|
| **OPEX** (operational) | Fuel cost, demand curtailment penalties, transmission losses |
| **CAPEX** (capital) | Annualised investment in new generation, storage, and transmission capacity |

**Mathematical core:** The problem is formulated as a large sparse **Linear
Program (LP)** or **Mixed-Integer Program (MIP)** and solved with pluggable
LP solver backends (CLP, CBC, CPLEX, HiGHS). See
[Mathematical Formulation](formulation/mathematical-formulation.md) for the
full LP formulation.

---

## Repository Layout

```
gtopt/
├── include/gtopt/          # ~150 public C++ headers (library API)
├── source/                 # ~70 .cpp implementation files (libgtopt)
├── test/source/            # ~110 test files (doctest framework)
│
├── standalone/             # thin main() wrapper → builds the `gtopt` binary
├── plugins/                # dynamically-loaded solver backends
│   ├── osi/                #   COIN-OR CLP/CBC
│   ├── cplex/              #   IBM CPLEX
│   └── highs/              #   HiGHS open-source solver
│
├── all/                    # super-project CMakeLists.txt (builds everything)
├── cmake/ + cmake_local/   # CMake modules (CPM, compiler checks, patches)
│
├── cases/                  # sample optimization cases (JSON + Parquet)
│   ├── ieee_4b_ori/        #   4-bus OPF (simplest)
│   ├── ieee_9b_ori/        #   classic IEEE 9-bus
│   ├── ieee_9b/            #   9-bus with 24-hour solar profile
│   ├── ieee_14b_ori/       #   IEEE 14-bus standard
│   ├── c0/, c1/            #   single-bus multi-stage expansion
│   ├── bat_4b*/            #   battery storage cases
│   └── sddp_hydro_3phase/  #   SDDP hydro with 3 stages
│
├── scripts/                # Python conversion utilities (self-contained package)
│   ├── plp2gtopt/          #   PLP → gtopt JSON
│   ├── igtopt/             #   Excel → gtopt JSON
│   ├── cvs2parquet/        #   CSV → Parquet
│   ├── pp2gtopt/           #   pandapower → gtopt
│   └── ts2gtopt/           #   time-series → gtopt horizon
│
├── webservice/             # Next.js REST API + browser UI
├── guiservice/             # Flask GUI for case editing and monitoring
│
├── docs/                   # written documentation
│   ├── overview.md         #   ← this file
│   ├── planning-guide.md   #   step-by-step planning walkthrough
│   ├── usage.md            #   CLI reference
│   ├── input-data.md       #   JSON/Parquet format reference
│   ├── formulation/        #   mathematical LP/MIP formulation
│   └── methods/            #   sddp.md, cascade.md, monolithic.md
│
├── documentation/          # Doxygen + m.css API docs build
└── integration_test/       # CMake helpers for end-to-end tests
```

---

## Core Library Architecture

The C++ library (`libgtopt`) is organized in five conceptual layers, each
built on top of the previous one.

### 1. Domain Model

**Location:** `include/gtopt/*.hpp` (header-only data structures)

Plain C++ structs that represent every element of a power system:

| Category | Types |
|----------|-------|
| **Electrical network** | `Bus`, `Line` |
| **Generation** | `Generator`, `GeneratorProfile` |
| **Demand** | `Demand`, `DemandProfile` |
| **Storage** | `Battery`, `Converter`, `Reservoir`, `Turbine` |
| **Hydro cascade** | `Junction`, `Waterway`, `Flow`, `Filtration`, `VolumeRight`, `FlowRight` |
| **Reserves** | `ReserveZone`, `ReserveProvision` |
| **Time** | `Phase`, `Scene`, `Stage`, `Block`, `Scenario` |
| **Top-level** | `System` (all components), `Planning` (system + options + simulation) |

All structs use C++26 designated initializers and are directly deserialised
from JSON (via [DAW JSON Link](https://github.com/beached/daw_json_link),
a zero-overhead compile-time reflection library).

A `FieldSched<T>` variant type (`std::variant<T, std::vector<T>, FileSched>`)
is the standard way to express parameters that can be a scalar, an inline
vector, or an external Parquet/CSV filename.

### 2. LP Assembly

**Location:** `include/gtopt/*_lp.hpp`, `source/*_lp.cpp`

Each domain component has a corresponding `*LP` class that translates the
component's physical model into LP variables and constraints:

| LP class | What it adds to the LP |
|----------|------------------------|
| `GeneratorLP` | `generation` variables, cost coefficients, capacity constraints |
| `DemandLP` | `load` and `fail` (curtailment) variables, penalty costs |
| `LineLP` | `flowp` variables, Kirchhoff angle-difference constraints |
| `BusLP` | `balance` constraints (Kirchhoff node equation) |
| `BatteryLP` / `StorageLP` | SoC state variable, charge/discharge efficiencies, energy bounds |
| `ReservoirLP` | Volume balance across blocks (hm³ units) |
| `ReserveZoneLP` | Up/down spinning-reserve requirement rows |
| `SimulationLP` | Assembles all component LP blocks for one time-slice |
| **`PlanningLP`** | Top-level orchestrator; wires stages/scenarios/components into the full LP |

The sparse LP matrix is built using `SparseCol` and `SparseRow` value types,
stored in `boost::container::flat_map` for 10–27× faster iteration compared
to `std::map`.

### 3. Solver Interface and Plugins

**Location:** `include/gtopt/solver_backend.hpp`, `include/gtopt/solver_registry.hpp`, `plugins/`

- **`SolverBackend`** is a pure virtual interface: `solve()`, `get_solution()`,
  `get_kappa()`, etc.
- **`SolverRegistry`** discovers plugins at runtime by scanning for
  `libgtopt_solver_*.so` files in standard paths (`$GTOPT_PLUGIN_DIR`,
  `<exe>/../lib/gtopt/plugins/`, etc.).
- Three plugins are provided: **OSI** (wraps CLP/CBC from COIN-OR), **CPLEX**
  (direct C Callable Library), and **HiGHS**.

This design means the solver can be swapped without recompiling the core
library. Priority order if multiple plugins are present: CPLEX > HiGHS > CBC > CLP.

### 4. Solution Methods

**Location:** `source/*_method.cpp`

Three planning methods are available, selected via the `method` option:

| Method | Key file | When to use |
|--------|----------|-------------|
| **Monolithic** (default) | `monolithic_method.cpp` | Single-period or small multi-stage problems |
| **SDDP** | `sddp_method.cpp` | Large multi-stage stochastic problems (Stochastic Dual Dynamic Programming with Benders cuts, forward/backward passes) |
| **Cascade** | `cascade_method.cpp` | Hierarchical multi-level SDDP with progressive LP refinement and warm-start |

See [SDDP Method](methods/sddp.md), [Cascade Method](methods/cascade.md), and
[Monolithic Method](methods/monolithic.md) for detailed documentation.

### 5. I/O Layer

**Location:** `include/gtopt/input_context.hpp`, `include/gtopt/output_context.hpp`, `source/array_index_traits.cpp`

- **Input**: Arrow table cache; reads Parquet (default) or CSV files on demand,
  keyed by `(component_type, field_name)`. Falls back to the other format
  if the preferred one is not found.
- **Output**: `OutputContext` writes solution arrays (generation, flows, duals,
  SoC) to Parquet or CSV files organised by component type in the
  `output/` directory.
- **JSON**: `Planning` is deserialised directly from the JSON planning file(s)
  via DAW JSON Link.

---

## Data Flow

```
  JSON planning file(s)          Parquet / CSV time-series
         │                                 │
         ▼                                 ▼
  Planning (C++ struct) ◄──── InputContext (Arrow table cache)
         │
         ▼
  PlanningLP.resolve()
         │
   ┌─────┴──────────────────────────────────┐
   │  SimulationLP × (stages × scenarios)   │
   │    GeneratorLP  DemandLP  LineLP  ...  │
   │    ↓ SparseCol + SparseRow             │
   └─────────────────────────────────────────┘
         │  LinearProblem (flat sparse matrix)
         ▼
  SolverBackend.solve()     ← dynamic plugin (CLP/CBC/CPLEX/HiGHS)
         │
         ▼
  OutputContext.write()
         │
         ▼
  output/solution.csv
  output/Generator/generation_sol.parquet
  output/Bus/balance_dual.parquet   (LMPs)
  output/Line/flowp_sol.parquet
  ...
```

---

## Key Technologies

| Technology | Version / Notes | Role |
|------------|----------------|------|
| **C++26** | Clang 21 (CI) | Core language — concepts, `std::format`, `std::ranges`, `std::expected`, `std::flat_map`, designated initializers |
| **CMake** | ≥ 3.31 | Build system; CPM for dependency management |
| **Apache Arrow / Parquet** | 12.0+ | Default I/O format for large time-series |
| **COIN-OR CLP/CBC** | via OSI plugin | Default LP/MIP solver |
| **CPLEX / HiGHS** | optional plugins | Alternative solver backends |
| **DAW JSON Link** | header-only | Zero-overhead compile-time JSON deserialisation |
| **spdlog** | std::format backend | Structured logging |
| **Boost.Container** | `flat_map` | Fast sparse LP matrix iteration |
| **doctest** | 2.4+ | C++ unit testing framework |
| **Next.js / TypeScript** | Node 20 | Web service (REST API + browser UI) |
| **Flask / Python** | Python ≥ 3.10 | GUI service (case editor + monitoring) |
| **ruff / pylint / mypy** | | Python linting, type-checking |

---

## Power System Domain Quick Reference

### Time Structure

The planning horizon is decomposed into three nested levels:

```
Scenario  (probability weight)
  └─ Stage   (investment period, e.g. one year; carries a discount factor)
       └─ Block  (operating period, e.g. one hour; carries a duration in hours)
```

- A **Block** is the smallest time unit. Energy = Power × duration.
- A **Stage** groups blocks into one planning year. Capacity built in a stage
  persists to later stages.
- A **Scenario** is one realisation of uncertain inputs (hydrology, demand).
  Its `probability_factor` weights it in the expected-cost objective.

### System Components

| Component | Role |
|-----------|------|
| `Bus` | Electrical node; reference bus has θ = 0 |
| `Generator` | Thermal / renewable / hydro unit (`gcost` in $/MWh) |
| `GeneratorProfile` | Per-block capacity-factor multiplier (0–1) for solar/wind |
| `Demand` | Fixed or flexible load |
| `DemandProfile` | Per-block demand scaling |
| `Line` | Transmission branch (`reactance` required for Kirchhoff mode) |
| `Battery` | Energy storage: charge/discharge efficiencies, SoC bounds |
| `Converter` | Couples a `Battery` to a `Generator` (discharge) and `Demand` (charge) |
| `ReserveZone` | Up/down spinning-reserve requirement |
| `ReserveProvision` | Links a generator to a reserve zone |
| `Junction` | Hydraulic node in a cascaded hydro system |
| `Waterway` | Water channel between junctions |
| `Reservoir` | Storage lake; volume balance in hm³ across blocks |
| `Turbine` | Links a waterway to a generator |
| `Flow` | External inflow or evaporation at a junction |
| `Filtration` | Water seepage from a waterway into a reservoir |

### Modeling Modes

| Option | Default | Effect |
|--------|---------|--------|
| `use_single_bus` | `false` | `true` → "copper plate" (no network constraints) |
| `use_kirchhoff` | `true` | `true` → full DC OPF with voltage angle variables |
| `use_line_losses` | `true` | Model resistive line losses |
| `demand_fail_cost` | 1000 $/MWh | Penalty for unserved load |
| `scale_objective` | 1000 | Divides all objective coefficients for solver numerics |
| `method` | `"monolithic"` | Planning method: `monolithic`, `sddp`, `cascade` |

---

## Python Ecosystem

The `scripts/` directory is a **self-contained Python package** (`pip install
./scripts`) with its own `pyproject.toml`. It provides CLI tools for
converting data from other power-system formats into gtopt's JSON + Parquet
format, plus validation and visualization utilities:

| Tool | Purpose |
|------|---------|
| `plp2gtopt` | Convert PLP Fortran `.dat` files → gtopt JSON + Parquet |
| `igtopt` | Convert Excel workbook (one sheet per component) → gtopt JSON |
| `pp2gtopt` | Convert pandapower network → gtopt JSON |
| `gtopt2pp` | Convert gtopt JSON → pandapower (with optional DC OPF) |
| `cvs2parquet` | Convert CSV time-series → Parquet |
| `ts2gtopt` | Project hourly time-series onto a gtopt planning horizon |
| `gtopt_diagram` | Generate network topology and planning diagrams |
| `gtopt_compare` | Validate gtopt results against pandapower DC OPF |
| `run_gtopt` | Smart solver wrapper with pre/post-flight checks |
| `sddp_monitor` | Live SDDP convergence monitoring dashboard |
| `gtopt_check_json` | Validate JSON planning files |
| `gtopt_check_lp` | Diagnose infeasible LP files |
| `gtopt_check_output` | Analyse solver output completeness |

For full details see the [Scripts Guide](scripts-guide.md).

---

## Web Service and GUI Service

**Web Service** (`webservice/`) — a **Next.js / TypeScript** application:

- REST API: upload a case ZIP, trigger the gtopt solver, poll status, download results.
- Browser UI: interactive tables and charts for submitting and reviewing
  optimization results.
- Can run as a standalone daemon (`gtopt_websrv`) or be integrated into the
  GUI workflow.

**GUI Service** (`guiservice/`) — a **Flask / Python** application:

- Web-based editor for creating and modifying gtopt cases
  (buses, generators, storage, demands).
- Connects to the web service for remote solving.
- `gtopt_gui` command opens the browser and auto-starts both services.

```
                  Browser
                     │
          ┌──────────┼──────────┐
          │                     │
   guiservice (Flask)    webservice (Next.js)
   port 5001               port 3000
          │                     │
          └──────────┬──────────┘
                     │
              gtopt binary
              (local or remote)
```

---

## CI/CD Pipelines

Located in `.github/workflows/`:

| Workflow | Trigger | Purpose |
|----------|---------|---------|
| `ubuntu.yml` | push / PR | Build (Clang 21), unit + integration tests; uploads `gtopt-binary-debug` artifact |
| `style.yml` | every push | clang-format + ruff format checks (warning only) |
| `autoformat.yml` | push (non-main) | Auto-applies clang-format + ruff, commits a fixup |
| `scripts.yml` | `scripts/**` changes | Python lint (ruff + pylint + mypy), unit tests, integration tests |
| `guiservice.yml` | `guiservice/**` changes | Flask lint + tests |
| `webservice.yml` | push / PR to main | Next.js build + e2e tests |
| `docs.yml` | push to master | Doxygen + m.css docs → GitHub Pages |
| `coverage.yml` | manual / push | C++ and Python coverage reports |

---

## Where to Go Next

| Goal | Document |
|------|---------|
| Build gtopt from source | [BUILDING.md](../BUILDING.md) |
| Run your first case | [Planning Guide → Quickstart](planning-guide.md#quickstart-your-first-solve) |
| Understand the LP formulation | [Mathematical Formulation](formulation/mathematical-formulation.md) |
| Full CLI reference | [Usage Guide](usage.md) |
| JSON / Parquet input format | [Input Data Reference](input-data.md) |
| Convert a PLP or pandapower case | [Scripts Guide](scripts-guide.md) |
| SDDP decomposition | [SDDP Method](methods/sddp.md) |
| Cascade multi-level method | [Cascade Method](methods/cascade.md) |
| Write user-defined constraints | [User Constraints](user-constraints.md) |
| Contribute code | [CONTRIBUTING.md](../CONTRIBUTING.md) |
| API documentation (Doxygen) | [GitHub Pages](https://marcelomatus.github.io/gtopt/) |
