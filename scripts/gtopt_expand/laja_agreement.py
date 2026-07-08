# -*- coding: utf-8 -*-

"""Laja irrigation agreement → gtopt entities.

Reads a canonical ``laja.json`` agreement description (the Stage-1 product
of ``plp2gtopt`` or hand-authored) and produces:

* ``FlowRight`` entries (irrigation, electric, mixed, anticipated, district
  withdrawals).
* ``VolumeRight`` entries (rights buckets + economy trackers).
* ``UserConstraint`` entries (flow partition balance).
* A companion ``laja.pampl`` file containing the named sets and parameters.

This is the canonical Stage-2 transform in the irrigation pipeline (see
``project_irrigation_pipeline.md``).  Stage 1 (``plp2gtopt``) converts
``plplajam.dat`` to ``laja.json``; Stage 3 (``gtopt``) consumes the
entities the same way it consumes any other JSON/PAMPL input.

``laja.json`` schema
--------------------

A flat dict mirroring the field set produced by ``LajaParser`` (see
``scripts/plp2gtopt/laja_parser.py``).  Required keys:

* ``central_laja`` (str), ``vol_max`` (float), ``vol_muerto`` (float).
* ``zone_widths`` (list[float]) — width of each volume zone.
* ``irr_base`` / ``elec_base`` / ``mixed_base`` (float) and matching
  ``*_factors`` (list[float]) — one factor per zone.
* ``max_irr`` / ``max_elec`` / ``max_mixed`` / ``max_anticipated`` (float).
* ``qmax_*`` flow caps (m³/s) for each rights category.
* ``cost_irr_ns`` (retiro deficit penalty base) plus the four usage
  costs ``cost_irr_uso`` / ``cost_elec_uso`` / ``cost_mixed_uso`` /
  ``cost_antic_uso`` (float, positive PLP objective coefficients on
  the rights flows; ``cost_mixed`` is accepted as a legacy alias for
  ``cost_mixed_uso``).
* ``monthly_cost_*`` and ``monthly_usage_*`` arrays (12 floats, hydro
  year Apr-Mar) for each category.
* ``ini_*`` initial bucket levels for each category and economy tracker.
* ``districts`` (list[dict]) with ``name``, ``injection``, ``cost_factor``,
  and ``pct_*`` shares.
* ``demand_*`` and ``seasonal_*`` arrays for the four irrigation classes.

Stages can be supplied via a ``stage_parser``-shaped object whose
``get_all()`` returns dicts containing ``number`` and ``month``; if no
stage_parser is given the schedules stay in raw hydro-year form.
"""

from __future__ import annotations

import logging
from typing import Any

from gtopt_expand._base import _RightsAgreementBase

_logger = logging.getLogger(__name__)


# Hydrological year mapping: PLP uses Apr=1..Mar=12
# gtopt uses calendar months: january=1..december=12
# So PLP hydro month H maps to calendar month (H + 3) % 12, with 0 → 12
_HYDRO_TO_CALENDAR = [4, 5, 6, 7, 8, 9, 10, 11, 12, 1, 2, 3]

# Calendar month number (1..12) → gtopt MonthType JSON spelling.
_MONTH_NAMES = [
    "january",
    "february",
    "march",
    "april",
    "may",
    "june",
    "july",
    "august",
    "september",
    "october",
    "november",
    "december",
]


def _hydro_month_name(hydro_month: int) -> str:
    """Map a PLP hydrological month (1=Apr..12=Mar) to a calendar name.

    Used for the VolumeRight ``reset_month`` fields: PLP re-provisions
    the rights buckets at ``MesIniTempRiego`` (hydro month 9 =
    **december**, the start of the Dec-1..Apr-30 irrigation season per
    the 2017 Acuerdo, `genpdlajam.f` INICIOTEMP) and resets the
    anticipado counter at ``MesIniTempAntic`` (hydro month 6 =
    **september**, INICIOANTIC).
    """
    calendar = _HYDRO_TO_CALENDAR[(int(hydro_month) - 1) % 12]
    return _MONTH_NAMES[calendar - 1]


def _zones_to_bound_rule_segments(
    base: float,
    factors: list[float],
    widths: list[float],
    vol_muerto: float = 0.0,
    transfer_base: float = 0.0,
    transfer_factors: list[float] | None = None,
) -> list[dict[str, float]]:
    """Convert PLP volume zone factors to bound_rule segments.

    PLP formula: Rights = base + Sum_i(factor_i * min(Vi, width_i))
    where Vi is the volume within zone i.

    This is piecewise-linear in total volume V:
      Zone 0: V in [vol_muerto, vol_muerto + width[0])
        rights(V) = base + factor[0] * (V - vol_muerto)
      Zone 1: V in [vol_muerto + width[0], vol_muerto + width[0] + width[1])
        rights(V) = base + factor[0]*width[0] + factor[1]*(V - vol_muerto - width[0])
      etc.

    Each bound_rule segment stores {volume, slope, constant} where
    rights(V) = constant + slope * V for V >= volume.

    Mixed-rights transfer (irrigation only): PLP adds the UNUSED
    share of the mixed base to the irrigation provision in each zone
    -- ``DerRiego += DerMixtoBase * (1 - FactDerMixtoColchon(zone))``
    (genpdlajam.f:647-649).  Passing ``transfer_base`` /
    ``transfer_factors`` applies that per-zone constant, which makes
    the emitted rights match the 2017 Acuerdo's Tabla 1 exactly
    (e.g. V=1500 -> 668 + 0.40*(V-1370) = 720, not 690).  The
    transfer makes the function DISCONTINUOUS at the zone breakpoints
    -- a real feature of the agreement (570 -> 600 at 1200 hm3).

    Args:
        base: Base rights level (rights at vol_muerto)
        factors: Incremental factor per volume zone
        widths: Width of each volume zone [hm3]
        vol_muerto: Dead volume below which no extraction
        transfer_base: Mixed-rights base transferred per zone [hm3]
        transfer_factors: Mixed retention factor per zone (the zone
            keeps ``transfer_base * factor`` as mixed rights and
            transfers the complement to irrigation)

    Returns:
        List of bound_rule segments sorted by volume breakpoint
    """
    segments: list[dict[str, float]] = []
    cumulative_vol = vol_muerto
    cumulative_rights = base

    for i, (factor, width) in enumerate(zip(factors, widths)):
        # At volume = cumulative_vol, rights = cumulative_rights
        # and slope = factor
        # So: rights(V) = cumulative_rights + factor * (V - cumulative_vol)
        #                = (cumulative_rights - factor * cumulative_vol) + factor * V
        constant = cumulative_rights - factor * cumulative_vol
        if transfer_factors is not None:
            constant += transfer_base * (1.0 - transfer_factors[i])
        segments.append(
            {
                "volume": cumulative_vol,
                "slope": factor,
                "constant": constant,
            }
        )
        # Advance to next zone boundary
        cumulative_rights += factor * width
        cumulative_vol += width

    # If no zones, just emit a flat segment
    if not segments:
        segments.append({"volume": vol_muerto, "slope": 0, "constant": base})

    return segments


def _zones_to_step_segments(
    base: float,
    factors: list[float],
    widths: list[float],
    vol_muerto: float = 0.0,
) -> list[dict[str, float]]:
    """Convert PLP per-zone SELECTOR factors to step-function segments.

    PLP treats the mixed-rights factors as a selector, not cumulative
    slopes: ``DerMixto = DerMixtoBase * FactDerMixtoColchon(active
    zone)`` (genpdlajam.f:647-648).  With the CEN data ``[1, 0, 0, 0]``
    the mixed right is 30 hm3 in the lower cushion and 0 above it (the
    base transfers to irrigation — see
    ``_zones_to_bound_rule_segments``).

    Returns one zero-slope segment per zone with
    ``constant = base * factor``.
    """
    segments: list[dict[str, float]] = []
    cumulative_vol = vol_muerto
    for factor, width in zip(factors, widths):
        segments.append(
            {
                "volume": cumulative_vol,
                "slope": 0.0,
                "constant": base * factor,
            }
        )
        cumulative_vol += width
    if not segments:
        segments.append({"volume": vol_muerto, "slope": 0.0, "constant": base})
    return segments


class LajaAgreement(_RightsAgreementBase):
    """Stage-2 transform: laja.json → FlowRight/VolumeRight/UserConstraint.

    The JSON entity structure is defined in ``templates/laja.tson``.
    This class pre-computes all dynamic values (segments, schedules,
    costs) and passes them as template context parameters.

    Args:
        laja_config: Canonical Laja configuration dict (see module docstring
            for the schema).
        stage_parser: Optional stage-parser-shaped object whose ``get_all()``
            yields per-stage dicts with ``number`` and ``month``.  When
            omitted, monthly schedules stay in raw hydro-year form.
        options: Conversion options dict (e.g. ``last_stage``,
            ``blocks_per_stage``).
    """

    _ARTIFACT = "laja"
    _UID_START = 2000  # avoid collisions with Maule (1000..)

    def __init__(
        self,
        laja_config: dict[str, Any],
        stage_parser: Any = None,
        options: dict[str, Any] | None = None,
        water_value_resolver: Any = None,
    ):
        super().__init__(
            laja_config,
            stage_parser=stage_parser,
            options=options,
            water_value_resolver=water_value_resolver,
        )

    # ----------------------------------------------------- water-value helpers
    def _resolved_central_number(self, central_name: str) -> int | None:
        """Look up *central_name* in the resolver's by-name index.

        Returns the central's PLP ``number`` field or ``None`` when the
        resolver is not configured or the name is unknown.  Used both for
        the main Laja central and for per-district injection lookups.
        """
        resolver = self._water_value_resolver
        if resolver is None:
            return None
        # ``WaterValueResolver`` exposes ``_by_name``: a private dict of
        # ``{name: central}``.  We could expose a public accessor on the
        # resolver, but the current API uses ``cascade_lost_pf(number)``
        # — so we must look up the number ourselves here.  Keep the
        # lookup defensive: the dict is built from parsed PLP data which
        # may legitimately omit names this agreement references.
        by_name = getattr(resolver, "_by_name", None) or {}
        central = by_name.get(central_name)
        if central is None:
            return None
        try:
            return int(central.get("number", -1))
        except (TypeError, ValueError):
            return None

    def _override_main_costs(
        self,
        cfg: dict[str, Any],
        legacy_irr_ns: float,
    ) -> float:
        """Return ``cost_irr_ns`` after resolver substitution.

        When the resolver is active and the Laja main central is known,
        the retiro non-served base is replaced by
        ``resolver.fail_cost(cascade_pf)`` (units: ``$/(m³/s·h)``).
        Logs the substitution at INFO so the change is auditable in
        juan/IPLP-style end-to-end runs.

        Falls through to the legacy PLP value otherwise.  (The former
        ``cost_elec_ns`` override is gone: PLP has no electric
        non-served cost — that key was a parser column-shift misread;
        the electric field is a positive *usage* cost handled via
        ``use_value_elec``.)
        """
        resolver = self._water_value_resolver
        if resolver is None or not resolver.is_active:
            return legacy_irr_ns
        central_name = cfg["central_laja"]
        number = self._resolved_central_number(central_name)
        if number is None:
            _logger.info(
                "auto-water-fail-cost: laja central %r not found in"
                " WaterValueResolver index; keeping legacy costs.",
                central_name,
            )
            return legacy_irr_ns
        cascade_pf = resolver.cascade_lost_pf(number)
        new_value = resolver.fail_cost(cascade_pf)
        _logger.info(
            "auto-water-fail-cost: laja %s = %g (was %g, cascade_pf=%g at %s)",
            "cost_irr_ns",
            new_value,
            legacy_irr_ns,
            cascade_pf,
            central_name,
        )
        return new_value

    def _hydro_to_stage_schedule(self, hydro_monthly: list[float]) -> list[float]:
        """Convert 12-element hydrological-year array to per-stage schedule.

        PLP Laja uses hydrological year (Apr=index 0 .. Mar=index 11).
        Maps each stage's calendar month to the corresponding hydro index.
        """
        if self._stage_parser is None:
            return hydro_monthly

        stages = self._get_stages()
        schedule: list[float] = []
        for stage in stages:
            cal_month = stage.get("month", 1)  # calendar: 1=Jan..12=Dec
            # Calendar month → hydro index: Apr=0, May=1, ..., Mar=11
            hydro_idx = (cal_month - 4) % 12
            schedule.append(hydro_monthly[hydro_idx])
        return schedule

    def _fmax_schedule(
        self,
        qmax: float,
        usage: list[float],
    ) -> float | list[list[float]]:
        """Return a per-stage-block fmax schedule = ``qmax * usage[stage]``.

        Emits a TBRealFieldSched (scalar or 2D ``[[v]*nblocks for v in
        values]``) — the post-2026-05 FlowRight `fmax` field type
        (re-widened from 1D in the type-widening agent).
        """
        return self._to_tb_sched([qmax * u for u in usage])

    def _monthly_cost_schedule(
        self,
        base_cost: float,
        monthly_factors: list[float],
    ) -> float | list[list[float]]:
        """Return a per-stage cost schedule modulated by a hydro-year factor.

        Emits a TBRealFieldSched (2D per-stage × per-block) or scalar —
        the current C++ ``FlowRight.fcost`` / ``uvalue`` schema type.
        The post-2026-05 narrowing to a 1D ``TRealFieldSched`` was the
        intended direction but the C++ migration (schema → LP accessors
        → tests) was not landed, so the writer continues to emit 2D for
        now to keep ``daw::json`` happy.  ``CostHelper::block_ecost``
        still applies the block-duration weight uniformly at LP build
        time, so broadcasting the per-stage value across all blocks
        (what ``_to_tb_sched`` does) is loss-less.
        """
        modulated = self._hydro_to_stage_schedule(monthly_factors)
        return self._to_tb_sched([base_cost * f for f in modulated])

    def _prepare_context(self) -> dict[str, Any]:
        """Prepare the template context with all pre-computed values.

        Returns a dict of parameters that the laja.tson template uses
        via @param@ substitution.
        """
        cfg = self._cfg

        central = cfg["central_laja"]
        vol_muerto = cfg["vol_muerto"]

        # ── Auto water-fail-cost overrides ─────────────────────────────
        # When a WaterValueResolver is wired and active, replace the
        # legacy PLP retiro penalty base with an energy-equivalent price
        # derived from max(falla.gcost) × cascade_lost_pf at the Laja
        # main central.  The usage costs (``cost_*_uso``) are NOT
        # touched — those are operation-ordering costs on the rights
        # flows, not shortage penalties.
        cost_irr_ns = self._override_main_costs(
            cfg,
            legacy_irr_ns=float(cfg["cost_irr_ns"]),
        )

        # Usage costs on the four rights flows (PLP: CQVar(IQDR..IQGA),
        # applied as POSITIVE objective coefficients on qdr/qde/qdm/qga
        # in genpdlajam.f:163-165).  Accept the legacy hand-authored
        # keys (`cost_mixed`) as fallback for old canonical JSONs.
        cost_irr_uso = float(cfg.get("cost_irr_uso", 0.0))
        cost_elec_uso = float(cfg.get("cost_elec_uso", 0.0))
        cost_mixed_uso = float(cfg.get("cost_mixed_uso", cfg.get("cost_mixed", 0.0)))
        cost_antic_uso = float(cfg.get("cost_antic_uso", 0.0))

        # --- Bound_rule segments for each rights category ---
        # Irrigation receives the unused mixed share per zone
        # (DerRiego += DerMixtoBase*(1-FMixto), genpdlajam.f:647-649);
        # mixed itself is a per-zone step (base*factor selector).
        irr_segments = _zones_to_bound_rule_segments(
            cfg["irr_base"],
            cfg["irr_factors"],
            cfg["zone_widths"],
            vol_muerto,
            transfer_base=cfg["mixed_base"],
            transfer_factors=cfg["mixed_factors"],
        )
        elec_segments = _zones_to_bound_rule_segments(
            cfg["elec_base"], cfg["elec_factors"], cfg["zone_widths"], vol_muerto
        )
        mixed_segments = _zones_to_step_segments(
            cfg["mixed_base"], cfg["mixed_factors"], cfg["zone_widths"], vol_muerto
        )

        # --- Monthly usage schedules (hydro year → stage) ---
        usage_irr = self._hydro_to_stage_schedule(cfg["monthly_usage_irr"])
        usage_elec = self._hydro_to_stage_schedule(cfg["monthly_usage_elec"])
        usage_mixed = self._hydro_to_stage_schedule(cfg["monthly_usage_mixed"])
        usage_antic = self._hydro_to_stage_schedule(cfg["monthly_usage_anticipated"])

        # fmax = qmax * monthly_usage_factor
        fmax_irr = self._fmax_schedule(cfg["qmax_irr"], usage_irr)
        fmax_elec = self._fmax_schedule(cfg["qmax_elec"], usage_elec)
        fmax_mixed = self._fmax_schedule(cfg["qmax_mixed"], usage_mixed)
        fmax_antic = self._fmax_schedule(cfg["qmax_anticipated"], usage_antic)

        # --- Monthly cost modulation ---
        # PLP applies its usage costs as POSITIVE objective coefficients
        # on the rights flows.  gtopt's FlowRight expresses a per-unit
        # flow cost as a NEGATIVE ``uvalue`` on a target-0 kink (uvalue
        # rewards flow above target; a negative value therefore charges
        # for it — see flow_right_lp.cpp `attach_flow`).  Hence every
        # `cost_*_uso` is negated on emission.
        #
        # There are NO fail costs on the main rights flows: PLP's only
        # non-served penalties live on the per-retiro (district) deficit
        # variables — `CRiegoNSEta × FRiegoCost` (genpdlajam.f:355-358).
        use_value_irr = (
            self._monthly_cost_schedule(-cost_irr_uso, cfg["monthly_cost_irr"])
            if cost_irr_uso != 0
            else None
        )
        use_value_elec = (
            self._monthly_cost_schedule(-cost_elec_uso, cfg["monthly_cost_elec"])
            if cost_elec_uso != 0
            else None
        )
        use_value_mixed = (
            self._monthly_cost_schedule(-cost_mixed_uso, cfg["monthly_cost_mixed"])
            if cost_mixed_uso != 0
            else None
        )
        use_value_antic = (
            self._monthly_cost_schedule(
                -cost_antic_uso, cfg["monthly_cost_anticipated"]
            )
            if cost_antic_uso != 0
            else None
        )

        # --- Reset months (PLP: TipoEtaGM, genpdlajam.f:624-661) ---
        # Rights buckets are re-provisioned at the start of the
        # irrigation season (MesIniTempRiego, hydro month 9 = december
        # per the 2017 Acuerdo Art. 1); the anticipado counter resets at
        # the start of the anticipos window (MesIniTempAntic, hydro
        # month 6 = september).
        reset_month_rights = _hydro_month_name(cfg.get("mes_inicio_riego", 9))
        reset_month_antic = _hydro_month_name(cfg.get("mes_inicio_anticipos", 6))

        # --- District flow rights (pre-computed) ---
        district_flow_rights = self._compute_district_flow_rights(cost_irr_ns)

        # --- User constraint expressions ---
        expression_partition = (
            "flow_right('laja_q_turbinado').flow = "
            "flow_right('laja_der_riego').flow "
            "+ flow_right('laja_der_electrico').flow "
            "+ flow_right('laja_der_mixto').flow "
            "+ flow_right('laja_gasto_anticipado').flow"
        )
        description_partition = (
            "Flow partition: total generation equals sum of extractions"
        )

        return {
            # FlowRight: laja_q_turbinado
            "vol_max": cfg["vol_max"],
            # FlowRight: laja_der_riego
            "fmax_irr": fmax_irr,
            "use_value_irr": use_value_irr,
            # FlowRight: laja_der_electrico
            "fmax_elec": fmax_elec,
            "use_value_elec": use_value_elec,
            # FlowRight: laja_der_mixto
            "fmax_mixed": fmax_mixed,
            "use_value_mixed": use_value_mixed,
            # FlowRight: laja_gasto_anticipado
            "fmax_antic": fmax_antic,
            "use_value_antic": use_value_antic,
            # VolumeRight reset months (hydro → calendar)
            "reset_month_rights": reset_month_rights,
            "reset_month_antic": reset_month_antic,
            # FlowRight: districts
            "district_flow_rights": district_flow_rights,
            # VolumeRight common
            "central": central,
            # VolumeRight: irrigation
            "ini_irr": cfg["ini_irr"],
            "max_irr": cfg["max_irr"],
            "irr_segments": irr_segments,
            # VolumeRight: electric
            "ini_elec": cfg["ini_elec"],
            "max_elec": cfg["max_elec"],
            "elec_segments": elec_segments,
            # VolumeRight: mixed
            "ini_mixed": cfg["ini_mixed"],
            "max_mixed": cfg["max_mixed"],
            "mixed_segments": mixed_segments,
            # VolumeRight: anticipated (up-counter; saving inflow
            # capped at the anticipado flow limit)
            "ini_anticipated": cfg["ini_anticipated"],
            "max_anticipated": cfg["max_anticipated"],
            "qmax_anticipated": cfg.get("qmax_anticipated", 0.0),
            # VolumeRight: economy
            "ini_econ_endesa": cfg.get("ini_econ_endesa", 0),
            "ini_econ_reserve": cfg.get("ini_econ_reserve", 0),
            "ini_econ_polcura": cfg.get("ini_econ_polcura", 0),
            "saving_rate_econ": cfg.get("qmax_elec", 200),
            # UserConstraint: partition balance
            "expression_partition": expression_partition,
            "description_partition": description_partition,
        }

    def _compute_district_flow_rights(
        self,
        cost_irr_ns: float,
    ) -> list[dict[str, Any]]:
        """Pre-compute district withdrawal FlowRight entities.

        Returns a list of dicts ready for JSON serialization.
        Districts × categories, skipping zero-allocation entries.

        ``cost_irr_ns`` is the (possibly auto-derived) Laja-main fail
        cost in ``$/(m³/s·h)``.  Per-district fail costs are then
        computed as either:

        * ``resolver.fail_cost(cascade_pf(district_injection))`` when the
          resolver is active and the district has an explicit
          ``injection`` central, or
        * the legacy ``cost_irr_ns × district["cost_factor"]`` fallback
          (which under the resolver still uses the auto-derived
          ``cost_irr_ns`` value rather than the PLP one — so the
          per-district scaling composes coherently with the energy
          anchor).
        """
        cfg = self._cfg
        resolver = self._water_value_resolver
        resolver_active = resolver is not None and resolver.is_active
        demands = {
            "1o_reg": cfg["demand_1o_reg"],
            "2o_reg": cfg["demand_2o_reg"],
            "emergencia": cfg["demand_emergencia"],
            "saltos": cfg["demand_saltos"],
        }
        seasonal_keys = {
            "1o_reg": "seasonal_1o_reg",
            "2o_reg": "seasonal_2o_reg",
            "emergencia": "seasonal_emergencia",
            "saltos": "seasonal_saltos",
        }
        pct_keys = {
            "1o_reg": "pct_1o_reg",
            "2o_reg": "pct_2o_reg",
            "emergencia": "pct_emergencia",
            "saltos": "pct_saltos",
        }

        result: list[dict[str, Any]] = []
        for district in cfg["districts"]:
            for category, demand_base in demands.items():
                pct = district[pct_keys[category]]
                if pct <= 0 and demand_base <= 0:
                    continue

                seasonal = self._hydro_to_stage_schedule(cfg[seasonal_keys[category]])
                target_values = [demand_base * pct * s for s in seasonal]
                # `target` is the soft kink of the FlowRight; emit scalar
                # or 2D (matches the C++ jvtl_TBRealFieldSched variant).
                target_sched = self._to_tb_sched(target_values)

                fr_name = f"{district['name']}_{category}"
                injection = district.get("injection")
                fail_cost_base = self._district_fail_cost(
                    cost_irr_ns=cost_irr_ns,
                    cost_factor=district["cost_factor"],
                    injection=injection,
                    resolver_active=resolver_active,
                    fr_name=fr_name,
                )
                # PLP modulates the retiro deficit penalty by month:
                # CRiegoNSEta = CRiegoNS × FactMenCRiegoNS(mes)
                # (leelajam.f:221-229) before the per-retiro FRiegoCost
                # factor is applied — so the emitted fcost is a stage
                # schedule, not a flat scalar.
                fail_cost = self._monthly_cost_schedule(
                    fail_cost_base, cfg["monthly_cost_irr_ns"]
                )
                # Emit the canonical `target` / `fcost` keys (the gtopt
                # FlowRight binding still accepts the legacy `discharge` /
                # `fail_cost` aliases, but only re-emits the canonical
                # names on round-trip).
                fr_district: dict[str, Any] = {
                    "name": fr_name,
                    "purpose": "irrigation",
                    "direction": -1,
                    "target": target_sched,
                    "fcost": fail_cost,
                }
                if injection:
                    fr_district["junction_a"] = injection
                result.append(fr_district)

        return result

    def _district_fail_cost(
        self,
        *,
        cost_irr_ns: float,
        cost_factor: float,
        injection: Any,
        resolver_active: bool,
        fr_name: str,
    ) -> float:
        """Compute the fail_cost for one district FlowRight.

        Resolution order:

        1. **Resolver active + injection set** — look up the injection
           central by name.  When found, return
           ``resolver.fail_cost(cascade_pf(injection_number))``.
        2. **Resolver active + no injection** — fall back to the
           ``cost_irr_ns × cost_factor`` formula (where ``cost_irr_ns``
           was already auto-derived from the Laja main cascade).
        3. **Resolver inactive / unset** — pure legacy
           ``cost_irr_ns × cost_factor`` (PLP value × per-district factor).
        """
        legacy = float(cost_irr_ns) * float(cost_factor)
        if not resolver_active or not injection:
            return legacy
        resolver = self._water_value_resolver
        number = self._resolved_central_number(str(injection))
        if number is None:
            _logger.info(
                "auto-water-fail-cost: laja district %s injection %r"
                " not in resolver index; falling back to factor=%g.",
                fr_name,
                injection,
                cost_factor,
            )
            return legacy
        cascade_pf = resolver.cascade_lost_pf(number)
        new_value = float(resolver.fail_cost(cascade_pf))
        _logger.info(
            "auto-water-fail-cost: laja district %s fail_cost = %g"
            " (was %g, cascade_pf=%g at %s)",
            fr_name,
            new_value,
            legacy,
            cascade_pf,
            injection,
        )
        return new_value
