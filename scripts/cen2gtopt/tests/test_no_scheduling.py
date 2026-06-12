# SPDX-License-Identifier: BSD-3-Clause
"""CI guard — assert the cen2gtopt package imports no scheduling libraries.

Per master plan §11 P2.F acceptance criterion 3, ``cen2gtopt`` is a
batch tool meant to be wrapped by an EXTERNAL scheduler (cron,
systemd timer, k8s CronJob, …) — it must NOT bundle a scheduling
runtime of its own.  The CLI ``cen2gtopt.daily_update`` documents
the wrap-up pattern in its docstring, which is allowed; what is
NOT allowed is importing or invoking a scheduling library.

This test enforces the "no in-process scheduler" rule by inspecting
import statements and function calls only — docstrings and comments
that *describe* external scheduling are explicitly permitted.
"""

from __future__ import annotations

import re
from pathlib import Path


_PKG_ROOT = Path(__file__).resolve().parent.parent

# Forbid only IMPORT statements + function CALLS for known in-process
# schedulers.  Docstring / comment mentions of cron / systemd are
# permitted (they document the external-wrap-up pattern).
_FORBIDDEN_IMPORTS = re.compile(
    r"^\s*(?:import|from)\s+("
    r"schedule|apscheduler|crontab|python_crontab|"
    r"daemon|python_daemon|systemd|sched"
    r")\b",
    re.MULTILINE,
)
_FORBIDDEN_CALLS = re.compile(
    r"\b(?:apscheduler\.|schedule\.every|sched\.scheduler"
    r"|systemd\.daemon|daemonize|daemon\.DaemonContext)\b"
)


def test_no_scheduling_code_in_package():
    hits: list[tuple[Path, int, str]] = []
    for src in _PKG_ROOT.rglob("*.py"):
        # Skip the test directory and __pycache__.
        if "tests" in src.parts or "__pycache__" in src.parts:
            continue
        text = src.read_text(encoding="utf-8")
        for lineno, line in enumerate(text.splitlines(), start=1):
            stripped = line.lstrip()
            # Skip pure comment lines — they cannot import or call.
            if stripped.startswith("#"):
                continue
            if _FORBIDDEN_IMPORTS.match(line) or _FORBIDDEN_CALLS.search(line):
                hits.append((src, lineno, stripped))
    assert not hits, (
        "in-process scheduling import/call detected in cen2gtopt "
        f"production code (external wrappers like cron/systemd are OK): {hits}"
    )
