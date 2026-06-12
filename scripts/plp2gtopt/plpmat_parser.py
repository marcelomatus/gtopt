"""Parser for plpmat.dat — mathematical solver parameters.

The ``plpmat.dat`` file contains numerical configuration for the PLP solver,
including iteration limits, convergence tolerances, and penalty costs.

**File format** (Fortran ``LeePlpMat``)::

    # comment
    # PDMaxIte    PDError UmbIntConf NPlanosPorDefecto
    PDMaxIte      PDError UmbIntConf NPlanosPorDefecto
    # PMMaxIte    PMError
    PMMaxIte      PMError
    # Lambda CTasa CCauFal CVert CInter Ctransm FCotFinEF FPreProc FPrevia
    Lambda CTasa CCauFal CVert CInter Ctransm FCotFinEF FPreProc FPrevia
    # FFixTrasm FSeparaFCF FGrabaCSV FGrabaRES
    FFixTrasm FSeparaFCF FGrabaCSV FGrabaRES
    # ABLMax ABLEpsilon NumEtaCF
    ABLMax ABLEpsilon NumEtaCF
    # FConvPGradx FConvPVar UmbGradX UmbVar
    FConvPGradx FConvPVar UmbGradX UmbVar

Key fields mapped to gtopt:

- ``PDMaxIte`` (line 1, field 0) → ``sddp_options.max_iterations`` (only if > 1)
- ``CCauFal`` (line 3, field 2) — hydro flow failure cost (not demand failure)
"""

from pathlib import Path
from typing import Any, Optional

from .base_parser import BaseParser


class PlpmatParser(BaseParser):
    """Parse ``plpmat.dat`` — mathematical solver parameters.

    After parsing, the following attributes are available:

    - ``max_iterations`` — ``PDMaxIte``, the PD (primal-dual) iteration limit
    - ``pd_error`` — ``PDError``, convergence tolerance
    - ``default_cuts`` — ``NPlanosPorDefecto``, default number of cuts/planes
    - ``flow_fail_cost`` — ``CCauFal``, hydro flow failure cost ($/MWh)
    - ``vert_cost`` — ``CVert``, default vertimiento (per-block spill) cost.
      Used as the fallback spillway_cost for reservoirs that do NOT appear
      in plpvrebemb.dat (those that do appear use their per-embalse
      ``Costo de Rebalse`` instead).
    """

    def __init__(self, file_path: str | Path) -> None:
        super().__init__(file_path)
        self.max_iterations: int = 0
        self.pd_error: float = 0.0
        self.default_cuts: int = 0
        self.flow_fail_cost: float = 0.0
        self.vert_cost: float = 0.0

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the plpmat.dat file."""
        lines = self._read_non_empty_lines()
        if not lines:
            return

        # Line 0: PDMaxIte  PDError  UmbIntConf  NPlanosPorDefecto
        parts = lines[0].split()
        if len(parts) >= 1:
            self.max_iterations = self._parse_int(parts[0])
        if len(parts) >= 2:
            self.pd_error = self._parse_float(parts[1])
        if len(parts) >= 4:
            self.default_cuts = self._parse_int(parts[3])

        # Line 2: Lambda  CTasa  CCauFal  CVert  CInter  Ctransm  ...
        if len(lines) >= 3:
            parts2 = lines[2].split()
            if len(parts2) >= 3:
                self.flow_fail_cost = self._parse_float(parts2[2])
            if len(parts2) >= 4:
                self.vert_cost = self._parse_float(parts2[3])
