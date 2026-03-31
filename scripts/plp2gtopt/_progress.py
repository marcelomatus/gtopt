"""Live progress display for plp2gtopt conversion.

Shows a Claude-Code-style checklist with:
- Pending steps shown as dim circles
- Active step with animated braille spinner and elapsed time
- Completed steps with green checkmarks and elapsed time

Falls back to simple text output when stderr is not a terminal.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from rich.text import Text

logger = logging.getLogger(__name__)

# Braille spinner frames (same cadence as Claude Code)
_SPINNER = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]

# Ordered conversion steps: (key, human label)
CONVERSION_STEPS: list[tuple[str, str]] = [
    ("parse", "Parsing PLP input files"),
    ("options", "Processing options"),
    ("stages", "Processing stages and blocks"),
    ("scenarios", "Processing scenarios"),
    ("buses", "Processing buses and lines"),
    ("generators", "Processing generators"),
    ("demands", "Processing demands"),
    ("hydro", "Processing hydro system"),
    ("batteries", "Processing batteries"),
    ("boundary", "Processing boundary cuts"),
    ("write", "Writing output files"),
    ("validate", "Running validation checks"),
]


@dataclass
class _StepState:
    label: str
    status: str = "pending"  # pending | active | done | skipped
    t_start: float = 0.0
    t_end: float = 0.0

    @property
    def elapsed(self) -> float:
        if self.status == "active":
            return time.monotonic() - self.t_start
        if self.status == "done":
            return self.t_end - self.t_start
        return 0.0


@dataclass
class ConversionProgress:
    """Live progress tracker for PLP-to-gtopt conversion.

    Usage::

        with ConversionProgress() as progress:
            progress.step("parse")
            do_parsing()
            progress.step("options")
            do_options()
            ...
            progress.done()
    """

    steps: list[tuple[str, str]] = field(default_factory=lambda: list(CONVERSION_STEPS))
    _states: dict[str, _StepState] = field(default_factory=dict, init=False)
    _order: list[str] = field(default_factory=list, init=False)
    _live: Any = field(default=None, init=False)
    _is_terminal: bool = field(default=False, init=False)
    _t0: float = field(default=0.0, init=False)
    _frame: int = field(default=0, init=False)
    _current_key: str | None = field(default=None, init=False)
    _console: Any = field(default=None, init=False)

    def __post_init__(self) -> None:
        for key, label in self.steps:
            self._states[key] = _StepState(label=label)
            self._order.append(key)

    def __enter__(self) -> ConversionProgress:
        self._t0 = time.monotonic()
        try:
            from gtopt_check_json._terminal import console  # noqa: PLC0415

            con = console()
            self._console = con
            self._is_terminal = con.is_terminal
        except ImportError:
            self._is_terminal = False

        if self._is_terminal:
            from rich.live import Live  # noqa: PLC0415

            self._live = Live(
                self._render(),
                console=self._console,
                refresh_per_second=8,
                transient=True,
            )
            self._live.__enter__()
        return self

    def __exit__(self, *exc_info: object) -> None:
        if self._live is not None:
            self._live.__exit__(*exc_info)
            self._live = None
        # Print final static summary (unless there was an exception)
        if exc_info[0] is None and self._console is not None:
            self._console.print(self._render_final())

    def step(self, key: str) -> None:
        """Mark *key* as the active step, completing the previous one."""
        if self._current_key is not None:
            self._finish(self._current_key)
        st = self._states.get(key)
        if st is None:
            return
        st.status = "active"
        st.t_start = time.monotonic()
        self._current_key = key
        self._refresh()
        if not self._is_terminal:
            self._print_plain(key)

    def done(self) -> None:
        """Mark the last active step as done."""
        if self._current_key is not None:
            self._finish(self._current_key)
            self._current_key = None
            self._refresh()

    def skip(self, key: str) -> None:
        """Mark a step as skipped (won't show in output)."""
        st = self._states.get(key)
        if st is not None:
            st.status = "skipped"

    def _finish(self, key: str) -> None:
        st = self._states.get(key)
        if st is not None and st.status == "active":
            st.status = "done"
            st.t_end = time.monotonic()

    def _refresh(self) -> None:
        if self._live is not None:
            self._frame += 1
            self._live.update(self._render())

    def _render(self) -> Text:
        """Build the Rich renderable for the live display."""
        from rich.text import Text  # noqa: PLC0415

        lines = Text()
        for key in self._order:
            st = self._states[key]
            if st.status == "skipped":
                continue
            if st.status == "done":
                lines.append("  ")
                lines.append("✓", style="bold green")
                lines.append(f" {st.label}")
                lines.append(f"  ({st.elapsed:.1f}s)", style="dim")
                lines.append("\n")
            elif st.status == "active":
                spinner = _SPINNER[self._frame % len(_SPINNER)]
                lines.append("  ")
                lines.append(spinner, style="bold cyan")
                lines.append(f" {st.label}...")
                lines.append(f"  ({st.elapsed:.1f}s)", style="dim")
                lines.append("\n")
            else:
                lines.append("  ")
                lines.append("○", style="dim")
                lines.append(f" {st.label}", style="dim")
                lines.append("\n")

        # Bottom status line
        total = time.monotonic() - self._t0
        spinner = _SPINNER[self._frame % len(_SPINNER)]
        lines.append("\n  ")
        lines.append(spinner, style="bold cyan")
        lines.append(f" Converting...  ({total:.1f}s)", style="dim")
        return lines

    def _render_final(self) -> Text:
        """Build the final static summary after completion."""
        from rich.text import Text  # noqa: PLC0415

        lines = Text()
        for key in self._order:
            st = self._states[key]
            if st.status == "skipped":
                continue
            lines.append("  ")
            if st.status == "done":
                lines.append("✓", style="bold green")
                lines.append(f" {st.label}")
                lines.append(f"  ({st.elapsed:.1f}s)", style="dim")
            else:
                lines.append("○", style="dim")
                lines.append(f" {st.label}", style="dim")
            lines.append("\n")
        return lines

    def _print_plain(self, key: str) -> None:
        """Print a plain-text step line for non-terminal output."""
        import sys  # noqa: PLC0415

        st = self._states[key]
        print(f"  * {st.label}...", file=sys.stderr, flush=True)


def setup_file_logging(
    output_dir: Path, log_file: str | Path | None = None
) -> logging.Handler | None:
    """Add a file handler writing DEBUG-level logs.

    Parameters
    ----------
    output_dir
        Default directory: logs are written to ``<output_dir>/logs/conversion.log``.
    log_file
        Explicit log file path.  When given, it is used as-is and the file
        is opened in **append** mode so successive runs accumulate.

    The root logger level is lowered to DEBUG so that all messages reach
    the file handler (the console handler's own level filter is unaffected).

    Returns the handler so the caller can remove it later, or ``None``
    if the log directory cannot be created.
    """
    if log_file is not None:
        log_path = Path(log_file)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        mode = "a"
    else:
        log_dir = output_dir / "logs"
        try:
            log_dir.mkdir(parents=True, exist_ok=True)
        except OSError:
            logger.debug("Could not create log directory: %s", log_dir)
            return None
        log_path = log_dir / "conversion.log"
        mode = "w"

    handler = logging.FileHandler(log_path, mode=mode, encoding="utf-8")
    handler.setLevel(logging.DEBUG)
    handler.setFormatter(
        logging.Formatter("%(asctime)s %(levelname)-7s [%(name)s] %(message)s")
    )
    root = logging.getLogger()
    # Ensure DEBUG messages reach the file handler.  The console handler
    # already filters via CleanFormatter / its own level, so this only
    # affects file output.
    if root.level > logging.DEBUG:
        # Set console handlers to preserve their current effective level
        for h in root.handlers:
            if h.level == logging.NOTSET:
                h.setLevel(root.level)
        root.setLevel(logging.DEBUG)
    root.addHandler(handler)
    logger.debug("Logging to %s", log_file)
    return handler
