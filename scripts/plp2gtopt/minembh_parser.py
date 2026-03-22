# -*- coding: utf-8 -*-

"""Parser for plpminembh.dat — per-stage minimum reservoir volume with slack.

The ``plpminembh.dat`` file provides per-stage minimum volume constraints for
reservoirs, with an associated penalty cost for violation (soft constraint).

**File format** (Fortran ``LeeMinEmbH`` in ``leeminembh.f``)::

    # Archivo de minimos de embalses con holgura (plpminembh.dat)
    # Numero de embalses con mantenimientos
    N
    # For each reservoir:
    # Nombre del embalse
    'RESERVOIR_NAME'
    #   Numero de Etapas con vmin
    num_stages
    # Etapa     VolMin     Costo
    stage_num   vmin_raw   cost
    ...

Field definitions:
  RESERVOIR_NAME – Reservoir name (must match a central in plpcnfce.dat)
  num_stages     – Number of stages with minimum volume overrides
  stage_num      – Stage number (1-based)
  vmin_raw       – Minimum volume [raw PLP units]; converted to dam³
                   by multiplying by ``energy_scale`` (= EmbFEsc / 1E6)
  cost           – Penalty cost for violating the minimum [$/dam³]

Unit conversions (matching PLP Fortran ``leeminembh.f``):
  Fortran: ``EmbVMinH = EmbCMinEsc * EmbFEsc / 1D3`` (PLP internal)
  plp2gtopt: ``vmin_dam3 = vmin_raw × energy_scale`` (dam³)
  These are equivalent since ``energy_scale = EmbFEsc / 1E6`` and
  ``PLP_internal / 1E3 = dam³``.
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class MinembhParser(BaseParser):
    """Parse ``plpminembh.dat`` — per-stage minimum reservoir volumes.

    After parsing, each entry contains:

    - ``name`` — reservoir name
    - ``stage`` — numpy int32 array of stage indices (1-based)
    - ``vmin`` — numpy float64 array of minimum volumes [dam³]
    - ``cost`` — numpy float64 array of penalty costs [$/dam³]
    """

    @property
    def minembhs(self) -> List[Dict[str, Any]]:
        """Return the parsed minimum-volume entries."""
        return self.get_all()

    @property
    def num_minembhs(self) -> int:
        """Return the number of entries."""
        return len(self.minembhs)

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plpminembh.dat file."""
        self.validate_file()

        central_parser = parsers.get("central_parser") if parsers else None
        lines = []
        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("The plpminembh.dat file is empty or malformed.")

            idx = self._next_idx(-1, lines)
            num_reservoirs = self._parse_int(lines[idx])

            for _ in range(num_reservoirs):
                idx = self._next_idx(idx, lines)
                name = lines[idx].strip("'")

                central = (
                    central_parser.get_central_by_name(name) if central_parser else None
                )
                scale = central["energy_scale"] if central else 1.0

                idx = self._next_idx(idx, lines)
                num_stages = self._parse_int(lines[idx])
                if num_stages <= 0:
                    continue

                # Vectorized parse: col 0=stage, col 1=vmin, col 2=cost
                next_idx, cols = self._parse_numeric_block(
                    lines,
                    idx + 1,
                    num_stages,
                    int_cols=(0,),
                    float_cols=(1, 2),
                )
                idx = next_idx - 1

                entry: Dict[str, Any] = {
                    "name": name,
                    "stage": cols[0],
                    "vmin": cols[1] * scale,
                    "cost": cols[2],
                }
                self._append(entry)

        finally:
            lines.clear()

    def get_minembh_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get minimum-volume data for a specific reservoir name."""
        return self.get_item_by_name(name)
