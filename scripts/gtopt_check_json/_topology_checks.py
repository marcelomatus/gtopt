# SPDX-License-Identifier: BSD-3-Clause
"""Bus connectivity and topology validation checks."""

from collections import defaultdict
from dataclasses import dataclass
from typing import Any

from gtopt_check_json._checks_common import (
    Finding,
    Severity,
)


@dataclass
class _BusAnalysis:
    """Result of bus connectivity analysis."""

    components: list[list[Any]]
    unreferenced: set[Any]
    gen_names: dict[Any, list[str]]
    dem_names: dict[Any, list[str]]
    bat_names: dict[Any, list[str]]
    line_names: dict[Any, list[str]]


def _bus_connectivity_analysis(planning: dict[str, Any]) -> _BusAnalysis:
    """Compute connected components and per-bus element names.

    Returns a :class:`_BusAnalysis` with connected components,
    unreferenced bus set, and per-bus lists of generator, demand,
    and battery names.
    """
    sys = planning.get("system", {})
    buses = sys.get("bus_array", [])

    # Build bus id list
    bus_ids: list[Any] = []
    for bus in buses:
        name = bus.get("name")
        uid = bus.get("uid")
        bus_ids.append(name if name else uid)

    adjacency: dict[Any, set[Any]] = {bid: set() for bid in bus_ids}

    bus_uid_to_name: dict[Any, str] = {}
    for bus in buses:
        uid = bus.get("uid")
        name = bus.get("name", "")
        if uid is not None and name:
            bus_uid_to_name[uid] = name

    def _resolve_to_id(ref: Any) -> Any:
        if ref is None:
            return None
        if isinstance(ref, str) and ref in adjacency:
            return ref
        if ref in bus_uid_to_name:
            resolved = bus_uid_to_name[ref]
            if resolved in adjacency:
                return resolved
        if ref in adjacency:
            return ref
        return None

    for line in sys.get("line_array", []):
        a_id = _resolve_to_id(line.get("bus_a"))
        b_id = _resolve_to_id(line.get("bus_b"))
        if a_id is not None and b_id is not None:
            adjacency[a_id].add(b_id)
            adjacency[b_id].add(a_id)

    # BFS to find connected components
    visited: set[Any] = set()
    components: list[list[Any]] = []

    for start in bus_ids:
        if start in visited:
            continue
        component: list[Any] = []
        queue = [start]
        while queue:
            node = queue.pop(0)
            if node in visited:
                continue
            visited.add(node)
            component.append(node)
            for neighbor in adjacency.get(node, set()):
                if neighbor not in visited:
                    queue.append(neighbor)
        components.append(component)

    # Per-bus element names (resolve references to canonical bus ids)
    gen_names: dict[Any, list[str]] = defaultdict(list)
    dem_names: dict[Any, list[str]] = defaultdict(list)
    bat_names: dict[Any, list[str]] = defaultdict(list)
    line_names: dict[Any, list[str]] = defaultdict(list)

    referenced_buses: set[Any] = set()
    for gen in sys.get("generator_array", []):
        bus = gen.get("bus")
        if bus is not None:
            referenced_buses.add(bus)
            bid = _resolve_to_id(bus)
            if bid is not None:
                gen_names[bid].append(gen.get("name", str(gen.get("uid", "?"))))
    for dem in sys.get("demand_array", []):
        bus = dem.get("bus")
        if bus is not None:
            referenced_buses.add(bus)
            bid = _resolve_to_id(bus)
            if bid is not None:
                dem_names[bid].append(dem.get("name", str(dem.get("uid", "?"))))
    for line in sys.get("line_array", []):
        lname = line.get("name", str(line.get("uid", "?")))
        for key in ("bus_a", "bus_b"):
            bus = line.get(key)
            if bus is not None:
                referenced_buses.add(bus)
                bid = _resolve_to_id(bus)
                if bid is not None:
                    line_names[bid].append(lname)
    for bat in sys.get("battery_array", []):
        bus = bat.get("bus")
        if bus is not None:
            referenced_buses.add(bus)
            bid = _resolve_to_id(bus)
            if bid is not None:
                bat_names[bid].append(bat.get("name", str(bat.get("uid", "?"))))

    # Build unreferenced set using canonical bus ids
    unreferenced: set[Any] = set()
    for bus in buses:
        uid = bus.get("uid")
        name = bus.get("name", "")
        if uid not in referenced_buses and name not in referenced_buses:
            bid = name if name else uid
            unreferenced.add(bid)

    return _BusAnalysis(
        components=components,
        unreferenced=unreferenced,
        gen_names=dict(gen_names),
        dem_names=dict(dem_names),
        bat_names=dict(bat_names),
        line_names=dict(line_names),
    )


@dataclass
class IslandInfo:
    """Summary of a disconnected island for tabular display."""

    island_id: int
    buses: list[Any]
    line_names: list[str]
    generator_names: list[str]
    demand_names: list[str]
    battery_names: list[str]

    @property
    def has_elements(self) -> bool:
        """Return True if the island has any generators, demands, or batteries."""
        return bool(self.generator_names or self.demand_names or self.battery_names)


def analyse_bus_islands(planning: dict[str, Any]) -> list[IslandInfo]:
    """Return per-island metadata for disconnected buses.

    Each :class:`IslandInfo` reports the buses in the island and the
    names of lines, generators, demands, and batteries sitting on it.
    This is purely network-derived information --- no name-based guessing.

    The main grid (largest component) is excluded from the result.
    """
    analysis = _bus_connectivity_analysis(planning)
    if len(analysis.components) <= 1:
        return []

    # Sort components by size descending; first is the main grid
    analysis.components.sort(key=len, reverse=True)

    islands: list[IslandInfo] = []
    for i, comp in enumerate(analysis.components[1:], start=1):
        # Collect unique line names touching this island.
        # By definition, disconnected islands have no lines to the main
        # grid, so every line touching a bus here is internal.
        lines: list[str] = sorted(
            {ln for b in comp for ln in analysis.line_names.get(b, [])}
        )
        gens = [n for b in comp for n in analysis.gen_names.get(b, [])]
        dems = [n for b in comp for n in analysis.dem_names.get(b, [])]
        bats = [n for b in comp for n in analysis.bat_names.get(b, [])]
        islands.append(
            IslandInfo(
                island_id=i,
                buses=comp,
                line_names=lines,
                generator_names=gens,
                demand_names=dems,
                battery_names=bats,
            )
        )

    return islands


def check_bus_connectivity(planning: dict[str, Any]) -> list[Finding]:
    """Check that all electrical buses form a single connected component.

    Emits a compact summary: one finding for islands with elements
    (stranded power) and one for empty islands.
    """
    findings: list[Finding] = []
    sys = planning.get("system", {})

    buses = sys.get("bus_array", [])
    if len(buses) <= 1:
        return findings

    islands = analyse_bus_islands(planning)
    if not islands:
        return findings

    with_elements = [i for i in islands if i.has_elements]
    without_elements = [i for i in islands if not i.has_elements]

    if with_elements:
        all_buses = [b for isl in with_elements for b in isl.buses]
        names = ", ".join(str(b) for b in all_buses[:10])
        suffix = f" … and {len(all_buses) - 10} more" if len(all_buses) > 10 else ""
        findings.append(
            Finding(
                check_id="bus_connectivity",
                severity=Severity.WARNING,
                message=(
                    f"{len(with_elements)} island(s) with elements "
                    f"({len(all_buses)} bus(es)): {names}{suffix}"
                ),
                action="Check line definitions; add missing connections",
            )
        )

    if without_elements:
        all_buses = [b for isl in without_elements for b in isl.buses]
        names = ", ".join(str(b) for b in all_buses[:10])
        suffix = f" … and {len(all_buses) - 10} more" if len(all_buses) > 10 else ""
        findings.append(
            Finding(
                check_id="bus_connectivity",
                severity=Severity.WARNING,
                message=(
                    f"{len(without_elements)} empty island(s) "
                    f"({len(all_buses)} bus(es)): {names}{suffix}"
                ),
                action="Remove unused buses or add connections",
            )
        )

    return findings


def check_unreferenced_elements(
    planning: dict[str, Any],
) -> list[Finding]:
    """Check that all elements are referenced or connected to the system.

    For unreferenced buses, emits a single summary finding instead of
    one per bus.  Other element types are reported individually.
    """
    findings: list[Finding] = []
    sys = planning.get("system", {})

    # Check generators are referenced (by profile or converter)
    gen_referenced: set[Any] = set()
    for gp in sys.get("generator_profile_array", []):
        ref = gp.get("generator")
        if ref is not None:
            gen_referenced.add(ref)
    for conv in sys.get("converter_array", []):
        ref = conv.get("generator")
        if ref is not None:
            gen_referenced.add(ref)
    for turb in sys.get("turbine_array", []):
        ref = turb.get("generator")
        if ref is not None:
            gen_referenced.add(ref)
    for rp in sys.get("reserve_provision_array", []):
        ref = rp.get("generator")
        if ref is not None:
            gen_referenced.add(ref)

    # Generators are always connected via bus — skip if they have a bus.
    # Only warn about generators with no bus AND not referenced.
    for gen in sys.get("generator_array", []):
        bus = gen.get("bus")
        uid = gen.get("uid")
        name = gen.get("name", "")
        if bus is None and uid not in gen_referenced and name not in gen_referenced:
            label = name if name else str(uid)
            findings.append(
                Finding(
                    check_id="unreferenced_elements",
                    severity=Severity.WARNING,
                    message=(
                        f"Generator '{label}' (uid={uid}) has "
                        f"no bus and is not referenced by any "
                        f"profile, converter, turbine, or reserve"
                    ),
                )
            )

    # Check junctions are referenced
    junc_referenced: set[Any] = set()
    for ww in sys.get("waterway_array", []):
        for key in ("junction_a", "junction_b"):
            ref = ww.get(key)
            if ref is not None:
                junc_referenced.add(ref)
    for flow in sys.get("flow_array", []):
        ref = flow.get("junction")
        if ref is not None:
            junc_referenced.add(ref)
    for res in sys.get("reservoir_array", []):
        ref = res.get("junction")
        if ref is not None:
            junc_referenced.add(ref)

    for junc in sys.get("junction_array", []):
        uid = junc.get("uid")
        name = junc.get("name", "")
        if uid not in junc_referenced and name not in junc_referenced:
            label = name if name else str(uid)
            findings.append(
                Finding(
                    check_id="unreferenced_elements",
                    severity=Severity.WARNING,
                    message=(
                        f"Junction '{label}' (uid={uid}) is not "
                        f"referenced by any waterway, flow, or "
                        f"reservoir"
                    ),
                )
            )

    return findings
