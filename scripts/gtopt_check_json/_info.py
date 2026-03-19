# SPDX-License-Identifier: BSD-3-Clause
"""System / simulation statistics and global indicators for gtopt_check_json.

Replicates the C++ ``log_pre_solve_stats`` output so the gtopt binary can
delegate statistics printing to this script.

Also provides :func:`compute_indicators` which derives aggregate power-system
indicators (total generation capacity, peak demand, annual energy, capacity
adequacy ratio) directly from the in-memory planning dict.  These indicators
are useful for quick sanity checks and for comparing PLP and gtopt cases.
"""

from dataclasses import dataclass, field
from typing import Any


# ---------------------------------------------------------------------------
# Global indicator data model
# ---------------------------------------------------------------------------


@dataclass
class SystemIndicators:
    """Aggregate power-system indicators derived from a planning dict.

    Attributes
    ----------
    total_gen_capacity_mw : float
        Sum of ``capacity`` (or ``pmax``) across all generators (MW).
    peak_demand_mw : float
        Maximum total system demand across all blocks (MW).
    min_demand_mw : float
        Minimum total system demand across all blocks (MW).
    peak_demand_block : int
        Block index (0-based) at which peak demand occurs.
    total_demand_by_block : list[float]
        Sum of all demands per block (MW).  Length equals ``num_blocks``.
    total_energy_mwh : float
        Total annual energy demand = Σ (demand × duration) across all blocks
        and all demands (MWh).
    capacity_adequacy_ratio : float
        ``total_gen_capacity_mw / peak_demand_mw`` if peak > 0, else ``inf``.
        A ratio < 1.0 signals capacity deficit; > 1.0 signals surplus.
        This is a standard reliability indicator in generation expansion
        planning literature (see e.g. NERC *Probabilistic Adequacy and
        Measures* reports and IEA *World Energy Outlook* methodology).
    num_generators : int
        Count of generators used in the capacity sum.
    num_demands : int
        Count of demands used in the demand sum.
    num_blocks : int
        Number of blocks in the simulation.
    """

    total_gen_capacity_mw: float = 0.0
    peak_demand_mw: float = 0.0
    min_demand_mw: float = 0.0
    peak_demand_block: int = -1
    total_demand_by_block: list[float] = field(default_factory=list)
    total_energy_mwh: float = 0.0
    capacity_adequacy_ratio: float = float("inf")
    num_generators: int = 0
    num_demands: int = 0
    num_blocks: int = 0


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _first_scalar(val: Any) -> float | None:
    """Extract the first numeric value from a FieldSched-style field.

    Returns the scalar for a number, the first element for a list,
    or ``None`` for a string (file reference) or ``None``.
    """
    if val is None:
        return None
    if isinstance(val, (int, float)):
        return float(val)
    if isinstance(val, list) and val and isinstance(val[0], (int, float)):
        return float(val[0])
    return None


def _scalar_at_block(val: Any, block_idx: int) -> float | None:
    """Extract the numeric value for a specific block index.

    Returns the scalar for a number, ``val[block_idx]`` for a list,
    or ``None`` for a string (file reference) or ``None``.
    """
    if val is None:
        return None
    if isinstance(val, (int, float)):
        return float(val)
    if isinstance(val, list):
        if block_idx < len(val) and isinstance(val[block_idx], (int, float)):
            return float(val[block_idx])
        # If the list is shorter, return the last available value
        if val and isinstance(val[-1], (int, float)):
            return float(val[-1])
    return None


# ---------------------------------------------------------------------------
# Indicator computation
# ---------------------------------------------------------------------------


def compute_indicators(planning: dict[str, Any]) -> SystemIndicators:
    """Compute aggregate system indicators from a gtopt planning dict.

    The function extracts generation capacity and demand values directly
    from the in-memory JSON structure.  For generators whose ``pmax`` /
    ``capacity`` is given as a file reference (string), only the
    ``capacity`` scalar is used (which is always present in plp2gtopt
    output).

    Parameters
    ----------
    planning
        A planning dict as loaded from a gtopt JSON file (or built by
        ``GTOptWriter.to_json``).

    Returns
    -------
    SystemIndicators
        Aggregate indicators.
    """
    sys_data = planning.get("system", {})
    sim = planning.get("simulation", {})

    generators = sys_data.get("generator_array", [])
    demands = sys_data.get("demand_array", [])
    blocks = sim.get("block_array", [])
    num_blocks = len(blocks)

    # --- Total generation capacity (MW) ---
    total_gen_cap = 0.0
    num_gen = 0
    for gen in generators:
        cap = _first_scalar(gen.get("capacity"))
        if cap is None:
            cap = _first_scalar(gen.get("pmax"))
        if cap is not None and cap < 9000.0:
            # Exclude failure generators (pmax >= 9000 is a PLP convention)
            total_gen_cap += cap
            num_gen += 1

    # --- Total demand per block (MW) ---
    demand_by_block: list[float] = [0.0] * num_blocks if num_blocks > 0 else []
    num_dem = len(demands)

    for dem in demands:
        lmax = dem.get("lmax")
        if lmax is None:
            continue
        for b_idx in range(num_blocks):
            val = _scalar_at_block(lmax, b_idx)
            if val is not None:
                demand_by_block[b_idx] += val

    # If there are no blocks but demands have scalar lmax, treat as 1 block
    if num_blocks == 0 and demands:
        scalar_sum = 0.0
        for dem in demands:
            s = _first_scalar(dem.get("lmax"))
            if s is not None:
                scalar_sum += s
        if scalar_sum > 0:
            demand_by_block = [scalar_sum]

    peak_demand = max(demand_by_block) if demand_by_block else 0.0
    min_demand = min(demand_by_block) if demand_by_block else 0.0
    peak_block = demand_by_block.index(peak_demand) if demand_by_block else -1

    # --- Total energy (MWh) = Σ demand × duration ---
    total_energy = 0.0
    for b_idx in range(min(num_blocks, len(demand_by_block))):
        duration = blocks[b_idx].get("duration", 1.0) if b_idx < num_blocks else 1.0
        total_energy += demand_by_block[b_idx] * duration

    # --- Capacity adequacy ratio ---
    adequacy = total_gen_cap / peak_demand if peak_demand > 0 else float("inf")

    return SystemIndicators(
        total_gen_capacity_mw=total_gen_cap,
        peak_demand_mw=peak_demand,
        min_demand_mw=min_demand,
        peak_demand_block=peak_block,
        total_demand_by_block=demand_by_block,
        total_energy_mwh=total_energy,
        capacity_adequacy_ratio=adequacy,
        num_generators=num_gen,
        num_demands=num_dem,
        num_blocks=num_blocks if num_blocks > 0 else len(demand_by_block),
    )


def format_indicators(planning: dict[str, Any]) -> str:
    """Return a multi-line string with global system indicators.

    Parameters
    ----------
    planning
        A planning dict as loaded from a gtopt JSON file.

    Returns
    -------
    str
        Human-readable indicator summary.
    """
    ind = compute_indicators(planning)

    lines: list[str] = []
    lines.append("=== Global indicators ===")
    lines.append(
        f"  Total gen capacity  : {ind.total_gen_capacity_mw:,.1f} MW"
        f"  ({ind.num_generators} generators)"
    )
    lines.append(
        f"  Peak demand         : {ind.peak_demand_mw:,.1f} MW"
        f"  (block {ind.peak_demand_block})"
    )
    lines.append(f"  Min demand          : {ind.min_demand_mw:,.1f} MW")
    lines.append(f"  Total energy        : {ind.total_energy_mwh:,.1f} MWh")
    lines.append(f"  Capacity adequacy   : {ind.capacity_adequacy_ratio:.3f}")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# format_info (existing)
# ---------------------------------------------------------------------------


def format_info(planning: dict[str, Any]) -> str:
    """Return a multi-line string with system, simulation, and option stats.

    The output matches the format previously produced by the C++
    ``log_pre_solve_stats`` function in ``source/gtopt_main.cpp``,
    followed by the global system indicators.
    """
    sys_data = planning.get("system", {})
    sim = planning.get("simulation", {})
    opts = planning.get("options", {})

    lines: list[str] = []

    lines.append("=== System statistics ===")
    lines.append(f"  System name     : {sys_data.get('name', '(unnamed)')}")
    lines.append(f"  System version  : {sys_data.get('version', '')}")

    lines.append("=== System elements  ===")
    lines.append(f"  Buses           : {len(sys_data.get('bus_array', []))}")
    lines.append(f"  Generators      : {len(sys_data.get('generator_array', []))}")
    lines.append(
        f"  Generator profs : {len(sys_data.get('generator_profile_array', []))}"
    )
    lines.append(f"  Demands         : {len(sys_data.get('demand_array', []))}")
    lines.append(f"  Demand profs    : {len(sys_data.get('demand_profile_array', []))}")
    lines.append(f"  Lines           : {len(sys_data.get('line_array', []))}")
    lines.append(f"  Batteries       : {len(sys_data.get('battery_array', []))}")
    lines.append(f"  Converters      : {len(sys_data.get('converter_array', []))}")
    lines.append(f"  Reserve zones   : {len(sys_data.get('reserve_zone_array', []))}")
    lines.append(
        f"  Reserve provisions   : {len(sys_data.get('reserve_provision_array', []))}"
    )
    lines.append(f"  Junctions       : {len(sys_data.get('junction_array', []))}")
    lines.append(f"  Waterways       : {len(sys_data.get('waterway_array', []))}")
    lines.append(f"  Flows           : {len(sys_data.get('flow_array', []))}")
    lines.append(f"  Reservoirs      : {len(sys_data.get('reservoir_array', []))}")
    lines.append(f"  Filtrations     : {len(sys_data.get('filtration_array', []))}")
    lines.append(f"  Turbines        : {len(sys_data.get('turbine_array', []))}")

    lines.append("=== Simulation statistics ===")
    lines.append(f"  Blocks          : {len(sim.get('block_array', []))}")
    lines.append(f"  Stages          : {len(sim.get('stage_array', []))}")
    lines.append(f"  Scenarios       : {len(sim.get('scenario_array', []))}")

    lines.append("=== Key options ===")
    use_kirch = opts.get("use_kirchhoff", False)
    lines.append(f"  use_kirchhoff   : {'true' if use_kirch else 'false'}")
    use_sb = opts.get("use_single_bus", False)
    lines.append(f"  use_single_bus  : {'true' if use_sb else 'false'}")
    lines.append(f"  scale_objective : {opts.get('scale_objective', 1000.0)}")
    lines.append(f"  demand_fail_cost: {opts.get('demand_fail_cost', 0.0)}")
    lines.append(f"  input_directory : {opts.get('input_directory', '(default)')}")
    lines.append(f"  output_directory: {opts.get('output_directory', '(default)')}")
    lines.append(f"  output_format   : {opts.get('output_format', 'csv')}")

    # Append global indicators
    lines.append("")
    lines.append(format_indicators(planning))

    return "\n".join(lines)
