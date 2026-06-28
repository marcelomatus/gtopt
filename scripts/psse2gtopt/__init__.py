"""PSS(R)E RAW to gtopt conversion tool.

Converts a PSS/E ``.raw`` power-flow case (revisions 32/33) into a
single-snapshot multi-bus DC OPF gtopt JSON planning, mirroring the
role :mod:`plp2gtopt` and :mod:`sddp2gtopt` play for the PLP and PSR
SDDP dialects.

PSS/E ``.raw`` is a power-flow file: it carries network topology
(buses, branches, transformers), generator limits and loads, but **no
economic data**.  The converter therefore assigns a single configurable
generation cost; see :mod:`psse2gtopt.gtopt_writer`.
"""

from __future__ import annotations

__version__ = "0.1.0"

from .psse2gtopt import (
    convert_psse_case,
    resolve_raw_file,
    validate_psse_case,
)
from .raw_parser import parse_raw

__all__ = [
    "convert_psse_case",
    "parse_raw",
    "resolve_raw_file",
    "validate_psse_case",
]
