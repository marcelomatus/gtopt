"""PLEXOS (CEN PCP) to gtopt conversion tool.

This package converts a CEN PCP daily PLEXOS bundle (``PLEXOS{YYYYMMDD}.zip``
or its inner ``DATOS{date}.zip[.xz]`` payload) into a gtopt JSON planning,
mirroring the role :mod:`plp2gtopt` plays for PLP cases and
:mod:`sddp2gtopt` plays for PSR-SDDP cases.

The MVP target is one CEN PCP daily bundle → one gtopt planning JSON
with a single scene / single phase / 24 hourly chronological blocks,
matching what ``tools/ucjl2gtopt.py`` produces for UC.jl benchmarks.

See ``DESIGN.md`` for the full architecture and roadmap.
"""

from __future__ import annotations

__version__ = "0.1.0"

from .plexos2gtopt import convert_plexos_bundle, validate_plexos_bundle
from .plexos_loader import PlexosBundle, locate_bundle

__all__ = [
    "PlexosBundle",
    "convert_plexos_bundle",
    "locate_bundle",
    "validate_plexos_bundle",
]
