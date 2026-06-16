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
    turbine_is_builtin_waterway as _turbine_is_builtin_waterway,
    turbine_is_terminal_drain as _turbine_is_terminal_drain,
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
                        f"Junction {j.get('name')} (uid={uid}) — drain={is_drain}"
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
                    tooltip=(f"Reservoir {name} (uid={r.get('uid')}) — emax={emax}"),
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
            # ``turbine_drain`` kind variant for Mode B terminal turbines
            # (``junction_a`` set, ``junction_b`` unset) — water exits the
            # system through the turbine itself.  Slightly different
            # colour so the user can tell at a glance which turbines are
            # cascading vs draining to the sea.
            is_drain = _turbine_is_terminal_drain(t)
            kind = "turbine_drain" if is_drain else "turbine"
            drain_tag = " (drain)" if is_drain else ""
            lbl = (
                str(name)
                if self.opts.compact
                else f"[Turbine{drain_tag}] {name}\n{cap} MW  cr={cr}"
            )
            self.model.add_node(
                Node(
                    node_id=tid,
                    label=lbl,
                    kind=kind,
                    cluster="hydro",
                    tooltip=(
                        f"Turbine {name} (uid={t.get('uid')}) — cap={cap}"
                        + (" — terminal drain (run-to-sea)" if is_drain else "")
                    ),
                )
            )
            # Connect turbine to water source.  Mode priority follows
            # ``turbine.hpp``: flow > junctions (Mode B, built-in waterway)
            # > waterway (legacy).
            flow_ref = t.get("flow")
            ja_ref = t.get("junction_a")
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
            elif _turbine_is_builtin_waterway(t):
                # Mode B — built-in waterway via ``junction_a`` (+ optional
                # ``junction_b``).  The turbine carries its own flow column
                # and replaces the synthetic penstock Waterway entirely.
                ja_id = self._find_node_id("junction_array", ja_ref, self._jid)
                jb_id = self._find_node_id(
                    "junction_array", t.get("junction_b"), self._jid
                )
                if ja_id:
                    # Intake edge — same colour as the legacy waterway path so
                    # visual diffs stay small.  No label: the turbine itself
                    # *is* the waterway in this mode.
                    self.model.add_edge(
                        Edge(
                            ja_id,
                            tid,
                            color=_PALETTE["waterway_edge"],
                            directed=False,
                        )
                    )
                if jb_id:
                    # Cascade (between-junctions): tailrace edge.
                    self.model.add_edge(
                        Edge(
                            tid,
                            jb_id,
                            color=_PALETTE["waterway_edge"],
                            directed=False,
                        )
                    )
                # No ``junction_b`` → terminal / run-to-sea turbine.
                # The turbine node itself signals the drain via its
                # ``turbine_drain`` kind variant (slightly distinct
                # colour); no separate marker node or dangling edge.
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
                                tooltip=(
                                    f"Generator {gname} "
                                    f"(uid={gen.get('uid')}) "
                                    f"— type={gtype}, pmax={pmax}"
                                ),
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
                    tooltip=(f"Flow {name} (uid={f.get('uid')}) — discharge={disc}"),
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
                    tooltip=(
                        f"ReservoirSeepage {fi.get('name')} (uid={fi.get('uid')})"
                    ),
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
                        f"VolumeRight {vr.get('name')} (uid={vr.get('uid')})"
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
            # `target` is the canonical flow field (soft kink point); `discharge`
            # is its legacy alias kept for back-compat (see flow_right.hpp).
            target = self._resolve_field("FlowRight", fr, "target", fallback=None)
            if target is None:
                target = self._resolve_field(
                    "FlowRight", fr, "discharge", fallback=None
                )
            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [f"[FlowRight] {name}"]
                if purpose:
                    parts.append(str(purpose))
                if target is not None:
                    parts.append(f"{target} m\u00b3/s")
                lbl = "\n".join(parts)
            self.model.add_node(
                Node(
                    node_id=frid,
                    label=lbl,
                    kind="flow_right",
                    cluster="hydro",
                    tooltip=(
                        f"FlowRight {fr.get('name')} (uid={fr.get('uid')})"
                        f" purpose={purpose}"
                    ),
                    size=14.0,
                )
            )
            # Edge to reference junction
            junc_id = self._find_node_id(
                "junction_array", fr.get("junction_a"), self._jid
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
                    tooltip=(f"Pump {name} (uid={p.get('uid')}) — capacity={cap}"),
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
                        f"LngTerminal {lt.get('name')} (uid={lt.get('uid')})"
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
                    tooltip=(f"ReserveZone {rz.get('name')} (uid={rz.get('uid')})"),
                )
            )

    def _reserve_provisions(self):
        """Draw reserve provision edges: generator → reserve_zone."""
        for rp in self.sys.get("reserve_provision_array", []):
            gen_id = self._find_node_id(
                "generator_array", rp.get("generator"), self._gid
            )
            # reserve_zones is a typed array of zone refs (Uid number
            # or Name string per element).  Pre-2026-05-16 the field
            # was a colon-delimited string; accept both forms here so
            # legacy JSON inputs still render — drop the str branch
            # once nothing in the wild uses the old form.
            rz_field = rp.get("reserve_zones", [])
            rz_refs: list = []
            if isinstance(rz_field, list):
                rz_refs = [r for r in rz_field if r not in (None, "")]
            elif isinstance(rz_field, str) and rz_field:
                rz_refs = [s.strip() for s in rz_field.split(":") if s.strip()]
            for rz_ref in rz_refs:
                rz_id = self._find_node_id("reserve_zone_array", rz_ref, self._rzid)
                if gen_id and rz_id:
                    self.model.add_edge(
                        Edge(
                            gen_id,
                            rz_id,
                            label="reserve",
                            style="dotted",
                            color=_PALETTE.get("reserve_edge", _PALETTE["gen_border"]),
                        )
                    )

    # ── Fuel + Emission family ────────────────────────────────────────────

    def _fuels(self):
        """Draw Fuel nodes plus ``fuel → generator`` edges.

        A Fuel is a time-schedulable price + heat-content tag referenced by
        ``Generator.fuel``.  Drawn as a brown cylinder; one dotted edge per
        generator that consumes it.
        """
        fuels = self.sys.get("fuel_array", [])
        if not fuels:
            return
        # Pre-index generators by fuel reference for O(N+M) wiring
        gens_by_fuel: dict = {}
        for g in self.sys.get("generator_array", []):
            fref = g.get("fuel")
            if fref is not None:
                gens_by_fuel.setdefault(fref, []).append(g)
        for fl in fuels:
            name = _elem_name(fl)
            flid = self._fuelid(fl)
            price = self._resolve_field("Fuel", fl, "price", fallback=None)
            heat = self._resolve_field("Fuel", fl, "heat_content", fallback=None)
            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [f"[Fuel] {name}"]
                if price is not None:
                    parts.append(f"price={price}")
                if heat is not None:
                    parts.append(f"hc={heat}")
                lbl = "\n".join(parts)
            self.model.add_node(
                Node(
                    node_id=flid,
                    label=lbl,
                    kind="fuel",
                    cluster="electrical",
                    tooltip=(
                        f"Fuel {fl.get('name')} (uid={fl.get('uid')}) — price={price}"
                    ),
                    size=22.0,
                )
            )
            for g in gens_by_fuel.get(fl.get("uid"), []) + gens_by_fuel.get(
                fl.get("name"), []
            ):
                gen_id = self._gid(g)
                self.model.add_edge(
                    Edge(
                        flid,
                        gen_id,
                        label="" if self.opts.compact else "fuel",
                        style="dotted",
                        color=_PALETTE["fuel_edge"],
                        directed=True,
                        weight=0.4,
                    )
                )

    def _emissions(self):
        """Draw Emission (pollutant tag) nodes.

        An Emission is a tiny tag — only uid + name — referenced by
        ``EmissionSource.emission`` and ``EmissionZone.emissions[]``.  The
        edges land on it when the corresponding sources / zones are drawn.
        """
        for em in self.sys.get("emission_array", []):
            name = _elem_name(em)
            emid = self._emid(em)
            lbl = str(name) if self.opts.compact else f"[Emission] {name}"
            self.model.add_node(
                Node(
                    node_id=emid,
                    label=lbl,
                    kind="emission",
                    cluster="electrical",
                    tooltip=(f"Emission {em.get('name')} (uid={em.get('uid')})"),
                    size=14.0,
                )
            )

    def _emission_zones(self):
        """Draw EmissionZone nodes plus ``zone → emission`` coverage edges."""
        for ez in self.sys.get("emission_zone_array", []):
            name = _elem_name(ez)
            ezid = self._emzid(ez)
            cap = self._resolve_field("EmissionZone", ez, "cap", fallback=None)
            price = self._resolve_field("EmissionZone", ez, "price", fallback=None)
            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [f"[EmZone] {name}"]
                if cap is not None:
                    parts.append(f"cap={cap}")
                if price is not None:
                    parts.append(f"price={price}")
                lbl = "\n".join(parts)
            self.model.add_node(
                Node(
                    node_id=ezid,
                    label=lbl,
                    kind="emission_zone",
                    cluster="electrical",
                    tooltip=(
                        f"EmissionZone {ez.get('name')} (uid={ez.get('uid')}) — "
                        f"cap={cap} price={price}"
                    ),
                    size=22.0,
                )
            )
            # Each covered emission → dotted ``zone → emission`` edge.
            for row in ez.get("emissions", []) or []:
                em_ref = row.get("emission") if isinstance(row, dict) else None
                if em_ref is None:
                    continue
                em_id = self._find_node_id("emission_array", em_ref, self._emid)
                if em_id:
                    self.model.add_edge(
                        Edge(
                            ezid,
                            em_id,
                            label="" if self.opts.compact else "covers",
                            style="dotted",
                            color=_PALETTE["emission_edge"],
                            directed=True,
                            weight=0.3,
                        )
                    )

    def _emission_sources(self):
        """Draw EmissionSource nodes (generator → source → zone bridge).

        An EmissionSource ties a Generator to an EmissionZone with a rate
        coefficient.  Rendered as a small square sitting on the edge between
        the generator and the zone — drawn explicitly so the rate label
        stays attached even when several sources hit the same zone.
        """
        for es in self.sys.get("emission_source_array", []):
            name = _elem_name(es)
            esid = self._emsrcid(es)
            rate = self._resolve_field("EmissionSource", es, "rate", fallback=None)
            upstream = self._resolve_field(
                "EmissionSource", es, "upstream_rate", fallback=None
            )
            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [f"[EmSrc] {name}"]
                if rate is not None:
                    parts.append(f"rate={rate}")
                if upstream is not None:
                    parts.append(f"up={upstream}")
                lbl = "\n".join(parts)
            self.model.add_node(
                Node(
                    node_id=esid,
                    label=lbl,
                    kind="emission_source",
                    cluster="electrical",
                    tooltip=(
                        f"EmissionSource {es.get('name')} (uid={es.get('uid')}) — "
                        f"rate={rate}"
                    ),
                    size=14.0,
                )
            )
            gen_id = self._find_node_id(
                "generator_array", es.get("generator"), self._gid
            )
            zone_id = self._find_node_id(
                "emission_zone_array", es.get("zone"), self._emzid
            )
            if gen_id:
                self.model.add_edge(
                    Edge(
                        gen_id,
                        esid,
                        label="",
                        style="dotted",
                        color=_PALETTE["emission_edge"],
                        directed=True,
                        weight=0.3,
                    )
                )
            if zone_id:
                self.model.add_edge(
                    Edge(
                        esid,
                        zone_id,
                        label="" if self.opts.compact else "balance",
                        style="dotted",
                        color=_PALETTE["emission_edge"],
                        directed=True,
                        weight=0.3,
                    )
                )

    # ── Multi-carrier converters ───────────────────────────────────────────

    def _carrier_converters(self):
        """Draw CarrierConverter nodes (carrier-typed flow bridges).

        Each converter sits between a ``from_node`` and a ``to_node`` of
        possibly-different carriers.  Drawn as an ellipse with dashed input
        and solid (efficiency-tagged) output arrows.
        """
        for cc in self.sys.get("carrier_converter_array", []):
            name = _elem_name(cc)
            ccid = self._ccid(cc)
            cap = self._resolve_field("CarrierConverter", cc, "capacity", fallback=None)
            eff = self._resolve_field(
                "CarrierConverter", cc, "efficiency", fallback=None
            )
            ftype = cc.get("type", "")
            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [f"[CConv] {name}"]
                if ftype:
                    parts.append(str(ftype))
                if cap is not None:
                    parts.append(f"cap={cap}")
                if eff is not None:
                    parts.append(f"η={eff}")
                lbl = "\n".join(parts)
            self.model.add_node(
                Node(
                    node_id=ccid,
                    label=lbl,
                    kind="carrier_converter",
                    cluster="electrical",
                    tooltip=(
                        f"CarrierConverter {cc.get('name')} (uid={cc.get('uid')}) — "
                        f"from={cc.get('from_carrier')} to={cc.get('to_carrier')}"
                    ),
                    size=20.0,
                )
            )
            # The from/to nodes may live in any of bus/thermal_node/
            # hydrogen_node/ammonia_node arrays.  Carrier-typed balance nodes
            # other than electric Bus aren't drawn by ``_buses`` today, so we
            # auto-emit a stub bus-kind node when the ref resolves to such an
            # array; this keeps the converter's input/output edges anchored
            # without introducing a brand-new node kind per carrier.
            known_ids = {n.node_id for n in self.model.nodes}
            for ref, role in (
                (cc.get("from_node"), "from"),
                (cc.get("to_node"), "to"),
            ):
                if ref is None:
                    continue
                nid = None
                for arr_key in (
                    "bus_array",
                    "thermal_node_array",
                    "hydrogen_node_array",
                    "ammonia_node_array",
                ):
                    item = self._find(arr_key, ref)
                    if item is None:
                        continue
                    nid = self._bid(item)
                    if nid not in known_ids:
                        # Stub for non-bus carrier nodes — drawn as a plain
                        # bus so the edge has somewhere to land.  The label
                        # carries the carrier hint so the viewer can tell it
                        # apart from a real electric Bus.
                        carrier = (
                            cc.get("from_carrier")
                            if role == "from"
                            else cc.get("to_carrier")
                        ) or arr_key.replace("_array", "")
                        nname = _elem_name(item)
                        self.model.add_node(
                            Node(
                                node_id=nid,
                                label=(
                                    str(nname)
                                    if self.opts.compact
                                    else f"{nname}\n[{carrier}]"
                                ),
                                kind="bus",
                                cluster="electrical",
                                tooltip=(
                                    f"{arr_key.replace('_array', '')} "
                                    f"uid={item.get('uid')} carrier={carrier}"
                                ),
                            )
                        )
                        known_ids.add(nid)
                    break
                if nid:
                    src, dst = (nid, ccid) if role == "from" else (ccid, nid)
                    self.model.add_edge(
                        Edge(
                            src,
                            dst,
                            label="" if self.opts.compact else role,
                            style="dashed" if role == "from" else "solid",
                            color=_PALETTE["carrier_converter_border"],
                            directed=True,
                            weight=0.5,
                        )
                    )

    # ── User parameters / variables / constraints ─────────────────────────

    def _decision_variables(self):
        """Draw DecisionVariable nodes (free LP columns)."""
        for dv in self.sys.get("decision_variable_array", []):
            name = _elem_name(dv)
            dvid = self._dvid(dv)
            ctype = dv.get("cost_type") or ""
            lo = dv.get("lower_bound")
            hi = dv.get("upper_bound")
            cost = dv.get("cost")
            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [f"[DecVar] {name}"]
                if ctype:
                    parts.append(str(ctype))
                if lo is not None or hi is not None:
                    parts.append(
                        f"[{lo if lo is not None else '-∞'}, "
                        f"{hi if hi is not None else '+∞'}]"
                    )
                if cost is not None:
                    parts.append(f"cost={cost}")
                lbl = "\n".join(parts)
            self.model.add_node(
                Node(
                    node_id=dvid,
                    label=lbl,
                    kind="decision_variable",
                    cluster="electrical",
                    tooltip=(
                        f"DecisionVariable {dv.get('name')} (uid={dv.get('uid')}) — "
                        f"cost_type={ctype}"
                    ),
                    size=16.0,
                )
            )

    def _user_constraints(self):
        """Draw UserConstraint nodes (one per ``user_constraint_array`` entry).

        The constraint's *expression* references arbitrary LP primitives via
        an AMPL-inspired AST.  Parsing that AST to draw wire-level edges is
        out of scope here — instead we render a single tagged note-node and
        rely on the textual ``description`` / ``expression`` tooltip for
        provenance.
        """
        for uc in self.sys.get("user_constraint_array", []):
            name = _elem_name(uc)
            ucid = self._ucid(uc)
            expr = uc.get("expression", "")
            penalty = uc.get("penalty")
            if self.opts.compact:
                lbl = str(name)
            else:
                parts = [f"[UC] {name}"]
                if penalty is not None:
                    parts.append(f"penalty={penalty}")
                lbl = "\n".join(parts)
            tooltip_expr = (str(expr)[:80] + "...") if len(str(expr)) > 80 else expr
            self.model.add_node(
                Node(
                    node_id=ucid,
                    label=lbl,
                    kind="user_constraint",
                    cluster="electrical",
                    tooltip=(
                        f"UserConstraint {uc.get('name')} (uid={uc.get('uid')}) — "
                        f"{tooltip_expr}"
                    ),
                    size=18.0,
                )
            )

    # ── Unit commitment overlays ───────────────────────────────────────────

    def _commitments(self):
        """Draw Commitment overlays as dotted edges to their generators."""
        for cm in self.sys.get("commitment_array", []):
            name = _elem_name(cm)
            cmid = self._cmtid(cm)
            lbl = str(name) if self.opts.compact else f"[UC] {name}"
            self.model.add_node(
                Node(
                    node_id=cmid,
                    label=lbl,
                    kind="commitment",
                    cluster="electrical",
                    tooltip=(f"Commitment {cm.get('name')} (uid={cm.get('uid')})"),
                    size=14.0,
                )
            )
            gen_id = self._find_node_id(
                "generator_array", cm.get("generator"), self._gid
            )
            if gen_id:
                self.model.add_edge(
                    Edge(
                        cmid,
                        gen_id,
                        label="" if self.opts.compact else "commit",
                        style="dotted",
                        color=_PALETTE["commitment_border"],
                        directed=True,
                        weight=0.3,
                    )
                )

    def _simple_commitments(self):
        """Draw SimpleCommitment overlays as dotted edges to their generators."""
        for cm in self.sys.get("simple_commitment_array", []):
            name = _elem_name(cm)
            cmid = self._scmtid(cm)
            lbl = str(name) if self.opts.compact else f"[sUC] {name}"
            self.model.add_node(
                Node(
                    node_id=cmid,
                    label=lbl,
                    kind="simple_commitment",
                    cluster="electrical",
                    tooltip=(
                        f"SimpleCommitment {cm.get('name')} (uid={cm.get('uid')})"
                    ),
                    size=14.0,
                )
            )
            gen_id = self._find_node_id(
                "generator_array", cm.get("generator"), self._gid
            )
            if gen_id:
                self.model.add_edge(
                    Edge(
                        cmid,
                        gen_id,
                        label="" if self.opts.compact else "simple-commit",
                        style="dotted",
                        color=_PALETTE["simple_commitment_border"],
                        directed=True,
                        weight=0.3,
                    )
                )

    # Voltage-level suffix patterns stripped to derive a substation name
    # (e.g. ``CHARRUA_500KV`` / ``CHARRUA_220KV`` → ``CHARRUA``).
    _SUBSTATION_SUFFIX_PATTERNS = (
        r"_\d+(?:\.\d+)?\s*KV$",  # _500KV, _220KV, _13.8KV
        r"_V\d+$",  # _V500
        r"_\d{3,4}$",  # _500, _220 (bare voltage number with separator)
        r"(?<=[A-Za-z])\d{2,4}$",  # Charrua500, Charrua220 (no separator)
    )

    # Below these thresholds, a Line's resistance/reactance is treated as
    # "short / transformer-like" — small enough that the two buses it
    # joins are physically co-located.  Values are in per-unit (default
    # gtopt unit when ``voltage`` is unset) — Ω values stay above these
    # thresholds for any realistic transmission line.
    _SHORT_LINE_R_PU = 0.005
    _SHORT_LINE_X_PU = 0.05

    @staticmethod
    def _substation_prefix(bus_name: str) -> str | None:
        """Strip a recognised voltage suffix from a bus name.

        Returns the substation prefix, or ``None`` when no suffix matches
        (so the bus name has no clue suggesting it belongs to a
        substation cluster).
        """
        import re  # noqa: PLC0415

        for pat in TopologyHydroMixin._SUBSTATION_SUFFIX_PATTERNS:
            m = re.search(pat, bus_name)
            if m:
                prefix = bus_name[: m.start()]
                return prefix if prefix else None
        return None

    @staticmethod
    def _line_is_intra_substation(line: dict) -> bool:
        """Return True when a Line is a transformer or very-short line.

        Detected by structural marker (``type == "transformer"``) OR by
        per-unit R+X both below the short-line thresholds.  Lines whose
        R/X are missing default to "not short" — only positively-tagged
        candidates qualify.
        """
        ltype = (line.get("type") or "").lower()
        if "transformer" in ltype:
            return True
        try:
            r = float(line.get("resistance", 0.0) or 0.0)
            x = float(line.get("reactance", 0.0) or 0.0)
        except (TypeError, ValueError):
            return False
        return (
            0 <= r <= TopologyHydroMixin._SHORT_LINE_R_PU
            and 0 < x <= TopologyHydroMixin._SHORT_LINE_X_PU
        )

    # Stem-detection regex shared between explicit and auto-plant paths.
    # Recognised suffix patterns (each stripped off the end of the name):
    #   _1 / _12             — unit number
    #   _U1 / _U12           — explicit "Unit N"
    #   _GN_A / _GN_B        — fuel-band variant
    #   _A / _B (single)     — fall-through single-letter variant
    #   _ConfTGA / _ConfTV   — combined-cycle config (handled by explicit
    #                          Plant primitives but useful as fallback)
    _PLANT_STEM_PATTERNS = (
        # Order matters — longest first to avoid greedy partial matches.
        r"_ConfT[A-Z]+\d*$",
        r"_U\d+$",
        r"_GN_[A-Z]\d*$",
        r"_\d+$",
        r"_[A-Z]$",
    )

    @staticmethod
    def _hydro_plant_stem(name: str) -> str | None:
        """Return the inferred plant stem of a generator/turbine name.

        Strips a recognised unit/variant suffix.  Returns ``None`` when
        no pattern matches (the name is its own stem — no auto-grouping).
        """
        import re  # noqa: PLC0415

        for pat in TopologyHydroMixin._PLANT_STEM_PATTERNS:
            m = re.search(pat, name)
            if m:
                return name[: m.start()]
        return None

    def _turbine_upstream_junction(self, t: dict) -> str | None:
        """Return the name of the junction the turbine draws water from.

        Mode B (``junction_a``) wins; Mode A reads ``junction_a`` off
        the referenced ``Waterway``; Mode C (``flow``) and turbines
        with no resolvable upstream return ``None``.
        """
        # Mode B — built-in waterway.
        ja_ref = t.get("junction_a")
        if ja_ref is not None and not t.get("flow"):
            for j in self.sys.get("junction_array", []) or []:
                if j.get("name") == ja_ref or j.get("uid") == ja_ref:
                    return str(j.get("name") or j.get("uid"))
            return None
        # Mode A — legacy gen-Waterway path.
        ww_ref = t.get("waterway")
        if ww_ref is None:
            return None
        for w in self.sys.get("waterway_array", []) or []:
            if w.get("name") == ww_ref or w.get("uid") == ww_ref:
                ja = w.get("junction_a")
                for j in self.sys.get("junction_array", []) or []:
                    if j.get("name") == ja or j.get("uid") == ja:
                        return str(j.get("name") or j.get("uid"))
                return str(ja) if ja is not None else None
        return None

    def _auto_detect_hydro_plants(self) -> dict[str, tuple[str, list[str]]]:
        """Group turbines that share an upstream junction (hydro plant).

        Primary signal: turbines with the same ``junction_a`` (Mode B)
        or the same gen-Waterway's ``junction_a`` (Mode A) belong to
        the same hydro plant — physically they share the intake pond.
        The cluster label is the **common name stem** of the member
        generators when one can be inferred (e.g. COLBUN from
        COLBUN_1/2/3), otherwise the junction name itself.

        Returns ``{cluster_label: (display_label, [generator_name, ...])}``.

        Falls back to pure name-stem detection for generators whose
        turbines do NOT share a junction — covers combined-cycle
        thermal configs (``ATA_CC_1_ConfTGA`` / ``_ConfTGB`` / ``_ConfTV``)
        and similar where the LP "plant" is not a physical hydro
        cluster.  When no junction is involved, at least two distinct
        generators must share the stem for the group to be emitted.

        Explicit ``plant_array`` members and ``aggregate ∈
        {global, type}`` cases are excluded.
        """
        if self.opts.aggregate in ("global", "type"):
            return {}
        explicit_members: set[str] = set()
        for p in self.sys.get("plant_array", []) or []:
            for gn in p.get("generator_names") or []:
                explicit_members.add(str(gn))

        out: dict[str, tuple[str, list[str]]] = {}

        # 1. Junction-based grouping — the strong signal.
        by_junction: dict[str, list[str]] = {}
        for t in self.sys.get("turbine_array", []) or []:
            gen_ref = t.get("generator")
            if gen_ref is None:
                continue
            gen_name = (
                str(gen_ref)
                if isinstance(gen_ref, str)
                else next(
                    (
                        str(g.get("name"))
                        for g in self.sys.get("generator_array", []) or []
                        if g.get("uid") == gen_ref or g.get("name") == gen_ref
                    ),
                    None,
                )
            )
            if not gen_name or gen_name in explicit_members:
                continue
            junc = self._turbine_upstream_junction(t)
            if junc is None:
                continue
            by_junction.setdefault(junc, []).append(gen_name)

        for junc_name, members in by_junction.items():
            if len(members) < 2:
                continue
            # Prefer a shared name stem as the cluster label when one
            # is consistent across members (COLBUN_1/2/3 → "COLBUN");
            # otherwise fall back to the junction name.
            stems = {self._hydro_plant_stem(m) for m in members}
            stems.discard(None)
            if len(stems) == 1:
                label = next(iter(stems))
            else:
                label = junc_name
            cluster_key = f"plant:{label}"
            out[cluster_key] = (
                f"{label} ({len(members)} units · @ {junc_name})",
                members,
            )

        # 2. Generation-plant detection: generators sharing a name stem
        #    AND connected to the same bus.  The dual signal (name +
        #    bus) is what distinguishes "ATA-TG1A / -TG1B / -TV1C all
        #    at bus ATACAMA220" from a coincidental name collision in
        #    different parts of the system.  Catches thermal combined-
        #    cycle configs and any multi-unit plant whose units do not
        #    have hydro turbine wiring (hydro is already handled by
        #    the junction-based pass above).
        #
        #    A third, weaker fallback (name stem only, no bus) catches
        #    generators whose ``bus`` field is missing — typically only
        #    in synthetic test fixtures.
        already_grouped: set[str] = set()
        for _key, (_lbl, members) in out.items():
            already_grouped.update(members)
        # (stem, bus_ref) → [generator_name]
        stem_bus_groups: dict[tuple[str, str], list[str]] = {}
        # stem → [generator_name] (no-bus fallback)
        stem_only_groups: dict[str, list[str]] = {}
        for g in self.sys.get("generator_array", []) or []:
            gname = g.get("name")
            if not gname or gname in explicit_members or gname in already_grouped:
                continue
            stem = self._hydro_plant_stem(str(gname))
            if stem is None:
                continue
            bus_ref = g.get("bus")
            if bus_ref is None or bus_ref == "":
                stem_only_groups.setdefault(stem, []).append(str(gname))
            else:
                stem_bus_groups.setdefault((stem, str(bus_ref)), []).append(str(gname))
        # Resolve bus uid → bus name for nicer labels.
        bus_name_by_ref: dict[str, str] = {}
        for b in self.sys.get("bus_array", []) or []:
            nm = b.get("name")
            if nm is None:
                continue
            bus_name_by_ref[str(b.get("uid"))] = str(nm)
            bus_name_by_ref[str(nm)] = str(nm)

        for (stem, bus_ref), members in stem_bus_groups.items():
            if len(members) < 2:
                continue
            cluster_key = f"plant:{stem}"
            if cluster_key in out:
                continue  # earlier pass already claimed this stem
            bus_label = bus_name_by_ref.get(bus_ref, bus_ref)
            out[cluster_key] = (
                f"{stem} ({len(members)} units · @ {bus_label})",
                members,
            )

        for stem, members in stem_only_groups.items():
            if len(members) < 2:
                continue
            cluster_key = f"plant:{stem}"
            if cluster_key in out:
                continue
            out[cluster_key] = (f"{stem} ({len(members)} units · auto)", members)

        return out

    def _plants(self):
        """Cluster member generators of each Plant under a labelled subgraph.

        A ``Plant`` (see :file:`include/gtopt/plant.hpp`) is a passive
        descriptor — it adds no LP columns of its own.  In the diagram
        we tag every member generator with ``subcluster=f"plant:{name}"``
        so the graphviz renderer wraps them in a rounded subgraph; the
        subgraph label carries the plant's ``pmax`` / ``n_units`` /
        ``uniq_mutex`` metadata in a compact form.

        No new nodes or edges are emitted — the visual grouping is the
        sole effect.  Plants whose member generators were filtered out
        (focus, aggregate, top-N) silently produce empty clusters.
        """
        # Build name → Node lookup once.  Run even when ``plant_array``
        # is empty so the heuristic auto-detection still groups
        # turbines sharing a junction.
        gen_by_name: dict[str, Node] = {}
        for node in self.model.nodes:
            if node.kind in ("generator", "battery"):
                # Generators carry their canonical name as the label
                # stem; trust the JSON name field by looking up via _gid.
                pass
        for g in self.sys.get("generator_array", []):
            gid = self._gid(g)
            for node in self.model.nodes:
                if node.node_id == gid:
                    name = g.get("name")
                    if name:
                        gen_by_name[str(name)] = node
                    break
        # 1. Explicit Plant entries — always win, claim their members
        #    first so the auto-detection can skip them.
        for p in self.sys.get("plant_array", []) or []:
            pname = p.get("name")
            if not pname:
                continue
            subcluster_tag = f"plant:{pname}"
            members = p.get("generator_names") or []
            tagged = 0
            for gname in members:
                node = gen_by_name.get(str(gname))
                if node is None:
                    continue
                node.subcluster = subcluster_tag
                tagged += 1
            if tagged == 0:
                continue
            # Stash the Plant metadata on the GraphModel so the renderer
            # can read it back for the subgraph label.  Plants live in
            # an out-of-band dict keyed by subcluster tag so the Node
            # dataclass stays minimal.
            if not hasattr(self.model, "plant_meta"):
                # mypy false-positive: GraphModel is open for attribute
                # attachment (lacks __slots__).
                self.model.plant_meta = {}  # type: ignore[attr-defined]
            label_bits: list[str] = [str(pname)]
            if p.get("pmax") is not None:
                label_bits.append(f"pmax={p['pmax']:g}")
            if p.get("n_units") is not None:
                label_bits.append(f"n_units={p['n_units']}")
            if p.get("uniq_mutex"):
                label_bits.append("uniq")
            self.model.plant_meta[subcluster_tag] = (  # type: ignore[attr-defined]
                " · ".join(label_bits)
            )

        # 2. Auto-detected groups.
        #    Primary signal: turbines sharing the same upstream junction
        #    (physical hydro plant — common intake pond).
        #    Secondary fallback: matching name stem (e.g. combined-cycle
        #    thermal configs with no turbine entry).
        auto_groups = self._auto_detect_hydro_plants()
        for cluster_key, (label, members) in auto_groups.items():
            tagged = 0
            for gname in members:
                node = gen_by_name.get(gname)
                if node is None or node.subcluster:
                    continue
                node.subcluster = cluster_key
                tagged += 1
            if tagged < 2:
                continue
            if not hasattr(self.model, "plant_meta"):
                self.model.plant_meta = {}  # type: ignore[attr-defined]
            self.model.plant_meta[cluster_key] = label  # type: ignore[attr-defined]

    def _substations(self):
        """Cluster co-located buses into substation subgraphs.

        Auto-detects substations from TWO co-located signals (a bus pair
        must match BOTH):

        1. **Name prefix** — ``CHARRUA_500KV`` / ``CHARRUA_220KV`` share
           ``CHARRUA``.  Suffix patterns ``_NkV``, ``_VN``, ``_NNN`` are
           recognised; buses without a recognised suffix are excluded.
        2. **Electrically short connection** — a Line whose ``type``
           contains ``"transformer"`` OR whose per-unit R/X are both
           below the short-line thresholds joins the two buses.

        The two signals together avoid false positives: name-only would
        cluster identically-named buses with no physical link, and
        impedance-only would cluster geographically distant buses
        wired by a DC tie.

        Tags member buses with ``subcluster=f"substation:{prefix}"`` and
        stores a label in ``model.plant_meta`` keyed by the same tag.
        Buses already in a non-empty ``subcluster`` (e.g. a future
        per-bus Plant cluster) are not retagged.
        """
        # Build name → Node lookup for buses.
        bus_node_by_name: dict[str, Node] = {}
        for b in self.sys.get("bus_array", []) or []:
            bname = b.get("name")
            if not bname:
                continue
            bid = self._bid(b)
            for node in self.model.nodes:
                if node.node_id == bid:
                    bus_node_by_name[str(bname)] = node
                    break
        if not bus_node_by_name:
            return

        # Map (bus uid, bus name) → bus name (normalised lookup key).
        bus_ref_to_name: dict = {}
        for b in self.sys.get("bus_array", []) or []:
            bname = b.get("name")
            if not bname:
                continue
            bus_ref_to_name[b.get("uid")] = bname
            bus_ref_to_name[bname] = bname

        # Union-find over buses joined by a short / transformer line
        # whose endpoints share a substation prefix.
        parent: dict[str, str] = {}

        def find(x: str) -> str:
            while parent.get(x, x) != x:
                parent[x] = parent.get(parent[x], parent[x])
                x = parent[x]
            return x

        def union(a: str, b: str) -> None:
            ra, rb = find(a), find(b)
            if ra != rb:
                parent[ra] = rb

        for ln in self.sys.get("line_array", []) or []:
            if not self._line_is_intra_substation(ln):
                continue
            ba_ref = ln.get("bus_a")
            bb_ref = ln.get("bus_b")
            ba_name = bus_ref_to_name.get(ba_ref)
            bb_name = bus_ref_to_name.get(bb_ref)
            if not ba_name or not bb_name:
                continue
            pa = self._substation_prefix(str(ba_name))
            pb = self._substation_prefix(str(bb_name))
            if pa is None or pa != pb:
                continue
            parent.setdefault(str(ba_name), str(ba_name))
            parent.setdefault(str(bb_name), str(bb_name))
            union(str(ba_name), str(bb_name))

        # Collect groups by root → bus names.
        groups: dict[str, list[str]] = {}
        for bus_name in parent:
            root = find(bus_name)
            groups.setdefault(root, []).append(bus_name)

        for members in groups.values():
            if len(members) < 2:
                continue
            # Pick the longest common prefix from the members (they
            # already agreed via the per-pair check above).
            prefix = self._substation_prefix(members[0]) or members[0]
            cluster_key = f"substation:{prefix}"
            tagged = 0
            for bus_name in members:
                node = bus_node_by_name.get(bus_name)
                if node is None or node.subcluster:
                    continue
                node.subcluster = cluster_key
                tagged += 1
            if tagged < 2:
                continue
            if not hasattr(self.model, "plant_meta"):
                self.model.plant_meta = {}  # type: ignore[attr-defined]
            self.model.plant_meta[cluster_key] = (  # type: ignore[attr-defined]
                f"{prefix} substation ({tagged} buses)"
            )

    # ─── Basin detection (A: hydro drainage cascade) ────────────────────────
    def _basins(self):
        """Cluster hydro elements into drainage basins.

        A **basin** is the connected component of the hydro topology
        graph reached by traversing:

        * Reservoir's ``junction``
        * Waterway's ``junction_a`` ⇄ ``junction_b``
        * Turbine's ``junction_a`` ⇄ ``junction_b`` (Mode B built-in
          waterway)
        * Flow's ``junction``
        * FlowRight's ``junction``

        Every Junction, Waterway, Reservoir, Turbine, Flow source, and
        FlowRight that lands in a component with **at least one hydro
        element** gets ``super_cluster = "basin:<label>"``.  Generators
        wired to a basin turbine inherit the same super-cluster so the
        plant sub-cluster nests inside its basin.  Inflow-anchored
        single-junction basins are included (Rapel pattern: one
        junction with a Flow source + a terminal Mode B turbine).

        Connected components are computed via NetworkX, matching the
        convention used by ``gtopt_check_topology._analyzer``.

        Label preference:
          1. The largest-capacity reservoir in the component
          2. Otherwise the connected turbine's generator name (Rapel)
          3. Otherwise the first-named junction
        """
        import networkx as nx  # noqa: PLC0415

        junctions = self.sys.get("junction_array", []) or []
        waterways = self.sys.get("waterway_array", []) or []
        reservoirs = self.sys.get("reservoir_array", []) or []
        turbines = self.sys.get("turbine_array", []) or []
        flows = self.sys.get("flow_array", []) or []
        flow_rights = self.sys.get("flow_right_array", []) or []
        if not junctions and not waterways:
            return

        # Build a junction name lookup for ref resolution.
        junc_name_by_ref: dict = {}
        for j in junctions:
            j_name = j.get("name")
            if j_name is None:
                continue
            junc_name_by_ref[str(j.get("uid"))] = str(j_name)
            junc_name_by_ref[str(j_name)] = str(j_name)

        def jname(ref) -> str | None:
            if ref is None:
                return None
            return junc_name_by_ref.get(str(ref))

        # Build the undirected water-flow graph; connected components
        # ARE the basin topology.  Same convention as
        # ``gtopt_check_topology._analyzer._collect_reservoir_cascade_issues``.
        graph = nx.Graph()
        for j in junctions:
            jn = j.get("name")
            if jn is not None:
                graph.add_node(str(jn))
        # Waterway endpoints.
        for w in waterways:
            ja = jname(w.get("junction_a"))
            jb = jname(w.get("junction_b"))
            if ja and jb:
                graph.add_edge(ja, jb)
        # Mode B turbine endpoints (built-in waterway).
        for t in turbines:
            if t.get("flow"):
                continue
            ja = jname(t.get("junction_a"))
            jb = jname(t.get("junction_b"))
            if ja and jb:
                graph.add_edge(ja, jb)

        # Each connected component is a candidate basin.
        groups: dict[str, list[str]] = {}
        for i, comp_nodes in enumerate(nx.connected_components(graph)):
            members = sorted(comp_nodes)
            if not members:
                continue
            root = members[0]  # canonical key (lexicographic minimum)
            groups[root] = members
            _ = i  # quiet pylint when networkx returns lazily

        # Identify "interesting" junctions — those with at least one
        # hydro element attached (reservoir, turbine, flow source, or
        # flow_right).  A junction with NO hydro activity is just
        # topology, not a basin.
        interesting: set[str] = set()
        for r in reservoirs:
            jn = jname(r.get("junction"))
            if jn:
                interesting.add(jn)
        for t in turbines:
            if t.get("flow"):
                continue
            for ref in (t.get("junction_a"), t.get("junction_b")):
                jn = jname(ref)
                if jn:
                    interesting.add(jn)
            # Mode A turbines: follow the waterway to its junctions.
            ww_ref = t.get("waterway")
            if ww_ref:
                for w in waterways:
                    if w.get("name") == ww_ref or w.get("uid") == ww_ref:
                        for ref in (w.get("junction_a"), w.get("junction_b")):
                            jn = jname(ref)
                            if jn:
                                interesting.add(jn)
                        break
        for f in flows:
            jn = jname(f.get("junction"))
            if jn:
                interesting.add(jn)
        for fr in flow_rights:
            jn = jname(fr.get("junction_a"))
            if jn:
                interesting.add(jn)

        # Pick a label for each component.
        def pick_label(component: set[str]) -> str:
            # Largest-capacity reservoir wins.
            best_cap = -1.0
            best_name = None
            for r in reservoirs:
                jref = r.get("junction")
                jn = jname(jref) if jref else None
                if jn not in component:
                    continue
                cap = float(r.get("capacity") or r.get("emax") or 0.0)
                if cap > best_cap:
                    best_cap = cap
                    best_name = r.get("name")
            if best_name:
                return str(best_name)
            # No reservoir — pick the turbine's generator NAME (not
            # uid) as the label.  Typical hydro plant name: RAPEL,
            # CANUTILLAR, etc.
            gen_name_by_ref: dict = {}
            for g in self.sys.get("generator_array", []) or []:
                gn = g.get("name")
                if gn is None:
                    continue
                gen_name_by_ref[str(g.get("uid"))] = str(gn)
                gen_name_by_ref[str(gn)] = str(gn)
            for t in turbines:
                if t.get("flow"):
                    continue
                for ref in (t.get("junction_a"), t.get("junction_b")):
                    jn = jname(ref)
                    if jn in component:
                        gen_ref = t.get("generator")
                        if gen_ref is not None:
                            resolved = gen_name_by_ref.get(str(gen_ref))
                            if resolved:
                                return resolved
                        return str(t.get("name") or jn)
            # Otherwise first-named junction in the component.
            return sorted(component)[0]

        # Every connected component containing ≥1 interesting junction
        # becomes a basin — INCLUDING single-junction basins (Rapel-
        # style: one junction with an inflow + a turbine, no upstream
        # reservoir and no downstream cascade).
        basin_meta: dict[str, str] = {}
        comp_label: dict[frozenset, str] = {}
        for _root, members in groups.items():
            comp = set(members)
            if not comp & interesting:
                continue
            label = pick_label(comp)
            comp_label[frozenset(comp)] = label
            basin_meta[f"basin:{label}"] = f"{label} basin ({len(comp)} junctions)"

        if not comp_label:
            return

        # Junction → basin label lookup.
        junc_to_basin: dict[str, str] = {}
        for comp, label in comp_label.items():
            for jn in comp:
                junc_to_basin[jn] = label

        def tag(node_id: str, label: str) -> None:
            for n in self.model.nodes:
                if n.node_id == node_id:
                    n.super_cluster = f"basin:{label}"
                    return

        # Tag junction nodes.
        for j in junctions:
            jn = j.get("name")
            if not jn:
                continue
            lab = junc_to_basin.get(str(jn))
            if lab:
                tag(self._jid(j), lab)

        # Waterways are rendered as edges, not nodes — they inherit
        # their basin placement automatically from their endpoint
        # junctions via the renderer's edge bucketing.

        # Tag reservoir nodes.
        for r in reservoirs:
            jref = r.get("junction")
            jn = jname(jref) if jref else None
            lab = junc_to_basin.get(jn) if jn else None
            if lab:
                tag(self._rid(r), lab)

        # Tag turbine nodes (and the connected generator if available).
        for t in turbines:
            if t.get("flow"):
                continue
            ja = jname(t.get("junction_a"))
            jb = jname(t.get("junction_b"))
            lab = junc_to_basin.get(ja) if ja else None
            if lab is None and jb:
                lab = junc_to_basin.get(jb)
            if lab is None:
                # Mode A: try via waterway
                ww_ref = t.get("waterway")
                if ww_ref:
                    for w in waterways:
                        if w.get("name") == ww_ref or w.get("uid") == ww_ref:
                            ja2 = jname(w.get("junction_a"))
                            jb2 = jname(w.get("junction_b"))
                            lab = (junc_to_basin.get(ja2) if ja2 else None) or (
                                junc_to_basin.get(jb2) if jb2 else None
                            )
                            break
            if lab:
                tag(self._tid(t), lab)
                # Tag the connected generator too.
                gen_ref = t.get("generator")
                if gen_ref is not None:
                    for g in self.sys.get("generator_array", []) or []:
                        if g.get("uid") == gen_ref or g.get("name") == gen_ref:
                            tag(self._gid(g), lab)
                            break

        # Tag Flow source nodes.
        for f in flows:
            jref = f.get("junction")
            jn = jname(jref) if jref else None
            lab = junc_to_basin.get(jn) if jn else None
            if lab:
                tag(self._fid(f), lab)

        # Tag FlowRight nodes.
        for fr in flow_rights:
            jref = fr.get("junction_a")
            jn = jname(jref) if jref else None
            lab = junc_to_basin.get(jn) if jn else None
            if lab:
                tag(self._frid(fr), lab)

        if not hasattr(self.model, "plant_meta"):
            self.model.plant_meta = {}  # type: ignore[attr-defined]
        self.model.plant_meta.update(basin_meta)  # type: ignore[attr-defined]

    # ─── Reserve-zone grouping (B) ──────────────────────────────────────────
    def _reserve_zone_groups(self):
        """Cluster generators by their ReserveZone membership.

        Resolves via ``ReserveProvision.generator → reserve_zones``.
        Only applied when a generator does NOT already carry a
        ``subcluster`` (plant clustering always wins) and at least two
        generators share the zone.  Hydro plants and substations are
        unaffected.
        """
        zones = self.sys.get("reserve_zone_array", []) or []
        provisions = self.sys.get("reserve_provision_array", []) or []
        if not zones or not provisions:
            return

        # Resolve zone uid/name → canonical name.
        zone_name_by_ref: dict = {}
        for z in zones:
            zn = z.get("name")
            if zn is None:
                continue
            zone_name_by_ref[str(z.get("uid"))] = str(zn)
            zone_name_by_ref[str(zn)] = str(zn)

        # Group generators by reserve zone.
        by_zone: dict[str, list] = {}
        for p in provisions:
            gen_ref = p.get("generator")
            if gen_ref is None:
                continue
            for zref in p.get("reserve_zones") or []:
                zname = zone_name_by_ref.get(str(zref))
                if not zname:
                    continue
                by_zone.setdefault(zname, []).append(gen_ref)

        gen_by_ref: dict = {}
        for g in self.sys.get("generator_array", []) or []:
            gen_by_ref[str(g.get("uid"))] = g
            gen_by_ref[str(g.get("name") or "")] = g

        meta: dict[str, str] = {}
        for zname, refs in by_zone.items():
            cluster_key = f"reserve_zone:{zname}"
            tagged = 0
            for ref in refs:
                g = gen_by_ref.get(str(ref))
                if g is None:
                    continue
                gid = self._gid(g)
                for n in self.model.nodes:
                    if n.node_id == gid and not n.subcluster:
                        n.subcluster = cluster_key
                        tagged += 1
                        break
            if tagged < 2:
                continue
            meta[cluster_key] = f"{zname} reserve zone ({tagged} units)"

        if meta:
            if not hasattr(self.model, "plant_meta"):
                self.model.plant_meta = {}  # type: ignore[attr-defined]
            self.model.plant_meta.update(meta)  # type: ignore[attr-defined]

    # ─── Fuel-sharing grouping (C) ─────────────────────────────────────────
    @staticmethod
    def _fuel_family(fuel_name: str) -> str:
        """Roll up a fuel-tier name to its plant/family root.

        PLEXOS subdivides each fuel into price tiers (``Gas_<plant>_A``
        / ``_B`` / ``_C`` / ``_GN_A`` / ``_INF`` etc.).  Each tier
        ends up with only a handful of consumers — too few for the
        cluster to fire — even though the underlying plant has 12+
        generators in total.  This helper strips the trailing tier
        suffix so all tiers of one plant share the family name:

          ``Gas_EnelMejillones_GN_A``   → ``Gas_EnelMejillones``
          ``Gas_EnelMejillones_GN_B``   → ``Gas_EnelMejillones``
          ``Gas_GNLQuintero_INF``       → ``Gas_GNLQuintero``
          ``Diesel_AguasBlancas``       → ``Diesel_AguasBlancas`` (no tier)
          ``Carbon_Cochrane1``          → ``Carbon_Cochrane1`` (no tier)

        Heuristic: keep the first two ``_``-separated segments
        (``<carrier>_<plant>``) and drop the rest — this is the
        PLEXOS convention for Gas / GLP / FuelOil tiers; Diesel /
        Carbon / Biomasa fuels typically only have two segments
        already so the rollup is a no-op for them.
        """
        parts = fuel_name.split("_")
        if len(parts) <= 2:
            return fuel_name
        # Carriers known to be tier-subdivided in PLEXOS:
        if parts[0] in {"Gas", "GLP", "FuelOil"}:
            return "_".join(parts[:2])
        # Everything else: keep as-is.
        return fuel_name

    def _fuel_groups(self):
        """Cluster generators that consume the same Fuel **family**.

        Only generators without an existing ``subcluster`` are tagged
        (plant / hydro / reserve-zone clusters all win).  A family
        must have ≥2 consumers to form a cluster.  Tier subdivisions
        (``Gas_*_A``/``_B``/``_GN_C``/``_INF``…) are rolled up to one
        family per plant via :meth:`_fuel_family`, so a single bus
        with 12 ``Gas_EnelMejillones_*`` tiers all show as the same
        cluster instead of dissolving into too-small per-tier groups.

        Works in both the un-aggregated mode (tags individual ``gen_*``
        nodes) and the aggregated modes (``agg_bus_*`` /
        ``agg_type_*`` / ``agg_global_*`` super-nodes inherit a family
        cluster when ALL their member generators share that family).
        """
        fuels = self.sys.get("fuel_array", []) or []
        if not fuels:
            return

        # Resolve fuel uid/name → canonical name → family.
        fuel_family_by_ref: dict = {}
        for f in fuels:
            fn = f.get("name")
            if fn is None:
                continue
            family = self._fuel_family(str(fn))
            fuel_family_by_ref[str(f.get("uid"))] = family
            fuel_family_by_ref[str(fn)] = family

        # Group generators by fuel family.
        gen_family: dict = {}  # gen uid → family
        for g in self.sys.get("generator_array", []) or []:
            fref = g.get("fuel")
            if fref is None or fref == "":
                continue
            family = fuel_family_by_ref.get(str(fref))
            if not family:
                continue
            gen_family[g.get("uid")] = family

        if not gen_family:
            return

        # Build node-id → family map.  For aggregated nodes, the
        # family is well-defined only when ALL member gens share it.
        nid_family: dict = {}
        agg_members: dict = {}  # nid → list of gen-uids represented
        for g in self.sys.get("generator_array", []) or []:
            gid = self._gid(g)
            agg_members.setdefault(gid, []).append(g.get("uid"))
        # ``_resolve_gen_node_id`` returns the rendered node id for any
        # generator (respects aggregation).  Group gens by their
        # rendered node id, then assign that node a family only when
        # every member agrees.
        rendered_by_gen: dict = {}
        try:
            resolver_fn = self._resolve_gen_node_id
        except AttributeError:
            resolver_fn = None
        for g in self.sys.get("generator_array", []) or []:
            if resolver_fn:
                rid = resolver_fn(g.get("uid")) or self._gid(g)
            else:
                rid = self._gid(g)
            rendered_by_gen.setdefault(rid, []).append(g.get("uid"))

        from collections import Counter  # noqa: PLC0415

        for rid, member_uids in rendered_by_gen.items():
            families: list = [gen_family[u] for u in member_uids if u in gen_family]
            if not families:
                continue
            counts = Counter(families)
            (top_family, top_count) = counts.most_common(1)[0]
            # Dominant family wins when it covers ≥50 % of the
            # generators with a fuel reference at this rendered node.
            # Rationale: aggregated bus super-nodes typically hold one
            # plant's fleet (e.g. 100 Gas_EnelMejillones gens at
            # Atacama220_BP1) plus a sprinkling of diesel back-ups —
            # treating the dominant family as the cluster tag matches
            # the user's mental model of "this bus is the Mejillones
            # gas plant".  Strict all-share would never fire on real
            # PLEXOS topology.
            if top_count * 2 >= len(families):
                nid_family[rid] = top_family

        # Now group rendered node-ids by family + tag.
        by_family: dict = {}
        for nid, family in nid_family.items():
            by_family.setdefault(family, []).append(nid)

        meta: dict[str, str] = {}
        for family, nids in by_family.items():
            cluster_key = f"fuel:{family}"
            tagged = 0
            for nid in nids:
                for n in self.model.nodes:
                    if n.node_id == nid and not n.subcluster:
                        n.subcluster = cluster_key
                        tagged += 1
                        break
            if tagged < 2:
                continue
            meta[cluster_key] = f"{family} fuel ({tagged} units)"

        if meta:
            if not hasattr(self.model, "plant_meta"):
                self.model.plant_meta = {}  # type: ignore[attr-defined]
            self.model.plant_meta.update(meta)  # type: ignore[attr-defined]

    @staticmethod
    def _bus_ref_resolver(buses: list) -> tuple[dict, dict]:
        """Return (ref → canonical uid, uid → node_id-builder dict).

        ``bus_a`` / ``bus_b`` may be either an integer uid or a string
        name — both forms must resolve to the same canonical key.
        """
        ref_to_uid: dict = {}
        bus_by_uid: dict = {}
        for b in buses:
            uid = b.get("uid")
            if uid is None:
                continue
            ref_to_uid[uid] = uid
            ref_to_uid[str(uid)] = uid
            name = b.get("name")
            if name is not None:
                ref_to_uid[name] = uid
                ref_to_uid[str(name)] = uid
            bus_by_uid[uid] = b
        return ref_to_uid, bus_by_uid

    # ─── N1: critical topology (bridges + articulation points) ───────────────
    def _critical_topology(self):
        """Mark AC bridges and articulation points (N-1 single-failure).

        Uses :func:`networkx.bridges` and
        :func:`networkx.articulation_points` on the AC bus+line graph.
        HVDC ties and out-of-service lines are excluded.
        """
        import networkx as nx  # noqa: PLC0415

        buses = self.sys.get("bus_array", []) or []
        lines = self.sys.get("line_array", []) or []
        if not buses or not lines:
            return

        ref_to_uid, bus_by_uid = self._bus_ref_resolver(buses)
        graph = nx.Graph()
        for uid in bus_by_uid:
            graph.add_node(uid)
        for ln in lines:
            ltype = (ln.get("type") or "").lower()
            if "dc" in ltype or "hvdc" in ltype:
                continue
            if ln.get("active") in (0, False, "0", "false"):
                continue
            a = ref_to_uid.get(ln.get("bus_a"))
            b = ref_to_uid.get(ln.get("bus_b"))
            if a is None or b is None or a == b:
                continue
            graph.add_edge(a, b)

        if graph.number_of_edges() == 0:
            return

        bridges = set()
        for a, b in nx.bridges(graph):
            bridges.add((a, b))
            bridges.add((b, a))
        articulations = set(nx.articulation_points(graph))

        bus_node_id_by_uid: dict = {uid: self._bid(b) for uid, b in bus_by_uid.items()}
        for n in self.model.nodes:
            for uid, nid in bus_node_id_by_uid.items():
                if n.node_id == nid and uid in articulations:
                    n.is_critical = True
                    if n.tooltip and "articulation" not in n.tooltip:
                        n.tooltip += " [critical: articulation point]"
                    break

        nid_to_uid = {nid: uid for uid, nid in bus_node_id_by_uid.items()}
        for e in self.model.edges:
            a = nid_to_uid.get(e.src)
            b = nid_to_uid.get(e.dst)
            if a is None or b is None:
                continue
            if (a, b) in bridges:
                e.is_critical = True
                if e.label and "BRIDGE" not in e.label:
                    e.label = e.label + "\n[BRIDGE]"

    # ─── N2: community detection ──────────────────────────────────────────────
    def _communities(self):
        """Auto-detect topological communities of buses via modularity.

        Uses :func:`networkx.algorithms.community.greedy_modularity_communities`
        on the AC bus+line graph.  Each community ≥3 buses (small ones
        are noise) becomes a ``super_cluster = "community:N"`` tag on
        its member buses — and on the generators wired to those buses
        that are not already in a basin.

        Hydro junctions / reservoirs / turbines are NOT touched —
        ``_basins`` runs after this and uses ``super_cluster`` to mean
        ``basin:X`` on the water side.  Same field, disjoint node types.
        """
        import networkx as nx  # noqa: PLC0415
        from networkx.algorithms import community as nx_community  # noqa: PLC0415

        buses = self.sys.get("bus_array", []) or []
        lines = self.sys.get("line_array", []) or []
        if len(buses) < 6 or not lines:
            return

        ref_to_uid, bus_by_uid = self._bus_ref_resolver(buses)
        graph = nx.Graph()
        for uid in bus_by_uid:
            graph.add_node(uid)
        for ln in lines:
            ltype = (ln.get("type") or "").lower()
            if "dc" in ltype or "hvdc" in ltype:
                continue
            if ln.get("active") in (0, False, "0", "false"):
                continue
            a = ref_to_uid.get(ln.get("bus_a"))
            b = ref_to_uid.get(ln.get("bus_b"))
            if a is None or b is None or a == b:
                continue
            graph.add_edge(a, b)

        if graph.number_of_edges() == 0:
            return

        try:
            communities = list(nx_community.greedy_modularity_communities(graph))
        except (StopIteration, ValueError):
            return

        uid_to_community: dict = {}
        for idx, comm in enumerate(communities):
            if len(comm) < 3:
                continue
            for uid in comm:
                uid_to_community[uid] = idx + 1

        if not uid_to_community:
            return

        bus_node_id_by_uid: dict = {uid: self._bid(b) for uid, b in bus_by_uid.items()}

        community_meta: dict[str, str] = {}
        community_counts: dict[str, int] = {}
        for uid, cidx in uid_to_community.items():
            nid = bus_node_id_by_uid.get(uid)
            if nid is None:
                continue
            for n in self.model.nodes:
                if n.node_id == nid and not n.super_cluster:
                    label = f"community:{cidx}"
                    n.super_cluster = label
                    community_counts[label] = community_counts.get(label, 0) + 1
                    break

        # Tag generators by their bus's community (only if they aren't
        # already in a basin from the hydro side).
        for g in self.sys.get("generator_array", []) or []:
            bus_uid = ref_to_uid.get(g.get("bus"))
            cidx = uid_to_community.get(bus_uid)
            if cidx is None:
                continue
            gid = self._gid(g)
            for n in self.model.nodes:
                if n.node_id == gid and not n.super_cluster:
                    label = f"community:{cidx}"
                    n.super_cluster = label
                    community_counts[label] = community_counts.get(label, 0) + 1
                    break

        for label, count in community_counts.items():
            community_meta[label] = f"Region {label.split(':')[1]} ({count} nodes)"

        if community_meta:
            if not hasattr(self.model, "plant_meta"):
                self.model.plant_meta = {}  # type: ignore[attr-defined]
            self.model.plant_meta.update(community_meta)  # type: ignore[attr-defined]

    # ─── N4: basin layout via topological sort ────────────────────────────────
    def _basin_layout_hints(self):
        """Annotate basin junctions with upstream→downstream rank hints.

        Builds a directed water-flow graph (junction_a → junction_b on
        every Waterway and Mode B Turbine) per basin, runs
        :func:`networkx.topological_sort`, and stores the canonical
        order on ``model.basin_topo_order`` keyed by basin tag.  The
        graphviz renderer can then emit ``rank=source`` on the highest
        upstream junction and ``rank=sink`` on the terminal-most
        junctions of each basin so the cluster lays out top → bottom
        following the water-flow.

        Returns without action when the water-flow graph contains a
        cycle (gtopt_check_topology already reports those as a data
        bug — we silently fall back to default layout).
        """
        import networkx as nx  # noqa: PLC0415

        junctions = self.sys.get("junction_array", []) or []
        waterways = self.sys.get("waterway_array", []) or []
        turbines = self.sys.get("turbine_array", []) or []
        if not junctions or not waterways:
            return

        junc_name_by_ref: dict = {}
        for j in junctions:
            j_name = j.get("name")
            if j_name is None:
                continue
            junc_name_by_ref[str(j.get("uid"))] = str(j_name)
            junc_name_by_ref[str(j_name)] = str(j_name)

        def jname(ref) -> str | None:
            if ref is None:
                return None
            return junc_name_by_ref.get(str(ref))

        # Junction name → basin tag (set by `_basins`).
        junc_to_basin: dict[str, str] = {}
        for n in self.model.nodes:
            if not n.node_id.startswith("junc_") or not n.super_cluster:
                continue
            for j in junctions:
                if self._jid(j) == n.node_id:
                    name = j.get("name")
                    if name:
                        junc_to_basin[str(name)] = n.super_cluster
                    break

        if not junc_to_basin:
            return

        # Per-basin directed water-flow graph.
        per_basin_graph: dict[str, "nx.DiGraph"] = {}

        def graph_for(basin_tag: str) -> "nx.DiGraph":
            if basin_tag not in per_basin_graph:
                per_basin_graph[basin_tag] = nx.DiGraph()
            return per_basin_graph[basin_tag]

        for w in waterways:
            ja = jname(w.get("junction_a"))
            jb = jname(w.get("junction_b"))
            if not ja or not jb:
                continue
            tag = junc_to_basin.get(ja) or junc_to_basin.get(jb)
            if not tag:
                continue
            graph_for(tag).add_edge(ja, jb)

        for t in turbines:
            if t.get("flow"):
                continue
            ja = jname(t.get("junction_a"))
            jb = jname(t.get("junction_b"))
            if not ja or not jb:
                continue
            tag = junc_to_basin.get(ja) or junc_to_basin.get(jb)
            if not tag:
                continue
            graph_for(tag).add_edge(ja, jb)

        # Map junction name → its diagram node_id for renderer lookup.
        junc_name_to_nid: dict[str, str] = {}
        for j in junctions:
            jn = j.get("name")
            if jn is not None:
                junc_name_to_nid[str(jn)] = self._jid(j)

        topo_order: dict[str, list[str]] = {}
        for tag, g in per_basin_graph.items():
            try:
                order = list(nx.topological_sort(g))
            except nx.NetworkXUnfeasible:
                # Water-flow cycle — leave layout default.
                continue
            nid_order = [
                junc_name_to_nid[name] for name in order if name in junc_name_to_nid
            ]
            if len(nid_order) >= 2:
                topo_order[tag] = nid_order

        if topo_order:
            # Stash on the model for the renderer to consult.  Maps
            # basin tag → ordered list of junction node IDs from
            # upstream to downstream.
            self.model.basin_topo_order = topo_order  # type: ignore[attr-defined]

    # ─── N3: edge betweenness centrality (backbone identification) ────────────
    def _backbone_betweenness(self):
        """Score AC lines by edge betweenness centrality.

        Edges on many shortest paths between bus pairs carry more of
        the network's transit load — they're the **backbone**.  Stash
        normalised scores in ``model.edge_betweenness`` keyed by
        (bus_uid_a, bus_uid_b); the renderer scales line pen-width
        proportional to the score so the HV trunk pops visually.

        Uses :func:`networkx.edge_betweenness_centrality` with
        ``weight = reactance`` so the "shortest path" is the
        electrical-distance path that current actually prefers
        (Kirchhoff voltage law).  Edges without reactance default to
        unit weight.

        Skipped on very small cases (< 4 buses) — betweenness is
        meaningless there.
        """
        import networkx as nx  # noqa: PLC0415

        buses = self.sys.get("bus_array", []) or []
        lines = self.sys.get("line_array", []) or []
        if len(buses) < 4 or not lines:
            return

        ref_to_uid, bus_by_uid = self._bus_ref_resolver(buses)
        graph = nx.Graph()
        for uid in bus_by_uid:
            graph.add_node(uid)
        for ln in lines:
            ltype = (ln.get("type") or "").lower()
            if "dc" in ltype or "hvdc" in ltype:
                continue
            if ln.get("active") in (0, False, "0", "false"):
                continue
            a = ref_to_uid.get(ln.get("bus_a"))
            b = ref_to_uid.get(ln.get("bus_b"))
            if a is None or b is None or a == b:
                continue
            try:
                w = float(ln.get("reactance") or 0.0)
            except (TypeError, ValueError):
                w = 0.0
            if w <= 0.0:
                w = 1.0
            graph.add_edge(a, b, weight=w)

        if graph.number_of_edges() < 2:
            return

        try:
            betw = nx.edge_betweenness_centrality(graph, weight="weight")
        except (StopIteration, ValueError):
            return

        if not betw:
            return

        max_b = max(betw.values()) or 1.0

        scored: dict = {}
        for (a, b), v in betw.items():
            normalised = v / max_b
            scored[(a, b)] = normalised
            scored[(b, a)] = normalised

        # Stash the betweenness map on the model so the renderer can
        # add it as a separate visual emphasis without mutating the
        # existing voltage-derived edge weights (which the rendering
        # tests pin to specific values).  Keyed by (node_id_a,
        # node_id_b) for direct renderer access; symmetric.
        bus_node_id_by_uid: dict = {}
        for b in buses:
            uid = b.get("uid")
            if uid is not None:
                bus_node_id_by_uid[uid] = self._bid(b)
        by_nid: dict = {}
        for (a, b), score in scored.items():
            na = bus_node_id_by_uid.get(a)
            nb = bus_node_id_by_uid.get(b)
            if na and nb:
                by_nid[(na, nb)] = score
                by_nid[(nb, na)] = score
        self.model.edge_betweenness = by_nid  # type: ignore[attr-defined]

    # ─── Isolated bus + orphan generator + electrical-loop detection ─────────
    def _isolated_buses(self):
        """Flag buses with no AC connections.

        Uses :func:`networkx.isolates` on the AC bus+line graph.  An
        isolated bus has no line, no transformer — anything wired to
        it cannot exchange power with the rest of the system.  Marked
        with ``is_critical = True`` and a "[isolated]" tooltip badge.
        """
        import networkx as nx  # noqa: PLC0415

        buses = self.sys.get("bus_array", []) or []
        lines = self.sys.get("line_array", []) or []
        if len(buses) < 2:
            return  # single-bus copper plate is intentionally "isolated".

        ref_to_uid, bus_by_uid = self._bus_ref_resolver(buses)
        graph = nx.Graph()
        for uid in bus_by_uid:
            graph.add_node(uid)
        for ln in lines:
            if ln.get("active") in (0, False, "0", "false"):
                continue
            a = ref_to_uid.get(ln.get("bus_a"))
            b = ref_to_uid.get(ln.get("bus_b"))
            if a is None or b is None or a == b:
                continue
            graph.add_edge(a, b)

        isolated_uids = set(nx.isolates(graph))
        if not isolated_uids:
            return

        bus_node_id_by_uid: dict = {uid: self._bid(b) for uid, b in bus_by_uid.items()}
        for n in self.model.nodes:
            for uid, nid in bus_node_id_by_uid.items():
                if n.node_id == nid and uid in isolated_uids:
                    n.is_critical = True
                    if "isolated" not in (n.tooltip or ""):
                        n.tooltip = (n.tooltip or "") + " [critical: isolated bus]"
                    break

        # Stash count on the model for the renderer / report.
        existing = getattr(self.model, "isolated_bus_count", 0)
        self.model.isolated_bus_count = (  # type: ignore[attr-defined]
            existing + len(isolated_uids)
        )

    def _orphan_generators(self):
        """N11: flag generators whose bus can't reach any demand bus.

        Uses :func:`networkx.has_path` from the generator's bus to
        every demand-carrying bus.  When NONE of them are reachable,
        the generator is an orphan — its output is stranded.
        """
        import networkx as nx  # noqa: PLC0415

        buses = self.sys.get("bus_array", []) or []
        lines = self.sys.get("line_array", []) or []
        generators = self.sys.get("generator_array", []) or []
        demands = self.sys.get("demand_array", []) or []
        if not generators or not demands or not lines or len(buses) < 2:
            return

        ref_to_uid, bus_by_uid = self._bus_ref_resolver(buses)
        graph = nx.Graph()
        for uid in bus_by_uid:
            graph.add_node(uid)
        for ln in lines:
            if ln.get("active") in (0, False, "0", "false"):
                continue
            a = ref_to_uid.get(ln.get("bus_a"))
            b = ref_to_uid.get(ln.get("bus_b"))
            if a is None or b is None or a == b:
                continue
            graph.add_edge(a, b)

        demand_bus_uids: set = set()
        for d in demands:
            uid = ref_to_uid.get(d.get("bus"))
            if uid is not None:
                demand_bus_uids.add(uid)
        if not demand_bus_uids:
            return

        # For efficiency: precompute the connected component of each
        # demand bus.  A generator's bus is "connected to a demand"
        # iff it lies in the same component as ≥1 demand bus.
        comp_with_demand: set = set()
        for comp in nx.connected_components(graph):
            if comp & demand_bus_uids:
                comp_with_demand.update(comp)

        orphans: list[str] = []
        for g in generators:
            bus_uid = ref_to_uid.get(g.get("bus"))
            if bus_uid is None:
                continue
            if bus_uid in comp_with_demand:
                continue
            gid = self._gid(g)
            for n in self.model.nodes:
                if n.node_id == gid:
                    n.is_critical = True
                    if "orphan" not in (n.tooltip or ""):
                        n.tooltip = (n.tooltip or "") + " [critical: orphan generator]"
                    orphans.append(gid)
                    break

        if orphans:
            existing = getattr(self.model, "orphan_generator_count", 0)
            self.model.orphan_generator_count = (  # type: ignore[attr-defined]
                existing + len(orphans)
            )

    # ─── N5: k-core HV backbone overlay ──────────────────────────────────────
    def _hv_backbone(self):
        """Identify HV backbone via k-core (k ≥ 3) of the AC graph.

        Stashes the resulting bus uid set on ``model.hv_backbone_buses``
        so the renderer can draw the backbone with thicker borders /
        a different colour without re-tagging the subcluster.
        """
        import networkx as nx  # noqa: PLC0415

        buses = self.sys.get("bus_array", []) or []
        lines = self.sys.get("line_array", []) or []
        if len(buses) < 8 or not lines:
            return

        ref_to_uid, bus_by_uid = self._bus_ref_resolver(buses)
        graph = nx.Graph()
        for uid in bus_by_uid:
            graph.add_node(uid)
        for ln in lines:
            if ln.get("active") in (0, False, "0", "false"):
                continue
            a = ref_to_uid.get(ln.get("bus_a"))
            b = ref_to_uid.get(ln.get("bus_b"))
            if a is None or b is None or a == b:
                continue
            graph.add_edge(a, b)

        if graph.number_of_edges() == 0:
            return

        try:
            core = nx.k_core(graph, k=3)
        except (nx.NetworkXError, ValueError):
            return
        backbone_uids = set(core.nodes())
        if not backbone_uids:
            return

        bus_node_id_by_uid: dict = {uid: self._bid(b) for uid, b in bus_by_uid.items()}
        backbone_nids: set = {
            bus_node_id_by_uid[uid]
            for uid in backbone_uids
            if uid in bus_node_id_by_uid
        }
        self.model.hv_backbone_buses = backbone_nids  # type: ignore[attr-defined]

    # ─── N6 / N7: focus subgraph + electrical distance ───────────────────────
    def _focus_distance(self):
        """Annotate every bus with its electrical-distance hop count
        from the configured focus buses.

        Uses :func:`networkx.shortest_path_length` weighted by line
        reactance.  Distance 0 = a focus bus itself; ``None`` = bus
        not reachable from any focus bus.  Stashed on
        ``model.bus_focus_distance`` keyed by node_id.

        No-op when ``opts.focus_buses`` is empty.
        """
        import networkx as nx  # noqa: PLC0415

        focus = list(self.opts.focus_buses or [])
        if not focus:
            return
        buses = self.sys.get("bus_array", []) or []
        lines = self.sys.get("line_array", []) or []
        if not buses or not lines:
            return

        ref_to_uid, bus_by_uid = self._bus_ref_resolver(buses)
        graph = nx.Graph()
        for uid in bus_by_uid:
            graph.add_node(uid)
        for ln in lines:
            if ln.get("active") in (0, False, "0", "false"):
                continue
            a = ref_to_uid.get(ln.get("bus_a"))
            b = ref_to_uid.get(ln.get("bus_b"))
            if a is None or b is None or a == b:
                continue
            try:
                w = float(ln.get("reactance") or 0.0)
            except (TypeError, ValueError):
                w = 0.0
            graph.add_edge(a, b, weight=max(w, 0.001))

        focus_uids: set = {ref_to_uid.get(f) for f in focus}
        focus_uids.discard(None)
        if not focus_uids:
            return

        # Multi-source shortest path: distance from nearest focus bus.
        dist_by_uid: dict = {}
        for f_uid in focus_uids:
            if f_uid not in graph:
                continue
            try:
                distances = nx.single_source_dijkstra_path_length(
                    graph, f_uid, weight="weight"
                )
            except nx.NetworkXError:
                continue
            for uid, d in distances.items():
                if uid not in dist_by_uid or d < dist_by_uid[uid]:
                    dist_by_uid[uid] = d

        if not dist_by_uid:
            return

        bus_node_id_by_uid: dict = {uid: self._bid(b) for uid, b in bus_by_uid.items()}
        dist_by_nid: dict = {}
        for uid, d in dist_by_uid.items():
            nid = bus_node_id_by_uid.get(uid)
            if nid:
                dist_by_nid[nid] = d
        self.model.bus_focus_distance = dist_by_nid  # type: ignore[attr-defined]

    # ─── N10: electrical cycle detection ─────────────────────────────────────
    def _electrical_loops(self):
        """Detect AC loops (cycles) via NetworkX ``cycle_basis``.

        Stashes the cycle list on ``model.electrical_loops`` so the
        renderer / a future overlay can highlight cycle members.
        Cycles are reported as lists of bus node_ids.
        """
        import networkx as nx  # noqa: PLC0415

        buses = self.sys.get("bus_array", []) or []
        lines = self.sys.get("line_array", []) or []
        if len(buses) < 3 or not lines:
            return

        ref_to_uid, bus_by_uid = self._bus_ref_resolver(buses)
        graph = nx.Graph()
        for uid in bus_by_uid:
            graph.add_node(uid)
        for ln in lines:
            if ln.get("active") in (0, False, "0", "false"):
                continue
            a = ref_to_uid.get(ln.get("bus_a"))
            b = ref_to_uid.get(ln.get("bus_b"))
            if a is None or b is None or a == b:
                continue
            graph.add_edge(a, b)

        if graph.number_of_edges() < 3:
            return

        try:
            cycles = nx.cycle_basis(graph)
        except nx.NetworkXError:
            return
        if not cycles:
            return

        bus_node_id_by_uid: dict = {uid: self._bid(b) for uid, b in bus_by_uid.items()}
        cycle_nids: list[list[str]] = []
        for cyc in cycles:
            nids = [bus_node_id_by_uid.get(uid) for uid in cyc]
            if all(nids):
                cycle_nids.append([n for n in nids if n])
        self.model.electrical_loops = cycle_nids  # type: ignore[attr-defined]
