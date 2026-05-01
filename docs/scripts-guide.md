# gtopt Python Scripts

The `scripts/` directory contains Python command-line utilities for
preparing, converting, visualizing, and post-processing data for use with gtopt.

> **Detailed documentation** for each script is available in the
> [`docs/scripts/`](scripts/) directory. This page provides a summary
> with links to the full documentation.

## Table of Contents

- [Installation](#installation)
- **Data Preparation & Conversion**
  - [gtopt_diagram](#gtopt_diagram) · [full docs](scripts/gtopt_diagram.md)
  - [plp2gtopt](#plp2gtopt) · [full docs](scripts/plp2gtopt.md)
  - [sddp2gtopt](#sddp2gtopt) · [full docs](scripts/sddp2gtopt.md)
  - [pp2gtopt](#pp2gtopt) · [full docs](scripts/pp2gtopt.md)
  - [gtopt2pp](#gtopt2pp) — convert gtopt JSON back to pandapower
  - [igtopt](#igtopt) · [full docs](scripts/igtopt.md) · [Excel template](templates/gtopt_template.xlsx) · `igtopt --make-template` regenerates the template
  - [cvs2parquet](#cvs2parquet) · [full docs](scripts/cvs2parquet.md)
  - [ts2gtopt](#ts2gtopt) · [full docs](scripts/ts2gtopt.md)
  - [gtopt_compare](#gtopt_compare) · [full docs](scripts/gtopt_compare.md)
- **Running & Monitoring**
  - [run_gtopt](#run_gtopt) — smart solver wrapper with pre/post-flight checks
  - [gtopt_monitor](#gtopt_monitor) — live solver convergence dashboard
- **Validation & Diagnostics**
  - [gtopt_check_json](#gtopt_check_json) — validate JSON planning files
  - [gtopt_check_lp](#gtopt_check_lp) — diagnose infeasible LP files
  - [gtopt_check_output](#gtopt_check_output) — analyze solver output
  - [gtopt_check_solvers](#gtopt_check_solvers) — discover and validate LP solver plugins
  - [gtopt_compress_lp](#gtopt_compress_lp) — compress LP debug files
  - [gtopt_check_fingerprint](#gtopt_check_fingerprint) — LP structural fingerprint verification · [full docs](lp-fingerprint.md)
  - [gtopt_check_pampl](#gtopt_check_pampl) — validate PAMPL expression syntax and LP resolution
  - [plp_compress_case](#plp_compress_case) — compress PLP case files
- **Utilities**
  - [gtopt_config](#gtopt_config) — unified configuration for all tools
  - [gtopt_field_extractor](#gtopt_field_extractor)
- [Tool Comparison (gtopt vs PLP vs pandapower)](tools/comparison.md)
- [Using with gtopt\_guisrv and gtopt\_websrv](#using-with-gtopt_guisrv-and-gtopt_websrv)

---

## Installation

Install all tools with a single `pip` command from the repository root:

```bash
pip install ./scripts
```

This registers all 22 command-line tools on your `PATH`:
`gtopt_diagram`, `plp2gtopt`, `sddp2gtopt`, `pp2gtopt`, `gtopt2pp`,
`igtopt`, `cvs2parquet`, `ts2gtopt`, `gtopt_compare`, `run_gtopt`,
`gtopt_monitor`, `gtopt_check_json`, `gtopt_check_lp`,
`gtopt_check_output`, `gtopt_check_solvers`, `gtopt_compress_lp`,
`gtopt_check_fingerprint`, `gtopt_field_extractor`, `gtopt_check_pampl`,
`gtopt_expand`, `cen_demanda`,
and `plp_compress_case`.  An editable install is useful during
development:

```bash
pip install -e "./scripts[dev]"
```

Optional extras unlock additional output formats:

```bash
pip install -e "./scripts[dev,diagram]"
# adds: graphviz, pyvis, cairosvg
```

### Package organization

Each command-line tool lives in its own Python package directory under
`scripts/`:

| Package directory | Command | Description |
|-------------------|---------|-------------|
| `gtopt_compare/` | `gtopt_compare` | pandapower ↔ gtopt comparison |
| `cvs2parquet/` | `cvs2parquet` | CSV → Parquet converter |
| `gtopt_diagram/` | `gtopt_diagram` | Network topology / planning diagrams |
| `gtopt_field_extractor/` | `gtopt_field_extractor` | C++ header field metadata extractor |
| `igtopt/` | `igtopt` | Excel → gtopt JSON converter |
| `plp2gtopt/` | `plp2gtopt` | PLP → gtopt JSON converter |
| `sddp2gtopt/` | `sddp2gtopt` | PSR SDDP → gtopt JSON converter |
| `pp2gtopt/` | `pp2gtopt` | pandapower → gtopt JSON converter |
| `gtopt2pp/` | `gtopt2pp` | gtopt JSON → pandapower converter |
| `run_gtopt/` | `run_gtopt` | Smart solver wrapper with pre/post-flight checks |
| `gtopt_check_json/` | `gtopt_check_json` | JSON planning file validator |
| `gtopt_check_lp/` | `gtopt_check_lp` | Infeasible LP file diagnostic tool |
| `gtopt_check_output/` | `gtopt_check_output` | Solver output analyzer |
| `gtopt_check_solvers/` | `gtopt_check_solvers` | LP solver plugin discovery & validation |
| `gtopt_compress_lp/` | `gtopt_compress_lp` | LP debug file compressor |
| `gtopt_check_fingerprint/` | `gtopt_check_fingerprint` | LP structural fingerprint verification & comparison |
| `gtopt_check_pampl/` | `gtopt_check_pampl` | PAMPL expression syntax and LP resolution validator |
| `plp_compress_case/` | `plp_compress_case` | PLP case file compressor |
| `gtopt_config/` | *(library)* | Unified configuration management |
| `gtopt_monitor/` | `gtopt_monitor` | Live solver monitoring dashboard |
| `ts2gtopt/` | `ts2gtopt` | Time-series → gtopt block schedule converter |
| `gtopt_expand/` | `gtopt_expand` | Expansion transforms: LNG terminal, ROR promotion, pumped storage, irrigation agreements (`gtopt_irrigation` is a deprecated alias) |
| `cen_demanda/` | `cen_demanda` | Coordinador Eléctrico Nacional SIPUB hourly demand downloader |

### Dependencies

| Package | Purpose |
|---------|---------|
| `numpy` | Numerical array processing |
| `pandas` | DataFrame I/O |
| `pyarrow` | Parquet read/write |
| `openpyxl` | Excel file support (`igtopt`) |
| `pandapower` | Power system network data (`pp2gtopt`, `gtopt2pp`, `gtopt_compare`) |
| `rich` | Styled terminal output (`gtopt_check_json`, `plp2gtopt`, `igtopt`, `run_gtopt`) |
| `matplotlib` *(optional)* | live charts (`gtopt_monitor` GUI mode) |
| `graphviz` *(optional)* | SVG/PNG/PDF rendering (`gtopt_diagram`) |
| `pyvis` *(optional)* | Interactive HTML diagrams (`gtopt_diagram`) |
| `cairosvg` *(optional)* | High-res PNG/PDF export (`gtopt_diagram`) |

---

## gtopt_diagram

> **[→ Full documentation](scripts/gtopt_diagram.md)**

Generates **network topology and planning-structure diagrams** from a gtopt
JSON planning file.  Supports multiple output formats (SVG, PNG, PDF, DOT,
Mermaid, interactive HTML) and automatic simplification of large cases via
aggregation modes.

### Basic usage

```bash
# Auto mode (default) – picks the best aggregation for your case size
gtopt_diagram cases/ieee_9b/ieee_9b.json -o ieee9b.svg

# Interactive HTML with physics simulation
gtopt_diagram cases/ieee_9b/ieee_9b.json --format html -o ieee9b.html

# Mermaid Markdown snippet (no extra dependencies)
gtopt_diagram cases/ieee_9b/ieee_9b.json --format mermaid

# Network-only: no generator nodes (clean topology view)
gtopt_diagram large_case.json --no-generators -o topo.svg

# Planning time-structure diagram
gtopt_diagram cases/c0/system_c0.json --diagram-type planning --format html
```

### Output formats (`--format`)

| Format | Description | Requires |
|--------|-------------|----------|
| `svg` | Scalable vector graphic (default) | `graphviz` |
| `png` | Raster image | `graphviz` + `cairosvg` |
| `pdf` | PDF document | `graphviz` + `cairosvg` |
| `dot` | Graphviz DOT source | — (no extra deps) |
| `mermaid` | Mermaid flowchart source | — (no extra deps) |
| `html` | Interactive vis.js browser diagram | `pyvis` |

### Aggregation modes (`--aggregate`)

| Mode | When auto selects it | Description |
|------|---------------------|-------------|
| `auto` | (default) | Chooses based on element count |
| `none` | < 100 elements | Every generator shown individually |
| `bus` | 100–999 elements | One summary node per bus |
| `type` | ≥ 1000 elements | One node per (bus, generator-type) pair |
| `global` | — (manual only) | One node per generator type, system-wide |

### Reducing large diagrams

```bash
# Keep only buses ≥ 220 kV (lump low-voltage buses into HV neighbours)
gtopt_diagram large_case.json --voltage-threshold 220 -o hv_topo.svg

# Show only hydro generators within 3 hops of a specific bus
gtopt_diagram large_case.json --filter-type hydro --focus-bus Chapo220 --focus-hops 3

# Hard node-count cap: escalate aggregation until ≤ 50 nodes remain
gtopt_diagram large_case.json --max-nodes 50 -o compact.svg

# Keep only the top-2 generators per bus by pmax
gtopt_diagram large_case.json --top-gens 2 -o top2.svg
```

### Visual features

- **Voltage-based line coloring**: transmission lines are drawn with color
  intensity and width proportional to the bus voltage level (higher voltage
  lines appear darker and wider).
- **Reservoir sizing**: reservoir nodes are scaled by their storage capacity
  (`emax`), so larger reservoirs appear visually larger in the diagram.
- **Reserve zones and provisions**: `reserve_zone_array` entries are rendered
  as zone nodes connected to the generators that provide reserves via
  `reserve_provision_array`.
- **Generator and demand profiles**: `generator_profile_array` and
  `demand_profile_array` entries are rendered as profile nodes linked to
  their parent generator or demand.
- **Colorblind palette**: use `--palette colorblind` to switch all element
  colors to a palette designed for color-vision deficiency accessibility.

### File-referenced value resolution

When fields like `pmax` or `lmax` are stored as Parquet file references,
`gtopt_diagram` resolves them using `--scenario`, `--stage`, and `--block`
(all default to UID 1).  This controls which row of the Parquet file is
used for sizing and labeling diagram elements.

### All options

```text
positional arguments:
  json_file             gtopt JSON planning file

options:
  -t, --diagram-type    topology (default) or planning
  -f, --format          dot | png | svg | pdf | mermaid | html  (default: svg)
  -o, --output          output file path
  -s, --subsystem       full | electrical | hydro  (default: full)
  -L, --layout          dot | neato | fdp | sfdp | circo | twopi
  -d, --direction       LR | TD | BT | RL  (Mermaid direction, default: LR)
  --clusters            Group in Graphviz sub-clusters
  --palette             default | colorblind  (default: default)
  --scenario UID        Scenario UID for resolving file-referenced values (default: 1)
  --stage UID           Stage UID for resolving file-referenced values (default: 1)
  --block UID           Block UID for resolving file-referenced values (default: 1)

reduction options:
  -a, --aggregate       auto | none | bus | type | global  (default: auto)
  --no-generators       Omit all generator nodes
  -g, --top-gens N      Keep only top-N generators per bus by pmax
  --filter-type TYPE    Show only: hydro solar wind thermal battery
  --focus-bus BUS       Show only elements within N hops of BUS (repeatable)
  --focus-generator GEN Focus on bus(es) connected to these generators (by name or uid)
  --focus-area KV       Focus on buses at or above this voltage level (kV)
  --focus-hops N        Hops for --focus-bus (default: 2)
  --max-nodes N         Hard cap; escalate aggregation until ≤ N nodes
  -V, --voltage-threshold KV  Lump buses below KV into HV neighbours
  --hide-isolated       Remove unconnected nodes
  --compact             Omit detail labels (names/counts only)
```

---

## plp2gtopt

> **[→ Full documentation](scripts/plp2gtopt.md)**

Converts a **PLP** (*Programación de Largo Plazo*) case directory to the gtopt JSON + Parquet
format.  Reads the standard PLP data files (`plpblo.dat`, `plpbar.dat`,
`plpcosce.dat`, `plpcnfce.dat`, `plpcnfli.dat`, `plpdem.dat`, `plpeta.dat`,
and others) and writes:

- A **gtopt JSON file** (`<output-dir>.json`) with the complete system,
  simulation, and options configuration.
- **Parquet time-series files** organised in subdirectories under
  `<output-dir>/` (e.g. `Demand/lmax.parquet`, `Generator/pmin.parquet`,
  `Afluent/afluent.parquet`).

### Hydro system conversion

When `plpcnfce.dat` contains reservoir (*embalse*) or series (*serie*)
centrals, `plp2gtopt` converts the cascaded hydro system into gtopt arrays.
Two additional **optional** PLP files extend the hydro model:

| PLP file | gtopt array | Description |
|----------|-------------|-------------|
| `plpcnfce.dat` | `junction_array`, `waterway_array`, `turbine_array`, `reservoir_array`, `flow_array` | Main hydro topology (required when hydro centrals exist) |
| `plpcenre.dat` | `reservoir_production_factor_array` | Volume-dependent turbine production factor (PLP *rendimiento*): piecewise-linear conversion rate as a function of reservoir storage |
| `plpcenfi.dat` | `reservoir_seepage_array` | Waterway-to-reservoir seepage (PLP *filtración*): linear model `flow = slope × volume + constant` |

Both `plpcenre.dat` and `plpcenfi.dat` are **optional** — if absent, the
corresponding arrays are simply not written.  When present and non-empty they
are silently parsed and appended to the JSON output.

**`plpcenre.dat` format** (*Archivo de Rendimiento de Embalses*):

```text
# Number of entries
N
# For each entry:
'CENTRAL_NAME'     ← turbine (central) name
'EMBALSE_NAME'     ← reservoir name
mean_production_factor    ← fallback production factor [MW·s/m³]
num_segments       ← number of piecewise-linear segments
idx  volume  slope  constant  scale  ← one line per segment
```

**`plpcenfi.dat` format** (*Archivo de Centrales Filtración*):

```text
# Number of entries
N
# For each entry:
'CENTRAL_NAME'     ← waterway source central name
'EMBALSE_NAME'     ← receiving reservoir name
slope  constant    ← seepage model [m³/s/dam³] and [m³/s]
```

### Basic usage

```bash
# Default: reads ./input, writes ./output.json + ./output/
plp2gtopt

# Explicit input/output directories
plp2gtopt -i plp_case_dir -o gtopt_case_dir

# Limit conversion to the first 5 stages
plp2gtopt -i input/ -s 5

# Single hydrology (1-based, default)
plp2gtopt -i input/ -y 1

# Two hydrology scenarios (1-based) with 60/40 probability split
plp2gtopt -i input/ -y 1,2 -p 0.6,0.4

# Range selector: hydrologies 1, 2, and 5 through 10
plp2gtopt -i input/ -y 1,2,5-10

# Group PLP stages 1–4 into phase 1, then one stage per phase after
plp2gtopt -i input/ --stages-phase '1:4,5,6,7,8,9,10,...'

# Apply a 10% annual discount rate
plp2gtopt -i input/ -d 0.10

# Verbose debug output
plp2gtopt -i input/ -l DEBUG
```

### Hydrology index format (`-y` / `--hydrologies`)

Hydrology indices follow the **Fortran 1-based convention**: index `1` refers
to the first hydrology column in the PLP data files.  The argument accepts
comma-separated values and ranges:

| Syntax | Meaning |
|--------|---------|
| `1` | First hydrology only (default) |
| `1,2` | Hydrologies 1 and 2 |
| `1,2,5-10` | Hydrologies 1, 2, and 5 through 10 |
| `1,2,5-10,11` | Hydrologies 1, 2, 5-10, and 11 |

The internal 0-based index stored in the output JSON (`"hydrology"` field in
`scenario_array`) is automatically computed as `input_index - 1`.

### Phase layout (`--stages-phase`)

By default, phase assignment is controlled by `--solver`:
- `sddp` (default): one phase per PLP stage
- `mono` / `monolithic`: one phase covering all stages

The `--stages-phase` option overrides this with an explicit mapping.  Tokens
are comma-separated and use 1-based PLP stage indices:

| Token | Meaning |
|-------|---------|
| `N` | Single stage N as one phase |
| `N:M` | Stages N through M (inclusive) as one phase |
| `...` | (trailing) auto-expand one stage per phase for remaining stages |

```bash
# Stages 1-4 as phase 1, stages 5-10 each as their own phase,
# then one stage per phase for any remaining stages
plp2gtopt -i input/ --stages-phase '1:4,5,6,7,8,9,10,...'

# Group all stages into two phases: 1-12 and 13-24
plp2gtopt -i input/ --stages-phase '1:12,13:24'
```

### Pasada (run-of-river) hydro modeling (`--pasada-hydro`)

By default (`--pasada-hydro`, enabled), PLP *pasada* (run-of-river) centrals
are converted into the full gtopt hydro topology: a junction, waterway,
turbine, and flow element for each central.  This preserves hydrological
connectivity and allows the solver to model water balance constraints.

Use `--no-pasada-hydro` to revert to the legacy behavior, where pasada
centrals are modeled as generators with time-series profiles containing
normalized capacity factors derived from the PLP afluent data.

### Block-to-hour map (`indhor.csv`)

When the PLP input directory contains `indhor.csv`, `plp2gtopt` reads it and
writes a normalised `BlockHourMap/block_hour_map.parquet` file alongside the
other Parquet outputs.  A `"block_hour_map"` key is added to the simulation
section of the output JSON so that post-processing tools (e.g. `ts2gtopt`)
can reconstruct hourly time-series from block-granularity solver output.

The `indhor.csv` format has columns:
`Año, Mes, Dia, Hora, Bloque` (all integers; `Hora` is 1-based 1-24).

### ZIP output (`-z` / `--zip`)

The `-z` flag creates a single **ZIP archive** that bundles the JSON
configuration file and all Parquet/CSV data files together, preserving the
full output directory structure.  This archive is directly compatible with
**gtopt\_guisrv** (upload via the GUI) and **gtopt\_websrv** (submit via the
REST API):

```bash
plp2gtopt -z -i plp_case_2y -o gtopt_case_2y
# Produces: gtopt_case_2y.zip
```

The ZIP layout is:

```text
gtopt_case_2y.zip
├── gtopt_case_2y.json          ← main system/simulation/options config
└── gtopt_case_2y/              ← input_directory (data files)
    ├── Demand/
    │   └── lmax.parquet
    ├── Generator/
    │   ├── pmin.parquet
    │   └── pmax.parquet
    └── Afluent/
        └── afluent.parquet
```

> **Note**: the `input_directory` field in the JSON options matches the name
> of the subdirectory inside the ZIP, so gtopt\_guisrv and gtopt\_websrv can
> locate the data files without any extra configuration.

### Conversion statistics

After a successful conversion, `plp2gtopt` logs statistics (at INFO level)
similar to the pre-solve statistics printed by the `gtopt` solver:

```text
=== System statistics ===
  System name     : plp2gtopt
=== System elements  ===
  Buses           : 2
  Generators      : 3
  Generator profs : 1
  Demands         : 2
  Lines           : 1
  Batteries       : 0
  Converters      : 0
  Junctions       : 2
  Waterways       : 1
  Reservoirs      : 1
  Turbines        : 1
=== Simulation statistics ===
  Blocks          : 8760
  Stages          : 5
  Scenarios       : 2
=== Key options ===
  use_single_bus  : False
  scale_objective : 1000
  demand_fail_cost: 1000
  input_directory : gtopt_case_2y
  annual_discount : 0.1
=== Conversion time ===
  Elapsed         : 1.234s
```

Use `-l DEBUG` to also see which individual `.dat` files are being parsed.

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `-i, --input-dir DIR` | `input` | PLP input directory |
| `-o, --output-dir DIR` | `output` | Output directory for Parquet/CSV files |
| `-f, --output-file FILE` | `<output-dir>.json` | JSON output file path |
| `-z, --zip` | off | Create a ZIP archive of the JSON + data files |
| `-s, --last-stage N` | all | Stop after stage N |
| `-d, --discount-rate RATE` | `0.0` | Annual discount rate (e.g. `0.10` for 10%) |
| `-m, --management-factor F` | `0.0` | Demand management factor |
| `-t, --last-time T` | all | Stop at time T |
| `-c, --compression ALG` | `gzip` | Parquet compression (`gzip`, `snappy`, `brotli`, `none`) |
| `-y, --hydrologies H1[,H2,…]` | `1` | Hydrology scenario indices (1-based Fortran convention; accepts ranges e.g. `1,2,5-10`) |
| `-p, --probability-factors P1[,P2,…]` | equal | Probability weights per scenario |
| `--stages-phase SPEC` | (solver default) | Explicit phase layout; comma-separated stage indices/ranges with optional `...` wildcard |
| `--solver TYPE` | `sddp` | Simulation structure: `sddp` (one phase/scene per stage/scenario) or `mono`/`monolithic` (single phase and scene) |
| `--pasada-hydro / --no-pasada-hydro` | enabled | Model *pasada* (run-of-river) centrals as full hydro topology (junction, waterway, turbine, flow) instead of generator profiles. Use `--no-pasada-hydro` for legacy behavior with generator profiles using normalized capacity factors |
| `--stationary-tol TOL` | (auto) | Secondary convergence tolerance for stationary-gap detection. When the relative change in the SDDP gap over the last `--stationary-window` iterations falls below this value, the solver declares convergence even if gap > `convergence_tol`. Default: `convergence_tol / 10`. Set to `0` to disable |
| `--stationary-window N` | `4` | Number of iterations to look back when checking gap stationarity. Only used when `--stationary-tol` is set |
| `-l, --log-level LEVEL` | `INFO` | Verbosity (`DEBUG`, `INFO`, `WARNING`, `ERROR`) |
| `-V, --version` | — | Print version and exit |

### Isolated centrals

During hydro topology conversion, PLP centrals that have no bus assignment
(`bus <= 0`), no waterway connections, and are not referenced by any other
central are considered **isolated** and are silently skipped.  After the
conversion statistics, a "Skipped Centrals" section lists all isolated
centrals by name so the user can verify that they are genuinely unused.

### Error messages

`plp2gtopt` raises descriptive errors for common problems:

| Situation | Error message |
|-----------|---------------|
| Input directory missing | `Input directory does not exist: 'plp_case/'` |
| Required `.dat` file missing | `Required file not found: …/plpblo.dat` |
| Invalid data format | `Invalid data format: …` |

---

## sddp2gtopt

> **[→ Full documentation](scripts/sddp2gtopt.md)**

Converts a **PSR SDDP** (*Stochastic Dual Dynamic Programming*, by
PSR Inc.) case directory to a gtopt JSON planning, mirroring the
role `plp2gtopt` plays for PLP cases.  v0 is JSON-first: it reads
the typed `psrclasses.json` snapshot the SDDP GUI saves alongside
the Fortran-style `.dat` files, parses the standard PSR collections
(`PSRStudy`, `PSRSystem`, `PSRDemand`, `PSRDemandSegment`, `PSRFuel`,
`PSRThermalPlant`, `PSRHydroPlant`, `PSRGaugingStation`), and writes
a single-bus monolithic gtopt planning that `gtopt --lp-only`
ingests directly.

> ⚠️  Targets the **PSR Inc. commercial** SDDP format only — *not*
> the academic Julia [SDDP.jl](https://sddp.dev) package.

### Basic usage

```bash
# Inspect a case (no conversion)
sddp2gtopt --info  /path/to/sddp_case

# Schema sanity check
sddp2gtopt --validate /path/to/sddp_case

# Convert (default: writes ./gtopt_<case>/<case>.json)
sddp2gtopt /path/to/sddp_case

# Explicit output dir
sddp2gtopt -i /path/to/sddp_case -o gtopt_case
```

### What gets mapped

| PSR collection                   | gtopt target                       | Status        |
|----------------------------------|------------------------------------|---------------|
| `PSRStudy`                       | `options` + `simulation`           | ✅            |
| `PSRSystem`                      | synthesised single bus per system  | ✅ (single)   |
| `PSRThermalPlant` + `PSRFuel`    | `generator_array` (`gcost = CEsp × Custo`) | ✅      |
| `PSRHydroPlant`                  | `generator_array` (zero-cost, flat)| ⚠️ flattened  |
| `PSRDemand` + `PSRDemandSegment` | `demand_array` with inline `lmax`  | ✅ (GWh→MW)   |
| `PSRGaugingStation`              | inflow series                      | ⏳ deferred   |
| Multi-system / multi-bus         | extra `bus_array` + `line_array`   | ⏳ deferred   |

The converter's roadmap (hydro reservoir + waterway, multi-bus,
multi-scenario, `.dat` fallback) is tracked in
[`scripts/sddp2gtopt/DESIGN.md`](../scripts/sddp2gtopt/DESIGN.md);
the per-element mapping rules and unit conversions are documented
in detail in [`scripts/sddp2gtopt.md`](scripts/sddp2gtopt.md).

---

## pp2gtopt

> **[→ Full documentation](scripts/pp2gtopt.md)**

Converts a **pandapower** network to gtopt JSON format.  Accepts either a
built-in IEEE test network (via `-n`) or any pandapower network file saved to
disk (via `-f`).  Writes a self-contained gtopt JSON file ready to be solved
directly with the `gtopt` binary or submitted via `gtopt_guisrv` / `gtopt_websrv`.

### Basic usage

```bash
# Convert the default IEEE 30-bus built-in network → ieee30b.json
pp2gtopt

# Convert a saved pandapower JSON file
pp2gtopt -f my_network.json -o my_case.json

# Convert a MATPOWER case file
pp2gtopt -f case39.m -o case39.json

# Convert a pandapower Excel workbook
pp2gtopt -f network.xlsx -o network.json

# Use a specific built-in test network
pp2gtopt -n case14 -o ieee14b.json

# List all available built-in test networks
pp2gtopt --list-networks
```

### Input from file (`-f / --file`)

`-f FILE` loads any pandapower network saved to disk.  The format is
auto-detected from the file extension:

| Extension | Format | Produced by |
|-----------|--------|-------------|
| `.json` | pandapower JSON | `pandapower.to_json()` |
| `.xlsx` / `.xls` | pandapower Excel | `pandapower.to_excel()` |
| `.m` | MATPOWER case file | MATPOWER / Octave |

The output JSON system `name` is derived from the file stem (e.g. `case39.m`
→ `"case39"`).

### Available built-in networks (`-n / --network`)

| Network name | pandapower function | Description |
|---|---|---|
| `ieee30b` *(default)* | `case_ieee30` | IEEE 30-bus (Washington) |
| `case4gs` | `case4gs` | 4-bus Glover-Sarma |
| `case5` | `case5` | 5-bus example |
| `case6ww` | `case6ww` | 6-bus Wood-Wollenberg |
| `case9` | `case9` | IEEE 9-bus |
| `case14` | `case14` | IEEE 14-bus |
| `case33bw` | `case33bw` | 33-bus Baran-Wu |
| `case57` | `case57` | IEEE 57-bus |
| `case118` | `case118` | IEEE 118-bus |

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `-f, --file FILE` | — | pandapower network file (`.json`, `.xlsx`/`.xls`, `.m`) |
| `-n, --network NAME` | `ieee30b` | built-in test network (mutually exclusive with `-f`) |
| `-o, --output FILE` | `<stem>.json` | Output JSON file path |
| `--list-networks` | — | Print all available built-in network names and exit |
| `-V, --version` | — | Print version and exit |

---

## igtopt

> **[→ Full documentation](scripts/igtopt.md)** ·
> **[Excel template](templates/gtopt_template.xlsx)**

Converts an **Excel workbook** to a gtopt JSON case.  Reads all named sheets
from the workbook and writes:

- A **gtopt JSON file** with the complete system, simulation, and options
  configuration.
- **Parquet (or CSV) time-series files** written to the `input_directory` for
  any sheet whose name contains `@` (e.g. `Demand@lmax` →
  `<input_dir>/Demand/lmax.parquet`).

### Basic usage

```bash
# Basic conversion – output JSON and input directory derived from workbook name
igtopt system.xlsx

# Write output to an explicit JSON file
igtopt system.xlsx -j output/system.json

# Pretty-printed JSON (4-space indented), skip null/NaN values
igtopt system.xlsx --pretty --skip-nulls

# CSV time-series files instead of Parquet
igtopt system.xlsx -f csv

# Bundle JSON + data files into a ZIP archive (ready for gtopt_guisrv/websrv)
igtopt system.xlsx --zip

# Convert multiple workbooks in one run (merges into a single JSON)
igtopt case_a.xlsx case_b.xlsx -d /data/input

# Validate the workbook without writing output (exit 0 = OK, 1 = errors)
igtopt system.xlsx --validate

# Proceed even if some sheets have errors (partial output)
igtopt system.xlsx --ignore-errors

# Show debug log messages
igtopt system.xlsx -l DEBUG
```

### Template generation (`-T` / `--make-template`)

The `--make-template` flag reads the gtopt C++ JSON header files
(`include/gtopt/json/`) and generates a ready-to-use Excel template.
Re-running after adding a new JSON element to the C++ source automatically
produces an up-to-date workbook.

```bash
# Regenerate docs/templates/gtopt_template.xlsx (default output)
igtopt --make-template

# Write to a custom path (reuses the -j/--json-file flag)
igtopt --make-template -j /tmp/my_template.xlsx

# Print the sheet list that would be generated (no file written)
igtopt --make-template --list-sheets

# Custom C++ header directory
igtopt --make-template --header-dir /path/to/include/gtopt
```

### ZIP output (`-z` / `--zip`)

The `-z` flag creates a single **ZIP archive** that bundles the JSON
configuration file and all Parquet/CSV data files together, preserving the
full directory structure.  This archive is directly compatible with
**gtopt\_guisrv** (upload via the GUI) and **gtopt\_websrv** (submit via the
REST API):

```bash
igtopt system.xlsx --zip
# Produces: system.zip
#   system.zip
#   ├── system.json
#   └── system/
#       ├── Demand/
#       │   └── lmax.parquet
#       └── GeneratorProfile/
#           └── profile.parquet
```

### Conversion statistics

After a successful conversion, `igtopt` logs statistics (at INFO level)
similar to those printed by `plp2gtopt`:

```text
=== System statistics ===
  Buses           : 57
  Generators      : 7
  Demands         : 42
  Lines           : 80
=== Simulation statistics ===
  Blocks          : 1
  Stages          : 1
  Scenarios       : 1
=== Key options ===
  use_single_bus  : False
  scale_objective : 1000
  demand_fail_cost: 1000
  input_directory : system
=== Conversion time ===
  Elapsed         : 0.123s
```

### Excel workbook format

The workbook can contain any of the following named sheets.  Sheets whose name
starts with `.` (e.g. `.notes`) are silently skipped.

#### System and simulation sheets

| Sheet name | Description |
|------------|-------------|
| `options` | Key/value pairs written to the JSON `options` block (two columns: `option`, `value`) |
| `block_array` | Time blocks (`uid`, `name`, `duration`) |
| `stage_array` | Investment stages (`uid`, `first_block`, `count_block`, `discount_factor`) |
| `scenario_array` | Scenarios (`uid`, `probability_factor`) |
| `phase_array` | SDDP phases (`uid`, `first_stage`, `count_stage`, `aperture_set`) — leave empty for monolithic solver; `aperture_set` is a JSON array of aperture UIDs to restrict the SDDP backward pass for this phase |
| `scene_array` | SDDP scenes (`uid`, `first_scenario`, `count_scenario`) — leave empty for monolithic solver |
| `bus_array` | Electrical buses (`uid`, `name`, `voltage`, `reference_theta`, `use_kirchhoff`) |
| `generator_array` | Generators (`uid`, `name`, `bus`, `gcost`, `pmax`, `capacity`, …) |
| `generator_profile_array` | Generator capacity factors (`uid`, `name`, `generator`, `profile`, `scost`) |
| `demand_array` | Demands (`uid`, `name`, `bus`, `lmax`, `fcost`, …) |
| `demand_profile_array` | Demand scaling profiles (`uid`, `name`, `demand`, `profile`, `scost`) |
| `line_array` | Transmission lines (`uid`, `name`, `bus_a`, `bus_b`, `reactance`, `tmax_ab`, `tmax_ba`, …) |
| `battery_array` | Batteries (`uid`, `name`, `bus`, `emin`, `emax`, `pmax_charge`, `pmax_discharge`, …) |
| `converter_array` | Battery charge/discharge converter links (`uid`, `name`, `battery`, `generator`, `demand`) |
| `reserve_zone_array` | Spinning-reserve zones (`uid`, `name`, `urreq`, `drreq`) |
| `reserve_provision_array` | Generator–zone reserve provision links (`uid`, `name`, `generator`, `reserve_zones`) |
| `junction_array` | Hydraulic junctions (`uid`, `name`, `drain`) |
| `waterway_array` | Water channels between junctions (`uid`, `name`, `junction_a`, `junction_b`, `fmax`) |
| `flow_array` | External inflows/outflows at junctions (`uid`, `name`, `junction`, `discharge`, `direction`) |
| `reservoir_array` | Storage lakes/dams (`uid`, `name`, `junction`, `emin`, `emax`, `ecost`) |
| `reservoir_seepage_array` | Water seepage from waterways into reservoirs (`uid`, `name`, `waterway`, `reservoir`, `slope`, `constant`, `segments` — `segments` is a JSON array of `{"volume", "slope", "constant"}` for piecewise-linear seepage) |
| `turbine_array` | Hydro turbines (`uid`, `name`, `waterway`, `generator`, `conversion_rate`) |
| `reservoir_production_factor_array` | Volume-dependent turbine productivity curves (`uid`, `name`, `turbine`, `reservoir`, `mean_production_factor`) |
| `user_constraint_array` | User-defined custom LP constraints added to the problem |

#### Time-series sheets (`@` convention)

Any sheet whose name contains `@` encodes a time-series table that is written
as a Parquet (or CSV) file to the `input_directory`.  The naming convention is:

```text
<component_type>@<field_name>
```

The sheet must contain `scenario`, `stage`, `block` index columns followed by
one column per element (named after the element's `name` field).

| Sheet name example | Produces | Description |
|--------------------|----------|-------------|
| `Demand@lmax` | `<input_dir>/Demand/lmax.parquet` | Per-block demand limits |
| `GeneratorProfile@profile` | `<input_dir>/GeneratorProfile/profile.parquet` | Solar/wind capacity factors |
| `Generator@pmax` | `<input_dir>/Generator/pmax.parquet` | Per-block generator max output |
| `Battery@emax` | `<input_dir>/Battery/emax.parquet` | Per-block battery energy upper bound |

When a time-series sheet is present, the corresponding field in the system
array sheet (e.g. `lmax` in `demand_array`) should contain the string
`"lmax"` (the file stem without extension) as a file reference.

#### Example: demand with a 24-hour lmax time-series

`demand_array` sheet:

| uid | name | bus | lmax  |
|-----|------|-----|-------|
| 1   | d3   | b3  | lmax  |
| 2   | d4   | b4  | lmax  |

`Demand@lmax` sheet:

| scenario | stage | block | d3  | d4  |
|----------|-------|-------|-----|-----|
| 1        | 1     | 1     | 30  | 20  |
| 1        | 1     | 2     | 28  | 18  |
| …        | …     | …     | …   | …   |

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `XLSX` (positional) | — | Excel workbook(s) to convert (one or more) |
| `-j, --json-file FILE` | `<first stem>.json` | Output JSON file path |
| `-d, --input-directory DIR` | `<first stem>/` | Directory for time-series data files |
| `-f, --input-format {parquet,csv}` | `parquet` | Format for time-series output files |
| `-n, --name NAME` | `<first stem>` | System name written to JSON `name` field |
| `-c, --compression ALG` | `gzip` | Parquet compression (`gzip`, `snappy`, `brotli`, `''` for none) |
| `-p, --pretty` | off | Write indented (4-space) JSON instead of compact |
| `-N, --skip-nulls` | off | Omit keys with null/NaN values from JSON output |
| `-U, --parse-unexpected-sheets` | off | Also process sheets not in the expected list |
| `-z, --zip` | off | Bundle JSON + data files into a ZIP archive |
| `--validate` | off | Check workbook for errors without writing output (exit 0 = OK, 1 = errors) |
| `--ignore-errors` | off | Proceed despite errors in individual sheets (output may be incomplete) |
| `-l, --log-level LEVEL` | `INFO` | Verbosity (`DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL`) |
| `-V, --version` | — | Print version and exit |
| `-T, --make-template` | off | Generate the Excel template from C++ JSON headers instead of converting |
| `--header-dir DIR` | auto-detect | Path to `include/gtopt/` (used with `--make-template`) |
| `--list-sheets` | off | Print sheet list from C++ headers and exit (used with `--make-template`) |

---

## cvs2parquet

> **[→ Full documentation](scripts/cvs2parquet.md)**

Converts **CSV time-series files** to Parquet format.

```bash
# Convert a single file
cvs2parquet input.csv output.parquet

# Use an explicit PyArrow schema for type enforcement
cvs2parquet --schema input.csv output.parquet
```

Columns named `stage`, `block`, or `scenario` are cast to `int32`; all other
columns are cast to `float64`.

---

## ts2gtopt

> **[→ Full documentation](scripts/ts2gtopt.md)**

Projects **hourly (or finer) time-series data** onto a gtopt planning horizon
and produces block-aggregated schedule files (Parquet or CSV) ready for use as
gtopt input schedules.  It also embeds an `hour_block_map` in the output
planning JSON so that block-level solver results can be reconstructed back into
a full hourly time-series.

### Concepts

| Term | Meaning |
|------|---------|
| **Horizon** | A JSON definition of scenarios, stages (investment periods), and blocks (representative operating hours) |
| **Block schedule** | A Parquet/CSV file with `scenario, stage, block, uid:X` columns consumed directly by gtopt as an input schedule (e.g. `Generator/pmax.parquet`) |
| **`hour_block_map`** | An array of `{"hour": i, "stage": s, "block": b}` entries embedded in the planning JSON that maps each processed calendar hour back to the `(stage, block)` it was projected into |
| **`output_hour/`** | Post-processing output directory with `scenario, hour, uid:X` files reconstructed from block-level solver output |

### Basic usage

```bash
# Project a single hourly CSV onto an auto-generated 12-stage × 24-block horizon
ts2gtopt demand.csv -y 2023 -o input/

# Write 4 seasonal stages × 6 blocks each
ts2gtopt solar.csv -y 2023 --stages 4 --blocks 6 -o input/

# Use a custom horizon JSON and export updated block durations
ts2gtopt pmax.csv -y 2023 -H my_horizon.json --output-horizon updated_horizon.json -o input/

# Use the horizon embedded in an existing planning JSON (reads simulation.stage_array / block_array)
ts2gtopt load.csv -y 2023 -P case.json -o input/
```

### Reconstructing hourly output

After running the `gtopt` solver (which produces `output/Generator/generation_sol.csv` etc.),
use the `hour_block_map` embedded in the planning JSON to expand block-level
results back into a full hourly time-series:

```python
from ts2gtopt import write_output_hours

# Reads hour_block_map from case.json, writes output_hour/ next to output/
written = write_output_hours("bat_4b_2023.json")
# → output_hour/Generator/generation_sol.csv
#    scenario  hour   uid:1   uid:2
#    1          0    127.5    0.0
#    1          1    124.9    0.0
#    ...  (8760 rows for a full-year case)
```

Or from the public API:

```python
from ts2gtopt import build_hour_block_map, reconstruct_output_hours, load_horizon

h = load_horizon("my_horizon.json")
hour_map = build_hour_block_map(h, year=2023)
reconstruct_output_hours("output/", hour_map, output_hour_dir="output_hour/")
```

### CLI reference

```text
ts2gtopt [options] INPUT [INPUT ...]

Positional arguments:
  INPUT                  Input time-series file(s) (CSV or Parquet)

Options:
  -y, --year YEAR        Calendar year for the projection (required unless -H/-P is given with explicit dates)
  -o, --output DIR       Output directory for block schedule files (default: current directory)
  -f, --format {parquet,csv}
                         Output file format (default: parquet)
  -s, --stages N         Number of planning stages (default: 12, one per calendar month)
  -b, --blocks N         Number of representative blocks per stage (default: 24, one per hour of day)
  -H, --horizon FILE     Load planning horizon from a JSON file
  -P, --planning FILE    Load horizon from an existing gtopt planning JSON
  --output-horizon FILE  Write the duration-updated horizon to FILE after projecting
  --agg {mean,median,min,max,sum}
                         Aggregation function (default: mean)
  --interval-hours H     Duration of each input observation in hours (auto-detected by default)
  --verify               Print energy-conservation ratios after projection
  -v, --verbose          Enable verbose logging
```

---

## gtopt2pp

Converts a **gtopt JSON case** back to a **pandapower** network file,
optionally solving a DC OPF and/or running topology diagnostics.  This is
the reverse direction of `pp2gtopt`.

### Basic usage

```bash
# Convert a gtopt case to pandapower JSON
gtopt2pp cases/ieee_9b_ori/ieee_9b_ori.json

# Specify output file
gtopt2pp cases/ieee_9b_ori/ieee_9b_ori.json -o ieee9b_pp.json

# Convert and solve DC OPF
gtopt2pp cases/ieee_9b_ori/ieee_9b_ori.json --solve

# Convert all blocks (one output file per block)
gtopt2pp cases/ieee_9b/ieee_9b.json --all-blocks

# Select specific blocks (comma-separated, ranges)
gtopt2pp cases/ieee_9b/ieee_9b.json -b 1,5-10

# Run pandapower diagnostic on converted network
gtopt2pp cases/ieee_14b_ori/ieee_14b_ori.json --diagnostic
```

### Multi-block support

When the source case has multiple blocks (e.g. 24-hour dispatch), use
`--all-blocks` or `-b SPEC` to produce one pandapower network per block:

```bash
gtopt2pp cases/ieee_9b/ieee_9b.json --all-blocks
# Produces: ieee_9b_pp_b1.json, ieee_9b_pp_b2.json, …, ieee_9b_pp_b24.json
```

### Elements skipped during conversion

The following gtopt elements have no pandapower equivalent and are silently
skipped: batteries, converters, junctions, waterways, reservoirs, turbines,
filtrations, flows.  A summary of skipped elements is logged.

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `case_file` (positional) | — | Path to gtopt JSON case file |
| `-o, --output PATH` | `<stem>_pp.json` | Output pandapower JSON file path |
| `-s, --scenario UID` | first | Scenario UID to convert |
| `-b, --block SPEC` | first | Block UID spec: single (`1`), list (`1,2,4`), range (`1-5`), or mix (`1,3-5,8`) |
| `--solve` | off | Run pandapower DC OPF after conversion |
| `--all-blocks` | off | Convert all blocks (one file per block) |
| `--check / --no-check` | enabled | Validate source JSON via `gtopt_check_json` |
| `--diagnostic` | off | Run pandapower topology diagnostic |

---

## run_gtopt

Smart **solver wrapper** that detects case types (PLP, gtopt directory, or
JSON file), runs conversions when needed, and invokes the `gtopt` binary with
appropriate runtime options.  Integrates pre-flight and post-flight checks
automatically.

### Basic usage

```bash
# Auto-detect case type from CWD
run_gtopt

# PLP case: auto-convert to gtopt and solve
run_gtopt plp_case_2y

# gtopt case directory: solve directly
run_gtopt cases/ieee_9b

# Explicit JSON file
run_gtopt cases/ieee_9b/ieee_9b.json

# Pass extra arguments to the gtopt binary after --
run_gtopt cases/ieee_9b -- --set use_single_bus=true --stats
```

### Pre-flight checks (enabled by default)

Before invoking the solver, `run_gtopt` validates:

- JSON syntax and file readability
- Input file existence (Parquet/CSV references)
- Output directory writability
- Compression codec availability (auto-fallback if unavailable)
- System/simulation statistics (`gtopt_check_json --info`)
- Full JSON validation (`gtopt_check_json`)

Disable with `--no-check` or abort on any warning with `--strict`.

### Post-flight checks

After the solver completes, `run_gtopt` automatically:

- Analyzes any error LP files via `gtopt_check_lp`
- Validates solver output via `gtopt_check_output`
- Prints a solution summary

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `CASE` (positional) | CWD | PLP directory, gtopt directory, or JSON file |
| `-t, --threads N` | auto | Number of LP solver threads |
| `-C, --compression CODEC` | `zstd` | Output compression codec |
| `-o, --output-dir DIR` | — | Override output directory |
| `--plp-args ARGS` | — | Extra arguments for `plp2gtopt` (quote whole string) |
| `--check / --no-check` | enabled | Run pre-flight checks |
| `--strict` | off | Abort on any warning |
| `--enable-check NAME` | — | Enable specific check (repeatable) |
| `--disable-check NAME` | — | Disable specific check (repeatable) |
| `--list-checks` | — | List available checks and exit |
| `--convert-only` | off | Convert PLP case but do not run solver |
| `--export-json FILE` | — | Write sanitized planning JSON to file |
| `--dry-run` | off | Print commands without executing |
| `-l, --log-level LEVEL` | `INFO` | Verbosity: `DEBUG`, `INFO`, `WARNING`, `ERROR` |
| `-V, --version` | — | Print version and exit |
| `-- [ARGS]` | — | Pass remaining arguments to gtopt binary |

---

## gtopt_check_json

Validates **gtopt JSON planning files** and reports potential issues.  Also
serves as a quick system/simulation statistics tool (similar to
`gtopt --stats`).

### Basic usage

```bash
# Validate a JSON case and report issues
gtopt_check_json cases/ieee_9b/ieee_9b.json

# Print system/simulation statistics only (no validation)
gtopt_check_json --info cases/ieee_9b/ieee_9b.json

# Show detailed simulation structure (scenarios, stages, phases, apertures)
gtopt_check_json --show-simulation cases/sddp_hydro_3phase/sddp_hydro_3phase.json

# Validate multiple JSON files (merged before checking)
gtopt_check_json system.json overrides.json

# Run interactive configuration setup
gtopt_check_json --init-config
```

### Validation checks

The tool runs configurable checks organized by severity:

| Severity | Color | Meaning |
|----------|-------|---------|
| **CRITICAL** | Red | Issues that will likely cause solver failure |
| **WARNING** | Yellow | Potential problems or suboptimal configurations |
| **NOTE** | Cyan | Informational observations |

Individual checks can be enabled/disabled via `--init-config` or the
configuration file (`~/.gtopt.conf`).

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | OK — no critical issues (warnings/notes may be present) |
| `1` | Critical issues found |

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `json_files` (positional) | — | Path(s) to gtopt JSON case files |
| `--info` | off | Print system/simulation statistics and exit |
| `--show-simulation` | off | Print detailed simulation structure |
| `--config PATH` | `~/.gtopt.conf` | Path to configuration file |
| `--init-config` | off | Run interactive configuration setup |
| `--no-color` | off | Disable colored output |

---

## gtopt_check_lp

Diagnoses **infeasible LP files** generated by the gtopt solver.  Combines
static analysis, local IIS (Irreducible Infeasible Subsystem) solvers, and
optional NEOS remote analysis to pinpoint the root cause of infeasibility.

### Basic usage

```bash
# Analyze an LP file
gtopt_check_lp error_0.lp

# Analyze a gzip-compressed LP file
gtopt_check_lp error_0.lp.gz

# Auto-find the newest error*.lp[.gz] in the current directory
gtopt_check_lp --last

# Static analysis only (no solver invocation)
gtopt_check_lp error_0.lp --analyze-only

# Use a specific solver
gtopt_check_lp error_0.lp --solver coinor

# Submit to NEOS remote server (requires email)
gtopt_check_lp error_0.lp --solver neos --email user@example.com

# Run interactive configuration setup
gtopt_check_lp --init-config
```

### Analysis pipeline

1. **Static analysis** — parses the LP file directly:
   - Variables with conflicting bounds (`lb > ub`)
   - Empty or fixed constraints
   - Numerical range issues (very large/small coefficients)
   - Duplicate constraint names
   - Problem statistics (rows, columns, non-zeros)

2. **Local IIS finding** — tries available solvers in order:
   CPLEX → HiGHS → COIN-OR CLP → CBC → GLPK

3. **NEOS remote analysis** — submits to https://neos-server.org via XML-RPC
   using CPLEX (requires email)

4. **AI diagnostics** *(optional)* — expert infeasibility diagnosis from
   Claude, OpenAI, DeepSeek, or GitHub AI

### Quiet mode

When called with `--quiet` (used automatically by the `gtopt` binary and
`run_gtopt`), the tool never fails (always exits 0), never prompts for
input, and handles all errors gracefully with warnings.

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `LP_FILE` (positional) | — | Path to LP file (`.lp`, `.lp.gz`, `.lp.gzip`) |
| `--last` | off | Auto-find newest `error*.lp[.gz]` |
| `--analyze-only` | off | Static analysis only, no solver |
| `-q, --quiet` | off | Non-failing quiet mode |
| `--solver SOLVER` | `all` | Solver strategy: `all`, `auto`, `cplex`, `highs`, `coinor`, `glpk`, `neos` |
| `--no-neos` | off | Skip NEOS submissions |
| `--email EMAIL` | — | Email for NEOS submissions |
| `--output FILE` | — | Write report to file |
| `-v, --verbose` | off | Verbose logging |
| `--config FILE` | `~/.gtopt.conf` | Config file path |
| `--init-config` | off | Interactive config wizard |

---

## gtopt_check_output

Validates and analyzes **gtopt solver output** for completeness and
correctness.  Auto-discovers results and JSON files in a case directory.

### Basic usage

```bash
# Analyze output in a case directory
gtopt_check_output cases/ieee_9b

# Explicit paths to results and JSON
gtopt_check_output -r output/ -j ieee_9b.json

# Quiet mode — only show warnings and critical findings
gtopt_check_output cases/ieee_9b --quiet

# Run interactive configuration setup
gtopt_check_output --init-config
```

### Checks performed

- **Output completeness** — verifies expected output files exist
  (`solution.csv`, `generation_sol`, `balance_dual`, etc.)
- **Load shedding analysis** — detects unserved energy in `fail_sol`
- **Generation/demand balance** — validates energy conservation
- **Line congestion ranking** — identifies most congested transmission lines
- **LMP statistics** — locational marginal price analysis
- **Cost breakdown** — dispatches by generator cost tier

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `CASE_DIR` (positional) | CWD | Case directory (auto-discovers JSON and results) |
| `-r, --results-dir DIR` | — | Explicit results directory |
| `-j, --json-file FILE` | — | Explicit planning JSON file |
| `--no-color` | off | Disable colored output |
| `-q, --quiet` | off | Only show warnings and critical findings |
| `-l, --log-level LEVEL` | `WARNING` | Verbosity: `DEBUG`, `INFO`, `WARNING`, `ERROR` |
| `--config FILE` | `~/.gtopt.conf` | Config file path |
| `--init-config` | off | Initialize config section |
| `-V, --version` | — | Print version and exit |

---

## gtopt_check_solvers

Discovers and validates **gtopt LP solver plugins** by running a built-in
test suite against each available solver.  Useful for quickly verifying that
solver backends (CLP, CBC, HiGHS, CPLEX, …) are correctly installed and
produce correct results.

### Basic usage

```bash
# List all available LP solver plugins
gtopt_check_solvers --list

# Check all available solvers (default)
gtopt_check_solvers

# Check a specific solver
gtopt_check_solvers --solver clp

# Check multiple specific solvers
gtopt_check_solvers --solver clp --solver highs

# Use an explicit gtopt binary path
gtopt_check_solvers --gtopt-bin /opt/gtopt/bin/gtopt

# Run only a subset of built-in tests
gtopt_check_solvers --test single_bus_lp --test kirchhoff_lp

# Verbose output (show failure details)
gtopt_check_solvers --verbose
```

### Built-in test cases

| Test name | Description | Expected status |
|-----------|-------------|-----------------|
| `single_bus_lp` | Single-bus copper-plate LP: 1 generator, 1 demand | 0 (optimal) |
| `kirchhoff_lp` | 4-bus DC OPF with Kirchhoff voltage-angle constraints | 0 (optimal) |
| `feasibility_lp` | Feasibility check: demand exactly at generator capacity | 0 (optimal) |

For each test the tool:
1. Writes the problem as a temporary gtopt JSON file
2. Calls `gtopt <file> --solver <name>` with a per-test timeout
3. Reads `output/solution.csv` and validates `status` and `obj_value`

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | All solvers and all tests passed |
| 1 | At least one test failed (or no solvers found) |
| 2 | Error: gtopt binary not found or invalid path |

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `--list`, `-l` | off | List available solvers and exit |
| `--solver SOLVER`, `-s` | all | Solver to test (repeatable) |
| `--test TEST`, `-t` | all | Test case to run (repeatable; choices: `single_bus_lp`, `kirchhoff_lp`, `feasibility_lp`) |
| `--gtopt-bin PATH` | auto-detect | Path to the `gtopt` binary |
| `--timeout SECONDS` | `60` | Per-test timeout in seconds |
| `--no-color` | off | Disable colored output |
| `-v, --verbose` | off | Show failure details |
| `-V, --version` | — | Print version and exit |

### Binary discovery

The tool searches for the `gtopt` binary in this order:

1. `GTOPT_BIN` environment variable
2. `gtopt` on `PATH`
3. Standard build directories (`build/standalone/gtopt`, etc.)

---

## gtopt_compress_lp

Compresses **LP debug files** generated by the gtopt solver (when
`lp_debug=true` or `lp_only=true`, i.e. CLI `--lp-debug` / `--lp-only`).  Supports multiple compression
formats and integrates seamlessly with the solver's post-solve workflow.

### Basic usage

```bash
# Compress an LP file
gtopt_compress_lp file.lp

# Use a specific compression codec
gtopt_compress_lp file.lp --codec zstd

# Quiet mode (non-interactive, never fails)
gtopt_compress_lp file.lp --quiet

# Show available compression tools
gtopt_compress_lp --list-tools

# Run interactive setup
gtopt_compress_lp --init-config
```

### Supported compressors

| Tool | Extension | Notes |
|------|-----------|-------|
| `zstd` | `.zst` | Recommended — fast with excellent compression ratio |
| `gzip` | `.gz` | Universally available |
| `lz4` | `.lz4` | Fastest compression/decompression |
| `bzip2` | `.bz2` | Best ratio for text files |
| `xz` | `.xz` | Excellent ratio, slower compression |
| `lzma` | `.lzma` | Similar to `xz` |

### Compression cascade

1. Codec hint from `--codec` (if provided and available)
2. Configured compressor from config file (`auto` → first available)
3. First available tool found on `PATH`
4. Skip compression (leave original unchanged)

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `FILE.lp` (positional) | — | LP file(s) to compress |
| `--init-config` | off | Run interactive setup wizard |
| `--list-tools` | off | Show available compression tools |
| `--quiet` | off | Non-interactive, never-fail mode |
| `--codec CODEC` | — | Codec suggestion: `gzip`, `zstd`, `lz4`, etc. |
| `--compressor TOOL` | — | Override configured compressor |
| `--config PATH` | `~/.gtopt_compress_lp.conf` | Config file path |
| `--color {auto,always,never}` | `auto` | Terminal color output |
| `--version` | — | Show version and exit |

---

## gtopt_check_fingerprint

Computes, verifies, and compares **LP structural fingerprints** — a
SHA-256 hash of the sorted set of `(class, variable, context_type)` triples
that captures which types of LP variables and constraints exist, independent
of element counts.  See [LP Fingerprint](lp-fingerprint.md) for full details.

### Basic usage

```bash
# Compute fingerprint from an LP file
gtopt_check_fingerprint compute problem.lp -o fingerprint.json

# Verify an LP file against a golden fingerprint
gtopt_check_fingerprint verify --lp-file problem.lp --golden golden.json

# Compare two fingerprint JSON files (structural only, ignores stats)
gtopt_check_fingerprint compare --actual actual.json --expected golden.json
```

### Subcommands

| Subcommand | Purpose |
|------------|---------|
| `compute` | Parse an LP file and write a fingerprint JSON |
| `verify` | Parse an LP file and compare against a golden JSON |
| `compare` | Compare two fingerprint JSON files |

### Compute options

| Flag | Default | Description |
|------|---------|-------------|
| `LP_FILE` (positional) | — | LP file to parse (`.lp` format) |
| `-o`, `--output` | stdout | Output fingerprint JSON path |

### Verify options

| Flag | Default | Description |
|------|---------|-------------|
| `--lp-file` | — | LP file to parse |
| `--golden` | — | Golden fingerprint JSON to compare against |

### Compare options

| Flag | Default | Description |
|------|---------|-------------|
| `--actual` | — | Actual fingerprint JSON |
| `--expected` | — | Expected (golden) fingerprint JSON |

Exit code 0 = match, 1 = structural mismatch.  The `stats` section
(element counts) is always ignored — only structural template and hash
are compared.

---

## gtopt_config

Unified **configuration management** library used by all gtopt Python scripts.
Reads and writes a single INI-format configuration file shared across tools.

> `gtopt_config` is a library module, not a standalone command.  It is used by
> `gtopt_check_json`, `gtopt_check_lp`, `gtopt_check_output`, `run_gtopt`, and
> `gtopt_compress_lp`.

### Configuration file

Default location: `~/.gtopt.conf`.  The file uses INI format with a `[global]`
section for shared settings, per-tool sections, and a `[gtopt]` section for the
C++ binary:

```ini
[global]
ai_enabled  = false
ai_provider = claude
ai_model    =
color       = auto

[gtopt]
solver              = highs
algorithm           = barrier
threads             = 4
output-format       = parquet
output-compression  = zstd

[gtopt_check_json]
check_uid_uniqueness = true

[gtopt_check_output]
congestion_top_n = 10

[run_gtopt]
default_threads = 0
```

The `[gtopt]` section provides default values for the C++ binary's command-line
options.  CLI flags always take precedence.  See [Usage
Guide](usage.md#configuration-file-gtoptconf) for the full list of supported
keys.

### Initializing configuration

Each tool that uses `gtopt_config` provides an `--init-config` flag that runs
an interactive setup wizard to create or update its section:

```bash
gtopt_check_json --init-config
gtopt_check_lp --init-config
gtopt_check_output --init-config
run_gtopt --init-config     # via run_gtopt --list-checks / --init-config
```

---

## gtopt_monitor

Interactive **solver monitoring dashboard** for gtopt.  Polls the JSON status file
written by the gtopt solver (SDDP or monolithic) and displays live charts in two
figure windows:

- **Figure 1** – Real-time charts: CPU load (%) and active worker threads vs wall-clock seconds.
- **Figure 2** – Iteration-indexed charts: objective upper/lower bounds per scene and convergence
  gap vs iteration number.

```bash
# Monitor the default output/solver_status.json
gtopt_monitor

# Specify a custom status file and polling interval
gtopt_monitor --status-file /path/to/solver_status.json --poll 2.0

# Headless mode (print to stdout, no GUI window)
gtopt_monitor --no-gui

# Query a planning option value
gtopt_monitor --get method --case-dir ./mycase
gtopt_monitor --get sddp_options.max_iterations --case-dir ./mycase
```

The `sddp_monitor` command is still available as a backward-compatible alias.

The tool exits when the solver reports "converged" or when you press Ctrl-C.

### Text mode output

In headless mode (`--no-gui`), the tool prints a summary table to stdout:

```text
[Time]  [Iter]  [LB]           [UB]           [Gap]      [Status]
  7.1s   45     12345.1234   12450.5678     0.008765   running
```

### All options

| Flag | Default | Description |
|------|---------|-------------|
| `--status-file PATH` | `output/solver_status.json` | Path to solver status file |
| `--poll SECONDS` | `1.0` | Polling interval |
| `--no-gui` | off | Print to stdout instead of GUI windows |

---

## gtopt_field_extractor

Extracts **field metadata** from gtopt C++ headers and generates Markdown or
HTML documentation tables.  Parses `///< Description [units]` comments on
struct member declarations and produces a table with columns:
`Field`, `C++ Type`, `JSON Type`, `Units`, `Required`, `Description`.

```bash
# Dump all element tables to stdout as Markdown
gtopt_field_extractor

# Write full HTML reference to a file
gtopt_field_extractor --format html --output INPUT_DATA_API.html

# Extract only Generator and Demand elements
gtopt_field_extractor --elements Generator Demand
```

---

## Using with gtopt\_guisrv and gtopt\_websrv

Both services accept a **ZIP archive** containing the JSON configuration file
and its associated Parquet/CSV data files.  Use `plp2gtopt -z` to produce a
ZIP that is ready to upload without any extra packaging step.

### gtopt\_guisrv (browser GUI)

1. Start the GUI service:

   ```bash
   gtopt_guisrv
   # Open http://localhost:5001
   ```

2. Click **Upload case** and select the `.zip` file produced by
   `plp2gtopt -z`.

3. Edit the case if needed, then click **Solve** to submit it to the
   webservice.

For installation and service setup, see
[guiservice/INSTALL.md](guiservice/INSTALL.md).

### gtopt\_websrv (REST API)

Submit a ZIP directly to the REST API:

```bash
curl -X POST http://localhost:3000/api/solve \
  -F "file=@gtopt_case_2y.zip"
```

The server runs the `gtopt` solver and returns a results ZIP containing the
solution files.

For full API reference and deployment instructions, see
[webservice/INSTALL.md](webservice/INSTALL.md).

## gtopt_check_pampl

Validates PAMPL (gtopt constraint expression language) syntax and verifies
that all LP column references resolve correctly for a given JSON planning file.

```bash
# Check all user constraints in a planning file
gtopt_check_pampl cases/mycase/mycase.json

# Verbose output showing each constraint and its resolution
gtopt_check_pampl cases/mycase/mycase.json --verbose

# Check only a specific constraint file
gtopt_check_pampl --constraint-file constraints.json cases/mycase/mycase.json
```

### Common use cases

- Verify new user constraints before running the solver
- Diagnose resolution errors (unknown elements, bad expression syntax)
- Confirm LP column names in constraint expressions match the actual LP

## plp_compress_case

Compresses a PLP case directory for archival or transport, using xz compression
on the data files.

```bash
# Compress a PLP case directory
plp_compress_case /path/to/plp/case

# Decompress
plp_compress_case --decompress /path/to/plp/case.tar.xz

# List contents
plp_compress_case --list /path/to/plp/case.tar.xz
```

A Bash variant `plp_compress_case.sh` is also available in the `scripts/`
directory for environments without Python.
