# SPDX-License-Identifier: BSD-3-Clause
"""Cross-language sync gate for :mod:`gtopt_shared.options_meta`.

The frozensets in ``options_meta.py`` mirror the C++ ``json_data_contract<...>``
field lists in ``include/gtopt/json/*.hpp``.  When the C++ side adds or
removes a field, the Python side must follow — otherwise igtopt's
Excel template + JSON serialisation will silently drop or fail on it.

This module reads each relevant C++ header, extracts the canonical
field-name list, and asserts every Python frozenset is **a subset of**
its C++ source (drops are loud; additions are advisory because the
C++ contract may legitimately surface more fields than the Excel
template needs).

The gate is intentionally one-way (Python ⊆ C++) so the test cost is
low and the failure mode (Python has a field the C++ removed) is the
truly dangerous one.  Mirrors the ``test_naming_dialects_consistency``
and ``test_ampl_dispatch_consistency`` precedents in this folder.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from gtopt_shared import options_meta


REPO_ROOT = Path(__file__).resolve().parents[3]
JSON_DIR = REPO_ROOT / "include" / "gtopt" / "json"


# Pattern matching daw::json contract field names:
#   json_<kind>_null<"field_name", ...>
#   json_<kind><"field_name", ...>
_FIELD_NAME_RE = re.compile(r"json_[a-z_]+<\"([a-zA-Z_][a-zA-Z0-9_]*)\"")


def _extract_field_names(header: Path) -> set[str]:
    """Return the set of canonical field names declared in ``header``."""
    text = header.read_text(encoding="utf-8")
    return set(_FIELD_NAME_RE.findall(text))


# Map each Python frozenset to its C++ source header.  Missing headers
# cause the test to skip (with a clear reason) rather than fail —
# someone restructuring the C++ layout shouldn't be surprised by a
# spurious red CI.
_PARTITION_TO_HEADER: tuple[tuple[str, str], ...] = (
    ("SDDP_OPTION_KEYS", "json_sddp_options.hpp"),
    ("MODEL_OPTION_KEYS", "json_model_options.hpp"),
    ("MONOLITHIC_OPTION_KEYS", "json_monolithic_options.hpp"),
    ("SOLVER_OPTION_KEYS", "json_solver_options.hpp"),
)

# DOCUMENTED DRIFT — keys present in the Python frozenset but absent
# from the matching C++ header on master, with a one-line rationale.
# Each entry is a follow-up TODO: either restore the C++ field, drop
# the Python entry, or extend the matcher.  As entries are resolved,
# delete them from this dict.
#
# Issue #507 follow-up: surfaced by the new C++ sync gate landed today.
_KNOWN_DRIFT: dict[str, frozenset[str]] = {
    "SDDP_OPTION_KEYS": frozenset(
        {
            # Inspected json_sddp_options.hpp on master — neither
            # name appears in any json_*<"..."> contract member.
            # Either renamed without Python update or removed.
            "production_factor_update_skip",
            "warm_start",
        }
    ),
    "SOLVER_OPTION_KEYS": frozenset(
        {
            # Not in json_solver_options.hpp on master.
            "reuse_basis",
        }
    ),
}


@pytest.mark.parametrize(
    "partition_name,header_filename",
    _PARTITION_TO_HEADER,
    ids=[name for name, _ in _PARTITION_TO_HEADER],
)
def test_python_partition_is_subset_of_cpp_contract(
    partition_name: str, header_filename: str
) -> None:
    """Every key in the Python frozenset must exist in the C++ contract.

    Catches the silent-drift failure mode where the C++ side renames or
    removes a field but the Python frozenset still references the old
    name — the Excel template would generate a stale row that gets
    silently ignored by the C++ JSON parser.
    """
    header = JSON_DIR / header_filename
    if not header.exists():
        pytest.skip(
            f"C++ header {header_filename} not found at {header}; "
            "build tree may have been relocated"
        )

    cpp_fields = _extract_field_names(header)
    if not cpp_fields:
        pytest.skip(
            f"no daw::json contract fields parsed from {header_filename}; "
            "the matcher may need updating for a new contract macro"
        )

    py_partition: frozenset[str] = getattr(options_meta, partition_name)
    # Subtract the documented exception classes:
    #  * LEGACY_OPTION_KEY_ALIASES — intentional gtopt-legacy dialect aliases
    #    (kept for back-compat; have a canonical C++ counterpart).
    #  * _KNOWN_DRIFT[partition_name] — already-flagged TODO entries with a
    #    rationale recorded above.  Removing entries from _KNOWN_DRIFT
    #    forces the gate to re-validate them.
    waivers = options_meta.LEGACY_OPTION_KEY_ALIASES | _KNOWN_DRIFT.get(
        partition_name, frozenset()
    )
    stale = (py_partition - cpp_fields) - waivers
    assert not stale, (
        f"{partition_name} has keys not present in C++ {header_filename}: "
        f"{sorted(stale)}.  Either the C++ side renamed/removed these "
        "fields (Python must follow), the matcher missed them, or they "
        "need to be added to LEGACY_OPTION_KEY_ALIASES (intentional "
        f"alias) or _KNOWN_DRIFT (with a rationale).  Inspect {header}."
    )


@pytest.mark.parametrize(
    "partition_name,header_filename",
    _PARTITION_TO_HEADER,
    ids=[name for name, _ in _PARTITION_TO_HEADER],
)
def test_known_drift_entries_still_drift(
    partition_name: str, header_filename: str
) -> None:
    """``_KNOWN_DRIFT`` entries that the C++ side restored should be cleaned up.

    When the C++ contract adds back a field that's currently in
    ``_KNOWN_DRIFT`` (or the Python side removes it), this test fails
    and prompts the developer to remove the stale waiver — preventing
    the drift map from going stale and silently waiving real new drift.
    """
    drift = _KNOWN_DRIFT.get(partition_name, frozenset())
    if not drift:
        pytest.skip(f"{partition_name} has no documented drift")

    header = JSON_DIR / header_filename
    if not header.exists():
        pytest.skip(f"C++ header {header_filename} not found")

    cpp_fields = _extract_field_names(header)
    py_partition: frozenset[str] = getattr(options_meta, partition_name)

    resolved = drift & (cpp_fields | (drift - py_partition))
    assert not resolved, (
        f"{partition_name}: _KNOWN_DRIFT entries are no longer drifting "
        f"and should be removed: {sorted(resolved)}.  Edit "
        "_KNOWN_DRIFT in this file to remove them."
    )


# Reverse-direction gate, SDDP partition ONLY.  ``SDDP_OPTION_KEYS``
# is load-bearing for igtopt's flat-key → ``sddp_options`` routing
# (igtopt.py): a C++ contract field missing from the Python set is
# NOT merely "advisory extra surface" — the key set at top level is
# silently mis-placed instead of nested into ``sddp_options``.  That
# exact failure shipped with ``integer_cuts_mode`` (2026-07-08 wave):
# the C++ side added the field, 13 of 14 plumbing sites followed, and
# this set was the one missed.  The other partitions legitimately
# surface more C++ fields than the Excel template needs, so the
# reverse gate stays SDDP-only.
#
# Fields the C++ contract declares that the Python routing set
# deliberately omits.  Empty today; add entries ONLY with a rationale.
_SDDP_CPP_ONLY_WAIVERS: frozenset[str] = frozenset()


def test_sddp_cpp_contract_is_subset_of_python_partition() -> None:
    """Every C++ ``json_data_contract<SddpOptions>`` field must be routed.

    The forward gate above catches Python-side staleness; this reverse
    gate catches the C++-side *addition* that forgets the Python set —
    the failure mode that silently drops a user's flat option key.
    """
    header = JSON_DIR / "json_sddp_options.hpp"
    if not header.exists():
        pytest.skip("json_sddp_options.hpp not found; build tree relocated")

    cpp_fields = _extract_field_names(header)
    if not cpp_fields:
        pytest.skip("no daw::json contract fields parsed; matcher stale")

    missing = (cpp_fields - options_meta.SDDP_OPTION_KEYS) - _SDDP_CPP_ONLY_WAIVERS
    assert not missing, (
        f"C++ SddpOptions contract fields missing from SDDP_OPTION_KEYS: "
        f"{sorted(missing)}.  igtopt will NOT nest these keys into "
        "sddp_options (silent drop).  Add them to SDDP_OPTION_KEYS in "
        "gtopt_shared/options_meta.py, or waive with a rationale in "
        "_SDDP_CPP_ONLY_WAIVERS."
    )


def test_intentional_overlaps_are_actually_present_in_both_partitions() -> None:
    """Each ``INTENTIONAL_OPTION_KEY_OVERLAPS`` key must be in BOTH SDDP and monolithic.

    The constant documents the SDDP/monolithic boundary-cuts overlap —
    if a key drops out of one side, the constant should be updated to
    reflect the new partition shape, not silently kept as a stale
    waiver.
    """
    overlaps = options_meta.INTENTIONAL_OPTION_KEY_OVERLAPS
    missing_in_sddp = overlaps - options_meta.SDDP_OPTION_KEYS
    missing_in_mono = overlaps - options_meta.MONOLITHIC_OPTION_KEYS
    assert not missing_in_sddp, (
        f"overlaps declared but missing from SDDP_OPTION_KEYS: "
        f"{sorted(missing_in_sddp)}"
    )
    assert not missing_in_mono, (
        f"overlaps declared but missing from MONOLITHIC_OPTION_KEYS: "
        f"{sorted(missing_in_mono)}"
    )
