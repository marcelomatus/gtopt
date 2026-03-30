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
    hydro_capacity_mw : float
        Sum of capacity for hydro-type generators (embalse, serie, pasada,
        hydro_reservoir, hydro_ror, hydro_small, hydro_pumped).
    thermal_capacity_mw : float
        Sum of capacity for thermal/fossil generators (termica, thermal,
        gas, coal, diesel, nuclear, biomass, geothermal).
    renewable_capacity_mw : float
        Sum of capacity for non-hydro renewable generators (solar, wind,
        csp, renewable).
    total_line_capacity_mw : float
        Sum of ``tmax_ab + tmax_ba`` across all lines (MW).
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
        Total energy demand = Σ (demand × duration) across all blocks
        and all demands (MWh).
    avg_annual_energy_mwh : float
        Average annual energy = total_energy_mwh × 8760 / total_hours.
        Normalises total energy to a single year regardless of the
        number of stages or blocks in the simulation.
    total_hours : float
        Sum of all block durations (hours).  Used to compute
        ``avg_annual_energy_mwh``.
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
    total_water_volume_hm3 : float
        Total water volume across all flows and blocks (Hm³).
        Computed as Σ (flow_m³/s × duration_h × 0.0036) where 0.0036 is
        the conversion factor m³/s → Hm³/h.
    avg_flow_m3s : float
        Duration-weighted average flow per affluent (m³/s).
        ``total_water_volume_hm3 / (total_hours × 0.0036) / num_flows``.
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
    hydro_capacity_mw: float = 0.0
    thermal_capacity_mw: float = 0.0
    renewable_capacity_mw: float = 0.0
    total_line_capacity_mw: float = 0.0
    first_block_demand_mw: float = 0.0
    last_block_demand_mw: float = 0.0
    peak_demand_mw: float = 0.0
    min_demand_mw: float = 0.0
    peak_demand_block: int = -1
    total_demand_by_block: list[float] = field(default_factory=list)
    total_energy_mwh: float = 0.0
    avg_annual_energy_mwh: float = 0.0
    total_hours: float = 0.0
    capacity_adequacy_ratio: float = float("inf")
    first_block_affluent_avg: float = 0.0
    last_block_affluent_avg: float = 0.0
    total_water_volume_hm3: float = 0.0
    avg_flow_m3s: float = 0.0
    avg_fcost: float = 0.0
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


class _BlockTotalsResult:
    """Per-block totals and the number of uid columns that contributed."""

    __slots__ = ("totals", "num_uids")

    def __init__(self, totals: list[float], num_uids: int) -> None:
        self.totals = totals
        self.num_uids = num_uids


def _resolve_block_totals_from_file(
    base_dir: str,
    input_dir: str,
    class_name: str,
    field_name: str,
    block_uids: list[int],
) -> _BlockTotalsResult | None:
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
    _BlockTotalsResult | None
        Per-block totals (same length as *block_uids*) and the count of
        ``uid:*`` columns found, or ``None`` if the file cannot be read.
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
        return _BlockTotalsResult(result, len(uid_cols))

    # Build block_uid → position map
    uid_to_idx: dict[int, int] = {}
    for idx, buid in enumerate(block_uids):
        uid_to_idx[buid] = idx

    # Vectorized: filter to valid blocks and compute row totals
    df_filtered = df[df["block"].isin(uid_to_idx)]
    if df_filtered.empty:
        return _BlockTotalsResult(result, len(uid_cols))

    row_totals = df_filtered[uid_cols].sum(axis=1)
    for blk_uid, total in zip(df_filtered["block"], row_totals):
        pos = uid_to_idx.get(int(blk_uid))
        if pos is not None:
            result[pos] += float(total)

    return _BlockTotalsResult(result, len(uid_cols))


# ---------------------------------------------------------------------------
# Indicator computation
# ---------------------------------------------------------------------------


def _avg_demand_fcost(
    demands: list[dict[str, Any]],
    base_dir: str | None,
    input_dir: str,
) -> float:
    """Compute average demand failure cost.

    Handles three forms of the ``fcost`` field:

    - numeric scalar or list → resolved directly via :func:`_first_scalar`
    - string (FieldSched file reference, e.g. ``"fcost"`` or
      ``"Demand@fcost"``) → resolved per demand from the corresponding
      Parquet/CSV file using :func:`resolve_file_sched_value`.
    """
    fcost_values: list[float] = []

    for d in demands:
        fval = d.get("fcost")
        resolved = _first_scalar(fval)
        if resolved is not None:
            fcost_values.append(resolved)
        elif isinstance(fval, str) and base_dir:
            # FieldSched file reference — resolve per demand
            try:
                from ._file_reader import resolve_file_sched_value  # noqa: PLC0415

                root = (
                    str(Path(base_dir) / input_dir)
                    if input_dir != "."
                    else base_dir
                )
                val = resolve_file_sched_value(
                    root,
                    "Demand",
                    fval,
                    element_uid=d.get("uid"),
                    element_name=d.get("name"),
                    scenario_uid=None,
                    block_uid=None,
                )
                if val is not None:
                    fcost_values.append(val)
            except ImportError:
                pass

    return sum(fcost_values) / len(fcost_values) if fcost_values else 0.0


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
    lines_arr = sys_data.get("line_array", [])
    blocks = sim.get("block_array", [])
    num_blocks = len(blocks)
    block_uids = [b.get("uid", i + 1) for i, b in enumerate(blocks)]

    # --- Total generation capacity (MW) ---
    # Use shared classification from tech_classify
    try:
        from plp2gtopt.tech_classify import (  # noqa: PLC0415
            HYDRO_TYPES,
            RENEWABLE_TYPES,
            THERMAL_TYPES,
            classify_type,
        )
    except ImportError:
        # Fallback if plp2gtopt is not installed
        HYDRO_TYPES = frozenset(
            {
                "embalse",
                "serie",
                "pasada",
                "hydro_reservoir",
                "hydro_ror",
                "hydro_small",
                "hydro_pumped",
            }
        )
        THERMAL_TYPES = frozenset(
            {
                "termica",
                "thermal",
                "gas",
                "coal",
                "diesel",
                "nuclear",
                "biomass",
                "geothermal",
            }
        )
        RENEWABLE_TYPES = frozenset(
            {
                "solar",
                "wind",
                "csp",
                "renewable",
            }
        )

        def classify_type(gtype: str) -> str:
            gtype = gtype.lower()
            if gtype in HYDRO_TYPES:
                return "hydro"
            if gtype in THERMAL_TYPES:
                return "thermal"
            if gtype in RENEWABLE_TYPES:
                return "renewable"
            return "other"

    total_gen_cap = 0.0
    hydro_cap = 0.0
    thermal_cap = 0.0
    renewable_cap = 0.0
    cap_by_type: dict[str, float] = {}
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
            gtype = str(gen.get("type", "")).lower()
            category = classify_type(gtype)
            cap_by_type[gtype] = cap_by_type.get(gtype, 0.0) + cap
            if category == "hydro":
                hydro_cap += cap
            elif category == "thermal":
                thermal_cap += cap
            elif category == "renewable":
                renewable_cap += cap

    # --- Total line capacity (MW) ---
    # Also include battery discharge capacity (pmax_discharge) as generation
    # capacity.  In PLP, batteries are centrals of type "bateria" counted in
    # the generation total.  In gtopt, they live in battery_array and get
    # auto-created auxiliary generators at LP time, so we must add them here.
    batteries = sys_data.get("battery_array", [])
    for bat in batteries:
        pmax_d = _first_scalar(bat.get("pmax_discharge"))
        if pmax_d is not None:
            total_gen_cap += pmax_d
            num_gen += 1

    total_line_cap = 0.0
    for line in lines_arr:
        for key in ("tmax_ab", "tmax_ba"):
            val = _first_scalar(line.get(key))
            if val is not None:
                total_line_cap += val

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
                res = _resolve_block_totals_from_file(
                    base_dir,
                    input_dir,
                    "Demand",
                    lmax,
                    block_uids,
                )
                if res is not None:
                    for b_idx in range(min(len(res.totals), len(demand_by_block))):
                        demand_by_block[b_idx] += res.totals[b_idx]
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
    total_hours = 0.0
    for b_idx in range(min(num_blocks, len(demand_by_block))):
        duration = blocks[b_idx].get("duration", 1.0) if b_idx < num_blocks else 1.0
        total_energy += demand_by_block[b_idx] * duration
        total_hours += duration

    # Avg annual energy: total_energy / (total_hours / 8760)
    _HOURS_PER_YEAR = 8760.0
    avg_annual_energy = (
        total_energy * _HOURS_PER_YEAR / total_hours if total_hours > 0 else 0.0
    )

    # --- Accumulated affluent (hydro inflow) and total water volume ---
    # Conversion factor: 1 m³/s flowing for 1 h = 0.0036 Hm³
    _M3S_TO_HM3_PER_H = 0.0036
    first_block_afl = 0.0
    last_block_afl = 0.0
    total_water_vol_hm3 = 0.0
    # Count only flows that actually contribute discharge data, so that
    # avg_flow_m3s uses the same denominator semantics as the PLP side
    # (which counts only flows with hydrology data > 0).
    num_active_flows = 0

    # Collect unique file references for flows
    _file_flow_done: set[str] = set()

    for fl in flows:
        discharge = fl.get("discharge")
        if discharge is None:
            continue
        if isinstance(discharge, str):
            if base_dir and discharge not in _file_flow_done:
                _file_flow_done.add(discharge)
                res = _resolve_block_totals_from_file(
                    base_dir,
                    input_dir,
                    "Flow",
                    discharge,
                    block_uids,
                )
                if res is not None and len(res.totals) > 0:
                    # The Parquet file sums all uid:* columns (one per
                    # flow/central), so num_uids is the true active
                    # flow count for this file.
                    num_active_flows += res.num_uids
                    first_block_afl += res.totals[0]
                    last_block_afl += res.totals[-1]
                    for b_idx in range(min(len(res.totals), num_blocks)):
                        dur = (
                            blocks[b_idx].get("duration", 1.0)
                            if b_idx < num_blocks
                            else 1.0
                        )
                        total_water_vol_hm3 += (
                            res.totals[b_idx] * dur * _M3S_TO_HM3_PER_H
                        )
            continue
        # Inline scalar or array discharge
        has_data = False
        first_val = _first_scalar(discharge)
        if first_val is not None:
            has_data = True
            first_block_afl += first_val
        if isinstance(discharge, list) and discharge:
            last_vals = discharge[-1]
            if isinstance(last_vals, (int, float)):
                last_block_afl += float(last_vals)
            else:
                last_block_afl += first_val or 0.0
        elif isinstance(discharge, (int, float)):
            last_block_afl += float(discharge)
        # Accumulate water volume for inline discharge values
        for b_idx in range(num_blocks):
            val = _scalar_at_block(discharge, b_idx)
            if val is not None:
                dur = blocks[b_idx].get("duration", 1.0) if b_idx < num_blocks else 1.0
                total_water_vol_hm3 += val * dur * _M3S_TO_HM3_PER_H
        if has_data:
            num_active_flows += 1

    num_flow = num_active_flows

    # Average flow per affluent (m³/s) = total_volume / total_time / num_flows
    avg_flow = (
        total_water_vol_hm3 / (total_hours * _M3S_TO_HM3_PER_H) / num_flow
        if total_hours > 0 and num_flow > 0
        else 0.0
    )

    # --- Capacity adequacy ratio ---
    adequacy = total_gen_cap / peak_demand if peak_demand > 0 else float("inf")

    return SystemIndicators(
        total_gen_capacity_mw=total_gen_cap,
        hydro_capacity_mw=hydro_cap,
        thermal_capacity_mw=thermal_cap,
        renewable_capacity_mw=renewable_cap,
        total_line_capacity_mw=total_line_cap,
        first_block_demand_mw=first_block_demand,
        last_block_demand_mw=last_block_demand,
        peak_demand_mw=peak_demand,
        min_demand_mw=min_demand,
        peak_demand_block=peak_block,
        total_demand_by_block=demand_by_block,
        total_energy_mwh=total_energy,
        avg_annual_energy_mwh=avg_annual_energy,
        total_hours=total_hours,
        capacity_adequacy_ratio=adequacy,
        first_block_affluent_avg=first_block_afl,
        last_block_affluent_avg=last_block_afl,
        total_water_volume_hm3=total_water_vol_hm3,
        avg_flow_m3s=avg_flow,
        avg_fcost=_avg_demand_fcost(demands, base_dir, input_dir),
        num_generators=num_gen,
        num_demands=num_dem,
        num_blocks=num_blocks if num_blocks > 0 else len(demand_by_block),
        num_flows=num_flow,
    )


def _build_indicator_pairs(ind: SystemIndicators) -> list[tuple[str, str]]:
    """Build (label, value) pairs for indicator display."""
    from gtopt_check_json._terminal import fmt_num  # noqa: PLC0415

    pairs: list[tuple[str, str]] = [
        (
            "Total gen capacity",
            f"{fmt_num(ind.total_gen_capacity_mw)} MW  ({ind.num_generators} generators)",
        ),
        ("  Hydro capacity", f"{fmt_num(ind.hydro_capacity_mw)} MW"),
        ("  Thermal capacity", f"{fmt_num(ind.thermal_capacity_mw)} MW"),
        ("  Renewable capacity", f"{fmt_num(ind.renewable_capacity_mw)} MW"),
        ("Line capacity", f"{fmt_num(ind.total_line_capacity_mw)} MW"),
        ("First block demand", f"{fmt_num(ind.first_block_demand_mw)} MW"),
        ("Last block demand", f"{fmt_num(ind.last_block_demand_mw)} MW"),
        (
            "Peak demand",
            f"{fmt_num(ind.peak_demand_mw)} MW  (block {ind.peak_demand_block})",
        ),
        ("Min demand", f"{fmt_num(ind.min_demand_mw)} MW"),
        ("Total energy", f"{fmt_num(ind.total_energy_mwh)} MWh"),
        ("Avg annual energy", f"{fmt_num(ind.avg_annual_energy_mwh)} MWh"),
        ("Capacity adequacy", fmt_num(ind.capacity_adequacy_ratio)),
    ]
    if ind.num_flows > 0:
        pairs.extend(
            [
                (
                    "First block flow",
                    f"{fmt_num(ind.first_block_affluent_avg)} m³/s"
                    f"  ({ind.num_flows} flows)",
                ),
                ("Last block flow", f"{fmt_num(ind.last_block_affluent_avg)} m³/s"),
                ("Total water volume", f"{fmt_num(ind.total_water_volume_hm3)} Hm³"),
                ("Avg flow per affluent", f"{fmt_num(ind.avg_flow_m3s)} m³/s"),
            ]
        )
    if ind.avg_fcost > 0:
        pairs.append(("Avg failure cost", f"{fmt_num(ind.avg_fcost)} $/MWh"))
    return pairs


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
        Ignored (kept for API compatibility).  Output is always plain
        text; use :func:`print_indicators` for styled terminal output.

    Returns
    -------
    str
        Human-readable indicator summary.
    """
    from gtopt_check_json._terminal import render_kv_table  # noqa: PLC0415

    ind = compute_indicators(planning, base_dir=base_dir)
    return render_kv_table(_build_indicator_pairs(ind), title="Global Indicators")


def print_indicators(
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> None:
    """Print styled global indicators directly to the terminal."""
    from gtopt_check_json._terminal import print_kv_table  # noqa: PLC0415

    ind = compute_indicators(planning, base_dir=base_dir)
    print_kv_table(_build_indicator_pairs(ind), title="Global Indicators")


# ---------------------------------------------------------------------------
# format_info (existing)
# ---------------------------------------------------------------------------


def _build_info_sections(
    planning: dict[str, Any],
) -> list[tuple[str, list[tuple[str, str]]]]:
    """Build titled sections of (label, value) pairs for info display.

    Returns a list of ``(title, pairs)`` tuples for the system, elements,
    simulation, and options sections.  The global-indicators section is
    handled separately by :func:`_build_indicator_pairs`.
    """
    sys_data = planning.get("system", {})
    sim = planning.get("simulation", {})
    opts = planning.get("options", {})

    sections: list[tuple[str, list[tuple[str, str]]]] = []

    # -- System info --
    sections.append(
        (
            "System",
            [
                ("System name", sys_data.get("name", "(unnamed)")),
                ("System version", sys_data.get("version", "")),
            ],
        )
    )

    # -- Element counts (skip zero-count entries) --
    all_elems = [
        ("Buses", len(sys_data.get("bus_array", []))),
        ("Generators", len(sys_data.get("generator_array", []))),
        ("Generator profiles", len(sys_data.get("generator_profile_array", []))),
        ("Demands", len(sys_data.get("demand_array", []))),
        ("Demand profiles", len(sys_data.get("demand_profile_array", []))),
        ("Lines", len(sys_data.get("line_array", []))),
        ("Batteries", len(sys_data.get("battery_array", []))),
        ("Converters", len(sys_data.get("converter_array", []))),
        ("Reserve zones", len(sys_data.get("reserve_zone_array", []))),
        ("Reserve provisions", len(sys_data.get("reserve_provision_array", []))),
        ("Junctions", len(sys_data.get("junction_array", []))),
        ("Waterways", len(sys_data.get("waterway_array", []))),
        ("Flows", len(sys_data.get("flow_array", []))),
        ("Reservoirs", len(sys_data.get("reservoir_array", []))),
        ("ReservoirSeepages", len(sys_data.get("reservoir_seepage_array", []))),
        ("DischargeLimits", len(sys_data.get("reservoir_discharge_limit_array", []))),
        (
            "ProductionFactors",
            len(sys_data.get("reservoir_production_factor_array", [])),
        ),
        ("Turbines", len(sys_data.get("turbine_array", []))),
    ]
    elem_pairs = [(k, str(v)) for k, v in all_elems if v > 0]
    if not elem_pairs:
        elem_pairs = [(k, str(v)) for k, v in all_elems]
    sections.append(("Elements", elem_pairs))

    # -- Simulation --
    sections.append(
        (
            "Simulation",
            [
                ("Blocks", str(len(sim.get("block_array", [])))),
                ("Stages", str(len(sim.get("stage_array", [])))),
                ("Scenarios", str(len(sim.get("scenario_array", [])))),
            ],
        )
    )

    # -- Key options --
    use_kirch = opts.get("use_kirchhoff", False)
    use_sb = opts.get("use_single_bus", False)
    sections.append(
        (
            "Options",
            [
                ("use_kirchhoff", "true" if use_kirch else "false"),
                ("use_single_bus", "true" if use_sb else "false"),
                ("scale_objective", str(opts.get("scale_objective", 1000.0))),
                ("demand_fail_cost", str(opts.get("demand_fail_cost", 0.0))),
                ("input_directory", str(opts.get("input_directory", "(default)"))),
                ("output_directory", str(opts.get("output_directory", "(default)"))),
                ("output_format", str(opts.get("output_format", "csv"))),
            ],
        )
    )

    return sections


def _build_skipped_pairs(
    planning: dict[str, Any],
) -> list[tuple[str, str]] | None:
    """Build (name, reason) pairs for skipped isolated centrals, or None."""
    skipped = planning.get("_skipped_isolated", [])
    if not skipped:
        return None
    return [(name, "isolated (bus<=0, no waterways)") for name in sorted(skipped)]


def format_info(
    planning: dict[str, Any],
    base_dir: str | None = None,
    *,
    colr: bool = False,
) -> str:
    """Return a multi-line string with system, simulation, and option stats.

    The output is presented as compact plain-text tables suitable for
    logging and piping to files.  For styled terminal output use
    :func:`print_info` instead.

    Parameters
    ----------
    planning
        A planning dict as loaded from a gtopt JSON file.
    base_dir
        Optional case directory for resolving FieldSched file references.
    colr
        Ignored (kept for API compatibility).
    """
    from gtopt_check_json._terminal import render_kv_table  # noqa: PLC0415

    lines: list[str] = []
    for title, pairs in _build_info_sections(planning):
        lines.append(render_kv_table(pairs, title=title))

    # -- Skipped isolated centrals (from plp2gtopt) --
    skipped_pairs = _build_skipped_pairs(planning)
    if skipped_pairs is not None:
        lines.append(
            render_kv_table(
                skipped_pairs,
                title=f"Skipped Centrals ({len(skipped_pairs)})",
            )
        )

    # -- Global indicators --
    lines.append(format_indicators(planning, base_dir=base_dir))

    return "\n".join(lines)


def print_info(
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> None:
    """Print styled system info directly to the terminal.

    Uses :mod:`rich` via :mod:`gtopt_check_json._terminal` for styled
    tables with automatic colour and Unicode detection.
    """
    from gtopt_check_json._terminal import print_kv_table  # noqa: PLC0415

    for title, pairs in _build_info_sections(planning):
        print_kv_table(pairs, title=title)

    # -- Skipped isolated centrals (from plp2gtopt) --
    skipped_pairs = _build_skipped_pairs(planning)
    if skipped_pairs is not None:
        print_kv_table(
            skipped_pairs,
            title=f"Skipped Centrals ({len(skipped_pairs)})",
        )

    # -- Global indicators --
    print_indicators(planning, base_dir=base_dir)
