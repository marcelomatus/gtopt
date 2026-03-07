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
  6. Install build dependencies and build from source (``--build`` flag).

Quick-start for agents
----------------------
**Option A – run the script directly:**

.. code-block:: bash

    # From the repository root – prints the binary path on stdout
    python tools/get_gtopt_binary.py

    # Force a fresh CI download even if a binary is already present
    python tools/get_gtopt_binary.py --force-download

    # Install dependencies and build from source as a last resort
    python tools/get_gtopt_binary.py --build

    # Install deps + build, then export for test runs
    export GTOPT_BIN=$(python tools/get_gtopt_binary.py --build)
    pytest scripts/igtopt/tests/ -m integration -v

**Option B – import programmatically:**

.. code-block:: python

    import sys, pathlib
    sys.path.insert(0, str(pathlib.Path(__file__).parents[N] / "tools"))

    from get_gtopt_binary import get_gtopt_binary
    bin_path = get_gtopt_binary()                  # str | raises RuntimeError
    bin_path = get_gtopt_binary(allow_build=True)  # also installs deps & builds

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

Build-from-source step-by-step (Ubuntu 24.04)
----------------------------------------------
The ``--build`` flag (or ``allow_build=True``) automatically runs every step
below before calling cmake.  You can also run the steps manually:

.. code-block:: bash

    # STEP 1 – ccache  (must be installed BEFORE cmake configure)
    sudo apt-get install -y ccache

    # STEP 2 – Core build dependencies
    sudo apt-get install -y --no-install-recommends \\
        coinor-libcbc-dev libboost-container-dev libspdlog-dev \\
        liblapack-dev libblas-dev zlib1g-dev ca-certificates lsb-release wget

    # STEP 3 – Arrow / Parquet via conda  (APT source is often network-blocked)
    conda install -y -c conda-forge arrow-cpp parquet-cpp boost-cpp

    # STEP 4 – Clang 21 via LLVM APT (primary compiler; GCC 14 used as fallback)
    wget -qO /tmp/llvm.gpg https://apt.llvm.org/llvm-snapshot.gpg.key
    sudo gpg --dearmor -o /usr/share/keyrings/llvm.gpg /tmp/llvm.gpg
    CODENAME=$(lsb_release -cs)
    echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] \\
        https://apt.llvm.org/${CODENAME}/ llvm-toolchain-${CODENAME}-21 main" \\
        | sudo tee /etc/apt/sources.list.d/llvm-21.list
    sudo apt-get update -q
    sudo apt-get install -y --no-install-recommends \\
        clang-21 clang-tools-21 clang-format-21 clang-tidy-21 \\
        llvm-21-dev libomp-21-dev libc++-21-dev libc++abi-21-dev \\
        libclang-common-21-dev libclang-21-dev libclang-cpp21-dev
    # Register unversioned aliases
    for v in /usr/bin/clang*-21 /usr/bin/llvm*-21; do
        [ -e "$v" ] || continue
        b=$(basename "$v" "-21")
        sudo update-alternatives --install /usr/bin/"$b" "$b" "$v" 100
    done

    # STEP 5 – Configure (ccache + GCC/Clang + conda Arrow prefix)
    cmake -S all -B build -DCMAKE_BUILD_TYPE=Debug \\
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \\
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \\
        -DCMAKE_PREFIX_PATH="$(conda info --base)"

    # STEP 6 – Build
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
import glob as _glob
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
# Build from source – dependency installation helpers
# ---------------------------------------------------------------------------

#: Minimum Clang version the project accepts (see cmake/CompilerCheck.cmake).
_MIN_CLANG_VER = 21
#: Minimum GCC version the project accepts.
_MIN_GCC_VER = 14


def _pick_c_compiler() -> str:
    """Return the best available C compiler meeting the project's minimum.

    Preference order:
      1. clang-21 … clang-30 (Clang ≥ 21, descending so we pick the latest)
      2. gcc-14 … gcc-20    (GCC ≥ 14)
      3. Unversioned ``clang`` / ``gcc`` (last resort; may be too old)
    """
    for ver in range(30, _MIN_CLANG_VER - 1, -1):
        candidate = shutil.which(f"clang-{ver}")
        if candidate:
            return candidate
    for ver in range(20, _MIN_GCC_VER - 1, -1):
        candidate = shutil.which(f"gcc-{ver}")
        if candidate:
            return candidate
    return shutil.which("clang") or shutil.which("gcc") or "cc"


def _pick_cxx_compiler() -> str:
    """Return the best available C++ compiler meeting the project's minimum."""
    for ver in range(30, _MIN_CLANG_VER - 1, -1):
        candidate = shutil.which(f"clang++-{ver}")
        if candidate:
            return candidate
    for ver in range(20, _MIN_GCC_VER - 1, -1):
        candidate = shutil.which(f"g++-{ver}")
        if candidate:
            return candidate
    return shutil.which("clang++") or shutil.which("g++") or "c++"


def _run_cmd(cmd: list, description: str, check: bool = True) -> int:
    """Run *cmd*, log *description*, and return the exit code."""
    log.info("%s", description)
    log.debug("  $ %s", " ".join(str(c) for c in cmd))
    result = subprocess.run(cmd, check=False)
    if check and result.returncode != 0:
        raise RuntimeError(
            f"{description} failed (exit {result.returncode})."
        )
    return result.returncode


def _apt_install(packages: list, description: str = "") -> None:
    """Run ``sudo apt-get install -y`` for *packages* (no-op if apt unavailable)."""
    apt = shutil.which("apt-get")
    sudo = shutil.which("sudo")
    if not apt:
        log.warning("apt-get not found – skipping: %s", description or packages)
        return
    prefix = [sudo] if sudo else []
    _run_cmd(
        prefix + ["apt-get", "install", "-y", "--no-install-recommends"] + packages,
        description or f"apt-get install {' '.join(packages)}",
    )


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


def install_ccache() -> bool:
    """Ensure ``ccache`` is installed; return True if available afterwards.

    ``ccache`` **must** be installed before ``cmake -S all -B build`` because
    CMake bakes the launcher path into the build system at configure time.
    Installing ccache after configure does nothing; the build dir must be
    deleted and cmake re-run from scratch.
    """
    if shutil.which("ccache"):
        log.info("ccache already available.")
        return True
    log.info("Installing ccache (required before cmake configure)…")
    try:
        _apt_install(["ccache"], "install ccache")
        return shutil.which("ccache") is not None
    except RuntimeError as exc:
        log.warning("Could not install ccache: %s", exc)
        return False


def install_apt_build_deps() -> None:
    """Install core apt build dependencies (COIN-OR, Boost, spdlog, etc.).

    This installs:
    * ``coinor-libcbc-dev`` – COIN-OR LP/MIP solver
    * ``libboost-container-dev`` – Boost.Container (``flat_map``)
    * ``libspdlog-dev`` – structured logging
    * ``liblapack-dev``, ``libblas-dev`` – linear algebra (required by COIN-OR)
    * ``zlib1g-dev`` – zlib (required by Arrow/Parquet)
    * ``ca-certificates``, ``lsb-release``, ``wget`` – needed for LLVM APT setup
    """
    log.info("Installing apt build dependencies…")
    _apt_install(
        [
            "coinor-libcbc-dev",
            "libboost-container-dev",
            "libspdlog-dev",
            "liblapack-dev",
            "libblas-dev",
            "zlib1g-dev",
            "ca-certificates",
            "lsb-release",
            "wget",
        ],
        "install core build deps (COIN-OR, Boost, spdlog, LAPACK, zlib, wget)",
    )


def install_arrow_conda() -> Optional[str]:
    """Install Arrow and Parquet via conda-forge.

    The Apache Arrow APT source (``packages.apache.org/artifactory``) is
    frequently unreachable from network-restricted environments.  Installing
    via conda-forge is the verified-reliable alternative used in CI.

    Returns the conda base prefix (for ``-DCMAKE_PREFIX_PATH``), or ``None``
    if conda is not available.
    """
    conda = shutil.which("conda")
    if not conda:
        log.warning(
            "conda not found – Arrow/Parquet will not be installed via conda. "
            "CMake may fail if libarrow-dev is not available via apt either."
        )
        return None

    prefix = _conda_prefix()
    if prefix:
        # Check if Arrow is already present
        arrow_cmake = pathlib.Path(prefix) / "lib" / "cmake" / "Arrow"
        if arrow_cmake.exists():
            log.info("Arrow already installed in conda prefix %s", prefix)
            return prefix

    log.info("Installing Arrow/Parquet/Boost via conda-forge (this may take a while)…")
    _run_cmd(
        [
            conda,
            "install",
            "-y",
            "-c",
            "conda-forge",
            "arrow-cpp",
            "parquet-cpp",
            "boost-cpp",
        ],
        "conda install arrow-cpp parquet-cpp boost-cpp",
    )
    return _conda_prefix()


def install_clang21() -> bool:
    """Install Clang 21 from the LLVM APT repository on Ubuntu/Debian.

    This mirrors the steps in ``.github/actions/install-clang/action.yml``.
    The function:

    1. Adds the LLVM GPG key and APT source for the running Ubuntu release.
    2. Installs ``clang-21``, ``clang-format-21``, ``clang-tidy-21``, and
       related LLVM packages.
    3. Registers unversioned ``update-alternatives`` entries so that ``clang``,
       ``clang++``, etc. resolve to version 21.

    Returns ``True`` if Clang 21 is available after the call.

    .. note::

        Clang 22 packages are not yet available on ``apt.llvm.org``.
        This function always installs version 21.

    Prerequisites: ``wget``, ``gpg``, ``lsb-release``, ``sudo`` must be on PATH.
    """
    # Already installed?
    if shutil.which("clang-21"):
        log.info("clang-21 already installed.")
        return True

    wget = shutil.which("wget")
    gpg = shutil.which("gpg")
    lsb = shutil.which("lsb_release")
    sudo = shutil.which("sudo")
    if not (wget and gpg and lsb):
        log.warning(
            "wget/gpg/lsb_release not found – cannot install Clang 21 from LLVM APT."
        )
        return False

    log.info("Setting up LLVM APT repository for Clang 21…")
    key_path = pathlib.Path("/tmp/llvm-snapshot.gpg.key")
    keyring_path = pathlib.Path("/usr/share/keyrings/llvm-snapshot.gpg")
    sources_path = pathlib.Path("/etc/apt/sources.list.d/llvm-21.list")

    # Step 1: download GPG key
    _run_cmd(
        [wget, "-qO", str(key_path), "https://apt.llvm.org/llvm-snapshot.gpg.key"],
        "download LLVM snapshot GPG key",
    )

    # Step 2: dearmor key into keyring
    prefix = [sudo] if sudo else []
    _run_cmd(
        prefix + ["gpg", "--dearmor", "-o", str(keyring_path), str(key_path)],
        "dearmor LLVM GPG key",
    )

    # Step 3: get Ubuntu codename and write APT source
    codename_result = subprocess.run(
        [lsb, "-cs"], capture_output=True, text=True, check=True
    )
    codename = codename_result.stdout.strip()
    apt_line = (
        f"deb [signed-by={keyring_path}] "
        f"https://apt.llvm.org/{codename}/ "
        f"llvm-toolchain-{codename}-21 main\n"
    )
    if sources_path.exists():
        log.info("LLVM 21 APT source already configured at %s", sources_path)
    else:
        try:
            sources_path.write_text(apt_line)
        except PermissionError:
            if sudo:
                tee = shutil.which("tee")
                if tee:
                    proc = subprocess.run(
                        [sudo, tee, str(sources_path)],
                        input=apt_line,
                        text=True,
                        check=True,
                    )
                    if proc.returncode != 0:
                        log.warning("tee failed writing %s", sources_path)
            else:
                log.warning(
                    "Cannot write %s (no sudo) – skipping Clang 21 install",
                    sources_path,
                )
                return False

    # Step 4: apt-get update + install Clang 21 packages
    apt = shutil.which("apt-get")
    if not apt:
        log.warning("apt-get not found – cannot install Clang 21")
        return False

    _run_cmd(
        prefix + ["apt-get", "update", "-q"],
        "apt-get update (after adding LLVM repo)",
    )
    _apt_install(
        [
            "clang-21",
            "clang-tools-21",
            "clang-format-21",
            "clang-tidy-21",
            "llvm-21-dev",
            "llvm-21-tools",
            "libomp-21-dev",
            "libc++-21-dev",
            "libc++abi-21-dev",
            "libclang-common-21-dev",
            "libclang-21-dev",
            "libclang-cpp21-dev",
        ],
        "install Clang 21 packages",
    )

    # Step 5: register unversioned update-alternatives entries
    log.info("Registering unversioned clang/llvm alternatives…")
    update_alt = shutil.which("update-alternatives")
    if update_alt:
        for versioned_glob in (
            "/usr/bin/clang*-21",
            "/usr/bin/llvm*-21",
        ):
            for versioned in _glob.glob(versioned_glob):
                base = pathlib.Path(versioned).name.rsplit("-21", 1)[0]
                try:
                    subprocess.run(
                        prefix + [update_alt, "--remove-all", base],
                        check=False,
                        capture_output=True,
                    )
                    subprocess.run(
                        prefix
                        + [
                            update_alt,
                            "--install",
                            f"/usr/bin/{base}",
                            base,
                            versioned,
                            "100",
                        ],
                        check=False,
                        capture_output=True,
                    )
                except OSError:
                    pass

    available = shutil.which("clang-21") is not None
    if available:
        log.info("clang-21 installed successfully.")
    else:
        log.warning("clang-21 installation may have failed.")
    return available


def install_build_deps(
    install_clang: bool = True,
    install_arrow: bool = True,
) -> Optional[str]:
    """Install all build dependencies needed to compile gtopt on Ubuntu 24.04.

    This function mirrors the bootstrap sequence from
    ``.github/workflows/ubuntu.yml`` and the documentation in ``CLAUDE.md``
    and ``.github/copilot-instructions.md``.

    Step order (important – ccache MUST be installed first):

    1. **ccache** – must come before cmake configure so the launcher path is
       baked into the build system at configure time.
    2. **Core apt deps** – COIN-OR, Boost.Container, spdlog, LAPACK/BLAS, zlib,
       wget, lsb-release (prerequisites for the LLVM APT setup in step 4).
    3. **Arrow / Parquet** via ``conda install -c conda-forge`` – more reliable
       than the Apache Arrow APT repository in network-restricted environments.
    4. **Clang 21** via the LLVM APT repository – primary compiler; GCC 14 is
       used automatically if Clang 21 cannot be installed.

    Parameters
    ----------
    install_clang:
        Whether to attempt Clang 21 installation (default: True).
        Set to False in environments where LLVM APT access is blocked and
        GCC 14 is already available.
    install_arrow:
        Whether to install Arrow/Parquet via conda (default: True).

    Returns
    -------
    Optional[str]
        The conda base prefix string (for ``-DCMAKE_PREFIX_PATH``), or
        ``None`` if conda is not available.
    """
    # Step 1: ccache FIRST (cmake bakes the launcher path at configure time)
    install_ccache()

    # Step 2: core apt build dependencies
    install_apt_build_deps()

    # Step 3: Arrow / Parquet via conda-forge
    conda_prefix = install_arrow_conda() if install_arrow else _conda_prefix()

    # Step 4: Clang 21 (optional; GCC 14 used as fallback)
    if install_clang:
        install_clang21()

    return conda_prefix


# ---------------------------------------------------------------------------
# Build from source – cmake configure + build
# ---------------------------------------------------------------------------


def build_gtopt_from_source(
    repo_root: Optional[pathlib.Path] = None,
    build_dir: Optional[pathlib.Path] = None,
    build_type: str = "Debug",
    jobs: Optional[int] = None,
    with_deps: bool = True,
    install_clang: bool = True,
    install_arrow: bool = True,
) -> pathlib.Path:
    """Build ``gtopt`` from source using cmake.

    When *with_deps* is ``True`` (default), this function first calls
    :func:`install_build_deps` which installs ccache, COIN-OR, Arrow/Parquet,
    and optionally Clang 21 before invoking cmake.  This is the full
    end-to-end path: from a bare Ubuntu 24.04 machine to a working binary.

    The project requires **GCC ≥ 14** or **Clang ≥ 21**.  The function
    automatically selects the best available compiler (Clang 21+ preferred,
    GCC 14+ as fallback).

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
    with_deps:
        When ``True`` (default), install build dependencies before configuring.
        Set to ``False`` if all dependencies are already installed.
    install_clang:
        Passed to :func:`install_build_deps` – whether to attempt Clang 21
        installation (default: True).
    install_arrow:
        Passed to :func:`install_build_deps` – whether to install Arrow via
        conda (default: True).

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

    # ---- Step 1: install dependencies (ccache BEFORE cmake configure) ------
    conda_prefix: Optional[str]
    if with_deps:
        conda_prefix = install_build_deps(
            install_clang=install_clang,
            install_arrow=install_arrow,
        )
    else:
        conda_prefix = _conda_prefix()

    cmake = shutil.which("cmake")
    if not cmake:
        raise RuntimeError(
            "cmake not found on PATH.  Install cmake to build gtopt from source."
        )

    bin_path = build_dir / "standalone" / "gtopt"
    if bin_path.exists():
        log.info("Using existing build at %s", bin_path)
        return bin_path

    # ---- Step 2: pick best compiler ----------------------------------------
    # Prefer Clang 21+; fall back to GCC 14+.
    c_compiler = _pick_c_compiler()
    cxx_compiler = _pick_cxx_compiler()
    log.info("Using C compiler:   %s", c_compiler)
    log.info("Using C++ compiler: %s", cxx_compiler)

    # ---- Step 3: cmake configure -------------------------------------------
    # ccache must already be installed (done in install_build_deps above).
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
    else:
        log.warning(
            "ccache not found at configure time – builds will be slower and "
            "may fail if the build dir was previously configured with ccache."
        )
    if conda_prefix:
        configure_cmd += [f"-DCMAKE_PREFIX_PATH={conda_prefix}"]

    log.info("Configuring gtopt (build_type=%s, build_dir=%s)…", build_type, build_dir)
    result = subprocess.run(
        configure_cmd, capture_output=False, cwd=str(repo_root), check=False
    )
    if result.returncode != 0:
        # Stale CMakeCache.txt may cause failures after a compiler change.
        # Delete the build dir and retry once.
        cache_file = build_dir / "CMakeCache.txt"
        if cache_file.exists():
            log.warning(
                "cmake configure failed; removing stale build dir and retrying…"
            )
            shutil.rmtree(build_dir)
            result = subprocess.run(
                configure_cmd,
                capture_output=False,
                cwd=str(repo_root),
                check=False,
            )
        if result.returncode != 0:
            raise RuntimeError(
                f"cmake configure failed (returncode={result.returncode}).  "
                "Check that all build dependencies are installed.  "
                "Run with --verbose for more detail."
            )

    # ---- Step 4: cmake build -----------------------------------------------
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


# ---------------------------------------------------------------------------
# Top-level helper
# ---------------------------------------------------------------------------


def get_gtopt_binary(
    allow_download: bool = True,
    allow_build: bool = False,
    force_download: bool = False,
    install_clang: bool = True,
    install_arrow: bool = True,
) -> str:
    """Return the path to a working ``gtopt`` binary.

    Parameters
    ----------
    allow_download:
        If ``True`` (default), attempt to download the CI artifact when the
        binary is not found locally.
    allow_build:
        If ``True``, fall back to building from source (including dependency
        installation) when download fails or is unavailable.
        Default: ``False`` (build is slow).
    force_download:
        If ``True``, skip the local-search phase and go straight to CI
        artifact download.
    install_clang:
        Passed to :func:`build_gtopt_from_source` – whether to attempt Clang 21
        installation when building from source.  Default: ``True``.
    install_arrow:
        Passed to :func:`build_gtopt_from_source` – whether to install
        Arrow/Parquet via conda when building from source.  Default: ``True``.

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
            bin_path = build_gtopt_from_source(
                install_clang=install_clang,
                install_arrow=install_arrow,
            )
            return str(bin_path)
        except RuntimeError as exc:
            raise RuntimeError(
                f"Could not obtain gtopt binary: build from source failed: {exc}"
            ) from exc

    raise RuntimeError(
        "gtopt binary not found.  Options:\n"
        "  1. Set GTOPT_BIN=/path/to/gtopt\n"
        "  2. Add gtopt to PATH\n"
        "  3. Build: python tools/get_gtopt_binary.py --build\n"
        "  4. Set GITHUB_TOKEN and run: python tools/get_gtopt_binary.py\n"
        "  5. Manual cmake build:\n"
        "     cmake -S all -B build && cmake --build build -j$(nproc)\n"
        "     ./build/standalone/gtopt --version"
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
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
examples:
  # Find existing binary or download from CI
  python tools/get_gtopt_binary.py

  # Install all build deps + compile from source (full bootstrap)
  python tools/get_gtopt_binary.py --build

  # Build without installing Clang 21 (use GCC 14 already present)
  python tools/get_gtopt_binary.py --build --no-clang

  # Build without touching conda (Arrow already installed)
  python tools/get_gtopt_binary.py --build --no-arrow

  # Force a fresh CI artifact download
  python tools/get_gtopt_binary.py --force-download

  # Export the path for use in tests
  export GTOPT_BIN=$(python tools/get_gtopt_binary.py --build)
  pytest scripts/igtopt/tests/ -m integration -v
""",
    )
    parser.add_argument(
        "--force-download",
        action="store_true",
        help="Skip local search and download from CI artifact directly.",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help=(
            "Install build dependencies (ccache, COIN-OR, Arrow, Clang 21) "
            "and build gtopt from source when CI artifact is unavailable."
        ),
    )
    parser.add_argument(
        "--no-clang",
        dest="install_clang",
        action="store_false",
        default=True,
        help=(
            "Skip Clang 21 installation when --build is used. "
            "GCC 14 will be used as the compiler instead."
        ),
    )
    parser.add_argument(
        "--no-arrow",
        dest="install_arrow",
        action="store_false",
        default=True,
        help=(
            "Skip Arrow/Parquet conda installation when --build is used. "
            "Assumes Arrow is already available on CMAKE_PREFIX_PATH."
        ),
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
            install_clang=args.install_clang,
            install_arrow=args.install_arrow,
        )
        print(binary)
        return 0
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
