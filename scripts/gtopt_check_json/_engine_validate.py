# SPDX-License-Identifier: BSD-3-Clause
"""Engine-side validation check — shells out to ``gtopt --lp-only`` and
harvests the C++-side ``Validation: …`` warnings / errors as Findings.

This avoids re-implementing every C++ validator (`check_referential_integrity`,
`check_positivity`, `check_piecewise_feasibility`, `check_aperture_references`,
`check_completeness`, `check_scenario_probabilities`, …) in Python: instead
we ask the canonical authority — the gtopt engine itself — to validate the
case and surface its findings as `gtopt_check_json` `Finding`s.

Picks up *every* future C++ validator automatically with zero drift.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from gtopt_check_json._checks_common import Finding, Severity


# ── Binary discovery (mirrors gtopt_check_solvers/_binary.py) ─────────────


def _standard_build_paths() -> list[Path]:
    """Return the standard build paths relative to the repo root."""
    repo = Path(__file__).resolve().parent.parent.parent
    return [
        repo / "build" / "standalone" / "gtopt",
        repo / "build" / "test" / "_deps" / "gtopt-build" / "standalone" / "gtopt",
        Path("/tmp/gtopt-ci-bin/gtopt"),
    ]


def _find_gtopt_binary() -> str | None:
    """Locate the gtopt binary.

    Search order: ``GTOPT_BIN`` env var → ``gtopt`` on ``PATH`` → standard
    build directories.  Returns ``None`` if no binary is found.
    """
    env_bin = os.environ.get("GTOPT_BIN")
    if env_bin and Path(env_bin).is_file():
        return env_bin

    on_path = shutil.which("gtopt")
    if on_path:
        return on_path

    for candidate in _standard_build_paths():
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)

    return None


# ── Output parsing ────────────────────────────────────────────────────────

# Matches a single `[ts] [warning] Validation: <msg>` or
#                  `[ts] [error]   Validation: <msg>` line written by
# `validate_planning.cpp` via `spdlog::warn("Validation: {}", …)` /
# `spdlog::error("Validation: {}", …)`.  We tolerate optional source-file
# prefix (`[file.cpp:LINE]`) inserted by some spdlog patterns.
_VALIDATION_RE = re.compile(
    r"\[(?P<level>warning|error)\]\s*"
    r"(?:\[[^\]]*\]\s*)?"
    r"Validation:\s*(?P<msg>.+?)\s*$"
)


def _parse_log_lines(log_text: str) -> list[Finding]:
    """Convert ``Validation: …`` lines from a gtopt log into Findings."""
    findings: list[Finding] = []
    for raw_line in log_text.splitlines():
        m = _VALIDATION_RE.search(raw_line)
        if m is None:
            continue
        level = m.group("level")
        msg = m.group("msg")
        sev = Severity.CRITICAL if level == "error" else Severity.WARNING
        findings.append(
            Finding(
                check_id="engine_validate",
                severity=sev,
                message=msg,
            )
        )
    return findings


# ── Check entry point ─────────────────────────────────────────────────────


def check_engine_validate(
    planning: dict[str, Any],
    json_paths: list[str] | None = None,
    base_dir: str = "",
) -> list[Finding]:
    """Run ``gtopt --lp-only`` on the same JSON inputs and harvest the
    C++ ``Validation: …`` log lines as Findings.

    Parameters
    ----------
    planning
        The merged planning dict.  Unused — the gtopt binary parses the
        original JSON files directly so JSON-merge precedence is exactly
        what gtopt itself sees.
    json_paths
        Original JSON file paths the user passed to ``gtopt_check_json``.
    base_dir
        Case directory (unused for this check).
    """
    # Silence unused-parameter linters — kept in the signature for the
    # uniform check-function contract.
    _ = planning, base_dir

    if not json_paths:
        return []

    binary = _find_gtopt_binary()
    if binary is None:
        return [
            Finding(
                check_id="engine_validate",
                severity=Severity.NOTE,
                message=(
                    "skipped: gtopt binary not found "
                    "(set GTOPT_BIN, add gtopt to PATH, or build the standalone)"
                ),
            )
        ]

    # Run gtopt with --lp-only so it parses + validates + builds LP and
    # exits before solving.  --output-directory + --log-directory point
    # both at a single tempdir we control, so the validation messages
    # land in `<tmpdir>/gtopt_*.log` (per the non-TTY logging convention).
    with tempfile.TemporaryDirectory(prefix="gtopt_check_lp_") as tmpdir:
        tmp = Path(tmpdir)
        cmd: list[str] = [
            binary,
            *json_paths,
            "--lp-only",
            "--set",
            f"output_directory={tmp}",
            "--set",
            f"log_directory={tmp}",
        ]
        try:
            subprocess.run(  # noqa: S603
                cmd,
                check=False,  # exit-1 on validation error is expected
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=120,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            return [
                Finding(
                    check_id="engine_validate",
                    severity=Severity.NOTE,
                    message=f"skipped: gtopt invocation failed: {exc}",
                )
            ]

        log_files = sorted(tmp.glob("gtopt_*.log"))
        if not log_files:
            return [
                Finding(
                    check_id="engine_validate",
                    severity=Severity.NOTE,
                    message=(
                        "skipped: gtopt produced no log file "
                        f"(checked {tmp}); the binary may be older than "
                        "the per-run gtopt_N.log convention"
                    ),
                )
            ]

        log_text = log_files[-1].read_text(encoding="utf-8", errors="replace")

    return _parse_log_lines(log_text)
