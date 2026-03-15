# SPDX-License-Identifier: BSD-3-Clause
"""System / simulation statistics for gtopt_check_json.

Replicates the C++ ``log_pre_solve_stats`` output so the gtopt binary can
delegate statistics printing to this script.
"""

from typing import Any


def format_info(planning: dict[str, Any]) -> str:
    """Return a multi-line string with system, simulation, and option stats.

    The output matches the format previously produced by the C++
    ``log_pre_solve_stats`` function in ``source/gtopt_main.cpp``.
    """
    sys = planning.get("system", {})
    sim = planning.get("simulation", {})
    opts = planning.get("options", {})

    lines: list[str] = []

    lines.append("=== System statistics ===")
    lines.append(f"  System name     : {sys.get('name', '(unnamed)')}")
    lines.append(f"  System version  : {sys.get('version', '')}")

    lines.append("=== System elements  ===")
    lines.append(f"  Buses           : {len(sys.get('bus_array', []))}")
    lines.append(
        f"  Generators      : {len(sys.get('generator_array', []))}"
    )
    lines.append(
        f"  Generator profs : "
        f"{len(sys.get('generator_profile_array', []))}"
    )
    lines.append(f"  Demands         : {len(sys.get('demand_array', []))}")
    lines.append(
        f"  Demand profs    : {len(sys.get('demand_profile_array', []))}"
    )
    lines.append(f"  Lines           : {len(sys.get('line_array', []))}")
    lines.append(
        f"  Batteries       : {len(sys.get('battery_array', []))}"
    )
    lines.append(
        f"  Converters      : {len(sys.get('converter_array', []))}"
    )
    lines.append(
        f"  Reserve zones   : {len(sys.get('reserve_zone_array', []))}"
    )
    lines.append(
        f"  Reserve provisions   : "
        f"{len(sys.get('reserve_provision_array', []))}"
    )
    lines.append(
        f"  Junctions       : {len(sys.get('junction_array', []))}"
    )
    lines.append(
        f"  Waterways       : {len(sys.get('waterway_array', []))}"
    )
    lines.append(f"  Flows           : {len(sys.get('flow_array', []))}")
    lines.append(
        f"  Reservoirs      : {len(sys.get('reservoir_array', []))}"
    )
    lines.append(
        f"  Filtrations     : {len(sys.get('filtration_array', []))}"
    )
    lines.append(
        f"  Turbines        : {len(sys.get('turbine_array', []))}"
    )

    lines.append("=== Simulation statistics ===")
    lines.append(
        f"  Blocks          : {len(sim.get('block_array', []))}"
    )
    lines.append(
        f"  Stages          : {len(sim.get('stage_array', []))}"
    )
    lines.append(
        f"  Scenarios       : {len(sim.get('scenario_array', []))}"
    )

    lines.append("=== Key options ===")
    use_kirch = opts.get("use_kirchhoff", False)
    lines.append(
        f"  use_kirchhoff   : {'true' if use_kirch else 'false'}"
    )
    use_sb = opts.get("use_single_bus", False)
    lines.append(
        f"  use_single_bus  : {'true' if use_sb else 'false'}"
    )
    lines.append(
        f"  scale_objective : {opts.get('scale_objective', 1000.0)}"
    )
    lines.append(
        f"  demand_fail_cost: {opts.get('demand_fail_cost', 0.0)}"
    )
    lines.append(
        f"  input_directory : {opts.get('input_directory', '(default)')}"
    )
    lines.append(
        f"  output_directory: {opts.get('output_directory', '(default)')}"
    )
    lines.append(
        f"  output_format   : {opts.get('output_format', 'csv')}"
    )

    return "\n".join(lines)
