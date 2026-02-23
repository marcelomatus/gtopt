# -*- coding: utf-8 -*-

"""Parser for plpbess.dat format files containing BESS configuration data.

Handles:
- File parsing and validation
- BESS data structure creation
- BESS lookup by name or number

File format (tabular, 12 fields per row after stripping # comments):
  Num  Nombre    Barra  PMaxC  PMaxD   nc     nd   HrsReg  EtaIni  EtaFin  VolIni  NCiclos
"""

from pathlib import Path
from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class BessParser(BaseParser):
    """Parser for plpbess.dat format files containing BESS configuration."""

    @property
    def besses(self) -> List[Dict[str, Any]]:
        """Return the parsed BESS entries."""
        return self.get_all()

    @property
    def num_besses(self) -> int:
        """Return the number of BESS entries in the file."""
        return len(self.besses)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the BESS file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The BESS file is empty or malformed.")

        idx = 0
        num_besses = self._parse_int(lines[idx])
        idx += 1

        for _ in range(num_besses):
            if idx >= len(lines):
                raise ValueError("Unexpected end of BESS file.")

            line = lines[idx]
            idx += 1

            # Extract quoted name first, then parse remaining fields
            name_start = line.find("'")
            name_end = line.rfind("'")
            if name_start == -1 or name_end == name_start:
                raise ValueError(f"Invalid BESS name in line: {line}")

            name = line[name_start + 1 : name_end]
            before_name = line[:name_start].split()
            after_name = line[name_end + 1 :].split()

            if len(before_name) < 1:
                raise ValueError(f"Missing BESS number in line: {line}")

            number = self._parse_int(before_name[0])

            # after_name: Barra PMaxC PMaxD nc nd HrsReg EtaIni EtaFin VolIni NCiclos
            if len(after_name) < 10:
                raise ValueError(
                    f"Expected 10 fields after name, got {len(after_name)} in: {line}"
                )

            bus_number = self._parse_int(after_name[0])
            pmax_charge = self._parse_float(after_name[1])
            pmax_discharge = self._parse_float(after_name[2])
            nc = self._parse_float(after_name[3])
            nd = self._parse_float(after_name[4])
            hrs_reg = self._parse_float(after_name[5])
            eta_ini = self._parse_int(after_name[6])
            eta_fin = self._parse_int(after_name[7])
            vol_ini = self._parse_float(after_name[8])
            n_ciclos = self._parse_float(after_name[9])

            bess: Dict[str, Any] = {
                "number": number,
                "name": name,
                "bus": bus_number,
                "pmax_charge": pmax_charge,
                "pmax_discharge": pmax_discharge,
                "nc": nc,
                "nd": nd,
                "hrs_reg": hrs_reg,
                "eta_ini": eta_ini,
                "eta_fin": eta_fin,
                "vol_ini": vol_ini,
                "n_ciclos": n_ciclos,
            }
            self._append(bess)

    def get_bess_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get BESS data by name."""
        return self.get_item_by_name(name)

    def get_bess_by_number(self, number: int) -> Optional[Dict[str, Any]]:
        """Get BESS data by number."""
        return self.get_item_by_number(number)
