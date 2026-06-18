# SPDX-License-Identifier: BSD-3-Clause
"""Tests for ``gtopt_shared.description_meta``."""

from __future__ import annotations

import pytest

from gtopt_shared.description_meta import (
    append_meta,
    get_meta,
    parse_meta,
    strip_meta,
    update_meta,
)


# ---------------------------------------------------------------------------
# parse_meta
# ---------------------------------------------------------------------------


def test_parse_meta_empty_inputs():
    empty: dict[str, str] = {}
    assert parse_meta(None) == empty
    assert parse_meta("") == empty
    assert parse_meta("Plain prose; nothing structured.") == empty


def test_parse_meta_key_value_pairs():
    desc = "PLEXOS biomass cogen. [gtopt-meta cogen_mode=dispatched plant=ARAUCO]"
    assert parse_meta(desc) == {"cogen_mode": "dispatched", "plant": "ARAUCO"}


def test_parse_meta_boolean_shorthand():
    assert parse_meta("[gtopt-meta is_phantom]") == {"is_phantom": True}
    assert parse_meta("foo; [gtopt-meta phantom_bus is_cogen=0]") == {
        "phantom_bus": True,
        "is_cogen": "0",
    }


def test_parse_meta_multi_value_string_kept_raw():
    """Comma-separated values are returned as a single string — caller
    splits if it wants a list.  Keeps the grammar narrow."""
    desc = "[gtopt-meta tags=must_run,priority,backup]"
    assert parse_meta(desc) == {"tags": "must_run,priority,backup"}


def test_parse_meta_rightmost_block_wins():
    """Defensive: when two blocks are present, the rightmost takes
    precedence.  Producers should use append_meta which strips first;
    parse_meta is just tolerant on the consumer side."""
    desc = "[gtopt-meta foo=bar] middle [gtopt-meta foo=qux baz=2]"
    assert parse_meta(desc) == {"foo": "qux", "baz": "2"}


def test_parse_meta_handles_extra_whitespace():
    desc = "[gtopt-meta  cogen_mode=must_run  plant=X]"
    assert parse_meta(desc) == {"cogen_mode": "must_run", "plant": "X"}


# ---------------------------------------------------------------------------
# strip_meta
# ---------------------------------------------------------------------------


def test_strip_meta_removes_block_and_separator():
    desc = "PLEXOS biomass cogen; [gtopt-meta cogen_mode=must_run]"
    assert strip_meta(desc) == "PLEXOS biomass cogen"


def test_strip_meta_no_block_returns_prose():
    assert strip_meta("Plain prose.") == "Plain prose."


def test_strip_meta_only_block_returns_empty():
    assert strip_meta("[gtopt-meta a=1]") == ""


def test_strip_meta_empty_inputs():
    assert strip_meta(None) == ""
    assert strip_meta("") == ""


def test_strip_meta_multiple_blocks_all_removed():
    desc = "Pre; [gtopt-meta a=1] mid; [gtopt-meta b=2]"
    assert strip_meta(desc) == "Pre;  mid"


# ---------------------------------------------------------------------------
# append_meta
# ---------------------------------------------------------------------------


def test_append_meta_to_empty():
    assert append_meta(None, cogen_mode="dispatched") == (
        "[gtopt-meta cogen_mode=dispatched]"
    )
    assert append_meta("", cogen_mode="dispatched") == (
        "[gtopt-meta cogen_mode=dispatched]"
    )


def test_append_meta_to_prose():
    out = append_meta("PLEXOS biomass cogen", cogen_mode="dispatched")
    assert out == "PLEXOS biomass cogen; [gtopt-meta cogen_mode=dispatched]"


def test_append_meta_is_idempotent_on_rewrite():
    """Calling append_meta twice replaces the block — no stacking."""
    first = append_meta("Plant X", cogen_mode="dispatched")
    second = append_meta(first, cogen_mode="must_run")
    assert second == "Plant X; [gtopt-meta cogen_mode=must_run]"
    # Block count stays at 1
    assert second.count("[gtopt-meta") == 1


def test_append_meta_skips_false_and_none():
    out = append_meta("prose", flag_a=True, flag_b=False, flag_c=None, key="v")
    # b and c omitted; a is bare; key=v keyword
    assert out == "prose; [gtopt-meta flag_a key=v]"


def test_append_meta_no_meta_returns_clean_prose():
    """When all values are False/None, no block is appended."""
    assert append_meta("prose", flag=False, other=None) == "prose"
    assert append_meta(None, flag=False) == ""


def test_append_meta_sorts_keys_for_stable_output():
    """Sorted keys → diff-stable across runs."""
    out = append_meta(None, zebra="last", apple="first", mango="mid")
    assert out == "[gtopt-meta apple=first mango=mid zebra=last]"


def test_append_meta_coerces_non_string_values():
    out = append_meta(None, count=42, ratio=0.5)
    assert out == "[gtopt-meta count=42 ratio=0.5]"


def test_append_meta_rejects_invalid_key():
    with pytest.raises(ValueError, match="key must match"):
        append_meta(None, **{"Bad-Key": "v"})
    with pytest.raises(ValueError, match="key must match"):
        append_meta(None, **{"with space": "v"})


def test_append_meta_rejects_invalid_value():
    with pytest.raises(ValueError, match="value must match"):
        append_meta(None, key="has space")
    with pytest.raises(ValueError, match="value must match"):
        append_meta(None, key="has]bracket")
    with pytest.raises(ValueError, match="value cannot be empty"):
        append_meta(None, key="")


# ---------------------------------------------------------------------------
# update_meta — preserves existing keys
# ---------------------------------------------------------------------------


def test_update_meta_merges_with_existing():
    desc = "prose; [gtopt-meta cogen_mode=dispatched plant=X]"
    out = update_meta(desc, mode_variant="primary")
    parsed = parse_meta(out)
    assert parsed == {
        "cogen_mode": "dispatched",
        "plant": "X",
        "mode_variant": "primary",
    }


def test_update_meta_overrides_existing_key():
    desc = "prose; [gtopt-meta cogen_mode=dispatched]"
    out = update_meta(desc, cogen_mode="must_run")
    assert parse_meta(out) == {"cogen_mode": "must_run"}


def test_update_meta_drops_key_when_set_to_false():
    desc = "prose; [gtopt-meta cogen_mode=dispatched flag]"
    out = update_meta(desc, flag=False)
    assert parse_meta(out) == {"cogen_mode": "dispatched"}


# ---------------------------------------------------------------------------
# get_meta convenience accessor
# ---------------------------------------------------------------------------


def test_get_meta_returns_value():
    desc = "[gtopt-meta cogen_mode=must_run plant=X]"
    assert get_meta(desc, "cogen_mode") == "must_run"
    assert get_meta(desc, "plant") == "X"


def test_get_meta_returns_default_when_absent():
    desc = "[gtopt-meta foo=bar]"
    assert get_meta(desc, "missing") is None
    assert get_meta(desc, "missing", default="fallback") == "fallback"


def test_get_meta_boolean_shorthand():
    desc = "[gtopt-meta is_phantom]"
    assert get_meta(desc, "is_phantom") is True
