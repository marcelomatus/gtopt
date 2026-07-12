# SPDX-License-Identifier: BSD-3-Clause
"""Filter user constraints that dangle after network reduction.

Aggregation renames every line (``agg_*``), so any user constraint that
references ``line("NAME")`` — PLEXOS Tx comparison/security rows emitted
by plexos2gtopt into ``uc_*.pampl`` files — can no longer resolve.  In a
reduced transport model those per-line rows are not meaningful anyway
(the aggregated corridor capacities carry the transfer limits), so they
are dropped, along with the ``var slack_<name>;`` declaration of soft
rows and the comment block introducing them.

Filtered copies are written next to the reduced JSON with the reduced
case's stem as suffix; ``system.user_constraint_files`` is rewritten to
point at them.  Files without line references are kept untouched (or
copied when the reduced JSON lives in another directory).
"""

from __future__ import annotations

import logging
import re
import shutil
from pathlib import Path

from gtopt_reduce_network._io import Case

logger = logging.getLogger(__name__)

_LINE_REF = re.compile(r'\bline\s*\(\s*"')


def filter_line_user_constraints(
    case: Case, *, input_dir: Path, out_dir: Path, tag: str
) -> int:
    """Drop line-referencing user constraints; returns the dropped count."""
    dropped_total = 0

    # Inline JSON user constraints (defensive — converters normally keep
    # line rows in the pampl files).
    inline = case.system.get("user_constraint_array")
    if isinstance(inline, list):
        kept = [
            uc for uc in inline if not _LINE_REF.search(str(uc.get("expression", "")))
        ]
        if len(kept) != len(inline):
            dropped_total += len(inline) - len(kept)
            logger.warning(
                "dropped %d inline user constraints referencing lines",
                len(inline) - len(kept),
            )
            case.system["user_constraint_array"] = kept

    files = case.system.get("user_constraint_files")
    if not isinstance(files, list):
        return dropped_total

    new_files: list[str] = []
    for fname in files:
        src = input_dir / str(fname)
        if not src.exists():
            new_files.append(str(fname))
            continue
        text, dropped_names = _filter_pampl_text(src.read_text(encoding="utf-8"))
        if dropped_names:
            new_name = f"{src.stem}_{tag}{src.suffix}"
            (out_dir / new_name).write_text(text, encoding="utf-8")
            new_files.append(new_name)
            dropped_total += len(dropped_names)
            logger.info(
                "%s: dropped %d line-referencing constraint(s) → %s (%s)",
                src.name,
                len(dropped_names),
                new_name,
                ", ".join(dropped_names[:5]) + ("…" if len(dropped_names) > 5 else ""),
            )
        else:
            if out_dir.resolve() != input_dir.resolve():
                shutil.copy2(src, out_dir / src.name)
            new_files.append(src.name)
    case.system["user_constraint_files"] = new_files
    return dropped_total


def _filter_pampl_text(text: str) -> tuple[str, list[str]]:
    """Remove ``constraint`` blocks whose body references ``line("...")``.

    A block is the ``constraint NAME …:`` line plus continuation lines up
    to the terminating ``;`` and the immediately preceding comment run.
    The ``var slack_<NAME>;`` declaration of each dropped soft row is
    removed too.  Returns ``(filtered_text, dropped_names)``.
    """
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    pending: list[str] = []  # comment/blank run preceding a constraint
    dropped: list[str] = []
    i = 0
    while i < len(lines):
        stripped = lines[i].strip()
        if stripped.startswith("#") or not stripped:
            pending.append(lines[i])
            i += 1
            continue
        if stripped.startswith("constraint "):
            j = i
            while not lines[j].rstrip().endswith(";"):
                j += 1
            block = lines[i : j + 1]
            if _LINE_REF.search("".join(block)):
                dropped.append(stripped.split()[1].rstrip(":"))
                pending = [p for p in pending if not p.strip()]  # keep blanks
            else:
                out.extend(pending)
                out.extend(block)
                pending = []
            i = j + 1
            continue
        out.extend(pending)
        pending = []
        out.append(lines[i])
        i += 1
    out.extend(pending)

    filtered = "".join(out)
    for name in dropped:
        filtered = re.sub(
            rf"^var\s+slack_{re.escape(name)}\s*;[^\S\n]*\n?",
            "",
            filtered,
            flags=re.MULTILINE,
        )
    return filtered, dropped
