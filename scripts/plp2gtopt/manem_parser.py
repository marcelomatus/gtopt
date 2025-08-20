# -*- coding: utf-8 -*-

"""Parser for plpmanem.dat format files containing reservoir maintenance data.

Handles:
- File parsing and validation
- Maintenance data structure creation
- Maintenance lookup by name
"""

from typing import Any, Dict, List, Optional
import numpy as np

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

                scale = central["vol_scale"] if central else 1.0

                # Get number of stages and intervals
                idx = self._next_idx(idx, lines)
                parts = lines[idx].split()
                num_stages = self._parse_int(parts[0])
                if num_stages <= 0:
                    continue

                # Initialize numpy arrays
                stages = np.empty(num_stages, dtype=np.int32)
                vmin = np.empty(num_stages, dtype=np.float64)
                vmax = np.empty(num_stages, dtype=np.float64)

                # Parse maintenance entries
                for i in range(num_stages):
                    idx = self._next_idx(idx, lines)
                    parts = lines[idx].split()
                    if len(parts) < 4:
                        raise ValueError(f"Invalid maintenance entry at line {idx+1}")

                    stages[i] = self._parse_int(parts[1])
                    vmin[i] = self._parse_float(parts[2]) * scale
                    vmax[i] = self._parse_float(parts[3]) * scale

                # Store complete data
                manem = {
                    "name": name,
                    "stage": stages,
                    "vmin": vmin,
                    "vmax": vmax,
                }
                self._append(manem)

        finally:
            lines.clear()

    def get_manem_by_name(self, name: str) -> Dict[str, Any] | None:
        """Get maintenance data for a specific reservoir name."""
        return self.get_item_by_name(name)
