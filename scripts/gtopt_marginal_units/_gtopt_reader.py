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
                pmin=_scalar_or_max(g.get("pmin"), 0.0),
                pmax=_scalar_or_max(g.get("pmax", g.get("capacity")), 0.0),
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
                tmax_ab=_scalar_or_max(ln.get("tmax_ab", ln.get("tmax")), 0.0),
                tmax_ba=_scalar_or_max(ln.get("tmax_ba", ln.get("tmax")), 0.0),
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


def _scalar_or_max(v: object, default: float = 0.0) -> float:
    """Reduce a gtopt ``TBRealFieldSched`` variant to a representative
    scalar suitable for Topology metadata.

    Accepted shapes:
      * ``int`` / ``float``         → cast to float
      * ``list[float]``             → ``max(.)``
      * ``list[list[float]]``       → ``max`` over the flattened matrix
      * ``str`` (FileSched parquet) → ``default`` (we don't open the
        parquet here; the LP solution carries the actual per-block
        capacity used during dispatch, so the metadata pmax is only a
        sanity bound).
      * Anything else / ``None``    → ``default``
    """
    if v is None:
        return default
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, list):
        flat: list[float] = []
        for x in v:
            if isinstance(x, (int, float)):
                flat.append(float(x))
            elif isinstance(x, list):
                flat.extend(float(y) for y in x if isinstance(y, (int, float)))
        return max(flat) if flat else default
    return default


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
    # ``Line/flowp_sol`` is the positive-direction primal; ``flown_sol`` is
    # the negative-direction primal.  In the optimal LP one of them is zero
    # for every (line, cell), so the line's signed net flow is
    # ``flowp - flown`` and its absolute magnitude (used for saturation
    # detection in main.py) is ``max(flowp, flown)``.  We merge both into
    # the canonical ``flow`` frame as the SIGNED net flow ``flowp - flown``,
    # PLUS a ``used_capacity = |flowp| + |flown| + loss`` column so the
    # saturation test in main.py matches gtopt's actual cap row
    # (``flowp + flown + Σ loss_seg ≤ tmax``) — a line at 99 % flow but
    # +1 % losses is at the cap and should split the topology graph.
    flow_pos_wide = read_table(output_dir, "Line/flowp_sol")
    flow_neg_wide = read_table(output_dir, "Line/flown_sol")
    flow_loss_wide = read_table(output_dir, "Line/loss_sol")
    flow_dual_wide = read_table(output_dir, "Line/flowp_cost")
    load_wide = read_table(output_dir, "Demand/load_sol")
    ens_wide = read_table(output_dir, "Demand/fail_sol")

    return Cells(
        dispatch=_wide_to_long(dispatch_wide, COL_GEN_UID, COL_DISPATCH),
        lmp=_wide_to_long(lmp_wide, COL_BUS_UID, COL_LMP),
        flow=_merge_signed_flow(flow_pos_wide, flow_neg_wide, flow_loss_wide),
        flow_dual=_wide_to_long(flow_dual_wide, COL_LINE_UID, COL_FLOW_DUAL),
        load=_wide_to_long(load_wide, COL_BUS_UID, COL_LOAD),
        ens=_wide_to_long(ens_wide, COL_BUS_UID, "ens"),
    )


def _merge_signed_flow(
    flow_pos_wide: pd.DataFrame | None,
    flow_neg_wide: pd.DataFrame | None,
    flow_loss_wide: pd.DataFrame | None = None,
) -> pd.DataFrame | None:
    """Combine ``flowp_sol`` + ``flown_sol`` (+ optional ``loss_sol``) into
    a long-form frame with two value columns:

    * ``flow`` — signed net flow ``flowp - flown`` (downstream consumers
      that want the magnitude take ``|flow|``)
    * ``used_capacity`` — ``|flowp| + |flown| + loss`` (matches the LP cap
      row ``flowp + flown + Σ loss_seg ≤ tmax``).  When ``loss_sol`` is
      missing, falls back to ``|flowp| + |flown|``.

    When either ``flowp_sol`` / ``flown_sol`` is missing, the other is
    returned with sign convention preserved (positive-only → returned
    as-is; negative-only → negated and returned).  Both missing → ``None``.
    """
    pos = _wide_to_long(flow_pos_wide, COL_LINE_UID, COL_FLOW)
    neg = _wide_to_long(flow_neg_wide, COL_LINE_UID, COL_FLOW)
    loss = _wide_to_long(flow_loss_wide, COL_LINE_UID, COL_FLOW)
    if pos is None and neg is None:
        return None

    key_cols = [
        COL_SCENARIO,
        COL_STAGE,
        COL_BLOCK,
        COL_DATE_UTC,
        COL_HOUR,
        COL_DATA_SOURCE,
        COL_LINE_UID,
    ]

    if neg is None:
        merged = pos.copy()
        merged["used_capacity"] = merged[COL_FLOW].abs()
    elif pos is None:
        merged = neg.copy()
        merged["used_capacity"] = merged[COL_FLOW].abs()
        merged[COL_FLOW] = -merged[COL_FLOW]
    else:
        merged = pos.merge(
            neg.rename(columns={COL_FLOW: "_neg"}), on=key_cols, how="outer"
        )
        pos_v = merged[COL_FLOW].fillna(0.0)
        neg_v = merged["_neg"].fillna(0.0)
        merged[COL_FLOW] = pos_v - neg_v
        merged["used_capacity"] = pos_v.abs() + neg_v.abs()
        merged = merged.drop(columns="_neg")

    if loss is not None:
        merged = merged.merge(
            loss.rename(columns={COL_FLOW: "_loss"}), on=key_cols, how="left"
        )
        merged["used_capacity"] = merged["used_capacity"] + merged["_loss"].fillna(0.0)
        merged = merged.drop(columns="_loss")

    return merged.reset_index(drop=True)


def _wide_to_long(
    wide: pd.DataFrame | None,
    uid_col: str,
    value_col: str,
) -> pd.DataFrame | None:
    """Melt a gtopt output frame into the canonical long form
    (cell-key cols + uid_col + value_col).

    Accepts both gtopt output layouts:

    * ``output_layout=wide`` (legacy): one ``uid:N`` column per element;
      we melt and split the column name to recover the uid.
    * ``output_layout=long`` (default since 2026-05-19): the frame is
      already long — we just rename ``uid``/``value`` to the caller's
      names.

    Returns None if ``wide`` is None or empty.
    """
    if wide is None or wide.empty:
        return None

    key_cols = [c for c in ("scenario", "stage", "block") if c in wide.columns]

    # Long-form sniff (matches the C++ writer in output_context.cpp).
    if "uid" in wide.columns and "value" in wide.columns:
        melted = wide[key_cols + ["uid", "value"]].rename(
            columns={"uid": uid_col, "value": value_col}
        )
        melted[uid_col] = melted[uid_col].astype(int)
    else:
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
