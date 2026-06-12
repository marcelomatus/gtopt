# SPDX-License-Identifier: BSD-3-Clause
"""Topology loader — accepts either a CEN-format JSON snapshot or a
gtopt planning JSON (fallback for users who have one).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Optional

from gtopt_canonical_feed import Bus, Generator, Line, Topology
from cen2gtopt._normalize import stable_uid


def load_topology(path: Path) -> Topology:
    """Load a topology snapshot.

    Recognises two formats:
      1. A gtopt planning JSON (with `system.bus_array`, etc.) —
         delegated to gtopt_marginal_units._gtopt_reader.
      2. A CEN-shaped JSON: ``{"buses": [...], "generators": [...],
         "lines": [...]}``. UIDs may be missing — we hash names
         deterministically via ``stable_uid``.
    """
    p = Path(path)
    data = json.loads(p.read_text(encoding="utf-8"))

    if "system" in data:
        # gtopt planning JSON.
        from gtopt_marginal_units._gtopt_reader import topology_from_planning  # noqa: PLC0415

        return topology_from_planning(data)

    return _load_cen_format(data)


def _load_cen_format(data: dict) -> Topology:
    buses_raw = data.get("buses") or data.get("barras") or []
    gens_raw = data.get("generators") or data.get("centrales") or []
    lines_raw = data.get("lines") or data.get("lineas") or []

    buses = []
    for b in buses_raw:
        name = str(b.get("name") or b.get("nombre"))
        uid = int(b.get("uid", 0)) if b.get("uid") is not None else stable_uid(name)
        buses.append(Bus(uid=uid, name=name, region=b.get("region")))
    bus_name_to_uid = {b.name: b.uid for b in buses}

    generators = []
    for g in gens_raw:
        name = str(g.get("name") or g.get("nombre"))
        uid = int(g.get("uid", 0)) if g.get("uid") is not None else stable_uid(name)
        bus_ref = g.get("bus") or g.get("barra")
        bus_uid = bus_name_to_uid.get(str(bus_ref), 0)
        generators.append(
            Generator(
                uid=uid,
                name=name,
                bus_uid=int(bus_uid),
                pmin=float(g.get("pmin", 0.0)),
                pmax=float(g.get("pmax", g.get("capacity", 0.0))),
                declared_MC=_opt_float(g.get("declared_MC") or g.get("costo_variable")),
                kind=str(g.get("kind", g.get("tecnologia", "thermal"))).lower(),
                emission_rate=_opt_float(g.get("emission_rate")),
            )
        )

    lines = []
    for ln in lines_raw:
        name = str(ln.get("name") or ln.get("nombre", f"l{ln.get('uid', 0)}"))
        uid = int(ln.get("uid", 0)) if ln.get("uid") is not None else stable_uid(name)
        lines.append(
            Line(
                uid=uid,
                bus_a_uid=int(
                    bus_name_to_uid.get(str(ln.get("bus_a") or ln.get("barra_a")), 0)
                ),
                bus_b_uid=int(
                    bus_name_to_uid.get(str(ln.get("bus_b") or ln.get("barra_b")), 0)
                ),
                tmax_ab=float(ln.get("tmax_ab", ln.get("tmax", 0.0))),
                tmax_ba=float(ln.get("tmax_ba", ln.get("tmax", 0.0))),
                reactance=_opt_float(ln.get("reactance") or ln.get("x")),
                active=bool(ln.get("active", True)),
            )
        )

    return Topology(buses=buses, generators=generators, lines=lines)


def _opt_float(v: object) -> Optional[float]:
    if v is None:
        return None
    try:
        return float(v)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return None
