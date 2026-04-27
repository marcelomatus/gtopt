# SPDX-License-Identifier: BSD-3-Clause
"""Hydro + reserve-zone builders for :class:`gtopt_diagram.TopologyBuilder`.

Mixin extracted from :mod:`gtopt_diagram.gtopt_diagram`.  Holds the
methods that walk the hydrological + reserve arrays of the JSON
system (junctions, waterways, reservoirs, turbines, flows, seepages,
volume rights, flow rights, reservoir efficiencies, pumps, LNG
terminals, reserve zones, reserve provisions).

The mixin assumes the host class provides:

* ``self.sys``           — the parsed ``system`` JSON dict
* ``self.opts``          — :class:`FilterOptions`
* ``self.model``         — :class:`GraphModel` accumulator
* ``self._turb_refs``      — turbine→generator reference map
* ``self._turb_way_refs``  — turbine→waterway reference map
* ``self._eff_turb_refs``  — efficiency→turbine pair map
* ``self._resolve_field``, ``self._bid``, ``self._gid``, … — methods
  inherited via :class:`gtopt_diagram._topology_ids.TopologyIdsMixin`.
"""

from __future__ import annotations

import logging
from typing import Any

from gtopt_diagram._classify import (
    elem_name as _elem_name,
    gen_pmax as _gen_pmax,
    resolve as _resolve,
)
from gtopt_diagram._graph_model import (
    Edge,
    FilterOptions,
    GraphModel,
    Node,
)
from gtopt_diagram._svg_constants import (
    _PALETTE,
    _reservoir_intensity,
)
from gtopt_diagram._topology_ids import TopologyIdsMixin

logger = logging.getLogger(__name__)


class TopologyHydroMixin(TopologyIdsMixin):
    """Composable hydro + reserve-zone builders."""

    sys: dict[str, Any]
    opts: FilterOptions
    model: GraphModel
    _turb_refs: set
    _turb_way_refs: set
    _eff_turb_refs: set

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
            cr = self._resolve_field("Turbine", t, "production_factor")
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
        When a ``bound_rule`` is present, an additional dashed edge from the
        bound reservoir to this right node shows the volume-dependent bound.
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
            # Edge from bound_rule reservoir (volume-dependent bound)
            bound_rule = vr.get("bound_rule")
            if isinstance(bound_rule, dict):
                br_res_ref = bound_rule.get("reservoir")
                if br_res_ref is not None:
                    br_res_id = self._find_node_id(
                        "reservoir_array", br_res_ref, self._rid
                    )
                    # Only draw the bound edge when the bound reservoir is
                    # different from the primary source reservoir to avoid a
                    # self-loop.
                    if br_res_id and br_res_id != res_id:
                        self.model.add_edge(
                            Edge(
                                br_res_id,
                                vrid,
                                label="" if self.opts.compact else "bound",
                                style="dashed",
                                color=_PALETTE["right_bound_edge"],
                                directed=True,
                                weight=0.3,
                            )
                        )

    def _flow_rights(self):
        """Draw FlowRight nodes attached to their reference junction.

        Each FlowRight is NOT part of the hydrological topology; it is an
        accounting entity tracking flow (m³/s) entitlements at a junction.
        Displayed as a separate node with a dotted edge to the referenced
        junction.  When a ``bound_rule`` is present, an additional dashed
        edge from the bound reservoir to this right node shows the
        volume-dependent flow bound.
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
            # Edge from bound_rule reservoir (volume-dependent flow bound)
            # Note: FlowRight has a junction reference, not a reservoir, so
            # there is no "same resource" check needed here.  Any bound_rule
            # reservoir is always a distinct relationship.
            bound_rule = fr.get("bound_rule")
            if isinstance(bound_rule, dict):
                br_res_ref = bound_rule.get("reservoir")
                if br_res_ref is not None:
                    br_res_id = self._find_node_id(
                        "reservoir_array", br_res_ref, self._rid
                    )
                    if br_res_id:
                        self.model.add_edge(
                            Edge(
                                br_res_id,
                                frid,
                                label="" if self.opts.compact else "bound",
                                style="dashed",
                                color=_PALETTE["right_bound_edge"],
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

    def _pumps(self):
        """Draw Pump nodes in the hydro cluster.

        A Pump draws electrical power from a Demand and pushes water through
        a Waterway (from junction_a upstream to junction_b).  Edges:

        * ``demand → pump`` (dashed, labeled "power in" — electrical coupling)
        * ``junction_a → pump`` (waterway inlet, labeled with waterway name)
        * ``pump → junction_b`` (waterway outlet)

        When the pump's waterway is not in ``turbine_waterway_refs`` (i.e.
        no turbine already drew the arc), the direct junctions are connected
        through the pump node.
        """
        for p in self.sys.get("pump_array", []):
            name = _elem_name(p)
            pid = self._pid(p)
            cap = self._resolve_field("Pump", p, "capacity", fallback=None)
            if cap is None:
                cap = self._resolve_field("Pump", p, "pmax", fallback="—")
            lbl = str(name) if self.opts.compact else f"[Pump] {name}\n{cap} MW"
            self.model.add_node(
                Node(
                    node_id=pid,
                    label=lbl,
                    kind="pump",
                    cluster="hydro",
                    tooltip=f"Pump uid={p.get('uid')} capacity={cap}",
                    size=22.0,
                )
            )
            # Electrical demand → pump (power consumption)
            dem_id = self._find_node_id("demand_array", p.get("demand"), self._did)
            if dem_id:
                self.model.add_edge(
                    Edge(
                        dem_id,
                        pid,
                        label="" if self.opts.compact else "power in",
                        style="dashed",
                        color=_PALETTE["pump_edge"],
                        directed=True,
                        weight=0.5,
                    )
                )
            # Waterway junctions: junction_a → pump → junction_b
            way_ref = p.get("waterway")
            if way_ref is not None:
                way = _resolve(self.sys.get("waterway_array", []), way_ref)
                if way:
                    way_name = _elem_name(way)
                    ja = self._find_node_id(
                        "junction_array", way.get("junction_a"), self._jid
                    )
                    jb = self._find_node_id(
                        "junction_array", way.get("junction_b"), self._jid
                    )
                    if ja:
                        self.model.add_edge(
                            Edge(
                                ja,
                                pid,
                                label="" if self.opts.compact else str(way_name),
                                color=_PALETTE["waterway_edge"],
                                directed=True,
                                weight=1.0,
                            )
                        )
                    if jb:
                        self.model.add_edge(
                            Edge(
                                pid,
                                jb,
                                color=_PALETTE["waterway_edge"],
                                directed=True,
                                weight=1.0,
                            )
                        )

    def _lng_terminals(self):
        """Draw LNG Terminal nodes in the electrical cluster.

        An LNG terminal stores Liquefied Natural Gas and feeds thermal
        generators via heat-rate coupling.  Edges:

        * ``lng_terminal → generator``  (dotted, labeled "fuel" — per linked generator)

        Tank capacity (``emax``) is shown in the label.
        """
        for lt in self.sys.get("lng_terminal_array", []):
            name = _elem_name(lt)
            ltid = self._lngid(lt)
            emax = self._resolve_field("LngTerminal", lt, "emax", fallback=None)
            if emax is None:
                emax = "—"
            sendout = self._resolve_field(
                "LngTerminal", lt, "sendout_max", fallback=None
            )
            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [f"[LNG] {name}"]
                if emax != "—":
                    parts.append(f"{emax} m\u00b3")
                if sendout is not None:
                    parts.append(f"Q_max={sendout} m\u00b3/h")
                lbl = "\n".join(parts)
            self.model.add_node(
                Node(
                    node_id=ltid,
                    label=lbl,
                    kind="lng_terminal",
                    cluster="electrical",
                    tooltip=(
                        f"LngTerminal uid={lt.get('uid')} name={lt.get('name')}"
                        f" emax={emax}"
                    ),
                    size=26.0,
                )
            )
            # Edges to linked generators (heat-rate coupling)
            for link in lt.get("generators", []):
                gen_ref = link.get("generator")
                heat_rate = link.get("heat_rate")
                if gen_ref is None:
                    continue
                gen_id = self._find_node_id("generator_array", gen_ref, self._gid)
                if gen_id:
                    hr_lbl = (
                        ""
                        if self.opts.compact
                        else (
                            f"fuel\n{heat_rate} m\u00b3/MWh"
                            if heat_rate is not None
                            else "fuel"
                        )
                    )
                    self.model.add_edge(
                        Edge(
                            ltid,
                            gen_id,
                            label=hr_lbl,
                            style="dotted",
                            color=_PALETTE["lng_edge"],
                            directed=True,
                            weight=0.5,
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
