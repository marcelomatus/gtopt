"""Tests for tools/lint_strong_types.py — Rule 13 (UID arithmetic).

Verifies that the regex-based UID-arithmetic detector fires on the
patterns it is supposed to flag and stays silent on lookalike code
that is actually fine.  Mirrors the recurrence the human caught
during the `bb05693b fix(phase_grid)` review: `value_of(phase_uid)
- 1` slipped past every other lint rule and made it onto master.

Run:  cd tools && python -m pytest test_lint_strong_types.py -q
"""

from __future__ import annotations

import pathlib
import sys
import tempfile

# Import the lint module under test.  The script lives next to this
# test, so we add its directory to sys.path explicitly (avoids the
# `pyproject.toml` rootdir dance — `tools/` has none of its own).
sys.path.insert(0, str(pathlib.Path(__file__).parent))

# pylint: disable=wrong-import-position
import lint_strong_types  # noqa: E402


def _scan_uid_arithmetic(src: str) -> list[lint_strong_types.Hit]:
    """Run `scan_uid_arithmetic` on a synthetic snippet and return hits.

    Wraps the snippet in a `.cpp` file and runs the rule against it.
    Returns the rule's hit list (empty when clean).
    """
    rule = lint_strong_types.Rule(
        name="r13",
        description="",
        fix="",
    )
    with tempfile.NamedTemporaryFile(suffix=".cpp", mode="w", delete=False) as f:
        f.write(src)
        tmp = pathlib.Path(f.name)
    try:
        lint_strong_types.scan_uid_arithmetic(rule, [tmp])
    finally:
        tmp.unlink()
    return rule.hits


# ─── Should fire ─────────────────────────────────────────────────────────────


def test_value_of_uid_minus_literal_fires():
    """The exact pre-bb05693b pattern: `value_of(phase_uid) - 1`."""
    hits = _scan_uid_arithmetic("auto x = value_of(phase_uid) - 1;\n")
    assert len(hits) == 1


def test_value_of_uid_plus_literal_fires():
    """`value_of(uid) + N` is the same category error."""
    hits = _scan_uid_arithmetic("auto x = value_of(scene_uid) + 2;\n")
    assert len(hits) == 1


def test_value_of_pascal_case_uid_fires():
    """PascalCase identifier ending in 'Uid' must also be caught."""
    hits = _scan_uid_arithmetic("auto x = value_of(phaseUid) - 1;\n")
    assert len(hits) == 1


def test_static_cast_int_uid_fires():
    """`static_cast<int>(*_uid) ± N` — the cast variant."""
    hits = _scan_uid_arithmetic("int x = static_cast<int>(scene_uid) - 1;\n")
    assert len(hits) == 1


def test_static_cast_size_t_uid_fires():
    """`static_cast<size_t>(*_uid) ± N` is also flagged."""
    hits = _scan_uid_arithmetic("auto x = static_cast<size_t>(iter_uid) + 5;\n")
    assert len(hits) == 1


def test_static_cast_ptrdiff_uid_fires():
    """`static_cast<std::ptrdiff_t>(*_uid) - N` is flagged too."""
    hits = _scan_uid_arithmetic(
        "auto x = static_cast<std::ptrdiff_t>(phase_uid) - 1;\n"
    )
    assert len(hits) == 1


# ─── Must NOT fire ───────────────────────────────────────────────────────────


def test_value_of_uid_no_arithmetic_clean():
    """Bare `value_of(*_uid)` (no `±N`) must not fire."""
    hits = _scan_uid_arithmetic(
        "auto x = value_of(phase_uid);  // OK — no arithmetic\n"
    )
    assert hits == []


def test_value_of_uid_in_equality_clean():
    """`if (value_of(uid) == 1)` is comparison, not arithmetic."""
    hits = _scan_uid_arithmetic("if (value_of(phase_uid) == 1) {}\n")
    assert hits == []


def test_nolint_suppression_clean():
    """`// NOLINT` opts a line out of the rule."""
    hits = _scan_uid_arithmetic("auto x = value_of(phase_uid) + 1;  // NOLINT\n")
    assert hits == []


def test_pure_comment_clean():
    """Pattern only inside a comment must not fire (comment stripping)."""
    hits = _scan_uid_arithmetic("// value_of(phase_uid) - 1 is a category error.\n")
    assert hits == []


def test_block_comment_clean():
    """Block comment with the bad pattern is still ignored."""
    hits = _scan_uid_arithmetic(
        "/* value_of(phase_uid) - 1 is wrong, do not ship. */\n"
    )
    assert hits == []


def test_value_of_non_uid_identifier_clean():
    """`value_of(other_var) - 1` where the var doesn't end in 'uid'/'Uid' is fine."""
    hits = _scan_uid_arithmetic("auto x = value_of(some_var) - 1;\n")
    assert hits == []


def test_static_cast_index_no_arith_clean():
    """`static_cast<int>(some_uid)` without `± N` is not arithmetic."""
    hits = _scan_uid_arithmetic("int x = static_cast<int>(scene_uid);\n")
    assert hits == []


# ─── Sanity — multiple hits and mixed lines ──────────────────────────────────


def test_two_offending_lines_in_one_file():
    """Two distinct violations land as two hits."""
    src = (
        "auto a = value_of(phase_uid) - 1;\n"
        "auto b = static_cast<size_t>(scene_uid) + 2;\n"
    )
    hits = _scan_uid_arithmetic(src)
    assert len(hits) == 2


def test_mixed_offending_and_clean_lines():
    """Mixed snippet — only the bad lines fire."""
    src = (
        "auto a = value_of(phase_uid);  // clean\n"
        "auto b = value_of(phase_uid) - 1;  // bad\n"
        "if (value_of(phase_uid) == 1) {}  // clean\n"
        "auto c = static_cast<int>(iter_uid) + 3;  // bad\n"
    )
    hits = _scan_uid_arithmetic(src)
    assert len(hits) == 2
