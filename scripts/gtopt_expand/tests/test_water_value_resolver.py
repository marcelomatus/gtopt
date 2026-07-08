# -*- coding: utf-8 -*-

"""WaterValueResolver integration tests for gtopt_expand agreements.

These tests pin the auto water-fail-cost wiring contract:

* When a :class:`plp2gtopt._water_value.WaterValueResolver` is supplied
  to :class:`gtopt_expand.laja_agreement.LajaAgreement` /
  :class:`gtopt_expand.maule_agreement.MauleAgreement` *and*
  ``resolver.is_active`` is ``True``, the rendered template context's
  ``cost_irr_ns`` (Laja retiro deficit base) and ``penalty_invernada``
  (Maule) are replaced by energy-equivalent values derived from
  :meth:`WaterValueResolver.fail_cost` / :meth:`efin_cost` at the
  relevant cascade lost-pf.  (``cost_elec_ns`` no longer exists — PLP
  has no electric non-served cost; the 1150 datum is a usage cost.)
* When the resolver is ``None`` or ``is_active`` is ``False`` the
  legacy PLP cost fields are preserved verbatim — this is the
  back-compat path used by every existing call site that hasn't
  opted in to the new pipeline.
* ``cost_irr_uso`` / ``cost_elec_uso`` / ``cost_mixed_uso`` /
  ``cost_antic_uso`` are usage costs on the rights flows (emitted as
  negative ``uvalue``), so they are NOT touched regardless of
  resolver state.
* Per-district FlowRights with an explicit ``injection`` central use
  ``cascade_lost_pf(injection)`` rather than the Laja-main cascade.
"""

from __future__ import annotations

from typing import Any

from gtopt_expand.laja_agreement import LajaAgreement
from gtopt_expand.maule_agreement import MauleAgreement


# ---------------------------------------------------------------------------
# Mock resolver — minimal stand-in for ``WaterValueResolver``
# ---------------------------------------------------------------------------
class _MockWaterValueResolver:
    """Tiny stand-in for :class:`plp2gtopt._water_value.WaterValueResolver`.

    Mirrors the public surface that :class:`LajaAgreement` and
    :class:`MauleAgreement` consume — ``is_active``,
    ``cascade_lost_pf``, ``junction_lost_pf``, ``fail_cost``,
    ``efin_cost``, ``water_fail_cost``, ``max_rendi`` — plus the
    ``_by_name`` private dict that the agreements walk to translate a
    central name into a PLP ``number``.

    The mock takes the energy-anchor (``water_fail_cost``) and a
    name → ``(number, lost_pf)`` map.  ``cascade_lost_pf(num)`` returns
    the matching ``lost_pf``; ``fail_cost`` / ``efin_cost`` use the
    same closed-form formulas as the real resolver
    (``ANCHOR × pf`` and ``ANCHOR × pf × 1e6 / 3600``).
    """

    _HM3_PER_M3 = 1.0e6
    _SECONDS_PER_HOUR = 3600.0

    def __init__(
        self,
        *,
        is_active: bool = True,
        water_fail_cost: float = 1100.0,
        centrals: dict[str, tuple[int, float]] | None = None,
    ) -> None:
        self._is_active = is_active
        self._water_fail_cost = water_fail_cost
        centrals = centrals or {}
        # ``_by_name`` shape mirrors ``WaterValueResolver._by_name`` so
        # ``LajaAgreement._resolved_central_number`` and
        # ``MauleAgreement._override_penalty_invernada`` can read the
        # ``"number"`` field directly.
        self._by_name: dict[str, dict[str, Any]] = {
            name: {"name": name, "number": num} for name, (num, _) in centrals.items()
        }
        self._cascade_by_number: dict[int, float] = dict(centrals.values())
        self._junction_by_number: dict[int, float] = dict(self._cascade_by_number)

    # ---- gating ---------------------------------------------------------
    @property
    def is_active(self) -> bool:
        return self._is_active

    @property
    def water_fail_cost(self) -> float:
        return self._water_fail_cost

    @property
    def anchor(self) -> float:  # parity with the real resolver
        return self._water_fail_cost

    # ---- topology -------------------------------------------------------
    def cascade_lost_pf(self, central_number: int) -> float:
        return self._cascade_by_number.get(int(central_number), 0.0)

    def junction_lost_pf(self, central_number: int) -> float:
        return self._junction_by_number.get(int(central_number), 0.0)

    def max_rendi(self, central_name: str) -> float:
        # Not consumed by the agreement classes today, but kept for
        # API parity in case future callers need it on the mock.
        for num, pf in self._cascade_by_number.items():
            if self._by_name.get(central_name, {}).get("number") == num:
                return pf
        return 0.0

    # ---- cost surfaces --------------------------------------------------
    def fail_cost(self, lost_pf: float) -> float:
        return self._water_fail_cost * float(lost_pf)

    def efin_cost(self, lost_pf: float) -> float:
        return (
            self._water_fail_cost
            * float(lost_pf)
            * self._HM3_PER_M3
            / self._SECONDS_PER_HOUR
        )


# ---------------------------------------------------------------------------
# Minimal config builders
# ---------------------------------------------------------------------------
def _minimal_laja_config() -> dict[str, Any]:
    """Mirror ``test_round_trip._minimal_laja_config`` so the assertions
    are reproducible across PRs.  Kept here (not imported) so changes to
    the round-trip fixture don't accidentally re-route the assertions
    for the auto-water-fail-cost contract.
    """
    return {
        "central_laja": "TEST_CENTRAL",
        "vol_max": 1000.0,
        "vol_muerto": 0.0,
        "zone_widths": [500, 500],
        "irr_base": 100,
        "irr_factors": [0.0, 0.5],
        "elec_base": 0,
        "elec_factors": [0.1, 0.2],
        "mixed_base": 10,
        "mixed_factors": [0.0, 0.0],
        "max_irr": 500,
        "max_elec": 200,
        "max_mixed": 10,
        "max_anticipated": 500,
        "qmax_irr": 100,
        "qmax_elec": 100,
        "qmax_mixed": 50,
        "qmax_anticipated": 0,
        "cost_irr_ns": 1000,
        "cost_irr_uso": 0.25,
        "cost_elec_uso": 0.1,
        "cost_mixed_uso": 1.0,
        "cost_antic_uso": 0.0,
        "monthly_usage_irr": [1] * 12,
        "monthly_usage_elec": [1] * 12,
        "monthly_usage_mixed": [1] * 12,
        "monthly_usage_anticipated": [0] * 12,
        "monthly_cost_irr_ns": [1.0] * 12,
        "monthly_cost_irr": [1.0] * 12,
        "monthly_cost_elec": [1.0] * 12,
        "monthly_cost_mixed": [1.0] * 12,
        "monthly_cost_anticipated": [0.0] * 12,
        "ini_irr": 100,
        "ini_elec": 50,
        "ini_mixed": 0,
        "ini_anticipated": 0,
        "filtracion_laja": 10.0,
        "districts": [
            {
                "name": "D1",
                "injection": None,
                "cost_factor": 1.0,
                "pct_1o_reg": 1.0,
                "pct_2o_reg": 0.0,
                "pct_emergencia": 0.0,
                "pct_saltos": 0.0,
            },
            {
                "name": "D2",
                "injection": "INJECTION_CENTRAL",
                "cost_factor": 0.5,
                "pct_1o_reg": 0.5,
                "pct_2o_reg": 0.0,
                "pct_emergencia": 0.0,
                "pct_saltos": 0.0,
            },
        ],
        "demand_1o_reg": 50,
        "demand_2o_reg": 0,
        "demand_emergencia": 0,
        "demand_saltos": 0,
        "seasonal_1o_reg": [1] * 12,
        "seasonal_2o_reg": [0] * 12,
        "seasonal_emergencia": [0] * 12,
        "seasonal_saltos": [0] * 12,
    }


def _minimal_maule_config() -> dict[str, Any]:
    """Mirror ``test_round_trip._minimal_maule_config``."""
    return {
        "central_maule": "LMAULE",
        "central_invernada": "CIPRESES",
        "central_melado": "PEHUENCHE",
        "central_colbun": "COLBUN",
        "v_reserva_extraord": 100.0,
        "v_reserva_ordinaria": 400.0,
        "v_der_riego_temp_max": 800.0,
        "v_der_elect_anu_max": 250.0,
        "v_comp_elec_max": 350.0,
        "gasto_elec_men_max": 25.0,
        "gasto_elec_dia_max": 30.0,
        "gasto_riego_max": 200.0,
        "mod_elec_reserva": [100] * 6 + [0] * 6,
        "pct_riego_mensual": [100] * 12,
        "pct_elec_reserva": 20.0,
        "pct_riego_reserva": 80.0,
        "caudal_res105": [80, 40, 40, 40, 40, 40, 40, 40, 60, 80, 80, 80],
        "v_gasto_elec_men_ini": 0.0,
        "v_gasto_elec_anu_ini": 50.0,
        "v_gasto_riego_ini": 400.0,
        "v_comp_elec_ini": 10.0,
        "v_econ_inver_ini": 0.0,
        "penalizador_1": 1500.0,
        "costo_riego_ns_maule": 1000.0,
        "costo_riego_ns_res105": 1000.0,
        "econ_inver_costo": 0.5,
        "costo_embalsar": 1500.0,
        "costo_no_embalsar": 1000.0,
        "bocatoma_canelon": "BCanelon",
        "costo_canelon": 10.0,
        "districts": [
            {"name": "Dist1", "percentage": 60.0, "has_slack": False},
            {"name": "Dist2", "percentage": 40.0, "has_slack": True},
        ],
    }


# ---------------------------------------------------------------------------
# Helpers — extract the rendered scalar from a per-stage schedule
# ---------------------------------------------------------------------------
def _scalar_from_sched(value: Any) -> float:
    """Return the inner scalar from a Laja FlowRight ``fail_cost`` field.

    The Laja templates emit ``fail_cost`` as either a bare scalar (when
    every stage shares the same modulated cost) or a 2D / 3D nested
    list.  The minimal config uses constant monthly modulators so the
    output should always collapse to a scalar; this helper makes the
    assertion explicit if the encoding ever changes.
    """
    if isinstance(value, (int, float)):
        return float(value)
    cur = value
    while isinstance(cur, list) and cur:
        cur = cur[0]
    return float(cur)


def _flow_right(agreement: Any, name: str) -> dict[str, Any]:
    for fr in agreement.flow_rights:
        if fr.get("name") == name:
            return fr
    raise AssertionError(f"flow_right {name!r} not in {agreement.flow_rights!r}")


# ---------------------------------------------------------------------------
# Laja tests
# ---------------------------------------------------------------------------
def test_laja_with_resolver_active_overrides_costs():
    """Active resolver → the district deficit base reflects the cascade.

    The main rights flows carry NO fail cost (PLP's non-served
    penalties live on the retiro deficits only), so the override is
    observable on the district FlowRights.
    """
    cfg = _minimal_laja_config()
    resolver = _MockWaterValueResolver(
        is_active=True,
        water_fail_cost=1100.0,
        centrals={"TEST_CENTRAL": (10, 0.7)},
    )
    agreement = LajaAgreement(cfg, water_value_resolver=resolver)

    expected = resolver.fail_cost(resolver.cascade_lost_pf(10))  # 1100 * 0.7
    fr_d1 = _flow_right(agreement, "D1_1o_reg")
    assert _scalar_from_sched(fr_d1["fcost"]) == expected  # cost_factor=1
    # The rights flows themselves never carry an NS cost.
    for name in ("laja_der_riego", "laja_der_electrico", "laja_gasto_anticipado"):
        assert "fcost" not in _flow_right(agreement, name)


def test_laja_without_resolver_preserves_legacy_costs():
    """No resolver → cfg values flow into the entities unchanged."""
    cfg = _minimal_laja_config()
    agreement = LajaAgreement(cfg)  # water_value_resolver=None by default

    fr_d1 = _flow_right(agreement, "D1_1o_reg")
    assert _scalar_from_sched(fr_d1["fcost"]) == cfg["cost_irr_ns"]
    fr_electrico = _flow_right(agreement, "laja_der_electrico")
    assert _scalar_from_sched(fr_electrico["uvalue"]) == -cfg["cost_elec_uso"]


def test_laja_resolver_inactive_preserves_legacy_costs():
    """Resolver passed but ``is_active=False`` → legacy path."""
    cfg = _minimal_laja_config()
    resolver = _MockWaterValueResolver(
        is_active=False,
        water_fail_cost=1100.0,
        centrals={"TEST_CENTRAL": (10, 0.7)},
    )
    agreement = LajaAgreement(cfg, water_value_resolver=resolver)
    fr_d1 = _flow_right(agreement, "D1_1o_reg")
    assert _scalar_from_sched(fr_d1["fcost"]) == cfg["cost_irr_ns"]


def test_laja_district_per_central_cascade():
    """District with ``injection`` set → resolver uses that cascade."""
    cfg = _minimal_laja_config()
    # D2 has injection='INJECTION_CENTRAL', a different cascade pf.
    resolver = _MockWaterValueResolver(
        is_active=True,
        water_fail_cost=1100.0,
        centrals={
            "TEST_CENTRAL": (10, 0.7),
            "INJECTION_CENTRAL": (20, 0.3),
        },
    )
    agreement = LajaAgreement(cfg, water_value_resolver=resolver)

    # D1 has injection=None → falls back to cost_irr_ns × cost_factor
    # (which itself was auto-derived from TEST_CENTRAL).
    fr_d1 = _flow_right(agreement, "D1_1o_reg")
    laja_main_cost = resolver.fail_cost(resolver.cascade_lost_pf(10))
    assert fr_d1["fcost"] == laja_main_cost * 1.0  # cost_factor=1

    # D2 has injection='INJECTION_CENTRAL' → uses its own cascade.
    fr_d2 = _flow_right(agreement, "D2_1o_reg")
    expected_d2 = resolver.fail_cost(resolver.cascade_lost_pf(20))  # 1100 * 0.3
    assert fr_d2["fcost"] == expected_d2


def test_laja_district_unknown_injection_falls_back():
    """Active resolver but unknown injection → legacy factor scaling."""
    cfg = _minimal_laja_config()
    # Resolver only knows TEST_CENTRAL, NOT 'INJECTION_CENTRAL', so D2
    # should fall back to ``cost_irr_ns * cost_factor`` (itself
    # auto-derived from TEST_CENTRAL).
    resolver = _MockWaterValueResolver(
        is_active=True,
        water_fail_cost=1100.0,
        centrals={"TEST_CENTRAL": (10, 0.7)},
    )
    agreement = LajaAgreement(cfg, water_value_resolver=resolver)
    fr_d2 = _flow_right(agreement, "D2_1o_reg")
    laja_main_cost = resolver.fail_cost(resolver.cascade_lost_pf(10))
    assert fr_d2["fcost"] == laja_main_cost * 0.5  # D2 cost_factor=0.5


def test_laja_use_values_unchanged():
    """``cost_irr_uso`` / ``cost_elec_uso`` / ``cost_mixed`` never overridden.

    These are *use_value* benefits the LP gains by using the water for
    irrigation/electric/mixed purposes, not penalty costs.  The
    auto-water-fail-cost pipeline must leave them untouched regardless
    of resolver state — pin this in both directions.
    """
    cfg = _minimal_laja_config()
    resolver_off = _MockWaterValueResolver(is_active=False)
    resolver_on = _MockWaterValueResolver(
        is_active=True,
        water_fail_cost=1100.0,
        centrals={"TEST_CENTRAL": (10, 0.7)},
    )

    a_off = LajaAgreement(cfg, water_value_resolver=resolver_off)
    a_on = LajaAgreement(cfg, water_value_resolver=resolver_on)

    # Usage costs are emitted as NEGATIVE uvalue (PLP charges the
    # rights flows) — `-cost_*_uso × monthly_factor` (here all 1.0) —
    # and must be identical whether the resolver is on or off.
    fr_riego_off = _flow_right(a_off, "laja_der_riego")
    fr_riego_on = _flow_right(a_on, "laja_der_riego")
    assert _scalar_from_sched(fr_riego_off["uvalue"]) == -cfg["cost_irr_uso"]
    assert _scalar_from_sched(fr_riego_on["uvalue"]) == -cfg["cost_irr_uso"]

    fr_elec_off = _flow_right(a_off, "laja_der_electrico")
    fr_elec_on = _flow_right(a_on, "laja_der_electrico")
    assert _scalar_from_sched(fr_elec_off["uvalue"]) == -cfg["cost_elec_uso"]
    assert _scalar_from_sched(fr_elec_on["uvalue"]) == -cfg["cost_elec_uso"]

    fr_mixed_off = _flow_right(a_off, "laja_der_mixto")
    fr_mixed_on = _flow_right(a_on, "laja_der_mixto")
    assert _scalar_from_sched(fr_mixed_off["uvalue"]) == -cfg["cost_mixed_uso"]
    assert _scalar_from_sched(fr_mixed_on["uvalue"]) == -cfg["cost_mixed_uso"]


# ---------------------------------------------------------------------------
# Maule tests
# ---------------------------------------------------------------------------
def _find_user_constraint(agreement: Any, name: str) -> dict[str, Any]:
    for uc in agreement.user_constraints:
        if uc.get("name") == name:
            return uc
    raise AssertionError(
        f"user_constraint {name!r} not found in {agreement.user_constraints!r}"
    )


def test_maule_with_resolver_overrides_penalty_invernada():
    """Active resolver → ``penalty_invernada`` reflects cascade(invernada)."""
    cfg = _minimal_maule_config()
    resolver = _MockWaterValueResolver(
        is_active=True,
        water_fail_cost=1100.0,
        centrals={"CIPRESES": (42, 0.45)},
    )
    agreement = MauleAgreement(cfg, water_value_resolver=resolver)

    # ``penalty_invernada`` = ``efin_cost(0.45) / 1e6``
    #                      = ANCHOR × pf × 1e6 / 3600 / 1e6
    #                      = ANCHOR × pf / 3600
    expected = 1100.0 * 0.45 / 3600.0
    uc = _find_user_constraint(agreement, "invernada_balance")
    assert uc.get("penalty") == expected


def test_maule_without_resolver_preserves_hydro_fail_cost_default():
    """No resolver → ``penalty_invernada`` falls back to the legacy default.

    With neither ``penalty_invernada`` nor ``hydro_fail_cost`` in the
    config the agreement uses its built-in 10.0 default.
    """
    cfg = _minimal_maule_config()
    agreement = MauleAgreement(cfg)  # resolver=None
    uc = _find_user_constraint(agreement, "invernada_balance")
    assert uc.get("penalty") == 10.0


def test_maule_resolver_inactive_preserves_hydro_fail_cost():
    """Resolver provided but inactive → legacy ``hydro_fail_cost`` honored."""
    cfg = _minimal_maule_config()
    cfg["hydro_fail_cost"] = 25.0  # exercise the legacy fallback
    resolver = _MockWaterValueResolver(
        is_active=False,
        water_fail_cost=1100.0,
        centrals={"CIPRESES": (42, 0.45)},
    )
    agreement = MauleAgreement(cfg, water_value_resolver=resolver)
    uc = _find_user_constraint(agreement, "invernada_balance")
    assert uc.get("penalty") == 25.0


def test_maule_resolver_unknown_invernada_falls_back():
    """Active resolver but central_invernada not in the index → legacy path."""
    cfg = _minimal_maule_config()
    cfg["hydro_fail_cost"] = 12.5
    resolver = _MockWaterValueResolver(
        is_active=True,
        water_fail_cost=1100.0,
        centrals={"NOT_CIPRESES": (1, 0.1)},  # CIPRESES is missing
    )
    agreement = MauleAgreement(cfg, water_value_resolver=resolver)
    uc = _find_user_constraint(agreement, "invernada_balance")
    assert uc.get("penalty") == 12.5


# ---------------------------------------------------------------------------
# from_json plumbing
# ---------------------------------------------------------------------------
def test_from_json_threads_resolver(tmp_path):
    """``LajaAgreement.from_json`` accepts a resolver kwarg.

    Pinned because the CLI relies on this to wire the
    ``--auto-water-fail-cost`` flag through to the agreement.
    """
    import json

    cfg = _minimal_laja_config()
    json_path = tmp_path / "laja.json"
    with open(json_path, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh)

    resolver = _MockWaterValueResolver(
        is_active=True,
        water_fail_cost=1100.0,
        centrals={"TEST_CENTRAL": (10, 0.7)},
    )
    agreement = LajaAgreement.from_json(json_path, water_value_resolver=resolver)

    fr_d1 = _flow_right(agreement, "D1_1o_reg")
    expected = resolver.fail_cost(resolver.cascade_lost_pf(10))
    assert _scalar_from_sched(fr_d1["fcost"]) == expected
