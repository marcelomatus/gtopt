# SPDX-License-Identifier: BSD-3-Clause
"""Discover the gtopt and plp2gtopt binaries."""

from __future__ import annotations

import os
import shutil
from pathlib import Path


def _standard_build_paths() -> list[Path]:
    """Return standard build paths relative to the repo root."""
    # Walk up from this file to find the repo root (scripts/run_gtopt/_binary.py)
    repo = Path(__file__).resolve().parent.parent.parent
    return [
        repo / "build" / "standalone" / "gtopt",
        repo / "build" / "test" / "_deps" / "gtopt-build" / "standalone" / "gtopt",
        Path("/tmp/gtopt-ci-bin/gtopt"),
    ]


def find_gtopt_binary() -> str | None:
    """Locate the gtopt binary.

    Search order:
    1. ``GTOPT_BIN`` environment variable.
    2. ``gtopt`` on ``PATH``.
    3. Standard build directories.
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


def find_plp2gtopt() -> str | None:
    """Locate the plp2gtopt script on PATH."""
    return shutil.which("plp2gtopt")
