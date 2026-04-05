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

_POLL_INTERVAL = 0.5  # seconds between status file polls
_MAX_LOG_LINES = 16  # rolling log buffer size
_MAX_HISTORY_ROWS = 8  # iteration rows shown in the table
_REFRESH_PER_SECOND = 4  # Rich Live refresh rate
_SPARKLINE_WIDTH = 24
_PROGRESS_BAR_WIDTH = 40

# Pass names from the C++ SDDPStatusSnapshot::current_pass enum
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
_SDDP_LOG_RE = re.compile(r"SDDP (\w+) \[i(\d+) s(\d+) p(\d+)(?:\s+a(\d+))?\]")

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
        has_aperture = m.group(5) is not None

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

    @property
    def active_cell(self) -> tuple[int, int, int] | None:
        return self._active_cell

    @property
    def kappa_warnings(self) -> int:
        return self._kappa_warnings

    @property
    def elastic_count(self) -> int:
        return self._elastic_count

    @property
    def infeasible_count(self) -> int:
        return self._infeasible_count

    @property
    def aperture_count(self) -> int:
        return self._aperture_count


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
    (re.compile(r"SDDP: === iteration (\d+) / (\d+)"), "optimize", "detail"),
    (
        re.compile(r"Monolithic(?:Method|Solver): scene (\d+) done.*\((\d+)/(\d+)\)"),
        "optimize",
        "detail",
    ),
    (re.compile(r"SDDP: === simulation pass"), "sim_pass", "start"),
    (re.compile(r"SDDP: simulation pass done"), "sim_pass", "done"),
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

    The C++ solver writes ``sddp_status.json`` (SDDP method) or
    ``monolithic_status.json`` (monolithic method) under the output
    directory — which defaults to ``<case>/output/``.
    """
    base = case_dir.parent if case_dir.is_file() else case_dir
    output_dir = base / "output"
    # Prefer SDDP (more detailed), fall back to monolithic
    sddp = output_dir / "sddp_status.json"
    mono = output_dir / "monolithic_status.json"
    return sddp if sddp.is_file() else mono if mono.is_file() else sddp


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
    grid.add_row(
        "Gap",
        Text(f"{gap:.6f}", style=gap_style),
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
    table.add_column("Cuts", justify="right", width=5)
    table.add_column("Fwd", justify="right", width=7)
    table.add_column("Bwd", justify="right", width=7)

    for rec in history[-_MAX_HISTORY_ROWS:]:
        g = rec.get("gap", 0.0)
        gap_style = "green" if g < 0.01 else "yellow" if g < 0.1 else "red"
        table.add_row(
            str(rec.get("iteration", 0)),
            _format_number(rec.get("lower_bound", 0.0)),
            _format_number(rec.get("upper_bound", 0.0)),
            f"[{gap_style}]{g:.6f}[/{gap_style}]",
            str(rec.get("cuts_added", 0)),
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


def _build_system(data: dict) -> Panel:
    from rich.panel import Panel  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    rt = data.get("realtime", {})
    cpus = rt.get("cpu_loads", [])
    workers = rt.get("active_workers", [])

    cpu_val = cpus[-1] if cpus else 0.0
    worker_val = workers[-1] if workers else 0
    filled = int(cpu_val * 30 / 100)
    cpu_bar = f"[green]{'█' * filled}[/green][dim]{'░' * (30 - filled)}[/dim]"

    grid = Table.grid(padding=(0, 2))
    grid.add_column(min_width=6)
    grid.add_column(min_width=40)
    grid.add_column(min_width=14)
    grid.add_row(
        Text("CPU", style="bold"),
        Text.from_markup(f"{cpu_bar} {cpu_val:.0f}%"),
        Text(f"Workers: {worker_val}", style="cyan"),
    )

    cpu_spark = _sparkline(cpus) if cpus else ""
    if cpu_spark:
        grid.add_row(
            Text("Load", style="dim"),
            Text(cpu_spark, style="green"),
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


def _build_command_bar(stop_sent: bool) -> Panel:
    """Render the interactive command bar at the bottom of the display."""
    from rich.panel import Panel  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    text = Text()
    text.append("  ")
    text.append("[s]", style="bold cyan")
    if stop_sent:
        text.append(" stop sent", style="dim")
    else:
        text.append(" stop", style="")
    text.append("    ")
    text.append("[g]", style="bold cyan")
    text.append(" grid", style="")
    text.append("    ")
    text.append("[i]", style="bold cyan")
    text.append(" stats", style="")
    text.append("    ")
    text.append("[h]", style="bold cyan")
    text.append(" help", style="")
    text.append("    ")
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
        ("s", "Graceful stop — finish current iteration, save cuts, exit"),
        ("g", "Toggle SDDP phase grid (forward/backward/elastic/aperture)"),
        ("i", "Toggle system stats overlay (buses, generators, etc.)"),
        ("h", "Toggle this help overlay"),
        ("q", "Quit — terminate the solver immediately"),
        ("Ctrl+C", "Same as quit"),
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


def _build_sddp_grid(tracker: SDDPGridTracker) -> Panel:
    """Render the SDDP phase/iteration activity grid.

    One matrix per scene.  X-axis = phases, Y-axis = iterations (most recent).
    Each cell is a colored character showing the activity type.
    """
    from rich.panel import Panel  # noqa: PLC0415
    from rich.text import Text  # noqa: PLC0415

    text = Text()

    max_ph = min(tracker.max_phase, _GRID_MAX_PHASES)
    scenes = tracker.scenes

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

    # Indicator lights for diagnostics
    indicators: list[tuple[str, str, str]] = []
    if tracker.kappa_warnings > 0:
        indicators.append(("kappa", str(tracker.kappa_warnings), "bold red"))
    if tracker.elastic_count > 0:
        indicators.append(("elastic", str(tracker.elastic_count), "bold yellow"))
    if tracker.aperture_count > 0:
        indicators.append(("aperture", str(tracker.aperture_count), "bold blue"))
    if tracker.infeasible_count > 0:
        indicators.append(("infeas", str(tracker.infeasible_count), "bold red"))
    if indicators:
        text.append("   ")
        for label, count, ind_style in indicators:
            text.append(f" {label}:", style="dim")
            text.append(count, style=ind_style)
    text.append("\n")

    for sc in scenes:
        # Scene header (compact — only show if multiple scenes)
        if len(scenes) > 1:
            text.append(f"  s{sc}", style="bold")
            text.append("\n")

        # Phase header row
        text.append("     ")  # indent for iter label
        for ph in range(max_ph + 1):
            if ph % 5 == 0:
                label = str(ph)
                text.append(label, style="dim")
                # Pad remaining chars of this label
                text.append(" " * max(0, 1 - len(label)))
            else:
                text.append(" ", style="dim")
        text.append("\n")

        # Iteration rows (most recent _GRID_MAX_ITERS)
        first_iter = max(0, tracker.max_iter - _GRID_MAX_ITERS + 1)
        for it in range(first_iter, tracker.max_iter + 1):
            text.append(f"  {it:>2} ", style="dim")
            for ph in range(max_ph + 1):
                state = tracker.get_cell(sc, it, ph)
                char, style = _GRID_CELL_STYLES[state]
                # Highlight current active cell with a blink-like effect
                if (
                    tracker.active_cell is not None
                    and tracker.active_cell == (sc, it, ph)
                    and state != _GRID_IDLE
                ):
                    text.append(char, style=f"{style} reverse")
                else:
                    text.append(char, style=style)
            text.append("\n")

    return Panel(
        text,
        title="[bold]SDDP Phase Grid[/bold]",
        border_style="dim cyan",
        padding=(0, 0),
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
        self._show_grid = False
        self._stop_sent = False
        self._system_stats: dict[str, Any] = {}
        self.quit_requested = threading.Event()

    # -- public API --------------------------------------------------------

    # Patterns to detect solver and method from gtopt log output
    _SOLVER_RE = re.compile(r"Solver:\s+(\S+)")
    _SDDP_RE = re.compile(r"SDDP:")
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
        # Initialize system stats (populated from log output and CLI flags)
        self._system_stats = {}
        if self._solver_hint:
            self._system_stats["solver"] = self._solver_hint
        self._thread = threading.Thread(
            target=self._run_loop, daemon=True, name="gtopt-tui"
        )
        self._thread.start()

    def _sync_stats_from_status(self) -> None:
        """Propagate solver/method from the status JSON into _system_stats."""
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
        elif key in ("g", "G"):
            self._show_grid = not self._show_grid
        elif key in ("h", "H", "?"):
            self._show_help = not self._show_help
            self._show_stats = False
        elif key in ("q", "Q"):
            self.quit_requested.set()

    def _cmd_stop(self) -> None:
        """Create the stop-request file for a graceful solver stop."""
        if self._stop_sent:
            return
        base = self._case_dir.parent if self._case_dir.is_file() else self._case_dir
        stop_file = base / "output" / _STOP_REQUEST_FILE
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

        if has_sddp:
            panels.append(_build_progress(data))
            panels.append(_build_stats(data))
            if data.get("history"):
                panels.append(_build_history(data))
            rt = data.get("realtime", {})
            if rt.get("cpu_loads") or rt.get("active_workers"):
                panels.append(_build_system(data))
        elif has_monolithic:
            panels.append(_build_progress(data))
            rt = data.get("realtime", {})
            if rt.get("cpu_loads") or rt.get("active_workers"):
                panels.append(_build_system(data))

        # SDDP phase grid (auto-show for SDDP, toggle with 'g')
        if self._show_grid and self._grid_tracker.has_data:
            panels.append(_build_sddp_grid(self._grid_tracker))

        # Overlays
        if self._show_help:
            panels.append(_build_help_overlay())
        elif self._show_stats:
            panels.append(_build_stats_overlay(self._system_stats))

        panels.append(_build_log(log_lines))
        panels.append(_build_command_bar(self._stop_sent))
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
                while not self._stop.wait(self.poll_interval):
                    # Poll keyboard for interactive commands
                    key = _poll_key(cbreak_active)
                    if key is not None:
                        self._handle_key(key)
                    # Lazily discover the status file once it appears
                    if self._status_file is None or not self._status_file.is_file():
                        self._status_file = _find_status_file(self._case_dir)
                    self._status = _load_status(self._status_file)
                    self._sync_stats_from_status()
                    live.update(self._build_display())

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
