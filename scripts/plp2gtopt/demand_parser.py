# -*- coding: utf-8 -*-

"""Parser for plpdem.dat format files containing bus demand data.

Handles:
- File parsing and validation
- Demand data structure creation
- Bus demand lookup
"""

from typing import Any, Optional

import numpy as np
from .base_parser import BaseParser


class DemandParser(BaseParser):
    """Parser for plpdem.dat format files containing bus demand data."""

    @property
    def demands(self):
        """Return the demand entries."""
        return self.get_all()

    @property
    def num_demands(self) -> int:
        """Return the number of demand entries in the file."""
        return len(self.demands)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the demand file and populate the demands structure."""
        self.validate_file()

        bus_parser = parsers["bus_parser"] if parsers else None
        block_parser = parsers["block_parser"] if parsers else None
        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("The demand file is empty or malformed.")

            idx = self._next_idx(-1, lines)
            num_dems = self._parse_int(lines[idx])
            for dem_number in range(1, num_dems + 1):
                # Get bus name
                idx = self._next_idx(idx, lines)
                name = lines[idx].strip("'").split()[0]
                bus = bus_parser.get_item_by_name(name) if bus_parser else None

                # Get number of demand entries
                idx = self._next_idx(idx, lines)
                num_blocks = self._parse_int(lines[idx].strip().split()[0])

                # Initialize numpy arrays for this bus
                blocks = np.empty(num_blocks, dtype=np.int32)
                values = np.empty(num_blocks, dtype=np.float64)

                # Parse demand entries
                demand_energy = 0.0
                energies: dict[int, float] = {}
                for i in range(num_blocks):
                    idx = self._next_idx(idx, lines)
                    parts = lines[idx].split()
                    if len(parts) < 3:
                        raise ValueError(f"Invalid demand entry at line {idx + 1}")

                    blocks[i] = self._parse_int(parts[1])  # Block number
                    values[i] = self._parse_float(parts[2])  # Demand value

                    block = (
                        block_parser.get_item_by_number(blocks[i])
                        if block_parser
                        else None
                    )
                    bdur = block.get("duration", 0.0) if block else 0.0
                    bener = values[i] * bdur
                    demand_energy += bener

                    stage = (
                        block_parser.get_stage_number(blocks[i]) if block_parser else -1
                    )
                    if stage >= 0:
                        energies[stage] = energies.get(stage, 0.0) + bener

                # Store complete data for this bus
                if demand_energy >= 0.0:
                    demand = {
                        "number": bus["number"] if bus else dem_number,
                        "name": name,
                        "blocks": blocks,
                        "values": values,
                        "stages": energies.keys(),
                        "energies": energies.values(),
                    }
                    self._append(demand)

        finally:
            lines.clear()

    def get_demand_by_name(self, name):
        """Get demand data for a specific bus name."""
        return self.get_item_by_name(name)
