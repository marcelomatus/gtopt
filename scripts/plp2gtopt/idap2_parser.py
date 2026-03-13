"""Parser for plpidap2.dat — simulation-independent aperture indices.

The ``plpidap2.dat`` file defines a **single** set of aperture indices per
stage, shared by all simulations.  It is used for hydro plants with
"independent hydrology" (regime pluvial — rainfall-driven rather than
snowmelt-driven).

**File format** (Fortran ``LeeIndApe2`` in ``plp-leeidap2.f``)::

    # comment line 1
    # comment line 2
    NEtaCau
    # comment line 3
    Mes  Etapa  NApert  ApertInd(1,...,NApert)

``ApertInd`` values are **1-based** hydrology class indices.
"""

from pathlib import Path
from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class IdAp2Parser(BaseParser):
    """Parse ``plpidap2.dat`` — simulation-independent aperture definitions.

    After parsing, ``self.items`` is a list of dicts, one per stage::

        {
            "stage": <1-based>,
            "month": <1-based>,
            "num_apertures": int,
            "indices": [int, …],   # 1-based hydrology class indices
        }

    Access helpers:

    - ``get_apertures(stage)`` → list of 1-based indices
    """

    def __init__(self, file_path: str | Path) -> None:
        super().__init__(file_path)
        self.num_stages: int = 0

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plpidap2.dat file."""
        lines = self._read_non_empty_lines()
        if len(lines) < 2:
            return

        # Line 0: NEtaCau
        self.num_stages = self._parse_int(lines[0].split()[0])

        # Data lines: Mes  Etapa  NApert  ApertInd(1,...,NApert)
        for line in lines[1:]:
            parts = line.split()
            if len(parts) < 3:
                continue
            month = self._parse_int(parts[0])
            stage = self._parse_int(parts[1])
            num_apertures = self._parse_int(parts[2])
            indices = [
                self._parse_int(parts[3 + i])
                for i in range(min(num_apertures, len(parts) - 3))
            ]
            self._append(
                {
                    "stage": stage,
                    "month": month,
                    "num_apertures": num_apertures,
                    "indices": indices,
                }
            )

    def get_apertures(self, stage: int) -> List[int]:
        """Return 1-based hydrology indices for a given stage.

        Parameters
        ----------
        stage : int
            1-based stage number.

        Returns
        -------
        list of int
            List of 1-based hydrology class indices for apertures.
        """
        for entry in self._data:
            if entry["stage"] == stage:
                return entry["indices"]
        return []
