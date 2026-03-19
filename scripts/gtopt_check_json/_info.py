# SPDX-License-Identifier: BSD-3-Clause
"""System / simulation statistics and global indicators for gtopt_check_json.

Replicates the C++ ``log_pre_solve_stats`` output so the gtopt binary can
delegate statistics printing to this script.

Also provides :func:`compute_indicators` which derives aggregate power-system
indicators (total generation capacity, first/last block demand and affluent,
capacity adequacy ratio) directly from the in-memory planning dict.  These
indicators are useful for quick sanity checks and for comparing PLP and gtopt
cases.

When a ``base_dir`` is supplied, file-referenced FieldSched values (e.g.
``"lmax"`` pointing to ``Demand/lmax.parquet``) are resolved by reading
the corresponding Parquet/CSV files from disk.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Global indicator data model
# ---------------------------------------------------------------------------

# Type value that marks a PLP failure (modelling artefact) generator.
_FAILURE_GEN_TYPE = "falla"

# Minimum capacity adequacy ratio to avoid a thin-margin warning (15%).
_CAPACITY_MARGIN_THRESHOLD = 1.15


@dataclass
class SystemIndicators:
    """Aggregate power-system indicators derived from a planning dict.

    Attributes
    ----------
    total_gen_capacity_mw : float
        Sum of ``capacity`` (or ``pmax``) across all non-failure generators
        (MW).  A generator is considered a failure generator when its
        ``type`` attribute equals ``"falla"``.
    first_block_demand_mw : float
        Total system demand at the first block (MW).
    last_block_demand_mw : float
        Total system demand at the last block (MW).
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
    first_block_affluent_avg : float
        Average (across scenarios) total discharge at the first block (m³/s).
        Computed from ``Flow.discharge`` fields in ``flow_array``.
    last_block_affluent_avg : float
        Average (across scenarios) total discharge at the last block (m³/s).
        Computed from ``Flow.discharge`` fields in ``flow_array``.
    num_generators : int
        Count of generators used in the capacity sum.
    num_demands : int
        Count of demands used in the demand sum.
    num_blocks : int
        Number of blocks in the simulation.
    num_flows : int
        Number of Flow elements in ``flow_array``.
    """

    total_gen_capacity_mw: float = 0.0
    first_block_demand_mw: float = 0.0
    last_block_demand_mw: float = 0.0
    peak_demand_mw: float = 0.0
    min_demand_mw: float = 0.0
    peak_demand_block: int = -1
    total_demand_by_block: list[float] = field(default_factory=list)
    total_energy_mwh: float = 0.0
    capacity_adequacy_ratio: float = float("inf")
    first_block_affluent_avg: float = 0.0
    last_block_affluent_avg: float = 0.0
    num_generators: int = 0
    num_demands: int = 0
    num_blocks: int = 0
    num_flows: int = 0


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _is_failure_generator(gen: dict[str, Any]) -> bool:
    """Return True if *gen* is a PLP failure (``"falla"``) generator.

    Detection is purely by ``type`` attribute, **not** by pmax magnitude.
    """
    return str(gen.get("type", "")).lower() == _FAILURE_GEN_TYPE


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
        # If the block index exceeds the list, return the last available value
        if block_idx >= len(val) and val and isinstance(val[-1], (int, float)):
            return float(val[-1])
    return None


# ---------------------------------------------------------------------------
# File-reference resolution helpers
# ---------------------------------------------------------------------------


def _resolve_block_totals_from_file(
    base_dir: str,
    input_dir: str,
    class_name: str,
    field_name: str,
    block_uids: list[int],
) -> list[float] | None:
    """Read a FieldSched Parquet/CSV file and return per-block totals.

    Sums all ``uid:*`` value columns at each block position.  When a
    ``scenario`` column is present (e.g. ``Flow/discharge.parquet``), the
    values are averaged across scenarios first.

    Parameters
    ----------
    base_dir
        Absolute directory where the case JSON lives.
    input_dir
        The ``options.input_directory`` value (often ``"."``).
    class_name
        Element class directory name (e.g. ``"Demand"``, ``"Flow"``).
    field_name
        FieldSched file reference string (e.g. ``"lmax"``, ``"discharge"``).
    block_uids
        Ordered list of block UIDs from the simulation ``block_array``.

    Returns
    -------
    list[float] | None
        Per-block totals (same length as *block_uids*), or ``None`` if
        the file cannot be read.
    """
    try:
        from ._file_reader import build_table_path, try_read_table  # noqa: PLC0415
    except ImportError:
        return None

    root = str(Path(base_dir) / input_dir) if input_dir != "." else base_dir
    table_base = build_table_path(root, class_name, field_name)
    df = try_read_table(table_base)
    if df is None:
        return None

    # Identify value columns (uid:N pattern)
    uid_cols = [c for c in df.columns if c.startswith("uid:")]
    if not uid_cols:
        return None

    # When a scenario column exists, average across scenarios first.
    if "scenario" in df.columns and "block" in df.columns:
        df = df.groupby("block", as_index=False)[uid_cols].mean()

    num_blocks = len(block_uids)
    result = [0.0] * num_blocks

    if "block" not in df.columns:
        # No block column — treat whole file as a single block
        if num_blocks > 0:
            result[0] = float(df[uid_cols].sum().sum())
        return result

    # Build block_uid → position map
    uid_to_idx: dict[int, int] = {}
    for idx, buid in enumerate(block_uids):
        uid_to_idx[buid] = idx

    # Vectorized: filter to valid blocks and compute row totals
    df_filtered = df[df["block"].isin(uid_to_idx)]
    if df_filtered.empty:
        return result

    row_totals = df_filtered[uid_cols].sum(axis=1)
    for blk_uid, total in zip(df_filtered["block"], row_totals):
        pos = uid_to_idx.get(int(blk_uid))
        if pos is not None:
            result[pos] += float(total)

    return result


# ---------------------------------------------------------------------------
# Indicator computation
# ---------------------------------------------------------------------------


def compute_indicators(
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> SystemIndicators:
    """Compute aggregate system indicators from a gtopt planning dict.

    The function extracts generation capacity and demand values directly
    from the in-memory JSON structure.  For generators whose ``pmax`` /
    ``capacity`` is given as a file reference (string), only the
    ``capacity`` scalar is used (which is always present in plp2gtopt
    output).

    Failure generators are identified by ``type == "falla"`` (not by
    pmax magnitude).

    The demand comparison uses first and last block totals rather than
    global peak/min, which gives a clearer picture when comparing PLP
    and gtopt representations of the same case.

    An affluent (hydro inflow) indicator is computed as the sum of all
    ``Flow.discharge`` values at the first and last block, averaged
    across scenarios when the discharge is given per-scenario.

    When *base_dir* is provided, FieldSched string references (e.g.
    ``"lmax"`` → ``Demand/lmax.parquet``) are resolved by reading
    the Parquet/CSV files from disk.  Without *base_dir*, file
    references are silently skipped (demand / affluent values will
    be zero).

    Parameters
    ----------
    planning
        A planning dict as loaded from a gtopt JSON file (or built by
        ``GTOptWriter.to_json``).
    base_dir
        Absolute path to the directory that contains the case JSON file.
        Used to locate FieldSched Parquet/CSV files via
        ``options.input_directory``.  Pass ``None`` to skip file
        resolution (backward-compatible default).

    Returns
    -------
    SystemIndicators
        Aggregate indicators.
    """
    sys_data = planning.get("system", {})
    sim = planning.get("simulation", {})
    opts = planning.get("options", {})
    input_dir = opts.get("input_directory", ".")

    generators = sys_data.get("generator_array", [])
    demands = sys_data.get("demand_array", [])
    flows = sys_data.get("flow_array", [])
    blocks = sim.get("block_array", [])
    num_blocks = len(blocks)
    block_uids = [b.get("uid", i + 1) for i, b in enumerate(blocks)]

    # --- Total generation capacity (MW) ---
    total_gen_cap = 0.0
    num_gen = 0
    for gen in generators:
        if _is_failure_generator(gen):
            continue
        cap = _first_scalar(gen.get("capacity"))
        if cap is None:
            cap = _first_scalar(gen.get("pmax"))
        if cap is not None:
            total_gen_cap += cap
            num_gen += 1

    # --- Total demand per block (MW) ---
    demand_by_block: list[float] = [0.0] * num_blocks if num_blocks > 0 else []
    num_dem = len(demands)

    # Collect unique file references to read each Parquet file at most once.
    _file_demand_done: set[str] = set()

    for dem in demands:
        lmax = dem.get("lmax")
        if lmax is None:
            continue
        if isinstance(lmax, str):
            # File reference — resolve from Parquet/CSV when base_dir given
            if base_dir and lmax not in _file_demand_done:
                _file_demand_done.add(lmax)
                totals = _resolve_block_totals_from_file(
                    base_dir,
                    input_dir,
                    "Demand",
                    lmax,
                    block_uids,
                )
                if totals:
                    for b_idx in range(min(len(totals), len(demand_by_block))):
                        demand_by_block[b_idx] += totals[b_idx]
        else:
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
    first_block_demand = demand_by_block[0] if demand_by_block else 0.0
    last_block_demand = demand_by_block[-1] if demand_by_block else 0.0

    # --- Total energy (MWh) = Σ demand × duration ---
    total_energy = 0.0
    for b_idx in range(min(num_blocks, len(demand_by_block))):
        duration = blocks[b_idx].get("duration", 1.0) if b_idx < num_blocks else 1.0
        total_energy += demand_by_block[b_idx] * duration

    # --- Accumulated affluent (hydro inflow) at first and last block ---
    first_block_afl = 0.0
    last_block_afl = 0.0
    num_flow = len(flows)

    # Collect unique file references for flows
    _file_flow_done: set[str] = set()

    for fl in flows:
        discharge = fl.get("discharge")
        if discharge is None:
            continue
        if isinstance(discharge, str):
            if base_dir and discharge not in _file_flow_done:
                _file_flow_done.add(discharge)
                totals = _resolve_block_totals_from_file(
                    base_dir,
                    input_dir,
                    "Flow",
                    discharge,
                    block_uids,
                )
                if totals and len(totals) > 0:
                    first_block_afl += totals[0]
                    last_block_afl += totals[-1]
            continue
        first_val = _first_scalar(discharge)
        if first_val is not None:
            first_block_afl += first_val
        if isinstance(discharge, list) and discharge:
            last_vals = discharge[-1]
            if isinstance(last_vals, (int, float)):
                last_block_afl += float(last_vals)
            else:
                last_block_afl += first_val or 0.0
        elif isinstance(discharge, (int, float)):
            last_block_afl += float(discharge)

    # --- Capacity adequacy ratio ---
    adequacy = total_gen_cap / peak_demand if peak_demand > 0 else float("inf")

    return SystemIndicators(
        total_gen_capacity_mw=total_gen_cap,
        first_block_demand_mw=first_block_demand,
        last_block_demand_mw=last_block_demand,
        peak_demand_mw=peak_demand,
        min_demand_mw=min_demand,
        peak_demand_block=peak_block,
        total_demand_by_block=demand_by_block,
        total_energy_mwh=total_energy,
        capacity_adequacy_ratio=adequacy,
        first_block_affluent_avg=first_block_afl,
        last_block_affluent_avg=last_block_afl,
        num_generators=num_gen,
        num_demands=num_dem,
        num_blocks=num_blocks if num_blocks > 0 else len(demand_by_block),
        num_flows=num_flow,
    )


def format_indicators(
    planning: dict[str, Any],
    base_dir: str | None = None,
    *,
    colr: bool = False,
) -> str:
    """Return a multi-line string with global system indicators.

    Parameters
    ----------
    planning
        A planning dict as loaded from a gtopt JSON file.
    base_dir
        Optional case directory for resolving FieldSched file references.
    colr
        Whether to include ANSI colour codes in the output.

    Returns
    -------
    str
        Human-readable indicator summary.
    """
    from gtopt_check_json._terminal import (  # noqa: PLC0415
        cc,
        kv_table,
        BOLD,
        CYAN,
        DIM,
    )

    ind = compute_indicators(planning, base_dir=base_dir)

    pairs: list[tuple[str, str]] = [
        ("Total gen capacity", f"{ind.total_gen_capacity_mw:,.1f} MW  ({ind.num_generators} generators)"),
        ("First block demand", f"{ind.first_block_demand_mw:,.1f} MW"),
        ("Last block demand", f"{ind.last_block_demand_mw:,.1f} MW"),
        ("Peak demand", f"{ind.peak_demand_mw:,.1f} MW  (block {ind.peak_demand_block})"),
        ("Min demand", f"{ind.min_demand_mw:,.1f} MW"),
        ("Total energy", f"{ind.total_energy_mwh:,.1f} MWh"),
        ("Capacity adequacy", f"{ind.capacity_adequacy_ratio:.3f}"),
    ]
    if ind.num_flows > 0:
        pairs.append(
            ("First block affluent", f"{ind.first_block_affluent_avg:,.1f} m³/s  ({ind.num_flows} flows)")
        )
        pairs.append(
            ("Last block affluent", f"{ind.last_block_affluent_avg:,.1f} m³/s")
        )

    return kv_table(pairs, colr=colr, title="Global Indicators")


# ---------------------------------------------------------------------------
# format_info (existing)
# ---------------------------------------------------------------------------


def format_info(
    planning: dict[str, Any],
    base_dir: str | None = None,
    *,
    colr: bool = False,
) -> str:
    """Return a multi-line string with system, simulation, and option stats.

    The output is presented as styled tables when ``colr=True``, or as
    plain-text tables suitable for logging.

    Parameters
    ----------
    planning
        A planning dict as loaded from a gtopt JSON file.
    base_dir
        Optional case directory for resolving FieldSched file references.
    colr
        Whether to include ANSI colour codes in the output.
    """
    from gtopt_check_json._terminal import (  # noqa: PLC0415
        build_table,
        kv_table,
    )

    sys_data = planning.get("system", {})
    sim = planning.get("simulation", {})
    opts = planning.get("options", {})

    lines: list[str] = []

    # -- System info --
    sys_pairs: list[tuple[str, str]] = [
        ("System name", sys_data.get("name", "(unnamed)")),
        ("System version", sys_data.get("version", "")),
    ]
    lines.append(kv_table(sys_pairs, colr=colr, title="System"))

    # -- Element counts --
    elem_rows: list[tuple[str, str]] = [
        ("Buses", str(len(sys_data.get("bus_array", [])))),
        ("Generators", str(len(sys_data.get("generator_array", [])))),
        ("Generator profiles", str(len(sys_data.get("generator_profile_array", [])))),
        ("Demands", str(len(sys_data.get("demand_array", [])))),
        ("Demand profiles", str(len(sys_data.get("demand_profile_array", [])))),
        ("Lines", str(len(sys_data.get("line_array", [])))),
        ("Batteries", str(len(sys_data.get("battery_array", [])))),
        ("Converters", str(len(sys_data.get("converter_array", [])))),
        ("Reserve zones", str(len(sys_data.get("reserve_zone_array", [])))),
        ("Reserve provisions", str(len(sys_data.get("reserve_provision_array", [])))),
        ("Junctions", str(len(sys_data.get("junction_array", [])))),
        ("Waterways", str(len(sys_data.get("waterway_array", [])))),
        ("Flows", str(len(sys_data.get("flow_array", [])))),
        ("Reservoirs", str(len(sys_data.get("reservoir_array", [])))),
        ("Filtrations", str(len(sys_data.get("filtration_array", [])))),
        ("Turbines", str(len(sys_data.get("turbine_array", [])))),
    ]
    # Filter out zero-count elements for cleaner output
    elem_rows_filtered = [(k, v) for k, v in elem_rows if v != "0"]
    if not elem_rows_filtered:
        elem_rows_filtered = elem_rows
    lines.append(kv_table(elem_rows_filtered, colr=colr, title="Elements"))

    # -- Simulation --
    sim_pairs: list[tuple[str, str]] = [
        ("Blocks", str(len(sim.get("block_array", [])))),
        ("Stages", str(len(sim.get("stage_array", [])))),
        ("Scenarios", str(len(sim.get("scenario_array", [])))),
    ]
    lines.append(kv_table(sim_pairs, colr=colr, title="Simulation"))

    # -- Key options --
    use_kirch = opts.get("use_kirchhoff", False)
    use_sb = opts.get("use_single_bus", False)
    opt_pairs: list[tuple[str, str]] = [
        ("use_kirchhoff", "true" if use_kirch else "false"),
        ("use_single_bus", "true" if use_sb else "false"),
        ("scale_objective", str(opts.get("scale_objective", 1000.0))),
        ("demand_fail_cost", str(opts.get("demand_fail_cost", 0.0))),
        ("input_directory", str(opts.get("input_directory", "(default)"))),
        ("output_directory", str(opts.get("output_directory", "(default)"))),
        ("output_format", str(opts.get("output_format", "csv"))),
    ]
    lines.append(kv_table(opt_pairs, colr=colr, title="Options"))

    # -- Global indicators --
    lines.append(format_indicators(planning, base_dir=base_dir, colr=colr))

    return "\n".join(lines)
