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


# ─── Rule 14 — redundant [static_cast<size_t>(strong_index)] ─────────────────


def _scan_redundant_size_t_subscript(
    src: str,
) -> list[lint_strong_types.Hit]:
    """Run `scan_redundant_size_t_subscript` on a synthetic snippet."""
    rule = lint_strong_types.Rule(
        name="r14",
        description="",
        fix="",
    )
    with tempfile.NamedTemporaryFile(suffix=".cpp", mode="w", delete=False) as f:
        f.write(src)
        tmp = pathlib.Path(f.name)
    try:
        lint_strong_types.scan_redundant_size_t_subscript(rule, [tmp])
    finally:
        tmp.unlink()
    return rule.hits


def test_subscript_size_t_cast_on_index_fires():
    """`vec[static_cast<size_t>(index)]` — the canonical form."""
    hits = _scan_redundant_size_t_subscript(
        "auto x = vec[static_cast<size_t>(index)];\n"
    )
    assert len(hits) == 1


def test_subscript_size_t_cast_on_row_idx_fires():
    """`rin[static_cast<size_t>(row_idx)]` — common LP-assembly shape."""
    hits = _scan_redundant_size_t_subscript(
        "auto x = rin[static_cast<size_t>(row_idx)];\n"
    )
    assert len(hits) == 1


def test_subscript_size_t_cast_on_col_idx_fires():
    """`cin[static_cast<size_t>(col_idx)]` — column counterpart."""
    hits = _scan_redundant_size_t_subscript(
        "auto x = cin[static_cast<size_t>(col_idx)];\n"
    )
    assert len(hits) == 1


def test_subscript_std_size_t_cast_fires():
    """`vec[static_cast<std::size_t>(index)]` — qualified spelling."""
    hits = _scan_redundant_size_t_subscript(
        "auto x = vec[static_cast<std::size_t>(index)];\n"
    )
    assert len(hits) == 1


def test_subscript_size_t_cast_on_row_fires():
    """The exact form found in linear_interface.cpp:2857–2858."""
    hits = _scan_redundant_size_t_subscript(
        "diag.rhs_lb = row_lb[static_cast<size_t>(row)];\n"
    )
    assert len(hits) == 1


# ─── Rule 14 — must NOT fire ─────────────────────────────────────────────────


def test_size_t_cast_outside_subscript_clean():
    """`static_cast<size_t>(idx) + 1` for resize-arith — keep the cast."""
    hits = _scan_redundant_size_t_subscript(
        "vec.resize(static_cast<size_t>(idx) + 1);\n"
    )
    assert hits == []


def test_size_t_cast_on_loop_counter_clean():
    """`vec[static_cast<size_t>(i)]` — `i` is a generic int counter."""
    hits = _scan_redundant_size_t_subscript("auto x = vec[static_cast<size_t>(i)];\n")
    assert hits == []


def test_size_t_cast_on_size_call_clean():
    """`static_cast<size_t>(count)` — `count` is in the non-strong list."""
    hits = _scan_redundant_size_t_subscript(
        "auto x = vec[static_cast<size_t>(count)];\n"
    )
    assert hits == []


def test_subscript_no_cast_clean():
    """`rin[row_idx]` — already idiomatic, must stay clean."""
    hits = _scan_redundant_size_t_subscript("auto x = rin[row_idx];\n")
    assert hits == []


def test_subscript_int_cast_clean_for_rule_14():
    """`vec[static_cast<int>(idx)]` is Rule 8's domain, not Rule 14."""
    hits = _scan_redundant_size_t_subscript("auto x = vec[static_cast<int>(idx)];\n")
    assert hits == []


def test_size_t_cast_subscript_nolint_clean():
    """`// NOLINT` opts the line out."""
    hits = _scan_redundant_size_t_subscript(
        "auto x = vec[static_cast<size_t>(index)];  // NOLINT\n"
    )
    assert hits == []


def test_size_t_cast_subscript_in_comment_clean():
    """Pattern only inside a comment is ignored."""
    hits = _scan_redundant_size_t_subscript(
        "// vec[static_cast<size_t>(index)] is the bad pattern\n"
    )
    assert hits == []
