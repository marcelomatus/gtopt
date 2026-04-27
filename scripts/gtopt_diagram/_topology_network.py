# SPDX-License-Identifier: BSD-3-Clause
"""Network-element builders for :class:`gtopt_diagram.TopologyBuilder`.

Mixin extracted from :mod:`gtopt_diagram.gtopt_diagram`.  Holds the
methods that walk the electrical-network arrays of the JSON system
(buses, generators with the four aggregation modes, demands, lines,
batteries, converters, profiles) and stamp the corresponding nodes
and edges onto ``self.model``.

The mixin assumes the host class provides:

* ``self.sys``           — the parsed ``system`` JSON dict
* ``self.opts``          — :class:`FilterOptions`
* ``self.model``         — :class:`GraphModel` accumulator
* ``self._vmap``         — voltage-level bus reduction map
* ``self._bus_node_ids`` — set of bus node IDs visible in the model
* ``self._turb_refs``    — turbine→generator reference map
* ``self._resolve_field``, ``self._bid``, ``self._gid``, … — methods
  inherited via :class:`gtopt_diagram._topology_ids.TopologyIdsMixin`.
"""

from __future__ import annotations

from collections import defaultdict
from typing import Any, Optional

from gtopt_diagram._classify import (
    GEN_TYPE_META as _GEN_TYPE_META,
    classify_gen as _classify_gen,
    dominant_kind as _dominant_kind,
    elem_name as _elem_name,
    gen_pmax as _gen_pmax,
    scalar as _scalar,
)
from gtopt_diagram._data_utils import (
    resolve_bus_ref as _resolve_bus_ref,
)
from gtopt_diagram._graph_model import (
    Edge,
    FilterOptions,
    GraphModel,
    Node,
)
from gtopt_diagram._svg_constants import (
    _PALETTE,
    _line_color_width,
)
from gtopt_diagram._topology_ids import TopologyIdsMixin


class TopologyNetworkMixin(TopologyIdsMixin):
    """Composable network-element builders (buses, gens, demands, lines, …)."""

    sys: dict[str, Any]
    opts: FilterOptions
    model: GraphModel
    _vmap: dict[str, str]
    _bus_node_ids: set
    _turb_refs: set
    # Resolved at build() time, used by the aggregation methods below.
    _eff_agg: str
    _focus_nids: set | None

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

    def _resolve_gen_node_id(self, gen_ref) -> Optional[str]:
        """Return the id of the node that represents *gen_ref* in the current view.

        Honours the active aggregation mode: when generators are collapsed into
        ``agg_bus_*`` / ``agg_type_*`` / ``agg_global_*`` super-nodes, returns
        the id of the matching super-node so that profile/converter/turbine
        edges still land on something visible. Falls back to the individual
        generator id in ``none`` mode.
        """
        gen = self._find("generator_array", gen_ref)
        if gen is None:
            return None
        agg = self._eff_agg
        if agg == "none":
            return self._gid(gen)
        if agg == "bus":
            return f"agg_bus_{gen.get('bus')}"
        if agg == "type":
            gt = _classify_gen(gen, self._turb_refs)
            return f"agg_type_{gen.get('bus')}_{gt}"
        if agg == "global":
            gt = _classify_gen(gen, self._turb_refs)
            return f"agg_global_{gt}"
        return self._gid(gen)

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
            gen_id = self._resolve_gen_node_id(gp.get("generator"))
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
