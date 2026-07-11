"""Parser for plpidsim.dat — simulation scenario index mapping.

The ``plpidsim.dat`` file maps each (simulation, stage) pair to a hydrology
class index used as the column index into the ``plpaflce.dat`` affluent data.

**File format** (Fortran ``LeeIndSim`` in ``plp-leeidsim.f``)::

    # comment line 1
    # comment line 2
    NSimul  NEtaCau
    # comment line 3
    Mes  Etapa  SimulInd(1,...,NSimul)

where ``SimulInd(i)`` is a **1-based** hydrology class index.
"""

from pathlib import Path
from typing import Any, Dict, Optional

from .base_parser import BaseParser


class IdSimParser(BaseParser):
    """Parse ``plpidsim.dat`` — simulation-to-hydrology index mapping.

    After parsing, ``self.items`` is a list of dicts, one per stage::

        {"stage": <1-based>, "month": <1-based>, "indices": [int, …]}

    where each ``indices[i]`` is the 1-based hydrology class for
    simulation *i* at that stage.

    ``self.num_simulations`` is the number of simulations (columns).
    """

    def __init__(self, file_path: str | Path) -> None:
        super().__init__(file_path)
        self.num_simulations: int = 0
        self.num_stages: int = 0

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plpidsim.dat file."""
        lines = self._read_non_empty_lines()
        if len(lines) < 2:
            return

        # Line 0: NSimul  NEtaCau
        header = lines[0].split()
        self.num_simulations = self._parse_int(header[0])
        self.num_stages = self._parse_int(header[1])

        # Remaining lines: Mes  Etapa  SimulInd(1..NSimul)
        for line in lines[1:]:
            parts = line.split()
            if len(parts) < 2 + self.num_simulations:
                continue
            month = self._parse_int(parts[0])
            stage = self._parse_int(parts[1])
            indices = [
                self._parse_int(parts[2 + i]) for i in range(self.num_simulations)
            ]
            self._append(
                {
                    "stage": stage,
                    "month": month,
                    "indices": indices,
                }
            )

    def get_index(self, simulation: int, stage: int) -> Optional[int]:
        """Return the 1-based hydrology index for a (simulation, stage) pair.

        Parameters
        ----------
        simulation : int
            0-based simulation index.
        stage : int
            1-based stage number.

        Returns
        -------
        int or None
            1-based hydrology class index, or None if not found.
        """
        for entry in self._data:
            if entry["stage"] == stage:
                if 0 <= simulation < len(entry["indices"]):
                    return entry["indices"][simulation]
        return None

    def get_stage_map(self, simulation: int) -> Dict[int, int]:
        """Return the full per-stage hydrology row for one simulation.

        PLP advances the hydrology column at every calendar-year (January)
        boundary — ``SimulInd(ISim, IEta)`` is *per stage*, not a single
        column per simulation (``plp-leeidsim.f``; consumed per stage by
        ``plp-fasedual.f:605-620``).

        Parameters
        ----------
        simulation : int
            0-based simulation index.

        Returns
        -------
        dict
            ``{stage_1based: hydro_1based}`` for every parsed stage row
            covering this simulation.  Empty when the simulation index is
            out of range or no rows were parsed.
        """
        stage_map: Dict[int, int] = {}
        for entry in self._data:
            indices = entry["indices"]
            if 0 <= simulation < len(indices):
                stage_map[entry["stage"]] = indices[simulation]
        return stage_map

    def has_rotation(self, num_stages: Optional[int] = None) -> bool:
        """True when any stage's SimulInd row differs from the stage-1 row.

        A rotating mapping means each simulation advances to the next
        historical hydrological year at January boundaries, so consumers
        must select the aflce column **per stage** rather than freezing
        the stage-1 column.

        Parameters
        ----------
        num_stages : int, optional
            Restrict the check to stages ``1..num_stages``.  ``None``
            checks every parsed row.
        """
        rows = [
            entry
            for entry in self._data
            if num_stages is None or 1 <= entry["stage"] <= num_stages
        ]
        if not rows:
            return False
        base = min(rows, key=lambda e: e["stage"])["indices"]
        return any(entry["indices"] != base for entry in rows)
