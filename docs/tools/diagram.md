# gtopt_diagram — Network & Planning Diagram Tool

`gtopt_diagram` generates electrical network, hydro cascade, and
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
gtopt_diagram --help
```

---

## Quick Start

```bash
# Auto mode (DEFAULT) — the tool picks the right strategy for the case size
gtopt_diagram cases/ieee_9b/ieee_9b.json -o ieee9b.svg        # small: all individual
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json -o c2y.svg  # large: type+smart threshold (e.g. 345 kV for 2y)

# Interactive HTML for the battery 4-bus case
gtopt_diagram cases/bat_4b/bat_4b.json --format html -o bat4b.html

# Network topology only — no generators (clean bus/line view)
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json --no-generators -o topo.svg

# Hydro cascade only
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json --subsystem hydro -o hydro.svg

# Planning time structure
gtopt_diagram cases/c0/system_c0.json --diagram-type planning -o c0_planning.svg

# Mermaid snippet for GitHub Markdown
gtopt_diagram cases/ieee_9b/ieee_9b.json --format mermaid
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

### IEEE 9-bus electrical network (auto mode → individual elements)

![IEEE 9-bus electrical](../diagrams/ieee9b_electrical.svg)

### Battery 4-bus (electrical, showing battery–converter links)

![bat4b electrical](../diagrams/bat4b_electrical.svg)

### Planning time structure (c0 5-stage expansion case)

![c0 planning](../diagrams/c0_planning.svg)

---

## Examples by Case Type

### Small IEEE cases (< 100 elements)

These cases use `--aggregate none` (auto default) showing every element
individually:

```bash
# IEEE 4-bus – the simplest multi-bus case (4 buses, 2 generators, 5 lines)
gtopt_diagram cases/ieee_4b_ori/ieee_4b_ori.json -o ieee4b.svg

# IEEE 9-bus original – classic OPF benchmark (9 buses, 3 generators)
gtopt_diagram cases/ieee_9b_ori/ieee_9b_ori.json -o ieee9b.svg

# IEEE 9-bus with 24-hour solar profile
gtopt_diagram cases/ieee_9b/ieee_9b.json -o ieee9b_solar.svg

# IEEE 14-bus – standard 14-bus benchmark
gtopt_diagram cases/ieee_14b_ori/ieee_14b_ori.json -o ieee14b.svg
```

### Medium IEEE cases (100–999 elements)

```bash
# IEEE 30-bus – auto selects per-bus aggregation
gtopt_diagram cases/ieee30b/ieee30b.json -o ieee30b.svg

# IEEE 57-bus – 57 buses, 7 generators, 80 lines
gtopt_diagram cases/ieee_57b/ieee_57b.json -o ieee57b.svg

# Force individual elements for IEEE 57-bus (more detailed but busier)
gtopt_diagram cases/ieee_57b/ieee_57b.json --aggregate none -o ieee57b_full.svg

# Interactive HTML for IEEE 30-bus exploration
gtopt_diagram cases/ieee30b/ieee30b.json --format html -o ieee30b.html
```

### Battery and storage cases

```bash
# 4-bus with battery storage – shows battery, converter, generator, and demand
gtopt_diagram cases/bat_4b/bat_4b.json -o bat4b.svg

# 24-hour battery dispatch – same network, hourly blocks
gtopt_diagram cases/bat_4b_24/bat_4b_24.json -o bat4b_24.svg
```

### Expansion planning cases

```bash
# Battery expansion – shows expandable battery + converter links
gtopt_diagram cases/exp_bat_4b/exp_bat_4b.json -o exp_bat4b.svg

# 24-hour battery expansion variant
gtopt_diagram cases/exp_bat_4b_24/exp_bat_4b_24.json -o exp_bat4b_24.svg

# Generator expansion – shows expandable generators
gtopt_diagram cases/exp_gen_4b/exp_gen_4b.json -o exp_gen4b.svg

# Planning diagram for multi-stage expansion (5 stages)
gtopt_diagram cases/c0/system_c0.json --diagram-type planning -o c0_planning.svg
```

### SDDP and multi-phase cases

```bash
# SDDP hydro with 3 phases – planning structure
gtopt_diagram cases/sddp_hydro_3phase/sddp_hydro_3phase.json \
    --diagram-type planning -o sddp_planning.svg

# SDDP hydro topology (hydro subsystem only)
gtopt_diagram cases/sddp_hydro_3phase/sddp_hydro_3phase.json \
    --subsystem hydro -o sddp_hydro.svg
```

### Mermaid diagrams (embeddable in Markdown)

Mermaid output requires no external dependencies and can be pasted directly
into GitHub Markdown files:

```bash
# Generate a Mermaid snippet for an IEEE 9-bus case
gtopt_diagram cases/ieee_9b_ori/ieee_9b_ori.json --format mermaid

# Planning structure as Mermaid (top-to-bottom direction)
gtopt_diagram cases/c0/system_c0.json --diagram-type planning \
    --format mermaid --direction TD
```

Paste the output into a Markdown code fence:

````markdown
```mermaid
graph LR
    bus_1((b1))
    bus_2((b2))
    ...
```
````

---

## Reduction Options for Large Cases

Real-world cases can have hundreds of buses and thousands of generators.
The tool uses **`auto` mode by default** to automatically apply the right level
of reduction.

### Auto mode (`--aggregate auto`, the default)

The default mode selects a strategy based on the **total element count** of the
case (generators + buses + demands + lines + hydro elements + …):

| Total elements | Strategy applied |
|----------------|-----------------|
| **< 100** | `none` — show every element individually |
| **100 – 999** | `bus` — one summary node per bus |
| **≥ 1000** | `type` + **smart voltage threshold** — aggregate by type; threshold auto-computed to keep ≤ 64 representative buses |

The status line printed to stderr always shows what was chosen:

```
Diagram: 15 nodes, 15 edges                          # <100: none (ieee9b)
Diagram: 605 nodes, 727 edges  [aggregate=type, voltage≥345 kV, auto(2794 elements)]
```

To override auto mode, use one of the explicit values:

```bash
# Force individual elements even for a large case
gtopt_diagram big_case.json --aggregate none -o big_full.svg

# Force per-bus summary
gtopt_diagram big_case.json --aggregate bus -o big_bus.svg
```

### Topology-only view (`--no-generators`)

Sometimes you want to inspect the **network structure** (buses, lines, demands,
hydro cascade) without the clutter of generator nodes.  Use `--no-generators`:

```bash
# Network structure only — no generators
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --no-generators -o topo.svg

# Combine with voltage threshold for the HV backbone
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --no-generators --voltage-threshold 220 -o topo_hv220.svg
```

### Generator aggregation (`--aggregate`)

| Mode | Description |
|------|-------------|
| `auto` | Smart automatic selection (default) |
| `none` | Show every generator individually (best for small cases ≤ ~50 nodes) |
| `bus` | One summary node per bus: `"BusA generators\n12 units · 1 440 MW"` |
| `type` | One node per *(bus, type)* pair: `"☀️ Solar @ BusA\n3 units · 90 MW"` |
| `global` | One node per generator type for the whole system |

```bash
# Per-bus aggregation
gtopt_diagram case.json --aggregate bus -o case_bus.svg

# Per-(bus, type) aggregation
gtopt_diagram case.json --aggregate type -o case_types.svg

# Global summary (one node per type)
gtopt_diagram case.json --aggregate global -o case_global.svg
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

> **Auto mode applies this automatically** with a threshold chosen so that ≤ 64
> representative buses remain when the case has ≥ 1000 elements.
> For the `gtopt_case_2y` case this resolves to 345 kV (16 buses remain).

```bash
# Keep only buses ≥ 220 kV; fold everything below into the HV network
gtopt_diagram case.json --voltage-threshold 220 -o case_hv.svg

# Combine with type aggregation for cleaner output
gtopt_diagram case.json --aggregate type --voltage-threshold 220 -o case_hv_types.svg
```

### Type filter (`--filter-type`)

Show only generators of selected types.  Can be combined with any aggregation
mode.

```bash
# Only hydro generators (individual nodes)
gtopt_diagram case.json --filter-type hydro -o case_hydro_only.svg

# Only solar and wind, aggregated per bus
gtopt_diagram case.json --filter-type solar wind --aggregate bus -o case_renewables.svg
```

### Geographic focus (`--focus-bus`, `--focus-hops`)

Show only buses and elements within N line-hops of named buses.

```bash
# 2-hop neighbourhood of bus 'Chapo220' (default hops=2)
gtopt_diagram case.json --focus-bus Chapo220 -o chapo_area.svg

# 3-hop with hydro filter
gtopt_diagram case.json --focus-bus LagVerde220 --focus-hops 3 \
    --filter-type hydro -o lagverde_hydro.svg
```

### Top generators (`--top-gens N`)

Within each bus (or bus+type group), keep only the top-N generators by
`pmax`, summarising the rest.

```bash
gtopt_diagram case.json --aggregate bus --top-gens 5 -o case_top5.svg
```

### Hard node cap (`--max-nodes N`)

If the estimated node count exceeds N, the aggregation mode is automatically
upgraded: `none → bus → type → global`.

```bash
# Never produce more than 80 nodes; let the tool decide the aggregation
gtopt_diagram case.json --max-nodes 80 -o case_auto.svg
```

### Compact labels (`--compact`)

Suppress pmax / gcost / reactance detail labels, showing only names and counts.
Useful when printing large diagrams.

```bash
gtopt_diagram case.json --aggregate type --compact -o case_compact.svg
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

With 2 794 total elements the auto mode selects `type` + smart voltage threshold
(345 kV, keeping 16 buses) automatically — no flags needed:

```bash
# Auto mode picks type + smart threshold (345 kV here; ≤ 64 buses remain)
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json -o case2y_auto.svg
```

### Recommended workflows

```bash
# 0. Auto mode (default — no options needed)
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json -o case2y_auto.svg

# 1. Network topology only — no generators
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --no-generators -o case2y_topo.svg

# 2. HV backbone only (≥ 220 kV), no generators
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --no-generators --voltage-threshold 220 -o case2y_hv_topo.svg

# 3. Global summary — best overview, very fast
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --aggregate global --subsystem electrical \
    --compact --format svg -o case2y_global.svg

# 4. High-voltage network (≥ 220 kV) with type aggregation
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --aggregate type --voltage-threshold 220 \
    --compact --format svg -o case2y_hv220.svg

# 5. Hydro cascade
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --subsystem hydro --format svg -o case2y_hydro.svg

# 6. Interactive HTML — explore all 236 buses with physics simulation
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --aggregate type --voltage-threshold 100 --compact \
    --format html -o case2y_interactive.html

# 7. Planning structure (24 stages, 1 scenario)
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
    --diagram-type planning -o case2y_planning.svg

# 8. Focus on a local area (2 hops from a specific bus)
gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \
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
usage: gtopt_diagram [-h] [--diagram-type {topology,planning}]
                     [--format {dot,png,svg,pdf,mermaid,html}]
                     [--output OUTPUT] [--subsystem {full,electrical,hydro}]
                     [--layout {dot,neato,fdp,sfdp,circo,twopi}]
                     [--direction {LR,TD,BT,RL}] [--clusters]
                     [--aggregate MODE] [--no-generators] [--top-gens N]
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
  --aggregate, -a        auto (default) | none | bus | type | global
                         auto: <100 → none, 100-999 → bus,
                               ≥1000 → type+smart threshold (≤64 buses)
  --no-generators        Omit all generator nodes (topology-only view)
  --top-gens, -g N       Top-N generators per bus by pmax (0=all)
  --filter-type TYPE...  Show only: hydro solar wind thermal battery
  --focus-bus BUS...     Show only N-hop neighbourhood of these buses
  --focus-hops N         Hops for --focus-bus (default: 2)
  --max-nodes N          Hard cap: escalate aggregation to stay ≤ N nodes
  --voltage-threshold KV Lump buses below KV into nearest HV neighbour
  --hide-isolated        Remove unconnected nodes
  --compact              Omit pmax/gcost/reactance labels
```

---

## Guiservice Integration (interactive browser UI)

The `guiservice` Flask application exposes the topology diagram directly
in its browser UI through:

### REST API: `POST /api/diagram/topology`

Returns a vis.js-compatible JSON object for the current case data.

```json
// Request body
{
  "caseData":      { ... },          // GUI caseData object
  "subsystem":     "full",           // "full" | "electrical" | "hydro"
  "aggregate":     "auto",           // aggregation mode
  "no_generators": false,            // true to hide generators
  "compact":       false             // true for compact labels
}

// Response
{
  "nodes": [ { "id": "bus_1", "label": "b1", "kind": "bus", ... } ],
  "edges": [ { "from": "bus_1", "to": "bus_2", ... } ],
  "meta":  { "aggregate": "none", "voltage_threshold": 0, "n_nodes": 15, ... }
}
```

### Topology Tab in the GUI

The GUI has a dedicated **🗺 Topology** tab in the "Case Setup" sidebar
section.  It uses [vis-network](https://visjs.github.io/vis-network/) for
interactive exploration:

- **Pan / zoom** — drag the canvas, scroll to zoom
- **Click a node** — opens a floating popup with node details
  (name, type, voltage, pmax, connected elements, …)
- **Double-click canvas** — fits the view to all nodes
- **Controls** — Subsystem selector, Aggregation selector,
  "Hide generators" checkbox, "Compact labels" checkbox, Render/Fit buttons

The topology tab automatically renders when first opened.  The **smart auto
mode** (default) selects the most appropriate aggregation and voltage
threshold based on the case size, so the diagram is readable regardless
of whether the case has 15 nodes or thousands.

