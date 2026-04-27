#!/usr/bin/env python3
"""
gtopt_diagram.py — Generate electrical, hydro and planning-structure diagrams
from gtopt JSON planning files.

This module is installed as the ``gtopt_diagram`` command-line tool.
See ``DIAGRAM_TOOL.md`` for the full user guide.

Diagram types (--diagram-type)
-------------------------------
``topology``  — Network topology diagram (default).
``planning``  — Planning time hierarchy: scenarios → phases → stages → blocks.

Subsystems for topology diagrams (--subsystem)
-----------------------------------------------
``electrical`` — Buses, generators, demands, lines, batteries, converters,
                 LNG terminals.
``hydro``      — Junctions, waterways, reservoirs, turbines, flows, seepages,
                 pumps, volume rights, flow rights.
``full``       — Both subsystems together (default).

Output formats (--format)
--------------------------
``svg``      — Scalable SVG via Graphviz (recommended for docs).
``png``      — Rasterised PNG via Graphviz.
``pdf``      — PDF via Graphviz.
``dot``      — Raw Graphviz DOT source.
``mermaid``  — Mermaid flowchart source (embeds in GitHub Markdown).
``html``     — Interactive HTML with vis.js / font-awesome (open in browser).

Aggregation / reduction (--aggregate, default: auto)
-----------------------------------------------------
``auto``     Smart selection based on total element count (default):
               < 100  elements → ``none``  (show every element individually)
               100-999 elements → ``bus``  (one summary node per bus)
               ≥ 1000 elements  → ``type`` + smart voltage threshold
                                  (aggregate by type; threshold chosen so ≤ 64 buses remain)
``none``     Show every individual element.
``bus``      One summary node per bus.
``type``     One node per (bus, generator-type) pair with icon.
             Types: 💧 hydro ☀️ solar 🌬️ wind ⚡ thermal 🔋 battery.
``global``   One node per generator type for the whole system.

``--no-generators``  Omit all generator nodes (topology-only view):
                     buses, lines, demands, hydro elements only.

Other reduction options (--voltage-threshold, --filter-type, …)
----------------------------------------------------------------
``--voltage-threshold KV``   Lump buses below KV into their nearest HV neighbour.
``--filter-type TYPE...``    Show only generators of listed types.
``--focus-bus BUS...``       Show only N-hop neighbourhood of named buses.
``--top-gens N``             Keep only top-N generators per bus by pmax.
``--max-nodes N``            Escalate aggregation mode until ≤ N nodes.
``--compact``                Omit pmax/gcost/reactance labels.
``--hide-isolated``          Remove nodes with no connections.

Dependencies
------------
    pip install "gtopt-scripts[diagram]"   # installs graphviz pyvis cairosvg
    sudo apt-get install graphviz

Usage examples
--------------
    # Auto mode (default) — picks right strategy automatically
    gtopt_diagram cases/ieee_9b/ieee_9b.json -o ieee9b.svg       # <100 → none
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json -o c2y.svg  # ≥1000 → type+smart threshold

    # Topology-only (no generators) — clean network view
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json --no-generators -o topo.svg

    # Force explicit aggregation
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
        --aggregate type --voltage-threshold 220 --compact -o case2y_hv.svg

    # Global summary (one node per generator type)
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
        --aggregate global --compact -o case2y_global.svg

    # Hydro cascade only
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
        --subsystem hydro -o case2y_hydro.svg

    # Interactive HTML for exploration
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
        --aggregate type --voltage-threshold 100 --compact \\
        --format html -o case2y.html

    # Planning structure
    gtopt_diagram cases/gtopt_case_2y/gtopt_case_2y.json \\
        --diagram-type planning -o case2y_planning.svg

    # Mermaid snippet for GitHub Markdown
    gtopt_diagram cases/ieee_9b/ieee_9b.json --format mermaid
"""

from __future__ import annotations

import logging
import sys
from collections import defaultdict

from gtopt_diagram._classify import (
    classify_gen as _classify_gen_impl,
    dominant_kind as _dominant_kind_impl,
    efficiency_turbine_pairs as _efficiency_turbine_pairs_impl,
    elem_name as _elem_name_impl,
    gen_pmax as _gen_pmax_impl,
    icon_key_for_type as _icon_key_for_type_impl,
    resolve as _resolve_impl,
    scalar as _scalar_impl,
    turbine_gen_refs as _turbine_gen_refs_impl,
    turbine_waterway_refs as _turbine_waterway_refs_impl,
)
from gtopt_diagram._topology_hydro import TopologyHydroMixin
from gtopt_diagram._topology_network import TopologyNetworkMixin
from gtopt_diagram._data_utils import (
    auto_voltage_threshold,
    build_voltage_map as _build_voltage_map_impl,
    count_visible_buses as _count_visible_buses_impl,
    resolve_bus_ref as _resolve_bus_ref_impl,
)

# Edge/Node are re-exported for back-compat with `from gtopt_diagram.gtopt_diagram
# import Edge, Node` call sites in tests + downstream code.
# pylint: disable=unused-import
from gtopt_diagram._graph_model import (  # noqa: F401
    AUTO_BUS_THRESHOLD as _AUTO_BUS_THRESHOLD,
    AUTO_MAX_HV_BUSES as _AUTO_MAX_HV_BUSES,
    AUTO_NONE_THRESHOLD as _AUTO_NONE_THRESHOLD,
    Edge,
    FilterOptions,
    GraphModel,
    Node,
)
# pylint: enable=unused-import

# Re-export from new submodules for backward compatibility
# pylint: disable=unused-import
from gtopt_diagram._planning_diagram import (  # noqa: F401
    _build_planning_html,
    _build_planning_mermaid,
    _build_planning_svg,
    _DEFAULT_PLANNING,
)
from gtopt_diagram._renderers import (  # noqa: F401
    _auto_layout,
    _legend_html,
    _mermaid_to_html,
    _show_mermaid,
    display_diagram,
    model_to_visjs,
    render_graphviz,
    render_html,
    render_mermaid,
)
from gtopt_diagram._svg_constants import (  # noqa: F401
    _FA_CDN,
    _GEN_TYPE_ICON_SVG,
    _gen_type_icon_svg,
    _GV_SHAPE_MAP,
    _ICON_CACHE,
    _ICON_CACHE_VER,
    _ICON_SVG,
    _icon_b64_uri,
    _icon_cache_dir,
    _icon_png_path,
    _LAYOUT_DOT_THRESHOLD,
    _LAYOUT_FDP_THRESHOLD,
    _LAYOUT_NEATO_THRESHOLD,
    _LEGEND_LABELS,
    _line_color_width,
    _LINE_VOLTAGE_BANDS,
    _MM_ICONS,
    _MM_SHAPES,
    _MM_STYLES,
    _PALETTE,
    _PALETTE_COLORBLIND,
    _PALETTE_TO_GEN_TYPE,
    _PYVIS_COLORS,
    _PYVIS_SHAPE_MAP,
    _PYVIS_SIZE_MAP,
    _reservoir_intensity,
)

# Re-export main from cli module
from gtopt_diagram.cli import main  # noqa: F401

# pylint: enable=unused-import

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Generator-type classification
# ---------------------------------------------------------------------------

# _GEN_TYPE_META is imported from _classify (canonical definition lives there)


# Thin wrappers delegating to _classify module (preserves private-name API)
_turbine_gen_refs = _turbine_gen_refs_impl
_turbine_waterway_refs = _turbine_waterway_refs_impl
_efficiency_turbine_pairs = _efficiency_turbine_pairs_impl
_classify_gen = _classify_gen_impl
_gen_pmax = _gen_pmax_impl
_elem_name = _elem_name_impl


# ---------------------------------------------------------------------------
# FilterOptions, Node, Edge, GraphModel — imported from _graph_model
# ---------------------------------------------------------------------------

# (FilterOptions, Node, Edge, GraphModel imported at top of file)

# ---------------------------------------------------------------------------
# Voltage-level bus reduction — imported from _data_utils
# ---------------------------------------------------------------------------

_build_voltage_map = _build_voltage_map_impl
_count_visible_buses = _count_visible_buses_impl
_resolve_bus_ref = _resolve_bus_ref_impl
# auto_voltage_threshold imported directly at top of file


# ---------------------------------------------------------------------------
# JSON helpers
# ---------------------------------------------------------------------------


_scalar = _scalar_impl
_resolve = _resolve_impl

_icon_key_for_type = _icon_key_for_type_impl
_dominant_kind = _dominant_kind_impl


# ---------------------------------------------------------------------------
# Topology builder
# ---------------------------------------------------------------------------


class TopologyBuilder(
    TopologyNetworkMixin,
    TopologyHydroMixin,
):
    """Build a GraphModel from a gtopt JSON planning structure.

    Public surface composed via mixins:

    * :class:`gtopt_diagram._topology_ids.TopologyIdsMixin` — graph node-ID
      helpers (``_bid``, ``_gid``, …) plus lookup helpers
      (``_resolve_field``, ``_find``, ``_find_node_id``, ``_bus_node_id``,
      ``_gen_kind``).
    * :class:`gtopt_diagram._topology_network.TopologyNetworkMixin` —
      buses, generators (4 aggregation modes), demands, lines,
      batteries, converters, generator/demand profiles.
    * :class:`gtopt_diagram._topology_hydro.TopologyHydroMixin` —
      junctions, waterways, reservoirs, turbines, flows, seepages,
      rights, efficiencies, pumps, LNG terminals, reserve zones,
      reserve provisions.

    This class itself owns ``__init__``, the ``eff_*`` / ``auto_info``
    properties, ``_count_elements``, ``build``, and
    ``_compute_focus_set``.
    """

    def __init__(self, planning, subsystem="full", opts=None, resolver=None):
        self.sys = planning.get("system", {})
        self.subsystem = subsystem
        self.opts = opts or FilterOptions()
        self.model = GraphModel(title=self.sys.get("name", "gtopt Network"))
        self._resolver = resolver  # FieldSchedResolver or None
        self._turb_refs = _turbine_gen_refs(self.sys)
        self._turb_way_refs = _turbine_waterway_refs(self.sys)
        self._eff_turb_refs = _efficiency_turbine_pairs(self.sys)
        self._vmap = _build_voltage_map(
            self.sys.get("bus_array", []),
            self.sys.get("line_array", []),
            self.opts.voltage_threshold,
        )
        self._bus_node_ids: set = set()
        self._eff_agg: str = "none"
        self._eff_vthresh: float = self.opts.voltage_threshold
        self._auto_info = None  # set by build() when aggregate="auto"
        self._focus_nids = None

    # ── Public accessors for build metadata ────────────────────────────────

    @property
    def eff_agg(self) -> str:
        """Effective aggregation mode resolved by :meth:`build`."""
        return self._eff_agg

    @property
    def eff_vthresh(self) -> float:
        """Effective voltage threshold resolved by :meth:`build`."""
        return self._eff_vthresh

    @property
    def auto_info(self):
        """(n_total, agg, vthresh) tuple set by auto mode, or *None*."""
        return self._auto_info

    def _count_elements(self) -> int:
        """Return a rough total element count for auto-mode decisions."""
        s = self.sys
        return sum(
            len(s.get(k, []))
            for k in (
                "generator_array",
                "bus_array",
                "demand_array",
                "line_array",
                "battery_array",
                "converter_array",
                "junction_array",
                "waterway_array",
                "reservoir_array",
                "turbine_array",
                "flow_array",
                "reservoir_seepage_array",
                "reservoir_discharge_limit_array",
                "reservoir_production_factor_array",
                "volume_right_array",
                "flow_right_array",
                "pump_array",
                "lng_terminal_array",
            )
        )

    def build(self):
        agg = self.opts.aggregate
        vthresh = self.opts.voltage_threshold

        # ── Auto mode: choose strategy from element count ──────────────────
        # When focus_buses is set, the user has already narrowed the view,
        # so skip aggressive auto-reduction (no voltage threshold).
        if agg == "auto":
            n_total = self._count_elements()
            if self.opts.focus_buses:
                # Focused view: use "none" for small systems, "bus" otherwise
                agg = "none" if n_total < _AUTO_BUS_THRESHOLD else "bus"
            elif n_total < _AUTO_NONE_THRESHOLD:
                agg = "none"
            elif n_total < _AUTO_BUS_THRESHOLD:
                agg = "bus"
            else:
                # Aggressive: aggregate by type AND apply a smart voltage
                # threshold chosen so that ≤ _AUTO_MAX_HV_BUSES buses remain.
                agg = "type"
                if vthresh == 0.0:
                    vthresh = auto_voltage_threshold(
                        self.sys.get("bus_array", []),
                        self.sys.get("line_array", []),
                        max_buses=_AUTO_MAX_HV_BUSES,
                    )
            self._auto_info = (n_total, agg, vthresh)

        # ── Explicit max_nodes escalation (overrides auto-resolved mode) ───
        if self.opts.max_nodes > 0:
            n_g = len(self.sys.get("generator_array", []))
            n_b = len(self.sys.get("bus_array", []))
            n_d = len(self.sys.get("demand_array", []))
            rough = n_b + n_g + n_d
            if agg == "none" and rough > self.opts.max_nodes:
                agg = "bus"
            if agg == "bus" and (n_b * 2 + n_d) > self.opts.max_nodes:
                agg = "type"
            if agg == "type" and n_b * 6 > self.opts.max_nodes * 2:
                agg = "global"

        self._eff_agg = agg
        self._eff_vthresh = vthresh
        # Update the voltage map if auto-mode changed the threshold
        if vthresh != self.opts.voltage_threshold:
            self._vmap = _build_voltage_map(
                self.sys.get("bus_array", []),
                self.sys.get("line_array", []),
                vthresh,
            )
            self._bus_node_ids = set()  # reset; will be repopulated by _buses()

        self._focus_nids = self._compute_focus_set() if self.opts.focus_buses else None

        # Auto-hide hydro subsystem when no hydro elements exist
        if self.subsystem == "full":
            sys = self.sys
            has_hydro = any(
                sys.get(k)
                for k in (
                    "junction_array",
                    "waterway_array",
                    "reservoir_array",
                    "turbine_array",
                    "flow_array",
                    "reservoir_seepage_array",
                    "reservoir_discharge_limit_array",
                    "volume_right_array",
                    "flow_right_array",
                    "pump_array",
                )
            )
            if not has_hydro:
                self.subsystem = "electrical"
                logger.debug(
                    "No hydro elements found; switching to electrical subsystem"
                )

        s = self.subsystem
        if s in ("full", "electrical"):
            self._buses()
            if not self.opts.no_generators:
                self._generators()
            self._demands()
            self._lines()
            self._batteries()
            self._converters()
            self._generator_profiles()
            self._demand_profiles()
            self._lng_terminals()
        if s in ("full", "hydro"):
            self._junctions()
            self._waterways()
            self._reservoirs()
            self._turbines()
            self._flows()
            self._seepages()
            self._pumps()
            self._volume_rights()
            self._flow_rights()
            self._reservoir_efficiencies()
        if s == "full":
            self._reserve_zones()
            self._reserve_provisions()
        # Remove edges that reference nodes absent from the model (e.g. when
        # subsystem="hydro" skips _generators(), turbine→generator edges would
        # otherwise reference non-existent node IDs and crash pyvis/render_html).
        node_ids = {n.node_id for n in self.model.nodes}
        self.model.edges = [
            e for e in self.model.edges if e.src in node_ids and e.dst in node_ids
        ]
        if self.opts.hide_isolated:
            connected = {e.src for e in self.model.edges} | {
                e.dst for e in self.model.edges
            }
            self.model.nodes = [n for n in self.model.nodes if n.node_id in connected]
        return self.model

    def _compute_focus_set(self):
        adj = defaultdict(set)
        for line in self.sys.get("line_array", []):
            ab = self._find("bus_array", line.get("bus_a"))
            bb = self._find("bus_array", line.get("bus_b"))
            if ab and bb:
                a, b = self._bid(ab), self._bid(bb)
                adj[a].add(b)
                adj[b].add(a)
        seeds = set()
        for fb in self.opts.focus_buses:
            bus = self._find("bus_array", fb)
            if bus:
                seeds.add(self._bid(bus))
        visited, frontier = set(seeds), set(seeds)
        for _ in range(self.opts.focus_hops):
            nxt = set()
            for nid in frontier:
                for nb in adj.get(nid, set()):
                    if nb not in visited:
                        visited.add(nb)
                        nxt.add(nb)
            frontier = nxt
        return visited


if __name__ == "__main__":
    sys.exit(main())
