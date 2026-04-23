# SPDX-License-Identifier: BSD-3-Clause
"""Rendering backends: Graphviz, Mermaid, vis.js/pyvis HTML, image viewer.

This module provides functions to convert a :class:`GraphModel` into various
output formats (SVG, PNG, PDF, DOT, Mermaid, interactive HTML) and display
the results.
"""

from __future__ import annotations

import os
import tempfile
import textwrap
import webbrowser
from pathlib import Path
from typing import Optional

from gtopt_diagram._graph_model import GraphModel, Node
from gtopt_diagram._svg_constants import (
    _FA_CDN,
    _GV_SHAPE_MAP,
    _LAYOUT_DOT_THRESHOLD,
    _LAYOUT_FDP_THRESHOLD,
    _LAYOUT_NEATO_THRESHOLD,
    _LEGEND_LABELS,
    _MM_ICONS,
    _MM_SHAPES,
    _MM_STYLES,
    _PALETTE,
    _PYVIS_COLORS,
    _PYVIS_SHAPE_MAP,
    _PYVIS_SIZE_MAP,
    _icon_b64_uri,
    _icon_png_path,
)

# ---------------------------------------------------------------------------
# Graphviz topology renderer
# ---------------------------------------------------------------------------

_GRAPHVIZ_INSTALL_MSG = (
    "Graphviz executables not found.\n"
    "Install the system package, e.g.:\n"
    "  sudo apt-get install graphviz    # Debian/Ubuntu\n"
    "  brew install graphviz            # macOS\n"
    "  winget install graphviz          # Windows"
)


def _gv_node_attrs(node: Node, icon_path: Optional[str]) -> dict[str, str]:
    """Return Graphviz node attributes with native shapes and filled colors."""
    bg = _PALETTE.get(node.kind, "#FFF")
    border = _PALETTE.get(f"{node.kind}_border", "#333")
    lbl = node.label.replace("\n", "\\n")

    if icon_path:
        # Use image with HTML label (keeps TABLE for icon support)
        html_lbl = (
            node.label.replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace(">", "&gt;")
            .replace("\n", "<BR/>")
        )
        return {
            "label": (
                f'<<TABLE BORDER="0" CELLBORDER="0" CELLSPACING="0" CELLPADDING="4">'
                f'<TR><TD ALIGN="CENTER"><IMG SRC="{icon_path}"/></TD></TR>'
                f'<TR><TD ALIGN="CENTER">'
                f'<FONT FACE="Arial" POINT-SIZE="9">{html_lbl}</FONT>'
                f"</TD></TR></TABLE>>"
            ),
            "shape": "none",
        }

    shape = _GV_SHAPE_MAP.get(node.kind, "ellipse")
    width = str(max(0.5, node.size / 30.0)) if node.size > 0 else ""
    attrs: dict[str, str] = {
        "label": lbl,
        "shape": shape,
        "style": "filled",
        "fillcolor": bg,
        "color": border,
        "fontname": "Arial",
        "fontsize": "9",
        "penwidth": "1.5",
    }
    if width:
        attrs["width"] = width
        attrs["height"] = width
    return attrs


def render_graphviz(
    model: GraphModel,
    fmt: str = "svg",
    output_path: Optional[str] = None,
    layout: str = "neato",
    use_clusters: bool = False,
) -> str:
    try:
        import graphviz as gv  # noqa: PLC0415
    except ImportError as err:
        raise SystemExit("pip install graphviz") from err

    is_directed = any(e.directed for e in model.edges)
    cls = gv.Digraph if is_directed else gv.Graph
    dot = cls(name=model.title, engine=layout, format=fmt if fmt != "dot" else "svg")
    dot.attr(
        label=f"<<B>{model.title}</B>>",
        labelloc="t",
        fontname="Arial",
        fontsize="14",
        bgcolor="white",
        pad="0.5",
        splines="curved",
        overlap="false",
        nodesep="0.6",
        ranksep="1.0",
    )
    dot.attr("node", margin="0.05,0.05", fontname="Arial", fontsize="9")
    dot.attr("edge", fontname="Arial", fontsize="9", penwidth="1.5")

    elec_nodes = {n.node_id for n in model.nodes if n.cluster == "electrical"}
    hydro_nodes = {n.node_id for n in model.nodes if n.cluster == "hydro"}
    has_both = bool(elec_nodes) and bool(hydro_nodes)

    def add_node(d, node: Node) -> None:
        icon = _icon_png_path(node.kind)
        attrs = _gv_node_attrs(node, icon)
        attrs["tooltip"] = node.tooltip or node.label
        d.node(node.node_id, **attrs)

    if has_both and use_clusters:
        with dot.subgraph(name="cluster_elec") as c:
            c.attr(
                label="Electrical Network",
                bgcolor="#F8FBFF",
                color="#AED6F1",
                style="rounded",
                fontname="Arial Bold",
            )
            for n in model.nodes:
                if n.cluster == "electrical":
                    add_node(c, n)
        with dot.subgraph(name="cluster_hydro") as c:
            c.attr(
                label="Hydro Cascade",
                bgcolor="#F0FFF4",
                color="#A9DFBF",
                style="rounded",
                fontname="Arial Bold",
            )
            for n in model.nodes:
                if n.cluster == "hydro":
                    add_node(c, n)
    else:
        for n in model.nodes:
            add_node(dot, n)

    for e in model.edges:
        attrs: dict = {}
        if e.color:
            attrs["color"] = e.color
        if e.label:
            attrs["label"] = e.label.replace("\n", "\\n")
        if e.style == "dashed":
            attrs.update(style="dashed", penwidth="1.2")
        elif e.style == "dotted":
            attrs.update(style="dotted", penwidth="1.0")
        if not e.directed:
            attrs["dir"] = "none"
            attrs["penwidth"] = str(max(1.5, min(4.0, 1.0 + e.weight / 100)))
        dot.edge(e.src, e.dst, **attrs)

    # Add legend as a subgraph when multiple node kinds are present
    kinds_in_model = {n.kind for n in model.nodes}
    if len(kinds_in_model) > 1:
        with dot.subgraph(name="cluster_legend") as legend:
            legend.attr(
                label="Legend",
                style="rounded",
                color="#CCCCCC",
                fontsize="10",
                fontname="Arial",
            )
            for kind in sorted(kinds_in_model):
                label_text = _LEGEND_LABELS.get(kind, kind)
                fill = _PALETTE.get(kind, "#FFFFFF")
                border = _PALETTE.get(f"{kind}_border", "#333333")
                legend.node(
                    f"legend_{kind}",
                    label=label_text,
                    shape="box",
                    style="filled",
                    fillcolor=fill,
                    color=border,
                    fontsize="8",
                    fontname="Arial",
                    width="0.1",
                    height="0.1",
                )

    if fmt == "dot":
        return dot.source

    if output_path:
        out = Path(output_path)
        try:
            rendered = dot.render(
                filename=str(out.stem),
                directory=str(out.parent) if str(out.parent) != "." else ".",
                format=fmt,
                cleanup=True,
            )
        except FileNotFoundError as err:
            raise SystemExit(_GRAPHVIZ_INSTALL_MSG) from err
        return rendered
    try:
        return dot.pipe(format=fmt).decode("utf-8", errors="replace")
    except FileNotFoundError as err:
        raise SystemExit(_GRAPHVIZ_INSTALL_MSG) from err


# ---------------------------------------------------------------------------
# Mermaid renderer
# ---------------------------------------------------------------------------


def render_mermaid(model: GraphModel, direction: str = "LR") -> str:
    lines = [
        "```mermaid",
        "---",
        f"title: {model.title}",
        "---",
        f"flowchart {direction}",
        "",
    ]

    for n in model.nodes:
        shapes = _MM_SHAPES.get(n.kind, ("[", "]"))
        icon = _MM_ICONS.get(n.kind, "")
        safe = n.node_id.replace("-", "_")
        lbl = (icon + " " + n.label).replace('"', "'").replace("\n", "<br/>")
        lines.append(f'    {safe}{shapes[0]}"{lbl}"{shapes[1]}')

    lines.append("")
    for e in model.edges:
        s = e.src.replace("-", "_")
        d = e.dst.replace("-", "_")
        lbl = e.label.replace('"', "'").replace("\n", " ") if e.label else ""
        if not e.directed:
            arrow = "---"
        elif e.style == "dashed":
            arrow = "-.->"
        elif e.style == "dotted":
            arrow = "...>"
        else:
            arrow = "-->"
        lines.append(f"    {s} {arrow}|{lbl}| {d}" if lbl else f"    {s} {arrow} {d}")

    lines.append("")
    kind_nodes: dict[str, list[str]] = {}
    for n in model.nodes:
        kind_nodes.setdefault(n.kind, []).append(n.node_id.replace("-", "_"))
    for kind, nids in kind_nodes.items():
        style = _MM_STYLES.get(kind, "fill:#FFF,stroke:#333")
        lines.append(f"    classDef cls_{kind} {style}")
        lines.append(f"    class {','.join(nids)} cls_{kind}")

    lines.append("```")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Public vis.js data export
# ---------------------------------------------------------------------------


def model_to_visjs(model: GraphModel) -> dict:
    """Convert a :class:`GraphModel` to a vis.js-compatible ``{nodes, edges}`` dict.

    This is the **public API** for converting a topology model to a format
    suitable for use with `vis-network <https://visjs.github.io/vis-network/>`_.
    It replaces the need to import the private ``_PYVIS_*`` constants directly.

    Returns a dict with two keys:

    ``nodes``
        A list of vis.js node objects, each with the fields ``id``, ``label``,
        ``title`` (tooltip), ``shape``, ``color`` (``{background, border}``),
        ``size``, ``kind``, and ``group``.

    ``edges``
        A list of vis.js edge objects with ``id``, ``from``, ``to``, ``label``,
        ``title``, ``dashes``, ``arrows``, ``color``, and ``width``.
    """
    vis_nodes = []
    for node in model.nodes:
        colors = _PYVIS_COLORS.get(
            node.kind, {"background": "#F0F0F0", "border": "#555"}
        )
        vis_nodes.append(
            {
                "id": node.node_id,
                "label": node.label,
                "title": node.tooltip or node.label,
                "shape": _PYVIS_SHAPE_MAP.get(node.kind, "dot"),
                "color": colors,
                "size": node.size
                if node.size > 0
                else _PYVIS_SIZE_MAP.get(node.kind, 20),
                "kind": node.kind,
                "group": node.cluster or node.kind,
            }
        )

    vis_edges = []
    for i, edge in enumerate(model.edges):
        vis_edges.append(
            {
                "id": i,
                "from": edge.src,
                "to": edge.dst,
                "label": edge.label.replace("\n", " ") if edge.label else "",
                "title": edge.label.replace("\n", " ") if edge.label else "",
                "dashes": edge.style in ("dashed", "dotted"),
                "arrows": "to" if edge.directed else "",
                "color": {"color": edge.color or "#2C3E50", "opacity": 0.8},
                "width": max(1.0, min(6.0, edge.weight)),
                # Short edges (weight < 1) get a shorter spring length
                "length": 50 if edge.weight < 1.0 else 160,
            }
        )

    return {"nodes": vis_nodes, "edges": vis_edges}


# ---------------------------------------------------------------------------
# Public ReactFlow data export
# ---------------------------------------------------------------------------


def _reactflow_layout(model: GraphModel) -> dict[str, tuple[float, float]]:
    """Compute a simple force-directed layout for ReactFlow.

    Uses a circular fallback layout when networkx is not available,
    and a spring layout (fruchterman_reingold) otherwise.  Returns a
    dict mapping ``node_id`` to ``(x, y)`` pixel coordinates.
    """
    if not model.nodes:
        return {}

    try:
        import math

        import networkx as nx

        graph: nx.Graph = nx.Graph()
        for node in model.nodes:
            graph.add_node(node.node_id)
        for edge in model.edges:
            if edge.src in graph and edge.dst in graph:
                graph.add_edge(edge.src, edge.dst)

        # spring_layout returns coords in [-1, 1]; scale to a reasonable pixel grid
        k = 1.5 / math.sqrt(max(1, len(model.nodes)))
        pos = nx.spring_layout(graph, k=k, seed=42, iterations=100)
        scale = 150.0 * math.sqrt(max(1, len(model.nodes)))
        return {
            nid: (float(x) * scale, float(y) * scale) for nid, (x, y) in pos.items()
        }
    except ImportError:
        # Fallback: circular layout
        import math

        n = len(model.nodes)
        radius = max(120.0, 40.0 * n / (2.0 * math.pi))
        return {
            node.node_id: (
                radius * math.cos(2.0 * math.pi * i / n),
                radius * math.sin(2.0 * math.pi * i / n),
            )
            for i, node in enumerate(model.nodes)
        }


# Mapping from GraphModel node kinds to ReactFlow custom node `type` identifiers.
# These identifiers are consumed by the ReactFlow frontend to select a custom
# node component.  Unknown kinds fall back to "default".
_REACTFLOW_NODE_TYPE = {
    "bus": "bus",
    "gen": "generator",
    "gen_solar": "generator",
    "gen_hydro": "generator",
    "gen_wind": "generator",
    "gen_thermal": "generator",
    "gen_battery": "generator",
    "demand": "demand",
    "battery": "battery",
    "converter": "converter",
    "junction": "junction",
    "reservoir": "reservoir",
    "turbine": "turbine",
    "flow": "flow",
    "seepage": "seepage",
    "reserve_zone": "reserve_zone",
}


def model_to_reactflow(model: GraphModel) -> dict:
    """Convert a :class:`GraphModel` to a ReactFlow-compatible ``{nodes, edges}`` dict.

    This is the **public API** for producing data consumable by a
    `React Flow <https://reactflow.dev/>`_ ``<ReactFlow>`` component.  The
    node layout is computed using a spring-force algorithm (or a circular
    fallback if networkx is not available).

    Returns a dict with two keys:

    ``nodes``
        A list of ReactFlow node objects, each with the fields ``id``, ``type``,
        ``position`` (``{x, y}``), and ``data`` (``{label, tooltip, kind,
        cluster, color}``).

    ``edges``
        A list of ReactFlow edge objects with ``id``, ``source``, ``target``,
        ``label``, ``type``, ``animated``, ``style``, and ``data``.
    """
    layout = _reactflow_layout(model)

    rf_nodes = []
    for node in model.nodes:
        colors = _PYVIS_COLORS.get(
            node.kind, {"background": "#F0F0F0", "border": "#555"}
        )
        x, y = layout.get(node.node_id, (0.0, 0.0))
        rf_nodes.append(
            {
                "id": node.node_id,
                "type": _REACTFLOW_NODE_TYPE.get(node.kind, "default"),
                "position": {"x": x, "y": y},
                "data": {
                    "label": node.label,
                    "tooltip": node.tooltip or node.label,
                    "kind": node.kind,
                    "cluster": node.cluster or "",
                    "background": colors.get("background", "#F0F0F0"),
                    "border": colors.get("border", "#555"),
                    "size": node.size
                    if node.size > 0
                    else _PYVIS_SIZE_MAP.get(node.kind, 20),
                },
            }
        )

    rf_edges = []
    for i, edge in enumerate(model.edges):
        edge_style = {
            "stroke": edge.color or "#2C3E50",
            "strokeWidth": max(1.0, min(6.0, edge.weight)),
        }
        if edge.style in ("dashed", "dotted"):
            edge_style["strokeDasharray"] = "6 4"
        rf_edges.append(
            {
                "id": f"e{i}",
                "source": edge.src,
                "target": edge.dst,
                "label": edge.label.replace("\n", " ") if edge.label else "",
                "type": "smoothstep",
                "animated": edge.style == "animated",
                "style": edge_style,
                "markerEnd": ({"type": "arrowclosed"} if edge.directed else None),
                "data": {
                    "kind": edge.style,
                    "weight": edge.weight,
                },
            }
        )

    return {"nodes": rf_nodes, "edges": rf_edges}


# ---------------------------------------------------------------------------
# Interactive HTML renderer (pyvis + font-awesome + custom icons)
# ---------------------------------------------------------------------------


def _legend_html(model: GraphModel) -> str:
    """Build an HTML legend that matches the actual vis.js node shapes and colors."""
    raw_kinds = {n.kind for n in model.nodes}
    # Normalize variant kinds (reservoir_sm -> reservoir) so a single
    # legend entry covers all intensity variants.
    kinds: set[str] = set()
    for k in raw_kinds:
        kinds.add(k)
        if k.startswith("reservoir_"):
            kinds.add("reservoir")
    labels = {
        "bus": "Bus (electrical node)",
        "gen": "Generator (thermal)",
        "gen_solar": "Generator (solar/wind)",
        "gen_hydro": "Generator (hydro-linked)",
        "demand": "Demand / load",
        "battery": "Battery storage",
        "converter": "Power converter",
        "junction": "Hydraulic junction",
        "reservoir": "Reservoir / dam",
        "turbine": "Hydraulic turbine",
        "flow": "Inflow / outflow",
        "seepage": "ReservoirSeepage / seepage",
        "reserve_zone": "Reserve zone",
        "gen_profile": "Generator profile",
        "dem_profile": "Demand profile",
    }
    # SVG path snippets that approximate vis.js node shapes
    _shape_svg = {
        "square": '<rect x="1" y="1" width="14" height="14"/>',
        "triangle": '<polygon points="8,1 15,15 1,15"/>',
        "triangleDown": '<polygon points="1,1 15,1 8,15"/>',
        "diamond": '<polygon points="8,1 15,8 8,15 1,8"/>',
        "hexagon": '<polygon points="4,1 12,1 15,8 12,15 4,15 1,8"/>',
        "box": '<rect x="1" y="3" width="14" height="10" rx="2"/>',
        "database": '<ellipse cx="8" cy="5" rx="7" ry="3"/>'
        '<rect x="1" y="5" width="14" height="7"/>'
        '<ellipse cx="8" cy="12" rx="7" ry="3"/>',
        "ellipse": '<ellipse cx="8" cy="8" rx="7" ry="6"/>',
        "dot": '<circle cx="8" cy="8" r="6"/>',
        "star": '<polygon points="8,1 10,6 15,6 11,10 13,15 8,12 3,15 5,10 1,6 6,6"/>',
    }
    entries = []
    for kind, lbl in labels.items():
        if kind not in kinds:
            continue
        # Use the same icon as the graph nodes when available
        icon_uri = _icon_b64_uri(kind)
        if icon_uri:
            icon_html = (
                f'<img src="{icon_uri}" width="20" height="20"'
                f' style="vertical-align:middle">'
            )
        else:
            colors = _PYVIS_COLORS.get(
                kind, {"background": "#F0F0F0", "border": "#555"}
            )
            bg = colors["background"]
            bd = colors["border"]
            shape_name = _PYVIS_SHAPE_MAP.get(kind, "dot")
            svg_inner = _shape_svg.get(shape_name, _shape_svg["dot"])
            icon_html = (
                f'<svg width="16" height="16" viewBox="0 0 16 16"'
                f' xmlns="http://www.w3.org/2000/svg">'
                f'<g fill="{bg}" stroke="{bd}" stroke-width="1.5">'
                f"{svg_inner}</g></svg>"
            )
        entries.append(
            f'<div style="display:flex;align-items:center;gap:8px;margin:3px 0">'
            f"{icon_html}<span>{lbl}</span></div>"
        )
    return (
        '<div style="position:fixed;bottom:20px;right:20px;'
        "background:rgba(255,255,255,0.97);border:1px solid #CCC;border-radius:10px;"
        "padding:14px 18px;font-family:Arial;font-size:12px;"
        'box-shadow:3px 3px 10px rgba(0,0,0,0.12);z-index:9999;min-width:200px;">'
        '<b style="display:block;margin-bottom:8px;font-size:13px">Legend</b>'
        + "".join(entries)
        + "</div>"
    )


def render_html(model: GraphModel, output_path: str) -> str:
    try:
        from pyvis.network import Network  # noqa: PLC0415
    except ImportError as err:
        raise SystemExit("pip install pyvis") from err

    net = Network(
        height="750px",
        width="100%",
        directed=True,
        notebook=False,
        bgcolor="#FAFAFA",
        font_color="#1C2833",
    )
    net.set_options("""{
      "physics": {
        "enabled": true,
        "barnesHut": {
          "gravitationalConstant": -7000,
          "centralGravity": 0.25,
          "springLength": 160,
          "springConstant": 0.04,
          "damping": 0.09
        },
        "stabilization": {"iterations": 400, "updateInterval": 10}
      },
      "edges": {
        "smooth": {"type": "curvedCW", "roundness": 0.12},
        "font": {"size": 10, "face": "Arial",
                 "background": "rgba(255,255,255,0.8)"},
        "scaling": {"min": 1, "max": 8}
      },
      "nodes": {
        "font": {"face": "Arial", "size": 12},
        "borderWidth": 2,
        "shadow": {"enabled": true, "size": 4, "x": 2, "y": 2}
      },
      "interaction": {
        "hover": true, "tooltipDelay": 150,
        "navigationButtons": true, "keyboard": true,
        "multiselect": true
      }
    }""")

    for node in model.nodes:
        color = dict(
            _PYVIS_COLORS.get(node.kind, {"background": "#FFF", "border": "#333"})
        )
        color["highlight"] = {"background": color["background"], "border": "#E74C3C"}
        size = int(node.size) if node.size > 0 else _PYVIS_SIZE_MAP.get(node.kind, 20)

        icon_uri = _icon_b64_uri(node.kind)
        if icon_uri:
            net.add_node(
                node.node_id,
                label=node.label,
                title=node.tooltip or node.label,
                shape="image",
                image=icon_uri,
                size=size + 8,
                font={"size": 11, "face": "Arial"},
                color=color,
            )
        else:
            net.add_node(
                node.node_id,
                label=node.label,
                title=node.tooltip or node.label,
                shape=_PYVIS_SHAPE_MAP.get(node.kind, "dot"),
                color=color,
                size=size,
                font={"size": 11, "face": "Arial"},
            )

    for e in model.edges:
        dashes = e.style in ("dashed", "dotted")
        c = e.color or "#2C3E50"
        net.add_edge(
            e.src,
            e.dst,
            label=e.label.replace("\n", " ") if e.label else "",
            title=e.label.replace("\n", " ") if e.label else "",
            color={"color": c, "opacity": 0.80},
            dashes=dashes,
            arrows="to" if e.directed else "",
            width=max(1.0, min(8.0, e.weight)) if not dashes else 1.5,
        )

    net.save_graph(output_path)
    html = Path(output_path).read_text(encoding="utf-8")
    fa = f'<link rel="stylesheet" href="{_FA_CDN}">\n'
    info = (
        f'<div style="position:fixed;top:0;left:0;right:0;background:#1A252F;'
        f"color:white;padding:8px 20px;font-family:Arial;font-size:13px;"
        f'z-index:10000;display:flex;align-items:center;gap:20px;">'
        f"<b>gtopt</b> \u2014 <span>{model.title}</span>"
        f'<span style="margin-left:auto;font-size:11px;opacity:0.7">'
        f"{len(model.nodes)} elements \u00b7 {len(model.edges)} connections</span>"
        f'</div><div style="margin-top:40px"></div>'
    )
    html = html.replace("</head>", fa + "</head>", 1)
    html = html.replace("<body>", f"<body>\n{info}", 1)
    html = html.replace("</body>", f"\n{_legend_html(model)}\n</body>", 1)
    Path(output_path).write_text(html, encoding="utf-8")
    return output_path


# ---------------------------------------------------------------------------
# Auto layout selection
# ---------------------------------------------------------------------------


def _auto_layout(model: GraphModel, subsystem: str) -> str:
    n = len(model.nodes)
    if subsystem == "hydro":
        return "dot"
    if n <= _LAYOUT_DOT_THRESHOLD:
        return "dot"
    if n <= _LAYOUT_NEATO_THRESHOLD:
        return "neato"
    if n <= _LAYOUT_FDP_THRESHOLD:
        return "fdp"
    return "sfdp"  # best for very large graphs


# ---------------------------------------------------------------------------
# Image viewer helper
# ---------------------------------------------------------------------------


def _mermaid_to_html(mermaid_text: str, title: str = "gtopt Diagram") -> str:
    """Return a self-contained HTML page that renders *mermaid_text* in a browser.

    The markdown backtick fences (`` ```mermaid`` / `` ``` ``) are stripped
    automatically so the text may be passed with or without them.
    """
    lines = mermaid_text.splitlines()
    if lines and lines[0].startswith("```"):
        lines = lines[1:]
    if lines and lines[-1].startswith("```"):
        lines = lines[:-1]
    mmd_inner = "\n".join(lines)

    return textwrap.dedent(f"""\
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>{title}</title>
<script src="https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js"></script>
<style>
  body {{ font-family: Arial, sans-serif; background: #F4F6F7; margin: 0; padding: 20px; }}
  h1   {{ color: #1A252F; font-size: 22px; margin-bottom: 16px; }}
  .card {{ background: white; border: 1px solid #D5D8DC; border-radius: 10px;
            box-shadow: 0 2px 8px rgba(0,0,0,0.07); padding: 16px; overflow-x: auto; }}
  .mermaid {{ text-align: center; }}
</style>
</head>
<body>
<h1>{title}</h1>
<div class="card">
  <div class="mermaid">
{mmd_inner}
  </div>
</div>
<script>
  mermaid.initialize({{ startOnLoad: true, theme: 'default',
    themeVariables: {{ fontSize: '13px' }} }});
</script>
</body>
</html>
""")


def _show_mermaid(mermaid_text: str, title: str = "gtopt Diagram") -> None:
    """Write *mermaid_text* as a temporary HTML file and open it in a browser."""
    html = _mermaid_to_html(mermaid_text, title=title)
    fd, tmp_path = tempfile.mkstemp(suffix=".html", prefix="gtopt_mermaid_")
    try:
        os.write(fd, html.encode("utf-8"))
    finally:
        os.close(fd)
    webbrowser.open(Path(tmp_path).as_uri())


def display_diagram(path: str, fmt: str) -> None:
    """Open *path* in a suitable viewer.

    Strategy (tried in order):
    1. For PNG: try ``PIL.Image`` (Pillow) ``show()``.
    2. For all formats: ``webbrowser.open()`` — works on Linux, macOS and
       Windows without any extra dependencies.
    """
    abs_path = str(Path(path).resolve())
    if fmt == "png":
        try:
            from PIL import Image  # noqa: PLC0415

            Image.open(abs_path).show()
            return
        except ImportError:
            pass  # fall through to webbrowser
    webbrowser.open(Path(abs_path).as_uri())
