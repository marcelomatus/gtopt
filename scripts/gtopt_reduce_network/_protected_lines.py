# SPDX-License-Identifier: BSD-3-Clause
"""Identify lines that must NOT be dropped/aggregated by the reduction.

A line is *protected* when a constraint of the FULL model references it by
name — inline ``user_constraint_array`` expressions, the ``uc_*.pampl``
files, or ``line_commitment_array``.  Aggregating such a line renames it
(``agg_*``) so its constraint can no longer resolve and must be dropped;
the reduced model then optimises WITHOUT that transmission-security /
comparison constraint, and its commitment (used as a MIP warm-start seed)
violates the constraint back in the full model — the fixed dispatch is
infeasible and the seed is rejected.

Protecting these lines keeps them intact (original uid / name / params,
endpoints pinned as anchors) so every line-referencing constraint stays
valid in the reduced case and the seed transfers.
"""

from __future__ import annotations

import glob
import logging
import re
from pathlib import Path

from gtopt_reduce_network._io import Case, resolve_bus_ref

logger = logging.getLogger(__name__)

# ``line("NAME")`` / ``line('NAME')`` / ``line(NAME)`` — capture the ref.
_LINE_REF = re.compile(r'\bline\s*\(\s*["\']?([^"\')]+)')


def collect_protected_lines(
    case: Case, *, input_dir: Path | None = None
) -> tuple[set[int], set[int]]:
    """Return ``(protected_line_uids, protected_endpoint_bus_uids)``.

    Scans inline user constraints, the ``uc_*.pampl`` files under
    ``input_dir`` (the ``user_constraint_files``), and
    ``line_commitment_array`` for line references; resolves them to line
    uids and collects the two endpoint bus uids of each protected line.
    """
    line_by_name = {
        str(ln.get("name")): ln for ln in case.array("line_array") if ln.get("name")
    }
    line_by_uid = {int(ln["uid"]): ln for ln in case.array("line_array")}

    refs: set[str] = set()

    # 1. Inline user_constraint_array expressions.
    for uc in case.array("user_constraint_array"):
        refs.update(_LINE_REF.findall(str(uc.get("expression", ""))))

    # 2. External uc_*.pampl files (system.user_constraint_files).
    files = case.system.get("user_constraint_files")
    search_dir = input_dir
    if isinstance(files, list) and search_dir is not None:
        for fname in files:
            p = search_dir / str(fname)
            if p.exists():
                refs.update(_LINE_REF.findall(p.read_text(encoding="utf-8")))
    elif search_dir is not None:
        for match in glob.glob(str(search_dir / "uc_*.pampl")):
            refs.update(_LINE_REF.findall(Path(match).read_text(encoding="utf-8")))

    # 3. line_commitment_array (references a line by name or uid).
    for lc in case.array("line_commitment_array"):
        r = lc.get("line")
        if r is not None:
            refs.add(str(r).strip())

    protected_uids: set[int] = set()
    for ref in refs:
        ref = ref.strip()
        ln = line_by_name.get(ref)
        if ln is None and ref.isdigit() and int(ref) in line_by_uid:
            ln = line_by_uid[int(ref)]
        if ln is not None and "uid" in ln:
            protected_uids.add(int(ln["uid"]))

    endpoint_buses: set[int] = set()
    for uid in protected_uids:
        ln = line_by_uid.get(uid)
        if ln is None:
            continue
        for end in ("bus_a", "bus_b"):
            try:
                b = resolve_bus_ref(case, ln.get(end))
            except (KeyError, TypeError):
                b = None
            if b is not None:
                endpoint_buses.add(b)

    if protected_uids:
        logger.info(
            "protecting %d constraint-referenced lines (%d endpoint buses "
            "pinned as anchors)",
            len(protected_uids),
            len(endpoint_buses),
        )
    return protected_uids, endpoint_buses
