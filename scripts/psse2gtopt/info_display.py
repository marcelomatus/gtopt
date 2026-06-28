"""``--info`` summary for a PSS/E RAW case.

Prints a compact, human-readable digest of the resolved ``.raw`` —
counts, capacities, demand, and the area breakdown — without performing
a conversion.
"""

from __future__ import annotations

from collections import defaultdict
from pathlib import Path
from typing import Any

from .entities import BUS_TYPE_ISOLATED, BUS_TYPE_PV, BUS_TYPE_SLACK
from .psse2gtopt import resolve_raw_file
from .raw_parser import parse_raw


def display_psse_info(options: dict[str, Any]) -> None:
    """Print a summary of the PSS/E case to stdout.

    Args:
        options: Must contain ``input``; ``raw`` optionally selects one
            file from a directory.

    Raises:
        FileNotFoundError / ValueError: Propagated from
            :func:`psse2gtopt.resolve_raw_file` / ``parse_raw``.
    """
    raw_input = options.get("input")
    if raw_input is None:
        raise ValueError("display_psse_info: 'input' option is required")

    raw_file = resolve_raw_file(Path(raw_input), options.get("raw"))
    case = parse_raw(raw_file)

    live_buses = [b for b in case.buses if not b.is_isolated]
    n_slack = sum(1 for b in case.buses if b.ide == BUS_TYPE_SLACK)
    n_pv = sum(1 for b in case.buses if b.ide == BUS_TYPE_PV)
    n_isolated = sum(1 for b in case.buses if b.ide == BUS_TYPE_ISOLATED)

    gens_on = [g for g in case.gens if g.status == 1 and g.pmax > 0]
    pmax_total = sum(g.pmax for g in gens_on)
    loads_on = [d for d in case.loads if d.status == 1 and d.pl > 0]
    load_total = sum(d.pl for d in loads_on)
    branches_on = [b for b in case.branches if b.status == 1]
    xf2 = sum(1 for t in case.transformers if t.windings == 2 and t.status != 0)
    xf3 = sum(1 for t in case.transformers if t.windings == 3 and t.status != 0)

    load_by_area: dict[int, float] = defaultdict(float)
    bus_area = {b.number: b.area for b in case.buses}
    for d in loads_on:
        load_by_area[bus_area.get(d.bus, 0)] += d.pl

    print(f"PSS/E RAW case: {raw_file}")
    print(
        f"  format revision : {case.case.rev}  (SBASE {case.case.sbase:g} MVA, "
        f"{case.case.base_freq:g} Hz)"
    )
    if case.case.title1:
        print(f"  title           : {case.case.title1}")
    if case.case.title2:
        print(f"                    {case.case.title2}")
    print(
        f"  buses           : {len(case.buses)} total "
        f"({len(live_buses)} in-service, {n_slack} slack, {n_pv} PV, "
        f"{n_isolated} isolated)"
    )
    print(
        f"  generators      : {len(case.gens)} total, {len(gens_on)} in-service, "
        f"Σpmax {pmax_total:,.0f} MW"
    )
    print(
        f"  loads           : {len(case.loads)} total, {len(loads_on)} in-service, "
        f"Σpl {load_total:,.0f} MW"
    )
    print(
        f"  branches        : {len(case.branches)} total, {len(branches_on)} in-service"
    )
    print(f"  transformers    : {xf2} two-winding, {xf3} three-winding (in-service)")
    if load_by_area:
        print("  load by area    :")
        for area in sorted(load_by_area):
            print(f"      area {area:>3}    : {load_by_area[area]:,.0f} MW")
