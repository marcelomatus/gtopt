# -*- coding: utf-8 -*-

"""Parser for plpmanem.dat format files containing reservoir maintenance data.

Handles:
- File parsing and validation
- Maintenance data structure creation
- Maintenance lookup by name
"""

from typing import Any, Dict, List, Optional

from .base_parser import BaseParser


class ManemParser(BaseParser):
    """Parser for plpmanem.dat format files containing reservoir maintenance data."""

    @property
    def manems(self) -> List[Dict[str, Any]]:
        """Return the maintenance entries."""
        return self.get_all()

    @property
    def num_manems(self) -> int:
        """Return the number of maintenance entries in the file."""
        return len(self.manems)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the maintenance file and populate the data structure."""
        self.validate_file()

        central_parser = parsers["central_parser"] if parsers else None
        lines = []
        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("The maintenance file is empty or malformed.")

            idx = self._next_idx(-1, lines)
            num_reservoirs = self._parse_int(lines[idx])

            for _ in range(num_reservoirs):
                # Get reservoir name
                idx = self._next_idx(idx, lines)
                name = lines[idx].strip("'")

                central = (
                    central_parser.get_central_by_name(name) if central_parser else None
                )

                scale = central["energy_scale"] if central else 1.0

                # Get number of stages and intervals
                idx = self._next_idx(idx, lines)
                parts = lines[idx].split()
                num_stages = self._parse_int(parts[0])
                if num_stages <= 0:
                    continue

                # Vectorized parse: col 1=stage, col 2=emin, col 3=emax
                next_idx, cols = self._parse_numeric_block(
                    lines,
                    idx + 1,
                    num_stages,
                    int_cols=(1,),
                    float_cols=(2, 3),
                )
                idx = next_idx - 1
                stages = cols[1]
                emin = cols[2] * scale
                emax = cols[3] * scale

                # Store complete data
                manem = {
                    "name": name,
                    "stage": stages,
                    "emin": emin,
                    "emax": emax,
                }
                self._append(manem)

        finally:
            lines.clear()

    def get_manem_by_name(self, name: str) -> Dict[str, Any] | None:
        """Get maintenance data for a specific reservoir name."""
        return self.get_item_by_name(name)
