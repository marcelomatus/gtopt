"""Shared pytest fixtures for ``sddp2gtopt`` tests."""

from __future__ import annotations

import os
import shutil
from pathlib import Path

import pytest


_DATA_DIR = Path(__file__).parent / "data"


def _case_fixture(name: str) -> Path:
    """Return the path to a fixture case dir, skipping if absent."""
    path = _DATA_DIR / name
    if not path.is_dir():
        pytest.skip(f"fixture not found: {path}")
    return path


@pytest.fixture(scope="session")
def case0_dir() -> Path:
    """The vendored PSR ``case0`` (real upstream sample)."""
    return _case_fixture("case0")


@pytest.fixture(scope="session")
def case0_psrclasses(case0_dir: Path) -> Path:
    """Path to ``case0/psrclasses.json``."""
    return case0_dir / "psrclasses.json"


@pytest.fixture(scope="session")
def case_min_dir() -> Path:
    """Smallest hand-crafted case (1 thermal, 1 demand, 1 stage)."""
    return _case_fixture("case_min")


@pytest.fixture(scope="session")
def case_thermal_only_dir() -> Path:
    """Thermal-only multi-stage case used to exercise the writer."""
    return _case_fixture("case_thermal_only")


@pytest.fixture(scope="session")
def case_two_systems_dir() -> Path:
    """Two-system case — must be rejected with a clear error."""
    return _case_fixture("case_two_systems")


@pytest.fixture(scope="session")
def case_bad_no_study_dir() -> Path:
    """Case with no ``PSRStudy`` — must fail ``--validate``."""
    return _case_fixture("case_bad_no_study")


@pytest.fixture(scope="session")
def case_bad_truncated_dir() -> Path:
    """Truncated JSON — must surface a parse error."""
    return _case_fixture("case_bad_truncated")


# --- Upstream PSRClassesInterface.jl reference cases -----------------
# These are the additional sample cases shipped by PSR's Julia
# library (case1..case3 in `test/data/`), vendored to broaden our
# coverage of real-world SDDP topologies.  They exceed v0's single-
# bus, single-system scope and are used today as loader / parser
# stress tests; v1+ converters will start running them end-to-end.


@pytest.fixture(scope="session")
def psri_case1_dir() -> Path:
    """IEEE-123 — 129 buses, 130 lines, 9 gens, 3 batteries (v3+ target)."""
    return _case_fixture("psri_case1")


@pytest.fixture(scope="session")
def psri_case2_dir() -> Path:
    """Two-system hydrothermal with reserve & generation constraints."""
    return _case_fixture("psri_case2")


@pytest.fixture(scope="session")
def psri_case3_dir() -> Path:
    """Variant of case2 with the same multi-system topology."""
    return _case_fixture("psri_case3")


def _find_gtopt_binary() -> str | None:
    """Locate the gtopt binary for end-to-end LP-build smoke tests."""
    env_bin = os.environ.get("GTOPT_BIN")
    if env_bin and Path(env_bin).exists():
        return env_bin
    repo_root = Path(__file__).resolve().parents[3]
    for rel in (
        "build/standalone/gtopt",
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
