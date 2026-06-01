# SPDX-License-Identifier: BSD-3-Clause
"""Tests for :mod:`gtopt_shared.pampl_ident`."""

from __future__ import annotations

from gtopt_shared.pampl_ident import (
    PENALTY_TIER_NAMES,
    pampl_ident,
    penalty_param_name,
)


def test_plain_identifier_passes_through() -> None:
    assert pampl_ident("ELToro_Pmin") == "ELToro_Pmin"
    assert pampl_ident("_already_valid") == "_already_valid"


def test_hyphen_replaced_with_underscore() -> None:
    assert pampl_ident("Cap-ricornio-LaNegra") == "Cap_ricornio_LaNegra"


def test_dot_paren_space_replaced() -> None:
    assert pampl_ident("Antuco (Pmin) v.1") == "Antuco__Pmin__v_1"


def test_leading_digit_gets_uc_prefix() -> None:
    assert pampl_ident("2024084134_constraint") == "uc_2024084134_constraint"


def test_empty_input_gets_uc_prefix() -> None:
    assert pampl_ident("") == "uc_"


def test_only_invalid_chars_become_underscores() -> None:
    # Sanitized result starts with '_' which is a valid PAMPL ident
    # start char, so no ``uc_`` prefix is needed.
    assert pampl_ident("---") == "___"


def test_leading_invalid_becomes_underscore_no_prefix() -> None:
    # Leading invalid char becomes '_' which is itself a valid ident
    # start char — no ``uc_`` prefix needed.
    assert pampl_ident("@0xfeed") == "_0xfeed"


def test_idempotent_on_sanitised_input() -> None:
    once = pampl_ident("Antuco-min")
    assert pampl_ident(once) == once


def test_penalty_tier_named() -> None:
    assert PENALTY_TIER_NAMES[10.0] == "soft_floor_penalty"
    assert penalty_param_name(10.0) == "soft_floor_penalty"


def test_penalty_value_derived_name() -> None:
    # 467.19 → "penalty_467_19"
    assert penalty_param_name(467.19) == "penalty_467_19"
    # 1e3 -> "penalty_1000"
    assert penalty_param_name(1000.0) == "penalty_1000"


def test_penalty_negative_value_strips_sign() -> None:
    # The non-digit dash is replaced with '_'. We aren't using this
    # path in production but the function should not crash.
    assert penalty_param_name(-5.0) == "penalty__5"
