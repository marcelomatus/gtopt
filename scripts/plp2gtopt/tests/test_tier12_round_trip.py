# -*- coding: utf-8 -*-

"""Tier 12 — parser/writer round-trip regression gates.

These tests guard the PLP → ``laja.json`` / ``maule.json`` → gtopt_irrigation
pipeline against silent data loss.  They come in two flavors:

1. **Mapping invariants**: ``_HYDRO_TO_CALENDAR`` is the single source of
   truth for the Apr=1..Mar=12 (PLP hydrological year) → Jan=1..Dec=12
   (calendar) conversion used by every Laja monthly schedule.  A regression
   here would quietly shift every seasonal profile by some number of
   months.  These tests pin the mapping and the formula used by
   ``LajaAgreement._hydro_to_stage_schedule``.

2. **Currently-used round-trip**: ``filtracion_laja`` is parsed by
   ``LajaParser`` and rendered verbatim into ``laja.pampl`` (line 317 of
   ``templates/laja.tampl``).  The test below verifies that changing the
   parsed value perturbs the rendered pampl — i.e. that the field is
   genuinely reachable, not accidentally shadowed by another source.

3. **Optional override / parameter round-trip**: ``LajaParser`` populates
   ``forced_flows`` and ``manual_withdrawals`` and ``MauleParser`` populates
   ``flag_embalsa_1`` / ``flag_embalsa_2`` / ``n_agnos_pct_manual`` /
   ``vol_acum_riego_temp``.  These are emitted as AMPL ``param``/``set``
   declarations in ``laja.tampl`` / ``maule.tampl`` so downstream AMPL
   models can reference them.  Each test below builds two agreements that
   differ *only* in one field and asserts the rendered pampl diverges,
   guarding against silent regressions in either the parser or the
   template.  No agreement constraint currently references these fields;
   they live alongside ``filtracion_laja`` as exposed-but-unconstrained
   data (see ``project_irrigation_test_ladder.md`` Tier 12).
"""

import copy
import json
from pathlib import Path
from typing import Any, Dict

import pytest

from gtopt_irrigation import LajaAgreement, MauleAgreement
from gtopt_irrigation.laja_agreement import _HYDRO_TO_CALENDAR


# ---------------------------------------------------------------------------
# Minimal fixtures (mirrored from gtopt_irrigation/tests/test_round_trip.py
# so this file stays self-contained and doesn't cross-import test helpers).
# ---------------------------------------------------------------------------


def _minimal_laja_config() -> Dict[str, Any]:
    """Minimal valid Laja config matching gtopt_irrigation's test fixture."""
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
        "cost_irr_uso": 0.0,
        "cost_elec_ns": 1100,
        "cost_elec_uso": 0.1,
        "cost_mixed": 1.0,
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
        ],
        "demand_1o_reg": 50,
        "demand_2o_reg": 0,
        "demand_emergencia": 0,
        "demand_saltos": 0,
        "seasonal_1o_reg": [1] * 12,
        "seasonal_2o_reg": [0] * 12,
        "seasonal_emergencia": [0] * 12,
        "seasonal_saltos": [0] * 12,
        # Dropped fields — present so XFAIL tests can mutate them.
        "forced_flows": [],
        "manual_withdrawals": [],
    }


def _minimal_maule_config() -> Dict[str, Any]:
    """Minimal valid Maule config matching gtopt_irrigation's test fixture."""
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
        # Dropped fields — present so XFAIL tests can mutate them.
        "flag_embalsa_1": False,
        "flag_embalsa_2": False,
        "n_agnos_pct_manual": 0,
        "vol_acum_riego_temp": [0.0] * 12,
    }


def _render_laja_pampl(cfg: Dict[str, Any], tmp_path: Path) -> str:
    """Build a LajaAgreement and return the rendered pampl text."""
    agreement = LajaAgreement(cfg)
    agreement.generate_pampl(tmp_path)
    return (tmp_path / "laja.pampl").read_text(encoding="utf-8")


def _render_maule_pampl(cfg: Dict[str, Any], tmp_path: Path) -> str:
    """Build a MauleAgreement and return the rendered pampl text."""
    agreement = MauleAgreement(cfg)
    agreement.generate_pampl(tmp_path)
    return (tmp_path / "maule.pampl").read_text(encoding="utf-8")


def _entities_json(agreement: Any) -> str:
    """Serialize an agreement's entity arrays deterministically."""
    return json.dumps(
        {
            "flow_rights": agreement.flow_rights,
            "volume_rights": agreement.volume_rights,
            "user_constraints": agreement.user_constraints,
        },
        sort_keys=True,
        default=str,
    )


# ---------------------------------------------------------------------------
# 12.1 — Hydro-to-calendar mapping invariants
# ---------------------------------------------------------------------------


class TestHydroToCalendar:
    """Pin the PLP hydro year (Apr=1..Mar=12) ↔ calendar mapping."""

    def test_mapping_values(self):
        """``_HYDRO_TO_CALENDAR[i]`` is the calendar month for hydro index ``i``."""
        # Apr(4)→idx0, May(5)→idx1, ..., Dec(12)→idx8,
        # Jan(1)→idx9, Feb(2)→idx10, Mar(3)→idx11.
        assert _HYDRO_TO_CALENDAR == [4, 5, 6, 7, 8, 9, 10, 11, 12, 1, 2, 3]

    def test_mapping_length(self):
        assert len(_HYDRO_TO_CALENDAR) == 12

    def test_mapping_is_permutation_of_calendar(self):
        """Every calendar month must appear exactly once."""
        assert sorted(_HYDRO_TO_CALENDAR) == list(range(1, 13))

    @pytest.mark.parametrize(
        "cal_month,hydro_idx",
        [
            (4, 0),  # April  → hydro index 0
            (5, 1),  # May    → 1
            (6, 2),
            (7, 3),
            (8, 4),
            (9, 5),
            (10, 6),
            (11, 7),
            (12, 8),  # December → 8
            (1, 9),  # January  → 9
            (2, 10),
            (3, 11),  # March    → 11 (end of hydro year)
        ],
    )
    def test_calendar_month_to_hydro_index(self, cal_month, hydro_idx):
        """Invert the mapping: calendar month → hydro index via ``(m-4) % 12``.

        This is the exact formula used by
        ``LajaAgreement._hydro_to_stage_schedule`` (laja_agreement.py:204).
        """
        assert (cal_month - 4) % 12 == hydro_idx
        # And the round-trip must agree with the pinned table.
        assert _HYDRO_TO_CALENDAR[hydro_idx] == cal_month

    def test_hydro_to_stage_schedule_full_cycle(self, tmp_path):
        """A 12-stage horizon covering Jan..Dec must shuffle the hydro array
        into calendar order.

        Uses ``LajaAgreement`` through a ``stage_parser``-shaped fake so the
        real ``_hydro_to_stage_schedule`` code path is exercised.
        """

        class _FakeStageParser:  # noqa: D401 — test shim
            def __init__(self, months):
                self._stages = [
                    {"number": i + 1, "month": m} for i, m in enumerate(months)
                ]

            def get_all(self):
                return self._stages

        cfg = _minimal_laja_config()
        # Unique marker per hydro index so the shuffled output is checkable.
        cfg["monthly_usage_irr"] = [float(i) for i in range(12)]
        cfg["monthly_cost_irr_ns"] = [1.0] * 12
        # Give fmax a distinct qmax so fmax_irr reflects the usage pattern.
        cfg["qmax_irr"] = 1.0

        calendar_order = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]
        stage_parser = _FakeStageParser(calendar_order)

        agreement = LajaAgreement(
            cfg, stage_parser=stage_parser, options={"blocks_per_stage": 1}
        )
        # The rendered entities are opaque, but we can poke the private
        # helper that's the unit under test.
        schedule = agreement._hydro_to_stage_schedule(cfg["monthly_usage_irr"])

        # Expected: for each calendar month m in stages, pick
        # cfg["monthly_usage_irr"][(m-4) % 12].
        expected = [float((m - 4) % 12) for m in calendar_order]
        assert schedule == expected

        # Smoke check: pampl still renders.
        agreement.generate_pampl(tmp_path)
        assert (tmp_path / "laja.pampl").exists()


# ---------------------------------------------------------------------------
# 12.2 — filtracion_laja round-trip (field is wired up → must survive)
# ---------------------------------------------------------------------------


class TestFiltracionLajaRoundTrip:
    """``filtracion_laja`` reaches ``laja.pampl`` verbatim."""

    def test_value_appears_in_pampl(self, tmp_path):
        cfg = _minimal_laja_config()
        cfg["filtracion_laja"] = 42.75
        text = _render_laja_pampl(cfg, tmp_path)
        assert "param filtracion_laja = 42.75;" in text

    def test_changing_value_changes_pampl(self, tmp_path):
        """Mutating ``filtracion_laja`` must perturb the rendered pampl.

        This is the positive counterpart to the xfail tests below: it
        proves the test methodology (build-two-agreements, diff output)
        actually detects field propagation when the field is wired up.
        """
        cfg_a = _minimal_laja_config()
        cfg_b = _minimal_laja_config()
        cfg_a["filtracion_laja"] = 10.0
        cfg_b["filtracion_laja"] = 99.5

        dir_a = tmp_path / "a"
        dir_b = tmp_path / "b"
        dir_a.mkdir()
        dir_b.mkdir()

        text_a = _render_laja_pampl(cfg_a, dir_a)
        text_b = _render_laja_pampl(cfg_b, dir_b)

        assert text_a != text_b
        assert "10.0" in text_a
        assert "99.5" in text_b


# ---------------------------------------------------------------------------
# 12.3 / 12.4 — Laja optional-override round-trip
# ---------------------------------------------------------------------------


class TestLajaOptionalOverrides:
    """``forced_flows`` and ``manual_withdrawals`` are exposed in laja.pampl.

    Each test builds two agreements that differ only in one override field
    and asserts the rendered pampl diverges.  These fields are emitted as
    AMPL ``param``/``set`` declarations (see ``templates/laja.tampl``); no
    constraint references them yet, but downstream AMPL models can read
    them to implement custom dispatch rules.
    """

    def test_forced_flows_affect_output(self, tmp_path):
        cfg_a = _minimal_laja_config()
        cfg_b = _minimal_laja_config()
        cfg_a["forced_flows"] = []
        cfg_b["forced_flows"] = [
            {"stage": 1, "flow": 50.0},
            {"stage": 2, "flow": 75.0},
        ]

        dir_a = tmp_path / "a"
        dir_b = tmp_path / "b"
        dir_a.mkdir()
        dir_b.mkdir()

        agr_a = LajaAgreement(copy.deepcopy(cfg_a))
        agr_b = LajaAgreement(copy.deepcopy(cfg_b))
        pampl_a = _render_laja_pampl(cfg_a, dir_a)
        pampl_b = _render_laja_pampl(cfg_b, dir_b)

        # Either the entity arrays must diverge OR the rendered pampl must
        # diverge — if neither changes, the field is silently dropped.
        assert _entities_json(agr_a) != _entities_json(agr_b) or pampl_a != pampl_b

    def test_manual_withdrawals_affect_output(self, tmp_path):
        cfg_a = _minimal_laja_config()
        cfg_b = _minimal_laja_config()
        cfg_a["manual_withdrawals"] = []
        cfg_b["manual_withdrawals"] = [
            {
                "stage": 1,
                "q_1o_reg": 40.0,
                "q_2o_reg": 0.0,
                "q_emergencia": 0.0,
                "q_saltos": 0.0,
            },
        ]

        dir_a = tmp_path / "a"
        dir_b = tmp_path / "b"
        dir_a.mkdir()
        dir_b.mkdir()

        agr_a = LajaAgreement(copy.deepcopy(cfg_a))
        agr_b = LajaAgreement(copy.deepcopy(cfg_b))
        pampl_a = _render_laja_pampl(cfg_a, dir_a)
        pampl_b = _render_laja_pampl(cfg_b, dir_b)

        assert _entities_json(agr_a) != _entities_json(agr_b) or pampl_a != pampl_b


# ---------------------------------------------------------------------------
# 12.5 / 12.6 / 12.7 — Maule storage-flag and target round-trip
# ---------------------------------------------------------------------------


class TestMauleOptionalParameters:
    """Maule storage-eligibility flags and season-end targets reach maule.pampl.

    Each test builds two agreements that differ only in one parameter and
    asserts the rendered pampl diverges.  These fields are emitted as
    AMPL ``param`` declarations (see ``templates/maule.tampl``); no
    constraint references them yet, but downstream AMPL models can read
    them to implement custom rules.
    """

    def test_flag_embalsa_affect_output(self, tmp_path):
        cfg_a = _minimal_maule_config()
        cfg_b = _minimal_maule_config()
        cfg_a["flag_embalsa_1"] = False
        cfg_a["flag_embalsa_2"] = False
        cfg_b["flag_embalsa_1"] = True
        cfg_b["flag_embalsa_2"] = True

        dir_a = tmp_path / "a"
        dir_b = tmp_path / "b"
        dir_a.mkdir()
        dir_b.mkdir()

        agr_a = MauleAgreement(copy.deepcopy(cfg_a))
        agr_b = MauleAgreement(copy.deepcopy(cfg_b))
        pampl_a = _render_maule_pampl(cfg_a, dir_a)
        pampl_b = _render_maule_pampl(cfg_b, dir_b)

        assert _entities_json(agr_a) != _entities_json(agr_b) or pampl_a != pampl_b

    def test_n_agnos_pct_manual_affect_output(self, tmp_path):
        cfg_a = _minimal_maule_config()
        cfg_b = _minimal_maule_config()
        cfg_a["n_agnos_pct_manual"] = 0
        cfg_b["n_agnos_pct_manual"] = 3

        dir_a = tmp_path / "a"
        dir_b = tmp_path / "b"
        dir_a.mkdir()
        dir_b.mkdir()

        agr_a = MauleAgreement(copy.deepcopy(cfg_a))
        agr_b = MauleAgreement(copy.deepcopy(cfg_b))
        pampl_a = _render_maule_pampl(cfg_a, dir_a)
        pampl_b = _render_maule_pampl(cfg_b, dir_b)

        assert _entities_json(agr_a) != _entities_json(agr_b) or pampl_a != pampl_b

    def test_vol_acum_riego_temp_affect_output(self, tmp_path):
        cfg_a = _minimal_maule_config()
        cfg_b = _minimal_maule_config()
        cfg_a["vol_acum_riego_temp"] = [0.0] * 12
        cfg_b["vol_acum_riego_temp"] = [
            100.0,
            200.0,
            300.0,
            400.0,
            500.0,
            600.0,
            700.0,
            800.0,
            750.0,
            600.0,
            400.0,
            200.0,
        ]

        dir_a = tmp_path / "a"
        dir_b = tmp_path / "b"
        dir_a.mkdir()
        dir_b.mkdir()

        agr_a = MauleAgreement(copy.deepcopy(cfg_a))
        agr_b = MauleAgreement(copy.deepcopy(cfg_b))
        pampl_a = _render_maule_pampl(cfg_a, dir_a)
        pampl_b = _render_maule_pampl(cfg_b, dir_b)

        assert _entities_json(agr_a) != _entities_json(agr_b) or pampl_a != pampl_b
