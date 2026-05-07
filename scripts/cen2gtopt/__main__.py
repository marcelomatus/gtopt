# SPDX-License-Identifier: BSD-3-Clause
"""python -m cen2gtopt → main.cli."""

from __future__ import annotations

import sys

from cen2gtopt.main import cli


if __name__ == "__main__":
    sys.exit(cli())
