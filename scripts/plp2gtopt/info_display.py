"""Display PLP case statistics and simulation structure.

Provides :func:`display_plp_info` which parses a PLP input directory and
prints a human-readable summary of:

* system topology (buses, generators, demands, lines, stages, blocks)
* available hydrology classes (from ``plpaflce.dat``)
* simulation-to-hydrology mapping (from ``plpidsim.dat``)
* aperture structure (from ``plpidap2.dat`` / ``plpidape.dat``)
* a suggested ``-y`` / ``-a`` command-line invocation
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from .plp_parser import PLPParser


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------


def _compact_range(values: List[int]) -> str:
    """Render a sorted list of integers as a compact range string.

    Examples
    --------
    >>> _compact_range([51, 52, 53, 54, 55])
    '51-55'
    >>> _compact_range([1, 2, 51, 52])
    '1-2,51-52'
    >>> _compact_range([1, 3, 5])
    '1,3,5'
    """
    if not values:
        return ""
    sorted_vals = sorted(values)
    segments: List[Tuple[int, int]] = []
    start = end = sorted_vals[0]
    for v in sorted_vals[1:]:
        if v == end + 1:
            end = v
        else:
            segments.append((start, end))
            start = end = v
    segments.append((start, end))
    parts = [str(s) if s == e else f"{s}-{e}" for s, e in segments]
    return ",".join(parts)


def _aperture_stage_groups(idap2_parser: Any, num_stages: int) -> str:
    """Summarise aperture sets grouped by contiguous stage ranges."""
    if idap2_parser is None:
        return "  (no plpidap2.dat)"

    # Group consecutive stages that share the same aperture set
    lines: List[str] = []
    prev_key: Optional[str] = None
    group_start: int = 0

    def _flush(start: int, end: int, key: str) -> None:
        stage_range = f"Stage {start}" if start == end else f"Stages {start}-{end}"
        lines.append(f"  {stage_range}: {key}")

    stage_range_end = min(num_stages, idap2_parser.num_stages)
    for stage in range(1, stage_range_end + 1):
        aps = idap2_parser.get_apertures(stage)
        key = f"{len(aps)} apertures  [{_compact_range(aps)}]"
        if key != prev_key:
            if prev_key is not None:
                _flush(group_start, stage - 1, prev_key)
            group_start = stage
            prev_key = key
    if prev_key is not None:
        _flush(group_start, stage_range_end, prev_key)

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def display_plp_info(options: Dict[str, Any]) -> None:
    """Parse *input_dir* and print a PLP case summary to stdout.

    Parameters
    ----------
    options : dict
        Must contain ``"input_dir"`` (Path or str).  Optional keys:
        ``"last_stage"`` (int, -1 = all) and ``"hydrologies"`` (str).
    """
    input_dir = Path(options.get("input_dir", "input"))
    last_stage: int = int(options.get("last_stage", -1))

    print(f"\nPLP Case Summary: {input_dir}")
    print("=" * 60)

    # ---------------------------------------------------------------
    # Parse
    # ---------------------------------------------------------------
    parser = PLPParser({"input_dir": input_dir})
    parser.parse_all()
    pd = parser.parsed_data

    # ---------------------------------------------------------------
    # SYSTEM
    # ---------------------------------------------------------------
    buses = pd.get("bus_parser")
    centrals = pd.get("central_parser")
    demands = pd.get("dem_parser")
    lines = pd.get("line_parser")
    block_parser = pd.get("block_parser")
    stage_parser = pd.get("stage_parser")

    n_buses = buses.num_buses if buses else 0
    n_stages = stage_parser.num_stages if stage_parser else 0
    n_blocks = block_parser.num_blocks if block_parser else 0
    effective_stages = n_stages if last_stage < 0 else min(last_stage, n_stages)

    n_centrals = 0
    type_counts: Dict[str, int] = {}
    if centrals:
        for c in centrals.centrals:
            ctype = c.get("type", "unknown")
            type_counts[ctype] = type_counts.get(ctype, 0) + 1
        n_centrals = len(centrals.centrals)

    type_str = "  ".join(f"{t}: {cnt}" for t, cnt in sorted(type_counts.items()))

    print("\nSYSTEM")
    print(f"  Buses              : {n_buses}")
    print(f"  Generators/plants  : {n_centrals}  ({type_str})")
    if demands:
        print(f"  Demand nodes       : {getattr(demands, 'num_demands', '?')}")
    if lines:
        print(f"  Transmission lines : {getattr(lines, 'num_lines', '?')}")
    print(f"  Stages (total)     : {n_stages}")
    print(f"  Stages (used)      : {effective_stages}  (use -s N to limit)")
    print(f"  Blocks (total)     : {n_blocks}")

    # ---------------------------------------------------------------
    # HYDROLOGY
    # ---------------------------------------------------------------
    aflce_parser = pd.get("aflce_parser")
    n_hydros = 0
    n_centrals_with_flow = 0
    if aflce_parser and aflce_parser.items:
        n_hydros = aflce_parser.items[0].get("num_hydrologies", 0)
        n_centrals_with_flow = len(aflce_parser.items)

    print("\nHYDROLOGY  [plpaflce.dat]")
    if n_hydros:
        print(f"  Centrals with flow : {n_centrals_with_flow}")
        print(
            f"  Hydrology classes  : {n_hydros}  (1-based Fortran indices: 1..{n_hydros})"
        )
    else:
        print("  (plpaflce.dat not found or empty)")

    # ---------------------------------------------------------------
    # SIMULATIONS  (plpidsim.dat)
    # ---------------------------------------------------------------
    idsim = pd.get("idsim_parser")
    print("\nSIMULATIONS")
    if idsim is not None and idsim.num_simulations > 0:
        print(
            f"  [plpidsim.dat]  {idsim.num_simulations} simulations × {idsim.num_stages} stages"
        )
        print("  Hydrology mapping (stage 1):")
        active_hydros: List[int] = []
        seen: set = set()
        for sim_idx in range(idsim.num_simulations):
            h = idsim.get_index(sim_idx, 1)
            if h is not None:
                print(f"    Simulation {sim_idx + 1:>3d}  →  Hydrology {h:>3d}")
                if h not in seen:
                    seen.add(h)
                    active_hydros.append(h)
        active_str = _compact_range(active_hydros)
        print(
            f"  Active hydrology classes : {active_str}  ({len(active_hydros)} total)"
        )
        print(f"  To use all active hydros : -y all     (equivalent: -y {active_str})")
        if active_hydros:
            ex = ",".join(str(h) for h in active_hydros[:3])
            print(f"  To use a subset          : -y {ex}  (first 3 active hydros)")
    else:
        print("  (no plpidsim.dat — -y values are raw 1-based hydrology class indices)")
        if n_hydros:
            print(f"  Available: -y 1 .. -y {n_hydros}   (or: -y all)")

    # ---------------------------------------------------------------
    # APERTURES  (plpidap2.dat / plpidape.dat)
    # ---------------------------------------------------------------
    idap2 = pd.get("idap2_parser")
    idape = pd.get("idape_parser")
    have_ape = idap2 is not None or idape is not None
    print("\nAPERTURES")
    if not have_ape:
        print("  (no plpidap2.dat / plpidape.dat — apertures disabled)")
    else:
        active_set = set(active_hydros) if idsim else set(range(1, n_hydros + 1))

        if idap2 is not None:
            # Collect union across effective stages
            ape_union: set = set()
            for entry in idap2.items:
                if 1 <= entry["stage"] <= effective_stages:
                    for h in entry["indices"]:
                        ape_union.add(h)

            extra = sorted(ape_union - active_set)
            print(f"  [plpidap2.dat]  {idap2.num_stages} stages with aperture data")
            print(_aperture_stage_groups(idap2, effective_stages))
            print(
                f"  Unique hydrology classes (stages 1-{effective_stages}): "
                f"{_compact_range(sorted(ape_union))}  ({len(ape_union)} total)"
            )
            if extra:
                print(
                    f"  Extra hydros (not in active forward set): {_compact_range(extra)}"
                )
                print(
                    f"  → {len(extra)} extra Flow discharge file(s) will be written"
                    " to apertures/Flow/"
                )
            else:
                print("  All aperture hydros are in the active forward set")
                print("  → no extra Flow discharge files needed")

        if idape is not None:
            print(
                f"  [plpidape.dat]  {idape.num_simulations} simulations × "
                f"{idape.num_stages} stages"
            )

    # ---------------------------------------------------------------
    # SUGGESTED COMMAND
    # ---------------------------------------------------------------
    print("\nSUGGESTED COMMAND")
    cmd = [f"plp2gtopt -i {input_dir}"]
    out_name = (
        input_dir.name.replace("plp_", "gtopt_", 1)
        if "plp_" in input_dir.name
        else "gtopt_case"
    )
    cmd.append(f"-o {out_name}")
    cmd.append("-y all")
    if have_ape:
        cmd.append("-a all")
    if last_stage > 0:
        cmd.append(f"-s {last_stage}")
    print("  " + " ".join(cmd))
    print()
