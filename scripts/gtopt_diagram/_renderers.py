# SPDX-License-Identifier: BSD-3-Clause
"""Rendering backends: Graphviz, Mermaid, vis.js/pyvis HTML, image viewer.

This module provides functions to convert a :class:`GraphModel` into various
output formats (SVG, PNG, PDF, DOT, Mermaid, interactive HTML) and display
the results.
"""

from __future__ import annotations

import json
import os
import tempfile
import textwrap
import webbrowser
from pathlib import Path
from typing import Optional

from gtopt_diagram._graph_model import Edge, GraphModel, Node
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
        # N1: articulation-point buses get a red border + thicker pen.
        if node.is_critical:
            attrs["color"] = "#D32F2F"
            attrs["penwidth"] = "2.5"
        d.node(node.node_id, **attrs)

    # Plant subclusters: render every node sharing a ``subcluster`` tag
    # inside a nested subgraph.  Plant metadata (pmax / n_units / uniq)
    # lives on ``model.plant_meta`` keyed by the same tag.
    plant_meta: dict[str, str] = getattr(model, "plant_meta", {}) or {}

    # Pre-compute node → cluster lookups so the edge-loop below can
    # decide which subgraph each edge belongs to: inner (subcluster)
    # if both endpoints share the same subcluster, else outer
    # (super_cluster) if both share the same super_cluster, else
    # top-level.
    node_subcluster: dict[str, str] = {n.node_id: n.subcluster for n in model.nodes}
    node_super: dict[str, str] = {n.node_id: n.super_cluster for n in model.nodes}
    # N3 edge betweenness — symmetric dict keyed by (node_id, node_id).
    betweenness: dict = getattr(model, "edge_betweenness", {}) or {}
    edges_in_subcluster: dict[str, list[Edge]] = {}
    for e in model.edges:
        sa = node_subcluster.get(e.src, "")
        sb = node_subcluster.get(e.dst, "")
        if sa and sa == sb:
            edges_in_subcluster.setdefault(sa, []).append(e)
            continue
        pa = node_super.get(e.src, "")
        pb = node_super.get(e.dst, "")
        if pa and pa == pb:
            edges_in_subcluster.setdefault(pa, []).append(e)
            continue
        edges_in_subcluster.setdefault("", []).append(e)

    def _add_edge_to(parent, e: Edge) -> None:
        attrs: dict = {}
        # N1: bridge edges are painted red — override any color.
        if e.is_critical:
            attrs["color"] = "#D32F2F"
            attrs["penwidth"] = "3.0"
            attrs["style"] = "dashed"
        elif e.color:
            attrs["color"] = e.color
        if e.label:
            attrs["label"] = e.label.replace("\n", "\\n")
        if not e.is_critical and e.style == "dashed":
            attrs.update(style="dashed", penwidth="1.2")
        elif not e.is_critical and e.style == "dotted":
            attrs.update(style="dotted", penwidth="1.0")
        if not e.directed:
            attrs["dir"] = "none"
            attrs["penwidth"] = str(max(1.5, min(4.0, 1.0 + e.weight / 100)))
        # N3: backbone emphasis — boost pen-width on high-betweenness
        # edges.  Applied at render time so it doesn't mutate the
        # edge.weight read by other consumers / tests.
        betw_score = betweenness.get((e.src, e.dst)) if not e.is_critical else None
        if betw_score is not None and betw_score >= 0.5:
            try:
                cur = float(attrs.get("penwidth", "1.5"))
            except ValueError:
                cur = 1.5
            attrs["penwidth"] = f"{max(cur, 2.0 + 3.0 * betw_score):.2f}"
        parent.edge(e.src, e.dst, **attrs)

    def _subcluster_style(tag: str) -> dict[str, str]:
        """Per-tag SVG style overrides keyed on the tag prefix."""
        if tag.startswith("substation:"):
            return {"bgcolor": "#E3F2FD", "color": "#1976D2"}
        if tag.startswith("reserve_zone:"):
            return {"bgcolor": "#E8F5E9", "color": "#388E3C"}
        if tag.startswith("fuel:"):
            return {"bgcolor": "#FFF3E0", "color": "#E65100"}
        # Plant is the default (yellow).
        return {"bgcolor": "#FFF8E1", "color": "#FFB300"}

    def _emit_inner_subclusters(parent, nodes: list[Node]) -> None:
        """Emit nodes grouped by their ``subcluster`` tag.

        Nodes with the same non-empty ``subcluster`` value are wrapped
        in a single rounded subgraph; nodes with an empty subcluster go
        directly under ``parent``.  Intra-subcluster edges (both
        endpoints share the same subcluster) are also drawn inside the
        subgraph.
        """
        by_sub: dict[str, list[Node]] = {}
        loose: list[Node] = []
        for nd in nodes:
            if nd.subcluster:
                by_sub.setdefault(nd.subcluster, []).append(nd)
            else:
                loose.append(nd)
        for nd in loose:
            add_node(parent, nd)
        for tag in sorted(by_sub):
            safe = tag.replace(":", "_").replace(" ", "_")
            style = _subcluster_style(tag)
            with parent.subgraph(name=f"cluster_{safe}") as sub:
                sub.attr(
                    label=plant_meta.get(tag, tag),
                    style="rounded,dashed",
                    fontname="Arial Bold",
                    fontsize="9",
                    **style,
                )
                for nd in by_sub[tag]:
                    add_node(sub, nd)
                for sub_edge in edges_in_subcluster.get(tag, []):
                    _add_edge_to(sub, sub_edge)

    def _emit_subcluster_groups(parent, nodes: list[Node]) -> None:
        """Render ``nodes`` honoring both ``super_cluster`` (basin) and
        ``subcluster`` (plant / substation / reserve zone / fuel).

        Two-level nesting: when a node carries a non-empty
        ``super_cluster``, it is wrapped in an OUTER rounded subgraph
        (the basin), and its ``subcluster`` membership produces an
        INNER subgraph nested inside that.  Nodes without a
        ``super_cluster`` use the single-level ``subcluster`` rendering
        directly under ``parent``.
        """
        by_super: dict[str, list[Node]] = {}
        outside: list[Node] = []
        for nd in nodes:
            if nd.super_cluster:
                by_super.setdefault(nd.super_cluster, []).append(nd)
            else:
                outside.append(nd)
        # Nodes outside any super-cluster: directly emit by subcluster.
        _emit_inner_subclusters(parent, outside)
        for super_tag in sorted(by_super):
            safe = super_tag.replace(":", "_").replace(" ", "_")
            # Distinct palette per super-cluster prefix:
            #   basin (hydro drainage area) — water-cyan
            #   community (electrical region) — lavender / purple
            #   anything else — neutral grey-blue fallback
            if super_tag.startswith("basin:"):
                super_bg, super_fg = "#E1F5FE", "#0277BD"  # cyan / deep blue
            elif super_tag.startswith("community:"):
                super_bg, super_fg = "#F3E5F5", "#6A1B9A"  # lavender / purple
            else:
                super_bg, super_fg = "#ECEFF1", "#37474F"  # neutral grey-blue
            with parent.subgraph(name=f"cluster_{safe}") as outer:
                outer.attr(
                    label=plant_meta.get(super_tag, super_tag),
                    bgcolor=super_bg,
                    color=super_fg,
                    style="rounded",
                    fontname="Arial Bold",
                    fontsize="10",
                )
                _emit_inner_subclusters(outer, by_super[super_tag])
                # Edges fully inside the super-cluster also live inside
                # the outer subgraph.  An edge between two nodes that
                # share the same super_cluster but DIFFERENT subclusters
                # belongs here too.
                for ed in edges_in_subcluster.get(super_tag, []):
                    _add_edge_to(outer, ed)
                # N4 layout hint — basin water-flow topological sort.
                # Emit ``{ rank=source; <upstream>; }`` /
                # ``{ rank=sink; <downstream>; }`` so the cluster lays
                # out top → bottom following the cascade.  Only emit
                # when the target nodes ARE in the model (filtering /
                # aggregation may have dropped them).
                basin_topo = getattr(model, "basin_topo_order", {}) or {}
                order = basin_topo.get(super_tag, [])
                present = {n.node_id for n in by_super[super_tag]}
                ordered_present = [nid for nid in order if nid in present]
                if len(ordered_present) >= 2:
                    with outer.subgraph() as rk:
                        rk.attr(rank="source")
                        rk.node(ordered_present[0])
                    with outer.subgraph() as rk:
                        rk.attr(rank="sink")
                        rk.node(ordered_present[-1])

    if has_both and use_clusters:
        with dot.subgraph(name="cluster_elec") as c:
            c.attr(
                label="Electrical Network",
                bgcolor="#F8FBFF",
                color="#AED6F1",
                style="rounded",
                fontname="Arial Bold",
            )
            _emit_subcluster_groups(
                c, [n for n in model.nodes if n.cluster == "electrical"]
            )
        with dot.subgraph(name="cluster_hydro") as c:
            c.attr(
                label="Hydro Cascade",
                bgcolor="#F0FFF4",
                color="#A9DFBF",
                style="rounded",
                fontname="Arial Bold",
            )
            _emit_subcluster_groups(c, [n for n in model.nodes if n.cluster == "hydro"])
    else:
        # Even without electrical/hydro split, honour plant subclusters
        # so Plant grouping is visible in the flat layout.
        _emit_subcluster_groups(dot, list(model.nodes))

    # Intra-subcluster edges were already emitted inside their subgraph
    # by ``_emit_subcluster_groups``.  Here we draw only edges that
    # cross subcluster boundaries (or whose endpoints have no subcluster).
    for e in edges_in_subcluster.get("", []):
        _add_edge_to(dot, e)

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
        base_size = node.size if node.size > 0 else _PYVIS_SIZE_MAP.get(node.kind, 20)
        # Use a kind-specific icon when one is available (matches the standalone
        # HTML renderer); fall back to the geometric vis.js shape otherwise.
        icon_uri = _icon_b64_uri(node.kind)
        if icon_uri:
            vis_nodes.append(
                {
                    "id": node.node_id,
                    "label": node.label,
                    "title": node.tooltip or node.label,
                    "shape": "image",
                    "image": icon_uri,
                    "color": colors,
                    "size": base_size + 8,
                    "kind": node.kind,
                    "group": node.subcluster or node.cluster or node.kind,
                }
            )
        else:
            vis_nodes.append(
                {
                    "id": node.node_id,
                    "label": node.label,
                    "title": node.tooltip or node.label,
                    "shape": _PYVIS_SHAPE_MAP.get(node.kind, "dot"),
                    "color": colors,
                    "size": base_size,
                    "kind": node.kind,
                    "group": node.subcluster or node.cluster or node.kind,
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
                    "subcluster": node.subcluster or "",
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

    # Per-cluster-prefix palette so the user sees the grouping at a
    # glance even when vis.js's auto-color cycler runs out of distinct
    # colours.  Keys are cluster-tag prefixes; the matching node
    # background/border are applied on top of the kind-based defaults.
    _CLUSTER_TINT = {
        "basin": ("#B3E5FC", "#0277BD"),  # cyan
        "community": ("#E1BEE7", "#6A1B9A"),  # lavender
        "substation": ("#90CAF9", "#1565C0"),  # blue
        "plant": ("#FFE082", "#F9A825"),  # amber
        "fuel": ("#FFCC80", "#E65100"),  # orange
        "reserve_zone": ("#A5D6A7", "#2E7D32"),  # green
    }

    for node in model.nodes:
        color = dict(
            _PYVIS_COLORS.get(node.kind, {"background": "#FFF", "border": "#333"})
        )
        # Override colour by the finest-grained cluster tag the node
        # carries — makes the per-group colour visible even without
        # icons or subgraph boxes (vis.js doesn't draw cluster
        # boundaries the way graphviz does).
        primary_tag = node.subcluster or node.super_cluster
        if primary_tag and ":" in primary_tag:
            tint = _CLUSTER_TINT.get(primary_tag.split(":", 1)[0])
            if tint:
                color["background"], color["border"] = tint
        # N1 / isolated / orphan: red border + thicker pen.
        if node.is_critical:
            color["border"] = "#D32F2F"
        color["highlight"] = {"background": color["background"], "border": "#E74C3C"}
        size = int(node.size) if node.size > 0 else _PYVIS_SIZE_MAP.get(node.kind, 20)
        if node.is_critical:
            size += 4
        group = node.subcluster or node.super_cluster or node.cluster or node.kind

        icon_uri = _icon_b64_uri(node.kind)
        # pyvis's ``add_node`` silently drops the explicit ``color``
        # kwarg whenever ``group`` is also set (its internal Node ctor
        # treats them as mutually exclusive — group-based styling
        # always wins).  We need BOTH: the group drives the vis.js
        # legend / filter dropdown, and the per-cluster tint above
        # makes the grouping visible at a glance.  Bypass ``add_node``
        # and write the node dict directly to ``net.nodes`` so both
        # fields survive into the serialised HTML.
        node_dict: dict = {
            "id": node.node_id,
            "label": node.label,
            "title": node.tooltip or node.label,
            "color": color,
            "group": group,
            "size": size + 8 if icon_uri else size,
            "font": {"size": 11, "face": "Arial"},
            "borderWidth": 3 if node.is_critical else 1,
        }
        if icon_uri:
            node_dict["shape"] = "image"
            node_dict["image"] = icon_uri
        else:
            node_dict["shape"] = _PYVIS_SHAPE_MAP.get(node.kind, "dot")
        net.nodes.append(node_dict)
        net.node_ids.append(node.node_id)
        net.node_map[node.node_id] = node_dict

    betweenness: dict = getattr(model, "edge_betweenness", {}) or {}
    for e in model.edges:
        dashes = e.style in ("dashed", "dotted") or e.is_critical
        c = "#D32F2F" if e.is_critical else (e.color or "#2C3E50")
        base_w = max(1.0, min(8.0, e.weight))
        # N3 backbone emphasis: blend betweenness score into edge width.
        score = betweenness.get((e.src, e.dst)) if not e.is_critical else None
        if score is not None and score >= 0.5:
            base_w = max(base_w, 2.0 + 5.0 * score)
        net.add_edge(
            e.src,
            e.dst,
            label=e.label.replace("\n", " ") if e.label else "",
            title=e.label.replace("\n", " ") if e.label else "",
            color={"color": c, "opacity": 0.80},
            dashes=dashes,
            arrows="to" if e.directed else "",
            width=4.0 if e.is_critical else base_w,
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

    # \u2500\u2500 Auto-clustering: after the network stabilises, collapse every
    # ``group:<tag>`` whose **prefix** is in ``opts.collapse_groups``
    # into a single vis.js cluster.  Tiers NOT listed stay expanded so
    # the grouping acts as a layout hint only (vis.js still gathers
    # members spatially via the shared ``group`` field).  Empty list
    # = no auto-collapse.  Double-click any collapsed bubble to
    # expand.  Each cluster keeps the group's colour + a count label.
    plant_meta: dict = getattr(model, "plant_meta", {}) or {}
    # Empty list = layout-hint mode → no auto-collapse on load.  When
    # non-empty, only the listed prefixes are collapsed; the others
    # stay expanded so vis.js physics still pulls members together.
    collapse_prefixes: set = set(getattr(model, "collapse_groups", []) or [])
    cluster_labels: dict = {}
    seen_groups: set = set()
    if collapse_prefixes:
        for n in model.nodes:
            tag = n.subcluster or n.super_cluster
            if not tag or tag in seen_groups:
                continue
            prefix = tag.split(":", 1)[0] if ":" in tag else ""
            if prefix not in collapse_prefixes:
                continue
            seen_groups.add(tag)
            cluster_labels[tag] = plant_meta.get(tag, tag)
    if cluster_labels:
        labels_json = json.dumps(cluster_labels)
        cluster_script = (
            '\n<script type="text/javascript">\n'
            "window.addEventListener('load', function() {\n"
            "  if (typeof network === 'undefined') return;\n"
            f"  var clusterLabels = {labels_json};\n"
            "  network.once('stabilized', function() {\n"
            "    Object.keys(clusterLabels).forEach(function(tag) {\n"
            "      var label = clusterLabels[tag];\n"
            "      var opts = {\n"
            "        joinCondition: function(nodeOpts) {\n"
            "          return nodeOpts.group === tag;\n"
            "        },\n"
            "        processProperties: function(clusterOptions, childNodes) {\n"
            "          clusterOptions.label = label + ' (' + childNodes.length + ')';\n"
            "          clusterOptions.shape = 'box';\n"
            "          clusterOptions.borderWidth = 3;\n"
            "          clusterOptions.font = { size: 14, face: 'Arial', "
            "color: '#1A252F', strokeWidth: 0 };\n"
            "          return clusterOptions;\n"
            "        },\n"
            "        clusterNodeProperties: {\n"
            "          id: 'cluster_' + tag.replace(/[^A-Za-z0-9_]/g, '_'),\n"
            "          allowSingleNodeCluster: false\n"
            "        }\n"
            "      };\n"
            "      network.cluster(opts);\n"
            "    });\n"
            "    network.fit();\n"
            "  });\n"
            "  network.on('doubleClick', function(params) {\n"
            "    if (params.nodes.length === 1 "
            "&& network.isCluster(params.nodes[0])) {\n"
            "      network.openCluster(params.nodes[0]);\n"
            "    }\n"
            "  });\n"
            "});\n"
            "</script>\n"
        )
        html = html.replace("</body>", cluster_script + "</body>", 1)
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
