"""Shared pytest fixtures for ``psse2gtopt`` tests."""

from __future__ import annotations

import os
import shutil
from pathlib import Path

import pytest


_DATA_DIR = Path(__file__).parent / "data"


def _raw_fixture(name: str) -> Path:
    """Return the path to a ``.raw`` fixture, skipping if absent."""
    path = _DATA_DIR / name
    if not path.is_file():
        pytest.skip(f"fixture not found: {path}")
    return path


@pytest.fixture(scope="session")
def data_dir() -> Path:
    """The directory holding the vendored + synthetic ``.raw`` fixtures."""
    return _DATA_DIR


@pytest.fixture(scope="session")
def synthetic_raw() -> Path:
    """Hand-crafted rev-33 fixture exercising every record type.

    Covers PQ/PV/slack/isolated buses, out-of-service loads and
    generators, a zero-MW load, a synchronous-condenser (pmax=0)
    generator, a zero-reactance branch (floor test), CZ=1 and CZ=2
    two-winding transformers, and a three-winding transformer (star
    expansion).
    """
    return _raw_fixture("synthetic.raw")


@pytest.fixture(scope="session")
def ieee14_raw() -> Path:
    """Vendored IEEE 14-bus case (PSS/E revision 32)."""
    return _raw_fixture("ieee14.raw")


@pytest.fixture(scope="session")
def ieee39_raw() -> Path:
    """Vendored New England 39-bus case (PSS/E revision 33)."""
    return _raw_fixture("ieee39.raw")


def _find_gtopt_binary() -> str | None:
    """Locate the gtopt binary for end-to-end solve smoke tests."""
    env_bin = os.environ.get("GTOPT_BIN")
    if env_bin and Path(env_bin).exists():
        return env_bin
    repo_root = Path(__file__).resolve().parents[3]
    for rel in (
        "build/standalone/gtopt",
        "build-release/standalone/gtopt",
        "build/gtopt",
        "all/build/gtopt",
    ):
        candidate = repo_root / rel
        if candidate.exists():
            return str(candidate)
    which_bin = shutil.which("gtopt")
    if which_bin:
        return which_bin
    return None


@pytest.fixture(scope="session")
def gtopt_bin() -> str:
    """Path to the gtopt binary; the test is skipped if it's missing."""
    binary = _find_gtopt_binary()
    if binary is None:
        pytest.skip(
            "gtopt binary not found — set GTOPT_BIN, add gtopt to PATH, "
            "or build the project first"
        )
    return binary
