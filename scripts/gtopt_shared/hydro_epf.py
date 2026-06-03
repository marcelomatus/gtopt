# SPDX-License-Identifier: BSD-3-Clause
"""Hydro Energy Production Factor (EPF) per Reservoir.

The EPF of a reservoir R is the total MW generated per m³/s of water
released, summed over every turbine in the cascade segment between R
and the next downstream reservoir (or the ocean / sink junction).

EPF[R] = Σ turbine.production_factor over every turbine on the path
         from R's output junction, walking the directed
         junction→waterway/turbine→junction graph downstream, STOPPING
         when:
           * the path reaches another reservoir's anchor junction
             (that reservoir owns its own downstream cascade segment), or
           * the path reaches a sink (junction with no outgoing edge —
             the ocean for terminal rivers).

Use cases (consumed by both plexos2gtopt and plp2gtopt):
  * **Emissions-mode terminal value** (issue #519): the carbon shadow
    price per stored cumec held in reservoir R is
        EPF[R] · gas_em · loss_mult       [tCO2eq per (m³/s)·h]
    with `gas_em = 0.5 tCO2eq/MWh` (long-term marginal replacement
    in Chile is gas) and `loss_mult = 0.95` (round-trip efficiency).
  * **Cross-check** against ``reservoir_production_factor_array``
    entries plp2gtopt emits — summing ``mean_production_factor`` over
    matching reservoir entries must equal the EPF derived here.
  * **Generic hydro-cascade analysis** — walking from any node, with or
    without the next-reservoir stopping rule.

The module deliberately depends only on ``networkx`` so it can run in
either converter's process without extra hooks.
"""

from __future__ import annotations

from typing import Any

import networkx as nx


_GAS_EMISSION_TCO2_PER_MWH: float = 0.5
"""Long-term marginal-replacement emission rate for Chile (gas)."""

_LOSS_MULTIPLIER: float = 0.95
"""Efficiency / loss multiplier applied to the gas-equivalent rate."""


def build_hydro_graph(system: dict[str, Any]) -> nx.DiGraph:
    """Build a directed junction-graph from a gtopt ``system`` dict.

    Nodes are junction names; edges are turbines (kind='turbine') or
    waterways (kind='waterway'), each carrying:

      * ``kind`` — 'turbine' or 'waterway'
      * ``name`` — element name
      * ``production_factor`` — float (0 for waterways)

    Turbines with no ``junction_b`` are skipped (built-in waterway
    schema where the downstream junction is implicit at the
    ``main_reservoir`` — those bus-only turbines don't contribute a
    cascade arc).
    """
    G: nx.DiGraph = nx.DiGraph()
    for j in system.get("junction_array", []) or []:
        name = j.get("name")
        if name:
            G.add_node(name)

    # (waterway-name → production_factor) from turbines that reference
    # a Waterway by name (plp2gtopt schema).  When multiple turbines
    # share a waterway the production_factors sum.
    pf_on_waterway: dict[str, float] = {}
    for t in system.get("turbine_array", []) or []:
        wname = t.get("waterway")
        if isinstance(wname, str) and wname:
            pf_on_waterway[wname] = pf_on_waterway.get(wname, 0.0) + float(
                t.get("production_factor", 0.0) or 0.0
            )

    # Add waterway edges — when a turbine references this waterway by
    # name, the edge inherits the turbine's production_factor (kind=
    # 'turbine' for cascade attribution).  Plain waterways get PF=0.
    for w in system.get("waterway_array", []) or []:
        a, b = w.get("junction_a"), w.get("junction_b")
        if not a or not b:
            continue
        wname = w.get("name", "")
        pf = pf_on_waterway.get(wname, 0.0)
        G.add_edge(
            a,
            b,
            kind="turbine" if pf > 0 else "waterway",
            name=wname,
            production_factor=pf,
        )

    # plexos2gtopt-style direct-arc turbines (own junction_a /
    # junction_b, no separate Waterway).  Skip turbines already
    # accounted for via the Waterway-name decoration above.
    for t in system.get("turbine_array", []) or []:
        a, b = t.get("junction_a"), t.get("junction_b")
        if not a or not b:
            continue
        if isinstance(t.get("waterway"), str) and t.get("waterway"):
            continue
        G.add_edge(
            a,
            b,
            kind="turbine",
            name=t.get("name", ""),
            production_factor=float(t.get("production_factor", 0.0) or 0.0),
        )
    return G


def epf_per_reservoir(
    system: dict[str, Any],
    *,
    graph: nx.DiGraph | None = None,
) -> dict[str, float]:
    """Return ``{reservoir_name: EPF}`` where EPF is MW per m³/s.

    The walk from each reservoir's anchor junction:
      * adds the ``production_factor`` of every outgoing edge as we
        descend (turbines contribute; waterways contribute 0),
      * STOPS descending when the next node anchors a *different*
        reservoir (that reservoir owns the rest of the cascade), AND
      * stops at sinks (no outgoing edges).
    """
    if graph is None:
        graph = build_hydro_graph(system)

    res_junctions: dict[str, str] = {
        r["name"]: r.get("junction", r["name"])
        for r in (system.get("reservoir_array", []) or [])
    }
    # junction → owning reservoir (1:1 in CEN; first wins on ties).
    junction_owner: dict[str, str] = {}
    for res_name, junc in res_junctions.items():
        junction_owner.setdefault(junc, res_name)

    out: dict[str, float] = {}
    for res_name, anchor in res_junctions.items():
        if anchor not in graph:
            out[res_name] = 0.0
            continue
        epf = 0.0
        visited = {anchor}
        queue: list[str] = [anchor]
        while queue:
            u = queue.pop(0)
            for v, edata in graph[u].items():
                if v in visited:
                    continue
                epf += edata.get("production_factor", 0.0)
                visited.add(v)
                owner = junction_owner.get(v)
                if owner is not None and owner != res_name:
                    # Reached a different reservoir's anchor — stop.
                    continue
                queue.append(v)
        out[res_name] = epf
    return out


def water_emission_value_per_cumec_hour(
    epf: float,
    *,
    gas_em: float = _GAS_EMISSION_TCO2_PER_MWH,
    loss_mult: float = _LOSS_MULTIPLIER,
) -> float:
    """Carbon shadow price of stored water [tCO2eq per (m³/s)·h].

    By assumption (long-term marginal replacement = gas):
        value = EPF · gas_em · loss_mult
              = (MW / (m³/s)) · (tCO2eq / MWh) · (dimensionless)
              = tCO2eq / (m³/s · h)

    Consumed by the emissions-mode terminal-value stamping (issue #519)
    on each reservoir, replacing the dollar-valued boundary cuts /
    water_value the cost-mode LP uses.
    """
    return float(epf) * float(gas_em) * float(loss_mult)


__all__ = [
    "build_hydro_graph",
    "epf_per_reservoir",
    "water_emission_value_per_cumec_hour",
    "_GAS_EMISSION_TCO2_PER_MWH",
    "_LOSS_MULTIPLIER",
]
