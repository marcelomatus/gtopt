"""Unit tests for :mod:`plexos2gtopt._uc_policy`.

The classifier centralises the entire regex-+-penalty ladder that used to
live inline in :func:`plexos2gtopt.parsers.extract_user_constraints`.  These
tests pin every branch so a future tweak to the policy module surfaces in
CI rather than being noticed weeks later in a PLEXOS-vs-gtopt comparison
drift.

One test case per branch in :func:`_uc_policy.classify`, plus a couple of
combination cases exercising the priority ordering (inactive wins over
shape, PLEXOS-supplied penalty wins over the default ladder, the
Gas_MaxOpDay override wins over the LHS-shape branches, …).
"""

from __future__ import annotations

import pytest

from plexos2gtopt._uc_policy import (
    HYDRO_SOFT,
    RESERVE_PROV_SUM,
    classify,
)
from plexos2gtopt.entities import ConstraintDirective


# Shared ``hard_set`` fixture — names confirmed PLEXOS-HARD by audit.  The
# real loader (:func:`parsers._load_plexos_hard_uc_list`) reads
# ``data/cen_pcp_hard_ucs.txt``; the classifier itself only needs the
# membership semantics so a plain set is enough.
_HARD: frozenset[str] = frozenset({"MACHICURA_GENT4def", "PEHUENCHE_GENT7def"})


def _call(
    *,
    name: str = "X",
    expression: str = '1 * generator("G").generation >= 0',
    op: str = ">=",
    plexos_penalty: float = 0.0,
    is_inactive: bool = False,
    hard_set: frozenset[str] = _HARD,
):
    """Tiny adapter so each test case reads as a single assert line."""
    return classify(
        constraint_name=name,
        expression=expression,
        op=op,
        plexos_penalty=plexos_penalty,
        is_inactive=is_inactive,
        hard_set=hard_set,
    )


# ── Priority-0: inactive constraints keep PLEXOS-supplied penalty ──────────
class TestInactivePassThrough:
    """``active = False`` ⇒ no reclassification, no directive."""

    def test_inactive_zero_penalty_stays_zero(self):
        out = _call(is_inactive=True, plexos_penalty=0.0)
        assert out.penalty == 0.0
        assert out.directive is None

    def test_inactive_nonzero_penalty_preserved(self):
        out = _call(is_inactive=True, plexos_penalty=42.5)
        assert out.penalty == 42.5
        assert out.directive is None


# ── Priority-1: PLEXOS-supplied penalty wins for active rows ──────────────
class TestPlexosPenaltyWins:
    """Any positive ``Penalty Price`` from PLEXOS is honoured verbatim."""

    def test_plexos_supplied_penalty_honoured(self):
        out = _call(plexos_penalty=123.0)
        assert out.penalty == 123.0
        assert out.directive is None

    def test_plexos_penalty_overrides_regrange_default(self):
        """RegRange shape + PLEXOS penalty 25 ⇒ keep 25, no directive."""
        out = _call(
            name="X_RegRange_e1",
            expression=(
                '1 * commitment("uc_g").status + 1 * generator("g").generation >= 0'
            ),
            plexos_penalty=25.0,
        )
        assert out.penalty == 25.0
        assert out.directive is None


# ── Priority-3: Gas_MaxOpDay / limited_generation_calculation override ────
class TestFuelOfftakeOverride:
    """``Gas_MaxOpDay*`` and ``limited_generation_calculation`` land on the
    high-soft tier with a ``reserve_prov_sum`` directive when active and
    not in ``hard_set``."""

    def test_gas_maxopday_high_soft_with_directive(self):
        out = _call(name="Gas_MaxOpDayENEL")
        assert out.penalty == RESERVE_PROV_SUM
        assert isinstance(out.directive, ConstraintDirective)
        assert out.directive.kind == "reserve_prov_sum"
        assert out.directive.penalty == RESERVE_PROV_SUM

    def test_limited_generation_calculation_high_soft(self):
        out = _call(name="limited_generation_calculation")
        assert out.penalty == RESERVE_PROV_SUM
        assert out.directive is not None
        assert out.directive.kind == "reserve_prov_sum"

    def test_gas_maxopday_in_hard_set_falls_through(self):
        """Audit override: when a Gas_MaxOpDay row is in ``hard_set`` it
        must NOT take the high-soft fallback — it should hit the HARD
        ``is_plexos_hard_singleton`` branch below."""
        hard = frozenset({"Gas_MaxOpDayENEL"})
        out = _call(name="Gas_MaxOpDayENEL", hard_set=hard)
        assert out.penalty == 0.0
        assert out.directive is None


# ── Priority-4a: HARD branches ─────────────────────────────────────────────
class TestHardBranches:
    def test_def_equation_is_hard(self):
        out = _call(
            name="CSF_LW_Def",
            expression=(
                '-1 * decision_variable("CSF_LW_Requirement").value + '
                '1 * reserve_provision("p").dn = 0'
            ),
            op="=",
        )
        assert out.penalty == 0.0
        assert out.directive is None

    def test_def_equation_requires_equality_op(self):
        """``_Def`` name with op ``>=`` is NOT a definitional equation."""
        out = _call(
            name="X_Def",
            expression='-1 * decision_variable("Y").value >= 0',
            op=">=",
        )
        # Falls through to the catch-all (no commitment / line refs)
        assert out.penalty == HYDRO_SOFT

    def test_bat_cf_gen_comp_is_hard(self):
        out = _call(
            name="BAT_X_CF_GEN_COMP",
            expression=(
                '1 * battery("BAT").discharge - 1 * reserve_provision("BAT").up <= 0'
            ),
            op="<=",
        )
        assert out.penalty == 0.0
        assert out.directive is None

    def test_bat_cf_load_comp_is_hard(self):
        out = _call(name="BAT_Y_CF_LOAD_COMP", op="<=")
        assert out.penalty == 0.0
        assert out.directive is None

    def test_plexos_hard_singleton(self):
        out = _call(name="MACHICURA_GENT4def", op="=")
        assert out.penalty == 0.0
        assert out.directive is None


# ── Priority-4c: MAXCSF placeholders demote to operational tier ──────────
class TestMaxcsfPlaceholder:
    def test_maxcsf_demoted_to_hydro_soft(self):
        out = _call(name="MAXCSF_ANGAMOS")
        assert out.penalty == HYDRO_SOFT
        assert out.directive is None


# ── Priority-4d: RegRange / ReserveProvSum get typed directives ───────────
class TestRegrangeReserveSum:
    def test_regrange_emits_typed_directive(self):
        out = _call(
            name="ATA_RegRange_e1",
            expression=(
                '1 * commitment("uc_g").status + 1 * generator("g").generation >= 0'
            ),
        )
        assert out.penalty == RESERVE_PROV_SUM
        assert out.directive is not None
        assert out.directive.kind == "regrange"
        assert out.directive.penalty == RESERVE_PROV_SUM

    def test_regrange_with_reserve_provision_atom(self):
        out = _call(
            name="X_RegRange_e2",
            expression=(
                '1 * commitment("uc_g").status + 1 * reserve_provision("p").up >= 0'
            ),
        )
        assert out.directive is not None
        assert out.directive.kind == "regrange"

    def test_pure_reserve_aggregator_3plus(self):
        """≥3 reserve_provision refs + no other atom kinds ⇒ reserve_prov_sum."""
        out = _call(
            name="CSF_DownMinProvision",
            expression=(
                '1 * reserve_provision("a").dn + '
                '1 * reserve_provision("b").dn + '
                '1 * reserve_provision("c").dn >= 0'
            ),
        )
        assert out.penalty == RESERVE_PROV_SUM
        assert out.directive is not None
        assert out.directive.kind == "reserve_prov_sum"

    def test_reserve_calculation_with_decision_variable(self):
        out = _call(
            name="CPF_DownCalculation",
            expression=(
                '-1 * decision_variable("CPF_DownReq").value + '
                '1 * decision_variable("Generation_SEN").value = 0'
            ),
            op="=",
        )
        assert out.penalty == RESERVE_PROV_SUM
        assert out.directive is not None
        assert out.directive.kind == "reserve_prov_sum"

    def test_two_reserve_provs_not_aggregator(self):
        """≤2 reserve_provision refs ⇒ not classified as the aggregator."""
        out = _call(
            name="X",
            expression=(
                '1 * reserve_provision("a").up + 1 * reserve_provision("b").up >= 0'
            ),
        )
        # Falls through to catch-all (no commitment / line refs)
        assert out.penalty == HYDRO_SOFT


# ── Priority-4e: commitment-referencing / pure line flow stay HARD ────────
class TestStructuralHardFallback:
    def test_pure_line_flow_is_hard(self):
        out = _call(
            name="SD_2024_X",
            expression='1 * line("A->B").flow <= 100',
            op="<=",
        )
        assert out.penalty == 0.0
        assert out.directive is None

    def test_commitment_reference_is_hard(self):
        out = _call(
            name="Campiche_starting",
            expression='1 * commitment("uc_Campiche").startup <= 0',
            op="<=",
        )
        assert out.penalty == 0.0
        assert out.directive is None

    def test_nventanas_starting_hard(self):
        out = _call(
            name="NVentanas_starting",
            expression='1 * commitment("uc_NV").status = 0',
            op="=",
        )
        assert out.penalty == 0.0


# ── Priority-4f: hydro operational floor catch-all ───────────────────────
class TestHydroCatchAll:
    def test_hydro_min_floor_soft(self):
        out = _call(
            name="ANTUCOmin",
            expression=(
                '1 * generator("ANTUCO_U1").generation + '
                '1 * generator("ANTUCO_U2").generation >= 137'
            ),
            op=">=",
        )
        assert out.penalty == HYDRO_SOFT
        assert out.directive is None

    def test_pure_generator_cap_soft(self):
        out = _call(
            name="ELTOROmax",
            expression='1 * generator("ELTORO_U1").generation <= 100',
            op="<=",
        )
        assert out.penalty == HYDRO_SOFT
        assert out.directive is None


# ── Cross-cutting invariants ────────────────────────────────────────────
class TestPolicyInvariants:
    """Invariants every classify() result must satisfy."""

    def test_directive_penalty_matches_outcome_penalty(self):
        """When a directive carries ``penalty``, it must equal the outcome
        scalar — the two should never drift (LP-build reads whichever wins
        per ``UserConstraintLP::effective_penalty`` but plexos2gtopt sets
        both for backward compat)."""
        for o in (
            _call(
                name="X_RegRange_e1",
                expression='1 * commitment("c").status + 1 * generator("g").generation >= 0',
            ),
            _call(
                name="CSF_DownMinProvision",
                expression=(
                    '1 * reserve_provision("a").dn + '
                    '1 * reserve_provision("b").dn + '
                    '1 * reserve_provision("c").dn >= 0'
                ),
            ),
            _call(name="Gas_MaxOpDayX"),
        ):
            assert o.directive is not None
            assert o.directive.penalty == pytest.approx(o.penalty)

    def test_hard_outcomes_have_no_directive(self):
        """HARD (penalty=0) outcomes never carry a directive on the
        Python side; the C++ schema disallows ``HydroFloor``/
        ``MaxStartsWindow`` payloads with a penalty so we keep the
        Python emission to None for the simple hard case."""
        for o in (
            _call(name="MACHICURA_GENT4def"),
            _call(
                name="X_Def", expression='-1 * decision_variable("y").value = 0', op="="
            ),
            _call(
                name="SD_2024_LineFlow",
                expression='1 * line("A->B").flow <= 100',
                op="<=",
            ),
        ):
            assert o.penalty == 0.0
            assert o.directive is None

    def test_classify_is_pure(self):
        """Same inputs ⇒ same outputs across repeated calls (no I/O,
        no env-var dependency)."""
        kwargs = {
            "constraint_name": "ANTUCOmin",
            "expression": '1 * generator("u").generation >= 137',
            "op": ">=",
            "plexos_penalty": 0.0,
            "is_inactive": False,
            "hard_set": _HARD,
        }
        first = classify(**kwargs)
        second = classify(**kwargs)
        assert first == second
