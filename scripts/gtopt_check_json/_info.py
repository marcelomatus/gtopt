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
    up_reserve_req_mw: float = 0.0
    dn_reserve_req_mw: float = 0.0
    # -- Tier 1: cost & fuel --
    avg_gcost: float = 0.0  # $/MWh, mean over non-failure generators
    min_gcost: float = 0.0
    max_gcost: float = 0.0
    avg_heat_rate: float = 0.0  # fuel-unit/MWh, mean over fuelled generators
    avg_fuel_price: float = 0.0  # $/fuel-unit, mean over priced fuels (Fuel_Price)
    num_fuelled_generators: int = 0
    total_fuel_offtake_cap: float = 0.0  # Σ Fuel.max_offtake (weekly caps)
    num_fuel_caps: int = 0
    # Take-or-pay floors (Fuel.min_offtake; PLEXOS Min Offtake family
    # pids 595-602).  Zero across every cached CEN PCP bundle
    # (2025-10..2026-05) — the converter ships dormant plumbing.
    total_fuel_offtake_floor: float = 0.0  # Σ Fuel.min_offtake
    num_fuel_floors: int = 0
    # -- Tier 2: commitment --
    total_startup_cost: float = 0.0
    total_shutdown_cost: float = 0.0
    num_committable: int = 0
    num_must_run: int = 0
    # -- Tier 3: storage & FCF --
    total_battery_energy_mwh: float = 0.0  # Σ Battery.emax
    total_battery_power_mw: float = 0.0  # Σ Battery.pmax_discharge
    avg_battery_efficiency: float = 0.0  # mean(input_eff × output_eff)
    total_reservoir_storage: float = 0.0  # Σ Reservoir.emax
    total_reservoir_initial: float = 0.0  # Σ Reservoir.eini
    total_reservoir_final: float = 0.0  # Σ Reservoir.efin
    fcf_intercept: float = 0.0  # FCF boundary-cut RHS ($)
    num_water_value_reservoirs: int = 0
    # -- Tier 4: network & demand --
    num_lossy_lines: int = 0  # lines with a piecewise/linear loss model
    total_turbine_capacity_mw: float = 0.0  # Σ pmax of turbine-driven gens
    total_up_provision_mw: float = 0.0  # Σ peak ReserveProvision.urmax
    total_dn_provision_mw: float = 0.0  # Σ peak ReserveProvision.drmax
    up_reserve_margin_mw: float = 0.0  # peak provision − peak requirement (up)
    dn_reserve_margin_mw: float = 0.0  # peak provision − peak requirement (dn)
    num_fuel_emission_factors: int = 0  # fuels with a synthesized emission factor
    fixed_load_energy_gwh: float = 0.0  # Σ Generator.pmin × duration (must-take)
    num_gen_pmax_profiles: int = 0  # generators with a non-constant pmax profile
    num_provision_profiles: int = 0  # provisions with a non-constant urmax/drmax
    demand_fail_cost: float = 0.0  # VoLL [$/MWh]
    renewable_energy_gwh: float = 0.0  # Σ renewable capacity × duration
    num_generators: int = 0
    num_demands: int = 0
    num_blocks: int = 0
    num_flows: int = 0
    # -- Tier 5: input-file read receipts --
    # One aggregate per PLEXOS input file that no other indicator reflects, so
    # a file silently not read (empty / missing) shows up as a zero here.
    total_gen_min_stable_mw: float = (
        0.0  # Σ peak Generator.pmin (Gen_FixedLoad forced floor)
    )
    commitment_min_stable_mw: float = (
        0.0  # Σ peak Commitment.pmin (Gen_MinStableLevel, when-committed)
    )
    total_initial_gen_power_mw: float = (
        0.0  # Σ Commitment.initial_power (Gen_IniGeneration)
    )
    num_units_initial_on: int = 0  # Commitment.initial_status>0 (Gen_IniUnits)
    total_initial_commit_hours: float = 0.0  # Σ |initial_hours| (Gen_IniHoursUp/Down)
    num_units_with_ramp_limit: int = 0  # Commitment.ramp_up>0 (CPF/Max Ramp)
    total_battery_initial_mwh: float = 0.0  # Σ Battery.eini (BESS_IniValue)
    total_reservoir_min_vol: float = 0.0  # Σ Reservoir.emin (Hydro_MinVolume)


@dataclass
class SystemCounts:
    """Element counts derived from a gtopt planning dict (the JSON authority).

    Counts every element class :func:`_build_info_sections` already lists,
    PLUS the plexos2gtopt-specific classes (fuels, flow rights, commitments,
    decision variables, FCF boundary cuts) and — the centerpiece — user
    constraints broken down by *family* and by active/inactive.

    User constraints may be emitted two ways and both are counted:

    * **inline** — ``system.user_constraint_array`` (each entry classified by
      :func:`uc_family` on its ``name``);
    * **pampl** — modular ``uc_<family>.pampl`` files referenced by
      ``system.user_constraint_files`` (counted via
      :func:`gtopt_check_pampl.compute_stats` when *base_dir* is supplied).
    """

    # -- Network / generation --
    buses: int = 0
    generators: int = 0
    gen_by_type: dict[str, int] = field(default_factory=dict)
    generator_profiles: int = 0
    demands: int = 0
    demand_profiles: int = 0
    lines: int = 0
    batteries: int = 0
    converters: int = 0
    # -- Reserves --
    reserve_zones: int = 0
    reserve_provisions: int = 0
    # -- Hydro --
    junctions: int = 0
    junctions_synth: int = 0  # synthesised <reservoir>_terminal_ocean sinks
    waterways: int = 0
    waterways_synth: int = 0  # synthesised penstock_<turbine> per-turbine arcs
    flows: int = 0
    reservoirs: int = 0
    reservoir_seepages: int = 0
    discharge_limits: int = 0
    production_factors: int = 0
    turbines: int = 0
    # -- PLEXOS extras --
    fuels: int = 0
    flow_rights: int = 0
    commitments: int = 0
    decision_variables: int = 0
    decision_variables_alpha: int = 0  # synthesised FCF alpha_fcf column(s)
    # -- User constraints (centerpiece) --
    user_constraints_total: int = 0
    user_constraints_active: int = 0
    user_constraints_inactive: int = 0
    uc_by_family: dict[str, int] = field(default_factory=dict)
    uc_inline: int = 0
    uc_in_pampl: int = 0
    # -- Boundary cuts (FCF) --
    boundary_cuts: int = 0
    boundary_state_variables: int = 0
    # -- Simulation --
    num_blocks: int = 0
    num_stages: int = 0
    num_scenarios: int = 0


# ---------------------------------------------------------------------------
# UC family classifier — soft import the single source of truth from
# plexos2gtopt so both the converter and gtopt_check classify identically.
# Falls back to a single bucket when plexos2gtopt is not importable (counts
# still total correctly).
# ---------------------------------------------------------------------------
try:
    from plexos2gtopt.uc_families import (  # noqa: PLC0415
        UC_FAMILY_ORDER,
        uc_family,
    )
except ImportError:  # pragma: no cover - exercised only without plexos2gtopt
    UC_FAMILY_ORDER = ("operational",)

    def uc_family(_name: str) -> str:  # type: ignore[misc]
        """Fallback classifier: everything is ``operational``."""
        return "operational"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _is_failure_generator(gen: dict[str, Any]) -> bool:
    """Return True if *gen* is a PLP failure (``"falla"``) generator.

    Detection is purely by ``type`` attribute, **not** by pmax magnitude.
    """
    return str(gen.get("type", "")).lower() == _FAILURE_GEN_TYPE


def _block_vector(val: Any) -> float | list[Any] | None:
    """Descend a FieldSched value to its innermost per-block numeric vector.

    Handles the inline shapes gtopt emits: a scalar, a flat ``[v0, v1, ...]``
    block vector, a single-stage TB-matrix ``[[v0, ...]]`` (what plexos2gtopt
    writes for ``Demand.lmax`` / per-block ``rhs``), and a
    scenario/stage matrix ``[[[v0, ...]]]`` (``Flow.discharge``).  Descends the
    FIRST element of each wrapping list-of-lists layer (first scenario / first
    stage) until it reaches a list whose elements are scalars, then returns
    that vector.  Returns the scalar unchanged, or ``None`` for strings (file
    references) / ``None``.
    """
    if val is None or isinstance(val, str):
        return None
    if isinstance(val, (int, float)):
        return float(val)
    if not isinstance(val, list):
        return None
    cur: Any = val
    while isinstance(cur, list) and cur and isinstance(cur[0], list):
        cur = cur[0]
    return cur if isinstance(cur, list) else None


def _first_scalar(val: Any) -> float | None:
    """Extract the first numeric value from a FieldSched-style field.

    Returns the scalar for a number, the first element for a (possibly
    nested) block vector, or ``None`` for a string (file reference) / ``None``.
    """
    vec = _block_vector(val)
    if vec is None:
        return None
    if isinstance(vec, float):
        return vec
    if vec and isinstance(vec[0], (int, float)):
        return float(vec[0])
    return None


def _peak_scalar(val: Any) -> float | None:
    """Peak (max) numeric value of a FieldSched field — i.e. nameplate.

    Used for generation *capacity*: a renewable's ``capacity`` is stored as an
    hourly availability profile, so the first block (e.g. midnight solar) would
    understate the installed capacity.  The profile peak recovers the rated
    nameplate.  Scalars pass through; strings / ``None`` give ``None``.
    """
    vec = _block_vector(val)
    if vec is None:
        return None
    if isinstance(vec, float):
        return vec
    nums = [float(v) for v in vec if isinstance(v, (int, float))]
    return max(nums) if nums else None


def _scalar_at_block(val: Any, block_idx: int) -> float | None:
    """Extract the numeric value for a specific block index.

    Returns the scalar for a number, ``vec[block_idx]`` for a (possibly
    nested) block vector, or ``None`` for a string (file reference) / ``None``.
    """
    vec = _block_vector(val)
    if vec is None:
        return None
    if isinstance(vec, float):
        return vec
    if block_idx < len(vec) and isinstance(vec[block_idx], (int, float)):
        return float(vec[block_idx])
    # If the block index exceeds the list, return the last available value
    if block_idx >= len(vec) and vec and isinstance(vec[-1], (int, float)):
        return float(vec[-1])
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

                root = str(Path(base_dir) / input_dir) if input_dir != "." else base_dir
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

    # Generators driven by a turbine are hydro regardless of their ``type``
    # tag (plexos2gtopt tags hydro units "renewable"/"thermal" but links them
    # to a Turbine; PLP tags them directly).  Recognise both.
    turbine_gen_names = frozenset(
        str(t.get("generator", ""))
        for t in sys_data.get("turbine_array", [])
        if t.get("generator")
    )

    total_gen_cap = 0.0
    hydro_cap = 0.0
    thermal_cap = 0.0
    renewable_cap = 0.0
    cap_by_type: dict[str, float] = {}
    num_gen = 0
    for gen in generators:
        if _is_failure_generator(gen):
            continue
        # Capacity = nameplate: the profile peak, not the first block (a
        # renewable's first hour can be 0, e.g. midnight solar).
        cap = _peak_scalar(gen.get("capacity"))
        if cap is None:
            cap = _peak_scalar(gen.get("pmax"))
        if cap is not None:
            total_gen_cap += cap
            num_gen += 1
            gtype = str(gen.get("type", "")).lower()
            category = classify_type(gtype)
            if str(gen.get("name", "")) in turbine_gen_names:
                category = "hydro"
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

    # Line capacity = the NOMINAL bidirectional rating.  For plexos2gtopt
    # soft-capped (ex-EL0) lines, ``tmax_ab`` / ``tmax_ba`` hold the inflated
    # soft-cap HARD limit (e.g. 5× rating) and ``tmax_normal_*`` the free-band
    # threshold (e.g. 2× rating) — neither is the physical rating.  The true
    # nominal is preserved in ``loss_envelope``, so count it bidirectionally
    # (2×) for those lines; everyone else (hard caps, PLP) keeps the plain
    # ``tmax_ab + tmax_ba`` sum.
    total_line_cap = 0.0
    for line in lines_arr:
        env = _peak_scalar(line.get("loss_envelope"))
        if env is not None and env > 0.0:
            total_line_cap += 2.0 * env
            continue
        # Peak (nameplate) rating, not the first block — dynamic-line-rating
        # (DLR) lines carry a per-block tmax profile whose peak is the rated
        # capacity (the off-peak first block understates it).
        for key in ("tmax_ab", "tmax_ba"):
            val = _peak_scalar(line.get(key))
            if val is not None:
                total_line_cap += val

    # --- Reserve requirement (MW) ---
    # PEAK up (``urreq``) and down (``drreq``) reserve requirement summed across
    # all reserve zones.  Requirements are day-type profiles; block 0 is the
    # off-peak (midnight) hour, so peak is the binding system requirement.
    up_reserve_req = 0.0
    dn_reserve_req = 0.0
    for zone in sys_data.get("reserve_zone_array", []):
        ur = _peak_scalar(zone.get("urreq"))
        dr = _peak_scalar(zone.get("drreq"))
        if ur is not None:
            up_reserve_req += ur
        if dr is not None:
            dn_reserve_req += dr

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
        # Inline scalar or (possibly nested) array discharge
        has_data = False
        vec = _block_vector(discharge)
        if isinstance(vec, float):
            first_block_afl += vec
            last_block_afl += vec
            has_data = True
        elif isinstance(vec, list) and vec and isinstance(vec[0], (int, float)):
            first_block_afl += float(vec[0])
            last_block_afl += float(vec[-1])
            has_data = True
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

    # === Tier 1: cost & fuel ===
    # Effective marginal cost ($/MWh).  For fuel-linked units the writer emits
    # ``gcost`` = VOM + transport only and resolves the full cost at LP time as
    # ``fuel.price × heat_rate + gcost``; reconstruct that here so the
    # indicator reflects the true dispatch cost, not just the VOM component.
    fuel_price = {
        f.get("name"): (_first_scalar(f.get("price")) or 0.0)
        for f in sys_data.get("fuel_array", [])
    }
    gcosts: list[float] = []
    heat_rates: list[float] = []
    for gen in generators:
        if _is_failure_generator(gen):
            continue
        base = _first_scalar(gen.get("gcost")) or 0.0
        hr = _first_scalar(gen.get("heat_rate"))
        fuel = gen.get("fuel")
        if fuel and hr is not None and hr > 0.0:
            gcosts.append(fuel_price.get(fuel, 0.0) * hr + base)
        else:
            gcosts.append(base)
        if hr is not None and hr > 0.0:
            heat_rates.append(hr)
    avg_gcost = sum(gcosts) / len(gcosts) if gcosts else 0.0
    min_gcost = min(gcosts) if gcosts else 0.0
    max_gcost = max(gcosts) if gcosts else 0.0
    avg_heat_rate = sum(heat_rates) / len(heat_rates) if heat_rates else 0.0
    # Average fuel price ($/fuel-unit) over priced fuels — a direct read-receipt
    # for Fuel_Price.csv (the avg gen cost only reflects it indirectly, folded
    # through heat rate, so a fuel-price mismatch can hide there).
    fuel_prices = [p for p in fuel_price.values() if p > 0.0]
    avg_fuel_price = sum(fuel_prices) / len(fuel_prices) if fuel_prices else 0.0
    fuel_caps = [
        c
        for c in (
            _first_scalar(f.get("max_offtake")) for f in sys_data.get("fuel_array", [])
        )
        if c is not None
    ]
    fuel_floors = [
        c
        for c in (
            _first_scalar(f.get("min_offtake")) for f in sys_data.get("fuel_array", [])
        )
        if c is not None
    ]

    # Σ generator minimum stable level (Gen_MinStableLevel / Gen_FixedLoad).
    # ``pmin`` is emitted as a per-block ``[[...]]`` profile for fixed-load
    # forced units, so reduce by PEAK (not block 0 — which is midnight, where a
    # must-take solar/RoR fixed-load floor is 0 and would understate the total).
    total_gen_min_stable = sum(
        _peak_scalar(g.get("pmin")) or 0.0
        for g in generators
        if not _is_failure_generator(g)
    )

    # === Tier 2: commitment ===
    commits = sys_data.get("commitment_array", [])
    total_startup_cost = sum(
        _first_scalar(c.get("startup_cost")) or 0.0 for c in commits
    )
    total_shutdown_cost = sum(
        _first_scalar(c.get("shutdown_cost")) or 0.0 for c in commits
    )
    num_committable = sum(
        1 for c in commits if (_first_scalar(c.get("startup_cost")) or 0.0) > 0.0
    )
    num_must_run = sum(1 for c in commits if c.get("must_run"))
    # Read receipts for the per-generator initial-state / ramp inputs.
    total_initial_gen_power = sum(
        _first_scalar(c.get("initial_power")) or 0.0 for c in commits
    )
    num_units_initial_on = sum(
        1 for c in commits if (_first_scalar(c.get("initial_status")) or 0.0) > 0.0
    )
    total_initial_commit_hours = sum(
        abs(_first_scalar(c.get("initial_hours")) or 0.0) for c in commits
    )
    num_units_with_ramp_limit = sum(
        1 for c in commits if (_peak_scalar(c.get("ramp_up")) or 0.0) > 0.0
    )
    # Commitment-conditional Min Stable Level (Gen_MinStableLevel → Commitment
    # .pmin, enforced only when committed) — PEAK reduction (per-block profile).
    # Distinct from the unconditional Fixed-Load floor on generator.pmin.
    total_commitment_min_stable = sum(
        _peak_scalar(c.get("pmin")) or 0.0 for c in commits
    )

    # === Tier 3: storage & FCF ===
    bat_effs: list[float] = []
    for bat in batteries:
        ie = _first_scalar(bat.get("input_efficiency"))
        oe = _first_scalar(bat.get("output_efficiency"))
        if ie is not None and oe is not None:
            bat_effs.append(ie * oe)
    total_batt_energy = sum(_first_scalar(b.get("emax")) or 0.0 for b in batteries)
    total_batt_power = sum(
        _first_scalar(b.get("pmax_discharge")) or 0.0 for b in batteries
    )
    total_batt_initial = sum(_first_scalar(b.get("eini")) or 0.0 for b in batteries)
    reservoirs = sys_data.get("reservoir_array", [])
    total_res_storage = sum(_first_scalar(r.get("emax")) or 0.0 for r in reservoirs)
    total_res_initial = sum(_first_scalar(r.get("eini")) or 0.0 for r in reservoirs)
    total_res_final = sum(_first_scalar(r.get("efin")) or 0.0 for r in reservoirs)
    total_res_min_vol = sum(_first_scalar(r.get("emin")) or 0.0 for r in reservoirs)
    fcf_intercept, num_wv = _read_fcf_intercept(planning, base_dir)

    # === Tier 4: network & demand ===
    num_lossy = sum(1 for ln in lines_arr if ln.get("line_losses_mode"))
    pmax_by_name = {
        g.get("name"): (
            _peak_scalar(g.get("capacity")) or _peak_scalar(g.get("pmax")) or 0.0
        )
        for g in generators
    }
    total_turb_cap = sum(
        pmax_by_name.get(t.get("generator"), 0.0)
        for t in sys_data.get("turbine_array", [])
    )
    # Reserve provision caps: ``urmax``/``drmax`` are emitted as per-block
    # ``[[...]]`` profiles (the CFdata MRU/MRD per-hour reserve capability), so
    # reduce by PEAK.  Block 0 is midnight, where solar/BESS reserve capability
    # is ~0 — first-block would understate the total reserve capability by ~30%.
    provs = sys_data.get("reserve_provision_array", [])
    total_up_prov = sum(_peak_scalar(p.get("urmax")) or 0.0 for p in provs)
    total_dn_prov = sum(_peak_scalar(p.get("drmax")) or 0.0 for p in provs)
    model_opts = opts.get("model_options", {}) if isinstance(opts, dict) else {}
    demand_fail_cost = float(
        model_opts.get("demand_fail_cost", opts.get("demand_fail_cost", 0.0)) or 0.0
    )
    # Renewable available energy (GWh): Σ capacity × duration over non-hydro
    # renewable generators (the must-take availability profiles).
    renew_energy = 0.0
    for gen in generators:
        if _is_failure_generator(gen) or str(gen.get("name", "")) in turbine_gen_names:
            continue
        if classify_type(str(gen.get("type", ""))) != "renewable":
            continue
        cap_field = gen.get("capacity")
        if cap_field is None:
            cap_field = gen.get("pmax")
        for b_idx in range(num_blocks):
            v = _scalar_at_block(cap_field, b_idx)
            if v is not None:
                dur = blocks[b_idx].get("duration", 1.0) if b_idx < len(blocks) else 1.0
                renew_energy += v * dur

    # Fuels carrying an emission factor (#).  PLEXOS ships no explicit rates for
    # CEN PCP; the converter SYNTHESIZES them from the bundled IPCC defaults.
    # A COUNT (not a rate sum) is the robust check for synthesized data: a Δ vs
    # the PLEXOS-expected coverage flags a fuel family missing from the defaults
    # (the real risk), without depending on the exact rate values.
    num_fuel_emission_factors = sum(
        1 for f in sys_data.get("fuel_array", []) if f.get("emission_factors")
    )

    # Fixed-load forced energy (GWh): Σ generator.pmin per block × duration —
    # the must-take volume PLEXOS pins via Gen_FixedLoad (emitted as a per-block
    # ``pmin`` profile).  Reuses the per-block × duration accumulation so a
    # mismatch in the forced trajectory (not just its peak) is visible.
    fixed_load_energy = 0.0
    for gen in generators:
        if _is_failure_generator(gen):
            continue
        pmin_field = gen.get("pmin")
        if pmin_field is None:
            continue
        for b_idx in range(num_blocks):
            v = _scalar_at_block(pmin_field, b_idx)
            if v:
                dur = blocks[b_idx].get("duration", 1.0) if b_idx < len(blocks) else 1.0
                fixed_load_energy += v * dur

    # Profile-coverage counts — number of elements whose cap is emitted as a
    # NON-CONSTANT per-block profile.  These tie out with the PLEXOS side only
    # if every time-varying field survived conversion as a profile, so a
    # "profile silently flattened to a scalar" regression shows up as a Δ here.
    def _is_varying(field: Any) -> bool:
        vec = _block_vector(field)
        if not isinstance(vec, list):
            return False
        nums = [float(v) for v in vec if isinstance(v, (int, float))]
        return bool(nums) and (max(nums) - min(nums)) > 1e-9

    num_gen_pmax_profiles = sum(
        1
        for g in generators
        if _is_varying(
            g.get("capacity") if g.get("capacity") is not None else g.get("pmax")
        )
    )
    num_provision_profiles = sum(
        1 for p in provs if _is_varying(p.get("urmax")) or _is_varying(p.get("drmax"))
    )

    # Reserve adequacy margin (peak provision capability − peak requirement).
    # Negative = the summed peak reserve capability cannot meet the summed peak
    # requirement (an LP-feasibility red flag, not just a count).
    up_reserve_margin = total_up_prov - up_reserve_req
    dn_reserve_margin = total_dn_prov - dn_reserve_req

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
        up_reserve_req_mw=up_reserve_req,
        dn_reserve_req_mw=dn_reserve_req,
        avg_gcost=avg_gcost,
        min_gcost=min_gcost,
        max_gcost=max_gcost,
        avg_heat_rate=avg_heat_rate,
        avg_fuel_price=avg_fuel_price,
        num_fuelled_generators=len(heat_rates),
        total_fuel_offtake_cap=sum(fuel_caps),
        num_fuel_caps=len(fuel_caps),
        total_fuel_offtake_floor=sum(fuel_floors),
        num_fuel_floors=len(fuel_floors),
        total_startup_cost=total_startup_cost,
        total_shutdown_cost=total_shutdown_cost,
        num_committable=num_committable,
        num_must_run=num_must_run,
        total_battery_energy_mwh=total_batt_energy,
        total_battery_power_mw=total_batt_power,
        avg_battery_efficiency=(sum(bat_effs) / len(bat_effs) if bat_effs else 0.0),
        total_reservoir_storage=total_res_storage,
        total_reservoir_initial=total_res_initial,
        total_reservoir_final=total_res_final,
        fcf_intercept=fcf_intercept,
        num_water_value_reservoirs=num_wv,
        num_lossy_lines=num_lossy,
        total_turbine_capacity_mw=total_turb_cap,
        total_up_provision_mw=total_up_prov,
        total_dn_provision_mw=total_dn_prov,
        up_reserve_margin_mw=up_reserve_margin,
        dn_reserve_margin_mw=dn_reserve_margin,
        num_fuel_emission_factors=num_fuel_emission_factors,
        fixed_load_energy_gwh=fixed_load_energy / 1e3,
        num_gen_pmax_profiles=num_gen_pmax_profiles,
        num_provision_profiles=num_provision_profiles,
        demand_fail_cost=demand_fail_cost,
        renewable_energy_gwh=renew_energy / 1e3,
        num_generators=num_gen,
        num_demands=num_dem,
        num_blocks=num_blocks if num_blocks > 0 else len(demand_by_block),
        num_flows=num_flow,
        total_gen_min_stable_mw=total_gen_min_stable,
        commitment_min_stable_mw=total_commitment_min_stable,
        total_initial_gen_power_mw=total_initial_gen_power,
        num_units_initial_on=num_units_initial_on,
        total_initial_commit_hours=total_initial_commit_hours,
        num_units_with_ramp_limit=num_units_with_ramp_limit,
        total_battery_initial_mwh=total_batt_initial,
        total_reservoir_min_vol=total_res_min_vol,
    )


def _family_from_pampl_name(filename: str) -> str:
    """Derive the UC family from a ``uc_<family>.pampl`` filename stem.

    The converter routes each constraint to a per-family file named
    ``uc_<family>.pampl`` (see ``write_user_constraint_pampl``).  Strip the
    directory, the ``.pampl`` suffix and the ``uc_`` prefix to recover the
    family; anything that does not match falls back to ``operational``.
    """
    stem = Path(filename).name
    if stem.endswith(".pampl"):
        stem = stem[: -len(".pampl")]
    if stem.startswith("uc_"):
        fam = stem[len("uc_") :]
        if fam in UC_FAMILY_ORDER:
            return fam
    return "operational"


def _read_fcf_intercept(
    planning: dict[str, Any], base_dir: str | None
) -> tuple[float, int]:
    """Return ``(fcf_intercept_rhs, num_water_value_reservoirs)``.

    Reads the FCF ``boundary_cuts.csv`` (header ``scene,rhs,<reservoir>...``,
    one data row): the ``rhs`` column is the future-cost intercept ($) and the
    reservoir columns are the per-reservoir water-value slopes.  Returns
    ``(0.0, 0)`` when no cut file is present or *base_dir* is unset.
    """
    opts = planning.get("options", {})
    mono = opts.get("monolithic_options", {}) if isinstance(opts, dict) else {}
    sim = planning.get("simulation", {})
    cut_file = mono.get("boundary_cuts_file") or sim.get("boundary_cuts_file")
    if not cut_file or not base_dir:
        return 0.0, 0
    path = Path(base_dir) / str(cut_file)
    if not path.is_file():
        return 0.0, 0
    try:
        import csv  # noqa: PLC0415

        with path.open("r", encoding="utf-8", newline="") as fh:
            rows = list(csv.reader(fh))
    except OSError:
        return 0.0, 0
    if len(rows) < 2:
        return 0.0, 0
    num_slopes = max(0, len(rows[0]) - 2)
    try:
        rhs = float(rows[1][1]) if len(rows[1]) > 1 else 0.0
    except ValueError:
        rhs = 0.0
    return rhs, num_slopes


def _count_boundary_cuts(
    planning: dict[str, Any], base_dir: str | None
) -> tuple[int, int]:
    """Return ``(num_cuts, num_state_variables)`` for the FCF boundary cut.

    The cut is referenced by ``options.monolithic_options.boundary_cuts_file``
    (or ``simulation.boundary_cuts_file``) and stored as a CSV whose header is
    ``scene,rhs,<reservoir>...`` followed by one data row per cut.  When
    *base_dir* is given the CSV is parsed (rows → cuts, slope columns → state
    variables); otherwise the file presence counts as a single cut.
    """
    opts = planning.get("options", {})
    mono = opts.get("monolithic_options", {}) if isinstance(opts, dict) else {}
    sim = planning.get("simulation", {})
    cut_file = mono.get("boundary_cuts_file") or sim.get("boundary_cuts_file")
    if not cut_file:
        return 0, 0
    if not base_dir:
        return 1, 0
    path = Path(base_dir) / str(cut_file)
    if not path.is_file():
        return 1, 0
    try:
        import csv  # noqa: PLC0415

        with path.open("r", encoding="utf-8", newline="") as fh:
            rows = list(csv.reader(fh))
    except OSError:
        return 1, 0
    if not rows:
        return 0, 0
    header = rows[0]
    # state variables = slope columns (everything past "scene","rhs")
    state_vars = max(0, len(header) - 2)
    num_cuts = max(0, len(rows) - 1)
    return num_cuts, state_vars


def _count_pampl_uc(
    planning: dict[str, Any], base_dir: str | None, counts: SystemCounts
) -> None:
    """Count user constraints emitted as modular ``uc_<family>.pampl`` files.

    Mutates *counts* in place: ``uc_in_pampl``, ``uc_by_family`` and the
    active/inactive totals.  Uses :func:`gtopt_check_pampl.compute_stats` for
    the authoritative per-file constraint count when *base_dir* is supplied;
    otherwise the family is recorded with an unknown (zero) count.
    """
    sys_data = planning.get("system", {})
    files: list[str] = list(sys_data.get("user_constraint_files", []) or [])
    legacy = sys_data.get("user_constraint_file")
    if isinstance(legacy, str) and legacy:
        files.append(legacy)
    if not files:
        return
    compute_stats = None
    if base_dir:
        try:
            from gtopt_check_pampl._checks import (  # noqa: PLC0415
                compute_stats as _cs,
            )

            compute_stats = _cs
        except ImportError:
            compute_stats = None
    for fname in files:
        fam = _family_from_pampl_name(fname)
        n_total = 0
        n_inactive = 0
        if compute_stats is not None and base_dir:
            path = Path(base_dir) / str(fname)
            if path.is_file():
                try:
                    src = path.read_text(encoding="utf-8")
                except OSError:
                    src = ""
                if src:
                    stats = compute_stats(src, str(fname))
                    n_total = stats.num_constraints
                    n_inactive = stats.num_inactive
        counts.uc_by_family[fam] = counts.uc_by_family.get(fam, 0) + n_total
        counts.uc_in_pampl += n_total
        counts.user_constraints_total += n_total
        counts.user_constraints_inactive += n_inactive
        counts.user_constraints_active += n_total - n_inactive


def _count_inline_uc(planning: dict[str, Any], counts: SystemCounts) -> None:
    """Count user constraints stored inline in ``system.user_constraint_array``.

    Mutates *counts* in place: ``uc_inline``, ``uc_by_family`` and the
    active/inactive totals.  A constraint is inactive only when it carries an
    explicit ``active == False``.
    """
    sys_data = planning.get("system", {})
    arr = sys_data.get("user_constraint_array", []) or []
    for uc in arr:
        name = str(uc.get("name", ""))
        fam = uc_family(name)
        counts.uc_by_family[fam] = counts.uc_by_family.get(fam, 0) + 1
        counts.uc_inline += 1
        counts.user_constraints_total += 1
        if uc.get("active") is False:
            counts.user_constraints_inactive += 1
        else:
            counts.user_constraints_active += 1


def compute_counts(
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> SystemCounts:
    """Count every element class in a gtopt planning dict.

    This is the gtopt-JSON-side authority used by the PLEXOS↔gtopt
    conversion-comparison report (and by ``gtopt_check_json --info``): the
    PLEXOS side reads the parsed bundle, the gtopt side calls this function so
    both sides agree on what landed in the JSON.

    Parameters
    ----------
    planning
        A planning dict as loaded from a gtopt JSON file (or built in memory).
    base_dir
        Case directory used to read referenced ``.pampl`` files and the
        ``boundary_cuts.csv``.  Without it, pampl UCs are recorded by family
        only (count unknown) and the boundary cut counts as 1.

    Returns
    -------
    SystemCounts
        Element counts, including user constraints by family and active state.
    """
    sys_data = planning.get("system", {})
    sim = planning.get("simulation", {})

    counts = SystemCounts(
        buses=len(sys_data.get("bus_array", [])),
        generators=len(sys_data.get("generator_array", [])),
        generator_profiles=len(sys_data.get("generator_profile_array", [])),
        demands=len(sys_data.get("demand_array", [])),
        demand_profiles=len(sys_data.get("demand_profile_array", [])),
        lines=len(sys_data.get("line_array", [])),
        batteries=len(sys_data.get("battery_array", [])),
        converters=len(sys_data.get("converter_array", [])),
        reserve_zones=len(sys_data.get("reserve_zone_array", [])),
        reserve_provisions=len(sys_data.get("reserve_provision_array", [])),
        junctions=len(sys_data.get("junction_array", [])),
        junctions_synth=sum(
            1
            for j in sys_data.get("junction_array", [])
            if str(j.get("name", "")).endswith("_terminal_ocean")
        ),
        waterways=len(sys_data.get("waterway_array", [])),
        waterways_synth=sum(
            1
            for w in sys_data.get("waterway_array", [])
            if str(w.get("name", "")).startswith("penstock_")
        ),
        flows=len(sys_data.get("flow_array", [])),
        reservoirs=len(sys_data.get("reservoir_array", [])),
        reservoir_seepages=len(sys_data.get("reservoir_seepage_array", [])),
        discharge_limits=len(sys_data.get("reservoir_discharge_limit_array", [])),
        production_factors=len(sys_data.get("reservoir_production_factor_array", [])),
        turbines=len(sys_data.get("turbine_array", [])),
        fuels=len(sys_data.get("fuel_array", [])),
        flow_rights=len(sys_data.get("flow_right_array", [])),
        commitments=len(sys_data.get("commitment_array", [])),
        decision_variables=len(sys_data.get("decision_variable_array", [])),
        decision_variables_alpha=sum(
            1
            for d in sys_data.get("decision_variable_array", [])
            if str(d.get("name", "")).startswith("alpha_fcf")
        ),
        num_blocks=len(sim.get("block_array", [])),
        num_stages=len(sim.get("stage_array", [])),
        num_scenarios=len(sim.get("scenario_array", [])),
    )

    # Generator type breakdown.  A generator driven by a turbine is HYDRO
    # regardless of its ``type`` tag (plexos2gtopt tags fuel-less hydro units
    # "renewable" but links them to a Turbine), mirroring ``compute_indicators``
    # so the hydro/renewable split lines up with the PLEXOS side.
    turbine_gen_names = frozenset(
        str(t.get("generator", ""))
        for t in sys_data.get("turbine_array", [])
        if t.get("generator")
    )
    for gen in sys_data.get("generator_array", []):
        if str(gen.get("name", "")) in turbine_gen_names:
            gtype = "hydro"
        else:
            gtype = str(gen.get("type", "")) or "(untyped)"
        counts.gen_by_type[gtype] = counts.gen_by_type.get(gtype, 0) + 1

    # User constraints — both emit modes (inline + pampl).
    _count_inline_uc(planning, counts)
    _count_pampl_uc(planning, base_dir, counts)

    # FCF boundary cut.
    counts.boundary_cuts, counts.boundary_state_variables = _count_boundary_cuts(
        planning, base_dir
    )

    return counts


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


def _build_counts_sections(
    counts: SystemCounts,
) -> list[tuple[str, list[tuple[str, str]]]]:
    """Build the "PLEXOS Elements" and "User Constraints" info sections.

    Returns only the sections that have something to show, so plain PLP /
    IEEE cases (no fuels, commitments or user constraints) are unaffected.
    """
    sections: list[tuple[str, list[tuple[str, str]]]] = []

    # -- PLEXOS-specific element classes (skip zero-count entries) --
    plexos_elems = [
        ("Fuels", counts.fuels),
        ("Flow rights", counts.flow_rights),
        ("Commitments", counts.commitments),
        ("Decision variables", counts.decision_variables),
        ("Boundary cuts", counts.boundary_cuts),
        ("Boundary state vars", counts.boundary_state_variables),
    ]
    plexos_pairs = [(k, str(v)) for k, v in plexos_elems if v > 0]
    if plexos_pairs:
        sections.append(("PLEXOS Elements", plexos_pairs))

    # -- User constraints (total / active / inactive / per family) --
    if counts.user_constraints_total > 0:
        uc_pairs: list[tuple[str, str]] = [
            ("Total", str(counts.user_constraints_total)),
            ("Active", str(counts.user_constraints_active)),
            ("Inactive", str(counts.user_constraints_inactive)),
            ("Inline", str(counts.uc_inline)),
            ("In .pampl", str(counts.uc_in_pampl)),
        ]
        # Per-family rows in stable taxonomy order, then any unknown family.
        seen = set()
        for fam in UC_FAMILY_ORDER:
            n = counts.uc_by_family.get(fam, 0)
            seen.add(fam)
            if n > 0:
                uc_pairs.append((f"  {fam}", str(n)))
        for fam in sorted(counts.uc_by_family):
            if fam not in seen and counts.uc_by_family[fam] > 0:
                uc_pairs.append((f"  {fam}", str(counts.uc_by_family[fam])))
        sections.append(("User Constraints", uc_pairs))

    return sections


def format_counts(
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> str:
    """Return a multi-line string with PLEXOS-element and UC counts."""
    from gtopt_check_json._terminal import render_kv_table  # noqa: PLC0415

    counts = compute_counts(planning, base_dir=base_dir)
    return "\n".join(
        render_kv_table(pairs, title=title)
        for title, pairs in _build_counts_sections(counts)
    )


def print_counts(
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> None:
    """Print the PLEXOS-element and user-constraint count sections."""
    from gtopt_check_json._terminal import print_kv_table  # noqa: PLC0415

    counts = compute_counts(planning, base_dir=base_dir)
    for title, pairs in _build_counts_sections(counts):
        print_kv_table(pairs, title=title)


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

    # -- PLEXOS elements + user-constraint counts (skipped when absent) --
    counts_text = format_counts(planning, base_dir=base_dir)
    if counts_text:
        lines.append(counts_text)

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

    # -- PLEXOS elements + user-constraint counts (skipped when absent) --
    print_counts(planning, base_dir=base_dir)

    # -- Skipped isolated centrals (from plp2gtopt) --
    skipped_pairs = _build_skipped_pairs(planning)
    if skipped_pairs is not None:
        print_kv_table(
            skipped_pairs,
            title=f"Skipped Centrals ({len(skipped_pairs)})",
        )

    # -- Global indicators --
    print_indicators(planning, base_dir=base_dir)
