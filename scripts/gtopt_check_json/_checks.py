# SPDX-License-Identifier: BSD-3-Clause
"""Library of validation checks for gtopt JSON planning files.

Each public ``check_*`` function accepts a *planning* dict and returns a list
of :class:`Finding` objects.  The :func:`run_all_checks` function orchestrates
every check and returns a combined list.
"""

from collections import defaultdict
from dataclasses import dataclass
from enum import Enum
from typing import Any


# ---------------------------------------------------------------------------
# Finding data model
# ---------------------------------------------------------------------------


class Severity(Enum):
    """Severity levels for check findings."""

    CRITICAL = "CRITICAL"
    WARNING = "WARNING"
    NOTE = "NOTE"


@dataclass
class Finding:
    """A single check finding."""

    check_id: str
    severity: Severity
    message: str


# ---------------------------------------------------------------------------
# Element class helpers
# ---------------------------------------------------------------------------

# Maps each array name to a short human-readable label.
_ELEMENT_ARRAYS: dict[str, str] = {
    "bus_array": "Bus",
    "generator_array": "Generator",
    "demand_array": "Demand",
    "line_array": "Line",
    "battery_array": "Battery",
    "converter_array": "Converter",
    "reserve_zone_array": "ReserveZone",
    "reserve_provision_array": "ReserveProvision",
    "junction_array": "Junction",
    "waterway_array": "Waterway",
    "flow_array": "Flow",
    "reservoir_array": "Reservoir",
    "reservoir_seepage_array": "ReservoirSeepage",
    "reservoir_discharge_limit_array": "ReservoirDischargeLimit",
    "reservoir_production_factor_array": "ReservoirProductionFactor",
    "turbine_array": "Turbine",
    "generator_profile_array": "GeneratorProfile",
    "demand_profile_array": "DemandProfile",
}


def _get_uid_set(sys: dict[str, Any], array_key: str) -> dict[Any, list[int]]:
    """Return a mapping of uid → list of indices for an element array."""
    result: dict[Any, list[int]] = defaultdict(list)
    for idx, elem in enumerate(sys.get(array_key, [])):
        uid = elem.get("uid")
        if uid is not None:
            result[uid].append(idx)
    return result


def _get_name_set(sys: dict[str, Any], array_key: str) -> dict[str, list[int]]:
    """Return a mapping of name → list of indices for an element array."""
    result: dict[str, list[int]] = defaultdict(list)
    for idx, elem in enumerate(sys.get(array_key, [])):
        name = elem.get("name")
        if name:
            result[name].append(idx)
    return result


def _resolve_bus_lookup(
    sys: dict[str, Any],
) -> tuple[set[Any], dict[str, Any]]:
    """Return (uid_set, name_to_uid) for buses."""
    uid_set: set[Any] = set()
    name_to_uid: dict[str, Any] = {}
    for bus in sys.get("bus_array", []):
        uid = bus.get("uid")
        name = bus.get("name", "")
        if uid is not None:
            uid_set.add(uid)
        if name:
            name_to_uid[name] = uid
    return uid_set, name_to_uid


def _resolve_bus_ref(
    ref: Any,
    bus_uids: set[Any],
    bus_name_to_uid: dict[str, Any],
) -> bool:
    """Return True if *ref* resolves to an existing bus (by uid or name)."""
    if ref is None:
        return True  # optional reference
    if ref in bus_uids:
        return True
    if isinstance(ref, str) and ref in bus_name_to_uid:
        return True
    return False


def _resolve_uid_ref(
    ref: Any,
    uid_set: set[Any],
    name_map: dict[str, Any],
) -> bool:
    """Return True if *ref* resolves to an existing element."""
    if ref is None:
        return True
    if ref in uid_set:
        return True
    if isinstance(ref, str) and ref in name_map:
        return True
    return False


def _build_uid_name_maps(
    sys: dict[str, Any], array_key: str
) -> tuple[set[Any], dict[str, Any]]:
    """Return (uid_set, name_to_uid) for an element array."""
    uid_set: set[Any] = set()
    name_map: dict[str, Any] = {}
    for elem in sys.get(array_key, []):
        uid = elem.get("uid")
        name = elem.get("name", "")
        if uid is not None:
            uid_set.add(uid)
        if name:
            name_map[name] = uid
    return uid_set, name_map


def _extract_scalar_values(field: Any) -> list[float]:
    """Extract numeric values from a FieldSched-style field.

    Handles: scalar, list of scalars, nested lists, or returns []
    for string (file reference) or None.
    """
    if field is None:
        return []
    if isinstance(field, (int, float)):
        return [float(field)]
    if isinstance(field, str):
        return []  # file reference
    if isinstance(field, list):
        values: list[float] = []
        for item in field:
            if isinstance(item, (int, float)):
                values.append(float(item))
            elif isinstance(item, list):
                for sub in item:
                    if isinstance(sub, (int, float)):
                        values.append(float(sub))
        return values
    return []


# ---------------------------------------------------------------------------
# Individual checks
# ---------------------------------------------------------------------------


def check_uid_uniqueness(planning: dict[str, Any]) -> list[Finding]:
    """Check that all UIDs are unique within each element class."""
    findings: list[Finding] = []
    sys = planning.get("system", {})
    sim = planning.get("simulation", {})

    # System elements
    for array_key, label in _ELEMENT_ARRAYS.items():
        uid_map = _get_uid_set(sys, array_key)
        for uid, indices in uid_map.items():
            if len(indices) > 1:
                findings.append(
                    Finding(
                        check_id="uid_uniqueness",
                        severity=Severity.CRITICAL,
                        message=(f"{label}: duplicate uid={uid} at indices {indices}"),
                    )
                )

    # Simulation elements
    for array_key, label in [
        ("block_array", "Block"),
        ("stage_array", "Stage"),
        ("scenario_array", "Scenario"),
    ]:
        uid_map = _get_uid_set(sim, array_key)
        for uid, indices in uid_map.items():
            if len(indices) > 1:
                findings.append(
                    Finding(
                        check_id="uid_uniqueness",
                        severity=Severity.CRITICAL,
                        message=(f"{label}: duplicate uid={uid} at indices {indices}"),
                    )
                )

    return findings


def check_name_uniqueness(planning: dict[str, Any]) -> list[Finding]:
    """Check that all names are unique within each element class."""
    findings: list[Finding] = []
    sys = planning.get("system", {})

    for array_key, label in _ELEMENT_ARRAYS.items():
        name_map = _get_name_set(sys, array_key)
        for name, indices in name_map.items():
            if len(indices) > 1:
                findings.append(
                    Finding(
                        check_id="name_uniqueness",
                        severity=Severity.CRITICAL,
                        message=(
                            f"{label}: duplicate name='{name}' at indices {indices}"
                        ),
                    )
                )

    return findings


def check_demand_lmax_nonneg(planning: dict[str, Any]) -> list[Finding]:
    """Check that all Demand lmax values are non-negative."""
    findings: list[Finding] = []
    sys = planning.get("system", {})

    for idx, demand in enumerate(sys.get("demand_array", [])):
        lmax = demand.get("lmax")
        values = _extract_scalar_values(lmax)
        neg_vals = [v for v in values if v < 0]
        if neg_vals:
            name = demand.get("name", f"index {idx}")
            findings.append(
                Finding(
                    check_id="demand_lmax_nonneg",
                    severity=Severity.WARNING,
                    message=(
                        f"Demand '{name}' (uid={demand.get('uid')}): "
                        f"negative lmax value(s): {neg_vals}"
                    ),
                )
            )

    return findings


def check_affluent_nonneg(planning: dict[str, Any]) -> list[Finding]:
    """Check that all Flow affluent values are non-negative."""
    findings: list[Finding] = []
    sys = planning.get("system", {})

    for idx, flow in enumerate(sys.get("flow_array", [])):
        affluent = flow.get("affluent")
        values = _extract_scalar_values(affluent)
        neg_vals = [v for v in values if v < 0]
        if neg_vals:
            name = flow.get("name", f"index {idx}")
            findings.append(
                Finding(
                    check_id="affluent_nonneg",
                    severity=Severity.WARNING,
                    message=(
                        f"Flow '{name}' (uid={flow.get('uid')}): "
                        f"negative affluent value(s): {neg_vals}"
                    ),
                )
            )

    return findings


def check_element_references(
    planning: dict[str, Any],
) -> list[Finding]:
    """Check that all inter-element references point to existing elements."""
    findings: list[Finding] = []
    sys = planning.get("system", {})

    bus_uids, bus_names = _resolve_bus_lookup(sys)
    gen_uids, gen_names = _build_uid_name_maps(sys, "generator_array")
    dem_uids, dem_names = _build_uid_name_maps(sys, "demand_array")
    bat_uids, bat_names = _build_uid_name_maps(sys, "battery_array")
    junc_uids, junc_names = _build_uid_name_maps(sys, "junction_array")
    ww_uids, ww_names = _build_uid_name_maps(sys, "waterway_array")
    res_uids, res_names = _build_uid_name_maps(sys, "reservoir_array")
    rz_uids, rz_names = _build_uid_name_maps(sys, "reserve_zone_array")

    def _check_ref(
        elem_label: str,
        elem_name: str,
        field_name: str,
        ref: Any,
        target_uids: set[Any],
        target_names: dict[str, Any],
    ) -> None:
        if ref is not None and not _resolve_uid_ref(ref, target_uids, target_names):
            findings.append(
                Finding(
                    check_id="element_references",
                    severity=Severity.CRITICAL,
                    message=(
                        f"{elem_label} '{elem_name}': "
                        f"{field_name}={ref!r} does not match "
                        f"any existing element"
                    ),
                )
            )

    # Generator → Bus
    for gen in sys.get("generator_array", []):
        name = gen.get("name", str(gen.get("uid", "?")))
        _check_ref(
            "Generator",
            name,
            "bus",
            gen.get("bus"),
            bus_uids,
            bus_names,
        )

    # Demand → Bus
    for dem in sys.get("demand_array", []):
        name = dem.get("name", str(dem.get("uid", "?")))
        _check_ref(
            "Demand",
            name,
            "bus",
            dem.get("bus"),
            bus_uids,
            bus_names,
        )

    # Line → Bus (bus_a, bus_b)
    for line in sys.get("line_array", []):
        name = line.get("name", str(line.get("uid", "?")))
        _check_ref(
            "Line",
            name,
            "bus_a",
            line.get("bus_a"),
            bus_uids,
            bus_names,
        )
        _check_ref(
            "Line",
            name,
            "bus_b",
            line.get("bus_b"),
            bus_uids,
            bus_names,
        )

    # Battery → Bus (optional)
    for bat in sys.get("battery_array", []):
        name = bat.get("name", str(bat.get("uid", "?")))
        bus_ref = bat.get("bus")
        if bus_ref is not None:
            _check_ref(
                "Battery",
                name,
                "bus",
                bus_ref,
                bus_uids,
                bus_names,
            )
        gen_ref = bat.get("source_generator")
        if gen_ref is not None:
            _check_ref(
                "Battery",
                name,
                "source_generator",
                gen_ref,
                gen_uids,
                gen_names,
            )

    # Converter → Battery, Generator, Demand
    for conv in sys.get("converter_array", []):
        name = conv.get("name", str(conv.get("uid", "?")))
        _check_ref(
            "Converter",
            name,
            "battery",
            conv.get("battery"),
            bat_uids,
            bat_names,
        )
        _check_ref(
            "Converter",
            name,
            "generator",
            conv.get("generator"),
            gen_uids,
            gen_names,
        )
        _check_ref(
            "Converter",
            name,
            "demand",
            conv.get("demand"),
            dem_uids,
            dem_names,
        )

    # ReserveProvision → Generator, ReserveZone
    for rp in sys.get("reserve_provision_array", []):
        name = rp.get("name", str(rp.get("uid", "?")))
        _check_ref(
            "ReserveProvision",
            name,
            "generator",
            rp.get("generator"),
            gen_uids,
            gen_names,
        )
        _check_ref(
            "ReserveProvision",
            name,
            "reserve_zone",
            rp.get("reserve_zone"),
            rz_uids,
            rz_names,
        )

    # GeneratorProfile → Generator
    for gp in sys.get("generator_profile_array", []):
        name = gp.get("name", str(gp.get("uid", "?")))
        _check_ref(
            "GeneratorProfile",
            name,
            "generator",
            gp.get("generator"),
            gen_uids,
            gen_names,
        )

    # DemandProfile → Demand
    for dp in sys.get("demand_profile_array", []):
        name = dp.get("name", str(dp.get("uid", "?")))
        _check_ref(
            "DemandProfile",
            name,
            "demand",
            dp.get("demand"),
            dem_uids,
            dem_names,
        )

    # Turbine → Waterway, Generator
    for turb in sys.get("turbine_array", []):
        name = turb.get("name", str(turb.get("uid", "?")))
        _check_ref(
            "Turbine",
            name,
            "waterway",
            turb.get("waterway"),
            ww_uids,
            ww_names,
        )
        _check_ref(
            "Turbine",
            name,
            "generator",
            turb.get("generator"),
            gen_uids,
            gen_names,
        )

    # Waterway → Junction (junction_a, junction_b)
    for ww in sys.get("waterway_array", []):
        name = ww.get("name", str(ww.get("uid", "?")))
        _check_ref(
            "Waterway",
            name,
            "junction_a",
            ww.get("junction_a"),
            junc_uids,
            junc_names,
        )
        _check_ref(
            "Waterway",
            name,
            "junction_b",
            ww.get("junction_b"),
            junc_uids,
            junc_names,
        )

    # Flow → Junction
    for flow in sys.get("flow_array", []):
        name = flow.get("name", str(flow.get("uid", "?")))
        _check_ref(
            "Flow",
            name,
            "junction",
            flow.get("junction"),
            junc_uids,
            junc_names,
        )

    # Reservoir → Junction
    for res in sys.get("reservoir_array", []):
        name = res.get("name", str(res.get("uid", "?")))
        _check_ref(
            "Reservoir",
            name,
            "junction",
            res.get("junction"),
            junc_uids,
            junc_names,
        )

    # ReservoirSeepage → Waterway, Reservoir
    for filt in sys.get("reservoir_seepage_array", []):
        name = filt.get("name", str(filt.get("uid", "?")))
        _check_ref(
            "ReservoirSeepage",
            name,
            "waterway",
            filt.get("waterway"),
            ww_uids,
            ww_names,
        )
        _check_ref(
            "ReservoirSeepage",
            name,
            "reservoir",
            filt.get("reservoir"),
            res_uids,
            res_names,
        )

    # ReservoirDischargeLimit → Waterway, Reservoir
    for ddl in sys.get("reservoir_discharge_limit_array", []):
        name = ddl.get("name", str(ddl.get("uid", "?")))
        _check_ref(
            "ReservoirDischargeLimit",
            name,
            "waterway",
            ddl.get("waterway"),
            ww_uids,
            ww_names,
        )
        _check_ref(
            "ReservoirDischargeLimit",
            name,
            "reservoir",
            ddl.get("reservoir"),
            res_uids,
            res_names,
        )

    return findings


def check_bus_connectivity(planning: dict[str, Any]) -> list[Finding]:
    """Check that all electrical buses form a single connected component."""
    findings: list[Finding] = []
    sys = planning.get("system", {})

    buses = sys.get("bus_array", [])
    if len(buses) <= 1:
        return findings

    # Build adjacency using bus name/uid as keys
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
        """Resolve a bus reference to the canonical bus id."""
        if ref is None:
            return None
        if isinstance(ref, str) and ref in adjacency:
            return ref
        if ref in bus_uid_to_name:
            name = bus_uid_to_name[ref]
            if name in adjacency:
                return name
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

    if len(components) > 1:
        for i, comp in enumerate(components):
            findings.append(
                Finding(
                    check_id="bus_connectivity",
                    severity=Severity.WARNING,
                    message=(
                        f"Island {i + 1}: buses {comp} are "
                        f"disconnected from the rest of the network"
                    ),
                )
            )

    return findings


def check_unreferenced_elements(
    planning: dict[str, Any],
) -> list[Finding]:
    """Check that all elements are referenced or connected to the system."""
    findings: list[Finding] = []
    sys = planning.get("system", {})

    # Collect all bus names/uids that are referenced
    referenced_buses: set[Any] = set()

    for gen in sys.get("generator_array", []):
        bus = gen.get("bus")
        if bus is not None:
            referenced_buses.add(bus)

    for dem in sys.get("demand_array", []):
        bus = dem.get("bus")
        if bus is not None:
            referenced_buses.add(bus)

    for line in sys.get("line_array", []):
        for key in ("bus_a", "bus_b"):
            bus = line.get(key)
            if bus is not None:
                referenced_buses.add(bus)

    for bat in sys.get("battery_array", []):
        bus = bat.get("bus")
        if bus is not None:
            referenced_buses.add(bus)

    # Check each bus is referenced
    for bus in sys.get("bus_array", []):
        uid = bus.get("uid")
        name = bus.get("name", "")
        if uid not in referenced_buses and name not in referenced_buses:
            label = name if name else str(uid)
            findings.append(
                Finding(
                    check_id="unreferenced_elements",
                    severity=Severity.WARNING,
                    message=(
                        f"Bus '{label}' (uid={uid}) is not "
                        f"referenced by any generator, demand, "
                        f"line, or battery"
                    ),
                )
            )

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


# ---------------------------------------------------------------------------
# Battery efficiency
# ---------------------------------------------------------------------------


def check_battery_efficiency(planning: dict[str, Any]) -> list[Finding]:
    """Check that battery efficiencies are at most 1.0 (100 %).

    Both ``input_efficiency`` (charging) and ``output_efficiency``
    (discharging) must be in the range [0, 1].  Values above 1.0
    indicate a data entry error (energy would be created from nothing).
    """
    findings: list[Finding] = []
    sys = planning.get("system", {})

    for idx, bat in enumerate(sys.get("battery_array", [])):
        name = bat.get("name", f"index {idx}")
        uid = bat.get("uid")
        for field in ("input_efficiency", "output_efficiency"):
            values = _extract_scalar_values(bat.get(field))
            bad = [v for v in values if v > 1.0]
            if bad:
                findings.append(
                    Finding(
                        check_id="battery_efficiency",
                        severity=Severity.WARNING,
                        message=(
                            f"Battery '{name}' (uid={uid}): "
                            f"{field} value(s) > 1.0: {bad}"
                        ),
                    )
                )
            neg = [v for v in values if v < 0.0]
            if neg:
                findings.append(
                    Finding(
                        check_id="battery_efficiency",
                        severity=Severity.WARNING,
                        message=(
                            f"Battery '{name}' (uid={uid}): "
                            f"negative {field} value(s): {neg}"
                        ),
                    )
                )

    return findings


# ---------------------------------------------------------------------------
# AI system analysis (optional)
# ---------------------------------------------------------------------------


def check_ai_system_analysis(
    planning: dict[str, Any],
    ai_options: Any = None,
) -> list[Finding]:
    """Use AI to analyze the system for potential issues.

    This check attempts to use gtopt_diagram in mermaid format and
    gtopt2pp for pandapower analysis, then sends the combined report to
    an AI provider for review.

    Returns WARNING-level findings for any AI-detected issues.
    """
    findings: list[Finding] = []

    if ai_options is None:
        return findings

    # Try to collect system information for AI analysis
    report_parts: list[str] = []

    # Try mermaid diagram
    try:
        from gtopt_check_json._diagram_helper import (
            get_mermaid_summary,
        )

        mermaid = get_mermaid_summary(planning)
        if mermaid:
            report_parts.append("=== Network Diagram (Mermaid) ===\n" + mermaid)
    except ImportError:
        report_parts.append(
            "NOTE: gtopt_diagram not available for network visualization"
        )

    # Try pandapower validation
    try:
        from gtopt_check_json._pp_helper import (
            get_pandapower_diagnostics,
        )

        pp_report = get_pandapower_diagnostics(planning)
        if pp_report:
            report_parts.append("=== Pandapower Diagnostics ===\n" + pp_report)
    except ImportError:
        report_parts.append("NOTE: pandapower not available for grid diagnostics")

    if not report_parts:
        return findings

    # Send to AI
    try:
        from gtopt_check_lp._ai import (  # noqa: PLC0415
            _AI_DEFAULT_PROVIDER,
            query_ai,
        )

        combined = "\n\n".join(report_parts)
        prompt = (
            "You are an expert in power system planning and optimization.\n"
            "Analyze this gtopt power system case for potential issues:\n\n"
            f"{combined}\n\n"
            "Report any concerns about:\n"
            "1. Network topology issues\n"
            "2. Generator/demand sizing\n"
            "3. Missing or suspicious parameters\n"
            "4. Potential infeasibility risks\n"
            "Be concise and precise."
        )
        ok, response = query_ai(
            prompt,
            provider=getattr(ai_options, "provider", _AI_DEFAULT_PROVIDER),
            model=getattr(ai_options, "model", None) or None,
            prompt_template="{report}",
            api_key=getattr(ai_options, "key", None) or None,
            timeout=getattr(ai_options, "timeout", 60),
        )
        if ok and response:
            findings.append(
                Finding(
                    check_id="ai_system_analysis",
                    severity=Severity.WARNING,
                    message=f"AI analysis:\n{response}",
                )
            )
        elif not ok:
            findings.append(
                Finding(
                    check_id="ai_system_analysis",
                    severity=Severity.NOTE,
                    message=f"AI analysis unavailable: {response}",
                )
            )
    except ImportError:
        findings.append(
            Finding(
                check_id="ai_system_analysis",
                severity=Severity.NOTE,
                message="AI diagnostics require gtopt_check_lp package",
            )
        )

    return findings


# ---------------------------------------------------------------------------
# Capacity adequacy
# ---------------------------------------------------------------------------


def check_capacity_adequacy(planning: dict[str, Any]) -> list[Finding]:
    """Check that total generation capacity exceeds peak system demand.

    This implements a basic *capacity adequacy* check — a standard
    reliability indicator in generation expansion planning (see NERC
    *Probabilistic Adequacy and Measures* reports, IEA *World Energy
    Outlook* methodology, and Billinton & Allan, *Reliability Evaluation
    of Power Systems*).

    The check computes:

    * **Total generation capacity** — sum of the ``capacity`` (or
      ``pmax``) field of every generator, excluding failure generators
      (``type == "falla"``).
    * **Peak system demand** — maximum of the per-block total demand
      (sum of all ``lmax`` values at each block).

    Findings:

    * ``CRITICAL`` if capacity < peak demand (capacity deficit).
    * ``WARNING`` if capacity < 1.15 × peak demand (thin reserve margin;
      15 % is a common planning threshold).
    * ``NOTE`` otherwise, reporting the computed adequacy ratio.
    """
    from gtopt_check_json._info import (  # noqa: PLC0415
        _CAPACITY_MARGIN_THRESHOLD,
        compute_indicators,
    )

    findings: list[Finding] = []
    ind = compute_indicators(planning)

    if ind.peak_demand_mw <= 0:
        return findings  # no demand → nothing to check

    ratio = ind.capacity_adequacy_ratio

    if ratio < 1.0:
        findings.append(
            Finding(
                check_id="capacity_adequacy",
                severity=Severity.CRITICAL,
                message=(
                    f"Capacity deficit: total generation capacity "
                    f"({ind.total_gen_capacity_mw:,.1f} MW) is less than "
                    f"peak demand ({ind.peak_demand_mw:,.1f} MW). "
                    f"Adequacy ratio = {ratio:.3f}"
                ),
            )
        )
    elif ratio < _CAPACITY_MARGIN_THRESHOLD:
        findings.append(
            Finding(
                check_id="capacity_adequacy",
                severity=Severity.WARNING,
                message=(
                    f"Thin reserve margin: total generation capacity "
                    f"({ind.total_gen_capacity_mw:,.1f} MW) is only "
                    f"{ratio:.1%} of peak demand "
                    f"({ind.peak_demand_mw:,.1f} MW). "
                    f"A margin ≥ 15 % is recommended."
                ),
            )
        )

    return findings


# ---------------------------------------------------------------------------
# Cascade level validation
# ---------------------------------------------------------------------------


def check_cascade_levels(planning: dict[str, Any]) -> list[Finding]:
    """Check cascade level configuration for consistency.

    Validates:
    - Each level has a ``name``.
    - Level 0 should not have a ``transition`` (nothing to inherit from).
    - ``transition.target_penalty`` > 0 when ``inherit_targets`` is true.
    - ``transition.target_rtol`` in (0, 1] when present.
    - ``sddp_options.max_iterations`` >= 0 in each level.
    - ``sddp_options.convergence_tol`` in (0, 1) when present.
    - Warn if both ``inherit_optimality_cuts`` and ``inherit_targets`` are true.
    """
    findings: list[Finding] = []
    opts = planning.get("options", {})
    cascade = opts.get("cascade_options", {})
    levels = cascade.get("level_array", [])

    if not levels:
        return findings

    for idx, level in enumerate(levels):
        label = level.get("name", f"index {idx}")

        # Each level should have a name
        if not level.get("name"):
            findings.append(
                Finding(
                    check_id="cascade_levels",
                    severity=Severity.WARNING,
                    message=(f"Cascade level {idx}: missing 'name' field"),
                )
            )

        # Level 0 should not have a transition
        transition = level.get("transition")
        if idx == 0 and transition is not None:
            findings.append(
                Finding(
                    check_id="cascade_levels",
                    severity=Severity.WARNING,
                    message=(
                        f"Cascade level '{label}' (index 0): "
                        f"has 'transition' but is the first level "
                        f"(nothing to inherit from)"
                    ),
                )
            )

        if transition is not None:
            inherit_targets = transition.get("inherit_targets", False)
            inherit_cuts = transition.get("inherit_optimality_cuts", False)

            # target_penalty should be > 0 when inherit_targets is true
            target_penalty = transition.get("target_penalty")
            if inherit_targets and target_penalty is not None and target_penalty <= 0:
                findings.append(
                    Finding(
                        check_id="cascade_levels",
                        severity=Severity.WARNING,
                        message=(
                            f"Cascade level '{label}': "
                            f"target_penalty={target_penalty} should be "
                            f"> 0 when inherit_targets is true"
                        ),
                    )
                )

            # target_rtol should be in (0, 1] when present
            target_rtol = transition.get("target_rtol")
            if target_rtol is not None and (target_rtol <= 0 or target_rtol > 1):
                findings.append(
                    Finding(
                        check_id="cascade_levels",
                        severity=Severity.CRITICAL,
                        message=(
                            f"Cascade level '{label}': "
                            f"target_rtol={target_rtol} must be in (0, 1]"
                        ),
                    )
                )

            # Warn if both inherit_optimality_cuts and inherit_targets
            if inherit_cuts and inherit_targets:
                findings.append(
                    Finding(
                        check_id="cascade_levels",
                        severity=Severity.NOTE,
                        message=(
                            f"Cascade level '{label}': both "
                            f"inherit_optimality_cuts and inherit_targets "
                            f"are true (unusual but valid)"
                        ),
                    )
                )

        # sddp_options validation within each level
        sddp = level.get("sddp_options")
        if sddp is not None:
            max_iter = sddp.get("max_iterations")
            if max_iter is not None and max_iter < 0:
                findings.append(
                    Finding(
                        check_id="cascade_levels",
                        severity=Severity.CRITICAL,
                        message=(
                            f"Cascade level '{label}': "
                            f"sddp_options.max_iterations={max_iter} "
                            f"must be >= 0"
                        ),
                    )
                )

            conv_tol = sddp.get("convergence_tol")
            if conv_tol is not None and (conv_tol <= 0 or conv_tol >= 1):
                findings.append(
                    Finding(
                        check_id="cascade_levels",
                        severity=Severity.CRITICAL,
                        message=(
                            f"Cascade level '{label}': "
                            f"sddp_options.convergence_tol={conv_tol} "
                            f"must be in (0, 1)"
                        ),
                    )
                )

    return findings


# ---------------------------------------------------------------------------
# Simulation mode checks
# ---------------------------------------------------------------------------


def check_simulation_mode(planning: dict[str, Any]) -> list[Finding]:
    """Check simulation mode configuration for consistency.

    Validates:
    - ``simulation_mode=true`` with ``max_iterations > 0`` is a conflict.
    - ``simulation_mode=true`` with ``save_per_iteration=true`` warns that
      cuts are not saved in simulation mode.
    """
    findings: list[Finding] = []
    opts = planning.get("options", {})
    sddp = opts.get("sddp_options", {})

    sim_mode = sddp.get("simulation_mode", False)
    if not sim_mode:
        return findings

    max_iter = sddp.get("max_iterations")
    if max_iter is not None and max_iter > 0:
        findings.append(
            Finding(
                check_id="simulation_mode",
                severity=Severity.WARNING,
                message=(
                    f"simulation_mode is true but "
                    f"max_iterations={max_iter}; in simulation mode "
                    f"no training iterations are performed"
                ),
            )
        )

    save_per_iter = sddp.get("save_per_iteration")
    if save_per_iter is True:
        findings.append(
            Finding(
                check_id="simulation_mode",
                severity=Severity.WARNING,
                message=(
                    "simulation_mode is true but "
                    "save_per_iteration is true; cuts are not saved "
                    "in simulation mode"
                ),
            )
        )

    return findings


# ---------------------------------------------------------------------------
# General SDDP checks
# ---------------------------------------------------------------------------


def check_sddp_options(planning: dict[str, Any]) -> list[Finding]:
    """Check general SDDP option consistency.

    Validates:
    - ``min_iterations > max_iterations`` is suspicious (max wins).
    - ``max_iterations=0`` without hot-start cuts is an untrained policy.
    - ``convergence_tol`` outside (0, 1) is an error.
    """
    findings: list[Finding] = []
    opts = planning.get("options", {})
    sddp = opts.get("sddp_options", {})

    # Skip if no sddp_options present
    if not sddp:
        return findings

    max_iter = sddp.get("max_iterations")
    min_iter = sddp.get("min_iterations")

    # min_iterations > max_iterations
    if min_iter is not None and max_iter is not None and min_iter > max_iter:
        findings.append(
            Finding(
                check_id="sddp_options",
                severity=Severity.WARNING,
                message=(
                    f"min_iterations ({min_iter}) > "
                    f"max_iterations ({max_iter}); "
                    f"max_iterations takes precedence"
                ),
            )
        )

    # max_iterations=0 without hot-start cuts
    if max_iter is not None and max_iter == 0:
        hot_start = sddp.get("hot_start", False)
        hot_start_mode = sddp.get("hot_start_mode", "none")
        cuts_input = sddp.get("cuts_input_file", "")
        has_cuts = (
            hot_start is True
            or (hot_start_mode and hot_start_mode != "none")
            or bool(cuts_input)
        )
        if not has_cuts:
            findings.append(
                Finding(
                    check_id="sddp_options",
                    severity=Severity.WARNING,
                    message=(
                        "max_iterations=0 without hot-start cuts; "
                        "the policy will be untrained "
                        "(forward-only with no cuts)"
                    ),
                )
            )

    # convergence_tol outside (0, 1)
    conv_tol = sddp.get("convergence_tol")
    if conv_tol is not None and (conv_tol <= 0 or conv_tol >= 1):
        findings.append(
            Finding(
                check_id="sddp_options",
                severity=Severity.CRITICAL,
                message=(f"convergence_tol={conv_tol} must be in (0, 1)"),
            )
        )

    return findings


# ---------------------------------------------------------------------------
# Cascade + solver_type consistency
# ---------------------------------------------------------------------------


def check_cascade_solver_type(planning: dict[str, Any]) -> list[Finding]:
    """Check cascade_options and solver_type consistency.

    Validates:
    - ``cascade_options.level_array`` non-empty but ``solver_type`` is not
      ``"cascade"`` — warn about mismatch.
    - ``solver_type == "cascade"`` but no ``cascade_options.level_array``
      — info note that defaults will be used.
    """
    findings: list[Finding] = []
    opts = planning.get("options", {})

    solver_type = opts.get("solver_type", "")
    cascade = opts.get("cascade_options", {})
    levels = cascade.get("level_array", [])

    if levels and solver_type and solver_type != "cascade":
        findings.append(
            Finding(
                check_id="cascade_solver_type",
                severity=Severity.WARNING,
                message=(
                    f"cascade_options.level_array has {len(levels)} "
                    f"level(s) but solver_type='{solver_type}' "
                    f"(expected 'cascade')"
                ),
            )
        )

    if solver_type == "cascade" and not levels:
        findings.append(
            Finding(
                check_id="cascade_solver_type",
                severity=Severity.NOTE,
                message=(
                    "solver_type='cascade' but "
                    "cascade_options.level_array is empty; "
                    "a single default level will be used"
                ),
            )
        )

    return findings


# ---------------------------------------------------------------------------
# Registry and runner
# ---------------------------------------------------------------------------

# Registry of all checks: (check_id, function, needs_ai)
_CHECK_REGISTRY: list[tuple[str, Any, bool]] = [
    ("uid_uniqueness", check_uid_uniqueness, False),
    ("name_uniqueness", check_name_uniqueness, False),
    ("demand_lmax_nonneg", check_demand_lmax_nonneg, False),
    ("affluent_nonneg", check_affluent_nonneg, False),
    ("element_references", check_element_references, False),
    ("bus_connectivity", check_bus_connectivity, False),
    ("unreferenced_elements", check_unreferenced_elements, False),
    ("capacity_adequacy", check_capacity_adequacy, False),
    ("battery_efficiency", check_battery_efficiency, False),
    ("cascade_levels", check_cascade_levels, False),
    ("simulation_mode", check_simulation_mode, False),
    ("sddp_options", check_sddp_options, False),
    ("cascade_solver_type", check_cascade_solver_type, False),
    ("ai_system_analysis", check_ai_system_analysis, True),
]


def run_all_checks(
    planning: dict[str, Any],
    enabled_checks: set[str] | None = None,
    ai_options: Any = None,
) -> list[Finding]:
    """Run all enabled checks and return combined findings.

    Parameters
    ----------
    planning:
        The merged planning dict (loaded from JSON).
    enabled_checks:
        Set of check IDs to run.  ``None`` means all non-AI checks.
    ai_options:
        AI options for the AI system analysis check.  ``None`` disables it.
    """
    findings: list[Finding] = []

    for check_id, check_fn, needs_ai in _CHECK_REGISTRY:
        if enabled_checks is not None and check_id not in enabled_checks:
            continue
        if needs_ai:
            findings.extend(check_fn(planning, ai_options=ai_options))
        else:
            findings.extend(check_fn(planning))

    return findings
