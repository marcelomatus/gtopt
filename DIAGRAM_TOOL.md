# gtopt-diagram — Network & Planning Diagram Tool

`gtopt-diagram` generates electrical network, hydro cascade, and
planning-time-structure diagrams from any **gtopt JSON planning file**.

---

## Installation

The tool is part of the `gtopt-scripts` package and is installed
alongside the other conversion utilities:

```bash
# From the repository root — install scripts package in editable mode
pip install -e "./scripts"

# With optional diagram-rendering dependencies (Graphviz Python bindings,
# interactive HTML via pyvis, SVG→PNG/PDF via cairosvg):
pip install -e "./scripts[diagram]"

# Graphviz system package is also required for SVG/PNG/PDF output:
sudo apt-get install graphviz          # Ubuntu / Debian
brew install graphviz                  # macOS
```

After installation the command is available on `$PATH`:

```bash
gtopt-diagram --help
```

---

## Quick Start

```bash
# Electrical topology — IEEE 9-bus test case (SVG, default)
gtopt-diagram cases/ieee_9b/ieee_9b.json -o ieee9b.svg

# Interactive HTML for the battery 4-bus case
gtopt-diagram cases/bat_4b/bat_4b.json --format html -o bat4b.html

# Hydro cascade only
gtopt-diagram cases/bat_4b/bat_4b.json --subsystem hydro -o hydro.svg

# Planning time structure
gtopt-diagram cases/c0/system_c0.json --diagram-type planning -o c0_planning.svg

# Mermaid snippet for GitHub Markdown
gtopt-diagram cases/ieee_9b/ieee_9b.json --format mermaid
```

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

## Output Formats (`--format`)

| Format | Description | Requires |
|--------|-------------|---------|
| `svg` | Scalable SVG via Graphviz (default) | `graphviz` system package + `pip install graphviz` |
| `png` | Rasterised PNG via Graphviz | same |
| `pdf` | PDF via Graphviz | same |
| `dot` | Raw Graphviz DOT source | — |
| `mermaid` | Mermaid flowchart source (embeds in GitHub Markdown) | — |
| `html` | Interactive vis.js browser diagram with physics simulation | `pip install pyvis` |

---

## Diagram Samples

### IEEE 9-bus electrical network

![IEEE 9-bus electrical](docs/diagrams/ieee9b_electrical.svg)

### Battery 4-bus (electrical, showing battery–converter links)

![bat4b electrical](docs/diagrams/bat4b_electrical.svg)

### Planning time structure (c0 5-stage expansion case)

![c0 planning](docs/diagrams/c0_planning.svg)

---

## Reduction Options for Large Cases

Real-world cases can have hundreds of buses and thousands of generators.
The following options make diagrams manageable.

### Generator aggregation (`--aggregate`)

| Mode | Description |
|------|-------------|
| `none` | Show every generator individually (default; best for small cases ≤ 30 buses) |
| `bus` | One summary node per bus: `"BusA generators\n12 units · 1 440 MW"` |
| `type` | One node per *(bus, type)* pair: `"☀️ Solar @ BusA\n3 units · 90 MW"` |
| `global` | One node per generator type for the whole system |

```bash
# Per-bus aggregation
gtopt-diagram case.json --aggregate bus -o case_bus.svg

# Per-(bus, type) aggregation
gtopt-diagram case.json --aggregate type -o case_types.svg

# Global summary (one node per type)
gtopt-diagram case.json --aggregate global -o case_global.svg
```

Generator types recognised: **hydro**, **solar**, **wind**, **battery**,
**thermal** (everything else). Detection is based on:
- Presence of a linked turbine in `turbine_array` → **hydro**
- Name keywords (`solar`, `pv`, `foto`, `eol`, `wind`, …) → **solar** / **wind**
- Name keywords (`bat`, `bess`, `ess`) → **battery**
- Otherwise → **thermal**

### Voltage-level bus reduction (`--voltage-threshold KV`)

Buses whose `voltage` field is **strictly less** than the threshold [kV] are
*lumped into* their nearest high-voltage neighbour via BFS through the line
network.  Lines between lumped buses are hidden; parallel lines that collapse to
the same HV bus pair are de-duplicated.

```bash
# Keep only buses ≥ 220 kV; fold everything below into the HV network
gtopt-diagram case.json --voltage-threshold 220 -o case_hv.svg

# Combine with type aggregation for cleaner output
gtopt-diagram case.json --aggregate type --voltage-threshold 220 -o case_hv_types.svg
```

### Type filter (`--filter-type`)

Show only generators of selected types.  Can be combined with any aggregation
mode.

```bash
# Only hydro generators (individual nodes)
gtopt-diagram case.json --filter-type hydro -o case_hydro_only.svg

# Only solar and wind, aggregated per bus
gtopt-diagram case.json --filter-type solar wind --aggregate bus -o case_renewables.svg
```

### Geographic focus (`--focus-bus`, `--focus-hops`)

Show only buses and elements within N line-hops of named buses.

```bash
# 2-hop neighbourhood of bus 'Chapo220' (default hops=2)
gtopt-diagram case.json --focus-bus Chapo220 -o chapo_area.svg

# 3-hop with hydro filter
gtopt-diagram case.json --focus-bus LagVerde220 --focus-hops 3 \
    --filter-type hydro -o lagverde_hydro.svg
```

### Top generators (`--top-gens N`)

Within each bus (or bus+type group), keep only the top-N generators by
`pmax`, summarising the rest.

```bash
gtopt-diagram case.json --aggregate bus --top-gens 5 -o case_top5.svg
```

### Auto-upgrade (`--max-nodes N`)

If the estimated node count exceeds N, the aggregation mode is automatically
upgraded: `none → bus → type → global`.

```bash
# Never produce more than 80 nodes; let the tool decide the aggregation
gtopt-diagram case.json --max-nodes 80 -o case_auto.svg
```

### Compact labels (`--compact`)

Suppress pmax / gcost / reactance detail labels, showing only names and counts.
Useful when printing large diagrams.

```bash
gtopt-diagram case.json --aggregate type --compact -o case_compact.svg
```

### Hide isolated nodes (`--hide-isolated`)

Remove nodes that have no connections (e.g., buses that ended up with all their
generators filtered out).

---

## Large Case Example: `gtopt_case_2y`

The `cases/gtopt_case_2y/` case was converted from the PLP `plp_case_2y`
directory using `plp2gtopt`.  It contains:

| Element | Count |
|---------|-------|
| Buses | 236 (voltages: 1–1220 kV) |
| Generators | 1 740 (thermal, hydro, solar) |
| Demands | 163 |
| Lines | 330 |
| Batteries | 25 |
| Junctions (hydro) | 85 |
| Reservoirs | 10 |
| Turbines | 31 |
| Waterways | 113 |
| Stages | 24 |

### Recommended workflows

```bash
# 1. Global summary — best overview, very fast
gtopt-diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --aggregate global --subsystem electrical \
    --compact --format svg -o case2y_global.svg

# 2. High-voltage network (≥ 220 kV) with type aggregation
gtopt-diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --aggregate type --voltage-threshold 220 \
    --compact --format svg -o case2y_hv220.svg

# 3. Hydro cascade
gtopt-diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --subsystem hydro --format svg -o case2y_hydro.svg

# 4. Interactive HTML — explore all 236 buses with physics simulation
gtopt-diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --aggregate type --voltage-threshold 100 --compact \
    --format html -o case2y_interactive.html

# 5. Planning structure (24 stages, 1 scenario)
gtopt-diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --diagram-type planning -o case2y_planning.svg

# 6. Focus on a local area (2 hops from a specific bus)
gtopt-diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --focus-bus "AltaRapel" --focus-hops 3 --aggregate type \
    --format html -o case2y_focus.html
```

### Generating the case from PLP sources

```bash
plp2gtopt \
    -i scripts/cases/plp_case_2y \
    -o cases/gtopt_case_2y \
    -f cases/gtopt_case_2y/gtopt_case_2y.json \
    -n gtopt_case_2y \
    -s 24          # first 24 stages
```

---

## Generator Type Icons

Each generator type has a dedicated SVG icon used in Graphviz HTML labels
and interactive HTML diagrams.

| Type | Icon | Detection |
|------|------|-----------|
| Hydro | Dam + turbine | Linked turbine in `turbine_array` |
| Solar | Sun rays + PV panel | Name: `solar`, `pv`, `foto`, `fotov` |
| Wind | Turbine tower + 3 blades | Name: `wind`, `eol`, `eólico` |
| Thermal | Generator + chimney | Default (none of the above) |
| Battery / BESS | Battery with charge bars | Name: `bat`, `bess`, `ess` |
| Nuclear | Radiation symbol | Name: `nuclear`, `nuc` |
| Gas turbine | Axial turbine | Name: `gas`, `tg`, `ccgt` |

---

## All Options Reference

```
usage: gtopt-diagram [-h] [--diagram-type {topology,planning}]
                     [--format {dot,png,svg,pdf,mermaid,html}]
                     [--output OUTPUT] [--subsystem {full,electrical,hydro}]
                     [--layout {dot,neato,fdp,sfdp,circo,twopi}]
                     [--direction {LR,TD,BT,RL}] [--clusters]
                     [--aggregate MODE] [--top-gens N]
                     [--filter-type TYPE [TYPE ...]]
                     [--focus-bus BUS [BUS ...]] [--focus-hops N]
                     [--max-nodes N] [--voltage-threshold KV]
                     [--hide-isolated] [--compact]
                     [json_file]

Diagram type:
  --diagram-type, -t     topology (default) | planning

Output:
  --format, -f           svg (default) | png | pdf | dot | mermaid | html
  --output, -o           Output file path

Subsystem (topology):
  --subsystem, -s        full (default) | electrical | hydro

Layout (Graphviz):
  --layout, -l           dot | neato | fdp | sfdp | circo | twopi (auto)
  --clusters             Group electrical/hydro in sub-clusters

Mermaid:
  --direction, -d        LR (default) | TD | BT | RL

Reduction:
  --aggregate, -a        none (default) | bus | type | global
  --top-gens, -g N       Top-N generators per bus by pmax (0=all)
  --filter-type TYPE...  Show only: hydro solar wind thermal battery
  --focus-bus BUS...     Show only N-hop neighbourhood of these buses
  --focus-hops N         Hops for --focus-bus (default: 2)
  --max-nodes N          Auto-upgrade aggregation to stay ≤ N nodes
  --voltage-threshold KV Lump buses below KV into nearest HV neighbour
  --hide-isolated        Remove unconnected nodes
  --compact              Omit pmax/gcost/reactance labels
```
