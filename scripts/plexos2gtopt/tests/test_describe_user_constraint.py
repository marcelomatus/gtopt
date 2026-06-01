# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :func:`_describe_user_constraint` and
:func:`_is_tautology_single_term`.

These cover the three fixes landed in response to the CEN PCP
2026-04-22 bundle audit:

* the ``Σ`` template was stamped unconditionally even for 1-term rows
  (41 / 41 single-term rows in that bundle carried a misleading
  ``Σ commitment status [0/1]`` comment);
* the comment claimed ``commitment status`` even when the actual term
  used ``.startup`` / ``.shutdown``;
* tautological single-term rows (``±1·u ≤ 1`` etc.) survived as
  active LP rows even though PLEXOS itself never binds them.
"""

from __future__ import annotations

import pytest

from plexos2gtopt.parsers import (
    _describe_user_constraint,
    _is_tautology_single_term,
)


class TestSigmaPrefix:
    """``Σ`` only when the constraint is actually a sum (n_terms > 1)."""

    def test_single_term_drops_sigma(self) -> None:
        desc = _describe_user_constraint(
            "SSCC_NVentanas_Down",
            '1 * commitment("uc_NUEVA_VENTANAS").status <= 1',
            "<=",
            1.0,
            source_file="DBSEN_PRGDIARIO.xml",
            n_terms=1,
        )
        # No Σ for a 1-term row.
        assert " Σ " not in desc
        # The LHS label still names the variable kind.
        assert "commitment status [0/1]" in desc

    def test_multi_term_keeps_sigma(self) -> None:
        desc = _describe_user_constraint(
            "TOCOPILLA_U16_CC_DIE",
            '1 * commitment("uc_X").status + 1 * commitment("uc_Y").status <= 1',
            "<=",
            1.0,
            source_file="DBSEN_PRGDIARIO.xml",
            n_terms=2,
        )
        assert "Σ" in desc

    def test_zero_term_default_keeps_template(self) -> None:
        # n_terms=0 (caller omitted it) → drops Σ as well, matching
        # the conservative default.
        desc = _describe_user_constraint(
            "Empty",
            "0 <= 0",
            "<=",
            0.0,
            source_file="DBSEN_PRGDIARIO.xml",
        )
        assert "Σ" not in desc


class TestAccessorLabel:
    """Comment matches the actual ``.status`` / ``.startup`` / ``.shutdown``
    accessor used in the expression."""

    def test_status_only(self) -> None:
        desc = _describe_user_constraint(
            "X",
            '1 * commitment("uc_X").status <= 1',
            "<=",
            1.0,
            source_file="f",
            n_terms=1,
        )
        assert "commitment status [0/1]" in desc

    def test_startup_only(self) -> None:
        desc = _describe_user_constraint(
            "Campiche_starting",
            '1 * commitment("uc_CAMPICHE").startup <= 0',
            "<=",
            0.0,
            source_file="f",
            n_terms=1,
        )
        # Should label as startup, not status.
        assert "commitment startup [0/1]" in desc
        assert "commitment status [0/1]" not in desc

    def test_shutdown_only(self) -> None:
        desc = _describe_user_constraint(
            "ATA_TG1B_DIE_SSCC",
            '1 * commitment("uc_ATA_CC1_TGB_GNL").shutdown <= 1',
            "<=",
            1.0,
            source_file="f",
            n_terms=1,
        )
        assert "commitment shutdown [0/1]" in desc

    def test_mixed_accessors(self) -> None:
        desc = _describe_user_constraint(
            "Mixed",
            '1 * commitment("uc_X").startup + 1 * commitment("uc_X").shutdown <= 1',
            "<=",
            1.0,
            source_file="f",
            n_terms=2,
        )
        assert "commitment startup/shutdown [0/1]" in desc


class TestProvenanceAnnotation:
    """When ``n_filtered`` > 0 the description records the original
    PLEXOS membership count so audits against the source data round
    trip."""

    def test_no_filtered_no_annotation(self) -> None:
        desc = _describe_user_constraint(
            "X",
            '1 * commitment("uc_X").status <= 1',
            "<=",
            1.0,
            source_file="f",
            n_terms=1,
            n_filtered=0,
        )
        assert "consolidated from" not in desc

    def test_filtered_count_in_description(self) -> None:
        desc = _describe_user_constraint(
            "ATA_TG1B_DIE_SSCC",
            '1 * commitment("uc_ATA_CC1_TGB_GNL").shutdown <= 1',
            "<=",
            1.0,
            source_file="DBSEN_PRGDIARIO.xml",
            n_terms=1,
            n_filtered=73,
        )
        assert "consolidated from 74 PLEXOS member(s)" in desc
        assert "73 subsumed" in desc


class TestTautologyAnnotation:
    """The description carries a ``tautology under binary [0,1] domain``
    suffix when the caller flagged the row as LP-redundant."""

    def test_tautology_suffix(self) -> None:
        desc = _describe_user_constraint(
            "SSCC_NVentanas_Down",
            '1 * commitment("uc_NUEVA_VENTANAS").status <= 1',
            "<=",
            1.0,
            source_file="f",
            n_terms=1,
            tautology=True,
        )
        assert "tautology under binary [0,1] domain" in desc

    def test_no_tautology_no_suffix(self) -> None:
        desc = _describe_user_constraint(
            "X",
            '1 * commitment("uc_X").status >= 1',
            ">=",
            1.0,
            source_file="f",
            n_terms=1,
            tautology=False,
        )
        assert "tautology" not in desc


class TestIsTautologySingleTerm:
    """The single-term binary-domain tautology rule.

    LHS range for ``c·u`` with ``u ∈ [0, 1]`` is
    ``[min(0, c), max(0, c)]``.  Constraint ``LHS ≤ rhs`` is tautology
    iff ``rhs ≥ max(0, c)``; ``LHS ≥ rhs`` iff ``rhs ≤ min(0, c)``;
    ``LHS = rhs`` iff ``c = 0`` and ``rhs = 0``.
    """

    def test_positive_coef_le_one(self) -> None:
        # +1·u ≤ 1: tautology (u ∈ [0,1] always satisfies).
        terms = ['1 * commitment("uc_X").status']
        assert _is_tautology_single_term(terms, "<=", 1.0) is True

    def test_positive_coef_le_zero(self) -> None:
        # +1·u ≤ 0: NOT a tautology (forces u = 0).
        terms = ['1 * commitment("uc_X").startup']
        assert _is_tautology_single_term(terms, "<=", 0.0) is False

    def test_positive_coef_ge_zero(self) -> None:
        # +1·u ≥ 0: tautology (u ≥ 0 by domain).
        terms = ['1 * commitment("uc_X").status']
        assert _is_tautology_single_term(terms, ">=", 0.0) is True

    def test_positive_coef_ge_one(self) -> None:
        # +1·u ≥ 1: NOT a tautology (forces u = 1).
        terms = ['1 * commitment("uc_X").status']
        assert _is_tautology_single_term(terms, ">=", 1.0) is False

    def test_negative_coef_ge_minus_one(self) -> None:
        # -1·u ≥ -1: tautology (u ≤ 1 by domain).
        terms = ['-1 * commitment("uc_X").status']
        assert _is_tautology_single_term(terms, ">=", -1.0) is True

    def test_negative_coef_ge_zero(self) -> None:
        # -5·u ≥ 0: forces u ≤ 0, NOT a tautology — this is the
        # PLEXOS BAT_*_FV pattern that DOES bind the battery off.
        terms = ['-5 * commitment("uc_BAT_X_gen").status']
        assert _is_tautology_single_term(terms, ">=", 0.0) is False

    def test_scaled_le_rhs(self) -> None:
        # 36.4 · u ≤ 36.4: tautology (max LHS = 36.4 when u=1).
        terms = ['36.4 * commitment("uc_X").status']
        assert _is_tautology_single_term(terms, "<=", 36.4) is True

    def test_scaled_le_below_max(self) -> None:
        # 36.4 · u ≤ 30: NOT a tautology (LP would force u < 0.825).
        terms = ['36.4 * commitment("uc_X").status']
        assert _is_tautology_single_term(terms, "<=", 30.0) is False

    def test_multi_term_never_tautology(self) -> None:
        # The detector is single-term only; multi-term sums require a
        # broader analysis out of scope here.
        terms = [
            '1 * commitment("uc_A").status',
            '1 * commitment("uc_B").status',
        ]
        assert _is_tautology_single_term(terms, "<=", 1.0) is False

    def test_non_commitment_never_tautology(self) -> None:
        # Generator dispatch terms aren't bounded in [0, 1]; the
        # detector intentionally returns False.
        terms = ['1 * generator("g").generation']
        assert _is_tautology_single_term(terms, "<=", 1.0) is False

    def test_startup_shutdown_covered(self) -> None:
        # Same binary domain → same tautology rule.
        assert (
            _is_tautology_single_term(['1 * commitment("uc_X").startup'], "<=", 1.0)
            is True
        )
        assert (
            _is_tautology_single_term(['1 * commitment("uc_X").shutdown'], "<=", 1.0)
            is True
        )


class TestFuelOfftakePathUnchanged:
    """The fuel-offtake branch keeps its explicit ``Σ heat_rate·generation``
    template (it is always a real sum)."""

    def test_fuel_offtake_template_intact(self) -> None:
        desc = _describe_user_constraint(
            "Diesel_OffTakeDay",
            "...",
            "<=",
            9277.0,
            source_file="DBSEN_PRGDIARIO.xml",
            fuel_offtake=True,
        )
        assert "Σ heat_rate·generation" in desc


@pytest.mark.parametrize(
    ("inactive", "tautology", "expected_substr"),
    [
        (True, False, "active=False"),
        (False, True, "tautology"),
        (True, True, "tautology"),
        (False, False, ""),
    ],
)
def test_inactive_and_tautology_suffixes(
    *, inactive: bool, tautology: bool, expected_substr: str
) -> None:
    desc = _describe_user_constraint(
        "X",
        '1 * commitment("uc_X").status <= 1',
        "<=",
        1.0,
        source_file="f",
        n_terms=1,
        inactive=inactive,
        tautology=tautology,
    )
    if expected_substr:
        assert expected_substr in desc
    if not inactive and not tautology:
        assert "active=False" not in desc
        assert "tautology" not in desc
