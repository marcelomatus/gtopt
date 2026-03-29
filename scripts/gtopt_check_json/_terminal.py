# SPDX-License-Identifier: BSD-3-Clause
"""Shared terminal formatting helpers for gtopt Python tools.

Uses the ``rich`` library for terminal-ready tables, colours and
automatic fallback to ASCII when the output stream is not a TTY or
does not support UTF-8.

All scripts (``plp2gtopt``, ``gtopt_check_json``, ``igtopt``,
``pp2gtopt``, …) import from this module so the look-and-feel is
consistent everywhere.
"""

from __future__ import annotations

import logging
import sys
from typing import Literal, Sequence

from rich.console import Console
from rich.table import Table
from rich.theme import Theme

# Type alias for Rich column justification.
JustifyMethod = Literal["default", "left", "center", "right", "full"]

# ---------------------------------------------------------------------------
# Theme
# ---------------------------------------------------------------------------

_THEME = Theme(
    {
        "title": "bold cyan",
        "header": "bold",
        "key": "dim",
        "val": "",
        "ok": "bold green",
        "warn": "bold yellow",
        "err": "bold red",
        "note": "bold cyan",
        "dim": "dim",
    }
)


def get_console(
    *,
    stderr: bool = True,
    force_color: bool | None = None,
    force_ascii: bool | None = None,
) -> Console:
    """Return a :class:`rich.Console` configured for gtopt tools.

    Parameters
    ----------
    stderr
        Write to stderr (default) so tables never mix with stdout data.
    force_color
        Override colour auto-detection (``True`` = always colour,
        ``False`` = never, ``None`` = auto).
    force_ascii
        Override Unicode auto-detection (``True`` = ASCII-only,
        ``None`` = auto).
    """
    stream = sys.stderr if stderr else sys.stdout

    # Rich auto-detects colour when force_terminal is None.
    # Setting it explicitly overrides that.
    force_terminal: bool | None = None
    if force_color is True:
        force_terminal = True
    elif force_color is False:
        force_terminal = False

    no_color = force_color is False

    console = Console(
        file=stream,
        theme=_THEME,
        force_terminal=force_terminal,
        no_color=no_color,
        highlight=False,
        width=120,
    )

    return console


# Module-level console — lazily created on first use.
# Re-create via ``init()`` if you need different options.
_console: Console | None = None
_force_color: bool | None = None
_force_ascii: bool | None = None


def init(
    *,
    force_color: bool | None = None,
    force_ascii: bool | None = None,
) -> Console:
    """(Re-)initialise the module-level console.

    Call this once from your CLI ``main()`` before producing output.
    """
    global _console, _force_color, _force_ascii  # noqa: PLW0603
    _force_color = force_color
    _force_ascii = force_ascii
    _console = get_console(
        force_color=force_color,
        force_ascii=force_ascii,
    )
    return _console


def console() -> Console:
    """Return a console that writes to the *current* ``sys.stderr``.

    The console is recreated whenever ``sys.stderr`` has changed (e.g.
    during pytest ``capsys`` capturing) so output always goes to the
    right stream.
    """
    global _console  # noqa: PLW0603
    if _console is None or _console.file is not sys.stderr:
        _console = get_console(
            force_color=_force_color,
            force_ascii=_force_ascii,
        )
    return _console


# ---------------------------------------------------------------------------
# Convenience helpers
# ---------------------------------------------------------------------------


def print_section(title: str) -> None:
    """Print a styled section header."""
    con = console()
    con.print()
    con.print(f"  [title]━━ {title} ━━[/title]")
    con.print()


def print_kv_table(
    pairs: Sequence[tuple[str, str]],
    *,
    title: str = "",
) -> None:
    """Print a two-column key → value table.

    Parameters
    ----------
    pairs
        Sequence of ``(label, value)`` tuples.
    title
        Optional table title.
    """
    if not pairs:
        return
    table = Table(
        show_header=False,
        box=None,
        padding=(0, 2),
        title=f"[title]{title}[/title]" if title else None,
        title_justify="left",
        min_width=40,
    )
    table.add_column("Key", style="key", no_wrap=True)
    table.add_column("Value", style="val", justify="right")
    for key, val in pairs:
        table.add_row(key, val)
    console().print(table)


def print_table(
    headers: Sequence[str],
    rows: Sequence[Sequence[str]],
    *,
    aligns: Sequence[JustifyMethod] | None = None,
    title: str = "",
    styles: Sequence[str] | None = None,
) -> None:
    """Print a multi-column table.

    Parameters
    ----------
    headers
        Column header labels.
    rows
        Row data.
    aligns
        Per-column justification: ``"left"``, ``"right"``, ``"center"``.
    title
        Optional table title.
    styles
        Per-column Rich style strings (e.g. ``["bold", "", "green"]``).
    """
    from rich.box import ASCII, ROUNDED  # noqa: PLC0415

    con = console()
    box_style = ROUNDED if con.is_terminal else ASCII

    table = Table(
        title=f"[title]{title}[/title]" if title else None,
        title_justify="left",
        box=box_style,
        show_lines=False,
        padding=(0, 1),
    )
    ncols = len(headers)
    _aligns: Sequence[JustifyMethod] = (
        aligns if aligns is not None else (["left"] * ncols)
    )
    if styles is None:
        styles = [""] * ncols

    for i, hdr in enumerate(headers):
        table.add_column(
            hdr,
            justify=_aligns[i],
            style=styles[i] or None,
            no_wrap=True,
        )

    for row in rows:
        table.add_row(*(row[i] if i < len(row) else "" for i in range(ncols)))

    con.print(table)


def print_status(label: str, ok: bool, *, details: str = "") -> None:
    """Print a status line with ✓/✗ indicator."""
    icon = "[ok]✓[/ok]" if ok else "[err]✗[/err]"
    extra = f"  [dim]{details}[/dim]" if details else ""
    console().print(f"  {icon} {label}{extra}")


def print_finding(
    severity: str,
    check_id: str,
    message: str,
) -> None:
    """Print a single validation finding.

    Parameters
    ----------
    severity
        One of ``"CRITICAL"``, ``"WARNING"``, ``"NOTE"``.
    check_id
        The check identifier string.
    message
        Human-readable finding message.
    """
    sev_upper = severity.upper()
    if sev_upper == "CRITICAL":
        tag = "[err][CRITICAL][/err]"
    elif sev_upper == "WARNING":
        tag = "[warn][WARNING][/warn]"
    else:
        tag = "[note][NOTE][/note]"
    console().print(f"  {tag} ({check_id}) {message}")


def print_summary(
    critical: int,
    warnings: int,
    notes: int,
) -> None:
    """Print a findings summary line."""
    con = console()
    con.print()
    con.print(
        f"  Summary: [err]{critical}[/err] critical, "
        f"[warn]{warnings}[/warn] warnings, "
        f"[note]{notes}[/note] notes"
    )


def print_connectivity_table(islands: list) -> None:
    """Print a per-island table of disconnected buses.

    Each row shows one island with name lists per element category:
    buses, lines, generators, demands, batteries.

    Parameters
    ----------
    islands
        List of :class:`IslandInfo` objects from
        :func:`analyse_bus_islands`.
    """
    if not islands:
        return

    def _names(items: list, limit: int = 5) -> str:
        names = [str(x) for x in items]
        if not names:
            return ""
        text = ", ".join(names[:limit])
        if len(names) > limit:
            text += f" … (+{len(names) - limit})"
        return text

    rows: list[tuple[str, ...]] = []
    for isl in islands:
        rows.append(
            (
                str(isl.island_id),
                _names(isl.buses),
                _names(isl.line_names),
                _names(isl.generator_names),
                _names(isl.demand_names),
                _names(isl.battery_names),
            )
        )

    total_buses = sum(len(isl.buses) for isl in islands)
    print_table(
        headers=["Island", "Buses", "Lines", "Generators", "Demands", "Batteries"],
        rows=rows,
        aligns=["right", "left", "left", "left", "left", "left"],
        title=f"Disconnected Bus Islands ({len(islands)} islands, {total_buses} buses)",
    )


# ---------------------------------------------------------------------------
# format_info / format_indicators  (string-returning API)
# ---------------------------------------------------------------------------


def render_kv_table(
    pairs: Sequence[tuple[str, str]],
    *,
    title: str = "",
) -> str:
    """Return a key-value table as a plain string (no ANSI codes).

    Useful when the caller needs a string (e.g. for logging or tests)
    rather than direct terminal output.
    """
    buf_console = Console(
        file=None, force_terminal=False, no_color=True, highlight=False, width=120
    )
    if not pairs:
        return ""
    table = Table(
        show_header=False,
        box=None,
        padding=(0, 2),
        title=title or None,
        title_justify="left",
        min_width=40,
    )
    table.add_column("Key", no_wrap=True)
    table.add_column("Value", justify="right")
    for key, val in pairs:
        table.add_row(key, val)
    with buf_console.capture() as capture:
        buf_console.print(table)
    return capture.get().rstrip()


# ---------------------------------------------------------------------------
# Logging formatter
# ---------------------------------------------------------------------------


class CleanFormatter(logging.Formatter):
    """Formatter that uses minimal format for INFO, detailed for DEBUG.

    * ``DEBUG``: ``%(asctime)s %(levelname)s %(message)s``
    * ``INFO``:  ``%(message)s``
    * ``WARNING+``: ``%(levelname)s: %(message)s``
    """

    def __init__(self) -> None:
        super().__init__()
        self._formatters = {
            logging.DEBUG: logging.Formatter(
                "%(asctime)s %(levelname)s %(message)s",
            ),
            logging.INFO: logging.Formatter("%(message)s"),
            logging.WARNING: logging.Formatter("%(levelname)s: %(message)s"),
            logging.ERROR: logging.Formatter("%(levelname)s: %(message)s"),
            logging.CRITICAL: logging.Formatter("%(levelname)s: %(message)s"),
        }
        self._default = logging.Formatter("%(message)s")

    def format(self, record: logging.LogRecord) -> str:
        formatter = self._formatters.get(record.levelno, self._default)
        return formatter.format(record)
