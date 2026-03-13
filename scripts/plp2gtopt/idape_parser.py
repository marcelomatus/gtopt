"""Parser for plpidape.dat — per-simulation aperture index mapping.

The ``plpidape.dat`` file defines, for each (simulation, stage) pair, the set
of aperture indices (1-based hydrology classes) used in the SDDP backward pass.

**File format** (Fortran ``LeeIndApe`` in ``plp-leeidape.f``)::

    # comment line 1
    # comment line 2
    NSimul  NEtaCau
    # Mes  Etapa  NApert  ApertInd(1,...,NApert) - Simulacion=01
    ...  (NEtaCau lines for simulation 1)
    # Mes  Etapa  NApert  ApertInd(1,...,NApert) - Simulacion=02
    ...  (NEtaCau lines for simulation 2)
    ...

Each simulation block has NEtaCau data lines.  ``ApertInd`` values are
**1-based** hydrology class indices into ``plpaflce.dat``.

Default behavior in PLP: if no file is provided, each simulation's
apertures default to ``SimulInd(ISim, IEta)`` (i.e. the same hydrology
used in the forward pass for that simulation).
"""

from pathlib import Path
from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class IdApeParser(BaseParser):
    """Parse ``plpidape.dat`` — per-simulation aperture definitions.

    After parsing, ``self.items`` is a list of dicts, one per
    (simulation, stage) entry::

        {
            "simulation": <0-based sim index>,
            "stage": <1-based>,
            "month": <1-based>,
            "num_apertures": int,
            "indices": [int, …],   # 1-based hydrology class indices
        }

    Access helpers:

    - ``get_apertures(simulation, stage)`` → list of 1-based indices
    - ``self.num_simulations`` — number of simulations
    - ``self.num_stages`` — number of stages with aperture data
    """

    def __init__(self, file_path: str | Path) -> None:
        super().__init__(file_path)
        self.num_simulations: int = 0
        self.num_stages: int = 0

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plpidape.dat file."""
        lines = self._read_non_empty_lines()
        if len(lines) < 2:
            return

        # Line 0: NSimul  NEtaCau
        header = lines[0].split()
        self.num_simulations = self._parse_int(header[0])
        self.num_stages = self._parse_int(header[1])

        # Data lines: grouped by simulation, each with NEtaCau entries
        line_idx = 1
        sim_idx = 0
        while line_idx < len(lines) and sim_idx < self.num_simulations:
            stage_count = 0
            while line_idx < len(lines) and stage_count < self.num_stages:
                parts = lines[line_idx].split()
                if len(parts) < 3:
                    line_idx += 1
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
                        "simulation": sim_idx,
                        "stage": stage,
                        "month": month,
                        "num_apertures": num_apertures,
                        "indices": indices,
                    }
                )
                line_idx += 1
                stage_count += 1
            sim_idx += 1

    def get_apertures(self, simulation: int, stage: int) -> List[int]:
        """Return 1-based hydrology indices for a (simulation, stage) pair.

        Parameters
        ----------
        simulation : int
            0-based simulation index.
        stage : int
            1-based stage number.

        Returns
        -------
        list of int
            List of 1-based hydrology class indices for apertures.
        """
        for entry in self._data:
            if entry["simulation"] == simulation and entry["stage"] == stage:
                return entry["indices"]
        return []
