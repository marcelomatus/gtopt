# -*- coding: utf-8 -*-

"""Parser for plpess.dat format files containing ESS configuration data.

Handles:
- File parsing and validation
- ESS data structure creation
- ESS lookup by name or number

File format (tabular, 9 fields per row after stripping # comments):
  Num  Nombre  Barra  PMaxC  PMaxD  nc  nd  HrsReg  VolIni

The first non-empty line contains the number of ESS entries.
Each subsequent line contains the fields for one ESS entry.
"""

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

    def parse(self, parsers: Optional[Dict[str, Any]] = None) -> None:
        """Parse the ESS file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The ESS file is empty or malformed.")

        idx = 0
        num_esses = self._parse_int(lines[idx])
        idx += 1

        if num_esses < 0:
            raise ValueError(
                f"Invalid number of ESS entries: {num_esses}. Must be non-negative."
            )

        for _ in range(num_esses):
            if idx >= len(lines):
                raise ValueError("Unexpected end of ESS file.")

            line = lines[idx]
            idx += 1

            fields = line.split()
            if len(fields) < 9:
                raise ValueError(f"Missing ESS fields in line: {line}")

            number = self._parse_int(fields[0])
            name = fields[1].replace("'", "")
            bus = self._parse_int(fields[2])
            pmax_charge = self._parse_float(fields[3])
            pmax_discharge = self._parse_float(fields[4])
            nc = self._parse_float(fields[5])
            nd = self._parse_float(fields[6])
            hrs_reg = self._parse_float(fields[7])
            vol_ini = self._parse_float(fields[8])

            ess: Dict[str, Any] = {
                "number": number,
                "name": name,
                "bus": bus,
                "pmax_charge": pmax_charge,
                "pmax_discharge": pmax_discharge,
                "nc": nc,
                "nd": nd,
                "hrs_reg": hrs_reg,
                "vol_ini": vol_ini,
            }
            self._append(ess)

        if self.num_esses != num_esses:
            raise ValueError(
                f"Expected {num_esses} ESS entries but parsed {self.num_esses}."
            )

    def get_ess_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get ESS data by name."""
        return self.get_item_by_name(name)

    def get_ess_by_number(self, number: int) -> Optional[Dict[str, Any]]:
        """Get ESS data by number."""
        return self.get_item_by_number(number)
