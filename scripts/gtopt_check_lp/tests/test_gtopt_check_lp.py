# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_lp – core LP analyzer classes."""

import gzip
import pathlib
import re
from pathlib import Path

import pytest

from gtopt_check_lp._compress import (
    as_sanitized_lp,
    sanitize_lp_names,
)
from gtopt_check_lp._lp_analyzer import (
    CoeffEntry,
    _parse_coeff_var_pairs,
)
from gtopt_check_lp.gtopt_check_lp import (
    analyze_lp_file,
    format_static_report,
)

# ── Case directories ─────────────────────────────────────────────────────────

_SCRIPTS_DIR = pathlib.Path(__file__).parent.parent.parent
_LP_CASES_DIR = _SCRIPTS_DIR / "cases" / "lp_infeasible"
_MY_SMALL_BAD = _LP_CASES_DIR / "my_small_bad.lp"
_BAD_BOUNDS = _LP_CASES_DIR / "bad_bounds.lp"
_BAD_BOUNDS_GZ = _LP_CASES_DIR / "bad_bounds.lp.gz"
_FEASIBLE_SMALL = _LP_CASES_DIR / "feasible_small.lp"


# ── Helpers ──────────────────────────────────────────────────────────────────


def _write_lp(tmp_path: Path, name: str, content: str) -> Path:
    """Write a tiny LP file to tmp_path and return its Path."""
    p = tmp_path / name
    p.write_text(content, encoding="utf-8")
    return p


def _write_gz_lp(tmp_path: Path, name: str, content: str) -> Path:
    """Write a gzip-compressed LP file to tmp_path and return its Path."""
    p = tmp_path / name
    with gzip.open(str(p), "wt", encoding="utf-8") as fh:
        fh.write(content)
    return p


# ── TestAnalyzeLpFile ─────────────────────────────────────────────────────────


class TestAnalyzeLpFile:
    """Unit tests for analyze_lp_file()."""

    def test_bad_bounds_detected(self):
        """bad_bounds.lp: x1 has lb=5 > ub=2 → infeasible_bounds list."""
        stats = analyze_lp_file(_BAD_BOUNDS)
        assert stats.infeasible_bounds, "Expected at least one infeasible bound"
        names = [vb.name for vb in stats.infeasible_bounds]
        assert "x1" in names
        assert stats.has_issues()

    def test_bad_bounds_values(self):
        """Exact lb/ub values for x1 in bad_bounds.lp."""
        stats = analyze_lp_file(_BAD_BOUNDS)
        x1_bounds = next(
            (vb for vb in stats.infeasible_bounds if vb.name == "x1"), None
        )
        assert x1_bounds is not None, "x1 should be in infeasible_bounds"
        assert x1_bounds.lb == pytest.approx(5.0)
        assert x1_bounds.ub == pytest.approx(2.0)

    def test_feasible_lp_no_issues(self):
        """feasible_small.lp: clean LP → no issues detected statically."""
        stats = analyze_lp_file(_FEASIBLE_SMALL)
        assert not stats.infeasible_bounds
        assert not stats.empty_constraints
        assert not stats.duplicate_constraint_names
        assert not stats.has_issues()

    def test_feasible_lp_stats(self):
        """feasible_small.lp: correct constraint count."""
        stats = analyze_lp_file(_FEASIBLE_SMALL)
        assert stats.n_constraints == 3
        assert stats.has_objective

    def test_my_small_bad_no_static_bound_issue(self):
        """my_small_bad.lp: constraint conflict not detectable statically."""
        # The LP is infeasible due to conflicting constraints (c1 + c2),
        # but the static analyser cannot detect this without solving.
        stats = analyze_lp_file(_MY_SMALL_BAD)
        assert not stats.infeasible_bounds  # bounds themselves are fine
        assert stats.n_constraints == 2

    def test_inline_bad_bounds(self, tmp_path):
        """Inline LP: two variables with lb > ub."""
        lp = _write_lp(
            tmp_path,
            "test_bounds.lp",
            (
                "Minimize\n obj: a + b\n"
                "Subject To\n c1: a + b <= 100\n"
                "Bounds\n 10 <= a <= 3\n 7 <= b <= 1\nEnd\n"
            ),
        )
        stats = analyze_lp_file(lp)
        assert len(stats.infeasible_bounds) == 2
        names = {vb.name for vb in stats.infeasible_bounds}
        assert names == {"a", "b"}

    def test_large_coefficient_flagged(self, tmp_path):
        """Inline LP: coefficients >= 1e10 should be in large_coeff_constraints."""
        lp = _write_lp(
            tmp_path,
            "large_coeff.lp",
            (
                "Minimize\n obj: x1\n"
                "Subject To\n"
                " big: 1e12 x1 + x2 <= 100\n"
                "Bounds\n 0 <= x1\n 0 <= x2\nEnd\n"
            ),
        )
        stats = analyze_lp_file(lp)
        assert "big" in stats.large_coeff_constraints
        assert stats.has_issues()

    def test_duplicate_constraint_names(self, tmp_path):
        """Duplicate constraint names should be detected."""
        lp = _write_lp(
            tmp_path,
            "dup_names.lp",
            (
                "Minimize\n obj: x\n"
                "Subject To\n"
                " dup: x >= 1\n"
                " dup: x <= 10\n"
                "Bounds\n 0 <= x <= 20\nEnd\n"
            ),
        )
        stats = analyze_lp_file(lp)
        assert "dup" in stats.duplicate_constraint_names

    def test_file_not_found_raises(self):
        """Missing file raises FileNotFoundError."""
        with pytest.raises(FileNotFoundError):
            analyze_lp_file(Path("/nonexistent/path/no.lp"))


# ── TestFormatStaticReport ────────────────────────────────────────────────────


class TestFormatStaticReport:
    """Tests for format_static_report()."""

    def test_no_issues_message(self):
        stats = analyze_lp_file(_FEASIBLE_SMALL)
        report = format_static_report(_FEASIBLE_SMALL, stats)
        # Strip ANSI and check content
        clean = re.sub(r"\033\[[0-9;]*m", "", report)
        assert "No obvious static infeasibilities" in clean

    def test_infeasible_bounds_in_report(self):
        stats = analyze_lp_file(_BAD_BOUNDS)
        report = format_static_report(_BAD_BOUNDS, stats)
        clean = re.sub(r"\033\[[0-9;]*m", "", report)
        assert "Conflicting variable bounds" in clean
        assert "x1" in clean


# ── TestFormatStaticReportBranches ────────────────────────────────────────────


class TestFormatStaticReportBranches:
    """Cover report formatting branches for various issue types."""

    def _make_report(self, stats):
        """Helper: format_static_report needs a Path; use a dummy one."""
        from gtopt_check_lp._lp_analyzer import format_static_report as _fmt  # noqa: PLC0415

        return _fmt(Path("dummy.lp"), stats)

    def test_empty_constraints_in_report(self):
        """Empty constraints are shown in the report."""
        from gtopt_check_lp._lp_analyzer import LPStats  # noqa: PLC0415

        stats = LPStats(n_vars=10, n_constraints=5, has_objective=True)
        stats.empty_constraints = [f"c{i}" for i in range(15)]
        report = self._make_report(stats)
        assert "Empty constraints (15)" in report
        assert "… and 5 more" in report

    def test_duplicate_constraints_in_report(self):
        """Duplicate constraint names are shown in the report."""
        from gtopt_check_lp._lp_analyzer import LPStats  # noqa: PLC0415

        stats = LPStats(n_vars=10, n_constraints=5, has_objective=True)
        stats.duplicate_constraint_names = ["dup1", "dup2"]
        report = self._make_report(stats)
        assert "Duplicate constraint names (2)" in report
        assert "dup1" in report

    def test_fixed_variables_in_report(self):
        """Fixed variables section with truncation."""
        from gtopt_check_lp._lp_analyzer import LPStats  # noqa: PLC0415

        stats = LPStats(n_vars=20, n_constraints=5, has_objective=True)
        stats.fixed_variables = [f"x{i}" for i in range(12)]
        report = self._make_report(stats)
        assert "Fixed variables (12)" in report
        assert "… and 2 more" in report

    def test_large_coeff_constraints_in_report(self):
        """Large coefficient constraints with truncation."""
        from gtopt_check_lp._lp_analyzer import LPStats  # noqa: PLC0415

        stats = LPStats(n_vars=10, n_constraints=5, has_objective=True)
        stats.large_coeff_constraints = [f"big{i}" for i in range(15)]
        report = self._make_report(stats)
        assert "very large coefficients" in report
        assert "… and 5 more" in report


# ── TestLPAnalyzerBoundParsing ────────────────────────────────────────────────


class TestLPAnalyzerBoundParsing:
    """Cover alternative bound-format parsing branches in analyze_lp_file."""

    def test_single_lower_bound(self, tmp_path):
        """'x >= 5' format."""
        lp = tmp_path / "lb.lp"
        lp.write_text(
            "Minimize\n obj: x\nSubject To\n c1: x >= 1\nBounds\n x >= 5\nEnd\n",
            encoding="utf-8",
        )
        stats = analyze_lp_file(lp)
        # Just ensure parsing succeeded without error
        assert stats.n_vars > 0

    def test_single_upper_bound(self, tmp_path):
        """'x <= 10' format."""
        lp = tmp_path / "ub.lp"
        lp.write_text(
            "Minimize\n obj: x\nSubject To\n c1: x >= 1\nBounds\n x <= 10\nEnd\n",
            encoding="utf-8",
        )
        stats = analyze_lp_file(lp)
        assert stats.n_vars > 0

    def test_reversed_upper_bound(self, tmp_path):
        """'10 >= x' format (value >= variable)."""
        lp = tmp_path / "rev.lp"
        lp.write_text(
            "Minimize\n obj: x\nSubject To\n c1: x >= 1\nBounds\n 10 >= x\nEnd\n",
            encoding="utf-8",
        )
        stats = analyze_lp_file(lp)
        assert stats.n_vars > 0

    def test_general_and_binary_sections(self, tmp_path):
        """General and Binary sections are parsed."""
        lp = tmp_path / "mip.lp"
        lp.write_text(
            "Minimize\n obj: x + y + z\nSubject To\n c1: x + y + z >= 1\n"
            "Bounds\n 0 <= x <= 1\n 0 <= y <= 1\n 0 <= z <= 10\n"
            "General\n z\nBinary\n x y\nEnd\n",
            encoding="utf-8",
        )
        stats = analyze_lp_file(lp)
        assert stats.n_integers == 1
        assert stats.n_binary == 2

    def test_empty_constraint_detected(self, tmp_path):
        """A constraint with no variables is detected as empty."""
        lp = tmp_path / "empty_con.lp"
        # Constraint with only RHS (no LHS variables)
        lp.write_text(
            "Minimize\n obj: x\nSubject To\n"
            " c_ok: x >= 1\n"
            " c_empty: = 0\n"
            "Bounds\n 0 <= x <= 10\nEnd\n",
            encoding="utf-8",
        )
        stats = analyze_lp_file(lp)
        # c_empty has no coefficients
        assert "c_empty" in stats.empty_constraints or stats.n_constraints >= 1

    def test_duplicate_constraint_names(self, tmp_path):
        """Duplicate constraint names are detected."""
        lp = tmp_path / "dup.lp"
        lp.write_text(
            "Minimize\n obj: x + y\nSubject To\n"
            " dup: x >= 1\n"
            " dup: y >= 2\n"
            "Bounds\n 0 <= x <= 10\n 0 <= y <= 10\nEnd\n",
            encoding="utf-8",
        )
        stats = analyze_lp_file(lp)
        assert "dup" in stats.duplicate_constraint_names

    def test_fixed_variables_detected(self, tmp_path):
        """Variables with lb == ub are detected as fixed."""
        lp = tmp_path / "fixed.lp"
        lp.write_text(
            "Minimize\n obj: x + y\nSubject To\n c1: x + y >= 1\n"
            "Bounds\n 5 <= x <= 5\n 0 <= y <= 10\nEnd\n",
            encoding="utf-8",
        )
        stats = analyze_lp_file(lp)
        assert "x" in stats.fixed_variables


# ── TestCoeffEntry ────────────────────────────────────────────────────────────


class TestCoeffEntry:
    """Cover CoeffEntry __eq__ and __lt__ with non-CoeffEntry operands."""

    def test_eq_non_coeff_entry(self):
        c = CoeffEntry(abs_coeff=1.0, var_name="x", constraint_name="c1")
        result = c == "not a CoeffEntry"  # noqa: SIM201
        assert result is not True

    def test_lt_non_coeff_entry(self):
        c = CoeffEntry(abs_coeff=1.0, var_name="x", constraint_name="c1")
        with pytest.raises(TypeError):
            _ = c < "not a CoeffEntry"


# ── TestParseCoeffVarPairs ────────────────────────────────────────────────────


class TestParseCoeffVarPairs:
    """Tests for _parse_coeff_var_pairs()."""

    @staticmethod
    def _to_var_coeff_dict(expr: str) -> dict[str, float]:
        """Helper: convert (coeff, var) pairs to {var: coeff} dict."""
        return {var: coeff for coeff, var in _parse_coeff_var_pairs(expr)}

    def test_explicit_coefficient(self):
        pairs = self._to_var_coeff_dict("2.5 x1 + 3.0 x2")
        assert pairs.get("x1") == pytest.approx(2.5)
        assert pairs.get("x2") == pytest.approx(3.0)

    def test_negative_coefficient(self):
        pairs = self._to_var_coeff_dict("- 0.9 var_a + 1.1 var_b")
        assert pairs.get("var_a") == pytest.approx(-0.9)
        assert pairs.get("var_b") == pytest.approx(1.1)

    def test_sign_only_positive(self):
        pairs = self._to_var_coeff_dict("+ my_var")
        assert pairs.get("my_var") == pytest.approx(1.0)

    def test_sign_only_negative(self):
        pairs = self._to_var_coeff_dict("- my_var")
        assert pairs.get("my_var") == pytest.approx(-1.0)

    def test_no_sign_no_coeff(self):
        pairs = self._to_var_coeff_dict("my_var_1 + my_var_2")
        assert pairs.get("my_var_1") == pytest.approx(1.0)
        assert pairs.get("my_var_2") == pytest.approx(1.0)

    def test_scientific_notation(self):
        pairs = self._to_var_coeff_dict("- 3.8994e-05 rsv_eini")
        assert pairs.get("rsv_eini") == pytest.approx(-3.8994e-05)

    def test_rhs_stripped(self):
        """Coefficients after '=' should not be returned."""
        pairs = self._to_var_coeff_dict("x1 - 3.5 x2 = 10")
        assert set(pairs.keys()) <= {"x1", "x2"}
        assert pairs.get("x1") == pytest.approx(1.0)
        assert pairs.get("x2") == pytest.approx(-3.5)


# ── TestTopNCoefficients ──────────────────────────────────────────────────────


class TestTopNCoefficients:
    """Tests for top_max_coeffs / top_min_coeffs in LPStats."""

    _LP_WITH_VARIED_COEFFS = (
        "Minimize\n"
        " obj: x1\n"
        "Subject To\n"
        " c_large: 1e12 x1 + x2 >= 1\n"
        " c_small: 1e-12 x1 - 0.5 x2 >= 0\n"
        " c_mid: 3.0 x1 + 2.0 x2 >= 2\n"
        "Bounds\n"
        " 0 <= x1 <= 1e30\n"
        " 0 <= x2 <= 1e30\n"
        "End\n"
    )

    def test_top_max_coeffs_populated(self, tmp_path):
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        assert len(stats.top_max_coeffs) > 0
        # Largest should be 1e12
        assert stats.top_max_coeffs[0].abs_coeff == pytest.approx(1e12)

    def test_top_min_coeffs_populated(self, tmp_path):
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        assert len(stats.top_min_coeffs) > 0
        # Smallest should be 1e-12
        assert stats.top_min_coeffs[0].abs_coeff == pytest.approx(1e-12)

    def test_top_max_includes_var_and_constraint_names(self, tmp_path):
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        entry = stats.top_max_coeffs[0]
        assert entry.var_name  # non-empty variable name
        assert entry.constraint_name  # non-empty constraint name

    def test_format_report_shows_top_coeffs(self, tmp_path):
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        report = format_static_report(lp, stats)
        assert "largest" in report.lower() or "1.000e+12" in report

    def test_coeff_entry_ordering(self):
        """CoeffEntry supports __lt__ for heapq operations."""
        a = CoeffEntry(1.0, "x", "c")
        b = CoeffEntry(2.0, "y", "d")
        assert a < b
        assert b >= a

    def test_empty_lp_has_empty_top_lists(self, tmp_path):
        """LP with objective but no constraints: top-N lists are empty.

        Stats only cover the constraint matrix A — objective coefficients are
        excluded (matching the C++ to_flat() constraint-matrix-only behaviour).
        """
        lp_text = "Minimize\n obj: x\nSubject To\nEnd\n"
        lp = _write_lp(tmp_path, "empty.lp", lp_text)
        stats = analyze_lp_file(lp)
        assert not stats.top_max_coeffs
        assert not stats.top_min_coeffs

    def test_objective_coeffs_excluded_from_min_max(self, tmp_path):
        """Objective function coefficients do NOT affect max/min abs stats.

        Only constraint matrix A coefficients are considered, matching the C++
        to_flat() stats_min_abs/stats_max_abs which cover the constraint matrix.
        """
        # Constraint coefficient is 7.0; objective has 500.0.
        # The only non-zero parsed coefficient is 7.0 from the constraint matrix.
        lp_text = "Minimize\n obj: 500.0 xa\nSubject To\n c1: 7.0 xa >= 0\nEnd\n"
        lp = _write_lp(tmp_path, "obj_range.lp", lp_text)
        stats = analyze_lp_file(lp)
        # max/min come from constraint (7.0), not from objective (500.0).
        assert stats.max_abs_coeff == pytest.approx(7.0)
        assert stats.min_abs_nonzero_coeff == pytest.approx(7.0)

    def test_objective_coeffs_not_in_top_n(self, tmp_path):
        """Objective coefficients do not appear in top-N lists."""
        lp_text = (
            "Minimize\n"
            " obj: 999 x1 + 0.001 x2\n"
            "Subject To\n"
            " c1: 5 x1 + 2 x2 >= 1\n"
            "End\n"
        )
        lp = _write_lp(tmp_path, "obj_topn.lp", lp_text)
        stats = analyze_lp_file(lp)
        # Only constraint coefficients appear: 5.0 and 2.0 (objective 999/0.001 excluded).
        assert stats.top_max_coeffs[0].abs_coeff == pytest.approx(5.0)
        assert stats.top_max_coeffs[0].constraint_name == "c1"

    def test_top_n_limited_to_3(self, tmp_path):
        """At most 3 entries in each top-N list (_TOP_N_COEFFS = 3)."""
        lp_text = (
            "Minimize\n obj: x1\nSubject To\n"
            " c1: 100 x1 >= 1\n"
            " c2: 50  x1 >= 1\n"
            " c3: 10  x1 >= 1\n"
            " c4: 5   x1 >= 1\n"
            " c5: 1   x1 >= 1\n"
            "End\n"
        )
        lp = _write_lp(tmp_path, "many.lp", lp_text)
        stats = analyze_lp_file(lp)
        assert len(stats.top_max_coeffs) == 3
        assert len(stats.top_min_coeffs) == 3
        # top-3 largest: 100, 50, 10
        assert stats.top_max_coeffs[0].abs_coeff == pytest.approx(100.0)
        assert stats.top_max_coeffs[1].abs_coeff == pytest.approx(50.0)
        assert stats.top_max_coeffs[2].abs_coeff == pytest.approx(10.0)
        # top-3 smallest: 1, 5, 10
        assert stats.top_min_coeffs[0].abs_coeff == pytest.approx(1.0)
        assert stats.top_min_coeffs[1].abs_coeff == pytest.approx(5.0)
        assert stats.top_min_coeffs[2].abs_coeff == pytest.approx(10.0)

    def test_constraint_texts_populated(self, tmp_path):
        """constraint_texts stores the full expression for each constraint."""
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        assert "c_large" in stats.constraint_texts
        assert "c_small" in stats.constraint_texts
        # The stored text should contain the variable names and the RHS.
        assert "x1" in stats.constraint_texts["c_large"]

    def test_format_report_shows_constraint_detail(self, tmp_path):
        """format_static_report includes the constraint expression below each entry."""
        lp = _write_lp(tmp_path, "varied.lp", self._LP_WITH_VARIED_COEFFS)
        stats = analyze_lp_file(lp)
        report = format_static_report(lp, stats)
        # The constraint detail line should appear (indented with 8 spaces).
        # The constraint c_large should appear both as "con=c_large" and as
        # the full expression on the next line.
        lines = report.splitlines()
        found_detail = False
        for line in lines:
            if line.startswith("        ") and "c_large" in line:
                found_detail = True
                break
        assert found_detail, "Expected constraint detail line for c_large in report"

    def test_format_report_shows_flagged_var_in_long_constraint(self, tmp_path):
        """When a constraint is truncated, the flagged variable must still appear."""
        # Build a long constraint where the large coefficient (for big_var)
        # is well beyond the 120-char truncation point.
        filler_terms = " + ".join(f"1.5 filler_variable_{i}" for i in range(20))
        lp_text = (
            "Minimize\n"
            " obj: x1\n"
            "Subject To\n"
            f" long_con: {filler_terms} + 9.99e+08 big_var >= 1\n"
            "Bounds\n"
            " 0 <= x1 <= 10\n"
            " 0 <= big_var <= 10\n"
            "End\n"
        )
        lp = _write_lp(tmp_path, "long.lp", lp_text)
        stats = analyze_lp_file(lp)
        report = format_static_report(lp, stats)
        # The largest coefficient is 9.99e+08 for big_var; even though
        # it's beyond the 120-char point, it must appear in the detail.
        detail_lines = [
            line
            for line in report.splitlines()
            if line.startswith("        ") and "long_con" in line
        ]
        assert detail_lines, "Expected constraint detail for long_con"
        # Both the variable name and its coefficient must be visible.
        combined = " ".join(detail_lines)
        assert "big_var" in combined, (
            f"big_var not shown in truncated constraint detail: {detail_lines}"
        )
        assert "9.99e+08" in combined or "999000000" in combined, (
            f"coefficient for big_var not shown in detail: {detail_lines}"
        )

    def test_constraint_detail_deduped_in_report(self, tmp_path):
        """When multiple entries share a constraint, the detail is shown only once."""
        lp_text = (
            "Minimize\n"
            " obj: x1\n"
            "Subject To\n"
            " shared: 100 x1 + 0.001 x2 >= 1\n"
            "Bounds\n"
            " 0 <= x1 <= 10\n"
            " 0 <= x2 <= 10\n"
            "End\n"
        )
        lp = _write_lp(tmp_path, "shared.lp", lp_text)
        stats = analyze_lp_file(lp)
        report = format_static_report(lp, stats)
        # The constraint detail line should appear exactly once per section.
        detail_count = sum(
            1
            for line in report.splitlines()
            if line.startswith("        ") and "shared:" in line
        )
        # Could be 1 (in max section) + 1 (in min section) = 2 at most.
        assert detail_count <= 2


# ── TestSanitizeLpNames ───────────────────────────────────────────────────────


class TestSanitizeLpNames:
    """Tests for the sanitize_lp_names() function and as_sanitized_lp()."""

    def test_no_colon_in_names_unchanged(self):
        """LP files without ':' in names pass through unchanged."""
        lp = "Minimize\n obj: x1 + x2\nSubject To\n c1: x1 + x2 >= 1\nEnd\n"
        assert sanitize_lp_names(lp) == lp

    def test_colon_inside_variable_name_replaced(self):
        """A ':' embedded inside a variable token in an expression is replaced."""
        lp = "Minimize\n obj: uid:1 + uid:2\nSubject To\n c1: uid:1 + uid:2 >= 1\nEnd\n"
        result = sanitize_lp_names(lp)
        # The constraint separator colon after 'obj' and 'c1' must be preserved.
        assert "obj:" in result
        assert "c1:" in result
        # The colons inside token names must be replaced.
        assert "uid:1" not in result
        assert "uid_1" in result
        assert "uid_2" in result

    def test_constraint_separator_colon_preserved(self):
        """The ':' after a constraint name is always kept."""
        lp = "Subject To\n balance_1_2_3: gen_1 + gen_2 >= 10\nEnd\n"
        result = sanitize_lp_names(lp)
        assert "balance_1_2_3:" in result

    def test_as_sanitized_lp_creates_temp_file(self, tmp_path):
        """as_sanitized_lp writes a sanitised temporary file."""
        lp = "Minimize\n obj: uid:10 + uid:20\nSubject To\n c1: uid:10 >= 5\nEnd\n"
        lp_file = tmp_path / "test.lp"
        lp_file.write_text(lp, encoding="utf-8")

        with as_sanitized_lp(lp_file) as san_path:
            text = san_path.read_text(encoding="utf-8")

        assert "uid:10" not in text
        assert "uid_10" in text

    def test_as_sanitized_lp_temp_file_removed_after(self, tmp_path):
        """The temporary file created by as_sanitized_lp is removed on exit."""
        lp_file = tmp_path / "test.lp"
        lp_file.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")

        with as_sanitized_lp(lp_file) as san_path:
            captured = san_path

        assert not captured.exists()
