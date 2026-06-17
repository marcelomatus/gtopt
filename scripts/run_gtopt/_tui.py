# SPDX-License-Identifier: BSD-3-Clause
"""Rich terminal UI for interactive gtopt solver monitoring.

When gtopt runs in an interactive terminal, this module provides a
live dashboard (inspired by the Claude Code aesthetic) that shows:

  - Solver status, case name, method, elapsed time
  - Iteration progress bar with current pass indicator
  - Convergence statistics (LB, UB, gap) with sparkline trend
  - Iteration history table
  - System resource usage (CPU load, active workers)
  - Live solver log output
  - Interactive command bar (stop, stats, help)

When stdout is not a TTY (piped, redirected, or running in background),
the display is disabled and solver output flows to stdout/log files
unchanged.

Display modes (switch with Tab or 1/2/3):

  1  Dashboard — stats, progress, iteration history, log
  2  Grid — SDDP phase activity grid (forward/backward/elastic/aperture)
  3  Convergence — per-scene cost heatmap + bound sparklines

Interactive commands (single-key, no Enter required):

  s  Graceful stop — saves cuts, finishes current iteration
  i  Toggle system stats overlay (from planning JSON)
  h  Toggle help overlay
  q  Quit — terminate the solver immediately
"""

from __future__ import annotations

import json
import logging
import re
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from rich.console import Group
    from rich.panel import Panel

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_POLL_INTERVAL = 1.0  # seconds between status file polls
_MAX_LOG_LINES = 16  # rolling log buffer size
_MAX_HISTORY_ROWS = 8  # iteration rows shown in the table
# Rich Live refresh rate.  At 4 Hz the dashboard noticeably flickered and
# pegged a CPU core — 2 Hz is plenty for human feedback (Rich coalesces
# multiple updates per frame anyway) and roughly halves the TUI thread's
# CPU footprint.
_REFRESH_PER_SECOND = 2
_SPARKLINE_WIDTH = 24
_PROGRESS_BAR_WIDTH = 40

# Display modes — cycle with Tab, jump with 1/2/3
_MODE_DASHBOARD = 0  # full dashboard (stats, progress, history, log)
_MODE_GRID = 1  # SDDP phase activity grid
_MODE_CONVERGENCE = 2  # per-scene convergence heatmap + sparklines
_MODE_COUNT = 3
_MODE_NAMES = {
    _MODE_DASHBOARD: "dashboard",
    _MODE_GRID: "grid",
    _MODE_CONVERGENCE: "convergence",
}

# Convergence heatmap color ramp (green → yellow → red, 8 levels)
_HEAT_BLOCKS = "▁▂▃▄▅▆▇█"
_HEAT_STYLES = [
    "bold green",
    "green",
    "dark_green",
    "yellow",
    "dark_orange",
    "orange_red1",
    "red",
    "bold red",
]

# Pass names from the C++ SolverStatusSnapshot::current_pass enum
_PASS_NAMES = {0: "idle", 1: "forward", 2: "backward"}
_PASS_STYLES = {0: "dim", 1: "bold green", 2: "bold magenta"}

# The file name the C++ solver checks for graceful stop requests
_STOP_REQUEST_FILE = "sddp_stop_request.json"

# Braille spinner frames (same cadence as plp2gtopt / Claude Code)
_SPINNER = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]

# ---------------------------------------------------------------------------
# SDDP phase grid — per-phase/iteration activity tracker
# ---------------------------------------------------------------------------

# Regex for the uniform "SDDP <Tag> [i<iter> s<scene> p<phase>]" format
_SDDP_LOG_RE = re.compile(
    # Per-(scene, phase) activity lines from sddp_method_iteration / forward
    # / backward.  ``p(\d+)`` is followed by an optional ``/<total>`` suffix
    # since the per-phase Forward INFO log (sddp_forward_pass.cpp) emits
    # ``p3/51`` — capturing the ``<total>`` lets the grid pre-allocate
    # all phase columns from the very first log line, instead of waiting
    # until the solver actually reaches the highest phase to discover the
    # true width.  The aperture group ``a(\d+)`` remains optional and
    # exclusive (apertures don't co-emit a phase total).
    r"SDDP (\w+) \[i(\d+) s(\d+) p(\d+)(?:/(\d+))?(?:\s+a(\d+))?\]"
)
# Optional kappa magnitude trailing the "SDDP Kappa [...]" prefix; used
# to surface the worst observed conditioning, not just the warning count.
_SDDP_KAPPA_VALUE_RE = re.compile(r"high kappa\s+([0-9eE+\-.]+)")

# Cell states (priority order: higher overwrites lower)
_GRID_IDLE = 0
_GRID_FORWARD = 1
_GRID_BACKWARD = 2
_GRID_ELASTIC = 3  # elastic solve during forward pass
_GRID_APERTURE = 4  # aperture pass (backward)
_GRID_INFEASIBLE = 5  # infeasible / error

# Cell rendering: (character, Rich style)
_GRID_CELL_STYLES: dict[int, tuple[str, str]] = {
    _GRID_IDLE: ("·", "dim"),
    _GRID_FORWARD: ("▶", "bold cyan"),
    _GRID_BACKWARD: ("◀", "bold magenta"),
    _GRID_ELASTIC: ("◆", "bold yellow"),
    _GRID_APERTURE: ("▣", "bold blue"),
    _GRID_INFEASIBLE: ("✗", "bold red"),
}

# Maximum number of phases displayed in the grid
_GRID_MAX_PHASES = 52
# Maximum number of iterations shown (most recent)
_GRID_MAX_ITERS = 12

# Aggregate ("all scenes") cell shape — density of scene coverage at this
# (iter, phase) cell.  Index = floor(coverage * 5), clamped to [0, 4].
# Coverage 0 → "·"; coverage > 0 always renders something visible.
_GRID_AGG_SHAPES: tuple[str, ...] = ("·", "░", "▒", "▓", "█")


class SDDPGridTracker:
    """Tracks SDDP forward/backward/elastic/aperture activity per (scene, iter, phase).

    Parses log lines with the ``sddp_log()`` format and maintains a 3D grid:
    ``grid[scene][(iter, phase)] -> cell_state``.

    The grid is rendered as one small matrix per scene, with phases on X
    and iterations on Y, using colored block characters.
    """

    def __init__(self) -> None:
        # grid[scene][(iter, phase)] = cell_state
        self._grid: dict[int, dict[tuple[int, int], int]] = {}
        self._max_phase: int = 0
        self._max_iter: int = 0
        self._scenes: set[int] = set()
        self._active_cell: tuple[int, int, int] | None = None  # (scene, iter, phase)
        # Diagnostic counters for indicator lights
        self._kappa_warnings: int = 0
        self._max_kappa: float = 0.0
        self._elastic_count: int = 0
        self._infeasible_count: int = 0
        self._aperture_count: int = 0

    def process_line(self, line: str) -> None:
        """Parse an SDDP log line and update the grid."""
        m = _SDDP_LOG_RE.search(line)
        if m is None:
            return

        tag = m.group(1)
        it = int(m.group(2))
        sc = int(m.group(3))
        ph = int(m.group(4))
        # Group 5 = total phase count (e.g. "/51"); group 6 = aperture uid.
        total_phases_str = m.group(5)
        if total_phases_str is not None:
            # PhaseUid is 1-based; total=51 → highest valid ph index is 51.
            self._max_phase = max(self._max_phase, int(total_phases_str))
        has_aperture = m.group(6) is not None

        # Determine cell state from tag + context
        if tag == "Forward":
            if "elastic" in line.lower():
                state = _GRID_ELASTIC
                self._elastic_count += 1
            else:
                state = _GRID_FORWARD
        elif tag == "Backward":
            if has_aperture:
                state = _GRID_APERTURE
                self._aperture_count += 1
            else:
                state = _GRID_BACKWARD
        elif tag == "Aperture":
            state = _GRID_APERTURE
            self._aperture_count += 1
        elif tag == "Elastic":
            state = _GRID_ELASTIC
            self._elastic_count += 1
        elif tag == "Kappa":
            self._kappa_warnings += 1
            # Capture the actual kappa magnitude so the indicator strip
            # can show worst-conditioning instead of just a count of
            # warnings — the count alone tells you nothing about how
            # bad the LP is.
            kv = _SDDP_KAPPA_VALUE_RE.search(line)
            if kv is not None:
                try:
                    self._max_kappa = max(self._max_kappa, float(kv.group(1)))
                except ValueError:
                    pass
            return
        else:
            # Iter, Sim, Update, Init — no phase-level grid info
            return

        # Check for infeasible markers
        if "infeasib" in line.lower():
            self._infeasible_count += 1
            state = max(state, _GRID_INFEASIBLE)

        # Update grid dimensions and state
        self._scenes.add(sc)
        self._max_phase = max(self._max_phase, ph)
        self._max_iter = max(self._max_iter, it)

        if sc not in self._grid:
            self._grid[sc] = {}

        # Only overwrite if new state has higher priority
        cell_key = (it, ph)
        current = self._grid[sc].get(cell_key, _GRID_IDLE)
        if state > current:
            self._grid[sc][cell_key] = state

        self._active_cell = (sc, it, ph)

    @property
    def has_data(self) -> bool:
        return bool(self._grid)

    @property
    def scenes(self) -> list[int]:
        return sorted(self._scenes)

    @property
    def max_phase(self) -> int:
        return self._max_phase

    @property
    def max_iter(self) -> int:
        return self._max_iter

    def get_cell(self, scene: int, iteration: int, phase: int) -> int:
        sg = self._grid.get(scene)
        if sg is None:
            return _GRID_IDLE
        return sg.get((iteration, phase), _GRID_IDLE)

    def aggregate_cell(self, iteration: int, phase: int) -> tuple[int, float]:
        """Return ``(dominant_state, coverage_fraction)`` across all scenes.

        ``dominant_state`` is the highest-priority state seen at this
        ``(iteration, phase)`` in any scene — using the same priority
        order as :meth:`get_cell` (forward < backward < elastic <
        aperture < infeasible).  ``coverage_fraction`` is the share of
        registered scenes that have visited this cell at all (state >
        idle), in [0, 1].

        Used by :func:`_build_sddp_grid` to render the pinned
        "all scenes" summary row at the top of Grid mode.
        """
        if not self._scenes:
            return _GRID_IDLE, 0.0
        worst = _GRID_IDLE
        visited = 0
        for sc in self._scenes:
            sg = self._grid.get(sc)
            if sg is None:
                continue
            cell = sg.get((iteration, phase), _GRID_IDLE)
            if cell != _GRID_IDLE:
                visited += 1
                worst = max(worst, cell)
        return worst, visited / len(self._scenes)

    @property
    def active_cell(self) -> tuple[int, int, int] | None:
        return self._active_cell

    @property
    def kappa_warnings(self) -> int:
        return self._kappa_warnings

    @property
    def max_kappa(self) -> float:
        return self._max_kappa

    @property
    def elastic_count(self) -> int:
        return self._elastic_count

    @property
    def infeasible_count(self) -> int:
        return self._infeasible_count

    @property
    def aperture_count(self) -> int:
        return self._aperture_count

    # Map C++ GridCell chars to Python grid states
    _CELL_CHAR_MAP: dict[str, int] = {
        "F": _GRID_FORWARD,
        "B": _GRID_BACKWARD,
        "E": _GRID_ELASTIC,
        "A": _GRID_APERTURE,
        "X": _GRID_INFEASIBLE,
    }

    def load_from_status(self, status: dict) -> None:
        """Merge phase_grid data from the status JSON into the grid.

        The C++ solver writes a ``phase_grid`` section keyed by PhaseUid:
        ``{"i": 0, "s": 0, "cells": {"1": "F", "2": ".", "3": "B"}}``.
        Iterating in sorted-UID order gives the natural column ordering
        without any UID-as-index arithmetic on either side.  This
        format also tolerates legacy positional strings (``"FF.FEB"``)
        for backward compatibility with status files captured before
        the format change — the legacy branch treats each character
        position as a 0-based phase column.
        """
        grid_data = status.get("phase_grid")
        if not grid_data:
            return
        for row in grid_data.get("rows", []):
            it = row.get("i", 0)
            sc = row.get("s", 0)
            cells = row.get("cells")
            if not cells:
                continue
            if isinstance(cells, dict):
                # New format: UID-keyed.  Sort by integer UID so the
                # column ordering is stable; the TUI's `ph` axis is
                # 0-based ordinal-within-sorted-UIDs (the natural
                # phase column index for the dense 1-based PhaseUid
                # layout the simulation framework allocates).
                cells_sorted = sorted(cells.items(), key=lambda kv: int(kv[0]))
                cells_iter = enumerate(ch for _uid, ch in cells_sorted)
                width = len(cells_sorted)
            else:
                # Legacy format: positional packed string.
                cells_iter = enumerate(cells)
                width = len(cells)
            if width == 0:
                continue
            self._scenes.add(sc)
            self._max_iter = max(self._max_iter, it)
            self._max_phase = max(self._max_phase, width - 1)
            if sc not in self._grid:
                self._grid[sc] = {}
            for ph, ch in cells_iter:
                state = self._CELL_CHAR_MAP.get(ch, _GRID_IDLE)
                if state == _GRID_IDLE:
                    continue
                cell_key = (it, ph)
                current = self._grid[sc].get(cell_key, _GRID_IDLE)
                if state > current:
                    self._grid[sc][cell_key] = state


# ---------------------------------------------------------------------------
# Solver phase tracking (plan table)
# ---------------------------------------------------------------------------

# Ordered solver phases: (key, human label)
_SOLVER_PHASES: list[tuple[str, str]] = [
    ("parse", "Parsing input files"),
    ("validate", "Validating planning"),
    ("build_lp", "Building LP model"),
    ("optimize", "Solving"),
    ("sim_pass", "Simulation pass"),
    ("solution", "Solution statistics"),
    ("write", "Writing output files"),
]

# Log line patterns that trigger phase transitions.
# Each entry: (compiled regex, phase key, action).
# Actions: "start" = begin phase, "done" = complete phase, "detail" = update detail.
_PHASE_TRIGGERS: list[tuple[re.Pattern[str], str, str]] = [
    (re.compile(r"Parsing input file"), "parse", "start"),
    (re.compile(r"Parse all input files time"), "parse", "done"),
    (re.compile(r"Planning validation passed"), "validate", "done"),
    (re.compile(r"Planning validation failed"), "validate", "done"),
    (re.compile(r"=== Building LP model ==="), "build_lp", "start"),
    (re.compile(r"Build lp time"), "build_lp", "done"),
    (re.compile(r"=== System optimization ==="), "optimize", "start"),
    (
        re.compile(r"SDDP Iter \[i\d+\]: === starting \((\d+) of (\d+)\) ==="),
        "optimize",
        "detail",
    ),
    (
        re.compile(r"Monolithic(?:Method|Solver): scene (\d+) done.*\((\d+)/(\d+)\)"),
        "optimize",
        "detail",
    ),
    (re.compile(r"SDDP Sim \[i\d+\]: === simulation pass"), "sim_pass", "start"),
    (re.compile(r"SDDP Sim \[i\d+\]: done in"), "sim_pass", "done"),
    (re.compile(r"=== Solution statistics ==="), "solution", "done"),
    (re.compile(r"=== Output writing ==="), "write", "start"),
    (re.compile(r"Write output time"), "write", "done"),
]


@dataclass
class _PhaseState:
    """Tracks a single solver phase's status and timing."""

    label: str
    status: str = "pending"  # pending | active | done | skipped
    t_start: float = 0.0
    t_end: float = 0.0
    detail: str = ""

    @property
    def elapsed(self) -> float:
        if self.status == "active":
            return time.monotonic() - self.t_start
        if self.status == "done":
            return self.t_end - self.t_start
        return 0.0


class SolverPhaseTracker:
    """Tracks solver progress through its execution phases.

    Parses solver log lines to detect phase transitions and maintains
    state for each phase (pending / active / done / skipped).
    """

    def __init__(self) -> None:
        self._states: dict[str, _PhaseState] = {}
        self._order: list[str] = []
        self._current: str | None = None
        self._frame: int = 0
        self._is_monolithic: bool = False

        for key, label in _SOLVER_PHASES:
            self._states[key] = _PhaseState(label=label)
            self._order.append(key)

    def process_line(self, line: str) -> None:
        """Parse a solver log line and update phase states."""
        if not self._is_monolithic and (
            "MonolithicMethod:" in line or "MonolithicSolver:" in line
        ):
            self._is_monolithic = True
            self._states["sim_pass"].status = "skipped"

        for pattern, key, action in _PHASE_TRIGGERS:
            match = pattern.search(line)
            if match is None:
                continue
            st = self._states[key]

            if action == "start":
                self._finish_current()
                st.status = "active"
                st.t_start = time.monotonic()
                self._current = key
            elif action == "done":
                if st.status == "pending":
                    # Instant step (validate) or done without explicit start
                    st.status = "done"
                    st.t_start = st.t_end = time.monotonic()
                elif st.status == "active":
                    st.status = "done"
                    st.t_end = time.monotonic()
                if self._current == key:
                    self._current = None
            elif action == "detail" and st.status == "active":
                groups = match.groups()
                if len(groups) == 2:  # noqa: PLR2004
                    st.detail = f"iter {groups[0]}/{groups[1]}"
                elif len(groups) == 3:  # noqa: PLR2004
                    st.detail = f"scene {groups[1]}/{groups[2]}"
            break  # first match wins

    def finish_all(self) -> None:
        """Mark any active phase as done (called when solver exits)."""
        self._finish_current()

    def _finish_current(self) -> None:
        if self._current is not None:
            st = self._states[self._current]
            if st.status == "active":
                st.status = "done"
                st.t_end = time.monotonic()
            self._current = None

    @property
    def order(self) -> list[str]:
        return self._order

    @property
    def states(self) -> dict[str, _PhaseState]:
        return self._states

    def tick(self) -> None:
        """Advance the spinner frame counter."""
        self._frame += 1

    @property
    def frame(self) -> int:
        return self._frame


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def is_interactive() -> bool:
    """Return True when stdout is connected to an interactive terminal."""
    return hasattr(sys.stdout, "isatty") and sys.stdout.isatty()


def _sparkline(values: list[float], width: int = _SPARKLINE_WIDTH) -> str:
    """Render a Unicode sparkline for the last *width* values."""
    if not values:
        return ""
    blocks = " ▁▂▃▄▅▆▇█"
    vals = values[-width:]
    mn, mx = min(vals), max(vals)
    rng = mx - mn if mx > mn else 1.0
    return "".join(blocks[min(8, int((v - mn) / rng * 8))] for v in vals)


def _format_elapsed(seconds: float) -> str:
    """Format seconds as ``MM:SS`` or ``H:MM:SS``."""
    h, rem = divmod(int(seconds), 3600)
    m, s = divmod(rem, 60)
    return f"{h:d}:{m:02d}:{s:02d}" if h else f"{m:02d}:{s:02d}"


def _format_number(val: float) -> str:
    """Human-readable number formatting for objective values."""
    if abs(val) >= 1e9:
        return f"{val:,.0f}"
    if abs(val) >= 1e6:
        return f"{val:,.2f}"
    if abs(val) >= 1e3:
        return f"{val:,.4f}"
    return f"{val:.6f}"


def _load_status(path: Path | None) -> dict[str, Any]:
    """Read and parse the solver status JSON.  Returns ``{}`` on failure."""
    if path is None or not path.is_file():
        return {}
    try:
        text = path.read_text(encoding="utf-8")
        return json.loads(text)
    except (OSError, json.JSONDecodeError):
        return {}


def _find_status_file(case_dir: Path) -> Path:
    """Derive the expected status-file path from *case_dir*.

    The C++ solver writes ``solver_status.json`` under
    ``options.output_directory`` (resolved relative to the planning
    JSON's parent).  We honor that value when readable, and otherwise
    fall back to common conventional dirs (``output/``, ``results/``).
    Legacy file names are also probed for older gtopt builds.
    """
    base = case_dir.parent if case_dir.is_file() else case_dir

    # Build an ordered list of candidate output directories.  Prefer the
    # one declared in the planning JSON (when discoverable) so we follow
    # what the solver actually wrote.
    candidate_dirs: list[Path] = []
    json_path = _find_planning_json(case_dir)
    if json_path is not None:
        try:
            with open(json_path, encoding="utf-8") as f:
                planning = json.load(f)
            out_opt = planning.get("options", {}).get("output_directory", "")
            if isinstance(out_opt, str) and out_opt:
                p = Path(out_opt)
                candidate_dirs.append(p if p.is_absolute() else json_path.parent / p)
        except (OSError, json.JSONDecodeError):
            pass

    # Conventional fallbacks: gtopt cases historically used either
    # "output/" or "results/" depending on vintage.
    for sub in ("output", "results"):
        candidate_dirs.append(base / sub)

    for output_dir in candidate_dirs:
        status = output_dir / "solver_status.json"
        if status.is_file():
            return status
        # Legacy fallback for older gtopt builds
        sddp = output_dir / "sddp_status.json"
        mono = output_dir / "monolithic_status.json"
        if sddp.is_file():
            return sddp
        if mono.is_file():
            return mono

    # Nothing found yet — return the canonical expected path so the
    # caller can keep polling for it to appear.
    return candidate_dirs[0] / "solver_status.json"


def _find_planning_json(case_dir: Path) -> Path | None:
    """Locate the planning JSON file for the case."""
    if case_dir.is_file() and case_dir.suffix == ".json":
        return case_dir
    # Directory case: <dir>/<dir_name>.json
    candidate = case_dir / f"{case_dir.name}.json"
    if candidate.is_file():
        return candidate
    # Sanitized variant
    for suffix in ("_sanitized.json", ".json"):
        for p in case_dir.glob(f"*{suffix}"):
            if p.is_file():
                return p
    return None


def _load_system_stats(json_path: Path | None) -> dict[str, Any]:
    """Extract system stats from the planning JSON for the /stats overlay."""
    if json_path is None or not json_path.is_file():
        return {}
    try:
        with open(json_path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}

    system = data.get("system", {})
    simulation = data.get("simulation", {})
    options = data.get("options", {})

    # Count system elements
    element_counts: dict[str, int] = {}
    for key in (
        "bus_array",
        "generator_array",
        "demand_array",
        "line_array",
        "battery_array",
        "flow_array",
        "converter_array",
        "inflow_array",
        "reservoir_array",
        "fuel_array",
    ):
        arr = system.get(key, [])
        if arr:
            label = key.replace("_array", "").replace("_", " ").title()
            element_counts[label] = len(arr)

    # Simulation structure
    scenarios = simulation.get("scenario_array", [])
    stages = simulation.get("stage_array", [])
    blocks = simulation.get("block_array", [])

    # Options summary
    method = options.get("method", "?")
    solver = options.get("solver", "")
    scale_obj = options.get("scale_objective")
    kirchhoff = options.get("use_kirchhoff")
    single_bus = options.get("use_single_bus")

    return {
        "elements": element_counts,
        "scenarios": len(scenarios),
        "stages": len(stages),
        "blocks": len(blocks),
        "method": method,
        "solver": solver,
        "scale_objective": scale_obj,
        "use_kirchhoff": kirchhoff,
        "use_single_bus": single_bus,
    }


# ---------------------------------------------------------------------------
# Keyboard handling (cbreak mode for single-key input on Linux)
# ---------------------------------------------------------------------------


def _enter_cbreak() -> Any:
    """Set terminal to cbreak mode (single char, no echo).

    Returns the original settings for restoration, or None if
    cbreak mode is not available.
    """
    try:
        import termios  # noqa: PLC0415
        import tty  # noqa: PLC0415

        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        tty.setcbreak(fd)
        return old
    except (ImportError, termios.error, OSError, AttributeError):
        return None


def _exit_cbreak(old_settings: Any) -> None:
    """Restore terminal settings from cbreak mode."""
    if old_settings is None:
        return
    try:
        import termios  # noqa: PLC0415

        termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, old_settings)
    except (ImportError, termios.error, OSError, AttributeError):
        pass


def _poll_key(cbreak_active: bool) -> str | None:
    """Non-blocking single-key read.  Returns the key or None."""
    if not cbreak_active:
        return None
    try:
        import select as sel  # noqa: PLC0415

        if sel.select([sys.stdin], [], [], 0)[0]:
            return sys.stdin.read(1)
    except (OSError, ValueError):
        pass
    return None


# ---------------------------------------------------------------------------
# Rich panel builders
# ---------------------------------------------------------------------------


def _build_header(
    case_name: str,
    data: dict,
    elapsed: float,
    system_stats: dict | None = None,
) -> Panel:
    from rich.panel import Panel  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    status_str = data.get("status", "starting")

    # Method: from status JSON or log detection
    stats = system_stats or {}
    method = stats.get("method", "")
    if not method or method == "?":
        method = (
            "sddp"
            if data.get("max_iterations")
            else "monolithic"
            if data.get("total_scenes") is not None
            else "..."
        )

    # Solver: from log detection or CLI flag
    solver = stats.get("solver", "")

    style_map = {
        "running": "bold yellow",
        "converged": "bold green",
        "initializing": "bold cyan",
        "starting": "bold cyan",
        "completed": "bold green",
        "optimal": "bold green",
    }
    status_style = style_map.get(status_str, "bold white")

    # Build info line: "Method: sddp  Solver: cplex"
    info_parts = [f"Method: {method}"]
    if solver:
        info_parts.append(f"Solver: {solver}")
    info_text = "  ".join(info_parts)

    grid = Table.grid(padding=(0, 2))
    grid.add_column(ratio=1)
    grid.add_column(justify="center", ratio=1)
    grid.add_column(justify="right", ratio=1)
    grid.add_row(
        Text(f"Case: {case_name}", style="bold"),
        Text(info_text, style="dim"),
        Text(_format_elapsed(elapsed), style="bold cyan"),
    )
    grid.add_row(Text(status_str, style=status_style), "", "")

    # Always-visible LB / UB / gap line so operators see convergence
    # progress regardless of which display mode is active (Dashboard,
    # Grid, or Convergence).  Skipped before any iteration data exists.
    lb = data.get("lower_bound")
    ub = data.get("upper_bound")
    gap = data.get("gap")
    if lb is not None or ub is not None:
        # ``Text`` already imported at the top of the function — no
        # need to re-import as ``_T``; just alias it for the same
        # locality the prior alias provided.
        _T = Text
        bounds = _T()
        bounds.append("LB ", style="bold cyan")
        bounds.append(_format_number(lb or 0.0), style="cyan")
        bounds.append("   UB ", style="bold magenta")
        bounds.append(_format_number(ub or 0.0), style="magenta")
        if gap is not None:
            gap_style = (
                "bold green"
                if gap < 0.01
                else "bold yellow"
                if gap < 0.1
                else "bold red"
            )
            bounds.append("   Gap ", style="bold")
            bounds.append(f"{100.0 * gap:.2f}%", style=gap_style)
        # Δ Gap — the change in gap since the previous iteration (in
        # percentage points).  This is the quantity the SDDP/cascade
        # convergence test actually watches (ΔUB-stationarity): once it
        # falls below the per-level tolerance the solver stops.  Small
        # |Δ| → converging (green); larger → still moving (yellow).
        hist = data.get("history") or []
        if len(hist) >= 2:
            dgap = hist[-1].get("gap", 0.0) - hist[-2].get("gap", 0.0)
            adg = abs(dgap)
            dg_style = (
                "bold green"
                if adg < 0.01
                else "bold yellow"
                if adg < 0.05
                else "bold red"
            )
            bounds.append("   ΔGap ", style="bold")
            bounds.append(f"{100.0 * dgap:+.2f}%", style=dg_style)
        # Iteration counter, when SDDP-style data is present
        it = data.get("iteration")
        max_it = data.get("max_iterations")
        if it is not None and max_it:
            bounds.append(f"   iter {it}/{max_it}", style="dim")
        grid.add_row(bounds, "", "")

    return Panel(
        grid,
        title=(
            "[bold cyan]⚡ gtopt[/bold cyan]"
            " [dim]— Generation & Transmission Expansion Planning[/dim]"
        ),
        border_style="cyan",
        padding=(0, 1),
    )


def _build_progress(data: dict) -> Panel:
    from rich.panel import Panel  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    iteration = data.get("iteration", 0)
    max_iter = data.get("max_iterations", 0)
    current_pass = data.get("current_pass", 0)
    scenes_done = data.get("scenes_done")
    pass_name = _PASS_NAMES.get(current_pass, "?")
    pass_style = _PASS_STYLES.get(current_pass, "dim")

    if max_iter > 0:
        pct = min(100, iteration * 100 // max_iter)
        filled = pct * _PROGRESS_BAR_WIDTH // 100
        progress_bar = (
            f"[cyan]{'━' * filled}[/cyan]"
            f"[dim]{'─' * (_PROGRESS_BAR_WIDTH - filled)}[/dim]"
        )
        parts = [
            f"  Iteration [bold]{iteration}[/bold]/{max_iter}  ",
            progress_bar,
            f"  {pct}%",
        ]
        if scenes_done is not None:
            parts.append(f"  scenes: {scenes_done}")
        parts.append(f"  [{pass_style}]\\[{pass_name}][/{pass_style}]")
        text = Text.from_markup("".join(parts))
    else:
        text = Text.from_markup(
            f"  [bold]Solving...[/bold]  [{pass_style}]\\[{pass_name}][/{pass_style}]"
        )

    return Panel(text, border_style="dim cyan", padding=(0, 1))


def _build_async_panel(data: dict) -> Panel | None:
    """Render async scene execution state when max_async_spread > 0."""
    from rich.panel import Panel  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    async_data = data.get("async")
    if not async_data:
        return None

    spread = async_data.get("spread", 0)
    max_spread = async_data.get("max_async_spread", 0)
    converged = async_data.get("converged_scenes", 0)
    pending = async_data.get("pool_tasks_pending", 0)
    active = async_data.get("pool_tasks_active", 0)
    cpu = async_data.get("pool_cpu_load", 0.0)
    iters = async_data.get("scene_iterations", [])
    states = async_data.get("scene_states", [])

    grid = Table.grid(padding=(0, 2))
    grid.add_column(style="bold", min_width=16)
    grid.add_column(min_width=12)
    grid.add_column(style="bold", min_width=16)
    grid.add_column(min_width=12)

    spread_style = "bold red" if spread >= max_spread else "cyan"
    grid.add_row(
        "Spread",
        Text(f"{spread}/{max_spread}", style=spread_style),
        "Converged",
        Text(str(converged), style="bold green" if converged else "dim"),
    )
    grid.add_row(
        "Pool pending",
        Text(str(pending), style="cyan"),
        "Pool active",
        Text(str(active), style="cyan"),
    )
    if cpu > 0:
        grid.add_row("CPU load", Text(f"{cpu:.0f}%", style="cyan"), "", "")

    # Compact per-scene iteration display (up to 20 scenes)
    if iters:
        scene_parts: list[str] = []
        for si, it_val in enumerate(iters[:20]):
            st = states[si] if si < len(states) else "?"
            style = "green" if st == "done" else "cyan" if st == "training" else "dim"
            scene_parts.append(f"[{style}]s{si}:{it_val}[/{style}]")
        scene_text = Text.from_markup("  " + "  ".join(scene_parts))
        grid.add_row("", "", "", "")
        grid.add_row(scene_text, "", "", "")

    return Panel(
        grid,
        title="[bold]Async Scenes[/bold]",
        border_style="dim cyan",
        padding=(0, 1),
    )


def _build_stats(data: dict) -> Panel:
    from rich.panel import Panel  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    lb = data.get("lower_bound", 0.0)
    ub = data.get("upper_bound", 0.0)
    gap = data.get("gap", 0.0)
    converged = data.get("converged", False)
    history = data.get("history", [])
    total_cuts = sum(r.get("cuts_added", 0) for r in history)
    last_cuts = history[-1].get("cuts_added", 0) if history else 0

    gap_style = (
        "bold green" if gap < 0.01 else "bold yellow" if gap < 0.1 else "bold red"
    )
    conv_text = (
        Text("Yes ✓", style="bold green") if converged else Text("No", style="dim")
    )
    gaps = [r.get("gap", 0.0) for r in history]
    spark = _sparkline(gaps) if gaps else ""

    grid = Table.grid(padding=(0, 2))
    grid.add_column(style="bold", min_width=14)
    grid.add_column(min_width=18)
    grid.add_column(style="bold", min_width=14)
    grid.add_column(min_width=18)

    grid.add_row(
        "Lower Bound",
        Text(_format_number(lb), style="cyan"),
        "Upper Bound",
        Text(_format_number(ub), style="cyan"),
    )
    # Render gap as a percentage to match the per-iteration log format
    # (sddp_iteration.cpp emits ``gap=25.00%``).  Six decimals on a
    # fraction like ``0.250043`` is unintuitive for operators glancing
    # at the dashboard.
    grid.add_row(
        "Gap",
        Text(f"{100.0 * gap:.2f}%", style=gap_style),
        "Converged",
        conv_text,
    )
    grid.add_row(
        "Cuts (iter)",
        Text(str(last_cuts)),
        "Cuts (total)",
        Text(str(total_cuts)),
    )
    if spark:
        grid.add_row("Gap trend", Text(spark, style="cyan"), "", "")

    return Panel(
        grid,
        title="[bold]Solver Statistics[/bold]",
        border_style="dim cyan",
        padding=(0, 1),
    )


def _build_history(data: dict) -> Panel:
    from rich.panel import Panel  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415

    history = data.get("history", [])
    table = Table(
        show_header=True,
        header_style="bold",
        border_style="dim",
        padding=(0, 1),
        expand=True,
    )
    table.add_column("Iter", justify="right", style="bold", width=5)
    table.add_column("Lower Bound", justify="right", width=16)
    table.add_column("Upper Bound", justify="right", width=16)
    table.add_column("Gap", justify="right", width=10)
    table.add_column("Δ Gap", justify="right", width=9)
    table.add_column("Fwd", justify="right", width=7)
    table.add_column("Bwd", justify="right", width=7)

    # Δ Gap = change in the gap since the previous iteration, in percentage
    # points (negative = gap shrinking, i.e. converging).  Computed over the
    # FULL history so the first visible row keeps its true predecessor even
    # when the table is sliced to the last _MAX_HISTORY_ROWS.  Replaces the
    # former "Cuts" column — the per-iter gap movement is the convergence
    # signal operators actually track.
    for idx, rec in enumerate(history[-_MAX_HISTORY_ROWS:]):
        full_idx = max(0, len(history) - _MAX_HISTORY_ROWS) + idx
        g = rec.get("gap", 0.0)
        gap_style = "green" if g < 0.01 else "yellow" if g < 0.1 else "red"
        if full_idx > 0:
            dg = g - history[full_idx - 1].get("gap", 0.0)
            # Shrinking gap (dg < 0) is progress → green; growing → red.
            dg_style = "green" if dg < 0 else "red" if dg > 0 else "dim"
            dg_cell = f"[{dg_style}]{100.0 * dg:+.2f}%[/{dg_style}]"
        else:
            dg_cell = "[dim]—[/dim]"
        table.add_row(
            str(rec.get("iteration", 0)),
            _format_number(rec.get("lower_bound", 0.0)),
            _format_number(rec.get("upper_bound", 0.0)),
            f"[{gap_style}]{100.0 * g:.2f}%[/{gap_style}]",
            dg_cell,
            f"{rec.get('forward_pass_s', 0.0):.1f}s",
            f"{rec.get('backward_pass_s', 0.0):.1f}s",
        )

    if not history:
        table.add_row(
            "[dim]waiting for first iteration...[/dim]", "", "", "", "", "", ""
        )

    return Panel(
        table,
        title="[bold]Iteration History[/bold]",
        border_style="dim cyan",
        padding=(0, 0),
    )


def _format_mb(mb: float) -> str:
    """Render a megabyte count as MB or GB depending on magnitude."""
    if mb >= 1024.0:
        return f"{mb / 1024.0:.1f} GB"
    return f"{mb:.0f} MB"


def _has_resource_telemetry(data: dict) -> bool:
    """True when *data* carries any CPU/memory observation worth showing."""
    rt = data.get("realtime") or {}
    if rt.get("cpu_loads") or rt.get("active_workers"):
        return True
    return any(
        data.get(k) is not None
        for k in (
            "pool_process_rss_mb",
            "pool_available_memory_mb",
            "pool_memory_percent",
        )
    )


def _build_system(data: dict) -> Panel:
    from rich.panel import Panel  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    rt = data.get("realtime", {})
    cpus = rt.get("cpu_loads", [])
    workers = rt.get("active_workers", [])
    mem_pcts = rt.get("memory_percent", [])
    rss_vals = rt.get("process_rss_mb", [])
    free_vals = rt.get("available_memory_mb", [])

    # The realtime ring is the AUTHORITATIVE live source: the background
    # SolverMonitor refreshes it every ~0.5 s, so it always reflects the
    # current poll.  The top-level `pool_*` snapshot is written only at
    # iteration boundaries and can lag (or be 0 when the snapshot pool's
    # monitor is stale) — so prefer the ring and use `pool_*` only as a
    # fallback when the ring is empty.  A non-positive `pool_*` is treated
    # as "no reading" so a stale 0 never shadows a fresh ring value.
    pool_rss = data.get("pool_process_rss_mb")
    pool_free = data.get("pool_available_memory_mb")
    pool_pct = data.get("pool_memory_percent")
    async_data = data.get("async") or {}
    pool_active = async_data.get("pool_tasks_active")
    pool_pending = async_data.get("pool_tasks_pending")

    def _live(ring: list, pool_v) -> float:
        if ring:
            return float(ring[-1])
        return float(pool_v) if pool_v is not None and float(pool_v) > 0.0 else 0.0

    cpu_val = cpus[-1] if cpus else 0.0
    worker_val = workers[-1] if workers else 0
    mem_val = _live(mem_pcts, pool_pct)
    rss_val = _live(rss_vals, pool_rss)
    free_val = _live(free_vals, pool_free)

    bar_width = 30
    cpu_filled = int(cpu_val * bar_width / 100)
    cpu_bar = (
        f"[green]{'█' * cpu_filled}[/green][dim]{'░' * (bar_width - cpu_filled)}[/dim]"
    )
    mem_filled = int(mem_val * bar_width / 100)
    mem_bar = (
        f"[blue]{'█' * mem_filled}[/blue][dim]{'░' * (bar_width - mem_filled)}[/dim]"
    )

    grid = Table.grid(padding=(0, 2))
    grid.add_column(min_width=6)
    grid.add_column(min_width=40)
    grid.add_column(min_width=20)

    # Right-column annotations: CPU row reports active workers; MEM row
    # reports gtopt's RSS plus system free memory (the user-visible
    # "headroom" before the OS starts pressuring the process).
    cpu_right = Text(f"Workers: {worker_val}", style="cyan")
    if pool_active is not None or pool_pending is not None:
        a = pool_active if pool_active is not None else 0
        p = pool_pending if pool_pending is not None else 0
        cpu_right = Text(
            f"Workers: {worker_val}  Pool: {a} active  {p} pending",
            style="cyan",
        )

    if free_val > 0.0:
        mem_right = Text(
            f"gtopt: {_format_mb(rss_val)}   Free: {_format_mb(free_val)}",
            style="blue",
        )
    else:
        mem_right = Text(f"gtopt: {_format_mb(rss_val)}", style="blue")

    grid.add_row(
        Text("CPU", style="bold"),
        Text.from_markup(f"{cpu_bar} {cpu_val:.0f}%"),
        cpu_right,
    )
    grid.add_row(
        Text("MEM", style="bold"),
        Text.from_markup(f"{mem_bar} {mem_val:.0f}%"),
        mem_right,
    )

    cpu_spark = _sparkline(cpus) if cpus else ""
    mem_spark = _sparkline(mem_pcts) if mem_pcts else ""
    if cpu_spark:
        grid.add_row(
            Text("Load", style="dim"),
            Text(cpu_spark, style="green"),
            "",
        )
    if mem_spark:
        grid.add_row(
            Text("Mem", style="dim"),
            Text(mem_spark, style="blue"),
            "",
        )

    # LP task stats summary (from top-level JSON, not realtime)
    lp_stats = data.get("lp_task_stats")
    if lp_stats:
        dispatched = lp_stats.get("dispatched", 0)
        avg_cpu = lp_stats.get("avg_cpu_pct", 0.0)
        avg_mem = lp_stats.get("avg_rss_delta_mb", 0.0)
        grid.add_row(
            Text("LP", style="dim"),
            Text(
                f"{dispatched} tasks  avg CPU {avg_cpu:.0f}%  "
                f"avg mem Δ{_format_mb(avg_mem)}",
                style="dim",
            ),
            "",
        )

    return Panel(grid, border_style="dim cyan", padding=(0, 1))


def _build_log(lines: list[str]) -> Panel:
    from rich.panel import Panel  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    log_text = Text()
    if not lines:
        log_text.append("  Waiting for solver output...", style="dim")
    else:
        for line in lines[-10:]:
            lower = line.lower()
            if "error" in lower or "fail" in lower:
                style = "bold red"
            elif "warn" in lower:
                style = "yellow"
            elif "converge" in lower:
                style = "bold green"
            elif "iteration" in lower or "iter " in lower:
                style = "cyan"
            else:
                style = "dim"
            log_text.append(f"  {line}\n", style=style)

    return Panel(
        log_text,
        title="[bold]Output[/bold]",
        border_style="dim cyan",
        padding=(0, 0),
    )


def _build_command_bar(stop_sent: bool, mode: int = _MODE_DASHBOARD) -> Panel:
    """Render the interactive command bar at the bottom of the display."""
    from rich.panel import Panel  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    text = Text()

    # Mode selector: [1] dashboard  [2] grid  [3] convergence  [Tab] cycle
    text.append("  ")
    for m, label in _MODE_NAMES.items():
        key = str(m + 1)
        if m == mode:
            text.append(f"[{key}]", style="bold green")
            text.append(f" {label}", style="bold green")
        else:
            text.append(f"[{key}]", style="dim cyan")
            text.append(f" {label}", style="dim")
        text.append("  ")
    text.append("[Tab]", style="dim cyan")
    text.append(" cycle", style="dim")

    # Separator
    text.append("    │    ", style="dim")

    # Action keys
    text.append("[s]", style="bold cyan")
    if stop_sent:
        text.append(" stop sent", style="dim")
    else:
        text.append(" stop", style="")
    text.append("  ")
    text.append("[i]", style="bold cyan")
    text.append(" stats", style="")
    text.append("  ")
    text.append("[h]", style="bold cyan")
    text.append(" help", style="")
    text.append("  ")
    text.append("[q]", style="bold cyan")
    text.append(" quit", style="")

    return Panel(text, border_style="dim", padding=(0, 0))


def _build_plan_panel(tracker: SolverPhaseTracker) -> Panel:
    """Render the solver phase checklist (plan table)."""
    from rich.panel import Panel as RPanel  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    text = Text()
    for key in tracker.order:
        st = tracker.states[key]
        if st.status == "skipped":
            continue
        text.append("  ")
        if st.status == "done":
            text.append("✓", style="bold green")
            text.append(f" {st.label}")
            text.append(f"  ({st.elapsed:.1f}s)", style="dim")
        elif st.status == "active":
            spinner = _SPINNER[tracker.frame % len(_SPINNER)]
            text.append(spinner, style="bold cyan")
            text.append(f" {st.label}...")
            if st.detail:
                text.append(f"  ({st.detail})", style="cyan")
            text.append(f"  ({st.elapsed:.1f}s)", style="dim")
        else:
            text.append("○", style="dim")
            text.append(f" {st.label}", style="dim")
        text.append("\n")

    return RPanel(text, border_style="dim cyan", padding=(0, 0))


def _build_help_overlay() -> Panel:
    """Render the help overlay panel."""
    from rich.panel import Panel  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    table = Table(show_header=False, border_style="dim", padding=(0, 2), expand=True)
    table.add_column("Key", style="bold cyan", width=8)
    table.add_column("Description")

    commands = [
        ("1", "Dashboard mode — stats, history table, log output"),
        ("2", "Grid mode — SDDP phase activity grid"),
        ("3", "Convergence mode — per-scene cost heatmap + sparklines"),
        ("Tab", "Cycle through display modes"),
        ("s", "Graceful stop — finish current iteration, save cuts, exit"),
        ("i", "Toggle system stats overlay (buses, generators, etc.)"),
        ("h", "Toggle this help overlay"),
        ("q", "Quit — terminate the solver immediately"),
        ("Ctrl+C", "Same as quit"),
        ("j / k", "Grid mode: scroll scenes down / up by 1"),
        ("J / K", "Grid mode: scroll scenes down / up by 5 (page)"),
        ("g / G", "Grid mode: jump to first / last scene"),
    ]
    for key, desc in commands:
        table.add_row(Text(key, style="bold cyan"), desc)

    return Panel(
        table,
        title="[bold]Help — Interactive Commands[/bold]",
        border_style="cyan",
        padding=(0, 1),
    )


def _build_stats_overlay(stats: dict[str, Any]) -> Panel:
    """Render the system stats overlay from the planning JSON."""
    from rich.panel import Panel  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    if not stats:
        return Panel(
            Text("  No planning JSON found", style="dim"),
            title="[bold]System Stats[/bold]",
            border_style="cyan",
            padding=(0, 1),
        )

    grid = Table.grid(padding=(0, 2))
    grid.add_column(style="bold", min_width=18)
    grid.add_column(min_width=10)
    grid.add_column(style="bold", min_width=18)
    grid.add_column(min_width=10)

    # System elements (2-column layout)
    elements = stats.get("elements", {})
    items = list(elements.items())
    for idx in range(0, len(items), 2):
        name1, count1 = items[idx]
        if idx + 1 < len(items):
            name2, count2 = items[idx + 1]
            grid.add_row(
                name1,
                Text(str(count1), style="cyan"),
                name2,
                Text(str(count2), style="cyan"),
            )
        else:
            grid.add_row(name1, Text(str(count1), style="cyan"), "", "")

    # Simulation structure
    grid.add_row("", "", "", "")
    grid.add_row(
        "Scenarios",
        Text(str(stats.get("scenarios", "?")), style="cyan"),
        "Stages",
        Text(str(stats.get("stages", "?")), style="cyan"),
    )
    grid.add_row(
        "Blocks",
        Text(str(stats.get("blocks", "?")), style="cyan"),
        "Method",
        Text(str(stats.get("method", "?")), style="cyan"),
    )
    solver = stats.get("solver", "")
    if solver:
        grid.add_row(
            "Solver",
            Text(solver, style="cyan"),
            "",
            Text("", style="cyan"),
        )

    # Key options
    rows: list[tuple[str, str]] = []
    if stats.get("scale_objective") is not None:
        rows.append(("Scale objective", str(stats["scale_objective"])))
    if stats.get("use_kirchhoff") is not None:
        rows.append(("Kirchhoff", str(stats["use_kirchhoff"])))
    if stats.get("use_single_bus") is not None:
        rows.append(("Single bus", str(stats["use_single_bus"])))
    for idx in range(0, len(rows), 2):
        name1, val1 = rows[idx]
        if idx + 1 < len(rows):
            name2, val2 = rows[idx + 1]
            grid.add_row(name1, Text(val1, style="dim"), name2, Text(val2, style="dim"))
        else:
            grid.add_row(name1, Text(val1, style="dim"), "", "")

    return Panel(
        grid,
        title="[bold]System Stats[/bold]",
        border_style="cyan",
        padding=(0, 1),
    )


def _build_sddp_grid(
    tracker: SDDPGridTracker,
    scroll_offset: int = 0,
    max_visible_scenes: int | None = None,
    active_scene_hint: int | None = None,
) -> Panel:
    """Render the SDDP phase/iteration activity grid.

    Layout (top → bottom):
      1. Legend + diagnostic-indicator lights (always visible).
      2. Pinned **aggregate ("all scenes") row** — one (iter, phase)
         matrix where each cell encodes coverage as a density block
         (``·░▒▓█``) and color as the highest-priority state seen
         across all scenes.  This is what makes the panel *useful*
         when there are many scenes; without it you see at most the
         first N that fit in the terminal.
      3. Visible per-scene matrices, starting at ``scroll_offset``,
         clipped to ``max_visible_scenes``.  Above/below ribbons
         indicate how many scenes are off-screen.

    @p scroll_offset is the index into ``tracker.scenes`` of the first
    scene to render.
    @p max_visible_scenes caps the number of per-scene blocks emitted
    (a budget computed from the live console height in
    :meth:`SolverDisplay._build_display` so the panel never overflows
    the terminal — fixing the "can't scroll" hang).
    @p active_scene_hint is the scene id of the current active cell;
    used purely for the visible-range header so users with many
    scenes see *which* scene the SDDP solver is currently inside.
    """
    from rich.panel import Panel  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    text = Text()

    max_ph = min(tracker.max_phase, _GRID_MAX_PHASES)
    scenes = tracker.scenes
    n_scenes = len(scenes)

    # Legend
    text.append("  ")
    for state, (char, style) in _GRID_CELL_STYLES.items():
        if state == _GRID_IDLE:
            continue
        labels = {
            _GRID_FORWARD: "fwd",
            _GRID_BACKWARD: "bwd",
            _GRID_ELASTIC: "elastic",
            _GRID_APERTURE: "aperture",
            _GRID_INFEASIBLE: "infeas",
        }
        text.append(char, style=style)
        text.append(f" {labels.get(state, '?')}  ", style="dim")

    # Indicator lights for diagnostics.
    # ``kappa`` shows the worst observed conditioning value alongside the
    # warning count — the count alone tells the user nothing about how
    # ill-conditioned the LP got.  Aperture is intentionally omitted
    # here: it is already visible via the ▣ cells in the grid and per-
    # aperture counts are noise during a normal run.
    indicators: list[tuple[str, str, str]] = []
    if tracker.kappa_warnings > 0:
        if tracker.max_kappa > 0:
            kappa_val = f"{tracker.kappa_warnings} (max {tracker.max_kappa:.1e})"
        else:
            kappa_val = str(tracker.kappa_warnings)
        indicators.append(("kappa", kappa_val, "bold red"))
    if tracker.elastic_count > 0:
        indicators.append(("elastic", str(tracker.elastic_count), "bold yellow"))
    if tracker.infeasible_count > 0:
        indicators.append(("infeas", str(tracker.infeasible_count), "bold red"))
    if indicators:
        text.append("   ")
        for label, count, ind_style in indicators:
            text.append(f" {label}:", style="dim")
            text.append(count, style=ind_style)
    text.append("\n")

    # Phase header row reused for the aggregate block AND each per-scene
    # block.  Inlined as a closure to keep the layout tweaks local.
    def _emit_phase_header() -> None:
        text.append("     ")  # indent matches the "  ##  " iter label width
        for ph in range(max_ph + 1):
            if ph % 5 == 0:
                label = str(ph)
                text.append(label, style="dim")
                text.append(" " * max(0, 1 - len(label)))
            else:
                text.append(" ", style="dim")
        text.append("\n")

    # ------------------------------------------------------------------
    # Pinned aggregate row — "all scenes" coverage + dominant state.
    # ------------------------------------------------------------------
    if n_scenes > 0:
        text.append(
            f"  all  ({n_scenes} scenes)",
            style="bold cyan",
        )
        text.append("\n")
        _emit_phase_header()
        first_iter = max(0, tracker.max_iter - _GRID_MAX_ITERS + 1)
        for it in range(first_iter, tracker.max_iter + 1):
            text.append(f"  {it:>2} ", style="dim")
            for ph in range(max_ph + 1):
                state, coverage = tracker.aggregate_cell(it, ph)
                if state == _GRID_IDLE:
                    text.append(" ", style="dim")
                    continue
                # Density char from coverage; color from dominant state.
                # Coverage > 0 always renders at least a "░" so the user
                # sees that *some* scene visited even sparse cells.
                shape_idx = min(
                    len(_GRID_AGG_SHAPES) - 1,
                    max(1, int(coverage * len(_GRID_AGG_SHAPES))),
                )
                _, base_style = _GRID_CELL_STYLES[state]
                text.append(_GRID_AGG_SHAPES[shape_idx], style=base_style)
            text.append("\n")
        # Visual divider before the per-scene blocks.
        text.append("\n")

    # ------------------------------------------------------------------
    # Per-scene blocks — windowed by scroll_offset + max_visible_scenes.
    # ------------------------------------------------------------------
    if n_scenes == 0:
        return Panel(
            text,
            title="[bold]SDDP Phase Grid[/bold]",
            border_style="dim cyan",
            padding=(0, 0),
        )

    if max_visible_scenes is None or max_visible_scenes >= n_scenes:
        visible_count = n_scenes
        offset = 0
    else:
        visible_count = max(1, max_visible_scenes)
        offset = max(0, min(scroll_offset, n_scenes - visible_count))

    visible_scenes = scenes[offset : offset + visible_count]
    above_hidden = offset
    below_hidden = n_scenes - (offset + visible_count)

    if above_hidden > 0 or below_hidden > 0:
        # Range header so operators know which scenes are on-screen.
        active_tag = ""
        if active_scene_hint is not None:
            active_tag = f"   active: s{active_scene_hint}"
        text.append(
            f"  scenes {visible_scenes[0]}–{visible_scenes[-1]} of {n_scenes}"
            f"   (j/k scroll, J/K page, g/G top/bottom){active_tag}",
            style="dim",
        )
        text.append("\n")
    if above_hidden > 0:
        text.append(f"  ▲ {above_hidden} scene(s) above\n", style="dim")

    for sc in visible_scenes:
        text.append(f"  s{sc}", style="bold")
        text.append("\n")
        _emit_phase_header()
        first_iter = max(0, tracker.max_iter - _GRID_MAX_ITERS + 1)
        for it in range(first_iter, tracker.max_iter + 1):
            text.append(f"  {it:>2} ", style="dim")
            for ph in range(max_ph + 1):
                state = tracker.get_cell(sc, it, ph)
                char, style = _GRID_CELL_STYLES[state]
                if (
                    tracker.active_cell is not None
                    and tracker.active_cell == (sc, it, ph)
                    and state != _GRID_IDLE
                ):
                    text.append(char, style=f"{style} reverse")
                else:
                    text.append(char, style=style)
            text.append("\n")

    if below_hidden > 0:
        text.append(f"  ▼ {below_hidden} scene(s) below\n", style="dim")

    return Panel(
        text,
        title="[bold]SDDP Phase Grid[/bold]",
        border_style="dim cyan",
        padding=(0, 0),
    )


# ---------------------------------------------------------------------------
# Convergence heatmap / sparkline panel
# ---------------------------------------------------------------------------


def _heat_style(value: float, lo: float, hi: float) -> tuple[str, str]:
    """Map *value* in [lo, hi] to a (block_char, Rich style) pair."""
    rng = hi - lo if hi > lo else 1.0
    idx = min(7, int((value - lo) / rng * 8))
    return _HEAT_BLOCKS[idx], _HEAT_STYLES[idx]


def _build_convergence(data: dict) -> Panel:
    """Render the per-scene convergence heatmap and bound sparklines.

    Layout:
      1. Aggregate LB / UB sparkline convergence curves
      2. Per-scene cost heatmap (rows=iterations, cols=scenes)
      3. Per-scene UB sparklines
    """
    from rich.panel import Panel  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    text = Text()
    history = data.get("history", [])

    if not history:
        text.append("  Waiting for iteration data...", style="dim")
        return Panel(
            text,
            title="[bold]Convergence[/bold]",
            border_style="dim cyan",
            padding=(0, 1),
        )

    # -- 1. Aggregate bound sparklines --
    lbs = [r.get("lower_bound", 0.0) for r in history]
    ubs = [r.get("upper_bound", 0.0) for r in history]
    gaps = [r.get("gap", 0.0) for r in history]

    text.append("  LB ", style="bold cyan")
    text.append(_sparkline(lbs, width=40), style="cyan")
    text.append(f"  {_format_number(lbs[-1])}", style="cyan")
    text.append("\n")
    text.append("  UB ", style="bold magenta")
    text.append(_sparkline(ubs, width=40), style="magenta")
    text.append(f"  {_format_number(ubs[-1])}", style="magenta")
    text.append("\n")
    text.append("  Gap", style="bold")
    text.append(" ")
    text.append(_sparkline(gaps, width=40), style="yellow")
    gap_val = gaps[-1]
    gap_style = (
        "bold green"
        if gap_val < 0.01
        else "bold yellow"
        if gap_val < 0.1
        else "bold red"
    )
    text.append(f"  {100.0 * gap_val:.2f}%", style=gap_style)
    text.append("\n\n")

    # -- 2. Per-scene cost heatmap --
    # Collect scene_upper_bounds from history
    n_scenes = 0
    scene_ub_matrix: list[list[float]] = []  # [iter_idx][scene_idx]
    for rec in history:
        sub = rec.get("scene_upper_bounds", [])
        if sub:
            n_scenes = max(n_scenes, len(sub))
            scene_ub_matrix.append(sub)

    if scene_ub_matrix and n_scenes > 0:
        # Find global min/max for color mapping
        all_vals = [v for row in scene_ub_matrix for v in row]
        lo, hi = min(all_vals), max(all_vals)

        text.append("  Scene Costs", style="bold")
        text.append("  (rows=iterations, cols=scenes)\n", style="dim")

        # Scene header
        text.append("       ")
        for sc in range(n_scenes):
            if n_scenes <= 20 or sc % 5 == 0:
                label = f"s{sc}"
                text.append(f"{label:<3}", style="dim")
            else:
                text.append("   ", style="dim")
        text.append("\n")

        # Show last _MAX_HISTORY_ROWS iterations
        display_rows = scene_ub_matrix[-_MAX_HISTORY_ROWS:]
        first_iter = len(scene_ub_matrix) - len(display_rows)
        for row_idx, row in enumerate(display_rows):
            it = first_iter + row_idx
            text.append(f"  {it:>3} ", style="dim")
            for val in row:
                char, style = _heat_style(val, lo, hi)
                text.append(f" {char} ", style=style)
            text.append("\n")

        # Color bar legend
        text.append("\n       ")
        text.append("low", style="bold green")
        text.append(" ")
        for i in range(8):
            text.append(_HEAT_BLOCKS[i], style=_HEAT_STYLES[i])
        text.append(" ")
        text.append("high", style="bold red")
        text.append(
            f"   range: [{_format_number(lo)} .. {_format_number(hi)}]", style="dim"
        )
        text.append("\n")

        # -- 3. Per-scene UB sparklines --
        if n_scenes <= 20 and len(scene_ub_matrix) >= 2:
            text.append("\n")
            text.append("  Per-Scene Trends\n", style="bold")
            for sc in range(n_scenes):
                scene_vals = [row[sc] for row in scene_ub_matrix if sc < len(row)]
                text.append(f"  s{sc:<3}", style="dim")
                text.append(_sparkline(scene_vals, width=30))
                text.append(f"  {_format_number(scene_vals[-1])}", style="dim")
                text.append("\n")
    else:
        text.append("  Per-scene data not yet available\n", style="dim")
        text.append(
            "  (scene_upper_bounds will appear after first SDDP iteration)\n",
            style="dim",
        )

    return Panel(
        text,
        title="[bold]Convergence[/bold]",
        border_style="dim cyan",
        padding=(0, 1),
    )


# ---------------------------------------------------------------------------
# Main display class
# ---------------------------------------------------------------------------


class SolverDisplay:
    """Rich-based live dashboard for gtopt solver monitoring.

    Start the display in a background thread with :meth:`start`.  Feed
    solver output lines with :meth:`add_log_line`.  Stop with
    :meth:`stop` (blocks until the render thread exits).

    The display thread polls the solver status JSON file and rebuilds
    the Rich layout each cycle.  It also reads single-key commands
    from stdin (in cbreak mode) for interactive control.
    """

    def __init__(
        self,
        case_name: str,
        case_dir: Path,
        poll_interval: float = _POLL_INTERVAL,
        solver_hint: str = "",
    ) -> None:
        self.case_name = case_name
        self.poll_interval = poll_interval
        self._status_file: Path | None = None
        self._case_dir = case_dir
        self._solver_hint = solver_hint

        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._log_lines: deque[str] = deque(maxlen=_MAX_LOG_LINES)
        self._status: dict[str, Any] = {}
        self._start_time = time.monotonic()
        self._lock = threading.Lock()
        self._phase_tracker = SolverPhaseTracker()
        self._grid_tracker = SDDPGridTracker()

        # Interactive command state
        self._show_help = False
        self._show_stats = False
        self._display_mode = _MODE_DASHBOARD
        self._stop_sent = False
        self._system_stats: dict[str, Any] = {}
        self.quit_requested = threading.Event()

        # Grid-mode scroll state (vim-style j/k/J/K/g/G).  Tracks the
        # offset into the scene list rendered below the pinned
        # aggregate row.  Reset to 0 whenever the scene set changes
        # so a freshly-discovered scene scrolls back into view.
        self._grid_scroll_offset: int = 0
        self._grid_user_scrolled: bool = False
        self._grid_last_scene_count: int = 0

    # -- public API --------------------------------------------------------

    # Patterns to detect solver and method from gtopt log output
    _SOLVER_RE = re.compile(r"Solver:\s+(\S+)")
    _SDDP_RE = re.compile(r"SDDP[: ]")
    _MONO_RE = re.compile(r"Monolithic(?:Method|Solver):")

    def add_log_line(self, line: str) -> None:
        """Append a solver output line (thread-safe)."""
        stripped = line.rstrip()
        with self._lock:
            self._log_lines.append(stripped)
            self._phase_tracker.process_line(stripped)
            self._grid_tracker.process_line(stripped)
            # Detect solver from log output (e.g. "  Solver: cplex/22.1.1")
            if not self._system_stats.get("solver"):
                m = self._SOLVER_RE.search(stripped)
                if m:
                    self._system_stats["solver"] = m.group(1)
            # Detect method from log output
            if not self._system_stats.get("method"):
                if self._SDDP_RE.search(stripped):
                    self._system_stats["method"] = "sddp"
                elif self._MONO_RE.search(stripped):
                    self._system_stats["method"] = "monolithic"

    def start(self) -> None:
        """Launch the dashboard render thread."""
        self._start_time = time.monotonic()
        # Initialize system stats from planning JSON so the method is
        # displayed correctly from the very first frame.
        json_path = _find_planning_json(self._case_dir)
        self._system_stats = _load_system_stats(json_path)
        if self._solver_hint:
            self._system_stats["solver"] = self._solver_hint
        self._thread = threading.Thread(
            target=self._run_loop, daemon=True, name="gtopt-tui"
        )
        self._thread.start()

    def _sync_stats_from_status(self) -> None:
        """Propagate solver/method and phase grid from the status JSON."""
        status = self._status
        if not status:
            return
        with self._lock:
            if not self._system_stats.get("solver"):
                solver = status.get("solver", "")
                if solver:
                    self._system_stats["solver"] = solver
            if not self._system_stats.get("method"):
                method = status.get("method", "")
                if method:
                    self._system_stats["method"] = method
            # Merge phase grid from status JSON (enables remote monitoring)
            if "phase_grid" in status:
                self._grid_tracker.load_from_status(status)

    def stop(self) -> None:
        """Signal the render thread to finish and wait for it."""
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)

    def print_final(self, return_code: int) -> None:
        """Print a static completion banner after the live display ends."""
        from rich.console import Console  # noqa: PLC0415
        from rich.panel import Panel as RPanel  # noqa: PLC0415
        from rich.text import Text  # noqa: PLC0415

        console = Console()
        elapsed = time.monotonic() - self._start_time

        if return_code == 0:
            status = self._status
            converged = status.get("converged", False)
            iteration = status.get("iteration", "?")
            gap = status.get("gap")

            parts = [f"  Completed in {_format_elapsed(elapsed)}"]
            if converged:
                parts.append(f"  Converged at iteration {iteration}")
            if gap is not None:
                parts.append(f"  Final gap: {gap:.6f}")
            text = Text("\n".join(parts), style="bold green")
            panel = RPanel(
                text,
                title="[bold green]✓ gtopt — Success[/bold green]",
                border_style="green",
                padding=(0, 1),
            )
        else:
            msg = f"  Solver exited with code {return_code}\n"
            if self._stop_sent:
                msg += "  (graceful stop was requested — cuts should be saved)\n"
            msg += f"  Elapsed: {_format_elapsed(elapsed)}"
            style = "bold yellow" if self._stop_sent else "bold red"
            title_label = "Stopped" if self._stop_sent else "Failed"
            title_style = "yellow" if self._stop_sent else "red"
            text = Text(msg, style=style)
            panel = RPanel(
                text,
                title=f"[bold {title_style}]{'⏸' if self._stop_sent else '✗'}"
                f" gtopt — {title_label}[/bold {title_style}]",
                border_style=title_style,
                padding=(0, 1),
            )
        console.print(panel)

    # -- command handlers --------------------------------------------------

    def _handle_key(self, key: str) -> None:
        """Process a single-key command."""
        if key in ("s", "S"):
            self._cmd_stop()
        elif key in ("i", "I"):
            self._show_stats = not self._show_stats
            self._show_help = False
            # Lazy-load stats from planning JSON on first toggle
            if self._show_stats and not self._system_stats.get("elements"):
                json_path = _find_planning_json(self._case_dir)
                file_stats = _load_system_stats(json_path)
                # Merge file stats under log-detected values (log wins)
                for k, v in file_stats.items():
                    if k not in self._system_stats or not self._system_stats[k]:
                        self._system_stats[k] = v
        elif key in ("\t",):  # Tab — cycle display mode
            self._display_mode = (self._display_mode + 1) % _MODE_COUNT
        elif key == "1":
            self._display_mode = _MODE_DASHBOARD
        elif key == "2":
            self._display_mode = _MODE_GRID
        elif key == "3":
            self._display_mode = _MODE_CONVERGENCE
        elif key in ("h", "H", "?"):
            self._show_help = not self._show_help
            self._show_stats = False
        elif key in ("q", "Q"):
            self.quit_requested.set()
        elif self._display_mode == _MODE_GRID and key in (
            "j",
            "k",
            "J",
            "K",
            "g",
            "G",
        ):
            self._scroll_grid(key)

    def _scroll_grid(self, key: str) -> None:
        """Adjust the per-scene scroll offset in Grid mode (vim keys).

        Page sizes are intentionally fixed (1 line / 5 lines) rather
        than read from the live console height — the render path
        clamps the offset to the visible window anyway, and using a
        fixed step keeps the user's mental model stable across
        terminal resizes.  ``g``/``G`` jump to top/bottom; both also
        latch ``_grid_user_scrolled`` so the auto-follow logic in
        :func:`_build_sddp_grid` stops yanking the viewport away from
        what the user explicitly asked for.
        """
        n_scenes = len(self._grid_tracker.scenes)
        if n_scenes == 0:
            return
        self._grid_user_scrolled = True
        if key == "j":
            self._grid_scroll_offset += 1
        elif key == "k":
            self._grid_scroll_offset -= 1
        elif key == "J":
            self._grid_scroll_offset += 5
        elif key == "K":
            self._grid_scroll_offset -= 5
        elif key == "g":
            self._grid_scroll_offset = 0
            self._grid_user_scrolled = False  # re-enable auto-follow
        elif key == "G":
            self._grid_scroll_offset = max(0, n_scenes - 1)
        # Clamp; final clamping against the dynamic visible-window
        # size happens in the renderer.
        self._grid_scroll_offset = max(
            0, min(self._grid_scroll_offset, max(0, n_scenes - 1))
        )

    def _grid_visible_window(self) -> tuple[int | None, int | None]:
        """Return ``(max_visible_scenes, active_scene_hint)`` for the
        Grid panel renderer.

        The visible scene count is derived from the live console height
        minus a fixed budget for the surrounding panels (header, plan,
        progress, command bar, log) plus the legend, the pinned
        aggregate row, and the per-scene phase header.  Scenes that
        wouldn't fit are scrolled off-screen and surfaced as
        "▲ N above / ▼ N below" ribbons.

        Auto-follow: if the user hasn't manually scrolled (i.e. used
        j/k/J/K/G — but ``g`` resets the latch) and the active scene
        falls outside the current window, the offset is yanked so it
        stays visible.  This matches what an SDDP operator wants —
        watch the scene currently being solved without having to keep
        pressing j.
        """
        from rich.console import Console  # noqa: PLC0415

        scenes = self._grid_tracker.scenes
        n = len(scenes)
        if n == 0:
            return None, None

        # Console height — fall back to a sane default if Rich can't
        # detect the terminal (e.g. piped stdout).
        try:
            height = Console().size.height
        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            height = 40
        # Per-scene block height = scene header (1) + phase axis (1)
        # + iter rows (_GRID_MAX_ITERS).  Aggregate row + legend +
        # other panels eat ~20 lines of fixed overhead.
        per_block = _GRID_MAX_ITERS + 2
        budget = max(1, (height - 22) // per_block)

        # Reset scroll if scene list shrank below the offset.
        if self._grid_scroll_offset >= n:
            self._grid_scroll_offset = 0

        # Auto-follow active scene when the user hasn't scrolled.
        active = self._grid_tracker.active_cell
        active_scene = active[0] if active is not None else None
        if (
            not self._grid_user_scrolled
            and active_scene is not None
            and active_scene in scenes
        ):
            idx = scenes.index(active_scene)
            if idx < self._grid_scroll_offset:
                self._grid_scroll_offset = idx
            elif idx >= self._grid_scroll_offset + budget:
                self._grid_scroll_offset = max(0, idx - budget + 1)

        # Reset latch if scene set changed (new scenes appeared).
        if n != self._grid_last_scene_count:
            self._grid_last_scene_count = n
        return budget, active_scene

    def _cmd_stop(self) -> None:
        """Create the stop-request file for a graceful solver stop.

        The stop file must be written into whichever directory the
        solver is reading — that is, ``options.output_directory`` from
        the planning JSON (canonical "results/" for cascade cases,
        "output/" for the older convention).  Falling back to
        "<case>/output/" silently broke graceful stop on every cascade
        run.  We mirror :func:`_find_status_file`'s discovery logic so
        stop and status always agree.
        """
        if self._stop_sent:
            return
        # Already-discovered status file is the highest-confidence
        # signal of where the solver writes — drop the stop request
        # next to it.  Falling back to the same candidate-directory
        # search keeps the behavior consistent for early stops issued
        # before the first status JSON appears.
        if self._status_file is not None:
            stop_file = self._status_file.with_name(_STOP_REQUEST_FILE)
        else:
            stop_file = _find_status_file(self._case_dir).with_name(_STOP_REQUEST_FILE)
        try:
            stop_file.parent.mkdir(parents=True, exist_ok=True)
            payload = {
                "requested_at": time.time(),
                "source": "run_gtopt_tui",
            }
            stop_file.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
            self._stop_sent = True
            with self._lock:
                self._log_lines.append(
                    ">>> Graceful stop requested — solver will finish "
                    "current iteration and save cuts"
                )
            log.info("created stop request: %s", stop_file)
        except OSError as exc:
            log.warning("failed to create stop request file: %s", exc)

    # -- internals ---------------------------------------------------------

    def _build_display(self) -> Group:
        from rich.console import Group  # noqa: PLC0415

        data = self._status
        elapsed = time.monotonic() - self._start_time
        mode = self._display_mode

        with self._lock:
            log_lines = list(self._log_lines)
            # Enrich optimize detail from status JSON (more current than logs)
            opt_st = self._phase_tracker.states["optimize"]
            if opt_st.status == "active" and data:
                if data.get("max_iterations"):
                    opt_st.detail = (
                        f"iter {data.get('iteration', 0)}/{data['max_iterations']}"
                    )
                elif data.get("total_scenes") is not None:
                    opt_st.detail = (
                        f"scene {data.get('scenes_done', 0)}/{data['total_scenes']}"
                    )
            self._phase_tracker.tick()

        panels: list[Any] = [
            _build_header(self.case_name, data, elapsed, self._system_stats)
        ]
        panels.append(_build_plan_panel(self._phase_tracker))

        has_sddp = bool(data.get("max_iterations"))
        has_monolithic = data.get("total_scenes") is not None

        if mode == _MODE_DASHBOARD:
            # Full dashboard: stats, progress, history, system, log
            if has_sddp:
                panels.append(_build_progress(data))
                panels.append(_build_stats(data))
                async_panel = _build_async_panel(data)
                if async_panel is not None:
                    panels.append(async_panel)
                if data.get("history"):
                    panels.append(_build_history(data))
                if _has_resource_telemetry(data):
                    panels.append(_build_system(data))
            elif has_monolithic:
                panels.append(_build_progress(data))
                if _has_resource_telemetry(data):
                    panels.append(_build_system(data))
            panels.append(_build_log(log_lines))

        elif mode == _MODE_GRID:
            # SDDP phase activity grid
            if has_sddp:
                panels.append(_build_progress(data))
            if self._grid_tracker.has_data:
                visible_budget, active_hint = self._grid_visible_window()
                panels.append(
                    _build_sddp_grid(
                        self._grid_tracker,
                        scroll_offset=self._grid_scroll_offset,
                        max_visible_scenes=visible_budget,
                        active_scene_hint=active_hint,
                    )
                )
            panels.append(_build_log(log_lines))

        elif mode == _MODE_CONVERGENCE:
            # Per-scene convergence heatmap + sparklines
            if has_sddp:
                panels.append(_build_progress(data))
            panels.append(_build_convergence(data))

        # Overlays (available in all modes)
        if self._show_help:
            panels.append(_build_help_overlay())
        elif self._show_stats:
            panels.append(_build_stats_overlay(self._system_stats))

        panels.append(_build_command_bar(self._stop_sent, mode))
        return Group(*panels)

    def _run_loop(self) -> None:
        from rich.console import Console  # noqa: PLC0415
        from rich.live import Live  # noqa: PLC0415

        console = Console()
        old_settings = _enter_cbreak()
        cbreak_active = old_settings is not None
        try:
            with Live(
                self._build_display(),
                console=console,
                refresh_per_second=_REFRESH_PER_SECOND,
                transient=True,
            ) as live:
                # Decouple keyboard polling from status-file polling.
                # Status reads are expensive (full JSON parse + JSON
                # file may be tens of KB) so we keep their cadence at
                # ``poll_interval`` (default 1 s).  But waking only
                # once per poll_interval makes 'q', 's' and the mode
                # keys feel laggy — at 1 s the user releases the key
                # before we even see it.  Run an inner loop at ~50 ms
                # for keys, and only refresh the status when the
                # accumulated time crosses ``poll_interval``.
                key_interval = 0.05
                last_status_refresh = 0.0
                last_render = 0.0
                # Render at most ``_REFRESH_PER_SECOND`` Hz to avoid
                # flicker; key handlers force an immediate redraw so
                # mode switches still feel snappy.
                render_interval = 1.0 / max(1, _REFRESH_PER_SECOND)
                while not self._stop.wait(key_interval):
                    now = time.monotonic()
                    needs_render = False
                    # Poll keyboard for interactive commands
                    key = _poll_key(cbreak_active)
                    if key is not None:
                        self._handle_key(key)
                        needs_render = True
                    # Refresh status no faster than poll_interval
                    if now - last_status_refresh >= self.poll_interval:
                        if self._status_file is None or not self._status_file.is_file():
                            self._status_file = _find_status_file(self._case_dir)
                        self._status = _load_status(self._status_file)
                        self._sync_stats_from_status()
                        last_status_refresh = now
                        needs_render = True
                    if needs_render and (now - last_render) >= render_interval:
                        live.update(self._build_display())
                        last_render = now

                # One final render
                self._status = _load_status(self._status_file)
                self._sync_stats_from_status()
                with self._lock:
                    self._phase_tracker.finish_all()
                live.update(self._build_display())
        except Exception:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            # Never let the TUI thread crash the solver
            log.debug("TUI render error", exc_info=True)
        finally:
            _exit_cbreak(old_settings)
