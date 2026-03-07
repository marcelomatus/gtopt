#!/usr/bin/env python3
"""get_gtopt_binary.py – Standalone helper for Copilot/Claude agents.

This script lives in ``tools/`` (not ``scripts/``) because it is a
**developer/agent tool**, NOT an end-user utility.  It must **not** be
installed via ``pip install ./scripts`` or ``cmake install``.

Purpose
-------
Obtain a working ``gtopt`` binary with the minimum possible effort inside a
Copilot / codespace / CI agent session where building from scratch would take
5-10 minutes.

Strategy (tried in order):
  1. ``GTOPT_BIN`` environment variable.
  2. ``gtopt`` already on ``PATH``.
  3. Standard build-directory paths relative to the repository root.
  4. ``/tmp/gtopt-ci-bin/gtopt`` – previously downloaded artifact.
  5. Download ``gtopt-binary-debug`` artifact from the latest successful
     ``ubuntu.yml`` CI run (requires ``GITHUB_TOKEN`` or ``gh`` CLI).
  6. Build from source using ``cmake`` (slow, but always works if build
     dependencies are available).

Quick-start for agents
----------------------
**Option A – run the script directly:**

.. code-block:: bash

    # From the repository root – prints the binary path on stdout
    python tools/get_gtopt_binary.py

    # Force a fresh CI download even if a binary is already present
    python tools/get_gtopt_binary.py --force-download

    # Build from source as a last resort
    python tools/get_gtopt_binary.py --build

    # Export the result for subsequent test runs
    export GTOPT_BIN=$(python tools/get_gtopt_binary.py)
    pytest scripts/igtopt/tests/ -m integration -v

**Option B – import programmatically:**

.. code-block:: python

    import sys, pathlib
    sys.path.insert(0, str(pathlib.Path(__file__).parents[N] / "tools"))

    from get_gtopt_binary import get_gtopt_binary
    bin_path = get_gtopt_binary()       # str | raises RuntimeError
    bin_path = get_gtopt_binary(allow_build=True)   # also tries cmake

CI artifact download step-by-step
----------------------------------
``GITHUB_TOKEN`` is automatically injected in every GitHub Actions runner, so
agents running inside CI don't need any extra configuration.

.. code-block:: bash

    # Find the latest non-expired artifact id
    ART_ID=$(gh api \\
        "repos/marcelomatus/gtopt/actions/artifacts?name=gtopt-binary-debug" \\
        --jq '.artifacts | map(select(.expired|not)) | .[0].id')

    # Download and unzip to /tmp/gtopt-ci-bin/
    mkdir -p /tmp/gtopt-ci-bin
    gh api repos/marcelomatus/gtopt/actions/artifacts/${ART_ID}/zip \\
        --header "Accept: application/vnd.github+json" > /tmp/gtopt.zip
    unzip -o /tmp/gtopt.zip -d /tmp/gtopt-ci-bin
    chmod +x /tmp/gtopt-ci-bin/gtopt

    # Point tests at it
    export GTOPT_BIN=/tmp/gtopt-ci-bin/gtopt
    pytest scripts/igtopt/tests/ -m integration -v

Build-from-source step-by-step
-------------------------------
.. code-block:: bash

    # Prerequisites (Ubuntu 24.04)
    sudo apt-get install -y ccache coinor-libcbc-dev libboost-container-dev \\
        libspdlog-dev liblapack-dev libblas-dev
    conda install -y -c conda-forge arrow-cpp parquet-cpp boost-cpp

    # Configure + build (takes ~5-10 min on a fresh machine)
    cmake -S all -B build -DCMAKE_BUILD_TYPE=Debug \\
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \\
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \\
        -DCMAKE_PREFIX_PATH="$(conda info --base)"
    cmake --build build -j$(nproc)
    ./build/standalone/gtopt --version

Notes
-----
* The ``gtopt-binary-debug`` artifact expires **7 days** after the CI run.
* The binary is linked against Ubuntu 24.04 shared libraries (conda
  Arrow/Parquet + COIN-OR).  It runs on any Ubuntu 24.04 environment
  with the same libraries.
* This file must NOT be added to ``pyproject.toml`` ``py-modules`` or
  ``packages.find.include``, and must NOT be added to a cmake install target.
"""

from __future__ import annotations

import argparse
import json as _json
import logging
import multiprocessing
import os
import pathlib
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from typing import Optional

_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent  # tools/../ = repo root
_GITHUB_REPO = "marcelomatus/gtopt"
_ARTIFACT_NAME = "gtopt-binary-debug"
_DEFAULT_CI_BIN_DIR = pathlib.Path("/tmp/gtopt-ci-bin")

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Binary discovery
# ---------------------------------------------------------------------------


def find_gtopt_binary() -> Optional[str]:
    """Locate an existing ``gtopt`` binary without downloading or building.

    Search order:

    1. ``GTOPT_BIN`` environment variable.
    2. ``shutil.which("gtopt")`` (binary on ``PATH``).
    3. Standard build-directory paths relative to the repository root:

       - ``build/standalone/gtopt``   (``cmake -S all -B build`` default)
       - ``build/gtopt``
       - ``build-standalone/gtopt``
       - ``all/build/gtopt``

    4. ``/tmp/gtopt-ci-bin/gtopt`` – previously downloaded CI artifact.

    Returns ``None`` when the binary cannot be found.
    """
    env_bin = os.environ.get("GTOPT_BIN")
    if env_bin and pathlib.Path(env_bin).exists():
        return env_bin

    which_bin = shutil.which("gtopt")
    if which_bin:
        return which_bin

    for rel in (
        "build/standalone/gtopt",
        "build/gtopt",
        "build-standalone/gtopt",
        "all/build/gtopt",
    ):
        candidate = _REPO_ROOT / rel
        if candidate.exists():
            return str(candidate)

    ci_bin = _DEFAULT_CI_BIN_DIR / "gtopt"
    if ci_bin.exists():
        return str(ci_bin)

    return None


# ---------------------------------------------------------------------------
# CI artifact download
# ---------------------------------------------------------------------------


def download_gtopt_from_ci(
    dest_dir: pathlib.Path = _DEFAULT_CI_BIN_DIR,
    repo: str = _GITHUB_REPO,
    artifact_name: str = _ARTIFACT_NAME,
) -> pathlib.Path:
    """Download the ``gtopt-binary-debug`` artifact from the latest CI run.

    Uses the ``gh`` CLI when available; otherwise falls back to the
    ``GITHUB_TOKEN`` bearer token with ``urllib.request``.

    Parameters
    ----------
    dest_dir:
        Destination directory for the extracted binary.
        Default: ``/tmp/gtopt-ci-bin``.
    repo:
        GitHub repository in ``owner/name`` format.
    artifact_name:
        Name of the workflow artifact to download.

    Returns
    -------
    pathlib.Path
        Path to the executable ``gtopt`` binary.

    Raises
    ------
    RuntimeError
        When the artifact cannot be found or downloaded.
    """
    dest_dir = pathlib.Path(dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)
    bin_path = dest_dir / "gtopt"
    zip_path = dest_dir / "gtopt-artifact.zip"

    gh = shutil.which("gh")
    token = os.environ.get("GITHUB_TOKEN", "")

    if not (gh or token):
        raise RuntimeError(
            "Neither 'gh' CLI nor GITHUB_TOKEN is available. "
            "Install the GitHub CLI or set GITHUB_TOKEN to download CI artifacts."
        )

    # --- Step 1: resolve the latest non-expired artifact id -----------------
    artifact_id = _resolve_artifact_id(gh, token, repo, artifact_name)

    log.info(
        "Downloading artifact %s (id=%s) from %s…", artifact_name, artifact_id, repo
    )

    # --- Step 2: download the artifact ZIP ----------------------------------
    _download_artifact_zip(gh, token, repo, artifact_id, zip_path)

    # --- Step 3: extract and make executable --------------------------------
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(dest_dir)
    zip_path.unlink(missing_ok=True)

    if not bin_path.exists():
        raise RuntimeError(
            f"Expected 'gtopt' inside artifact ZIP, but not found in {dest_dir}. "
            f"Contents: {list(dest_dir.iterdir())}"
        )

    bin_path.chmod(0o755)
    log.info("gtopt binary available at %s", bin_path)
    return bin_path


def _resolve_artifact_id(
    gh: Optional[str], token: str, repo: str, artifact_name: str
) -> str:
    """Return the id of the latest non-expired artifact (as a string)."""
    if gh:
        try:
            result = subprocess.run(
                [
                    gh,
                    "api",
                    f"repos/{repo}/actions/artifacts?name={artifact_name}",
                    "--jq",
                    ".artifacts | map(select(.expired|not)) | .[0].id",
                ],
                capture_output=True,
                text=True,
                timeout=30,
                check=True,
            )
            artifact_id = result.stdout.strip()
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                f"gh api failed while looking up artifact '{artifact_name}': "
                f"{exc.stderr}"
            ) from exc
    else:
        url = (
            f"https://api.github.com/repos/{repo}/actions/artifacts"
            f"?name={artifact_name}"
        )
        req = urllib.request.Request(
            url,
            headers={
                "Authorization": f"Bearer {token}",
                "Accept": "application/vnd.github+json",
            },
        )
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = _json.loads(resp.read())
        valid = [a for a in data.get("artifacts", []) if not a.get("expired")]
        if not valid:
            raise RuntimeError(
                f"No non-expired artifact '{artifact_name}' found in {repo}"
            )
        artifact_id = str(valid[0]["id"])

    if not artifact_id or artifact_id == "null":
        raise RuntimeError(
            f"No non-expired artifact '{artifact_name}' found in {repo}."
        )
    return artifact_id


def _download_artifact_zip(
    gh: Optional[str],
    token: str,
    repo: str,
    artifact_id: str,
    zip_path: pathlib.Path,
) -> None:
    """Download the artifact ZIP to *zip_path*."""
    if gh:
        try:
            with open(zip_path, "wb") as fh:
                subprocess.run(
                    [
                        gh,
                        "api",
                        f"repos/{repo}/actions/artifacts/{artifact_id}/zip",
                        "--header",
                        "Accept: application/vnd.github+json",
                    ],
                    stdout=fh,
                    timeout=120,
                    check=True,
                )
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                f"gh api failed while downloading artifact {artifact_id}: {exc.stderr}"
            ) from exc
    else:
        url = f"https://api.github.com/repos/{repo}/actions/artifacts/{artifact_id}/zip"
        req = urllib.request.Request(
            url,
            headers={
                "Authorization": f"Bearer {token}",
                "Accept": "application/vnd.github+json",
            },
        )
        with urllib.request.urlopen(req, timeout=120) as resp, open(
            zip_path, "wb"
        ) as fh:
            fh.write(resp.read())


# ---------------------------------------------------------------------------
# Build from source
# ---------------------------------------------------------------------------


def build_gtopt_from_source(
    repo_root: Optional[pathlib.Path] = None,
    build_dir: Optional[pathlib.Path] = None,
    build_type: str = "Debug",
    jobs: Optional[int] = None,
) -> pathlib.Path:
    """Build ``gtopt`` from source using cmake.

    This is the **slowest** option (~5-10 minutes on a fresh machine) but
    works when CI artifacts are unavailable.

    Prerequisites (Ubuntu 24.04):

    .. code-block:: bash

        sudo apt-get install -y ccache coinor-libcbc-dev \\
            libboost-container-dev libspdlog-dev liblapack-dev libblas-dev
        conda install -y -c conda-forge arrow-cpp parquet-cpp boost-cpp

    Parameters
    ----------
    repo_root:
        Repository root directory.  Defaults to the parent of ``tools/``.
    build_dir:
        CMake build directory.  Defaults to ``<repo_root>/build``.
    build_type:
        CMake build type (``Debug``, ``Release``, ``RelWithDebInfo``).
    jobs:
        Number of parallel jobs for ``cmake --build``.
        Defaults to ``multiprocessing.cpu_count()``.

    Returns
    -------
    pathlib.Path
        Path to the compiled ``gtopt`` binary.

    Raises
    ------
    RuntimeError
        When cmake is not found or the build fails.
    """
    if repo_root is None:
        repo_root = _REPO_ROOT
    if build_dir is None:
        build_dir = repo_root / "build"
    if jobs is None:
        jobs = multiprocessing.cpu_count()

    cmake = shutil.which("cmake")
    if not cmake:
        raise RuntimeError(
            "cmake not found on PATH.  Install cmake to build gtopt from source."
        )

    bin_path = build_dir / "standalone" / "gtopt"
    if bin_path.exists():
        log.info("Using existing build at %s", bin_path)
        return bin_path

    # Detect compilers
    c_compiler = shutil.which("clang") or shutil.which("gcc") or "cc"
    cxx_compiler = shutil.which("clang++") or shutil.which("g++") or "c++"

    # Try to determine conda prefix for Arrow/Parquet
    conda_prefix = _conda_prefix()

    log.info("Configuring gtopt (build_type=%s, build_dir=%s)…", build_type, build_dir)
    configure_cmd = [
        cmake,
        "-S",
        str(repo_root / "all"),
        "-B",
        str(build_dir),
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DCMAKE_C_COMPILER={c_compiler}",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
    ]
    if shutil.which("ccache"):
        configure_cmd += [
            "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
            "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache",
        ]
    if conda_prefix:
        configure_cmd += [f"-DCMAKE_PREFIX_PATH={conda_prefix}"]

    result = subprocess.run(
        configure_cmd, capture_output=False, cwd=str(repo_root), check=False
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"cmake configure failed (returncode={result.returncode}).  "
            "Check that all build dependencies are installed."
        )

    log.info("Building gtopt with %d jobs…", jobs)
    build_result = subprocess.run(
        [cmake, "--build", str(build_dir), f"-j{jobs}"],
        capture_output=False,
        cwd=str(repo_root),
        check=False,
    )
    if build_result.returncode != 0:
        raise RuntimeError(
            f"cmake --build failed (returncode={build_result.returncode})."
        )

    if not bin_path.exists():
        raise RuntimeError(f"Build succeeded but binary not found at {bin_path}.")

    log.info("Build complete: %s", bin_path)
    return bin_path


def _conda_prefix() -> Optional[str]:
    """Return the active conda base prefix, or None if conda is not available."""
    conda = shutil.which("conda")
    if not conda:
        return None
    try:
        result = subprocess.run(
            [conda, "info", "--base"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        prefix = result.stdout.strip()
        return prefix if prefix else None
    except (subprocess.SubprocessError, OSError):
        return None


# ---------------------------------------------------------------------------
# Top-level helper
# ---------------------------------------------------------------------------


def get_gtopt_binary(
    allow_download: bool = True,
    allow_build: bool = False,
    force_download: bool = False,
) -> str:
    """Return the path to a working ``gtopt`` binary.

    Parameters
    ----------
    allow_download:
        If ``True`` (default), attempt to download the CI artifact when the
        binary is not found locally.
    allow_build:
        If ``True``, fall back to building from source when download fails or
        is unavailable.  Default: ``False`` (build is slow).
    force_download:
        If ``True``, skip the local-search phase and go straight to CI
        artifact download.

    Returns
    -------
    str
        Path to the ``gtopt`` binary.

    Raises
    ------
    RuntimeError
        When the binary cannot be obtained by any available strategy.
    """
    if not force_download:
        binary = find_gtopt_binary()
        if binary:
            log.info("Found gtopt binary: %s", binary)
            return binary

    if allow_download:
        gh = shutil.which("gh")
        token = os.environ.get("GITHUB_TOKEN", "")
        if gh or token:
            try:
                bin_path = download_gtopt_from_ci()
                return str(bin_path)
            except RuntimeError as exc:
                log.warning("CI artifact download failed: %s", exc)
        else:
            log.info("Skipping CI download: no 'gh' CLI and no GITHUB_TOKEN found.")

    if allow_build:
        try:
            bin_path = build_gtopt_from_source()
            return str(bin_path)
        except RuntimeError as exc:
            raise RuntimeError(
                f"Could not obtain gtopt binary: build from source failed: {exc}"
            ) from exc

    raise RuntimeError(
        "gtopt binary not found.  Options:\n"
        "  1. Set GTOPT_BIN=/path/to/gtopt\n"
        "  2. Add gtopt to PATH\n"
        "  3. Build: cmake -S all -B build && cmake --build build -j$(nproc)\n"
        "  4. Set GITHUB_TOKEN and run: python tools/get_gtopt_binary.py\n"
        "  5. Run: python tools/get_gtopt_binary.py --build"
    )


# ---------------------------------------------------------------------------
# pytest fixture helper  (imported by conftest.py)
# ---------------------------------------------------------------------------


def make_gtopt_bin_fixture(allow_build: bool = False):
    """Return a pytest fixture function that yields the gtopt binary path.

    Usage in a ``conftest.py``::

        from get_gtopt_binary import make_gtopt_bin_fixture
        gtopt_bin = make_gtopt_bin_fixture()

    The fixture skips (not fails) when the binary cannot be obtained.
    """
    import pytest  # pylint: disable=import-outside-toplevel

    @pytest.fixture(scope="module")
    def gtopt_bin() -> str:  # pylint: disable=redefined-outer-name
        """Pytest fixture providing the ``gtopt`` binary path."""
        try:
            return get_gtopt_binary(allow_build=allow_build)
        except RuntimeError as exc:
            pytest.skip(str(exc))
        return ""  # pragma: no cover

    return gtopt_bin


# ---------------------------------------------------------------------------
# CLI entry-point
# ---------------------------------------------------------------------------


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="get_gtopt_binary",
        description=(
            "Obtain the gtopt binary for agent/CI use. "
            "Prints the binary path to stdout."
        ),
    )
    parser.add_argument(
        "--force-download",
        action="store_true",
        help="Skip local search and download from CI artifact directly.",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="Fall back to building from source if download is unavailable.",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Enable verbose logging.",
    )
    return parser


def main() -> int:
    """CLI entry-point: find/download/build gtopt and print its path."""
    parser = _build_parser()
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(levelname)s: %(message)s",
    )

    try:
        binary = get_gtopt_binary(
            allow_download=True,
            allow_build=args.build,
            force_download=args.force_download,
        )
        print(binary)
        return 0
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
