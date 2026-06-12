# SPDX-License-Identifier: BSD-3-Clause
"""Structural + provenance tests for :mod:`gtopt_shared.field_meta`.

Pins the public surface of the lifted ``field_meta`` module: the
``FIELD_META`` dict shape (array_name → list of (field, type, required,
desc, example) tuples) and the legacy ``igtopt._field_meta.FIELD_META``
shim.
"""

from __future__ import annotations

from gtopt_shared import field_meta


def test_field_meta_is_a_dict() -> None:
    """``FIELD_META`` is a non-empty dict keyed by array name."""
    assert isinstance(field_meta.FIELD_META, dict)
    assert field_meta.FIELD_META, "FIELD_META is empty"


def test_field_meta_entries_have_five_tuple_shape() -> None:
    """Each row is ``(field, type, required, description, example)``."""
    for array_name, rows in field_meta.FIELD_META.items():
        assert isinstance(rows, list), (
            f"FIELD_META[{array_name!r}] is not a list of tuples"
        )
        for i, row in enumerate(rows):
            assert isinstance(row, tuple), (
                f"FIELD_META[{array_name!r}][{i}] is not a tuple: got {type(row)}"
            )
            assert len(row) == 5, (
                f"FIELD_META[{array_name!r}][{i}] has {len(row)} entries; "
                f"expected 5 (field, type, required, desc, example)"
            )
            field, json_type, required, desc, _example = row
            assert isinstance(field, str) and field, (
                f"FIELD_META[{array_name!r}][{i}].field must be a non-empty str"
            )
            assert isinstance(json_type, str), (
                f"FIELD_META[{array_name!r}][{i}].type must be a str"
            )
            assert isinstance(required, bool), (
                f"FIELD_META[{array_name!r}][{i}].required must be a bool"
            )
            assert isinstance(desc, str), (
                f"FIELD_META[{array_name!r}][{i}].description must be a str"
            )


def test_field_meta_covers_the_canonical_entity_arrays() -> None:
    """Every gtopt entity array that exists in C++ has a FIELD_META entry.

    This is a coverage smoke test, not a structural pin — it asserts
    that the most commonly emitted arrays appear in the schema.  Lift
    failures here usually mean the dict copy was incomplete.
    """
    expected_subset = {
        "bus_array",
        "generator_array",
        "demand_array",
        "line_array",
        "battery_array",
    }
    actual = set(field_meta.FIELD_META.keys())
    missing = expected_subset - actual
    assert not missing, (
        f"FIELD_META is missing canonical entity arrays: {sorted(missing)}"
    )


def test_igtopt_shim_re_exports_match() -> None:
    """The legacy ``igtopt._field_meta.FIELD_META`` shim still works."""
    # pylint: disable=import-outside-toplevel
    from igtopt import _field_meta as legacy

    assert legacy.FIELD_META is field_meta.FIELD_META, (
        "shim re-export FIELD_META is a different object than the shared one"
    )
