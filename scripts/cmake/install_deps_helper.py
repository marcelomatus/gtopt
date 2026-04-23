#!/usr/bin/env python3
"""Install scripts dependencies for the CTest ``scripts-install-deps`` fixture.

Exit codes
----------
0   – all dependencies are available (either already installed or just
      installed by this script).
77  – dependencies could not be installed; the CTest fixture should be
      marked as *skipped* (via ``SKIP_RETURN_CODE 77``) so that all
      dependent script tests are skipped rather than counted as failures.
"""

import argparse
import subprocess
import sys


def _packages_available() -> bool:
    """Return True if all key script packages are importable.

    Keep this list in sync with the packages declared in
    ``scripts/pyproject.toml`` ``[project]`` and
    ``[project.optional-dependencies]``, and with the corresponding
    import check in ``scripts/cmake/scripts_install_deps.cmake``.
    """
    try:
        # Keep this list in sync with pyproject.toml [project] and
        # [project.optional-dependencies].
        # fmt: off
        import pytest          # noqa: F401 -- dev dependency
        import plp2gtopt       # noqa: F401
        import igtopt          # noqa: F401
        import cvs2parquet     # noqa: F401
        import ts2gtopt        # noqa: F401
        import pp2gtopt        # noqa: F401
        import gtopt_compare   # noqa: F401
        import gtopt_check_lp  # noqa: F401
        import gtopt_check_json  # noqa: F401
        import gtopt2pp        # noqa: F401
        import gtopt_diagram   # noqa: F401
        # fmt: on
        return True
    except ImportError:
        return False


def _pip_install(scripts_dir: str, extra_args: list[str] | None = None) -> int:
    """Run pip install and return the process exit code."""
    cmd = [
        sys.executable,
        "-m",
        "pip",
        "install",
        "-q",
        *(extra_args or []),
        "-e",
        f"{scripts_dir}[dev]",
    ]
    return subprocess.call(cmd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--scripts-dir",
        required=True,
        help="Absolute path to the scripts/ source directory.",
    )
    args = parser.parse_args()

    # Fast path: everything is already importable.
    if _packages_available():
        print("scripts-install-deps: packages already available, skipping pip install")
        return 0

    # Try a normal pip install first.
    rc = _pip_install(args.scripts_dir)

    if rc != 0:
        # Retry with --break-system-packages for PEP 668
        # externally-managed environments (Debian/Ubuntu 24.04+).
        rc = _pip_install(args.scripts_dir, ["--break-system-packages"])

    if rc != 0:
        print(
            f"pip install failed (exit code {rc}).\n"
            f"Hint: run 'pip install -e {args.scripts_dir}[dev]' manually "
            "to diagnose.\n"
            "Scripts tests will be skipped.",
            file=sys.stderr,
        )
        return 77  # SKIP – picked up by SKIP_RETURN_CODE

    return 0


if __name__ == "__main__":
    sys.exit(main())
