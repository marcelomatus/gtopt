# SPDX-License-Identifier: BSD-3-Clause
"""Read a gtopt output directory + its planning JSON and build the
canonical-schema (Topology, Cells) frames.

Reuses the existing helpers in ``scripts/gtopt_check_output/_reader.py``:
- ``read_table(directory, stem)`` — handles parquet-dataset / single-
  parquet / CSV-shard layouts transparently.
- ``load_planning(json_path)`` — loads a planning JSON.

Each `uid:N` column in the gtopt output frames becomes a long-form
row in the canonical Cells frame.
"""

from __future__ import annotations

from pathlib import Path

import pandas as pd

from gtopt_canonical_feed import (
    Bus,
    Cells,
    Generator,
    Line,
    Topology,
)
from gtopt_canonical_feed.cells import (
    COL_BLOCK,
    COL_BUS_UID,
    COL_DATA_SOURCE,
    COL_DATE_UTC,
    COL_DISPATCH,
    COL_FLOW,
    COL_FLOW_DUAL,
    COL_GEN_UID,
    COL_HOUR,
    COL_LINE_UID,
    COL_LMP,
    COL_LOAD,
    COL_SCENARIO,
    COL_STAGE,
)
from gtopt_check_output._reader import load_planning, read_table
from gtopt_marginal_units._lp_duals import (
    GtoptLpDuals,
    load_gtopt_lp_duals,
)
from gtopt_marginal_units.errors import (
    ExpansionNotSupportedError,
    InputValidationError,
)


# ---------------------------------------------------------------------------
# Planning JSON → Topology
# ---------------------------------------------------------------------------


def topology_from_planning(planning: dict) -> Topology:
    """Build a Topology from a gtopt planning JSON.

    Raises ``ExpansionNotSupportedError`` if any generator or line
    has ``expansion=true`` (or an ``expansion`` block) — see master
    plan §3.2 v1 limitation P0.1.
    """
    system = planning.get("system", {})

    # Buses.
    buses = [
        Bus(
            uid=int(b["uid"]),
            name=str(b.get("name", f"b{b['uid']}")),
            region=b.get("region"),
        )
        for b in system.get("bus_array", [])
    ]
    bus_name_to_uid = {b.name: b.uid for b in buses}

    def _resolve_bus(ref: object) -> int:
        # gtopt accepts either a numeric UID or a name string.
        if isinstance(ref, (int, float)):
            return int(ref)
        if isinstance(ref, str):
            if ref in bus_name_to_uid:
                return bus_name_to_uid[ref]
            # Sometimes the name is just "uid:N".
            if ref.startswith("uid:"):
                return int(ref.split(":", 1)[1])
        raise InputValidationError(f"cannot resolve bus reference: {ref!r}")

    # Generators.
    generators = []
    for g in system.get("generator_array", []):
        if g.get("expansion"):
            raise ExpansionNotSupportedError(
                f"generator {g.get('uid')} has expansion=true; "
                f"v1 does not support expansion (master plan §3.2)."
            )
        gcost = g.get("gcost")
        # gcost may be a number or a file/schedule reference; v1 only
        # handles the numeric case. Non-numeric gcost → declared_MC=None.
        declared_mc = None
        if isinstance(gcost, (int, float)):
            declared_mc = float(gcost)

        # Kind inference: explicit `type` field, else fall back by
        # presence in profile_array / battery_array (handled below).
        kind = str(g.get("type", "thermal")).lower()
        if kind not in ("thermal", "hydro", "battery", "profile"):
            kind = "thermal"

        generators.append(
            Generator(
                uid=int(g["uid"]),
                name=str(g.get("name", f"g{g['uid']}")),
                bus_uid=_resolve_bus(g.get("bus")),
                pmin=float(g.get("pmin", 0.0)),
                pmax=float(g.get("pmax", g.get("capacity", 0.0))),
                declared_MC=declared_mc,
                kind=kind,
                emission_rate=_opt_float(g.get("emission_rate")),
            )
        )

    # Profile generators (renewables) — promote to kind=profile.
    profile_uids = {int(p["uid"]) for p in system.get("generator_profile_array", [])}
    if profile_uids:
        generators = [
            Generator(
                uid=g.uid,
                name=g.name,
                bus_uid=g.bus_uid,
                pmin=g.pmin,
                pmax=g.pmax,
                declared_MC=g.declared_MC,
                kind="profile" if g.uid in profile_uids else g.kind,
                emission_rate=g.emission_rate,
            )
            for g in generators
        ]

    # Battery — kind=battery.
    battery_uids = {int(b["uid"]) for b in system.get("battery_array", [])}
    if battery_uids:
        generators = [
            Generator(
                uid=g.uid,
                name=g.name,
                bus_uid=g.bus_uid,
                pmin=g.pmin,
                pmax=g.pmax,
                declared_MC=g.declared_MC,
                kind="battery" if g.uid in battery_uids else g.kind,
                emission_rate=g.emission_rate,
            )
            for g in generators
        ]

    # Lines.
    lines = []
    for ln in system.get("line_array", []):
        if ln.get("expansion"):
            raise ExpansionNotSupportedError(
                f"line {ln.get('uid')} has expansion=true; v1 does not "
                f"support expansion (master plan §3.2)."
            )
        lines.append(
            Line(
                uid=int(ln["uid"]),
                bus_a_uid=_resolve_bus(ln.get("bus_a")),
                bus_b_uid=_resolve_bus(ln.get("bus_b")),
                tmax_ab=float(ln.get("tmax_ab", ln.get("tmax", 0.0))),
                tmax_ba=float(ln.get("tmax_ba", ln.get("tmax", 0.0))),
                reactance=_opt_float(ln.get("reactance")),
                active=bool(ln.get("active", True)),
            )
        )

    return Topology(buses=buses, generators=generators, lines=lines)


def _opt_float(v: object) -> float | None:
    if v is None:
        return None
    if isinstance(v, (int, float)):
        return float(v)
    return None


# ---------------------------------------------------------------------------
# Output dir → Cells
# ---------------------------------------------------------------------------


def cells_from_gtopt_output(
    output_dir: Path,
) -> Cells:
    """Read the gtopt output directory and produce the canonical
    long-form Cells frames.

    Recognises the standard gtopt layout:
        Generator/generation_sol  → dispatch
        Bus/balance_dual          → lmp
        Line/flowp_sol            → flow
        Line/flowp_cost           → flow_dual (with the §3.2 caveat)
        Demand/load_sol           → load
        Demand/fail_sol           → ens
    Each is read via gtopt_check_output._reader.read_table which
    handles both parquet-dataset and CSV-shard layouts.
    """
    output_dir = Path(output_dir)

    dispatch_wide = read_table(output_dir, "Generator/generation_sol")
    if dispatch_wide is None or dispatch_wide.empty:
        raise InputValidationError(
            f"missing or empty Generator/generation_sol in {output_dir}; "
            "cannot run mode=simulated without dispatch data."
        )
    lmp_wide = read_table(output_dir, "Bus/balance_dual")
    flow_wide = read_table(output_dir, "Line/flowp_sol")
    flow_dual_wide = read_table(output_dir, "Line/flowp_cost")
    load_wide = read_table(output_dir, "Demand/load_sol")
    ens_wide = read_table(output_dir, "Demand/fail_sol")

    return Cells(
        dispatch=_wide_to_long(dispatch_wide, COL_GEN_UID, COL_DISPATCH),
        lmp=_wide_to_long(lmp_wide, COL_BUS_UID, COL_LMP),
        flow=_wide_to_long(flow_wide, COL_LINE_UID, COL_FLOW),
        flow_dual=_wide_to_long(flow_dual_wide, COL_LINE_UID, COL_FLOW_DUAL),
        load=_wide_to_long(load_wide, COL_BUS_UID, COL_LOAD),
        ens=_wide_to_long(ens_wide, COL_BUS_UID, "ens"),
    )


def _wide_to_long(
    wide: pd.DataFrame | None,
    uid_col: str,
    value_col: str,
) -> pd.DataFrame | None:
    """Melt a (scenario, stage, block, uid:1, uid:2, …) gtopt frame
    into long form (cell-key cols + uid_col + value_col).

    Returns None if ``wide`` is None or empty.
    """
    if wide is None or wide.empty:
        return None

    key_cols = [c for c in ("scenario", "stage", "block") if c in wide.columns]
    uid_cols = [c for c in wide.columns if c.startswith("uid:")]
    if not uid_cols:
        return None

    melted = wide.melt(
        id_vars=key_cols,
        value_vars=uid_cols,
        var_name=uid_col,
        value_name=value_col,
    )
    melted[uid_col] = melted[uid_col].str.split(":").str[1].astype(int)
    # Map gtopt's (scenario, stage, block) into the canonical cell key,
    # leaving (date_utc, hour) NA and tagging data_source="simulated".
    out = pd.DataFrame()
    out[COL_SCENARIO] = melted.get("scenario", pd.NA)
    out[COL_STAGE] = melted.get("stage", pd.NA)
    out[COL_BLOCK] = melted.get("block", pd.NA)
    out[COL_DATE_UTC] = pd.NA
    out[COL_HOUR] = pd.NA
    out[COL_DATA_SOURCE] = "simulated"
    out[uid_col] = melted[uid_col]
    out[value_col] = melted[value_col].astype(float)
    return out


# ---------------------------------------------------------------------------
# Convenience entry — load planning + outputs in one call.
# ---------------------------------------------------------------------------


def read_gtopt(
    planning_path: Path, output_dir: Path
) -> tuple[Topology, Cells, GtoptLpDuals]:
    """Read a gtopt planning JSON + its output directory.

    Returns ``(Topology, Cells, GtoptLpDuals)``. The third element is
    optional in the sense that every field on it may be ``None`` when
    gtopt was run without the corresponding ``--write-out`` flag; the
    fast-fail check lives in :func:`_lp_duals.check_write_out_flags`,
    not here.
    """
    planning = load_planning(Path(planning_path))
    topology = topology_from_planning(planning)
    cells = cells_from_gtopt_output(Path(output_dir))
    lp_duals = load_gtopt_lp_duals(Path(output_dir))
    return topology, cells, lp_duals
