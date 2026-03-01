# -*- coding: utf-8 -*-

"""Parser for plpess.dat format files containing ESS configuration data.

Handles:
- File parsing and validation
- ESS data structure creation
- ESS lookup by name or number

File format (tabular, 6-8 fields per row after stripping # comments):
  Nombre  nd  nc  mloss  emax  dcmax  [dcmod]  [cenpc]

The first non-empty line contains the number of ESS entries.
Each subsequent line contains the fields for one ESS entry.

Field definitions (from PLP Fortran paress.f / genpdess.f LeeEss READ order):
  Nombre  – ESS name (must match a BAT central in plpcnfce.dat)
  nd      – discharge efficiency (p.u.)
  nc      – charge efficiency (p.u.)
  mloss   – monthly energy loss (% / month)
  emax    – maximum energy capacity (MWh)
  dcmax   – maximum discharge/charge power (MW)
  dcmod   – charge mode: 0 = standalone, 1 = coupled to cenpc (optional)
  cenpc   – primary charge central name (optional)

Note: The Fortran READ statement is ``CenNombre, nd, nc, mloss, Emax, DCMax,
DCMod, CenCarga`` (discharge efficiency first, then charge efficiency).  Some
PLP data file comments label the columns as "nc nd" which reverses the names,
but the Fortran reads nd first.
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
            if len(fields) < 6:
                raise ValueError(f"Missing ESS fields in line: {line}")

            # Fortran READ order: CenNombre, nd, nc, mloss, Emax, DCMax, ...
            name = fields[0].replace("'", "")
            nd = self._parse_float(fields[1])
            nc = self._parse_float(fields[2])
            mloss = self._parse_float(fields[3])
            emax = self._parse_float(fields[4])
            dcmax = self._parse_float(fields[5])
            dcmod = self._parse_int(fields[6]) if len(fields) > 6 else 0
            cenpc = fields[7].strip().replace("'", "") if len(fields) > 7 else ""

            ess: Dict[str, Any] = {
                "name": name,
                "nc": nc,
                "nd": nd,
                "mloss": mloss,
                "emax": emax,
                "dcmax": dcmax,
                "dcmod": dcmod,
                "cenpc": cenpc,
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
