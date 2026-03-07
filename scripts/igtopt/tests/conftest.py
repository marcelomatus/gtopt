"""Pytest fixtures for igtopt integration tests.

The ``gtopt_bin`` fixture provides the path to the ``gtopt`` binary.

It checks (in order):

1. ``GTOPT_BIN`` environment variable – set by CTest (``ubuntu.yml`` build
   workflow) or by the ``scripts.yml`` pre-download step.
2. ``gtopt`` on ``PATH`` (``shutil.which``).
3. Standard build-directory paths relative to the repository root:
   ``build/standalone/gtopt``, ``build/gtopt``, ``build-standalone/gtopt``,
   ``all/build/gtopt``.

The test is **skipped** (not failed) when the binary cannot be found.
The fixture never downloads anything or installs libraries.  Set up the
binary before running tests:

* **In CI (ubuntu.yml)**: CTest sets ``GTOPT_BIN`` automatically.
* **In CI (scripts.yml)**: the ``Download gtopt binary`` workflow step
  runs ``python tools/get_gtopt_binary.py`` once to fetch the CI artifact
  and install runtime libraries, then exports ``GTOPT_BIN``.
* **Locally**: build the project (``cmake -S all -B build && cmake --build
  build -j$(nproc)``) or set ``GTOPT_BIN=/path/to/gtopt``.
"""

from __future__ import annotations

import pathlib
import shutil

import pytest


def _find_gtopt_binary() -> str | None:
    """Locate the gtopt binary without downloading or installing anything.

    Checks (in order):

    1. ``GTOPT_BIN`` environment variable.
    2. ``shutil.which("gtopt")`` (binary on ``PATH``).
    3. Standard build-directory paths relative to the repository root.

    Returns the path as a string, or ``None`` when not found.
    """
    import os  # pylint: disable=import-outside-toplevel

    env_bin = os.environ.get("GTOPT_BIN")
    if env_bin and pathlib.Path(env_bin).exists():
        return env_bin

    which_bin = shutil.which("gtopt")
    if which_bin:
        return which_bin

    # __file__ = scripts/igtopt/tests/conftest.py  →  parents[3] = repo root
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
