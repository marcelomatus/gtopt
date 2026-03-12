# gtopt Python Scripts

The `scripts/` directory contains Python command-line utilities for
preparing, converting, visualising, and post-processing data for use with gtopt.

> **Detailed documentation** for each script is available in the
> [`docs/scripts/`](docs/scripts/) directory. This page provides a summary
> with links to the full documentation.

## Table of Contents

- [Installation](#installation)
- [gtopt-diagram](#gtopt-diagram) · [full docs](docs/scripts/gtopt-diagram.md)
- [plp2gtopt](#plp2gtopt) · [full docs](docs/scripts/plp2gtopt.md)
- [pp2gtopt](#pp2gtopt) · [full docs](docs/scripts/pp2gtopt.md)
- [igtopt](#igtopt) · [full docs](docs/scripts/igtopt.md) · [Excel template](docs/templates/gtopt_template.xlsx) · `igtopt --make-template` regenerates the template
- [cvs2parquet](#cvs2parquet) · [full docs](docs/scripts/cvs2parquet.md)
- [ts2gtopt](#ts2gtopt) · [full docs](docs/scripts/ts2gtopt.md)
- [gtopt-compare](#gtopt-compare) · [full docs](docs/scripts/gtopt-compare.md)
- [sddp-monitor](#sddp-monitor)
- [gtopt-field-extractor](#gtopt-field-extractor)
- [Using with gtopt\_guisrv and gtopt\_websrv](#using-with-gtopt_guisrv-and-gtopt_websrv)

---

## Installation

Install all tools with a single `pip` command from the repository root:

```bash
pip install ./scripts
```

This registers the `gtopt-diagram`, `plp2gtopt`, `pp2gtopt`, `igtopt`,
`cvs2parquet`, `ts2gtopt`, `gtopt-compare`, `sddp-monitor`,
`gtopt-field-extractor`, and other commands on your `PATH`.  An editable install is useful during
development:

```bash
pip install -e "./scripts[dev]"
```

Optional extras unlock additional output formats:

```bash
pip install -e "./scripts[dev,diagram]"
# adds: graphviz, pyvis, cairosvg
```

### Package organisation

Each command-line tool lives in its own Python package directory under
`scripts/`:

| Package directory | Command | Description |
|-------------------|---------|-------------|
| `gtopt_compare/` | `gtopt-compare` | pandapower ↔ gtopt comparison |
| `cvs2parquet/` | `cvs2parquet` | CSV → Parquet converter |
| `gtopt_diagram/` | `gtopt-diagram` | Network topology / planning diagrams |
| `gtopt_field_extractor/` | `gtopt-field-extractor` | C++ header field metadata extractor |
| `igtopt/` | `igtopt` | Excel → gtopt JSON converter |
| `plp2gtopt/` | `plp2gtopt` | PLP → gtopt JSON converter |
| `pp2gtopt/` | `pp2gtopt` | pandapower → gtopt JSON converter |
| `sddp_monitor/` | `sddp-monitor` | SDDP solver live monitoring dashboard |
| `ts2gtopt/` | `ts2gtopt` | Time-series → gtopt block schedule converter |

### Dependencies

| Package | Purpose |
|---------|---------|
| `numpy` | Numerical array processing |
| `pandas` | DataFrame I/O |
| `pyarrow` | Parquet read/write |
| `openpyxl` | Excel file support (`igtopt`) |
| `pandapower` | Power system network data (`pp2gtopt`) |
| `graphviz` *(optional)* | SVG/PNG/PDF rendering (`gtopt-diagram`) |
| `pyvis` *(optional)* | Interactive HTML diagrams (`gtopt-diagram`) |
| `cairosvg` *(optional)* | High-res PNG/PDF export (`gtopt-diagram`) |

---

## gtopt-diagram

> **[→ Full documentation](docs/scripts/gtopt-diagram.md)**

Generates **network topology and planning-structure diagrams** from a gtopt
JSON planning file.  Supports multiple output formats (SVG, PNG, PDF, DOT,
Mermaid, interactive HTML) and automatic simplification of large cases via
aggregation modes.

### Basic usage

```bash
# Auto mode (default) – picks the best aggregation for your case size
gtopt-diagram cases/ieee_9b/ieee_9b.json -o ieee9b.svg

# Interactive HTML with physics simulation
gtopt-diagram cases/ieee_9b/ieee_9b.json --format html -o ieee9b.html

# Mermaid Markdown snippet (no extra dependencies)
gtopt-diagram cases/ieee_9b/ieee_9b.json --format mermaid

# Network-only: no generator nodes (clean topology view)
gtopt-diagram large_case.json --no-generators -o topo.svg

# Planning time-structure diagram
gtopt-diagram cases/c0/system_c0.json --diagram-type planning --format html
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
gtopt-diagram large_case.json --voltage-threshold 220 -o hv_topo.svg

# Show only hydro generators within 3 hops of a specific bus
gtopt-diagram large_case.json --filter-type hydro --focus-bus Chapo220 --focus-hops 3

# Hard node-count cap: escalate aggregation until ≤ 50 nodes remain
gtopt-diagram large_case.json --max-nodes 50 -o compact.svg

# Keep only the top-2 generators per bus by pmax
gtopt-diagram large_case.json --top-gens 2 -o top2.svg
```

### All options

```
positional arguments:
  json_file             gtopt JSON planning file

options:
  -t, --diagram-type    topology (default) or planning
  -f, --format          dot | png | svg | pdf | mermaid | html  (default: svg)
  -o, --output          output file path
  -s, --subsystem       full | electrical | hydro  (default: full)
  -l, --layout          dot | neato | fdp | sfdp | circo | twopi
  -d, --direction       LR | TD | BT | RL  (Mermaid direction, default: LR)
  --clusters            Group in Graphviz sub-clusters

reduction options:
  -a, --aggregate       auto | none | bus | type | global  (default: auto)
  --no-generators       Omit all generator nodes
  -g, --top-gens N      Keep only top-N generators per bus by pmax
  --filter-type TYPE    Show only: hydro solar wind thermal battery
  --focus-bus BUS       Show only elements within N hops of BUS (repeatable)
  --focus-hops N        Hops for --focus-bus (default: 2)
  --max-nodes N         Hard cap; escalate aggregation until ≤ N nodes
  -V, --voltage-threshold KV  Lump buses below KV into HV neighbours
  --hide-isolated       Remove unconnected nodes
  --compact             Omit detail labels (names/counts only)
```

---

## plp2gtopt

> **[→ Full documentation](docs/scripts/plp2gtopt.md)**

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
| `plpcenre.dat` | `reservoir_efficiency_array` | Volume-dependent turbine efficiency (PLP *rendimiento*): piecewise-linear conversion rate as a function of reservoir storage |
| `plpcenfi.dat` | `filtration_array` | Waterway-to-reservoir seepage (PLP *filtración*): linear model `flow = slope × volume + constant` |

Both `plpcenre.dat` and `plpcenfi.dat` are **optional** — if absent, the
corresponding arrays are simply not written.  When present and non-empty they
are silently parsed and appended to the JSON output.

**`plpcenre.dat` format** (*Archivo de Rendimiento de Embalses*):

```
# Number of entries
N
# For each entry:
'CENTRAL_NAME'     ← turbine (central) name
'EMBALSE_NAME'     ← reservoir name
mean_efficiency    ← fallback efficiency [MW·s/m³]
num_segments       ← number of piecewise-linear segments
idx  volume  slope  constant  scale  ← one line per segment
```

**`plpcenfi.dat` format** (*Archivo de Centrales Filtración*):

```
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

```
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

```
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
| `-l, --log-level LEVEL` | `INFO` | Verbosity (`DEBUG`, `INFO`, `WARNING`, `ERROR`) |
| `-V, --version` | — | Print version and exit |

### Error messages

`plp2gtopt` raises descriptive errors for common problems:

| Situation | Error message |
|-----------|---------------|
| Input directory missing | `Input directory does not exist: 'plp_case/'` |
| Required `.dat` file missing | `Required file not found: …/plpblo.dat` |
| Invalid data format | `Invalid data format: …` |

---

## pp2gtopt

> **[→ Full documentation](docs/scripts/pp2gtopt.md)**

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

> **[→ Full documentation](docs/scripts/igtopt.md)** ·
> **[Excel template](docs/templates/gtopt_template.xlsx)**

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

```
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
| `phase_array` | SDDP phases (`uid`, `first_stage`, `count_stage`) — leave empty for monolithic solver |
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
| `filtration_array` | Water seepage from waterways into reservoirs (`uid`, `name`, `waterway`, `reservoir`) |
| `turbine_array` | Hydro turbines (`uid`, `name`, `waterway`, `generator`, `conversion_rate`) |
| `reservoir_efficiency_array` | Volume-dependent turbine productivity curves (`uid`, `name`, `turbine`, `reservoir`, `mean_efficiency`) |

#### Time-series sheets (`@` convention)

Any sheet whose name contains `@` encodes a time-series table that is written
as a Parquet (or CSV) file to the `input_directory`.  The naming convention is:

```
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
| `-l, --log-level LEVEL` | `INFO` | Verbosity (`DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL`) |
| `-V, --version` | — | Print version and exit |
| `-T, --make-template` | off | Generate the Excel template from C++ JSON headers instead of converting |
| `--header-dir DIR` | auto-detect | Path to `include/gtopt/` (used with `--make-template`) |
| `--list-sheets` | off | Print sheet list from C++ headers and exit (used with `--make-template`) |

---

## cvs2parquet

> **[→ Full documentation](docs/scripts/cvs2parquet.md)**

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

> **[→ Full documentation](docs/scripts/ts2gtopt.md)**

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

```
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

## sddp-monitor

Interactive **SDDP solver monitoring dashboard**.  Polls the JSON status file
written by the gtopt SDDP solver and displays live charts in two figure windows:

- **Figure 1** – Real-time charts: CPU load (%) and active worker threads vs wall-clock seconds.
- **Figure 2** – Iteration-indexed charts: objective upper/lower bounds per scene and convergence
  gap vs iteration number.

```bash
# Monitor the default output/sddp_status.json
sddp-monitor

# Specify a custom status file and polling interval
sddp-monitor --status-file /path/to/sddp_status.json --poll 2.0

# Headless mode (print to stdout, no GUI window)
sddp-monitor --no-gui
```

The tool exits when the solver reports "converged" or when you press Ctrl-C.

---

## gtopt-field-extractor

Extracts **field metadata** from gtopt C++ headers and generates Markdown or
HTML documentation tables.  Parses `///< Description [units]` comments on
struct member declarations and produces a table with columns:
`Field`, `C++ Type`, `JSON Type`, `Units`, `Required`, `Description`.

```bash
# Dump all element tables to stdout as Markdown
gtopt-field-extractor

# Write full HTML reference to a file
gtopt-field-extractor --format html --output INPUT_DATA_API.html

# Extract only Generator and Demand elements
gtopt-field-extractor --elements Generator Demand
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
