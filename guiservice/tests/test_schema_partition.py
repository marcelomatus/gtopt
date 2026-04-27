# SPDX-License-Identifier: BSD-3-Clause
"""Schema-partition invariants for ``guiservice.app``.

Refactor safety net for the upcoming move of the three schema dicts
(``OPTIONS_SCHEMA``, ``ELEMENT_SCHEMAS``, ``ELEMENT_TO_ARRAY_KEY``) into
a sibling ``_schemas.py`` module.  The dicts are independently
maintained today; without these tests it is easy to add an element to
``ELEMENT_SCHEMAS`` and forget the entry in ``ELEMENT_TO_ARRAY_KEY``,
causing a silent 404 on the GUI when the element is later edited.

After the move, these tests survive unchanged — they just import from
``guiservice._schemas`` via the back-compat re-export in
``guiservice.app``.
"""

from __future__ import annotations

import pytest

from guiservice.app import (
    ELEMENT_SCHEMAS,
    ELEMENT_TO_ARRAY_KEY,
    OPTIONS_SCHEMA,
)


# ---------------------------------------------------------------------------
# Shape invariants — every entry has the fields the GUI consumes.
# ---------------------------------------------------------------------------


def test_options_schema_every_entry_has_type() -> None:
    """``OPTIONS_SCHEMA`` rows must carry at least a ``type`` field.

    The /api/options_schema endpoint returns the dict verbatim; a
    missing ``type`` makes the GUI's option editor fall back to a raw
    text input, which is hard to debug from the user side.
    """
    for key, spec in OPTIONS_SCHEMA.items():
        assert isinstance(spec, dict), f"{key!r} schema is not a dict"
        assert "type" in spec, f"{key!r} schema is missing 'type'"


def test_element_schemas_every_entry_has_fields() -> None:
    """Every ``ELEMENT_SCHEMAS`` value must declare a ``fields`` list."""
    for elem, spec in ELEMENT_SCHEMAS.items():
        assert isinstance(spec, dict), f"{elem!r} entry is not a dict"
        assert "label" in spec, f"{elem!r} schema is missing 'label'"
        assert "fields" in spec, f"{elem!r} schema is missing 'fields'"
        assert isinstance(spec["fields"], list), f"{elem!r} fields is not a list"


# Element schemas keyed by ``name`` only (no ``uid``) — these are the
# documented exceptions to the uid+name foundational-fields contract.
# The C++ JSON parser uses the name as the primary key for these.
NAME_ONLY_ELEMENTS: frozenset[str] = frozenset({"user_param"})


@pytest.mark.parametrize("elem", sorted(ELEMENT_SCHEMAS.keys()))
def test_element_schemas_uid_and_name_are_required(elem: str) -> None:
    """Every element schema must mark ``name`` as required and, for
    uid-keyed elements, must also mark ``uid`` as required.

    The C++ JSON parser rejects elements without these fields; the GUI
    schema must reflect the same contract so the user sees the
    requirement up front.  Catches a future schema entry that forgets
    one of the foundational fields.
    """
    spec = ELEMENT_SCHEMAS[elem]
    field_index = {f["name"]: f for f in spec["fields"]}
    assert "name" in field_index, f"{elem!r} missing 'name' field"
    if elem in NAME_ONLY_ELEMENTS:
        return
    assert "uid" in field_index, f"{elem!r} missing 'uid' field"
    # uid is required for everything outside NAME_ONLY_ELEMENTS.
    assert field_index["uid"].get("required") is True, f"{elem!r} 'uid' must be required"


# ---------------------------------------------------------------------------
# Cross-dict partitioning — every element schema has a JSON array mapping.
# ---------------------------------------------------------------------------


def test_every_element_schema_has_array_key_mapping() -> None:
    """Every key in ``ELEMENT_SCHEMAS`` must appear in ``ELEMENT_TO_ARRAY_KEY``.

    Without this, the GUI's "edit array" dispatch silently 404s when
    a user opens the element panel for a newly added element type.
    """
    missing = sorted(set(ELEMENT_SCHEMAS.keys()) - set(ELEMENT_TO_ARRAY_KEY.keys()))
    assert not missing, (
        f"ELEMENT_SCHEMAS has elements without ELEMENT_TO_ARRAY_KEY mapping: {missing}"
    )


def test_array_key_mappings_follow_canonical_suffix() -> None:
    """Each ``ELEMENT_TO_ARRAY_KEY`` value must end in ``_array``.

    Mirrors the C++ JSON contract (``json_array_null<"<element>_array"…>``)
    in :file:`include/gtopt/json/json_system.hpp`.  Catches a typo'd
    entry that would otherwise produce a 200 OK with the wrong array.
    """
    for elem, array_key in ELEMENT_TO_ARRAY_KEY.items():
        assert isinstance(array_key, str), f"{elem!r} array key is not a str"
        assert array_key.endswith("_array"), f"{elem!r} → {array_key!r} does not end in '_array'"


def test_no_element_schema_has_orphan_ref() -> None:
    """Every ``ref`` field in ``ELEMENT_SCHEMAS`` points at another element.

    ``{"name": "bus", "ref": "bus"}`` style annotations tell the GUI to
    render a dropdown of existing elements of that type.  An orphan
    ``ref`` (pointing at a non-existent element type) silently falls
    back to a free-text input.
    """
    valid_refs = set(ELEMENT_SCHEMAS.keys())
    for elem, spec in ELEMENT_SCHEMAS.items():
        for field in spec["fields"]:
            ref = field.get("ref")
            if ref is None:
                continue
            assert ref in valid_refs, f"{elem!r} field {field['name']!r} has unknown ref={ref!r}"
