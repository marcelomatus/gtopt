# -*- coding: utf-8 -*-

"""Parser for plpmanbat.dat format files containing battery maintenance data.

Handles:
- File parsing and validation
- Battery maintenance data structure creation
- Maintenance lookup by battery name

File format (block-style, same structure as plpmanbess.dat except names are NOT quoted):
  N_batteries
  BATTERY_NAME     (NOT quoted, unlike manbess)
  N_stages
  Mes  Etapa  PMaxC  PMaxD
  ...

The maintenance overrides PMaxC and PMaxD per stage.
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

            # Get battery name (NOT quoted, unlike plpmanbess.dat)
            name = lines[idx].strip()
            idx += 1

            # Get number of maintenance stages
            if idx >= len(lines):
                raise ValueError(f"Missing stage count for battery '{name}'.")
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
                        f"Unexpected end of maintenance entries for battery '{name}'."
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

            manbat: Dict[str, Any] = {
                "name": name,
                "stage": stages,
                "pmax_charge": pmax_charge,
                "pmax_discharge": pmax_discharge,
            }
            self._append(manbat)

    def get_manbat_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get battery maintenance data by battery name."""
        return self.get_item_by_name(name)
