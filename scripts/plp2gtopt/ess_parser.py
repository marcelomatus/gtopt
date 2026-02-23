# -*- coding: utf-8 -*-

"""Parser for plpess.dat format files containing ESS configuration data.

Handles:
- File parsing and validation
- ESS data structure creation
- ESS lookup by name or number

File format (tabular, 9 fields per row after stripping # comments):
  Num  Nombre    Barra  PMaxC  PMaxD   nc     nd   HrsReg  VolIni

ESS differs from BESS in that it has no EtaIni/EtaFin/NCiclos fields
and is always active across all stages.
"""

from pathlib import Path
from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class EssParser(BaseParser):
    """Parser for plpess.dat format files containing ESS configuration."""

    @property
    def esses(self) -> List[Dict[str, Any]]:
        """Return the parsed ESS entries."""
        return self.get_all()

    @property
    def num_esses(self) -> int:
        """Return the number of ESS entries in the file."""
        return len(self.esses)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the ESS file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The ESS file is empty or malformed.")

        idx = 0
        num_esses = self._parse_int(lines[idx])
        idx += 1

        for _ in range(num_esses):
            if idx >= len(lines):
                raise ValueError("Unexpected end of ESS file.")

            line = lines[idx]
            idx += 1

            # Extract quoted name, then parse remaining fields
            name_start = line.find("'")
            name_end = line.rfind("'")
            if name_start == -1 or name_end == name_start:
                raise ValueError(f"Invalid ESS name in line: {line}")

            name = line[name_start + 1 : name_end]
            before_name = line[:name_start].split()
            after_name = line[name_end + 1 :].split()

            if len(before_name) < 1:
                raise ValueError(f"Missing ESS number in line: {line}")

            number = self._parse_int(before_name[0])

            # after_name: Barra PMaxC PMaxD nc nd HrsReg VolIni
            if len(after_name) < 7:
                raise ValueError(
                    f"Expected 7 fields after name, got {len(after_name)} in: {line}"
                )

            bus_number = self._parse_int(after_name[0])
            pmax_charge = self._parse_float(after_name[1])
            pmax_discharge = self._parse_float(after_name[2])
            nc = self._parse_float(after_name[3])
            nd = self._parse_float(after_name[4])
            hrs_reg = self._parse_float(after_name[5])
            vol_ini = self._parse_float(after_name[6])

            ess: Dict[str, Any] = {
                "number": number,
                "name": name,
                "bus": bus_number,
                "pmax_charge": pmax_charge,
                "pmax_discharge": pmax_discharge,
                "nc": nc,
                "nd": nd,
                "hrs_reg": hrs_reg,
                "vol_ini": vol_ini,
            }
            self._append(ess)

    def get_ess_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get ESS data by name."""
        return self.get_item_by_name(name)

    def get_ess_by_number(self, number: int) -> Optional[Dict[str, Any]]:
        """Get ESS data by number."""
        return self.get_item_by_number(number)
