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


def topology_from_planning(
    planning: dict,
    output_dir: Path | None = None,
) -> Topology:
    """Build a Topology from a gtopt planning JSON.

    When ``output_dir`` is supplied AND it carries
    ``EmissionSource/rate_sol.parquet``, the per-generator CO2eq factor
    is computed from the LP's **emitted** per-(source, cell) rate
    (averaged over (scenario, stage, block) because the rate is
    constant per source on every jan18-style case I've inspected;
    only varies with explicit per-block fuel switching).  This is the
    LP-authoritative number — captures any per-block adjustment the
    LP made vs the static JSON rate.  Falls back to the static
    ``emission_source_array[*].rate`` field when the parquet is
    absent.

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

    # Aggregate per-generator CO2eq emission factor from the
    # ``emission_source_array`` (one entry per (generator, emission,
    # zone) triple, each carrying a per-MWh rate).  Weight each rate
    # by the zone's emission weight so the final per-gen factor is
    # CO2-equivalent in the same units the zone uses (typically
    # kg CO2eq / MWh on the jan18 CEN bundle).  Falls back to the
    # per-gen ``emission_rate`` JSON field when no sources exist
    # (legacy plexos2gtopt / nrel118_to_gtopt converters that bake
    # the factor into the generator directly).
    zone_weights: dict[int, dict[str, float]] = {
        int(z["uid"]): {
            e["emission"]: float(e["weight"]) for e in z.get("emissions", [])
        }
        for z in system.get("emission_zone_array", [])
    }
    # Per-source LP-emitted rate from ``EmissionSource/rate_sol.parquet``.
    # Prefer this over the static planning-JSON rate when available — it
    # captures any per-block adjustment the LP applied (per-block fuel
    # switching, scale_objective drift, etc.) and is the authoritative
    # value the LP actually used.  Each source's rate is averaged across
    # all (scenario, stage, block) cells; on every case I've inspected
    # the rate is constant per source, so the mean equals every per-cell
    # value.  When the parquet is absent, falls back to the static rate.
    parq_rate: dict[int, float] = {}
    if output_dir is not None:
        parq_path = Path(output_dir) / "EmissionSource" / "rate_sol.parquet"
        if parq_path.exists():
            try:
                # pyarrow is already a hard dep of gtopt_marginal_units; no
                # extra import surface.
                import pyarrow.parquet as pq  # noqa: PLC0415

                rate_df = pq.read_table(parq_path).to_pandas()
                # Mean rate per source uid — collapses the constant
                # per-block dimension to a single representative value.
                grouped = rate_df.groupby("uid")["value"].mean()
                parq_rate = {int(u): float(v) for u, v in grouped.items()}
            except Exception:  # noqa: BLE001
                # Treat any parquet-read failure as "no override" — the
                # static rate is a perfectly valid fallback.
                parq_rate = {}

    gen_emission_co2eq: dict[int, float] = {}
    for src in system.get("emission_source_array", []):
        try:
            g_uid = int(src["generator"])
            z_uid = int(src["zone"])
            emission = str(src["emission"])
            s_uid = int(src.get("uid", -1))
            static_rate = float(src.get("rate", 0.0))
        except (KeyError, TypeError, ValueError):
            continue
        # LP-emitted rate wins when present; static rate is the fallback.
        rate = parq_rate.get(s_uid, static_rate)
        weight = zone_weights.get(z_uid, {}).get(emission, 0.0)
        if weight == 0.0 or rate == 0.0:
            continue
        gen_emission_co2eq[g_uid] = gen_emission_co2eq.get(g_uid, 0.0) + weight * rate

    # Build a lookup: Fuel uid OR Fuel name → derived ``kind`` ("thermal" /
    # "hydro" / "profile") used as a fallback when ``Generator.type`` is
    # missing / ambiguous.  ``Fuel.type`` carries the family tag from
    # plexos2gtopt (``"diesel"``, ``"gas"``, ``"biomasa"``, …); the
    # mapping mirrors the THERMAL_FAMILIES / RENEWABLE_FAMILIES split
    # in ``plexos2gtopt.parsers``.
    fuel_kind_by_ref: dict[object, str] = {}
    for f in system.get("fuel_array", []):
        ftype = str(f.get("type", "")).lower()
        if not ftype:
            continue
        if ftype in ("hydro",):
            fkind = "hydro"
        elif ftype in ("solar", "wind", "eolic"):
            fkind = "profile"
        elif ftype in (
            "diesel",
            "fuel_oil",
            "fuel-oil",
            "gas",
            "glp",
            "biomasa",
            "biomass",
            "biogas",
            "carbon",
            "coal",
            "otros",
            "geothermal",
        ):
            # Anything dispatchable / combustible → thermal pool.
            fkind = "thermal"
        else:
            continue
        # Index by both uid (numeric) and name (string) — Generator's
        # ``fuel`` field is OptSingleId which may carry either form.
        try:
            fuel_kind_by_ref[int(f["uid"])] = fkind
        except (KeyError, TypeError, ValueError):
            pass
        if f.get("name"):
            fuel_kind_by_ref[str(f["name"])] = fkind

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

        kind = _classify_kind(g, fuel_kind_by_ref)

        uid = int(g["uid"])
        # Prefer the per-gen JSON ``emission_rate`` when set; otherwise
        # fall back to the CO2eq factor derived from emission_source_array.
        emit_jsn = _opt_float(g.get("emission_rate"))
        emit_eff = emit_jsn if emit_jsn is not None else gen_emission_co2eq.get(uid)

        generators.append(
            Generator(
                uid=uid,
                name=str(g.get("name", f"g{g['uid']}")),
                bus_uid=_resolve_bus(g.get("bus")),
                pmin=_scalar_or_max(g.get("pmin"), 0.0),
                pmax=_scalar_or_max(g.get("pmax", g.get("capacity")), 0.0),
                declared_MC=declared_mc,
                kind=kind,
                emission_rate=emit_eff,
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

    # NOTE: do NOT cross-reference ``battery_array`` uids here.
    # ``Battery`` and ``Generator`` are SEPARATE element classes in the
    # gtopt schema, each with its own uid namespace.  A coincidental
    # collision (e.g. Generator uid=1 = ABANICO hydro AND Battery
    # uid=1 = some BESS) previously caused ABANICO to be reclassified
    # as ``kind=battery``.  Real batteries are tracked separately via
    # the ``battery_array`` LP class and don't appear in
    # ``generator_array`` at all — the only generators that LOOK
    # battery-shaped are the BAT_*_gen synthetic wraps used by
    # plexos2gtopt to expose a BESS's discharge column to the LP
    # bidding stack.  Those are tagged ``thermal`` by the converter
    # (heat_rate=0, no fuel), so the ``type``-based parser above sees
    # ``kind=thermal`` — correct, as their direct emission is zero and
    # the consequential MOER picker walks past them naturally.

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


def _classify_kind(g: dict, fuel_kind_by_ref: dict[object, str]) -> str:
    """Map a Generator's plexos2gtopt-style ``type`` tag (and, as a
    fallback, its ``fuel`` reference) onto one of the 4 buckets the
    marginal-unit classifier understands: ``"thermal"``, ``"hydro"``,
    ``"battery"``, ``"profile"``.

    plexos2gtopt emits hierarchical types (``"thermal:diesel"``,
    ``"renewable:hydro"``, ``"renewable:solar"``, …) under the
    THERMAL_FAMILIES / RENEWABLE_FAMILIES taxonomy in
    ``scripts/plexos2gtopt/parsers.py``.  Mapping:

      * ``thermal:*``           → "thermal"   (any combustion)
      * ``renewable:hydro``     → "hydro"     (storage value)
      * ``renewable:solar/wind``→ "profile"   (intermittent)
      * ``renewable:biomass/biogas/geothermal`` → "thermal"
          (dispatchable, emits via combustion / steam vents — for
          attribution they behave like thermal even though the energy
          source is "renewable")
      * bare ``renewable``      → fallback to fuel.type lookup, then
                                  "profile" (zero-emission default)
      * bare ``thermal``        → fallback to fuel.type lookup, then
                                  "thermal"
      * ``battery``             → "battery"
      * anything else / unset   → fallback to fuel.type lookup, then
                                  "thermal" (safe LP default)

    Fuel-type fallback: when the Generator's ``type`` field is missing
    or carries only the family ("thermal" / "renewable") with no
    sub-tag, look up the linked ``fuel`` element (uid OR name form,
    OptSingleId variant) and use its ``Fuel.type`` field via the
    pre-built ``fuel_kind_by_ref`` map.  Catches converters that emit
    only a bare family on Generator but a rich sub-family on Fuel.
    """
    raw_type = str(g.get("type", "")).lower()
    family, _, sub = raw_type.partition(":")

    def _from_fuel() -> str | None:
        ref = g.get("fuel")
        if ref is None:
            return None
        # OptSingleId variant — Uid (int) or Name (str), possibly nested
        # in a single-key dict like {"uid": 4} or {"name": "Diesel_X"}.
        if isinstance(ref, dict):
            for key in ("uid", "name"):
                v = ref.get(key)
                if v is not None and v in fuel_kind_by_ref:
                    return fuel_kind_by_ref[v]
            return None
        if isinstance(ref, (int, str)) and ref in fuel_kind_by_ref:
            return fuel_kind_by_ref[ref]
        return None

    if family == "thermal":
        if sub:
            return "thermal"
        return _from_fuel() or "thermal"
    if family == "renewable":
        if sub == "hydro":
            return "hydro"
        if sub in ("solar", "wind", "eolic"):
            return "profile"
        if sub in ("biomass", "biomasa", "biogas", "geothermal"):
            return "thermal"
        # Unknown / bare renewable — try fuel, default profile.
        return _from_fuel() or "profile"
    if family in ("hydro", "battery", "profile"):
        return family
    # Unknown / missing type — try fuel, default thermal.
    return _from_fuel() or "thermal"


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
    # gtopt emits ``Line/flow_sol`` as the signed net flow per the
    # unified API (positive ⇒ A→B, negative ⇒ B→A) and
    # ``Line/flow_cost`` as the sign-based active-direction reduced
    # cost.  Older gtopt builds emit the directional pair
    # ``Line/flowp_sol`` / ``Line/flown_sol``; when ``flow_sol`` is
    # absent we fall back to the merge of those two.
    flow_signed_wide = read_table(output_dir, "Line/flow_sol")
    flow_cost_wide = read_table(output_dir, "Line/flow_cost")
    if flow_signed_wide is None:
        flow_pos_wide = read_table(output_dir, "Line/flowp_sol")
        flow_neg_wide = read_table(output_dir, "Line/flown_sol")
        flow_frame = _merge_signed_flow(flow_pos_wide, flow_neg_wide)
        if flow_cost_wide is None:
            flow_cost_wide = read_table(output_dir, "Line/flowp_cost")
    else:
        flow_frame = _wide_to_long(flow_signed_wide, COL_LINE_UID, COL_FLOW)
    load_wide = read_table(output_dir, "Demand/load_sol")
    ens_wide = read_table(output_dir, "Demand/fail_sol")

    return Cells(
        dispatch=_wide_to_long(dispatch_wide, COL_GEN_UID, COL_DISPATCH),
        lmp=_wide_to_long(lmp_wide, COL_BUS_UID, COL_LMP),
        flow=flow_frame,
        flow_dual=_wide_to_long(flow_cost_wide, COL_LINE_UID, COL_FLOW_DUAL),
        load=_wide_to_long(load_wide, COL_BUS_UID, COL_LOAD),
        ens=_wide_to_long(ens_wide, COL_BUS_UID, "ens"),
    )


def _merge_signed_flow(
    flow_pos_wide: pd.DataFrame | None,
    flow_neg_wide: pd.DataFrame | None,
) -> pd.DataFrame | None:
    """Combine ``flowp_sol`` and ``flown_sol`` into a single signed flow
    frame (``flowp - flown``).

    Both inputs may be in long or wide layout; ``_wide_to_long`` handles
    both.  When either side is missing, the other is returned with sign
    convention preserved (positive-only → returned as-is; negative-only
    → negated and returned).  Both missing → ``None``.
    """
    pos = _wide_to_long(flow_pos_wide, COL_LINE_UID, COL_FLOW)
    neg = _wide_to_long(flow_neg_wide, COL_LINE_UID, COL_FLOW)
    if pos is None and neg is None:
        return None
    if neg is None:
        return pos
    if pos is None:
        neg = neg.copy()
        neg[COL_FLOW] = -neg[COL_FLOW]
        return neg

    key_cols = [
        COL_SCENARIO,
        COL_STAGE,
        COL_BLOCK,
        COL_DATE_UTC,
        COL_HOUR,
        COL_DATA_SOURCE,
        COL_LINE_UID,
    ]
    merged = pos.merge(neg.rename(columns={COL_FLOW: "_neg"}), on=key_cols, how="outer")
    merged[COL_FLOW] = merged[COL_FLOW].fillna(0.0) - merged["_neg"].fillna(0.0)
    merged = merged.drop(columns="_neg")
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
    topology = topology_from_planning(planning, output_dir=Path(output_dir))
    cells = cells_from_gtopt_output(Path(output_dir))
    lp_duals = load_gtopt_lp_duals(Path(output_dir))
    return topology, cells, lp_duals
