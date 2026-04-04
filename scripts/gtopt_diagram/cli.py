# SPDX-License-Identifier: BSD-3-Clause
"""Command-line interface for gtopt_diagram.

This module contains the ``main()`` entry-point that parses arguments, loads
the JSON planning file, dispatches to the appropriate diagram builder, and
writes the output.
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
import textwrap
from pathlib import Path

from gtopt_diagram._graph_model import FilterOptions
from gtopt_diagram._planning_diagram import (
    _build_planning_html,
    _build_planning_mermaid,
    _build_planning_svg,
)
from gtopt_diagram._renderers import (
    _auto_layout,
    _show_mermaid,
    display_diagram,
    render_graphviz,
    render_html,
    render_mermaid,
)
from gtopt_diagram._svg_constants import _PALETTE, _PALETTE_COLORBLIND

logger = logging.getLogger(__name__)


def main(argv: list[str] | None = None) -> int:  # noqa: C901,PLR0912,PLR0915
    parser = argparse.ArgumentParser(
        prog="gtopt_diagram",
        description=(
            "Generate electrical, hydro, and planning-structure diagrams "
            "from a gtopt JSON planning file."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
Diagram types (--diagram-type):
  topology   Network topology (buses, generators, lines, hydro elements, …)
  planning   Conceptual time-structure (blocks / stages / phases / scenarios)

Subsystems (topology, --subsystem):
  full        All elements together (default)
  electrical  Buses, generators, demands, lines, batteries, converters
  hydro       Junctions, waterways, reservoirs, turbines, flows, seepages

Output formats (--format):
  svg, png, pdf, dot   Graphviz-rendered static images (requires graphviz)
  mermaid              Mermaid source for GitHub Markdown
  html                 Interactive vis.js browser diagram (requires pyvis)

Aggregation modes (--aggregate):
  auto    [DEFAULT] Automatically choose based on total element count:
            < 100  elements → none   (show everything individually)
            100-999 elements → bus   (one summary node per bus)
            ≥ 1000 elements  → type + --voltage-threshold 200
                               (aggressive: per-type nodes + fold LV buses)
  none    Show every generator individually (best for small cases)
  bus     One summary node per bus (counts + total MW)
  type    One node per (bus, generator-type) pair with type icon
  global  One node per generator type for the whole system

Generator visibility:
  --no-generators     Show only network topology (buses, lines, demands,
                      hydro elements) — no generator nodes at all.
                      Useful for large networks where generators clutter the view.

Other reduction options:
  --top-gens N        Keep only the top-N generators per bus by pmax
  --filter-type TYPE  Show only generators of TYPE (hydro/solar/wind/thermal/battery)
  --focus-bus NAME    Show only buses within N hops of NAME (repeat for multiple)
  --focus-hops N      Number of hops for --focus-bus (default: 2)
  --max-nodes N       Hard cap: escalate aggregation until node count ≤ N
  --voltage-threshold V   Lump buses and lines below V kV into their nearest
                          high-voltage neighbour (e.g. --voltage-threshold 200)
  --hide-isolated     Remove nodes with no connections
  --compact           Omit detail labels (show only names / counts)

Examples:
  # Auto mode (default) — picks the right aggregation for case size
  gtopt_diagram cases/ieee_9b/ieee_9b.json -o ieee9b.svg          # <100: none
  gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json -o c2y.svg # ≥1000: type+smart threshold

  # Topology-only (no generators) — clean network diagram
  gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json --no-generators -o topo.svg

  # Interactive HTML — explore with physics simulation
  gtopt_diagram cases/ieee_9b/ieee_9b.json --format html -o ieee9b.html

  # Force explicit mode
  gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
      --aggregate type --voltage-threshold 220 --format html -o case2y.html

  # Show only hydro generators in 2-hop neighbourhood of a bus
  gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
      --filter-type hydro --focus-bus Chapo220 --focus-hops 3 --format svg

  # Planning structure from a real case
  gtopt_diagram cases/c0/system_c0.json --diagram-type planning --format html

  # Mermaid snippet for GitHub Markdown
  gtopt_diagram cases/bat_4b/bat_4b.json --format mermaid
        """),
    )
    parser.add_argument(
        "json_file",
        nargs="?",
        help="gtopt JSON planning file (optional for planning diagrams)",
    )
    parser.add_argument(
        "--diagram-type",
        "-t",
        choices=["topology", "planning"],
        default="topology",
        help="Diagram type (default: topology)",
    )
    parser.add_argument(
        "--format",
        "-f",
        choices=["dot", "png", "svg", "pdf", "mermaid", "html"],
        default="svg",
        help="Output format (default: svg)",
    )
    parser.add_argument(
        "--output",
        "-o",
        default=None,
        help="Output file path (default: <case>_<type>.<ext>)",
    )
    parser.add_argument(
        "--subsystem",
        "-s",
        choices=["full", "electrical", "hydro"],
        default="full",
        help="Subsystem for topology diagrams (default: full)",
    )
    parser.add_argument(
        "--layout",
        "-L",
        choices=["dot", "neato", "fdp", "sfdp", "circo", "twopi"],
        default=None,
        help="Graphviz layout engine (auto-selected if omitted)",
    )
    parser.add_argument(
        "--direction",
        "-d",
        choices=["LR", "TD", "BT", "RL"],
        default="LR",
        help="Mermaid flowchart direction (default: LR)",
    )
    parser.add_argument(
        "--clusters",
        action="store_true",
        default=False,
        help="Group electrical/hydro in Graphviz sub-clusters (full topology)",
    )

    # ── Reduction / aggregation options ──��──────────────────────────────────
    red = parser.add_argument_group(
        "reduction",
        "Options for simplifying large diagrams with many elements",
    )
    red.add_argument(
        "--aggregate",
        "-a",
        choices=["auto", "none", "bus", "type", "global"],
        default="auto",
        metavar="MODE",
        help=(
            "Aggregation mode (default: auto). "
            "auto=smart selection by element count (<100: none, 100-999: bus, "
            "≥1000: type+smart voltage threshold). "
            "none=individual elements, bus=per-bus summary, "
            "type=per-(bus,type) node, global=one node per type"
        ),
    )
    red.add_argument(
        "--no-generators",
        action="store_true",
        default=False,
        dest="no_generators",
        help=(
            "Omit all generator nodes — show only network topology "
            "(buses, lines, demands, hydro elements). "
            "Useful for very large cases where generators clutter the diagram."
        ),
    )
    red.add_argument(
        "--top-gens",
        "-g",
        type=int,
        default=0,
        metavar="N",
        help="Keep only the top-N generators per bus by pmax (0 = all)",
    )
    red.add_argument(
        "--filter-type",
        nargs="+",
        default=[],
        metavar="TYPE",
        dest="filter_types",
        help="Show only generators of these types: hydro solar wind thermal battery",
    )
    red.add_argument(
        "--focus-bus",
        nargs="+",
        default=[],
        metavar="BUS",
        dest="focus_buses",
        help="Show only elements reachable from these bus names",
    )
    red.add_argument(
        "--focus-generator",
        nargs="+",
        default=[],
        metavar="GEN",
        dest="focus_generators",
        help="Focus on the bus(es) connected to these generators (by name or uid)",
    )
    red.add_argument(
        "--focus-area",
        type=float,
        default=0.0,
        metavar="KV",
        help="Focus on buses at or above this voltage level (kV)",
    )
    red.add_argument(
        "--focus-hops",
        type=int,
        default=2,
        metavar="N",
        help="Number of line hops for --focus-bus (default: 2)",
    )
    red.add_argument(
        "--max-nodes",
        type=int,
        default=0,
        metavar="N",
        help="Auto-upgrade aggregation mode until node count ≤ N (0 = disabled)",
    )
    red.add_argument(
        "--voltage-threshold",
        type=float,
        default=0.0,
        metavar="KV",
        help=(
            "Lump buses below this voltage [kV] into their nearest HV neighbour. "
            "Lines between lumped buses are hidden. Buses without a voltage field "
            "are never lumped. (0 = disabled, e.g. --voltage-threshold 200)"
        ),
    )
    red.add_argument(
        "--hide-isolated",
        action="store_true",
        default=False,
        help="Remove nodes with no connections from the diagram",
    )
    red.add_argument(
        "--compact",
        action="store_true",
        default=False,
        help="Show only names/counts; omit pmax/gcost/reactance detail labels",
    )
    red.add_argument(
        "--show",
        action="store_true",
        default=False,
        help=(
            "Open the output file in a viewer after writing it. "
            "Uses Pillow (PIL) for PNG; webbrowser for SVG, PDF, HTML and Mermaid. "
            "Ignored for dot format written to stdout. "
            "Enabled automatically when no --output path is given."
        ),
    )
    parser.add_argument(
        "--palette",
        choices=["default", "colorblind"],
        default="default",
        help="Color palette for diagram elements (default: %(default)s)",
    )
    parser.add_argument(
        "--scenario",
        type=int,
        default=1,
        metavar="UID",
        help="Scenario UID for resolving file-referenced values (default: 1)",
    )
    parser.add_argument(
        "--stage",
        type=int,
        default=1,
        metavar="UID",
        help="Stage UID for resolving file-referenced values (default: 1)",
    )
    parser.add_argument(
        "--block",
        type=int,
        default=1,
        metavar="UID",
        help="Block UID for resolving file-referenced values (default: 1)",
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        metavar="LEVEL",
        help=(
            "logging verbosity: DEBUG, INFO, WARNING, ERROR, CRITICAL "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version="%(prog)s (gtopt-scripts)",
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        default=False,
        help="Disable coloured output.",
    )
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    # ── Load JSON ─────────────��─────────────────────────���────────────────────
    planning: dict = {}
    case_name = "gtopt"
    if args.json_file:
        jp = Path(args.json_file)
        if not jp.exists():
            logger.error("File not found: %s", jp)
            return 1
        with jp.open(encoding="utf-8") as fh:
            planning = json.load(fh)
        case_name = planning.get("system", {}).get("name", jp.stem)

    fmt = args.format
    sfx = {
        "dot": ".dot",
        "png": ".png",
        "svg": ".svg",
        "pdf": ".pdf",
        "mermaid": ".md",
        "html": ".html",
    }.get(fmt, f".{fmt}")
    out = args.output or f"{case_name}_{args.diagram_type}{sfx}"

    # Auto-enable show when no explicit output path was given (except for
    # text formats that are written to stdout, where there is nothing to open).
    show = args.show or (args.output is None and fmt not in ("mermaid", "dot"))

    # ── Planning structure diagram ───��───────────────────────────────────────
    if args.diagram_type == "planning":
        if fmt == "mermaid":
            result = _build_planning_mermaid(planning)
            if args.output:
                Path(out).write_text(result, encoding="utf-8")
                logger.info("Mermaid planning diagram written to %s", out)
            if show:
                _show_mermaid(result, title=f"{case_name} — Planning Structure")
            elif not args.output:
                print(result)
            return 0
        if fmt == "html":
            ttl = (
                f"{case_name} — Planning Structure"
                if case_name != "gtopt"
                else "gtopt Planning Structure"
            )
            Path(out).write_text(
                _build_planning_html(planning, title=ttl), encoding="utf-8"
            )
            logger.info("Planning HTML written to %s", out)
            if show:
                display_diagram(out, fmt)
            return 0
        svg_src = _build_planning_svg(planning)
        if fmt in ("svg", "dot"):
            Path(out).write_text(svg_src, encoding="utf-8")
        else:
            try:
                import cairosvg  # noqa: PLC0415

                if fmt == "png":
                    cairosvg.svg2png(
                        bytestring=svg_src.encode(), write_to=out, scale=2.0
                    )
                elif fmt == "pdf":
                    cairosvg.svg2pdf(bytestring=svg_src.encode(), write_to=out)
                else:
                    Path(out).write_text(svg_src, encoding="utf-8")
            except ImportError:
                logger.warning("cairosvg not found; writing SVG instead")
                out = out.rsplit(".", 1)[0] + ".svg"
                Path(out).write_text(svg_src, encoding="utf-8")
        logger.info("Planning diagram written to %s", out)
        if show:
            display_diagram(out, fmt)
        return 0

    # ��─ Topology diagram ─────────────────��───────────────────────────────────

    # Import TopologyBuilder here to avoid circular imports
    from gtopt_diagram.gtopt_diagram import TopologyBuilder  # noqa: PLC0415

    # Resolve --focus-generator to bus names
    extra_focus_buses: list[str] = list(args.focus_buses)
    sys_data = planning.get("system", {})
    if args.focus_generators:
        gen_array = sys_data.get("generator_array", [])
        gen_lookup: dict[str, dict] = {}
        for g in gen_array:
            gname = g.get("name")
            gid = g.get("uid")
            if gname is not None:
                gen_lookup[str(gname)] = g
            if gid is not None:
                gen_lookup[str(gid)] = g
        for gen_ref in args.focus_generators:
            g = gen_lookup.get(gen_ref)
            if g is None:
                logger.warning("--focus-generator: generator %r not found", gen_ref)
                continue
            bus_ref = g.get("bus")
            if bus_ref is not None:
                # Resolve bus uid to bus name
                bus_name = str(bus_ref)
                for bus in sys_data.get("bus_array", []):
                    if bus.get("uid") == bus_ref or str(bus.get("name")) == str(
                        bus_ref
                    ):
                        bus_name = str(bus.get("name", bus_ref))
                        break
                extra_focus_buses.append(bus_name)

    # Resolve --focus-area to bus names (buses at or above the given kV)
    if args.focus_area > 0:
        for bus in sys_data.get("bus_array", []):
            kv = bus.get("kv") or bus.get("voltage") or 0
            if float(kv) >= args.focus_area:
                bname = bus.get("name")
                if bname is not None:
                    extra_focus_buses.append(str(bname))

    # Apply --palette selection
    if args.palette == "colorblind":
        # Swap the module-level _PALETTE with colorblind variant
        _PALETTE.update(_PALETTE_COLORBLIND)

    # Print summary statistics if gtopt_check_json is available
    try:
        from gtopt_check_json._info import format_indicators  # noqa: PLC0415

        stats = format_indicators(
            planning,
            base_dir=str(Path(args.json_file).parent) if args.json_file else None,
        )
        print(stats, file=sys.stderr)
    except ImportError:
        pass

    opts = FilterOptions(
        aggregate=args.aggregate,
        no_generators=args.no_generators,
        top_gens=args.top_gens,
        filter_types=[t.lower() for t in args.filter_types],
        focus_buses=extra_focus_buses,
        focus_hops=args.focus_hops,
        max_nodes=args.max_nodes,
        voltage_threshold=args.voltage_threshold,
        hide_isolated=args.hide_isolated,
        compact=args.compact,
    )
    # Create resolver for file-referenced field values (pmax, lmax, etc.)
    resolver = None
    input_path = getattr(args, "input", None)
    if input_path:
        base_dir = str(Path(input_path).parent)
        input_dir = planning.get("options", {}).get("input_directory", ".")
        try:
            from gtopt_diagram._field_resolver import (  # noqa: PLC0415
                FieldSchedResolver,
            )

            resolver = FieldSchedResolver(
                base_dir=base_dir,
                input_dir=input_dir,
                scenario=getattr(args, "scenario", 1),
                stage=getattr(args, "stage", 1),
                block=getattr(args, "block", 1),
            )
        except Exception:  # pylint: disable=broad-except
            logger.debug("FieldSchedResolver not available; file references unresolved")

    builder = TopologyBuilder(
        planning, subsystem=args.subsystem, opts=opts, resolver=resolver
    )
    model = builder.build()

    n_nodes = len(model.nodes)
    n_edges = len(model.edges)
    if n_nodes == 0:
        logger.warning("No elements found for the requested subsystem / filters.")
    else:
        agg_used = builder.eff_agg
        vt_used = builder.eff_vthresh
        flags: list[str] = []
        if agg_used != "none":
            flags.append(f"aggregate={agg_used}")
        if vt_used > 0:
            flags.append(f"voltage\u2265{vt_used:.0f} kV")
        if opts.no_generators:
            flags.append("no-generators")
        if opts.aggregate == "auto" and builder.auto_info:
            n_total, _, _ = builder.auto_info
            flags.append(f"auto({n_total} elements)")
        suffix = ("  [" + ", ".join(flags) + "]") if flags else ""
        logger.info("Diagram: %d nodes, %d edges%s", n_nodes, n_edges, suffix)

    if fmt == "mermaid":
        result = render_mermaid(model, direction=args.direction)
        if args.output:
            Path(out).write_text(result, encoding="utf-8")
            logger.info("Mermaid topology written to %s", out)
        if show:
            _show_mermaid(result, title=model.title)
        elif not args.output:
            print(result)
        return 0

    if fmt == "html":
        render_html(model, output_path=out)
        logger.info("Interactive HTML written to %s", out)
        if show:
            display_diagram(out, fmt)
        return 0

    layout = args.layout or _auto_layout(model, args.subsystem)
    clusters = args.clusters and args.subsystem == "full"

    if fmt == "dot":
        src = render_graphviz(model, fmt="dot", layout=layout, use_clusters=clusters)
        if args.output:
            Path(out).write_text(src, encoding="utf-8")
            logger.info("DOT source written to %s", out)
        else:
            print(src)
        return 0

    rendered = render_graphviz(
        model, fmt=fmt, output_path=out, layout=layout, use_clusters=clusters
    )
    logger.info("Diagram written to %s", rendered)
    if show:
        display_diagram(rendered, fmt)
    return 0
