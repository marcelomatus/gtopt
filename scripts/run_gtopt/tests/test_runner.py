# SPDX-License-Identifier: BSD-3-Clause
"""Tests for solution checking, command building, and reporting."""

from pathlib import Path
from unittest.mock import patch

from run_gtopt._runner import (
    check_solution,
    find_error_lp_files,
    report_solution,
    run_check_lp,
    run_gtopt,
    run_plp2gtopt,
)


def test_check_solution_csv(tmp_path: Path):
    """Parse a minimal solution.csv."""
    sol = tmp_path / "solution.csv"
    sol.write_text("status,objective\n0,12345.6\n")
    result = check_solution(tmp_path)
    assert result["status"] == 0
    assert abs(result["objective"] - 12345.6) < 1e-6


def test_check_solution_missing(tmp_path: Path):
    """Missing solution file returns empty dict."""
    result = check_solution(tmp_path)
    assert not result


def test_check_solution_malformed(tmp_path: Path):
    """Malformed CSV returns empty or partial dict."""
    sol = tmp_path / "solution.csv"
    sol.write_text("garbage\n")
    result = check_solution(tmp_path)
    assert isinstance(result, dict)


def test_check_solution_non_optimal(tmp_path: Path):
    """Parse solution with non-optimal status."""
    sol = tmp_path / "solution.csv"
    sol.write_text("status,objective\n1,0.0\n")
    result = check_solution(tmp_path)
    assert result["status"] == 1


def test_report_solution_empty_dir(tmp_path: Path, capsys):
    """report_solution on empty directory does not crash."""
    report_solution(tmp_path)
    captured = capsys.readouterr()
    assert "no solution file found" in captured.out


def test_report_solution_optimal(tmp_path: Path, capsys):
    """report_solution shows OPTIMAL for status 0."""
    sol = tmp_path / "solution.csv"
    sol.write_text("status,objective\n0,500.0\n")
    report_solution(tmp_path)
    captured = capsys.readouterr()
    assert "OPTIMAL" in captured.out
    assert "500" in captured.out


def test_run_gtopt_builds_command(tmp_path: Path):
    """run_gtopt passes threads and compression to subprocess."""
    with patch("run_gtopt._runner.subprocess.run") as mock_run:
        mock_run.return_value.returncode = 0
        rc = run_gtopt("/usr/bin/gtopt", tmp_path, threads=4, compression="zstd")
        assert rc == 0
        cmd = mock_run.call_args[0][0]
        assert "--lp-threads" in cmd
        assert "4" in cmd
        assert "--output-compression" in cmd
        assert "zstd" in cmd


def test_run_plp2gtopt_builds_command(tmp_path: Path):
    """run_plp2gtopt passes input/output dirs to subprocess."""
    with patch("run_gtopt._runner.subprocess.run") as mock_run:
        mock_run.return_value.returncode = 0
        rc = run_plp2gtopt("/usr/bin/plp2gtopt", tmp_path / "in", tmp_path / "out")
        assert rc == 0
        cmd = mock_run.call_args[0][0]
        assert "-o" in cmd


def test_find_error_lp_files_empty(tmp_path: Path):
    """No error LP files in empty directory."""
    assert not find_error_lp_files(tmp_path)


def test_find_error_lp_files_found(tmp_path: Path):
    """Finds all error*.lp files sorted by mtime."""
    logs = tmp_path / "logs"
    logs.mkdir()
    import time

    for i in range(5):
        f = logs / f"error_{i}_0.lp"
        f.write_text(f"lp{i}")
        # Ensure distinct mtimes
        time.sleep(0.01)
    result = find_error_lp_files(logs)
    assert len(result) == 5
    assert all(f.name.startswith("error") for f in result)
    # Oldest first
    assert result[0].name == "error_0_0.lp"


def test_find_error_lp_files_nonexistent():
    """Non-existent directory returns empty list."""
    assert not find_error_lp_files(Path("/no/such/dir"))


def test_find_error_lp_files_compressed(tmp_path: Path):
    """Finds compressed error LP files (.lp.gz, .lp.zst)."""
    logs = tmp_path / "logs"
    logs.mkdir()
    (logs / "error_0_0.lp.gz").write_text("")
    (logs / "error_1_0.lp.zst").write_text("")
    result = find_error_lp_files(logs)
    assert len(result) == 2


def test_run_check_lp_no_files(capsys):
    """run_check_lp with empty list does nothing."""
    run_check_lp([])
    captured = capsys.readouterr()
    assert captured.out == ""


def test_run_check_lp_prints_instructions(tmp_path: Path, capsys):
    """run_check_lp prints user instructions even without the tool."""
    logs = tmp_path / "logs"
    logs.mkdir()
    lp = logs / "error_0_0.lp"
    lp.write_text("min: x;\n")
    with patch("shutil.which", return_value=None):
        run_check_lp([lp])
    captured = capsys.readouterr()
    assert "Error LP Diagnostics" in captured.out
    assert "gtopt_check_lp" in captured.out
    assert str(logs) in captured.out
