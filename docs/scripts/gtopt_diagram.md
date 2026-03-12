# gtopt_diagram — Network & Planning Diagram Tool

Generates **electrical network topology, hydro cascade, and planning
time-structure diagrams** from any gtopt JSON planning file.  Supports multiple
output formats and automatic simplification of large cases via aggregation
modes.

---

## Overview

Visualising a power system case is essential for debugging, presentation, and
validation.  `gtopt_diagram` reads a standard gtopt JSON planning file and
produces publication-quality diagrams showing:

- **Topology diagrams**: buses, generators (with type-specific icons), demands,
  transmission lines, batteries, converters, and hydro cascade elements
  (junctions, waterways, reservoirs, turbines, flows, filtrations).
- **Planning diagrams**: the time-structure hierarchy (scenarios → phases →
  stages → blocks) and the objective function formula.

For large cases (hundreds of buses, thousands of generators) the tool
automatically applies aggregation, voltage-level reduction, and smart
thresholds to keep diagrams readable.

---

## Installation

The tool is part of the gtopt scripts package:

```bash
# Basic install (registers gtopt_diagram on PATH)
pip install ./scripts

# With optional diagram-rendering dependencies
pip install -e "./scripts[diagram]"
# adds: graphviz (Python bindings), pyvis, cairosvg
```

The **Graphviz system package** is also required for SVG/PNG/PDF output:

```bash
# Ubuntu / Debian
sudo apt-get install graphviz

# macOS
brew install graphviz
```

After installation:

```bash
gtopt_diagram --help
```

### Dependencies

| Package | Purpose | Required for |
|---------|---------|-------------|
| `graphviz` (system) | Graph layout engine | SVG, PNG, PDF output |
| `graphviz` (Python) | Python bindings to Graphviz | SVG, PNG, PDF output |
| `pyvis` | Interactive vis.js diagrams | HTML output |
| `cairosvg` | High-res PNG/PDF export from SVG | PNG, PDF output |

> DOT and Mermaid output formats require **no extra dependencies**.

---

## Output Formats (`--format`)

| Format | Description | Requires |
|--------|-------------|----------|
| `svg` | Scalable vector graphic (default) | `graphviz` system package + Python bindings |
| `png` | Rasterised PNG image | `graphviz` + `cairosvg` |
| `pdf` | PDF document | `graphviz` + `cairosvg` |
| `dot` | Raw Graphviz DOT source | — (no extra deps) |
| `mermaid` | Mermaid flowchart source (embeds in GitHub Markdown) | — (no extra deps) |
| `html` | Interactive vis.js browser diagram with physics simulation | `pyvis` |

---

## Diagram Types (`--diagram-type`)

| Type | Description |
|------|-------------|
| `topology` | Network topology: buses, generators, demands, lines, batteries, converters, hydro elements (default) |
| `planning` | Planning time hierarchy: scenarios → phases → stages → blocks, plus objective function formula |

---

## Subsystems (`--subsystem`, topology only)

| Value | Elements shown |
|-------|---------------|
| `full` | Both electrical and hydro (default) |
| `electrical` | Buses, generators, demands, lines, batteries, converters |
| `hydro` | Junctions, waterways, reservoirs, turbines, flows, filtrations |

---

## Aggregation Modes (`--aggregate`)

| Mode | When auto selects it | Description |
|------|---------------------|-------------|
| `auto` | (default) | Chooses strategy based on element count |
| `none` | < 100 elements | Every generator shown individually |
| `bus` | 100–999 elements | One summary node per bus (e.g. `"BusA generators\n12 units · 1 440 MW"`) |
| `type` | ≥ 1000 elements | One node per `(bus, generator-type)` pair (e.g. `"☀️ Solar @ BusA\n3 units · 90 MW"`) |
| `global` | — (manual only) | One node per generator type for the whole system |

### Auto mode details

The default `auto` mode selects a strategy based on the **total element count**
(generators + buses + demands + lines + hydro elements):

| Total elements | Strategy |
|----------------|----------|
| < 100 | `none` — show every element individually |
| 100 – 999 | `bus` — one summary node per bus |
| ≥ 1000 | `type` + **smart voltage threshold** — threshold auto-computed to keep ≤ 64 buses |

The status line printed to stderr always shows what was chosen:

```
Diagram: 15 nodes, 15 edges                          # <100 (ieee9b)
Diagram: 605 nodes, 727 edges  [aggregate=type, voltage≥345 kV, auto(2794 elements)]
```

### Generator type detection

| Type | Detection method |
|------|-----------------|
| **Hydro** | Linked turbine in `turbine_array` |
| **Solar** | Name keywords: `solar`, `pv`, `foto`, `fotov` |
| **Wind** | Name keywords: `wind`, `eol`, `eólico` |
| **Battery** | Name keywords: `bat`, `bess`, `ess` |
| **Nuclear** | Name keywords: `nuclear`, `nuc` |
| **Gas turbine** | Name keywords: `gas`, `tg`, `ccgt` |
| **Thermal** | Default (none of the above match) |

Each type has a dedicated SVG icon used in Graphviz HTML labels and interactive
HTML diagrams.

---

## Reducing Large Diagrams

### Voltage-level bus reduction (`--voltage-threshold KV`)

Buses whose `voltage` field is strictly less than the threshold (kV) are
*lumped into* their nearest high-voltage neighbour via BFS through the line
network.  Lines between lumped buses are hidden; parallel lines that collapse
to the same HV bus pair are de-duplicated.

```bash
# Keep only buses ≥ 220 kV
gtopt_diagram large_case.json --voltage-threshold 220 -o hv_topo.svg
```

> Auto mode applies this automatically when the case has ≥ 1000 elements,
> choosing a threshold so that ≤ 64 representative buses remain.

### Type filter (`--filter-type`)

Show only generators of selected types:

```bash
# Only hydro generators
gtopt_diagram case.json --filter-type hydro -o hydro_only.svg

# Only solar and wind, aggregated per bus
gtopt_diagram case.json --filter-type solar wind --aggregate bus -o renewables.svg
```

### Geographic focus (`--focus-bus`, `--focus-hops`)

Show only buses and elements within N line-hops of named buses:

```bash
# 2-hop neighbourhood of bus 'Chapo220'
gtopt_diagram case.json --focus-bus Chapo220 -o chapo_area.svg

# 3-hop with hydro filter
gtopt_diagram case.json --focus-bus LagVerde220 --focus-hops 3 \
    --filter-type hydro -o lagverde_hydro.svg
```

### Top generators (`--top-gens N`)

Within each bus (or bus+type group), keep only the top-N generators by `pmax`,
summarising the rest:

```bash
gtopt_diagram case.json --top-gens 2 -o top2.svg
```

### Hard node cap (`--max-nodes N`)

If the estimated node count exceeds N, the aggregation mode is automatically
upgraded: `none → bus → type → global`:

```bash
gtopt_diagram case.json --max-nodes 80 -o compact.svg
```

### Compact labels (`--compact`)

Suppress pmax / gcost / reactance detail labels — show only names and counts:

```bash
gtopt_diagram case.json --aggregate type --compact -o compact.svg
```

### Hide isolated nodes (`--hide-isolated`)

Remove nodes that have no connections (e.g. buses with all generators filtered
out):

```bash
gtopt_diagram case.json --filter-type hydro --hide-isolated -o hydro_clean.svg
```

### Topology-only view (`--no-generators`)

Omit all generator nodes for a clean bus/line/demand view:

```bash
gtopt_diagram case.json --no-generators -o topo_only.svg
```

---

## All Options

```
positional arguments:
  json_file                  gtopt JSON planning file

Diagram type:
  -t, --diagram-type         topology (default) | planning

Output:
  -f, --format               svg (default) | png | pdf | dot | mermaid | html
  -o, --output               Output file path

Subsystem (topology only):
  -s, --subsystem            full (default) | electrical | hydro

Layout (Graphviz):
  -l, --layout               dot | neato | fdp | sfdp | circo | twopi (auto)
  --clusters                 Group electrical/hydro in Graphviz sub-clusters

Mermaid:
  -d, --direction            LR (default) | TD | BT | RL

Reduction options:
  -a, --aggregate            auto (default) | none | bus | type | global
  --no-generators            Omit all generator nodes (topology-only view)
  -g, --top-gens N           Top-N generators per bus by pmax (0 = all)
  --filter-type TYPE...      Show only: hydro solar wind thermal battery
  --focus-bus BUS...         Show only N-hop neighbourhood of these buses
  --focus-hops N             Hops for --focus-bus (default: 2)
  --max-nodes N              Hard cap: escalate aggregation to stay ≤ N nodes
  -V, --voltage-threshold KV Lump buses below KV into nearest HV neighbour
  --hide-isolated            Remove unconnected nodes
  --compact                  Omit pmax/gcost/reactance labels
```

---

## Examples

### Small case — IEEE 9-bus

```bash
# Auto mode selects 'none' (15 elements < 100 threshold)
gtopt_diagram cases/ieee_9b/ieee_9b.json -o ieee9b.svg

# Interactive HTML with physics simulation
gtopt_diagram cases/ieee_9b/ieee_9b.json --format html -o ieee9b.html

# Mermaid snippet for embedding in GitHub Markdown
gtopt_diagram cases/ieee_9b/ieee_9b.json --format mermaid
```

### Battery case — 4-bus with converter links

```bash
gtopt_diagram cases/bat_4b/bat_4b.json -o bat4b.svg
```

Shows battery–converter–generator/demand charge/discharge links.

### Planning time structure

```bash
gtopt_diagram cases/c0/system_c0.json --diagram-type planning -o c0_planning.svg
```

Shows the scenario → phase → stage → block hierarchy and the objective function.

### Large case — `gtopt_case_2y` (2 794 elements)

```bash
# Auto mode picks type + smart threshold (345 kV; 16 buses remain)
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json -o case2y_auto.svg

# Network topology only (no generators)
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --no-generators -o case2y_topo.svg

# HV backbone ≥ 220 kV, no generators
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --no-generators --voltage-threshold 220 -o case2y_hv_topo.svg

# Global summary — fastest overview
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --aggregate global --subsystem electrical --compact -o case2y_global.svg

# Hydro cascade only
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --subsystem hydro -o case2y_hydro.svg

# Interactive HTML for exploration
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --aggregate type --voltage-threshold 100 --compact \
    --format html -o case2y_interactive.html

# Focus on a local area (3 hops from a specific bus)
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --focus-bus "AltaRapel" --focus-hops 3 --aggregate type \
    --format html -o case2y_focus.html
```

### Generating the large case from PLP sources

```bash
plp2gtopt \
    -i scripts/cases/plp_case_2y \
    -o cases/gtopt_case_2y \
    -f cases/gtopt_case_2y/gtopt_case_2y.json \
    -n gtopt_case_2y \
    -s 24
```

---

## Guiservice Integration

The `guiservice` Flask application exposes the topology diagram directly in its
browser UI:

### REST API: `POST /api/diagram/topology`

Returns a vis.js-compatible JSON object for the current case data:

```json
{
  "caseData": { "..." },
  "subsystem": "full",
  "aggregate": "auto",
  "no_generators": false,
  "compact": false
}
```

### GUI Topology Tab

The browser GUI includes a **Topology** tab that renders an interactive
vis.js diagram.  Users can:
- Pan and zoom the network.
- Click nodes to see element details.
- Toggle generator visibility.
- Switch between electrical and hydro subsystems.

---

## See Also

- [SCRIPTS.md](../../SCRIPTS.md) — Overview of all gtopt Python scripts
- [DIAGRAM\_TOOL.md](../../DIAGRAM_TOOL.md) — Original diagram tool documentation
  with sample images
- [PLANNING\_GUIDE.md](../../PLANNING_GUIDE.md) — Guide to gtopt planning concepts
- [plp2gtopt](plp2gtopt.md) — Convert PLP cases (source of the large-case example)
- [pp2gtopt](pp2gtopt.md) — Convert pandapower networks (source of IEEE test cases)
