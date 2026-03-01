# -*- coding: utf-8 -*-

"""Parser for plpmanbat.dat format files containing battery maintenance data.

Handles:
- File parsing and validation
- Battery maintenance data structure creation
- Maintenance lookup by battery name

File format (from PLP Fortran ``genpdbaterias.f`` ``LeeManBat``):
  N_batteries
  BATTERY_NAME     (NOT quoted)
  N_blocks
  IBind  EMin  EMax
  ...

Each data line has 3 fields: block index, minimum energy (MWh),
maximum energy (MWh).  A value of -1 means "keep the default".

The maintenance overrides BatEMin and BatEMax per block in PLP.
In gtopt these map to Battery ``vmin`` and ``vmax`` schedules (normalised
to capacity).
"""

from typing import Any, Dict, List, Optional

import numpy as np

from .base_parser import BaseParser


class ManbatParser(BaseParser):
    """Parser for plpmanbat.dat battery maintenance schedule files."""

    @property
    def manbats(self) -> List[Dict[str, Any]]:
        """Return the battery maintenance entries."""
        return self.get_all()

    @property
    def num_manbats(self) -> int:
        """Return the number of battery maintenance entries."""
        return len(self.manbats)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the battery maintenance file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The battery maintenance file is empty or malformed.")

        idx = 0
        num_batteries = self._parse_int(lines[idx])
        idx += 1

        for _ in range(num_batteries):
            if idx >= len(lines):
                raise ValueError("Unexpected end of battery maintenance file.")

            # Get battery name (NOT quoted)
            name = lines[idx].strip()
            idx += 1

            # Get number of maintenance blocks
            if idx >= len(lines):
                raise ValueError(f"Missing block count for battery '{name}'.")
            num_blocks = self._parse_int(lines[idx].split()[0])
            idx += 1

            if num_blocks <= 0:
                continue

            block_index = np.empty(num_blocks, dtype=np.int32)
            emin = np.empty(num_blocks, dtype=np.float64)
            emax = np.empty(num_blocks, dtype=np.float64)

            for i in range(num_blocks):
                if idx >= len(lines):
                    raise ValueError(
                        f"Unexpected end of maintenance entries for battery '{name}'."
                    )
                parts = lines[idx].split()
                if len(parts) < 3:
                    raise ValueError(
                        f"Invalid maintenance entry at line {idx + 1}: {lines[idx]}"
                    )
                # Fortran: READ(URead, *) IBind, EMin, EMax
                block_index[i] = self._parse_int(parts[0])
                emin[i] = self._parse_float(parts[1])
                emax[i] = self._parse_float(parts[2])
                idx += 1

            self._append(
                {
                    "name": name,
                    "block_index": block_index,
                    "emin": emin,
                    "emax": emax,
                }
            )

    def get_manbat_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get battery maintenance data by battery name."""
        return self.get_item_by_name(name)
