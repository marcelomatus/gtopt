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
                    tooltip=(
                        f"Bus {bus.get('name')} (uid={bus.get('uid')})"
                        + (f" — {bus['voltage']} kV" if "voltage" in bus else "")
                    ),
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

    @staticmethod
    def _common_stem(names: list[str]) -> str:
        """Return a human-readable common name root for a list of names.

        Strategy:
          1. Longest common prefix of all names.
          2. Strip trailing separator / digit / single-letter variants
             (``_``, ``-``, ``.``, space, ``[0-9]``, ``[A-Z]``).
          3. If the result is too short (< 3 chars), fall back to the
             first name itself (the user wanted the base name; better
             to show the first unit than an unhelpful 1-2 char prefix).
        """
        if not names:
            return ""
        if len(names) == 1:
            return names[0]
        first = names[0]
        n = len(first)
        for nm in names[1:]:
            n = min(n, len(nm))
            while n > 0 and first[:n] != nm[:n]:
                n -= 1
            if n == 0:
                return first
        stem = first[:n]
        # Strip trailing junk so "COLBUN_" becomes "COLBUN" and
        # "ATA-T" stays "ATA-T".
        stem = stem.rstrip("_-. ")
        # If the stem ends with one trailing letter or digit AND the
        # NEXT char in every member is the same family (so the
        # trailing char is the variant suffix), drop it.  Eg first
        # name "ATA_TG1A", second "ATA_TG1B" → LCP "ATA_TG1" — already
        # clean.  But "Plant1A" / "Plant1B" → LCP "Plant1" → drop "1".
        # Conservative: only strip a single trailing digit-or-letter
        # if every member's next char (at position n-1) is a single
        # variant char [A-Z0-9].
        if (
            len(stem) >= 4
            and stem[-1].isalnum()
            and all(n - 1 < len(nm) and nm[n - 1 : n].isalnum() for nm in names)
            # Only strip when names AFTER the stem actually diverge —
            # never strip a digit that's part of a shared word.
            and len({nm[n - 1 : n] for nm in names}) > 1
        ):
            stem = stem[:-1].rstrip("_-. ")
        if len(stem) < 3:
            return first
        return stem

    def _gen_individual(self, gens):
        for gen in gens:
            # Capacity first, pmax as fallback
            cap = self._resolve_field("Generator", gen, "capacity", fallback=None)
            if cap is None:
                cap = self._resolve_field("Generator", gen, "pmax", fallback="—")
            gt = _classify_gen(gen, self._turb_refs)
            kind = self._gen_kind(gen)
            name = _elem_name(gen)
            # Visible label: drop the ``:uid`` suffix that ``_elem_name``
            # tacks on (it clutters the canvas; uid lives in the
            # tooltip).  Format matches the aggregated label so a
            # singleton-rendered generator and a bus-aggregated bubble
            # have the same line 1 = name / line 2 = capacity shape.
            display_name = str(gen.get("name") or name)
            lbl = display_name if self.opts.compact else f"{display_name}\n{cap} MW"
            nid = self._gid(gen)
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=kind,
                    cluster="electrical",
                    tooltip=(
                        f"Generator {name} (uid={gen.get('uid')}) "
                        f"— type={gt}, capacity={cap}"
                    ),
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
            # Single-unit groups: render exactly like the individual
            # generator so the user sees its real name and pmax
            # instead of a synthetic "Agg. generators …" placeholder.
            if len(grp) == 1:
                self._gen_individual(grp)
                continue
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
            # Label = common name stem of the members (e.g. "COLBUN"
            # for COLBUN_1 / COLBUN_2 / COLBUN_3) with a unit count
            # suffix.  Falls back to a generic placeholder only when
            # no usable common stem exists.
            # Use the raw ``name`` field (no ``:uid`` suffix) so the
            # common-stem detection sees the shared prefix cleanly.
            stem = self._common_stem([str(g.get("name") or _elem_name(g)) for g in grp])
            # Same line 1 = stem / line 2 = stats shape as
            # ``_gen_individual``, so the visual style is uniform
            # whether the bus has one or many gens.
            lbl = f"{stem}\n{len(grp)} units · {total:.0f} MW"
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=kind,
                    cluster="electrical",
                    tooltip=(f"{stem} @ {bname}: {len(grp)} units, {total:.0f} MW"),
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
            # Single-unit (bus, type) groups: render the unit directly.
            if len(grp) == 1:
                self._gen_individual(grp)
                continue
            total = sum(_gen_pmax(g) for g in grp)
            rep = _resolve_bus_ref(bus_ref, self._vmap)
            bus = self._find("bus_array", rep) or self._find("bus_array", bus_ref)
            bname = (
                _elem_name(bus)
                if bus
                else (bus_ref if isinstance(bus_ref, str) else f"bus{bus_ref}")
            )
            meta = _GEN_TYPE_META.get(gt, ("?", "⚡", "gen"))
            label, _icon, palette_key = meta
            nid = f"agg_type_{bus_ref}_{gt}"
            # Use the raw ``name`` field (no ``:uid`` suffix) so the
            # common-stem detection sees the shared prefix cleanly.
            stem = self._common_stem([str(g.get("name") or _elem_name(g)) for g in grp])
            # Same line 1 / line 2 shape as the bus aggregation; the
            # type-specific tint already discriminates solar / hydro /
            # thermal / BESS via the per-kind colour, so the explicit
            # ``{icon} {label}`` prefix would just add visual noise.
            lbl = f"{stem}\n{len(grp)} units · {total:.0f} MW"
            self.model.add_node(
                Node(
                    node_id=nid,
                    label=lbl,
                    kind=palette_key,
                    cluster="electrical",
                    tooltip=(
                        f"{stem} ({label}) @ {bname}: {len(grp)} units, {total:.0f} MW"
                    ),
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
            # Single-unit type groups: render the unit directly.
            if len(grp) == 1:
                self._gen_individual(grp)
                continue
            total = sum(_gen_pmax(g) for g in grp)
            meta = _GEN_TYPE_META.get(gt, ("?", "⚡", "gen"))
            label, _icon, palette_key = meta
            nid = f"agg_global_{gt}"
            # Global aggregation only has a type label (no per-bus
            # name stem); use it as line 1 and the unit count + total
            # MW as line 2, matching the other aggregation shapes.
            lbl = f"{label}\n{len(grp)} units · {total:.0f} MW"
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
                    tooltip=(
                        f"Demand {dem.get('name')} (uid={dem.get('uid')}) — lmax={lmax}"
                    ),
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
                    tooltip=(f"Battery {name} (uid={bat.get('uid')}) — emax={emax}"),
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
                    tooltip=(f"Converter {name} (uid={conv.get('uid')}) — cap={cap}"),
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

        Single-unit groups are short-circuited to the individual
        generator id — the aggregator emits the unit directly (with
        its real name) instead of a synthetic ``agg_bus_…`` placeholder,
        so the super-node id would dangle.
        """
        gen = self._find("generator_array", gen_ref)
        if gen is None:
            return None
        agg = self._eff_agg
        if agg == "none":
            return self._gid(gen)
        all_gens = self.sys.get("generator_array", []) or []
        if agg == "bus":
            bus = gen.get("bus")
            same_bus = [g for g in all_gens if g.get("bus") == bus]
            if len(same_bus) <= 1:
                return self._gid(gen)
            return f"agg_bus_{bus}"
        if agg == "type":
            gt = _classify_gen(gen, self._turb_refs)
            bus = gen.get("bus")
            same = [
                g
                for g in all_gens
                if g.get("bus") == bus and _classify_gen(g, self._turb_refs) == gt
            ]
            if len(same) <= 1:
                return self._gid(gen)
            return f"agg_type_{bus}_{gt}"
        if agg == "global":
            gt = _classify_gen(gen, self._turb_refs)
            same = [g for g in all_gens if _classify_gen(g, self._turb_refs) == gt]
            if len(same) <= 1:
                return self._gid(gen)
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
                    tooltip=(
                        f"GeneratorProfile {name} (uid={gp.get('uid')}) "
                        f"— profile={plbl}"
                    ),
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
                    tooltip=(
                        f"DemandProfile {name} (uid={dp.get('uid')}) — profile={plbl}"
                    ),
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
