"""Shared pytest fixtures and helpers for igtopt integration tests.

This module provides:

- :func:`_find_gtopt_binary` – locates the ``gtopt`` binary using standard
  search paths and environment variables.
- :func:`download_gtopt_from_ci` – downloads the ``gtopt-binary-debug``
  artifact from the latest successful GitHub Actions ``ubuntu.yml`` run on
  ``master``/``main``.  Requires the ``GITHUB_TOKEN`` environment variable
  (automatically available inside GitHub Actions runners).
- :fixture:`gtopt_bin` – pytest fixture that returns the binary path or
  skips the test when the binary is not available.

How to extract the gtopt binary without building from scratch
=============================================================

GitHub Actions uploads the compiled ``gtopt`` binary as the
``gtopt-binary-debug`` artifact on every successful ``ubuntu.yml`` run.
Inside a Copilot / codespace agent environment the binary can be obtained in
three ways (in order of preference):

1. **Environment variable** (fastest, zero network I/O):

   .. code-block:: bash

       export GTOPT_BIN=/path/to/gtopt
       pytest scripts/igtopt/tests/

   Set ``GTOPT_BIN`` to the path of any pre-existing ``gtopt`` binary.

2. **Build directory lookup** (works in a local dev checkout after
   ``cmake --build build``):

   The fixture automatically checks the following paths relative to the
   repository root::

       build/standalone/gtopt         (cmake -S all -B build default)
       build/gtopt
       build-standalone/gtopt
       all/build/gtopt

3. **CI artifact download via GitHub CLI** (useful when there is no local
   build but a ``GITHUB_TOKEN`` with ``actions:read`` permission is
   available – the token is automatically injected by GitHub Actions
   runners):

   .. code-block:: bash

       # From repo root – downloads to /tmp/gtopt-ci-bin/gtopt
       python - <<'EOF'
       from scripts.igtopt.tests.conftest import download_gtopt_from_ci
       import pathlib
       p = download_gtopt_from_ci(pathlib.Path("/tmp/gtopt-ci-bin"))
       print(p)
       EOF

   Or programmatically in a test / setup script::

       from igtopt.tests.conftest import download_gtopt_from_ci
       bin_path = download_gtopt_from_ci()

   The function uses the ``gh`` CLI (``gh api``) or, as a fallback, the
   ``requests`` + ``zipfile`` approach with the ``GITHUB_TOKEN`` bearer
   token.  The downloaded binary is placed in ``dest_dir`` (default:
   ``/tmp/gtopt-ci-bin``) and made executable.

   **Step-by-step for a Copilot agent session**:

   a. Check ``GITHUB_TOKEN`` is set (it always is inside an Actions runner):

      .. code-block:: bash

          echo ${GITHUB_TOKEN:0:4}...   # should print "ghu_..." or "ghp_..."

   b. Find the latest successful artifact ID:

      .. code-block:: bash

          gh api repos/marcelomatus/gtopt/actions/artifacts?name=gtopt-binary-debug \\
            --jq '.artifacts[0] | {id, name, expired, created_at}'

   c. Download and unzip:

      .. code-block:: bash

          ART_ID=$(gh api repos/marcelomatus/gtopt/actions/artifacts \\
              ?name=gtopt-binary-debug \\
              --jq '.artifacts | map(select(.expired|not)) | .[0].id')
          mkdir -p /tmp/gtopt-ci-bin
          gh api repos/marcelomatus/gtopt/actions/artifacts/${ART_ID}/zip \\
              --header "Accept: application/vnd.github+json" > /tmp/gtopt.zip
          unzip -o /tmp/gtopt.zip -d /tmp/gtopt-ci-bin
          chmod +x /tmp/gtopt-ci-bin/gtopt
          /tmp/gtopt-ci-bin/gtopt --version

   d. Point tests at it:

      .. code-block:: bash

          export GTOPT_BIN=/tmp/gtopt-ci-bin/gtopt
          pytest scripts/igtopt/tests/ -m integration -v

Notes
-----
* The artifact expires 7 days after the CI run that created it.
* The binary is a Debug build linked against the Ubuntu 24.04 shared
  libraries (libc, libstdc++, COIN-OR, Arrow/Parquet via conda).
  It runs on any Ubuntu 24.04 environment that has the same shared
  libraries available (conda environment or CI runner).
"""

from __future__ import annotations

import json as _json
import logging
import os
import pathlib
import shutil
import subprocess
import urllib.request
import zipfile
from typing import NoReturn, Optional

import pytest

_REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
_GITHUB_REPO = "marcelomatus/gtopt"
_ARTIFACT_NAME = "gtopt-binary-debug"
_CI_WORKFLOW = "ubuntu.yml"
_DEFAULT_CI_BIN_DIR = pathlib.Path("/tmp/gtopt-ci-bin")


def _find_gtopt_binary() -> Optional[str]:
    """Locate the ``gtopt`` binary.

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

    # Check previously downloaded CI artifact
    ci_bin = _DEFAULT_CI_BIN_DIR / "gtopt"
    if ci_bin.exists():
        return str(ci_bin)

    return None


def download_gtopt_from_ci(
    dest_dir: pathlib.Path = _DEFAULT_CI_BIN_DIR,
    repo: str = _GITHUB_REPO,
    artifact_name: str = _ARTIFACT_NAME,
) -> pathlib.Path:
    """Download the ``gtopt-binary-debug`` artifact from the latest successful CI run.

    Uses the ``gh`` CLI when available; otherwise falls back to the
    ``GITHUB_TOKEN`` bearer token with ``urllib.request``.

    The downloaded binary is placed at ``dest_dir/gtopt`` and made
    executable.

    Parameters
    ----------
    dest_dir:
        Directory where the binary will be extracted.  Created if it does not
        exist.  Default: ``/tmp/gtopt-ci-bin``.
    repo:
        GitHub repository in ``owner/name`` format.
    artifact_name:
        Name of the artifact to download.

    Returns
    -------
    Path to the extracted ``gtopt`` binary.

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

    # --- Step 1: resolve the latest non-expired artifact ID -----------------
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
    elif token:
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
                f"No non-expired artifact named '{artifact_name}' found in {repo}"
            )
        artifact_id = str(valid[0]["id"])
    else:
        raise RuntimeError(
            "Neither 'gh' CLI nor GITHUB_TOKEN is available. "
            "Set GITHUB_TOKEN or install the GitHub CLI to download CI artifacts."
        )

    if not artifact_id or artifact_id == "null":
        raise RuntimeError(
            f"No non-expired artifact named '{artifact_name}' found in {repo}."
        )

    logging.info(
        "Downloading artifact %s (id=%s) from %s…", artifact_name, artifact_id, repo
    )

    # --- Step 2: download the artifact ZIP ----------------------------------
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

    # --- Step 3: extract and make executable --------------------------------
    with zipfile.ZipFile(zip_path, "r") as zf:
        zf.extractall(dest_dir)
    zip_path.unlink(missing_ok=True)

    if not bin_path.exists():
        raise RuntimeError(
            f"Expected 'gtopt' inside artifact ZIP, but it was not found in {dest_dir}. "
            f"Contents: {list(dest_dir.iterdir())}"
        )

    bin_path.chmod(0o755)
    logging.info("gtopt binary available at %s", bin_path)
    return bin_path


def _skip(msg: str) -> NoReturn:
    """Raise a pytest.skip exception (typed as NoReturn for static analysis)."""
    pytest.skip(msg)
    raise AssertionError("unreachable")  # pragma: no cover


@pytest.fixture(scope="module")
def gtopt_bin() -> str:
    """Pytest fixture that provides the ``gtopt`` binary path.

    Discovery order:

    1. ``GTOPT_BIN`` environment variable.
    2. ``gtopt`` on the system ``PATH``.
    3. Standard build-directory paths relative to the repository root.
    4. ``/tmp/gtopt-ci-bin/gtopt`` (previously downloaded).
    5. Automatic download from the latest GitHub Actions artifact when
       ``GITHUB_TOKEN`` or the ``gh`` CLI is available.

    The test is **skipped** (not failed) when the binary cannot be found
    and no CI download is possible.
    """
    binary = _find_gtopt_binary()
    if binary:
        return binary

    # Try downloading from CI
    gh = shutil.which("gh")
    token = os.environ.get("GITHUB_TOKEN", "")
    if not (gh or token):
        _skip(
            "gtopt binary not found and no GITHUB_TOKEN / gh CLI available for "
            "automatic CI artifact download.  Set GTOPT_BIN or add gtopt to PATH."
        )

    try:
        bin_path = download_gtopt_from_ci()
        return str(bin_path)
    except RuntimeError as exc:
        _skip(f"gtopt binary not found and CI download failed: {exc}")
