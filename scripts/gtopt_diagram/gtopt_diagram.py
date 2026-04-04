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
``electrical`` — Buses, generators, demands, lines, batteries, converters.
``hydro``      — Junctions, waterways, reservoirs, turbines, flows, seepages.
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
from gtopt_diagram._classify import GEN_TYPE_META as _GEN_TYPE_META
from gtopt_diagram._data_utils import (
    auto_voltage_threshold,
    build_voltage_map as _build_voltage_map_impl,
    count_visible_buses as _count_visible_buses_impl,
    resolve_bus_ref as _resolve_bus_ref_impl,
)
from gtopt_diagram._graph_model import (
    AUTO_BUS_THRESHOLD as _AUTO_BUS_THRESHOLD,
    AUTO_MAX_HV_BUSES as _AUTO_MAX_HV_BUSES,
    AUTO_NONE_THRESHOLD as _AUTO_NONE_THRESHOLD,
    Edge,
    FilterOptions,
    GraphModel,
    Node,
)

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


class TopologyBuilder:
    """Build a GraphModel from a gtopt JSON planning structure."""

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

    @staticmethod
    def _make_id(prefix: str, item: dict) -> str:
        """Build a graph node ID from prefix, element name, and uid.

        Uses ``prefix_NAME_uid`` format to guarantee uniqueness across
        element types that may share the same name/uid (e.g. a turbine
        and its linked generator often share the same uid and name).
        """
        name = item.get("name")
        uid = item.get("uid")
        if name is not None:
            safe = str(name).replace(" ", "_").replace("-", "_")
            return f"{prefix}_{safe}_{uid}" if uid is not None else f"{prefix}_{safe}"
        return f"{prefix}_{uid}" if uid is not None else f"{prefix}_?"

    @staticmethod
    def _bid(b):
        return TopologyBuilder._make_id("bus", b)

    @staticmethod
    def _gid(g):
        return TopologyBuilder._make_id("gen", g)

    @staticmethod
    def _did(d):
        return TopologyBuilder._make_id("dem", d)

    @staticmethod
    def _batid(b):
        return TopologyBuilder._make_id("bat", b)

    @staticmethod
    def _cid(c):
        return TopologyBuilder._make_id("conv", c)

    @staticmethod
    def _jid(j):
        return TopologyBuilder._make_id("junc", j)

    @staticmethod
    def _rid(r):
        return TopologyBuilder._make_id("res", r)

    @staticmethod
    def _tid(t):
        return TopologyBuilder._make_id("turb", t)

    @staticmethod
    def _fid(f):
        return TopologyBuilder._make_id("flow", f)

    @staticmethod
    def _filtid(fi):
        return TopologyBuilder._make_id("filt", fi)

    @staticmethod
    def _rzid(rz):
        return TopologyBuilder._make_id("rzone", rz)

    @staticmethod
    def _rpid(rp):
        return TopologyBuilder._make_id("rprov", rp)

    @staticmethod
    def _gpid(gp):
        return TopologyBuilder._make_id("gprof", gp)

    @staticmethod
    def _dpid(dp):
        return TopologyBuilder._make_id("dprof", dp)

    @staticmethod
    def _vrid(vr):
        return TopologyBuilder._make_id("vright", vr)

    @staticmethod
    def _frid(fr):
        return TopologyBuilder._make_id("fright", fr)

    def _resolve_field(
        self, class_name: str, elem: dict, field: str, fallback: str = "—"
    ) -> str:
        """Resolve a field value, reading from Parquet if it's a file reference.

        Returns a formatted string suitable for display in labels.
        """
        val = elem.get(field)
        if val is None:
            return fallback
        if isinstance(val, (int, float)):
            return f"{val:,.1f}" if isinstance(val, float) else str(val)
        if isinstance(val, str) and self._resolver is not None:
            resolved = self._resolver.resolve(
                class_name, val, elem.get("uid"), fallback=None
            )
            if resolved is not None:
                return f"{resolved:,.1f}"
        if isinstance(val, str):
            return fallback  # unresolved file reference
        return _scalar(val)

    def _find(self, arr_key, ref):
        return _resolve(self.sys.get(arr_key, []), ref)

    def _find_node_id(self, arr_key, ref, id_fn):
        item = self._find(arr_key, ref)
        return id_fn(item) if item else None

    def _bus_node_id(self, bus_ref):
        rep = _resolve_bus_ref(bus_ref, self._vmap)
        bus = self._find("bus_array", rep) or self._find("bus_array", bus_ref)
        if bus is None:
            return None
        nid = self._bid(bus)
        return nid if nid in self._bus_node_ids else None

    def _gen_kind(self, gen):
        gt = _classify_gen(gen, self._turb_refs)
        return {"hydro": "gen_hydro", "solar": "gen_solar", "wind": "gen_solar"}.get(
            gt, "gen"
        )

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
        if s in ("full", "hydro"):
            self._junctions()
            self._waterways()
            self._reservoirs()
            self._turbines()
            self._flows()
            self._seepages()
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

    def _buses(self):
        seen = set()
        for bus in self.sys.get("bus_array", []):
            ref = bus.get("uid") if bus.get("uid") is not None else bus.get("name")
            rep = _resolve_bus_ref(ref, self._vmap)
            if rep != ref:
                continue
            nid = self._bid(bus)
            if nid in seen:
                continue
            seen.add(nid)
            if self._focus_nids is not None and nid not in self._focus_nids:
                continue
            v = f"\n{bus['voltage']} kV" if "voltage" in bus else ""
            name = _elem_name(bus)
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=f"{name}{v}",
                    kind="bus",
                    cluster="electrical",
                    tooltip=f"Bus uid={bus.get('uid')} name={bus.get('name')}{v}",
                )
            )
            self._bus_node_ids.add(nid)

    def _generators(self):
        gens = self.sys.get("generator_array", [])
        ftypes = [t.lower() for t in self.opts.filter_types]
        if ftypes:
            gens = [g for g in gens if _classify_gen(g, self._turb_refs) in ftypes]
        agg = self._eff_agg
        if agg == "none":
            self._gen_individual(gens)
        elif agg == "bus":
            self._gen_agg_bus(gens)
        elif agg == "type":
            self._gen_agg_type(gens)
        else:
            self._gen_agg_global(gens)

    def _gen_individual(self, gens):
        for gen in gens:
            # Capacity first, pmax as fallback
            cap = self._resolve_field("Generator", gen, "capacity", fallback=None)
            if cap is None:
                cap = self._resolve_field("Generator", gen, "pmax", fallback="—")
            gt = _classify_gen(gen, self._turb_refs)
            kind = self._gen_kind(gen)
            name = _elem_name(gen)
            lbl = f"{name}\n{cap} MW" if self.opts.compact else f"{name}\n{cap} MW"
            nid = self._gid(gen)
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=kind,
                    cluster="electrical",
                    tooltip=f"Generator uid={gen.get('uid')} type={gt} capacity={cap}",
                )
            )
            bus_id = self._bus_node_id(gen.get("bus"))
            if bus_id:
                self.model.add_edge(Edge(nid, bus_id, color=_PALETTE["bus_border"]))

    def _gen_agg_bus(self, gens):
        by_bus = defaultdict(list)
        for g in gens:
            by_bus[g.get("bus")].append(g)
        for bus_ref, grp in by_bus.items():
            bus_id = self._bus_node_id(bus_ref)
            if bus_id is None:
                continue
            if self.opts.top_gens > 0:
                grp = sorted(grp, key=_gen_pmax, reverse=True)[: self.opts.top_gens]
            total = sum(_gen_pmax(g) for g in grp)
            rep = _resolve_bus_ref(bus_ref, self._vmap)
            bus = self._find("bus_array", rep) or self._find("bus_array", bus_ref)
            bname = (
                _elem_name(bus)
                if bus
                else (bus_ref if isinstance(bus_ref, str) else f"bus{bus_ref}")
            )
            types = [_classify_gen(g, self._turb_refs) for g in grp]
            kind = _dominant_kind(types)
            nid = f"agg_bus_{bus_ref}"
            lbl = f"{bname} generators\n{len(grp)} units · {total:.0f} MW"
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=kind,
                    cluster="electrical",
                    tooltip=f"Agg. generators at {bname}: {len(grp)} units, {total:.0f} MW",
                )
            )
            self.model.add_edge(Edge(nid, bus_id, color=_PALETTE["bus_border"]))

    def _gen_agg_type(self, gens):
        by_bus_type = defaultdict(list)
        for g in gens:
            gt = _classify_gen(g, self._turb_refs)
            by_bus_type[(g.get("bus"), gt)].append(g)
        for (bus_ref, gt), grp in by_bus_type.items():
            bus_id = self._bus_node_id(bus_ref)
            if bus_id is None:
                continue
            if self.opts.top_gens > 0:
                grp = sorted(grp, key=_gen_pmax, reverse=True)[: self.opts.top_gens]
            total = sum(_gen_pmax(g) for g in grp)
            rep = _resolve_bus_ref(bus_ref, self._vmap)
            bus = self._find("bus_array", rep) or self._find("bus_array", bus_ref)
            bname = (
                _elem_name(bus)
                if bus
                else (bus_ref if isinstance(bus_ref, str) else f"bus{bus_ref}")
            )
            meta = _GEN_TYPE_META.get(gt, ("?", "⚡", "gen"))
            label, icon, palette_key = meta
            nid = f"agg_type_{bus_ref}_{gt}"
            lbl = f"{icon} {label} @ {bname}\n{len(grp)} units · {total:.0f} MW"
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=palette_key,
                    cluster="electrical",
                    tooltip=f"{label} at {bname}: {len(grp)} units, {total:.0f} MW",
                )
            )
            self.model.add_edge(Edge(nid, bus_id, color=_PALETTE["bus_border"]))

    def _gen_agg_global(self, gens):
        by_type = defaultdict(list)
        for g in gens:
            by_type[_classify_gen(g, self._turb_refs)].append(g)
        for gt, grp in by_type.items():
            if self.opts.top_gens > 0:
                grp = sorted(grp, key=_gen_pmax, reverse=True)[: self.opts.top_gens]
            total = sum(_gen_pmax(g) for g in grp)
            meta = _GEN_TYPE_META.get(gt, ("?", "⚡", "gen"))
            label, icon, palette_key = meta
            nid = f"agg_global_{gt}"
            lbl = f"{icon} {label}\n{len(grp)} units · {total:.0f} MW total"
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=palette_key,
                    cluster="electrical",
                    tooltip=f"All {label}: {len(grp)} units, {total:.0f} MW",
                )
            )
            seen_buses: set = set()
            for gen in grp:
                bus_id = self._bus_node_id(gen.get("bus"))
                if bus_id and bus_id not in seen_buses:
                    seen_buses.add(bus_id)
                    self.model.add_edge(Edge(nid, bus_id, color=_PALETTE["bus_border"]))

    def _demands(self):
        for dem in self.sys.get("demand_array", []):
            name = _elem_name(dem)
            lmax = self._resolve_field("Demand", dem, "lmax")
            nid = self._did(dem)
            lbl = f"{name}" if self.opts.compact else f"{name}\n{lmax} MW"
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind="demand",
                    cluster="electrical",
                    tooltip=f"Demand uid={dem.get('uid')} name={dem.get('name')} lmax={lmax}",
                )
            )
            bus_id = self._bus_node_id(dem.get("bus"))
            if bus_id:
                self.model.add_edge(Edge(bus_id, nid, color=_PALETTE["demand_border"]))

    def _lines(self):
        seen_edges: set = set()
        for line in self.sys.get("line_array", []):
            a_ref = line.get("bus_a")
            b_ref = line.get("bus_b")
            rep_a = _resolve_bus_ref(a_ref, self._vmap)
            rep_b = _resolve_bus_ref(b_ref, self._vmap)
            if rep_a == rep_b and self.opts.voltage_threshold > 0:
                continue
            ba = self._find("bus_array", rep_a) or self._find("bus_array", a_ref)
            bb = self._find("bus_array", rep_b) or self._find("bus_array", b_ref)
            if not ba or not bb:
                continue
            aid, bid2 = self._bid(ba), self._bid(bb)
            if aid not in self._bus_node_ids or bid2 not in self._bus_node_ids:
                continue
            if aid == bid2:
                continue
            # De-duplicate parallel lines in voltage-aggregated mode
            edge_key = tuple(sorted([aid, bid2]))
            if self.opts.voltage_threshold > 0 and edge_key in seen_edges:
                continue
            seen_edges.add(edge_key)
            name = _elem_name(line)
            tmax = self._resolve_field("Line", line, "tmax_ab", fallback=None)
            if tmax is None:
                tmax = self._resolve_field("Line", line, "tmax_ba", fallback="—")

            # Derive voltage from connected buses for color/width
            va = ba.get("voltage", 0) if ba else 0
            vb = bb.get("voltage", 0) if bb else 0
            line_kv = max(float(va or 0), float(vb or 0))

            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [str(name)]
                if line_kv > 0:
                    parts.append(f"{line_kv:.0f} kV")
                if tmax and tmax != "—":
                    parts.append(f"{tmax} MW")
                lbl = "\n".join(parts)

            color, width = _line_color_width(line_kv)
            self.model.add_edge(
                Edge(
                    src=aid,
                    dst=bid2,
                    label=lbl,
                    color=color,
                    directed=False,
                    weight=width,
                )
            )

    def _batteries(self):
        for bat in self.sys.get("battery_array", []):
            name = _elem_name(bat)
            emax = self._resolve_field("Battery", bat, "emax", fallback=None)
            if emax is None:
                emax = self._resolve_field("Battery", bat, "capacity", fallback="—")
            ein = self._resolve_field("Battery", bat, "input_efficiency", fallback="")
            eout = self._resolve_field("Battery", bat, "output_efficiency", fallback="")
            eff = f"\n\u03b7={ein}/{eout}" if ein else ""
            lbl = f"{name}" if self.opts.compact else f"{name}\n{emax} MWh{eff}"
            nid = self._batid(bat)
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind="battery",
                    cluster="electrical",
                    tooltip=f"Battery uid={bat.get('uid')} emax={emax}",
                )
            )
            # Connect battery to bus: use converter if defined, otherwise
            # connect directly to bus (C++ expand_batteries auto-creates
            # the converter/gen/demand at LP construction time).
            has_converter = any(
                c.get("battery") in (bat.get("uid"), bat.get("name"))
                for c in self.sys.get("converter_array", [])
            )
            if not has_converter:
                bus_id = self._bus_node_id(bat.get("bus"))
                if bus_id:
                    self.model.add_edge(
                        Edge(nid, bus_id, color=_PALETTE["battery_border"])
                    )

    def _converters(self):
        for conv in self.sys.get("converter_array", []):
            name = _elem_name(conv)
            cap = _scalar(conv.get("capacity"))
            cid = self._cid(conv)
            lbl = f"{name}" if self.opts.compact else f"{name}\n{cap} MW"
            self.model.add_node(
                Node(
                    node_id=cid,
                    label=lbl,
                    kind="converter",
                    cluster="electrical",
                    tooltip=f"Converter uid={conv.get('uid')} cap={cap}",
                )
            )
            bat_id = self._find_node_id(
                "battery_array", conv.get("battery"), self._batid
            )
            gen_id = self._find_node_id(
                "generator_array", conv.get("generator"), self._gid
            )
            dem_id = self._find_node_id("demand_array", conv.get("demand"), self._did)
            if bat_id:
                self.model.add_edge(
                    Edge(
                        bat_id,
                        cid,
                        label="stored\nenergy",
                        style="dashed",
                        color=_PALETTE["bat_link_edge"],
                    )
                )
            if gen_id:
                self.model.add_edge(
                    Edge(
                        cid,
                        gen_id,
                        label="discharge",
                        style="dashed",
                        color=_PALETTE["bat_link_edge"],
                    )
                )
            if dem_id:
                self.model.add_edge(
                    Edge(
                        dem_id,
                        cid,
                        label="charge",
                        style="dashed",
                        color=_PALETTE["bat_link_edge"],
                    )
                )

    def _generator_profiles(self):
        """Draw generator profile nodes linked to their generators."""
        for gp in self.sys.get("generator_profile_array", []):
            name = _elem_name(gp)
            gpid = self._gpid(gp)
            profile = gp.get("profile", "")
            plbl = str(profile)[:20] if isinstance(profile, str) else "inline"
            lbl = str(name) if self.opts.compact else f"[GenProfile] {name}\n{plbl}"
            self.model.add_node(
                Node(
                    node_id=gpid,
                    label=lbl,
                    kind="gen_profile",
                    cluster="electrical",
                    tooltip=f"GeneratorProfile uid={gp.get('uid')} profile={plbl}",
                )
            )
            gen_id = self._find_node_id(
                "generator_array", gp.get("generator"), self._gid
            )
            if gen_id:
                self.model.add_edge(
                    Edge(
                        gpid,
                        gen_id,
                        label="profile",
                        style="dotted",
                        color=_PALETTE.get("profile_edge", "#95A5A6"),
                    )
                )

    def _demand_profiles(self):
        """Draw demand profile nodes linked to their demands."""
        for dp in self.sys.get("demand_profile_array", []):
            name = _elem_name(dp)
            dpid = self._dpid(dp)
            profile = dp.get("profile", "")
            plbl = str(profile)[:20] if isinstance(profile, str) else "inline"
            lbl = str(name) if self.opts.compact else f"[DemProfile] {name}\n{plbl}"
            self.model.add_node(
                Node(
                    node_id=dpid,
                    label=lbl,
                    kind="dem_profile",
                    cluster="electrical",
                    tooltip=f"DemandProfile uid={dp.get('uid')} profile={plbl}",
                )
            )
            dem_id = self._find_node_id("demand_array", dp.get("demand"), self._did)
            if dem_id:
                self.model.add_edge(
                    Edge(
                        dpid,
                        dem_id,
                        label="profile",
                        style="dotted",
                        color=_PALETTE.get("profile_edge", "#95A5A6"),
                    )
                )

    def _junctions(self):
        for j in self.sys.get("junction_array", []):
            name = _elem_name(j)
            uid = j.get("uid")
            is_drain = j.get("drain", False)
            drain_lbl = " [drain]" if is_drain and not self.opts.compact else ""
            self.model.add_node(
                Node(
                    node_id=self._jid(j),
                    label=f"{name}{drain_lbl}",
                    kind="junction",
                    cluster="hydro",
                    tooltip=(
                        f"Junction uid={uid} name={j.get('name')} drain={is_drain}"
                    ),
                )
            )

    def _waterways(self):
        for w in self.sys.get("waterway_array", []):
            name = _elem_name(w)
            uid = w.get("uid")
            # Skip direct arc when a turbine already represents this waterway
            if uid in self._turb_way_refs or name in self._turb_way_refs:
                continue
            ja = self._find_node_id("junction_array", w.get("junction_a"), self._jid)
            jb = self._find_node_id("junction_array", w.get("junction_b"), self._jid)
            if ja and jb:
                lbl = str(name)
                self.model.add_edge(
                    Edge(
                        src=ja,
                        dst=jb,
                        label=lbl,
                        color=_PALETTE["waterway_edge"],
                        directed=False,
                        weight=1.5,
                    )
                )

    def _reservoirs(self):
        # Collect all emax values to compute relative sizes and colors
        all_emax = []
        for r in self.sys.get("reservoir_array", []):
            val = _gen_pmax({"pmax": r.get("emax") or r.get("capacity")})
            if val > 0:
                all_emax.append(val)
        max_emax = max(all_emax) if all_emax else 1.0

        for r in self.sys.get("reservoir_array", []):
            name = _elem_name(r)
            emax_val = _gen_pmax({"pmax": r.get("emax") or r.get("capacity")})
            emax = self._resolve_field("Reservoir", r, "emax", fallback=None)
            if emax is None:
                emax = self._resolve_field("Reservoir", r, "capacity", fallback="—")
            lbl = str(name) if self.opts.compact else f"{name}\n{emax} dam\u00b3"
            # Relative size: 0.0 (smallest) to 1.0 (largest)
            rel = emax_val / max_emax if max_emax > 0 else 0.5
            node_size = 20.0 + 30.0 * rel
            self.model.add_node(
                Node(
                    node_id=self._rid(r),
                    label=lbl,
                    kind=f"reservoir_{_reservoir_intensity(rel)}",
                    cluster="hydro",
                    tooltip=f"Reservoir uid={r.get('uid')} emax={emax}",
                    size=node_size,
                )
            )
            junc_id = self._find_node_id("junction_array", r.get("junction"), self._jid)
            if junc_id:
                self.model.add_edge(
                    Edge(
                        self._rid(r),
                        junc_id,
                        style="dashed",
                        color=_PALETTE["reservoir_border"],
                    )
                )

    def _turbines(self):
        for t in self.sys.get("turbine_array", []):
            name = _elem_name(t)
            cap = self._resolve_field("Turbine", t, "capacity")
            cr = self._resolve_field("Turbine", t, "conversion_rate")
            tid = self._tid(t)
            lbl = (
                str(name)
                if self.opts.compact
                else f"[Turbine] {name}\n{cap} MW  cr={cr}"
            )
            self.model.add_node(
                Node(
                    node_id=tid,
                    label=lbl,
                    kind="turbine",
                    cluster="hydro",
                    tooltip=f"Turbine uid={t.get('uid')} cap={cap}",
                )
            )
            # Connect turbine to water source: waterway OR flow
            flow_ref = t.get("flow")
            way_ref = t.get("waterway")
            if flow_ref:
                # Flow-turbine mode: direct flow → turbine edge
                flow_id = self._find_node_id("flow_array", flow_ref, self._fid)
                if flow_id:
                    self.model.add_edge(
                        Edge(
                            flow_id,
                            tid,
                            label="discharge",
                            color=_PALETTE["waterway_edge"],
                            directed=False,
                            weight=0.3,
                        )
                    )
            elif way_ref:
                way = _resolve(self.sys.get("waterway_array", []), way_ref)
                if way:
                    ja = self._find_node_id(
                        "junction_array", way.get("junction_a"), self._jid
                    )
                    jb = self._find_node_id(
                        "junction_array", way.get("junction_b"), self._jid
                    )
                    way_name = _elem_name(way)
                    lbl_w = str(way_name)
                    if ja:
                        self.model.add_edge(
                            Edge(
                                ja,
                                tid,
                                label=lbl_w,
                                color=_PALETTE["waterway_edge"],
                                directed=False,
                            )
                        )
                    if jb:
                        self.model.add_edge(
                            Edge(
                                tid,
                                jb,
                                color=_PALETTE["waterway_edge"],
                                directed=False,
                            )
                        )
            gen_ref = t.get("generator")
            gen = _resolve(self.sys.get("generator_array", []), gen_ref)
            if gen is None:
                logger.warning(
                    "Turbine %s references generator %r which is not in "
                    "generator_array; skipping turbine-generator edge.",
                    _elem_name(t),
                    gen_ref,
                )
            gen_id = self._gid(gen) if gen else None
            if gen_id:
                # In hydro subsystem the generator node may not exist yet;
                # add it so the turbine → generator "power out" edge renders.
                if gen_id not in {n.node_id for n in self.model.nodes}:
                    if gen:
                        gname = _elem_name(gen)
                        cap = self._resolve_field(
                            "Generator", gen, "capacity", fallback=None
                        )
                        pmax = (
                            cap
                            if cap is not None
                            else self._resolve_field(
                                "Generator", gen, "pmax", fallback="—"
                            )
                        )
                        gtype = gen.get("type", "hydro")
                        glbl = (
                            str(gname)
                            if self.opts.compact
                            else f"[Gen {gtype}] {gname}\n{pmax} MW"
                        )
                        self.model.add_node(
                            Node(
                                node_id=gen_id,
                                label=glbl,
                                kind="gen_hydro",
                                cluster="hydro",
                                tooltip=f"Generator uid={gen.get('uid')} type={gtype} pmax={pmax}",
                            )
                        )
                        # Connect generator to its bus (auto-create bus
                        # node if needed, e.g. in hydro-only subsystem)
                        bus_ref = gen.get("bus")
                        if bus_ref is not None:
                            bus = self._find("bus_array", bus_ref)
                            if bus:
                                bid = self._bid(bus)
                                if bid not in {n.node_id for n in self.model.nodes}:
                                    bname = _elem_name(bus)
                                    bv = (
                                        f"\n{bus['voltage']} kV"
                                        if "voltage" in bus
                                        else ""
                                    )
                                    self.model.add_node(
                                        Node(
                                            node_id=bid,
                                            label=f"{bname}{bv}",
                                            kind="bus",
                                            cluster="electrical",
                                        )
                                    )
                                self.model.add_edge(
                                    Edge(
                                        gen_id,
                                        bid,
                                        color=_PALETTE["gen_hydro_border"],
                                    )
                                )
                self.model.add_edge(
                    Edge(
                        tid,
                        gen_id,
                        label="power out",
                        color=_PALETTE["power_edge"],
                        style="dashed",
                    )
                )
            # Draw main_reservoir → turbine edge when not already covered
            # by a reservoir_production_factor_array entry (to avoid duplication).
            uid = t.get("uid")
            name_ref = t.get("name")
            if uid not in self._eff_turb_refs and name_ref not in self._eff_turb_refs:
                res_id = self._find_node_id(
                    "reservoir_array", t.get("main_reservoir"), self._rid
                )
                if res_id:
                    lbl_e = "" if self.opts.compact else "head"
                    self.model.add_edge(
                        Edge(
                            res_id,
                            tid,
                            label=lbl_e,
                            style="dashed",
                            color=_PALETTE["efficiency_edge"],
                        )
                    )

    def _flows(self):
        for f in self.sys.get("flow_array", []):
            name = _elem_name(f)
            disc = self._resolve_field("Flow", f, "discharge")
            direction = f.get("direction", 1)
            fid = self._fid(f)
            lbl = str(name) if self.opts.compact else f"{name}\n{disc} m\u00b3/s"
            self.model.add_node(
                Node(
                    node_id=fid,
                    label=lbl,
                    kind="flow",
                    cluster="hydro",
                    tooltip=f"Flow uid={f.get('uid')} discharge={disc}",
                    size=12.0,
                )
            )
            junc_id = self._find_node_id("junction_array", f.get("junction"), self._jid)
            if junc_id:
                src, dst = (fid, junc_id) if direction >= 0 else (junc_id, fid)
                # Short edge: keep flow nodes close to their junction
                self.model.add_edge(
                    Edge(
                        src,
                        dst,
                        color=_PALETTE["waterway_edge"],
                        directed=False,
                        weight=0.3,
                    )
                )

    def _seepages(self):
        for fi in self.sys.get("reservoir_seepage_array", []):
            name = _elem_name(fi)
            fiid = self._filtid(fi)
            lbl = str(name) if self.opts.compact else f"{name}\n(seepage)"
            self.model.add_node(
                Node(
                    node_id=fiid,
                    label=lbl,
                    kind="seepage",
                    cluster="hydro",
                    tooltip=f"ReservoirSeepage uid={fi.get('uid')} name={fi.get('name')}",
                )
            )
            wway = _resolve(self.sys.get("waterway_array", []), fi.get("waterway"))
            res_id = self._find_node_id(
                "reservoir_array", fi.get("reservoir"), self._rid
            )
            if wway:
                ja = self._find_node_id(
                    "junction_array", wway.get("junction_a"), self._jid
                )
                if ja:
                    self.model.add_edge(
                        Edge(
                            ja,
                            fiid,
                            style="dotted",
                            color=_PALETTE["seepage_border"],
                            directed=False,
                        )
                    )
            if res_id:
                self.model.add_edge(
                    Edge(
                        fiid,
                        res_id,
                        style="dotted",
                        color=_PALETTE["seepage_border"],
                        directed=False,
                    )
                )

    def _volume_rights(self):
        """Draw VolumeRight nodes attached to their source reservoir.

        Each VolumeRight is NOT part of the hydrological topology; it is an
        accounting entity tracking water volume entitlements.  Displayed as a
        separate node with a dotted edge to the referenced reservoir, and an
        optional dotted edge to a parent VolumeRight (right_reservoir).
        """
        for vr in self.sys.get("volume_right_array", []):
            name = _elem_name(vr)
            vrid = self._vrid(vr)
            purpose = vr.get("purpose", "")
            emax = self._resolve_field("VolumeRight", vr, "emax", fallback=None)
            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [f"[VolRight] {name}"]
                if purpose:
                    parts.append(str(purpose))
                if emax is not None:
                    parts.append(f"{emax} hm\u00b3")
                lbl = "\n".join(parts)
            self.model.add_node(
                Node(
                    node_id=vrid,
                    label=lbl,
                    kind="volume_right",
                    cluster="hydro",
                    tooltip=(
                        f"VolumeRight uid={vr.get('uid')} name={vr.get('name')}"
                        f" purpose={purpose}"
                    ),
                    size=18.0,
                )
            )
            # Edge to physical source reservoir
            res_id = self._find_node_id(
                "reservoir_array", vr.get("reservoir"), self._rid
            )
            if res_id:
                self.model.add_edge(
                    Edge(
                        res_id,
                        vrid,
                        label="" if self.opts.compact else "source",
                        style="dotted",
                        color=_PALETTE["right_edge"],
                        directed=True,
                        weight=0.5,
                    )
                )
            # Edge to parent VolumeRight (hierarchical rights structure)
            parent_id = self._find_node_id(
                "volume_right_array", vr.get("right_reservoir"), self._vrid
            )
            if parent_id:
                direction = vr.get("direction", -1)
                lbl_e = "" if self.opts.compact else ("→" if direction >= 0 else "←")
                self.model.add_edge(
                    Edge(
                        vrid,
                        parent_id,
                        label=lbl_e,
                        style="dotted",
                        color=_PALETTE["right_edge"],
                        directed=True,
                        weight=0.3,
                    )
                )

    def _flow_rights(self):
        """Draw FlowRight nodes attached to their reference junction.

        Each FlowRight is NOT part of the hydrological topology; it is an
        accounting entity tracking flow (m³/s) entitlements at a junction.
        Displayed as a separate node with a dotted edge to the referenced
        junction.
        """
        for fr in self.sys.get("flow_right_array", []):
            name = _elem_name(fr)
            frid = self._frid(fr)
            purpose = fr.get("purpose", "")
            discharge = self._resolve_field("FlowRight", fr, "discharge", fallback=None)
            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [f"[FlowRight] {name}"]
                if purpose:
                    parts.append(str(purpose))
                if discharge is not None:
                    parts.append(f"{discharge} m\u00b3/s")
                lbl = "\n".join(parts)
            self.model.add_node(
                Node(
                    node_id=frid,
                    label=lbl,
                    kind="flow_right",
                    cluster="hydro",
                    tooltip=(
                        f"FlowRight uid={fr.get('uid')} name={fr.get('name')}"
                        f" purpose={purpose}"
                    ),
                    size=14.0,
                )
            )
            # Edge to reference junction
            junc_id = self._find_node_id(
                "junction_array", fr.get("junction"), self._jid
            )
            if junc_id:
                self.model.add_edge(
                    Edge(
                        junc_id,
                        frid,
                        label="" if self.opts.compact else "extraction",
                        style="dotted",
                        color=_PALETTE["right_edge"],
                        directed=True,
                        weight=0.3,
                    )
                )

    def _reservoir_efficiencies(self):
        """Draw turbine-reservoir efficiency relationships.

        Each entry in ``reservoir_production_factor_array`` associates a turbine with
        a reservoir whose volume drives the turbine's conversion rate (hydraulic
        head effect).  The relationship is drawn as a dashed edge
        ``reservoir → turbine`` coloured with ``efficiency_edge``.
        """
        for e in self.sys.get("reservoir_production_factor_array", []):
            res_id = self._find_node_id(
                "reservoir_array", e.get("reservoir"), self._rid
            )
            turb_id = self._find_node_id("turbine_array", e.get("turbine"), self._tid)
            if res_id and turb_id:
                lbl = "" if self.opts.compact else "head"
                self.model.add_edge(
                    Edge(
                        res_id,
                        turb_id,
                        label=lbl,
                        style="dashed",
                        color=_PALETTE["efficiency_edge"],
                    )
                )

    def _reserve_zones(self):
        """Draw reserve zone nodes as dotted-outline clusters."""
        for rz in self.sys.get("reserve_zone_array", []):
            name = _elem_name(rz)
            rzid = self._rzid(rz)
            lbl = str(name) if self.opts.compact else f"[Reserve Zone] {name}"
            self.model.add_node(
                Node(
                    node_id=rzid,
                    label=lbl,
                    kind="reserve_zone",
                    cluster="electrical",
                    tooltip=f"ReserveZone uid={rz.get('uid')} name={rz.get('name')}",
                )
            )

    def _reserve_provisions(self):
        """Draw reserve provision edges: generator → reserve_zone."""
        for rp in self.sys.get("reserve_provision_array", []):
            gen_id = self._find_node_id(
                "generator_array", rp.get("generator"), self._gid
            )
            # reserve_zones is a colon-separated string of zone refs
            rz_str = rp.get("reserve_zones", "")
            if isinstance(rz_str, str) and rz_str:
                for rz_ref in rz_str.split(":"):
                    rz_ref = rz_ref.strip()
                    if not rz_ref:
                        continue
                    rz_id = self._find_node_id("reserve_zone_array", rz_ref, self._rzid)
                    if gen_id and rz_id:
                        self.model.add_edge(
                            Edge(
                                gen_id,
                                rz_id,
                                label="reserve",
                                style="dotted",
                                color=_PALETTE.get(
                                    "reserve_edge", _PALETTE["gen_border"]
                                ),
                            )
                        )
            elif isinstance(rz_str, (list, tuple)):
                for rz_ref in rz_str:
                    rz_id = self._find_node_id("reserve_zone_array", rz_ref, self._rzid)
                    if gen_id and rz_id:
                        self.model.add_edge(
                            Edge(
                                gen_id,
                                rz_id,
                                label="reserve",
                                style="dotted",
                                color=_PALETTE.get(
                                    "reserve_edge", _PALETTE["gen_border"]
                                ),
                            )
                        )


if __name__ == "__main__":
    sys.exit(main())
