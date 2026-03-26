# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for gtopt_check_solvers."""

from __future__ import annotations

import json
import os
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

from gtopt_check_solvers._binary import find_gtopt_binary
from gtopt_check_solvers._solver_tests import (
    BUILTIN_TESTS,
    SolverTestReport,
    SolverTestResult,
    _read_solution_csv,
    list_available_solvers,
    run_solver_tests,
)
from gtopt_check_solvers.gtopt_check_solvers import (
    _make_parser,
    main,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _write_solution_csv(output_dir: Path, status: int, obj_value: float) -> None:
    """Write a minimal solution.csv to *output_dir*."""
    output_dir.mkdir(parents=True, exist_ok=True)
    sol = output_dir / "solution.csv"
    sol.write_text(
        f"obj_value,kappa,status\n{obj_value},{1.0},{status}\n",
        encoding="utf-8",
    )


# ---------------------------------------------------------------------------
# _binary.py
# ---------------------------------------------------------------------------


class TestFindGtoptBinary:
    def test_returns_none_when_not_found(self, tmp_path):
        with patch.dict(os.environ, {"GTOPT_BIN": ""}, clear=False):
            with patch("shutil.which", return_value=None):
                with patch(
                    "gtopt_check_solvers._binary._standard_build_paths",
                    return_value=[],
                ):
                    result = find_gtopt_binary()
                    assert result is None

    def test_env_var_takes_precedence(self, tmp_path):
        fake_bin = tmp_path / "gtopt"
        fake_bin.touch(mode=0o755)
        with patch.dict(os.environ, {"GTOPT_BIN": str(fake_bin)}, clear=False):
            result = find_gtopt_binary()
            assert result == str(fake_bin)

    def test_falls_back_to_path(self, tmp_path):
        fake_bin = tmp_path / "gtopt"
        fake_bin.touch(mode=0o755)
        with patch.dict(os.environ, {"GTOPT_BIN": ""}, clear=False):
            with patch("shutil.which", return_value=str(fake_bin)):
                result = find_gtopt_binary()
                assert result == str(fake_bin)


# ---------------------------------------------------------------------------
# _solver_tests.py  — BUILTIN_TESTS
# ---------------------------------------------------------------------------


class TestBuiltinTests:
    def test_all_test_names_unique(self):
        names = [name for name, _, _ in BUILTIN_TESTS]
        assert len(names) == len(set(names)), "Duplicate test case names"

    def test_all_expected_statuses_valid(self):
        for name, _, expected_status in BUILTIN_TESTS:
            assert expected_status in (0, 1, 2), (
                f"Test '{name}' has unexpected expected_status={expected_status}"
            )

    def test_all_jsons_are_dicts(self):
        for name, planning, _ in BUILTIN_TESTS:
            assert isinstance(planning, dict), f"Test '{name}' planning is not a dict"
            assert "system" in planning, f"Test '{name}' missing 'system' key"
            assert "simulation" in planning, f"Test '{name}' missing 'simulation' key"


# ---------------------------------------------------------------------------
# _solver_tests.py  — _read_solution_csv
# ---------------------------------------------------------------------------


class TestReadSolutionCsv:
    def test_returns_empty_dict_when_missing(self, tmp_path):
        assert not _read_solution_csv(tmp_path)

    def test_reads_first_data_row(self, tmp_path):
        _write_solution_csv(tmp_path, status=0, obj_value=5.0)
        row = _read_solution_csv(tmp_path)
        assert row["status"] == "0"
        assert float(row["obj_value"]) == pytest.approx(5.0)

    def test_returns_empty_dict_for_empty_file(self, tmp_path):
        (tmp_path / "solution.csv").write_text("", encoding="utf-8")
        assert not _read_solution_csv(tmp_path)


# ---------------------------------------------------------------------------
# _solver_tests.py  — list_available_solvers
# ---------------------------------------------------------------------------


class TestListAvailableSolvers:
    def test_parses_gtopt_output(self):
        mock_out = "Available LP solvers:\n  clp\n  cbc\n"
        with patch("subprocess.run") as mock_run:
            mock_run.return_value = MagicMock(stdout=mock_out, returncode=0)
            solvers = list_available_solvers("/fake/gtopt")
        assert "clp" in solvers
        assert "cbc" in solvers

    def test_returns_empty_on_file_not_found(self):
        with patch("subprocess.run", side_effect=FileNotFoundError()):
            solvers = list_available_solvers("/nonexistent/gtopt")
        assert not solvers

    def test_returns_empty_on_timeout(self):
        import subprocess  # noqa: PLC0415

        with patch(
            "subprocess.run",
            side_effect=subprocess.TimeoutExpired("/fake/gtopt", 10),
        ):
            solvers = list_available_solvers("/fake/gtopt")
        assert not solvers

    def test_ignores_multiword_lines(self):
        mock_out = (
            "Available LP solvers:\n  clp\n  highs\nSome other line with spaces\n"
        )
        with patch("subprocess.run") as mock_run:
            mock_run.return_value = MagicMock(stdout=mock_out, returncode=0)
            solvers = list_available_solvers("/fake/gtopt")
        assert "clp" in solvers
        assert "highs" in solvers
        assert "Some" not in solvers

    def test_robust_against_descriptions(self):
        """Solver lines with appended descriptions should not be included."""
        mock_out = (
            "Available LP solvers:\n"
            "  clp\n"
            "  highs  (open-source)\n"
            "  cplex  (commercial, IBM)\n"
        )
        with patch("subprocess.run") as mock_run:
            mock_run.return_value = MagicMock(stdout=mock_out, returncode=0)
            solvers = list_available_solvers("/fake/gtopt")
        assert "clp" in solvers
        # Lines with descriptions are not indented-single-word; ignored
        assert "highs" not in solvers  # has trailing description
        assert "cplex" not in solvers  # has trailing description


# ---------------------------------------------------------------------------
# _solver_tests.py  — run_solver_tests (with mocked subprocess)
# ---------------------------------------------------------------------------


class TestRunSolverTests:
    def _make_mock_gtopt(self, tmp_path, status=0, obj_value=5.0, returncode=0):
        """Return a side_effect function that creates solution.csv."""

        def _side_effect(cmd, **kwargs):
            # Find the output directory from the JSON file
            json_arg = next(
                (a for a in cmd if a.endswith(".json") and Path(a).exists()), None
            )
            if json_arg:
                with open(json_arg, encoding="utf-8") as fh:
                    plan = json.load(fh)
                out_dir = Path(plan["options"].get("output_directory", tmp_path))
            else:
                out_dir = tmp_path
            _write_solution_csv(out_dir, status=status, obj_value=obj_value)
            return MagicMock(returncode=returncode, stdout="", stderr="")

        return _side_effect

    def test_all_tests_pass_with_mocked_binary(self, tmp_path):
        with patch(
            "subprocess.run",
            side_effect=self._make_mock_gtopt(tmp_path),
        ):
            report = run_solver_tests("/fake/gtopt", "clp")

        assert report.solver == "clp"
        assert report.n_passed == len(BUILTIN_TESTS)
        assert report.n_failed == 0
        assert report.passed

    def test_test_fails_when_solution_csv_missing(self, tmp_path):
        with patch("subprocess.run") as mock_run:
            mock_run.return_value = MagicMock(returncode=0, stdout="", stderr="")
            report = run_solver_tests("/fake/gtopt", "clp")

        assert report.n_failed > 0
        assert not report.passed

    def test_test_fails_on_gtopt_error_exit(self, tmp_path):
        with patch(
            "subprocess.run",
            side_effect=self._make_mock_gtopt(tmp_path, returncode=3),
        ):
            report = run_solver_tests("/fake/gtopt", "clp")

        assert not report.passed

    def test_test_fails_on_timeout(self):
        import subprocess  # noqa: PLC0415

        with patch(
            "subprocess.run",
            side_effect=subprocess.TimeoutExpired("/fake/gtopt", 60),
        ):
            report = run_solver_tests("/fake/gtopt", "clp")

        assert not report.passed
        assert any("timed out" in r.message for r in report.results)

    def test_subset_of_tests(self, tmp_path):
        with patch(
            "subprocess.run",
            side_effect=self._make_mock_gtopt(tmp_path),
        ):
            report = run_solver_tests(
                "/fake/gtopt", "clp", test_names=["single_bus_lp"]
            )

        assert len(report.results) == 1
        assert report.results[0].name == "single_bus_lp"


# ---------------------------------------------------------------------------
# SolverTestReport
# ---------------------------------------------------------------------------


class TestSolverTestReport:
    def test_passed_true_when_all_pass(self):
        report = SolverTestReport(solver="clp")
        report.results.append(SolverTestResult(name="t1", passed=True))
        report.results.append(SolverTestResult(name="t2", passed=True))
        assert report.passed
        assert report.n_passed == 2
        assert report.n_failed == 0

    def test_passed_false_when_any_fail(self):
        report = SolverTestReport(solver="clp")
        report.results.append(SolverTestResult(name="t1", passed=True))
        report.results.append(SolverTestResult(name="t2", passed=False, message="oops"))
        assert not report.passed
        assert report.n_passed == 1
        assert report.n_failed == 1


# ---------------------------------------------------------------------------
# CLI: _make_parser
# ---------------------------------------------------------------------------


class TestMakeParser:
    def test_defaults(self):
        args = _make_parser().parse_args([])
        assert not args.list_solvers
        assert args.solvers is None
        assert args.gtopt_bin is None
        assert args.timeout == pytest.approx(60.0)
        assert not args.verbose
        assert not args.no_color

    def test_list_flag(self):
        args = _make_parser().parse_args(["--list"])
        assert args.list_solvers

    def test_solver_flag_accumulates(self):
        args = _make_parser().parse_args(["--solver", "clp", "--solver", "highs"])
        assert args.solvers == ["clp", "highs"]

    def test_timeout_flag(self):
        args = _make_parser().parse_args(["--timeout", "30"])
        assert args.timeout == pytest.approx(30.0)

    def test_gtopt_bin_flag(self):
        args = _make_parser().parse_args(["--gtopt-bin", "/usr/bin/gtopt"])
        assert args.gtopt_bin == "/usr/bin/gtopt"


# ---------------------------------------------------------------------------
# CLI: main() — integration-style tests with mocked subprocess
# ---------------------------------------------------------------------------


class TestMain:
    def _make_mock_gtopt(self, status=0, obj_value=5.0, returncode=0):
        """Return a side_effect for subprocess.run that fakes gtopt."""

        def _side_effect(cmd, **kwargs):
            # --solvers query
            if "--solvers" in cmd:
                return MagicMock(returncode=0, stdout="Available LP solvers:\n  clp\n")
            # LP test run
            json_arg = next(
                (a for a in cmd if a.endswith(".json") and Path(a).exists()), None
            )
            if json_arg:
                with open(json_arg, encoding="utf-8") as fh:
                    plan = json.load(fh)
                out_dir = Path(plan["options"].get("output_directory", "/tmp"))
                _write_solution_csv(out_dir, status=status, obj_value=obj_value)
            return MagicMock(returncode=returncode, stdout="", stderr="")

        return _side_effect

    def test_list_flag_exits_0(self, tmp_path):
        fake_bin = tmp_path / "gtopt"
        fake_bin.touch(mode=0o755)
        with patch(
            "subprocess.run",
            side_effect=self._make_mock_gtopt(),
        ):
            rc = main(["--list", "--gtopt-bin", str(fake_bin)])
        assert rc == 0

    def test_check_all_solvers_passes(self, tmp_path):
        fake_bin = tmp_path / "gtopt"
        fake_bin.touch(mode=0o755)
        with patch(
            "subprocess.run",
            side_effect=self._make_mock_gtopt(),
        ):
            rc = main(["--gtopt-bin", str(fake_bin)])
        assert rc == 0

    def test_missing_gtopt_bin_returns_2(self, tmp_path):
        rc = main(["--gtopt-bin", str(tmp_path / "nonexistent")])
        assert rc == 2

    def test_no_binary_returns_2(self):
        with patch.dict(os.environ, {"GTOPT_BIN": ""}, clear=False):
            with patch("shutil.which", return_value=None):
                with patch(
                    "gtopt_check_solvers._binary._standard_build_paths",
                    return_value=[],
                ):
                    rc = main([])
        assert rc == 2

    def test_unknown_solver_warned_returns_1(self, tmp_path):
        fake_bin = tmp_path / "gtopt"
        fake_bin.touch(mode=0o755)
        with patch(
            "subprocess.run",
            side_effect=self._make_mock_gtopt(),
        ):
            rc = main(["--solver", "unknown_solver", "--gtopt-bin", str(fake_bin)])
        # unknown solver not in available list → no tests to run → exit 1
        assert rc == 1

    def test_version_flag(self, capsys):
        with pytest.raises(SystemExit) as exc:
            main(["--version"])
        assert exc.value.code == 0
