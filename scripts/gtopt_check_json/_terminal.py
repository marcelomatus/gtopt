# SPDX-License-Identifier: BSD-3-Clause
"""Shared terminal formatting helpers for gtopt Python tools.

Provides box-drawing table primitives and section-header helpers that
produce "terminal-ready" output with optional ANSI colour support.

When the output stream does not support UTF-8 or is not a TTY, the
module falls back to ASCII-safe table characters (``+``, ``-``, ``|``)
so tables render correctly in log files and dumb terminals.
"""

from __future__ import annotations

import logging
import os
import re
import sys
from typing import Sequence

# ---------------------------------------------------------------------------
# ANSI colour codes
# ---------------------------------------------------------------------------
BOLD = "\033[1m"
DIM = "\033[2m"
RED = "\033[91m"
GREEN = "\033[92m"
YELLOW = "\033[93m"
CYAN = "\033[96m"
MAGENTA = "\033[95m"
WHITE = "\033[97m"
RESET = "\033[0m"


# ---------------------------------------------------------------------------
# Capability detection
# ---------------------------------------------------------------------------


def _is_tty(stream: object = None) -> bool:
    """Return True when *stream* (default ``sys.stderr``) is a TTY."""
    s = stream or sys.stderr
    return hasattr(s, "isatty") and s.isatty()


def _supports_utf8(stream: object = None) -> bool:
    """Return True when *stream* likely supports UTF-8 encoding."""
    s = stream or sys.stderr
    enc = getattr(s, "encoding", "") or ""
    return "utf" in enc.lower()


def detect_fancy() -> bool:
    """Return True if stderr is an interactive UTF-8 terminal.

    When this returns False, table helpers automatically fall back to
    ASCII characters (``+``, ``-``, ``|``) so the output is safe for
    log files, CI, and non-UTF-8 terminals.
    """
    if os.environ.get("NO_COLOR"):
        return False
    return _is_tty() and _supports_utf8()


# Module-level flags — callers may override before producing output.
USE_COLOR: bool = detect_fancy()
USE_UNICODE: bool = detect_fancy()


# ---------------------------------------------------------------------------
# Colour helpers
# ---------------------------------------------------------------------------


def cc(code: str, text: str, enabled: bool | None = None) -> str:
    """Wrap *text* in ANSI *code* when colour is enabled."""
    on = enabled if enabled is not None else USE_COLOR
    return f"{code}{text}{RESET}" if on else str(text)


def vis_len(text: str) -> int:
    """Visible length of *text*, excluding ANSI escape sequences."""
    return len(re.sub(r"\033\[[0-9;]*m", "", text))


# ---------------------------------------------------------------------------
# Box-drawing character sets
# ---------------------------------------------------------------------------

# Unicode box-drawing
_U_TL, _U_TR, _U_BL, _U_BR = "┌", "┐", "└", "┘"
_U_HZ, _U_VT = "─", "│"
_U_LT, _U_RT, _U_TT, _U_BT, _U_CR = "├", "┤", "┬", "┴", "┼"
_U_HZ_BOLD = "━"
_U_CHECK, _U_CROSS = "✓", "✗"

# ASCII fallback
_A_TL, _A_TR, _A_BL, _A_BR = "+", "+", "+", "+"
_A_HZ, _A_VT = "-", "|"
_A_LT, _A_RT, _A_TT, _A_BT, _A_CR = "+", "+", "+", "+", "+"
_A_HZ_BOLD = "="
_A_CHECK, _A_CROSS = "ok", "FAIL"


def _ch() -> (  # noqa: PLR0911
    tuple[str, str, str, str, str, str, str, str, str, str, str, str, str, str]
):
    """Return (TL, TR, BL, BR, HZ, VT, LT, RT, TT, BT, CR, HZ_BOLD, CHECK, CROSS)."""
    if USE_UNICODE:
        return (
            _U_TL, _U_TR, _U_BL, _U_BR, _U_HZ, _U_VT,
            _U_LT, _U_RT, _U_TT, _U_BT, _U_CR,
            _U_HZ_BOLD, _U_CHECK, _U_CROSS,
        )
    return (
        _A_TL, _A_TR, _A_BL, _A_BR, _A_HZ, _A_VT,
        _A_LT, _A_RT, _A_TT, _A_BT, _A_CR,
        _A_HZ_BOLD, _A_CHECK, _A_CROSS,
    )


def check_mark(ok: bool = True) -> str:
    """Return a check-mark or cross character for the current mode."""
    _, _, _, _, _, _, _, _, _, _, _, _, chk, crs = _ch()
    return chk if ok else crs


# ---------------------------------------------------------------------------
# Padding
# ---------------------------------------------------------------------------


def _pad(text: str, width: int, align: str = "<") -> str:
    """Pad *text* to *width* visible chars, respecting ANSI codes."""
    vlen = vis_len(text)
    pad_n = max(0, width - vlen)
    if align == ">":
        return " " * pad_n + text
    if align == "^":
        left = pad_n // 2
        return " " * left + text + " " * (pad_n - left)
    return text + " " * pad_n


# ---------------------------------------------------------------------------
# Table builders
# ---------------------------------------------------------------------------


def _hz_line(
    widths: Sequence[int],
    left: str,
    mid: str,
    right: str,
    hz: str,
) -> str:
    """Build a horizontal line using the given box-drawing chars."""
    return left + mid.join(hz * (w + 2) for w in widths) + right


def build_table(
    headers: Sequence[str],
    rows: Sequence[Sequence[str]],
    aligns: Sequence[str] | None = None,
    *,
    colr: bool | None = None,
    title: str = "",
) -> str:
    """Build a bordered table and return it as a multi-line string.

    Parameters
    ----------
    headers
        Column header labels.
    rows
        Sequence of row tuples (each the same length as *headers*).
    aligns
        Per-column alignment: ``"<"`` (left), ``">"`` (right), ``"^"``
        (centre).  Defaults to left-aligned.
    colr
        Enable ANSI colours; defaults to the module-level ``USE_COLOR``.
    title
        Optional table title shown above the table.
    """
    tl, tr, bl, br, hz, vt, lt, rt, tt, bt, cr, _, _, _ = _ch()
    on = colr if colr is not None else USE_COLOR
    ncols = len(headers)
    if aligns is None:
        aligns = ["<"] * ncols

    # Compute column widths (max of header / data visible widths)
    widths = [vis_len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row[:ncols]):
            widths[i] = max(widths[i], vis_len(cell))

    lines: list[str] = []

    # Title
    if title:
        lines.append("")
        lines.append(cc(BOLD + CYAN, f"  {title}", on))
        lines.append("")

    # Top border
    lines.append("  " + _hz_line(widths, tl, tt, tr, hz))

    # Header row
    hdr_cells = vt.join(
        f" {cc(BOLD, _pad(h, widths[i], '^'), on)} "
        for i, h in enumerate(headers)
    )
    lines.append(f"  {vt}{hdr_cells}{vt}")

    # Header separator
    lines.append("  " + _hz_line(widths, lt, cr, rt, hz))

    # Data rows
    for row in rows:
        cells = vt.join(
            f" {_pad(row[i] if i < len(row) else '', widths[i], aligns[i])} "
            for i in range(ncols)
        )
        lines.append(f"  {vt}{cells}{vt}")

    # Bottom border
    lines.append("  " + _hz_line(widths, bl, bt, br, hz))

    return "\n".join(lines)


def section_header(title: str, *, colr: bool | None = None) -> str:
    """Return a styled section header string."""
    on = colr if colr is not None else USE_COLOR
    _, _, _, _, _, _, _, _, _, _, _, hz_bold, _, _ = _ch()
    bar = hz_bold * (len(title) + 2)
    return (
        f"\n  {cc(BOLD + CYAN, bar, on)}\n"
        f"  {cc(BOLD + WHITE, f' {title}', on)}\n"
        f"  {cc(BOLD + CYAN, bar, on)}"
    )


def kv_table(
    pairs: Sequence[tuple[str, str]],
    *,
    colr: bool | None = None,
    title: str = "",
) -> str:
    """Build a two-column key-value table.

    Parameters
    ----------
    pairs
        Sequence of (label, value) tuples.
    colr
        Enable ANSI colours.
    title
        Optional table title.
    """
    tl, _, bl, _, hz, vt, _, _, tt, bt, _, _, _, _ = _ch()
    on = colr if colr is not None else USE_COLOR
    if not pairs:
        return ""

    kw = max(vis_len(k) for k, _ in pairs)
    vw = max(vis_len(v) for _, v in pairs)

    lines: list[str] = []
    if title:
        lines.append("")
        lines.append(cc(BOLD + CYAN, f"  {title}", on))

    lines.append("  " + _hz_line([kw, vw], tl, tt, tl.replace(tl, _ch()[8]), hz))
    for key, val in pairs:
        k_cell = _pad(key, kw, "<")
        v_cell = _pad(val, vw, ">")
        lines.append(f"  {vt} {cc(DIM, k_cell, on)} {vt} {v_cell} {vt}")
    lines.append("  " + _hz_line([kw, vw], bl, bt, bl.replace(bl, _ch()[3]), hz))

    return "\n".join(lines)


def status_line(
    label: str,
    ok: bool,
    *,
    colr: bool | None = None,
    details: str = "",
) -> str:
    """Return a status line with check / cross indicator."""
    on = colr if colr is not None else USE_COLOR
    chk = check_mark(ok)
    icon = cc(GREEN, chk, on) if ok else cc(RED, chk, on)
    extra = f"  {cc(DIM, details, on)}" if details else ""
    return f"  {icon} {label}{extra}"


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
