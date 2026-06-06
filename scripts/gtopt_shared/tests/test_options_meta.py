# SPDX-License-Identifier: BSD-3-Clause
"""Structural + provenance tests for :mod:`gtopt_shared.options_meta`.

These tests pin the public surface of the lifted ``options_meta``
module (frozensets + ``_OPTIONS_FIELDS``) so a future refactor that
renames or drops a key fails immediately, and the legacy
``igtopt._options_meta`` shim continues to re-export the same data.
"""

from __future__ import annotations

from gtopt_shared import options_meta


# Keys that legitimately appear in MORE THAN ONE partition.  PLEXOS
# / SDDP / monolithic all expose the same boundary-cuts knobs under
# the same JSON name; the igtopt Excel sheet uses prefixed names
# (``monolithic_boundary_cuts_file``) to disambiguate on input but
# the JSON keys themselves are shared by design.
_INTENTIONAL_OVERLAPS: frozenset[str] = frozenset(
    {
        "boundary_cuts_file",
        "boundary_cuts_mode",
        "boundary_max_iterations",
    }
)


def test_frozensets_are_mostly_disjoint() -> None:
    """Each option key belongs to exactly one sub-object partition,
    except for the documented intentional overlaps."""
    partitions = {
        "sddp": options_meta.SDDP_OPTION_KEYS,
        "model": options_meta.MODEL_OPTION_KEYS,
        "monolithic": options_meta.MONOLITHIC_OPTION_KEYS,
        "solver": options_meta.SOLVER_OPTION_KEYS,
        "simulation": options_meta.SIMULATION_OPTION_KEYS,
    }
    names = list(partitions.keys())
    for i, a in enumerate(names):
        for b in names[i + 1 :]:
            overlap = partitions[a] & partitions[b]
            unexpected = overlap - _INTENTIONAL_OVERLAPS
            assert not unexpected, (
                f"option keys appear in both {a} and {b} partitions "
                f"without being documented as intentional overlaps: "
                f"{unexpected}"
            )


def test_frozensets_are_nonempty_where_expected() -> None:
    """The partitions with non-cascade content carry at least one key.

    ``CASCADE_OPTION_KEYS`` is intentionally empty (cascade params live
    inside per-level ``model_options``/``sddp_options`` overlays); every
    other partition should have at least one entry to avoid silent
    accidental wipes.
    """
    for name in (
        "SDDP_OPTION_KEYS",
        "MODEL_OPTION_KEYS",
        "MONOLITHIC_OPTION_KEYS",
        "SOLVER_OPTION_KEYS",
        "SIMULATION_OPTION_KEYS",
    ):
        partition = getattr(options_meta, name)
        assert partition, f"{name} is empty — at least one option key expected"


def test_options_fields_shape() -> None:
    """Each ``_OPTIONS_FIELDS`` entry is a ``(key, description, default)``.

    The igtopt Excel template surfaces a subset of the canonical
    options as a documented ``options`` worksheet.  Not every C++
    option must be in the Excel sheet (many advanced SDDP / solver
    knobs are JSON-only).  We only pin the row SHAPE here so future
    additions can't accidentally break the Excel generator.
    """
    # pylint: disable=protected-access
    assert options_meta._OPTIONS_FIELDS, "_OPTIONS_FIELDS is empty"
    seen_keys: set[str] = set()
    for i, row in enumerate(options_meta._OPTIONS_FIELDS):
        assert isinstance(row, tuple), (
            f"_OPTIONS_FIELDS[{i}] is not a tuple: got {type(row)}"
        )
        assert len(row) == 3, (
            f"_OPTIONS_FIELDS[{i}] has {len(row)} entries; expected 3 "
            "(key, description, default)"
        )
        key, desc, _default = row
        assert isinstance(key, str) and key, (
            f"_OPTIONS_FIELDS[{i}].key must be a non-empty str"
        )
        assert isinstance(desc, str), f"_OPTIONS_FIELDS[{i}].description must be a str"
        assert key not in seen_keys, f"duplicate _OPTIONS_FIELDS key: {key}"
        seen_keys.add(key)


def test_igtopt_shim_re_exports_match() -> None:
    """The legacy ``igtopt._options_meta`` re-export shim still works.

    Pinning this protects backward compatibility for any external code
    that imports the original private path; the shim must surface the
    exact same Python objects (identity check via ``is``).
    """
    # pylint: disable=import-outside-toplevel
    from igtopt import _options_meta as legacy

    for name in (
        "SDDP_OPTION_KEYS",
        "MODEL_OPTION_KEYS",
        "MONOLITHIC_OPTION_KEYS",
        "SOLVER_OPTION_KEYS",
        "SIMULATION_OPTION_KEYS",
        "CASCADE_OPTION_KEYS",
    ):
        assert getattr(legacy, name) is getattr(options_meta, name), (
            f"shim re-export {name} is a different object than the shared one"
        )
