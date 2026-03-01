# -*- coding: utf-8 -*-

"""Parser for plpmaness.dat format files containing ESS maintenance data.

File format (from PLP Fortran ``genpdess.f`` ``LeeManEss``):

  # header comment
  # header comment
   N
  # comment
  ESS_NAME               (quoted or unquoted)
  # comment
     K                   (number of blocks)
  # comment
     IBind  Emin  Emax  DCMin  DCMax  [DCMod]
     ...

Each data line has 5-6 fields: block index, minimum energy (MWh),
maximum energy (MWh), minimum charge/discharge power (MW),
maximum charge/discharge power (MW), optional charge mode.
"""

from typing import Any, Dict, List, Optional

import numpy as np

from .base_parser import BaseParser


class ManessParser(BaseParser):
    """Parser for plpmaness.dat ESS maintenance schedule files."""

    @property
    def manesses(self) -> List[Dict[str, Any]]:
        """Return the ESS maintenance entries."""
        return self.get_all()

    @property
    def num_manesses(self) -> int:
        """Return the number of ESS maintenance entries."""
        return len(self.manesses)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the ESS maintenance file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The ESS maintenance file is empty or malformed.")

        idx = 0
        num_esses = self._parse_int(lines[idx])
        idx += 1

        for _ in range(num_esses):
            if idx >= len(lines):
                raise ValueError("Unexpected end of ESS maintenance file.")

            name = self._parse_name(lines[idx])
            idx += 1

            if idx >= len(lines):
                raise ValueError(f"Missing block count for ESS '{name}'.")
            num_blocks = self._parse_int(lines[idx].split()[0])
            idx += 1

            if num_blocks <= 0:
                continue

            block_index = np.empty(num_blocks, dtype=np.int32)
            emin = np.empty(num_blocks, dtype=np.float64)
            emax = np.empty(num_blocks, dtype=np.float64)
            dcmin = np.empty(num_blocks, dtype=np.float64)
            dcmax = np.empty(num_blocks, dtype=np.float64)
            dcmod = np.empty(num_blocks, dtype=np.int32)

            for i in range(num_blocks):
                if idx >= len(lines):
                    raise ValueError(
                        f"Unexpected end of maintenance entries for ESS '{name}'."
                    )
                parts = lines[idx].split()
                if len(parts) < 5:
                    raise ValueError(
                        f"Invalid maintenance entry at line {idx + 1}: {lines[idx]}"
                    )
                # Fortran: READ(line,*) IBind, Emin, Emax, DCMin, DCMax[, DCMod]
                block_index[i] = self._parse_int(parts[0])
                emin[i] = self._parse_float(parts[1])
                emax[i] = self._parse_float(parts[2])
                dcmin[i] = self._parse_float(parts[3])
                dcmax[i] = self._parse_float(parts[4])
                dcmod[i] = self._parse_int(parts[5]) if len(parts) > 5 else 1
                idx += 1

            self._append(
                {
                    "name": name,
                    "block_index": block_index,
                    "emin": emin,
                    "emax": emax,
                    "dcmin": dcmin,
                    "dcmax": dcmax,
                    "dcmod": dcmod,
                }
            )

    def get_maness_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get ESS maintenance data by ESS name."""
        return self.get_item_by_name(name)
