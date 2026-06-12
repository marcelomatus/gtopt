# SPDX-License-Identifier: BSD-3-Clause
"""Module entry point so ``python -m gtopt_check_topology`` works."""

from gtopt_check_topology.main import main

if __name__ == "__main__":  # pragma: no cover - exercised via CLI
    raise SystemExit(main())
