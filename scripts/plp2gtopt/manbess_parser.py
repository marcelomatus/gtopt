# -*- coding: utf-8 -*-

"""Parser for plpmanbess.dat format files containing BESS maintenance data.

Handles:
- File parsing and validation
- BESS maintenance data structure creation
- Maintenance lookup by BESS name

File format (block-style, same structure as plpmance.dat):
  Per BESS block:
    'BESS_NAME'
    N_stages
    Mes  Etapa  PMaxC  PMaxD
    ...

The maintenance overrides PMaxC and PMaxD per stage.
"""

from typing import Any, Dict, List, Optional

import numpy as np

from .base_parser import BaseParser


class ManbessParser(BaseParser):
    """Parser for plpmanbess.dat BESS maintenance schedule files."""

    @property
    def manbesses(self) -> List[Dict[str, Any]]:
        """Return the BESS maintenance entries."""
        return self.get_all()

    @property
    def num_manbesses(self) -> int:
        """Return the number of BESS maintenance entries."""
        return len(self.manbesses)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the BESS maintenance file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The BESS maintenance file is empty or malformed.")

        idx = 0
        num_besses = self._parse_int(lines[idx])
        idx += 1

        for _ in range(num_besses):
            if idx >= len(lines):
                raise ValueError("Unexpected end of BESS maintenance file.")

            # Get BESS name (quoted)
            name = self._parse_name(lines[idx])
            idx += 1

            # Get number of maintenance stages
            if idx >= len(lines):
                raise ValueError(f"Missing stage count for BESS '{name}'.")
            num_stages = self._parse_int(lines[idx].split()[0])
            idx += 1

            if num_stages <= 0:
                continue

            stages = np.empty(num_stages, dtype=np.int32)
            pmax_charge = np.empty(num_stages, dtype=np.float64)
            pmax_discharge = np.empty(num_stages, dtype=np.float64)

            for i in range(num_stages):
                if idx >= len(lines):
                    raise ValueError(
                        f"Unexpected end of maintenance entries for BESS '{name}'."
                    )
                parts = lines[idx].split()
                if len(parts) < 4:
                    raise ValueError(
                        f"Invalid maintenance entry at line {idx + 1}: {lines[idx]}"
                    )
                stages[i] = self._parse_int(parts[1])
                pmax_charge[i] = self._parse_float(parts[2])
                pmax_discharge[i] = self._parse_float(parts[3])
                idx += 1

            manbess: Dict[str, Any] = {
                "name": name,
                "stage": stages,
                "pmax_charge": pmax_charge,
                "pmax_discharge": pmax_discharge,
            }
            self._append(manbess)

    def get_manbess_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get BESS maintenance data by BESS name."""
        return self.get_item_by_name(name)
