"""Shared pytest fixtures for plp2gtopt tests."""

from __future__ import annotations

import os
import pathlib
import shutil
from pathlib import Path

import pytest


def get_example_file(filename: str) -> Path:
    """Get path to example file in plp_dat_ex directory."""
    path = Path(__file__).parent.parent.parent / "cases" / "plp_dat_ex" / filename
    if not path.exists():
        raise FileNotFoundError(f"Example file not found: {path}")
    return path


@pytest.fixture
def valid_mance_file() -> Path:
    """Fixture providing path to valid test maintenance file."""
    return get_example_file("plpmance.dat")


def _find_gtopt_binary() -> str | None:
    """Locate the gtopt binary without downloading or installing anything.

    Checks (in order):

    1. ``GTOPT_BIN`` environment variable.
    2. ``shutil.which("gtopt")`` (binary on ``PATH``).
    3. Standard build-directory paths relative to the repository root.

    Returns the path as a string, or ``None`` when not found.
    """
    env_bin = os.environ.get("GTOPT_BIN")
    if env_bin and pathlib.Path(env_bin).exists():
        return env_bin

    # __file__ = scripts/plp2gtopt/tests/conftest.py  →  parents[3] = repo root
    repo_root = pathlib.Path(__file__).resolve().parents[3]
    for rel in (
        "build/standalone/gtopt",
        "build/gtopt",
        "build-standalone/gtopt",
        "all/build/gtopt",
    ):
        candidate = repo_root / rel
        if candidate.exists():
            return str(candidate)

    which_bin = shutil.which("gtopt")
    if which_bin:
        return which_bin

    return None


@pytest.fixture(scope="module")
def gtopt_bin() -> str:
    """Pytest fixture that provides the gtopt binary path.

    Skips the test module if the binary is not found.
    """
    binary = _find_gtopt_binary()
    if binary is None:
        pytest.skip(
            "gtopt binary not found – set GTOPT_BIN, add gtopt to PATH, "
            "or build the project first (cmake -S all -B build)"
        )
    return binary
