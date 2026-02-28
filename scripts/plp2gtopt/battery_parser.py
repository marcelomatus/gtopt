# -*- coding: utf-8 -*-

"""Parser for plpcenbat.dat format files containing battery configuration data.

Handles:
- File parsing and validation
- Battery data structure creation
- Battery lookup by name or number

File format (tabular with comment lines starting with #):
  After stripping comments:
  NBaterias  MaxIny
  BatInd  BatNom          (index and name, name is NOT quoted)
  NIny                    (number of injection centrals)
  NomBatIny  FPC          (injection central name, charge loss factor; repeated NIny times)
  BatBar  FPD  BatEMin  BatEMax  (bus, discharge factor, min energy MWh, max energy MWh)
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class BatteryParser(BaseParser):
    """Parser for plpcenbat.dat format files containing battery configuration."""

    @property
    def batteries(self) -> List[Dict[str, Any]]:
        """Return the parsed battery entries."""
        return self.get_all()

    @property
    def num_batteries(self) -> int:
        """Return the number of battery entries in the file."""
        return len(self.batteries)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the plpcenbat.dat file and populate the data structure."""
        self.validate_file()

        lines = self._read_non_empty_lines()
        if not lines:
            raise ValueError("The battery file is empty or malformed.")

        idx = 0
        # First non-comment line: NBaterias MaxIny
        fields = lines[idx].split()
        if len(fields) < 1:
            raise ValueError("Missing battery count in plpcenbat.dat.")
        num_batteries = self._parse_int(fields[0])
        idx += 1

        for _ in range(num_batteries):
            if idx >= len(lines):
                raise ValueError("Unexpected end of battery file.")

            # Battery header: BatInd  BatNom (name is NOT quoted)
            fields = lines[idx].split(None, 1)
            if len(fields) < 2:
                raise ValueError(f"Invalid battery header line: {lines[idx]}")
            number = self._parse_int(fields[0])
            name = fields[1].strip().replace("'", "")  # Remove quotes if present
            idx += 1

            # NIny (number of injection centrals)
            if idx >= len(lines):
                raise ValueError(f"Missing injection count for battery '{name}'.")
            n_iny = self._parse_int(lines[idx].split()[0])
            idx += 1

            injections = []
            for _ in range(n_iny):
                if idx >= len(lines):
                    raise ValueError(f"Missing injection data for battery '{name}'.")
                inj_parts = lines[idx].split()
                if len(inj_parts) < 2:
                    raise ValueError(f"Invalid injection line: {lines[idx]}")
                inj_name = (
                    inj_parts[0].strip().replace("'", "")
                )  # Remove quotes if present
                inj_fpc = self._parse_float(inj_parts[1])
                injections.append({"name": inj_name, "fpc": inj_fpc})
                idx += 1

            # Last line: BatBar  FPD  BatEMin  BatEMax
            if idx >= len(lines):
                raise ValueError(f"Missing bus/capacity line for battery '{name}'.")
            params = lines[idx].split()
            if len(params) < 4:
                raise ValueError(
                    f"Expected 4 fields (BatBar FPD EMin EMax), "
                    f"got {len(params)}: {lines[idx]}"
                )
            bus = self._parse_int(params[0])
            fpd = self._parse_float(params[1])
            emin = self._parse_float(params[2])  # Fixed: was _parse_floatplp
            emax = self._parse_float(params[3])
            idx += 1

            battery: Dict[str, Any] = {
                "number": number,
                "name": name,
                "injections": injections,
                "bus": bus,
                "fpd": fpd,
                "emin": emin,
                "emax": emax,
            }
            self._append(battery)

    def get_battery_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get battery data by name."""
        return self.get_item_by_name(name)

    def get_battery_by_number(self, number: int) -> Optional[Dict[str, Any]]:
        """Get battery data by number."""
        return self.get_item_by_number(number)
