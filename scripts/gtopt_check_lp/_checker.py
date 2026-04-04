# SPDX-License-Identifier: BSD-3-Clause
"""Report formatting helpers and file-finding utilities for gtopt_check_lp.

This module contains:
- ``_find_latest_error_lp`` -- locate the newest error LP file
- ``_format_iis_report`` -- format solver IIS output for screen and AI
- ``_write_report`` -- write a plain-text report file (ANSI stripped)
- Solver-output filtering and truncation helpers
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Optional

from . import _colors as _col

# ---------------------------------------------------------------------------
# ANSI helpers
# ---------------------------------------------------------------------------

_BOLD = _col._BOLD  # noqa: SLF001
_RED = _col._RED  # noqa: SLF001
_YELLOW = _col._YELLOW  # noqa: SLF001
_GREEN = _col._GREEN  # noqa: SLF001
_CYAN = _col._CYAN  # noqa: SLF001
_RESET = _col._RESET  # noqa: SLF001

_USE_COLOR = True  # written by main() before any output


def _c(code: str, text: str) -> str:
    return f"{code}{text}{_RESET}" if _USE_COLOR else text


def _header(title: str) -> str:
    return _col.header(title)


# ---------------------------------------------------------------------------
# Find the latest error LP file
# ---------------------------------------------------------------------------


def _find_latest_error_lp(
    search_dirs: Optional[list[Path]] = None,
) -> Optional[Path]:
    """
    Find the most recently modified ``error*.lp`` or compressed error LP file.

    Searches *search_dirs* (default: current directory and its ``logs/``
    and ``output/`` subdirectories) and returns the newest file, or ``None``.

    Recognised file patterns:
      - ``error*.lp``
      - ``error*.lp.gz``
      - ``error*.lp.gzip``
    """
    if search_dirs is None:
        cwd = Path.cwd()
        search_dirs = [cwd, cwd / "logs", cwd / "output"]

    candidates: list[Path] = []
    for d in search_dirs:
        if d.is_dir():
            candidates.extend(d.glob("error*.lp"))
            candidates.extend(d.glob("error*.lp.gz"))
            candidates.extend(d.glob("error*.lp.gzip"))

    if not candidates:
        return None

    return max(candidates, key=lambda p: p.stat().st_mtime)


# ---------------------------------------------------------------------------
# Solver output filtering and formatting
# ---------------------------------------------------------------------------

# Patterns that indicate infeasibility-relevant solver output (not runtime
# noise).
_SOLVER_RELEVANT_PATTERNS = [
    re.compile(r"infeasib", re.IGNORECASE),
    re.compile(r"conflict", re.IGNORECASE),
    re.compile(r"IIS\b", re.IGNORECASE),
    re.compile(r"bound", re.IGNORECASE),
    re.compile(r"violation", re.IGNORECASE),
    re.compile(r"unbounded", re.IGNORECASE),
    re.compile(r"bad bound", re.IGNORECASE),
    re.compile(r"constraint", re.IGNORECASE),
    re.compile(r"primal\s", re.IGNORECASE),
    re.compile(r"dual\s", re.IGNORECASE),
    re.compile(r"presolve.*infeasib", re.IGNORECASE),
    re.compile(r"model\s+status", re.IGNORECASE),
    re.compile(r"[\u2717\u2022]"),  # our own formatted findings
    re.compile(r"^---\s"),  # section headers
    re.compile(r"^\u250c"),  # section headers
    re.compile(r"Key infeasibility"),
    re.compile(r"objective\s+value", re.IGNORECASE),
    re.compile(r"rows?\s+\d|columns?\s+\d", re.IGNORECASE),
]


def _filter_solver_lines(output: str) -> list[str]:
    """Extract only infeasibility-relevant lines from solver output."""
    relevant: list[str] = []
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if any(pat.search(stripped) for pat in _SOLVER_RELEVANT_PATTERNS):
            relevant.append(line)
    return relevant


def _truncate_solver_block(block: str, max_lines: int = 30) -> str:
    r"""Truncate a single solver output block to *max_lines* relevant lines.

    Keeps the ``\u250c\u2500\u2500`` header line and appends up to *max_lines*
    of the most relevant (infeasibility-related) lines from the block body.
    """
    lines = block.splitlines()
    # Separate header (first line starting with \u250c) from body
    header_lines: list[str] = []
    body_lines: list[str] = []
    for ln in lines:
        if not body_lines and ln.lstrip().startswith("\u250c"):
            header_lines.append(ln)
        else:
            body_lines.append(ln)

    relevant = _filter_solver_lines("\n".join(body_lines))
    if not relevant:
        relevant = [ln for ln in body_lines if ln.strip()]

    truncated = relevant[-max_lines:]
    return "\n".join(header_lines + truncated)


def _format_iis_report(
    solver_name: str,
    success: bool,
    output: str,
    *,
    full: bool = False,
) -> tuple[str, str]:
    """Format the solver IIS output for screen and AI.

    Returns ``(screen_report, ai_report)``.  When *full* is ``False``
    (default), each solver block on screen is truncated to 30 relevant
    lines.  When *full* is ``True``, all lines are shown.
    """
    header = _header(f"IIS Analysis ({solver_name})")

    if not success:
        err = f"\n{_c(_RED, 'Solver error:')} {output}"
        return f"{header}{err}", f"{header}{err}"

    # Split combined output into per-solver blocks (delimited by \u250c\u2500\u2500)
    blocks = re.split(r"(?=\n?\u250c\u2500\u2500)", output)
    blocks = [b for b in blocks if b.strip()]

    if full:
        screen_text = "\n".join([header, ""] + [b.strip() for b in blocks])
    else:
        truncated = [_truncate_solver_block(b) for b in blocks]
        screen_text = "\n".join(
            [
                header,
                "  (showing last 30 relevant lines per solver; "
                "use --full for complete output)",
                "",
            ]
            + truncated
        )

    # AI report: filter to 50 relevant lines per block
    ai_blocks = [_truncate_solver_block(b, max_lines=50) for b in blocks]
    ai_text = "\n".join([header] + ai_blocks)
    return screen_text, ai_text


def _write_report(output_file: Path, parts: list[str]) -> None:
    """Write the full report (with ANSI codes stripped) to *output_file*."""
    ansi_re = re.compile(r"\033\[[0-9;]*m")
    content = "\n\n".join(parts)
    content = ansi_re.sub("", content)
    try:
        output_file.write_text(content + "\n", encoding="utf-8")
        print(f"\nReport written to: {output_file}")
    except OSError as exc:
        print(f"Warning: could not write report: {exc}", file=sys.stderr)
