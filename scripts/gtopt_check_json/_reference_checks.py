# SPDX-License-Identifier: BSD-3-Clause
"""Element reference validation checks."""

from typing import Any

from gtopt_check_json._checks_common import (
    Finding,
    Severity,
    _build_uid_name_maps,
    _resolve_bus_lookup,
    _resolve_uid_ref,
)


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

    # Generator -> Bus
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

    # Demand -> Bus
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

    # Line -> Bus (bus_a, bus_b)
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

    # Battery -> Bus (optional)
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

    # Converter -> Battery, Generator, Demand
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

    # ReserveProvision -> Generator, ReserveZone
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

    # GeneratorProfile -> Generator
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

    # DemandProfile -> Demand
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

    # Turbine -> Waterway, Generator
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

    # Waterway -> Junction (junction_a, junction_b)
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

    # Flow -> Junction
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

    # Reservoir -> Junction
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

    # ReservoirSeepage -> Waterway, Reservoir
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

    # ReservoirDischargeLimit -> Waterway, Reservoir
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
