# -*- coding: utf-8 -*-

"""Parser for plpcenre.dat format files containing reservoir efficiency data.

Handles:
- File parsing and validation
- Reservoir efficiency data structure creation
- Lookup by central or reservoir name

File format (plpcenre.dat - Archivo de Rendimiento de Embalses):
  # Número de Embalses con Rendimiento
  N
  # For each entry:
  # Nombre de Central
  'CENTRAL_NAME'
  # Nombre del Embalse
  'EMBALSE_NAME'
  # Rendimiento Medio
  mean_production_factor
  # Número de Tramos
  num_segments
  # For each segment:
  # Tramo   Volumen   Pendiente   Constante   F.Escala
  idx       volume    slope       constant    scale

Field definitions:
  CENTRAL_NAME  – Central/turbine name (must match a central in plpcnfce.dat)
  EMBALSE_NAME  – Reservoir name (must match a reservoir in plpcnfce.dat)
  mean_production_factor – Mean/fallback efficiency value [MW·s/m³]
  num_segments    – Number of piecewise-linear segments
  idx             – Segment index (1-based, informational)
  volume          – Volume breakpoint [raw]; converted to hm³ via × FEscala / 1E6
  slope           – Piecewise-linear slope [efficiency per hm³]
  constant        – Efficiency at the volume breakpoint [MW·s/m³]
  scale (FEscala) – Per-segment physical scale for the volume breakpoint

Unit conversions (matching PLP Fortran ``plp-leecenre.f``):
  volume = volume_raw × FEscala / 1E6  [hm³] (same as gtopt reservoir volumes)
  slope is kept as-is [efficiency per hm³]
  constant is kept as-is [MW·s/m³]

The efficiency at a given reservoir volume V is (concave-envelope minimum,
matching PLP Fortran ``FRendimientos`` in ``plp-frendim.f``):
  efficiency(V) = min_i { constant_i + slope_i × (V − volume_i) }

Note: Unlike seepage where ``constant`` is the y-intercept (value at V=0),
here ``constant`` is the efficiency value **at the breakpoint** (point-slope
form).  This is the native PLP convention for rendimientos.
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class CenreParser(BaseParser):
    """Parser for plpcenre.dat files containing reservoir efficiency data."""

    @property
    def efficiencies(self) -> List[Dict[str, Any]]:
        """Return the parsed reservoir efficiency entries."""
        return self.get_all()

    @property
    def num_efficiencies(self) -> int:
        """Return the number of efficiency entries."""
        return len(self.efficiencies)

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plpcenre.dat file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The plpcenre.dat file is empty or malformed.")

        idx = 0
        num_entries = self._parse_int(lines[idx])
        idx += 1

        if num_entries < 0:
            raise ValueError(
                f"Invalid number of efficiency entries: {num_entries}."
                " Must be non-negative."
            )

        for _ in range(num_entries):
            if idx >= len(lines):
                raise ValueError("Unexpected end of plpcenre.dat file.")

            # Central name (turbine)
            central_name = self._parse_name(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError("Unexpected end of plpcenre.dat file (reservoir).")

            # Reservoir name
            reservoir_name = self._parse_name(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError(
                    "Unexpected end of plpcenre.dat file (mean efficiency)."
                )

            # Mean efficiency
            mean_production_factor = self._parse_float(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError("Unexpected end of plpcenre.dat file (num_segments).")

            # Number of segments
            num_segments = self._parse_int(lines[idx])
            idx += 1

            segments: List[Dict[str, float]] = []
            for _ in range(num_segments):
                if idx >= len(lines):
                    raise ValueError(
                        "Unexpected end of plpcenre.dat file (segment data)."
                    )
                parts = lines[idx].split()
                if len(parts) < 4:
                    raise ValueError(f"Segment line has too few fields: {lines[idx]}")
                # Format: idx volume slope constant [FEscala]
                # FEscala is a per-segment physical scale for volume breakpoints.
                # Conversion: volume = raw_volume × FEscala / 1E6 [hm³]
                # Slope is in [efficiency per hm³] — no conversion needed.
                fescala = self._parse_float(parts[4]) if len(parts) >= 5 else 1.0e6
                seg = {
                    "volume": self._parse_float(parts[1]) * fescala / 1.0e6,
                    "slope": self._parse_float(parts[2]),
                    "constant": self._parse_float(parts[3]),
                }
                segments.append(seg)
                idx += 1

            entry: Dict[str, Any] = {
                "name": central_name,
                "reservoir": reservoir_name,
                "mean_production_factor": mean_production_factor,
                "segments": segments,
            }
            self._append(entry)

        if self.num_efficiencies != num_entries:
            raise ValueError(
                f"Expected {num_entries} efficiency entries but parsed"
                f" {self.num_efficiencies}."
            )

    def get_efficiency_by_central(self, central_name: str) -> Optional[Dict[str, Any]]:
        """Get efficiency data by central (turbine) name."""
        return self.get_item_by_name(central_name)
