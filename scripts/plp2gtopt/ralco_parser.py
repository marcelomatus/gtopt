# -*- coding: utf-8 -*-

"""Parser for plpralco.dat format files containing drawdown limit data.

This parser reads the PLP "Ralco" constraint file which defines a
piecewise-linear volume-dependent discharge limit for a reservoir.

File format (plpralco.dat)::

  # comments are allowed (lines starting with #)
  # Nombre Lago Ralco. Restriccion desemba
  'RESERVOIR_NAME'
  # Numero de Segmentos Qdes
  N
  # Vcota [10^3 m3/s]  b  a
  volume_dam3  intercept_m3s  slope_per_dam3
  ...

Field definitions:
  RESERVOIR_NAME  – Name of the reservoir
  N               – Number of piecewise-linear segments
  volume_dam3     – Volume breakpoint [dam³ = 10³ m³]
  intercept_m3s   – Y-intercept [m³/s] (Fortran BRalco)
  slope_per_dam3  – Slope [m³/s per dam³] (Fortran ARalco)

Note: The Fortran format uses 'd' for scientific notation (e.g. 6.9868d-5),
which is converted to 'e' for Python float parsing.

Unit conversions applied during parsing:
  volume: ÷ 1000    (dam³ → gtopt physical = 10⁶ m³)
  slope:  × 1000    (m³/s per dam³ → m³/s per gtopt physical)
  intercept: no conversion (already m³/s)

This is required because gtopt reservoir volumes are in 10⁶ m³ while
plpralco.dat stores volumes in dam³ (= 10³ m³).  The factor 1000
is constant across all reservoirs, derived from the PLP unit chain:
  dam³ = raw_value × FEscala / 1E3
  gtopt_physical = raw_value × FEscala / 1E6
  ⇒ gtopt_physical = dam³ / 1E3

See also:
  parralco.f  in PLP CEN65/src/ – Fortran data structure
  genpdralco.f  in PLP CEN65/src/ – LP assembly
"""

import re
from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class RalcoParser(BaseParser):
    """Parser for plpralco.dat files containing drawdown limit data.

    The piecewise-linear segments define a volume-dependent upper bound
    on the stage-average hourly discharge from a reservoir:
      max_discharge [m³/s] = slope × volume [dam³] + intercept [m³/s]
    """

    @property
    def reservoir_discharge_limits(self) -> List[Dict[str, Any]]:
        """Return the parsed drawdown limit entries."""
        return self.get_all()

    @property
    def num_reservoir_discharge_limits(self) -> int:
        """Return the number of drawdown limit entries."""
        return len(self.reservoir_discharge_limits)

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plpralco.dat file and populate the data structure.

        Reads piecewise-linear drawdown limit curves from the PLP file format.
        No unit conversions are applied — values are already in dam³ and m³/s.
        """
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The plpralco.dat file is empty or malformed.")

        idx = 0

        # Reservoir name
        reservoir_name = self._parse_name(lines[idx])
        idx += 1

        if idx >= len(lines):
            raise ValueError("Unexpected end of plpralco.dat file (num_segments).")

        # Number of segments
        num_segments = self._parse_int(lines[idx])
        idx += 1

        segments: List[Dict[str, float]] = []
        for _ in range(num_segments):
            if idx >= len(lines):
                raise ValueError("Unexpected end of plpralco.dat file (segment data).")
            parts = lines[idx].split()
            if len(parts) < 3:
                raise ValueError(f"Segment line has too few fields: {lines[idx]}")
            # Format: volume_dam3  intercept_m3s  slope_per_dam3
            # Note: PLP uses Fortran 'd' notation for doubles
            # Volume conversion: plpralco.dat stores volumes in dam³,
            # but gtopt reservoir volumes are in 10^6 m³ (= dam³ / 1000).
            # This factor is constant across all reservoirs regardless
            # of energy_scale — see PLP leeemb.f unit derivation:
            #   dam³ = raw × FEscala / 1E3
            #   gtopt_physical = raw × FEscala / 1E6
            #   ⇒ gtopt_physical = dam³ / 1E3
            seg: Dict[str, float] = {
                "volume": self._parse_fortran_float(parts[0]) / 1000.0,
                "slope": self._parse_fortran_float(parts[2]) * 1000.0,
                "intercept": self._parse_fortran_float(parts[1]),
            }
            segments.append(seg)
            idx += 1

        entry: Dict[str, Any] = {
            "name": reservoir_name,
            "reservoir": reservoir_name,
            "segments": segments,
        }
        self._append(entry)

    @staticmethod
    def _parse_fortran_float(value: str) -> float:
        """Parse a Fortran-style float (handles 'd' exponent notation)."""
        return float(re.sub(r"[dD]", "e", value))

    def get_reservoir_discharge_limit_by_reservoir(
        self, reservoir_name: str
    ) -> Optional[Dict[str, Any]]:
        """Get drawdown limit data by reservoir name."""
        return self.get_item_by_name(reservoir_name)
