# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the run discovery / attach helpers."""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

from run_gtopt._attach import (
    RunInfo,
    find_run_by_pid,
    list_runs,
    print_run_list,
    registry_dir,
    resolve_target,
)


def _make_status(out_dir: Path, **fields: object) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    payload: dict[str, object] = {
        "version": 1,
        "status": "running",
        "iteration": 5,
        "max_iterations": 50,
        "lower_bound": 100.0,
        "upper_bound": 110.0,
        "gap": 0.05,
        "elapsed_s": 12.3,
        "method": "cascade",
    }
    payload.update(fields)
    path = out_dir / "solver_status.json"
    path.write_text(json.dumps(payload), encoding="utf-8")
    return path


@pytest.fixture
def isolated_registry(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Point ``$XDG_CACHE_HOME`` at a fresh temp dir for the test."""
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    monkeypatch.delenv("HOME", raising=False)
    rdir = tmp_path / "cache" / "gtopt" / "runs"
    rdir.mkdir(parents=True, exist_ok=True)
    return rdir


def test_registry_dir_uses_xdg(monkeypatch: pytest.MonkeyPatch, tmp_path: Path):
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path))
    assert registry_dir() == tmp_path / "gtopt" / "runs"


def test_registry_dir_falls_back_to_home(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
):
    monkeypatch.delenv("XDG_CACHE_HOME", raising=False)
    monkeypatch.setenv("HOME", str(tmp_path))
    assert registry_dir() == tmp_path / ".cache" / "gtopt" / "runs"


def test_list_runs_empty_when_no_registry(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
):
    """No directory → empty list, no exception."""
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "missing"))
    monkeypatch.delenv("HOME", raising=False)
    assert not list_runs()


def test_list_runs_picks_up_live_pid(isolated_registry: Path, tmp_path: Path):
    out = tmp_path / "case" / "results"
    _make_status(out)
    pid = os.getpid()  # this test process is alive by definition
    (isolated_registry / str(pid)).write_text(str(out) + "\n")

    runs = list_runs()
    assert len(runs) == 1
    r = runs[0]
    assert r.pid == pid
    assert r.output_dir == out
    assert r.status == "running"
    assert r.iteration == 5
    assert r.gap == pytest.approx(0.05)
    assert r.method == "cascade"
    assert r.case_name == "case"
    assert r.alive is True


def test_list_runs_prunes_stale_entries(isolated_registry: Path, tmp_path: Path):
    """A registry entry whose PID isn't alive is removed by default."""
    out = tmp_path / "case" / "results"
    _make_status(out)
    # PID 0 is never a valid process id → guaranteed stale.
    stale = isolated_registry / "0"
    stale.write_text(str(out) + "\n")

    runs = list_runs(prune_stale=True)
    assert not runs
    assert not stale.exists(), "stale entry should have been pruned"


def test_list_runs_keeps_stale_when_requested(isolated_registry: Path, tmp_path: Path):
    out = tmp_path / "case" / "results"
    _make_status(out)
    stale = isolated_registry / "0"
    stale.write_text(str(out) + "\n")

    runs = list_runs(prune_stale=False)
    # Even with prune disabled, PID 0 may resolve as alive=False.
    assert len(runs) == 1
    assert runs[0].alive is False
    assert stale.exists(), "non-pruning mode must not delete the entry"


def test_find_run_by_pid_missing_returns_none(isolated_registry: Path):
    assert find_run_by_pid(424242) is None


def test_resolve_target_by_pid(isolated_registry: Path, tmp_path: Path):
    out = tmp_path / "case" / "results"
    _make_status(out)
    pid = os.getpid()
    (isolated_registry / str(pid)).write_text(str(out) + "\n")

    info = resolve_target(str(pid))
    assert info is not None
    assert info.pid == pid
    assert info.output_dir == out


def test_resolve_target_by_path_results_dir(tmp_path: Path):
    """A path to a case directory finds the status file under results/."""
    case = tmp_path / "case"
    out = case / "results"
    _make_status(out, pid=os.getpid())

    info = resolve_target(str(case))
    assert info is not None
    assert info.output_dir == out
    assert info.pid == os.getpid()


def test_resolve_target_by_path_status_file(tmp_path: Path):
    """A direct path to solver_status.json works too."""
    out = tmp_path / "case" / "out"
    sf = _make_status(out)
    info = resolve_target(str(sf))
    assert info is not None
    assert info.output_dir == out


def test_resolve_target_unknown_returns_none(tmp_path: Path):
    assert resolve_target(str(tmp_path / "nope")) is None


def test_print_run_list_handles_empty(capsys: pytest.CaptureFixture):
    print_run_list([])
    out = capsys.readouterr().out
    assert "No live gtopt runs" in out


def test_print_run_list_renders_row(capsys: pytest.CaptureFixture, tmp_path: Path):
    info = RunInfo(
        pid=12345,
        output_dir=tmp_path / "case" / "results",
        status="running",
        iteration=10,
        max_iterations=100,
        gap=0.025,
        elapsed_s=600.0,
        method="cascade",
        case_name="case",
        alive=True,
    )
    print_run_list([info])
    out = capsys.readouterr().out
    assert "12345" in out
    assert "running" in out
    assert "cascade" in out
    assert "10/100" in out
    assert "case" in out
