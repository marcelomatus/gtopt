# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_check_lp – benchmark module."""

import subprocess
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest


# ── Helpers ──────────────────────────────────────────────────────────────────


def _write_lp(tmp_path: Path, name: str, content: str) -> Path:
    """Write a tiny LP file to tmp_path and return its Path."""
    p = tmp_path / name
    p.write_text(content, encoding="utf-8")
    return p


# ── TestBenchmarkParsing ──────────────────────────────────────────────────────


class TestBenchmarkParsing:
    """Cover _benchmark.py parsing helpers and data classes."""

    def test_parse_time_cplex(self):
        from gtopt_check_lp._benchmark import _parse_time  # noqa: PLC0415

        assert _parse_time("Solution time =    0.12 sec.") == pytest.approx(0.12)

    def test_parse_time_highs(self):
        from gtopt_check_lp._benchmark import _parse_time  # noqa: PLC0415

        assert _parse_time("HiGHS run time      :          0.05") == pytest.approx(0.05)

    def test_parse_time_clp(self):
        from gtopt_check_lp._benchmark import _parse_time  # noqa: PLC0415

        assert _parse_time("Total time (CPU seconds):       0.02") == pytest.approx(
            0.02
        )

    def test_parse_time_unknown(self):
        from gtopt_check_lp._benchmark import _parse_time  # noqa: PLC0415

        assert _parse_time("no time info here") == -1.0

    def test_parse_objective_highs(self):
        from gtopt_check_lp._benchmark import _parse_objective  # noqa: PLC0415

        assert _parse_objective("Objective value     :  1.0e+01") == "1.0e+01"

    def test_parse_objective_cplex(self):
        from gtopt_check_lp._benchmark import _parse_objective  # noqa: PLC0415

        assert _parse_objective("Objective =  1.2345e+06") == "1.2345e+06"

    def test_parse_objective_clp(self):
        from gtopt_check_lp._benchmark import _parse_objective  # noqa: PLC0415

        assert _parse_objective("Optimal objective 10") == "10"

    def test_parse_objective_na(self):
        from gtopt_check_lp._benchmark import _parse_objective  # noqa: PLC0415

        assert _parse_objective("no objective here") == "N/A"

    def test_parse_status_optimal(self):
        from gtopt_check_lp._benchmark import _parse_status  # noqa: PLC0415

        assert _parse_status("Solution Optimal") == "optimal"

    def test_parse_status_infeasible(self):
        from gtopt_check_lp._benchmark import _parse_status  # noqa: PLC0415

        assert _parse_status("Primal infeasible") == "infeasible"

    def test_parse_status_unbounded(self):
        from gtopt_check_lp._benchmark import _parse_status  # noqa: PLC0415

        assert _parse_status("Problem unbounded") == "unbounded"

    def test_parse_status_highs(self):
        from gtopt_check_lp._benchmark import _parse_status  # noqa: PLC0415

        assert _parse_status("Model status        : Optimal") == "optimal"

    def test_parse_status_unknown(self):
        from gtopt_check_lp._benchmark import _parse_status  # noqa: PLC0415

        assert _parse_status("no status info") == "unknown"

    def test_parse_dimensions_highs(self):
        from gtopt_check_lp._benchmark import _parse_dimensions  # noqa: PLC0415

        r, c = _parse_dimensions("LP problem has 3 rows; 2 cols; 5 nonzeros")
        assert r == 3
        assert c == 2

    def test_parse_dimensions_generic(self):
        from gtopt_check_lp._benchmark import _parse_dimensions  # noqa: PLC0415

        r, c = _parse_dimensions("Problem has 10 rows and 20 columns")
        assert r == 10
        assert c == 20

    def test_parse_dimensions_none(self):
        from gtopt_check_lp._benchmark import _parse_dimensions  # noqa: PLC0415

        r, c = _parse_dimensions("no dims")
        assert r == 0
        assert c == 0


# ── TestBenchmarkRunners ──────────────────────────────────────────────────────


class TestBenchmarkRunners:
    """Cover benchmark runner functions with mocks."""

    def test_run_cplex_not_found(self):
        from gtopt_check_lp._benchmark import _run_cplex  # noqa: PLC0415

        with patch("gtopt_check_lp._benchmark.find_cplex_binary", return_value=None):
            result = _run_cplex(Path("d.lp"), "dual", 0, 30)
        assert result is None

    def test_run_cplex_success(self, tmp_path):
        from gtopt_check_lp._benchmark import _run_cplex  # noqa: PLC0415

        lp = tmp_path / "test.lp"
        lp.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")

        mock_result = MagicMock()
        mock_result.stdout = (
            "LP problem has 3 rows; 2 cols; 5 nonzeros\n"
            "Solution time =    0.05 sec.\nObjective =  10.0\nSolution Optimal"
        )
        mock_result.stderr = ""

        with patch(
            "gtopt_check_lp._benchmark.find_cplex_binary", return_value="/usr/bin/cplex"
        ), patch("subprocess.run", return_value=mock_result):
            result = _run_cplex(lp, "dual", 2, 30)
        assert result is not None
        assert result.solver == "cplex"
        assert result.time_s == pytest.approx(0.05)
        assert result.status == "optimal"

    def test_run_cplex_timeout(self, tmp_path):
        from gtopt_check_lp._benchmark import _run_cplex  # noqa: PLC0415

        lp = tmp_path / "test.lp"
        lp.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")

        with patch(
            "gtopt_check_lp._benchmark.find_cplex_binary", return_value="/usr/bin/cplex"
        ), patch("subprocess.run", side_effect=subprocess.TimeoutExpired("cplex", 30)):
            result = _run_cplex(lp, "barrier", 0, 30)
        assert result is not None
        assert "error" in result.status

    def test_run_highs_not_found(self):
        from gtopt_check_lp._benchmark import _run_highs  # noqa: PLC0415

        with patch("shutil.which", return_value=None):
            result = _run_highs(Path("d.lp"), "dual", 0, 30)
        assert result is None

    def test_run_highs_success(self, tmp_path):
        from gtopt_check_lp._benchmark import _run_highs  # noqa: PLC0415

        lp = tmp_path / "test.lp"
        lp.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")

        mock_result = MagicMock()
        mock_result.stdout = (
            "LP problem has 5 rows; 3 cols; 10 nonzeros\n"
            "Model status        : Optimal\n"
            "Objective value     :  42.0\n"
            "HiGHS run time      :          0.01"
        )
        mock_result.stderr = ""

        with patch("shutil.which", return_value="/usr/bin/highs"), patch(
            "subprocess.run", return_value=mock_result
        ):
            result = _run_highs(lp, "barrier", 4, 30)
        assert result is not None
        assert result.solver == "highs"
        assert result.time_s == pytest.approx(0.01)
        assert result.threads == 4

    def test_run_clp_not_found(self):
        from gtopt_check_lp._benchmark import _run_clp  # noqa: PLC0415

        with patch("shutil.which", return_value=None):
            result = _run_clp(Path("d.lp"), "dual", 0, 30)
        assert result is None

    def test_run_clp_success(self, tmp_path):
        from gtopt_check_lp._benchmark import _run_clp  # noqa: PLC0415

        lp = tmp_path / "test.lp"
        lp.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")

        mock_result = MagicMock()
        mock_result.stdout = (
            "10 rows and 5 columns\n"
            "Optimal objective 100\n"
            "Total time (CPU seconds):       0.02"
        )
        mock_result.stderr = ""

        with patch("shutil.which", return_value="/usr/bin/clp"), patch(
            "subprocess.run", return_value=mock_result
        ):
            result = _run_clp(lp, "primal", 2, 30)
        assert result is not None
        assert result.solver == "clp"
        assert result.algorithm == "primal"

    def test_run_cbc_not_found(self):
        from gtopt_check_lp._benchmark import _run_cbc  # noqa: PLC0415

        with patch("shutil.which", return_value=None):
            result = _run_cbc(Path("d.lp"), "dual", 0, 30)
        assert result is None

    def test_run_cbc_success(self, tmp_path):
        from gtopt_check_lp._benchmark import _run_cbc  # noqa: PLC0415

        lp = tmp_path / "test.lp"
        lp.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")

        mock_result = MagicMock()
        mock_result.stdout = "Optimal objective 50\nTotal time (CPU seconds):  0.03"
        mock_result.stderr = ""

        with patch("shutil.which", return_value="/usr/bin/cbc"), patch(
            "subprocess.run", return_value=mock_result
        ):
            result = _run_cbc(lp, "default", 0, 30)
        assert result is not None
        assert result.solver == "cbc"


# ── TestBenchmarkDriver ───────────────────────────────────────────────────────


class TestBenchmarkDriver:
    """Cover run_benchmark and format_benchmark_table."""

    def test_run_benchmark_no_solvers(self, tmp_path):
        from gtopt_check_lp._benchmark import run_benchmark  # noqa: PLC0415

        lp = tmp_path / "test.lp"
        lp.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")

        with patch(
            "gtopt_check_lp._benchmark.find_cplex_binary", return_value=None
        ), patch("shutil.which", return_value=None):
            report = run_benchmark(lp, algos=["dual"], thread_counts=[0])
        assert len(report.results) == 0

    def test_run_benchmark_with_highs(self, tmp_path):
        from gtopt_check_lp._benchmark import run_benchmark  # noqa: PLC0415

        lp = tmp_path / "test.lp"
        lp.write_text("Minimize\n obj: x\nEnd\n", encoding="utf-8")

        mock_result = MagicMock()
        mock_result.stdout = (
            "Model status        : Optimal\n"
            "Objective value     :  10.0\n"
            "HiGHS run time      :          0.01"
        )
        mock_result.stderr = ""

        def _which(name):
            return "/usr/bin/highs" if name == "highs" else None

        with patch(
            "gtopt_check_lp._benchmark.find_cplex_binary", return_value=None
        ), patch("shutil.which", side_effect=_which), patch(
            "subprocess.run", return_value=mock_result
        ):
            report = run_benchmark(lp, algos=["dual"], thread_counts=[0], timeout=10)
        assert len(report.results) > 0
        assert report.results[0].solver == "highs"

    def test_format_benchmark_table_empty(self):
        from gtopt_check_lp._benchmark import (  # noqa: PLC0415
            BenchmarkReport,
            format_benchmark_table,
        )

        report = BenchmarkReport(lp_path=Path("test.lp"))
        table = format_benchmark_table(report, use_color=False)
        assert "No solvers available" in table

    def test_format_benchmark_table_with_results(self):
        from gtopt_check_lp._benchmark import (  # noqa: PLC0415
            BenchmarkReport,
            BenchmarkResult,
            format_benchmark_table,
        )

        report = BenchmarkReport(
            lp_path=Path("test.lp"),
            results=[
                BenchmarkResult(
                    solver="highs",
                    algorithm="dual",
                    threads=0,
                    time_s=0.05,
                    status="optimal",
                    objective="10.0",
                    rows=5,
                    cols=3,
                ),
                BenchmarkResult(
                    solver="clp",
                    algorithm="barrier",
                    threads=2,
                    time_s=-1.0,
                    status="error: timeout",
                    objective="N/A",
                ),
            ],
        )
        table = format_benchmark_table(report, use_color=False)
        assert "highs" in table
        assert "clp" in table
        assert "5 rows" in table
        assert "error" in table
