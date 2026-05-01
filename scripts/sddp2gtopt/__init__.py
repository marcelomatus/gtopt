"""SDDP (PSR Inc.) to gtopt conversion tool.

This package converts a PSR SDDP case directory into a gtopt JSON
planning, mirroring the role :mod:`plp2gtopt` plays for PLP cases.

v0 is JSON-only: it reads ``psrclasses.json`` (the typed snapshot PSR
SDDP writes alongside the legacy ``.dat`` files) and produces a gtopt
planning + Parquet time-series. ``.dat`` fallback parsers will follow
in v1 for cases without a JSON manifest.

See ``DESIGN.md`` for the full architecture and roadmap.
"""

from __future__ import annotations

__version__ = "0.1.0"

from .psrclasses_loader import PsrClassesLoader, load_psrclasses
from .sddp2gtopt import convert_sddp_case, validate_sddp_case

__all__ = [
    "PsrClassesLoader",
    "convert_sddp_case",
    "load_psrclasses",
    "validate_sddp_case",
]
