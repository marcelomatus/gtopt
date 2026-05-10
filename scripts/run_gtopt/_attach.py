# SPDX-License-Identifier: BSD-3-Clause
"""Discovery and attach helpers for run_gtopt.

The C++ solver writes a small registry entry under
``$XDG_CACHE_HOME/gtopt/runs/<pid>`` (or ``~/.cache/gtopt/runs/<pid>``)
on the first ``write_solver_status()`` call.  Each entry is a one-line
file containing the absolute path of the solver's output directory.

This module provides:

* :func:`registry_dir` — resolve the registry path.
* :func:`list_runs`    — enumerate live runs (PID + output_dir + status
                         summary), skipping stale entries via
                         ``os.kill(pid, 0)``.
* :func:`resolve_target` — turn a CLI argument (PID or path) into the
                           directory we should attach to.
* :func:`run_attach_loop` — drive the existing :class:`SolverDisplay`
                            in observer mode (no subprocess).
"""

from __future__ import annotations

import json
import logging
import os
import time
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger(__name__)


@dataclass(slots=True)
class RunInfo:
    """Snapshot of one live (or recently-live) gtopt run."""

    pid: int
    output_dir: Path
    status: str | None = None  # "running" / "converged" / ...
    iteration: int | None = None
    max_iterations: int | None = None
    lower_bound: float | None = None
    upper_bound: float | None = None
    gap: float | None = None
    elapsed_s: float | None = None
    method: str | None = None
    case_name: str | None = None
    alive: bool = True


def registry_dir() -> Path:
    """Return ``$XDG_CACHE_HOME/gtopt/runs`` (or the HOME-derived path).

    Mirrors the C++ side in ``solver_status.cpp::runs_registry_dir``.
    """
    xdg = os.environ.get("XDG_CACHE_HOME") or ""
    if xdg:
        base = Path(xdg)
    else:
        home = os.environ.get("HOME") or ""
        base = Path(home) / ".cache" if home else Path(".")
    return base / "gtopt" / "runs"


def _pid_alive(pid: int) -> bool:
    """``True`` when ``pid`` is alive *and* belongs to this user."""
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        # Process exists but is owned by another user — treat as alive
        # so we don't pretend it's stale; just unstoppable from here.
        return True
    except OSError:
        return False
    return True


def _status_file_for(output_dir: Path) -> Path:
    return output_dir / "solver_status.json"


def _load_status(path: Path) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}


def _enrich(info: RunInfo) -> RunInfo:
    status_file = _status_file_for(info.output_dir)
    data = _load_status(status_file)
    if not data:
        return info
    info.status = data.get("status")
    info.iteration = data.get("iteration")
    info.max_iterations = data.get("max_iterations")
    info.lower_bound = data.get("lower_bound")
    info.upper_bound = data.get("upper_bound")
    info.gap = data.get("gap")
    info.elapsed_s = data.get("elapsed_s")
    info.method = data.get("method")
    # Best-effort case name = parent directory of output_dir.  Most
    # gtopt cases place their output under ``<case>/output/`` or
    # ``<case>/results/``, so the parent is the canonical case name.
    info.case_name = info.output_dir.parent.name or info.output_dir.name
    return info


def list_runs(prune_stale: bool = True) -> list[RunInfo]:
    """Enumerate live gtopt runs from the registry.

    Stale entries (process gone) are silently removed when
    ``prune_stale`` is ``True``.  Each surviving entry is enriched with
    the latest snapshot from its ``solver_status.json``.
    """
    rdir = registry_dir()
    if not rdir.is_dir():
        return []

    runs: list[RunInfo] = []
    for entry in sorted(rdir.iterdir(), key=lambda p: p.name):
        if not entry.is_file():
            continue
        try:
            pid = int(entry.name)
        except ValueError:
            continue
        try:
            line = entry.read_text(encoding="utf-8").strip()
        except OSError:
            continue
        if not line:
            continue
        out_dir = Path(line)
        alive = _pid_alive(pid)
        if not alive and prune_stale:
            try:
                entry.unlink()
            except OSError:
                pass
            continue
        runs.append(_enrich(RunInfo(pid=pid, output_dir=out_dir, alive=alive)))
    return runs


def find_run_by_pid(pid: int) -> RunInfo | None:
    entry = registry_dir() / str(pid)
    if not entry.is_file():
        return None
    try:
        out_dir = Path(entry.read_text(encoding="utf-8").strip())
    except OSError:
        return None
    return _enrich(RunInfo(pid=pid, output_dir=out_dir, alive=_pid_alive(pid)))


def resolve_target(target: str) -> RunInfo | None:
    """Resolve a CLI ``target`` to a :class:`RunInfo`.

    ``target`` may be:

    * a numeric PID present in the registry
    * an absolute or relative path to a case directory or directly to a
      ``solver_status.json`` (we infer the output_dir from its parent)
    """
    # Numeric PID lookup — first try registry, fall back to /proc cwd
    if target.isdigit():
        return find_run_by_pid(int(target))

    p = Path(target).expanduser().resolve()
    if not p.exists():
        return None
    if p.is_file() and p.name == "solver_status.json":
        out_dir = p.parent
    elif p.is_dir():
        # Try ``solver_status.json`` directly; if absent, look in
        # conventional sub-dirs that gtopt may use.
        candidates = [p, p / "results", p / "output"]
        out_dir = next(
            (c for c in candidates if (c / "solver_status.json").is_file()),
            p,
        )
    else:
        return None

    # Try to read the PID from the status file (newer builds embed it).
    data = _load_status(_status_file_for(out_dir))
    pid = int(data.get("pid", -1) or -1)
    return _enrich(RunInfo(pid=pid, output_dir=out_dir, alive=_pid_alive(pid)))


# ---------------------------------------------------------------------------
# Attach: drive the existing TUI in observer mode (no subprocess)
# ---------------------------------------------------------------------------


def run_attach_loop(info: RunInfo) -> int:
    """Attach the Rich TUI to an already-running gtopt.

    Returns 0 when the user quits (q) — we never SIGTERM the solver
    here.  ``s`` still works because the TUI writes the stop file
    next to the discovered ``solver_status.json``.
    """
    # Local import keeps the CLI fast for ``--list`` / ``--help``.
    from ._tui import SolverDisplay  # noqa: PLC0415

    case_dir = (
        info.output_dir.parent if info.output_dir.parent.exists() else (info.output_dir)
    )
    display = SolverDisplay(
        case_name=info.case_name or case_dir.name,
        case_dir=case_dir,
        solver_hint=info.method or "",
    )
    print(
        f"Attaching to gtopt pid={info.pid} "
        f"output={info.output_dir} (case={info.case_name})"
    )
    display.start()
    try:
        # Loop until the user presses 'q', or the solver disappears.
        # The TUI thread does its own polling; we just monitor liveness
        # at a relaxed cadence so an exited solver doesn't leave the
        # TUI running forever.
        while not display.quit_requested.is_set():
            if info.pid > 0 and not _pid_alive(info.pid):
                print(f"\ngtopt pid={info.pid} exited; detaching.")
                break
            time.sleep(0.5)
    except KeyboardInterrupt:
        # Ctrl-C in attach mode = detach without killing the solver.
        print("\nDetaching (Ctrl-C); gtopt continues running.")
    finally:
        display.stop()
        try:
            display.print_final(0)
        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            log.debug("print_final failed", exc_info=True)
    return 0


def print_run_list(runs: list[RunInfo]) -> None:
    """Render a compact one-row-per-run summary to stdout."""
    if not runs:
        print(f"No live gtopt runs found.\n  (Registry: {registry_dir()})")
        return

    print(
        f"{'PID':>7}  {'STATUS':<12} {'METHOD':<12} {'ITER':>10}  "
        f"{'GAP%':>7}  {'ELAPSED':>8}  CASE  /  OUTPUT_DIR"
    )
    for r in runs:
        iter_str = (
            f"{r.iteration}/{r.max_iterations}"
            if r.iteration is not None and r.max_iterations
            else (str(r.iteration) if r.iteration is not None else "-")
        )
        gap_str = f"{100.0 * r.gap:.2f}" if r.gap is not None else "-"
        elapsed_str = f"{int(r.elapsed_s)}s" if r.elapsed_s is not None else "-"
        flag = " " if r.alive else "*"
        print(
            f"{r.pid:>7}{flag} {r.status or '-':<12} {r.method or '-':<12} "
            f"{iter_str:>10}  {gap_str:>7}  {elapsed_str:>8}  "
            f"{r.case_name or '-'}  /  {r.output_dir}"
        )
    if any(not r.alive for r in runs):
        print("  (* = stale entry; process no longer alive)")
