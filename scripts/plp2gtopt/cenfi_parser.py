# -*- coding: utf-8 -*-

"""Parser for plpcenfi.dat format files containing reservoir filtration data.

Handles:
- File parsing and validation
- Filtration data structure creation
- Lookup by central name

File format (plpcenfi.dat - Archivo de Centrales Filtración):
  # Numero de centrales de filtracion
  N
  # For each entry:
  # Nombre de Central (source waterway)
  'CENTRAL_NAME'
  # Nombre del Embalse (receiving reservoir)
  'EMBALSE_NAME'
  # Pendiente [m³/s / dam³]  Constante [m³/s]
  slope  constant

Field definitions:
  CENTRAL_NAME  – Central/waterway name (must match a central in plpcnfce.dat)
  EMBALSE_NAME  – Receiving reservoir name (matches a reservoir in plpcnfce.dat)
  slope         – Seepage rate proportional to reservoir volume [m³/s / dam³]
  constant      – Constant seepage rate independent of volume [m³/s]

The filtration seepage flow per block is modelled as:
  seepage [m³/s] = slope × avg_reservoir_volume [dam³] + constant [m³/s]

where avg_reservoir_volume = (eini + efin) / 2.  This captures the hydrostatic
head dependence of soil seepage (Darcy's law approximation).
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class CenfiParser(BaseParser):
    """Parser for plpcenfi.dat files containing reservoir filtration data."""

    @property
    def filtrations(self) -> List[Dict[str, Any]]:
        """Return the parsed filtration entries."""
        return self.get_all()

    @property
    def num_filtrations(self) -> int:
        """Return the number of filtration entries."""
        return len(self.filtrations)

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the plpcenfi.dat file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The plpcenfi.dat file is empty or malformed.")

        idx = 0
        num_entries = self._parse_int(lines[idx])
        idx += 1

        if num_entries < 0:
            raise ValueError(
                f"Invalid number of filtration entries: {num_entries}."
                " Must be non-negative."
            )

        for _ in range(num_entries):
            if idx >= len(lines):
                raise ValueError("Unexpected end of plpcenfi.dat file.")

            # Central name (waterway source)
            central_name = self._parse_name(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError("Unexpected end of plpcenfi.dat file (reservoir).")

            # Reservoir name (seepage target)
            reservoir_name = self._parse_name(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError(
                    "Unexpected end of plpcenfi.dat file (slope/constant)."
                )

            # Slope and constant on the same line
            parts = lines[idx].split()
            if len(parts) < 2:
                raise ValueError(
                    f"Filtration slope/constant line has too few fields: {lines[idx]}"
                )
            slope = self._parse_float(parts[0])
            constant = self._parse_float(parts[1])
            idx += 1

            entry: Dict[str, Any] = {
                "name": central_name,
                "reservoir": reservoir_name,
                "slope": slope,
                "constant": constant,
            }
            self._append(entry)

        if self.num_filtrations != num_entries:
            raise ValueError(
                f"Expected {num_entries} filtration entries but parsed"
                f" {self.num_filtrations}."
            )

    def get_filtration_by_central(self, central_name: str) -> Optional[Dict[str, Any]]:
        """Get filtration data by central (waterway source) name."""
        return self.get_item_by_name(central_name)
