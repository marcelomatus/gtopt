# SPDX-License-Identifier: BSD-3-Clause
"""Case type detection: PLP, gtopt, or passthrough."""

from __future__ import annotations

import enum
from pathlib import Path

# Canonical PLP files that must be present for a valid PLP case.
# plpblo.dat (block definitions) is always present in every PLP case.
_PLP_REQUIRED = ("plpblo.dat",)

# Additional PLP indicator files — presence of any confirms it's PLP.
_PLP_INDICATORS = (
    "plpbar.dat",
    "plpdem.dat",
    "plpeta.dat",
    "plpcnfce.dat",
    "plpcnfli.dat",
    "plpcosce.dat",
)


class CaseType(enum.Enum):
    """Detected case type for a directory argument."""

    PLP = "plp"
    GTOPT = "gtopt"
    PASSTHROUGH = "passthrough"


def detect_case_type(path: Path) -> CaseType:
    """Classify a path as a PLP case, gtopt case, or passthrough.

    - **PLP**: directory contains ``plpblo.dat`` plus at least one other
      ``plp*.dat`` file.
    - **gtopt**: directory contains ``<dirname>/<dirname>.json``.
    - **passthrough**: everything else (file, non-existent, etc.).
    """
    if not path.is_dir():
        return CaseType.PASSTHROUGH

    # Check for PLP case: plpblo.dat is the canonical required file
    has_required = all((path / f).is_file() for f in _PLP_REQUIRED)
    if has_required:
        has_indicator = any((path / f).is_file() for f in _PLP_INDICATORS)
        if has_indicator:
            return CaseType.PLP

    # Check for gtopt case: dir/dir_name.json
    dir_name = path.name
    json_file = path / f"{dir_name}.json"
    if json_file.is_file():
        return CaseType.GTOPT

    return CaseType.PASSTHROUGH


def infer_gtopt_dir(plp_dir: Path) -> Path:
    """Derive gtopt output directory from a PLP directory name.

    ``plp_case_2y`` → ``gtopt_case_2y``.
    Non-``plp_`` prefixed names get a ``gtopt_`` prefix prepended.
    """
    name = plp_dir.name
    if name.startswith("plp_"):
        return plp_dir.parent / ("gtopt_" + name[4:])
    return plp_dir.parent / ("gtopt_" + name)
