# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for ``gtopt_shared.json_utils``.

Coverage:
* ``strip_internal_keys`` drops top-level ``_``-prefixed keys, keeps the rest.
* ``sanitize_inf`` substitutes ``±math.inf`` with the numeric
  ``±sys.float_info.max`` (DblMax) sentinel and recurses into nested
  dicts / lists.
* The default omit-key set (``fmax`` / ``fmin``) drops the field entirely
  when its value is exact ``±inf``.  Large finite values (e.g. PLP's
  ``1e30`` soft-inf) flow through unchanged — gtopt's solver-aware
  ``is_infinity()`` handles clamping at the LP layer.
* Custom ``omit_keys`` overrides extend the dropped-key set.
* Non-numeric values pass through untouched.
"""

from __future__ import annotations

import math
import sys

import pytest

from gtopt_shared.json_utils import (
    DEFAULT_INF_OMIT_KEYS,
    sanitize_inf,
    strip_internal_keys,
)


# ---------------------------------------------------------------------------
# strip_internal_keys
# ---------------------------------------------------------------------------


def test_strip_internal_keys_drops_underscore_keys() -> None:
    planning = {
        "options": {"a": 1},
        "system": {"b": 2},
        "_progress": "marker",
        "_excel_hint": [1, 2, 3],
    }
    cleaned = strip_internal_keys(planning)
    assert "options" in cleaned
    assert "system" in cleaned
    assert "_progress" not in cleaned
    assert "_excel_hint" not in cleaned


def test_strip_internal_keys_only_top_level() -> None:
    """Nested ``_``-prefixed keys are the caller's responsibility."""
    planning = {"system": {"_internal": "kept"}}
    cleaned = strip_internal_keys(planning)
    assert cleaned["system"]["_internal"] == "kept"


def test_strip_internal_keys_returns_new_dict() -> None:
    src = {"a": 1, "_x": 2}
    cleaned = strip_internal_keys(src)
    assert cleaned is not src
    assert "_x" in src  # source not mutated


# ---------------------------------------------------------------------------
# sanitize_inf — default omit-key behaviour
# ---------------------------------------------------------------------------


def test_sanitize_inf_omits_fmax_inf() -> None:
    obj = {"fmax": math.inf, "name": "w1"}
    out = sanitize_inf(obj)
    assert "fmax" not in out
    assert out == {"name": "w1"}


def test_sanitize_inf_omits_fmin_neg_inf() -> None:
    obj = {"fmin": -math.inf, "name": "w1"}
    out = sanitize_inf(obj)
    assert "fmin" not in out


def test_sanitize_inf_keeps_plp_soft_inf_sentinel() -> None:
    """PLP ships ``1e30`` as a soft-infinity sentinel.  Past versions of
    sanitize_inf treated ``≥ 1e20`` as inf and dropped the field, but
    that threshold was solver-specific (1e20 is CPLEX's infinity, 1e30
    is HiGHS's).  The writer no longer guesses: large finite values
    flow through unchanged and gtopt's ``is_infinity()`` clamps them
    at the LP layer per the configured solver.
    """
    obj = {"fmax": 1e30, "fmin": -1e30, "name": "w1"}
    out = sanitize_inf(obj)
    assert out["fmax"] == 1e30
    assert out["fmin"] == -1e30


# ---------------------------------------------------------------------------
# sanitize_inf — sentinel serialisation
# ---------------------------------------------------------------------------


def test_sanitize_inf_serialises_pos_inf_to_sentinel() -> None:
    obj = {"some_other_field": math.inf}
    out = sanitize_inf(obj)
    assert out["some_other_field"] == sys.float_info.max


def test_sanitize_inf_serialises_neg_inf_to_sentinel() -> None:
    obj = {"some_other_field": -math.inf}
    out = sanitize_inf(obj)
    assert out["some_other_field"] == -sys.float_info.max


def test_sanitize_inf_walks_nested_dict() -> None:
    obj = {"system": {"waterway_array": [{"fmax": math.inf, "v": 5.0}]}}
    out = sanitize_inf(obj)
    waterway = out["system"]["waterway_array"][0]
    assert "fmax" not in waterway
    assert waterway["v"] == 5.0


def test_sanitize_inf_walks_inside_list_serialising_inf() -> None:
    obj = [math.inf, -math.inf, 1.0, "x"]
    out = sanitize_inf(obj)
    assert out == [sys.float_info.max, -sys.float_info.max, 1.0, "x"]


# ---------------------------------------------------------------------------
# sanitize_inf — custom omit_keys
# ---------------------------------------------------------------------------


def test_sanitize_inf_custom_omit_keys() -> None:
    obj = {"my_field": math.inf, "fmax": math.inf, "other": math.inf}
    out = sanitize_inf(obj, omit_keys=frozenset({"my_field"}))
    assert "my_field" not in out
    # fmax + other now serialise to the DblMax sentinel because the
    # user's explicit set replaces the default.
    assert out["fmax"] == sys.float_info.max
    assert out["other"] == sys.float_info.max


def test_default_omit_keys_value() -> None:
    """Document the default set as a hard invariant for downstream."""
    assert "fmax" in DEFAULT_INF_OMIT_KEYS
    assert "fmin" in DEFAULT_INF_OMIT_KEYS


# ---------------------------------------------------------------------------
# Pass-through
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "value",
    [None, 0, 1, -1, 3.14, "string", True, False, [], {}],
)
def test_sanitize_inf_passes_through_non_inf_scalars(value: object) -> None:
    assert sanitize_inf(value) == value


def test_sanitize_inf_finite_floats_unchanged_inside_dict() -> None:
    obj = {"a": 1.5, "b": -2.5, "fmax": 100.0}
    out = sanitize_inf(obj)
    assert out["a"] == 1.5
    assert out["b"] == -2.5
    assert out["fmax"] == 100.0  # finite — kept
