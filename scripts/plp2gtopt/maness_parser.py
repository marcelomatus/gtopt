# -*- coding: utf-8 -*-

"""Parser for plpmaness.dat format files containing ESS maintenance data.

File format (block-style, identical to plpmanbess.dat):

  # Numero de ESS con mantenimiento
   N
  # Nombre de la ESS
  'ESS_NAME'
  # Numero de Etapas con mantenimiento
    K
  # Mes  Etapa  PMaxC  PMaxD
     01    001    0.0    0.0
     ...
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
                raise ValueError(f"Missing stage count for ESS '{name}'.")
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
                        f"Unexpected end of maintenance entries for ESS '{name}'."
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

            self._append(
                {
                    "name": name,
                    "stage": stages,
                    "pmax_charge": pmax_charge,
                    "pmax_discharge": pmax_discharge,
                }
            )

    def get_maness_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get ESS maintenance data by ESS name."""
        return self.get_item_by_name(name)
