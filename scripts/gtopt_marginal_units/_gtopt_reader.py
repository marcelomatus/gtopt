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

import logging
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

_LOG = logging.getLogger("gtopt_marginal_units")


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
            except Exception as exc:  # noqa: BLE001
                # Parquet read failed — fall back to static per-source
                # ``rate`` field on emission_source_array.  Don't swallow
                # silently: emission attribution will use a different
                # (possibly outdated) number than the LP actually emitted.
                _LOG.warning(
                    "EmissionSource/rate_sol.parquet read failed (%s); "
                    "falling back to static rates from "
                    "emission_source_array. Per-block fuel switching / "
                    "scale_objective drift will NOT be captured.",
                    exc,
                )
                parq_rate = {}

    gen_emission_co2eq: dict[int, float] = {}
    n_src_malformed = 0
    n_src_weight_zero = 0
    n_src_rate_zero = 0
    n_src_total = 0
    for src in system.get("emission_source_array", []):
        n_src_total += 1
        try:
            g_uid = int(src["generator"])
            z_uid = int(src["zone"])
            emission = str(src["emission"])
            s_uid = int(src.get("uid", -1))
            static_rate = float(src.get("rate", 0.0))
        except (KeyError, TypeError, ValueError) as exc:
            # A malformed EmissionSource row vanishes from CO2 attribution.
            # Log first 3 to help diagnose schema mismatches; aggregate
            # the rest into a final summary.
            if n_src_malformed < 3:
                _LOG.warning(
                    "emission_source_array row malformed (%s): %r — "
                    "row dropped from CO2 attribution.",
                    exc,
                    src,
                )
            n_src_malformed += 1
            continue
        # LP-emitted rate wins when present; static rate is the fallback.
        rate = parq_rate.get(s_uid, static_rate)
        weight = zone_weights.get(z_uid, {}).get(emission, 0.0)
        if weight == 0.0:
            # Legitimate "don't track this pollutant in this zone" signal;
            # count for visibility but don't warn (designed behaviour).
            n_src_weight_zero += 1
            continue
        if rate == 0.0:
            # A real EmissionSource with rate=0 either means the LP
            # produced zero for this (g, e, z) cell or the static rate
            # was never set.  Count for visibility.
            n_src_rate_zero += 1
            continue
        gen_emission_co2eq[g_uid] = gen_emission_co2eq.get(g_uid, 0.0) + weight * rate
    if n_src_total > 0:
        _LOG.info(
            "emission_source_array: %d rows total; %d aggregated, "
            "%d malformed, %d weight=0 (zone untracked), %d rate=0",
            n_src_total,
            n_src_total - n_src_malformed - n_src_weight_zero - n_src_rate_zero,
            n_src_malformed,
            n_src_weight_zero,
            n_src_rate_zero,
        )
    if n_src_malformed > 3:
        _LOG.warning(
            "emission_source_array: %d additional malformed rows not "
            "logged individually; CO2 attribution incomplete. Review "
            "the planning JSON schema.",
            n_src_malformed - 3,
        )

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
        except (KeyError, TypeError, ValueError) as exc:
            # A fuel with non-castable uid silently drops its uid-based
            # lookup but keeps the name-based one.  Log so a downstream
            # generator whose ``fuel`` field is the uid (not name) and
            # falls back to ``"thermal"`` default kind is traceable.
            _LOG.warning(
                "fuel %r has non-castable uid (%s); only name-based "
                "lookup will work for generators referencing it by uid.",
                f.get("name") or "<unnamed>",
                exc,
            )
        if f.get("name"):
            fuel_kind_by_ref[str(f["name"])] = fkind

    # Multi-pollutant CO2eq lookup from the factored data:
    #
    #   rate_g [tCO2eq / MWh] = heat_rate_g       [fuel-unit / MWh]
    #                        × heat_content_f    [GJ / fuel-unit]
    #                        × Σ_e ( combustion_{f,e} [tCO2 / GJ]
    #                              × weight_{z,e}   [tCO2eq / tCO2] )
    #
    # This keeps the per-pollutant decomposition intact: changing the
    # zone's GWP horizon (e.g. AR5 → AR6, GWP100 → GWP20) or adding a
    # pollutant requires no edits to ``Fuel.emission_factors`` or per-gen
    # JSON fields — the multiplication re-runs at read time.  Matches the
    # C++ ``EmissionSourceLP`` α factor in #522, so a recipe value and the
    # LP's per-block emission attribution agree by construction.
    #
    # Resolution order for a generator's effective CO2eq rate (per #522
    # convention used by the LP-side reader):
    #   1.  Sum of all ``emission_source_array`` rows × zone weight
    #       (LP-emitted ``EmissionSource/rate_sol.parquet`` overrides
    #       static rate per src — already populated above).
    #   2.  Per-gen JSON override ``Generator.emission_rate`` (lossy
    #       scalar; only honoured if explicitly set — kept for back-
    #       compat with legacy converters that bake the rate in).
    #   3.  Derived from ``heat_rate × heat_content × Σ combustion × weight``
    #       (this block).  Path 3 is the default for plexos2gtopt and
    #       plp2gtopt output that ships the factored multi-pollutant
    #       data without per-gen aggregation.
    zone_pollutant_weights: dict[str, float] = {}
    for z in system.get("emission_zone_array", []):
        for e in z.get("emissions", []) or []:
            name = str(e.get("emission", ""))
            if not name:
                continue
            zone_pollutant_weights[name] = zone_pollutant_weights.get(
                name, 0.0
            ) + float(e.get("weight", 0.0) or 0.0)

    fuel_co2eq_per_unit: dict[object, float] = {}
    if zone_pollutant_weights:
        for f in system.get("fuel_array", []):
            hc = float(f.get("heat_content", 0.0) or 0.0)
            if hc <= 0.0:
                continue
            agg = 0.0
            for ef in f.get("emission_factors", []) or []:
                if not isinstance(ef, dict):
                    continue
                w = zone_pollutant_weights.get(str(ef.get("emission", "")), 0.0)
                if w == 0.0:
                    continue
                agg += float(ef.get("combustion", 0.0) or 0.0) * w
            if agg <= 0.0:
                continue
            co2eq_per_fuel_unit = agg * hc  # tCO2eq / fuel-unit
            try:
                fuel_co2eq_per_unit[int(f["uid"])] = co2eq_per_fuel_unit
            except (KeyError, TypeError, ValueError):
                pass
            if f.get("name"):
                fuel_co2eq_per_unit[str(f["name"])] = co2eq_per_fuel_unit

    # Generators.
    generators = []
    n_thermal_no_emission_rate = 0
    sample_thermal_no_emission: list[str] = []
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
        is_cogen = _is_cogen(g)

        uid = int(g["uid"])
        # Resolve the per-gen CO2eq emission rate in #522 order:
        #   1. ``emission_source_array`` aggregate × zone weight
        #      (gen_emission_co2eq[uid] — already populated above).
        #   2. JSON override ``Generator.emission_rate`` (lossy scalar
        #      kept for back-compat with converters that bake the
        #      aggregate in — e.g. ``--only-emissions``).
        #   3. Derive from heat_rate × fuel.heat_content × Σ_e (combustion_e
        #      × zone_weight_e).  Default path for plexos2gtopt /
        #      plp2gtopt cost-mode JSONs (which carry the factored
        #      multi-pollutant data but not the aggregate).
        emit_eff = gen_emission_co2eq.get(uid)
        if emit_eff is None:
            emit_jsn = _opt_float(g.get("emission_rate"))
            if emit_jsn is not None:
                emit_eff = emit_jsn
        if emit_eff is None and fuel_co2eq_per_unit:
            hr = g.get("heat_rate")
            if isinstance(hr, (int, float)) and float(hr) > 0.0:
                fuel_ref = g.get("fuel")
                co2eq_per_unit = None
                if isinstance(fuel_ref, (int, float)):
                    co2eq_per_unit = fuel_co2eq_per_unit.get(int(fuel_ref))
                elif isinstance(fuel_ref, str):
                    co2eq_per_unit = fuel_co2eq_per_unit.get(fuel_ref)
                if co2eq_per_unit is not None and co2eq_per_unit > 0.0:
                    emit_eff = float(co2eq_per_unit) * float(hr)

        gen_name = str(g.get("name", f"g{g['uid']}"))
        # A real thermal that fell through all three paths is a missing
        # data point — not a silent zero-emission renewable.  Count it
        # for the end-of-function summary so the operator knows N
        # thermals are unattributed to CO2.
        if kind == "thermal" and not is_cogen and (emit_eff is None or emit_eff == 0.0):
            n_thermal_no_emission_rate += 1
            if len(sample_thermal_no_emission) < 5:
                sample_thermal_no_emission.append(gen_name)
        generators.append(
            Generator(
                uid=uid,
                name=gen_name,
                bus_uid=_resolve_bus(g.get("bus")),
                pmin=_scalar_or_max(g.get("pmin"), 0.0),
                pmax=_scalar_or_max(g.get("pmax", g.get("capacity")), 0.0),
                declared_MC=declared_mc,
                kind=kind,
                emission_rate=emit_eff,
                is_cogen=is_cogen,
                lossfactor=_opt_float(g.get("lossfactor")),
            )
        )

    # Phase 2 — mode-variant inheritance pass.  A CSV-only orphan
    # recovered by plexos2gtopt's ``_recover_csv_only_thermals`` carries
    # ``[gtopt-meta mode_variant=secondary inherits_emission_from=<parent>]``
    # in its description.  When such an orphan ends up without an
    # emission_rate after paths 1/2/3 (most of them have no fuel /
    # heat_rate of their own), look up the parent in the just-built
    # generator list and inherit.  Rare in practice (orphans have
    # pmax=0 so they almost never dispatch) but lands the right value
    # for the cells where the LP basis elects an orphan at a tie-break.
    from gtopt_shared.description_meta import (  # noqa: PLC0415
        parse_meta,
    )

    gen_by_name = {g.name: g for g in generators}
    n_inherited = 0
    for idx, g_struct in enumerate(generators):
        if g_struct.emission_rate is not None and g_struct.emission_rate != 0.0:
            continue
        # Look up the original JSON entry to read description meta.
        json_entry = next(
            (
                jg
                for jg in system.get("generator_array", [])
                if int(jg["uid"]) == g_struct.uid
            ),
            None,
        )
        if json_entry is None:
            continue
        meta = parse_meta(json_entry.get("description"))
        parent_name = meta.get("inherits_emission_from")
        if not parent_name or not isinstance(parent_name, str):
            continue
        parent = gen_by_name.get(parent_name)
        if parent is None or parent.emission_rate is None:
            continue
        # Inherit the parent's emission_rate.  Mutate via re-construction
        # because Generator is frozen.
        generators[idx] = Generator(
            uid=g_struct.uid,
            name=g_struct.name,
            bus_uid=g_struct.bus_uid,
            pmin=g_struct.pmin,
            pmax=g_struct.pmax,
            declared_MC=g_struct.declared_MC,
            kind=g_struct.kind,
            emission_rate=parent.emission_rate,
            is_cogen=g_struct.is_cogen,
            lossfactor=g_struct.lossfactor,
        )
        n_inherited += 1
        # Decrement the "no emission data" counter since this orphan
        # now has an inherited value.
        if g_struct.kind == "thermal" and not g_struct.is_cogen:
            n_thermal_no_emission_rate = max(0, n_thermal_no_emission_rate - 1)

    if n_inherited > 0:
        _LOG.info(
            "topology: %d thermal generators inherited emission_rate "
            "from a longest-prefix sibling via description meta "
            "(mode_variant=secondary).",
            n_inherited,
        )

    if n_thermal_no_emission_rate > 0:
        _LOG.warning(
            "topology: %d thermal generators have no emission_rate "
            "(none of emission_source_array, Generator.emission_rate, "
            "or Fuel.emission_factors × heat_rate gave a value). "
            "These will be treated as zero-emission in the recipe — "
            "review fuel data. Sample: %s%s",
            n_thermal_no_emission_rate,
            ", ".join(sample_thermal_no_emission),
            "" if n_thermal_no_emission_rate <= 5 else ", ...",
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
                is_cogen=g.is_cogen,
                lossfactor=g.lossfactor,
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


def demand_fail_cost_by_bus(
    planning: dict,
    global_fallback: float,
) -> dict[int, float]:
    """Build a per-bus demand_fail_cost lookup.

    Matches the gtopt C++ resolution order
    (``source/demand_lp.cpp``): per-Demand ``fcost`` wins; falls back
    to ``model_options.demand_fail_cost``; falls back to the supplied
    CLI ``global_fallback``.

    When multiple Demand elements share a bus, the lowest ``fcost``
    wins — that's the rationing-binding price the LP sees (cheapest
    demand to ration first sets the marginal).

    Non-scalar ``fcost`` (FileSched / per-block schedule) is treated
    as ``None`` here: the recipe lookup needs a scalar.  In that case
    the bus inherits the model-options / CLI fallback.  A per-block
    fcost recipe would need a per-cell dict and a real scalar
    resolution at recipe build time; the v1 simplification accepts
    the lossier per-cell answer.
    """
    sys_ = planning.get("system", {}) or {}
    bus_uid_by_name = {b["name"]: b["uid"] for b in sys_.get("bus_array", [])}

    def _resolve(ref: object) -> int | None:
        if isinstance(ref, (int, float)):
            return int(ref)
        if isinstance(ref, str) and ref in bus_uid_by_name:
            return int(bus_uid_by_name[ref])
        return None

    # Global fallback: model_options.demand_fail_cost wins over CLI when
    # set (the LP would use it too).
    model_opts = (planning.get("options") or {}).get("model_options") or {}
    options_dfc = model_opts.get("demand_fail_cost")
    if isinstance(options_dfc, (int, float)):
        global_fallback = float(options_dfc)

    by_bus: dict[int, float] = {}
    for d in sys_.get("demand_array", []) or []:
        bus_uid = _resolve(d.get("bus"))
        if bus_uid is None:
            continue
        fcost = d.get("fcost")
        if not isinstance(fcost, (int, float)):
            continue  # FileSched or missing — bus inherits global fallback
        v = float(fcost)
        cur = by_bus.get(bus_uid)
        # Min across demands at the same bus — cheapest-to-ration wins.
        by_bus[bus_uid] = v if cur is None else min(cur, v)

    # Fill remaining buses with the global fallback so every bus has a
    # value.  Callers can also use ``.get(bus, global_fallback)`` but
    # the explicit fill makes the per-bus lookup O(1) without branching.
    for b in sys_.get("bus_array", []):
        by_bus.setdefault(int(b["uid"]), float(global_fallback))
    return by_bus


def _opt_float(v: object) -> float | None:
    if v is None:
        return None
    if isinstance(v, (int, float)):
        return float(v)
    return None


# ----------------------------------------------------------------------
# CEN cogen reference list — loaded once and cached.
# ----------------------------------------------------------------------
#
# The bundled CSV at ``share/gtopt/cogen/cen_chile_cogen.csv`` carries
# explicit cogen identifications synthesised from three sources:
#
#   1. CEN-SIP ``unidades_generadoras`` (``nombre_tecnologia ==
#      'Cogeneración - *'``) — the formal CEN registry.
#   2. ``share/gtopt/emissions/cen_chile.json`` ``generator_overrides``
#      where ``type`` starts with ``thermal:cogen``.
#   3. Industrial-cogen central-name PREFIXES verified from CEN-SIP
#      cross-reference + Informe CEN private docs (pulp-mill black-
#      liquor cogen: CMPC, CELCO, ARAUCO, …; sulfur cogen: PAS_*; etc.).
#
# Loading is best-effort: if the CSV is missing or unreadable, ``_is_cogen``
# falls back to the inline type/fuel/name heuristic.
_COGEN_REFERENCE_NAMES: set[str] | None = None
_COGEN_REFERENCE_PREFIXES: tuple[str, ...] = ()


def _load_cogen_reference() -> tuple[set[str], tuple[str, ...]]:
    """Load (exact-names, prefix-tuple) from the bundled CEN cogen CSV.

    Cached after first call. Returns ``(set(), ())`` if the CSV is
    missing / unreadable — caller falls back to the inline heuristic.
    """
    global _COGEN_REFERENCE_NAMES, _COGEN_REFERENCE_PREFIXES
    if _COGEN_REFERENCE_NAMES is not None:
        return _COGEN_REFERENCE_NAMES, _COGEN_REFERENCE_PREFIXES
    names: set[str] = set()
    prefixes: list[str] = []
    # Try the canonical bundled path first, then a couple of
    # repo-root / pkg-relative fallbacks for source-tree runs.
    from pathlib import Path  # noqa: PLC0415

    candidates = [
        Path("/home/marce/git/gtopt/share/gtopt/cogen/cen_chile_cogen.csv"),
        Path(__file__).resolve().parents[3]
        / "share"
        / "gtopt"
        / "cogen"
        / "cen_chile_cogen.csv",
    ]
    loaded_from: Path | None = None
    last_err: Exception | None = None
    for path in candidates:
        if not path.is_file():
            continue
        try:
            import csv  # noqa: PLC0415

            with open(path, newline="", encoding="utf-8") as f:
                reader = csv.DictReader(f)
                for row in reader:
                    name = str(row.get("name_upper", "")).strip().upper()
                    if not name:
                        continue
                    source = str(row.get("source", "")).lower()
                    if source.startswith("pattern_prefix:"):
                        prefixes.append(name)
                    else:
                        names.add(name)
            loaded_from = path
            break
        except OSError as exc:
            last_err = exc
            continue
    if loaded_from is None:
        _LOG.warning(
            "cogen reference CSV not loaded (checked %d paths; last "
            "error: %s).  Falling back to inline heuristic — units "
            "whose Generator.type starts with 'thermal:cogen' will "
            "be marked cogen; all other dispatchable units are "
            "treated as non-cogen.  Some CEN biomass / sulfuric-acid "
            "plants may be miscategorised in the consequential MOER.",
            len(candidates),
            last_err,
        )
    else:
        _LOG.info(
            "cogen reference CSV loaded from %s (%d exact names, %d pattern prefixes)",
            loaded_from,
            len(names),
            len(prefixes),
        )
    _COGEN_REFERENCE_NAMES = names
    _COGEN_REFERENCE_PREFIXES = tuple(prefixes)
    return _COGEN_REFERENCE_NAMES, _COGEN_REFERENCE_PREFIXES


def _is_cogen(g: dict) -> bool:
    """Identify self-dispatching cogeneration units.

    Cogen units in CEN are MustRun (``declared_MC=0``) with a small
    leakage emission, and must NOT appear as backfill marginal units in
    the merit-order walk-up.  Lookup is layered, most-explicit first:

      1. **Explicit flag** — ``Generator.is_cogen == true`` on the
         planning JSON (set by ``plexos2gtopt`` / ``plp2gtopt`` when
         the converter has direct cogen info).
      2. **Explicit type tag** — ``Generator.type`` starts with
         ``thermal:cogen`` (the canonical convention used by
         ``share/gtopt/emissions/cen_chile.json`` overrides; applied
         by both converters when ``--emissions-file`` carries the
         ``generator_overrides`` section).
      3. **CEN reference list** — bundled
         ``share/gtopt/cogen/cen_chile_cogen.csv`` (synthesised from
         CEN-SIP ``unidades_generadoras`` + Informe-CEN industrial
         patterns).  Match by exact upper-case name OR by central
         name starting with one of the known cogen prefixes.
      4. **Heuristic fallback** — biomass / biogas / geothermal raw
         type, ``thermal:otros`` raw type, ``Otros_*`` / ``Noracid``
         fuel name, ``PAS_*`` central-name prefix.

    NOTE: ``thermal:gas`` / ``thermal:diesel`` segments of CCGT plants
    (``ATA_CC1_TGA_GNL``, ``MEJILLONES_3_CA_GNL`` …) also carry
    ``declared_MC=0`` (fuel cost attached to one segment of the block,
    MC=0 on the rest) but they ARE dispatchable peakers — keep them
    eligible.  The distinguishing feature is the fuel family / name,
    not the MC.
    """
    # 1) First-class ``cogen_mode`` C++ field (any value ⇒ cogen).
    # See include/gtopt/generator.hpp + generator_enums.hpp.  Replaces
    # the legacy ``is_cogen`` JSON-only field starting 2026-06.
    if g.get("cogen_mode"):
        return True

    # 1b) Legacy ``is_cogen`` flag — kept as back-compat for planning
    # JSONs predating the C++ field.  Once every converter pass and
    # bundled fixture has been re-generated, this branch can be dropped.
    if bool(g.get("is_cogen", False)):
        return True

    raw_type = str(g.get("type", "")).lower()

    # 2) Canonical ``thermal:cogen`` type tag (already applied by
    # plp2gtopt's emissions overlay; future plexos2gtopt extension).
    if raw_type.startswith("thermal:cogen"):
        return True

    # 3) CEN reference list — exact name OR prefix match.
    name = str(g.get("name", "")).strip().upper()
    if name:
        ref_names, ref_prefixes = _load_cogen_reference()
        if name in ref_names:
            return True
        for p in ref_prefixes:
            if name.startswith(p):
                return True

    # 4) Heuristic fallback.
    _family, _, sub = raw_type.partition(":")
    if sub in ("biomasa", "biomass", "biogas", "geothermal"):
        return True
    if sub == "otros":
        return True
    fuel = g.get("fuel")
    fuel_name = ""
    if isinstance(fuel, str):
        fuel_name = fuel
    elif isinstance(fuel, dict):
        fuel_name = str(fuel.get("name", ""))
    fuel_name_lc = fuel_name.lower()
    if fuel_name.startswith("Otros_") or "noracid" in fuel_name_lc:
        return True
    if name.startswith("PAS_"):
        return True
    return False


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
        if sub == "geothermal":
            # Geothermal is a price-TAKER for LMP attribution, not a
            # price-setter: it has zero marginal cost (no commercial
            # fuel — IPCC stationary-combustion default is 0 because
            # the energy source is endogenous heat, not combustion),
            # operates as fixed-inflow baseload like wind/solar, and
            # in CEN runs always at its rated capacity.  Classifying
            # as "profile" puts it in the merit-ineligible filter so
            # ``_select_marginal_candidates`` skips it; the recipe
            # layer's consequential walk-up then attributes both the
            # LMP and the carbon to the next-up thermal merit gen
            # actually setting the price.  Without this branch,
            # CERRO_PABELLON_U1 (the only commercial geothermal in
            # CEN) is elected as LP marginal in ~11 k jan18 cells
            # purely on tie-break (``rc ≈ 0`` because ``gcost = 0``)
            # and trips Guard B no_emission_data audit.
            return "profile"
        if sub in ("biomass", "biomasa", "biogas"):
            # Biomass / biogas DO have commercial fuel (purchased
            # residue, captured biogas) priced via the LP's
            # ``Fuel.price`` element, so they're real merchant
            # thermals even though the EF is biogenic-zero.  Keep
            # as kind="thermal".
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
