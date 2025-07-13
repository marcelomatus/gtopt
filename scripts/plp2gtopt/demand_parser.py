#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpdem.dat format files containing bus demand data.

Handles:
- File parsing and validation
- Demand data structure creation
- Bus demand lookup
"""

from pathlib import Path
from typing import Any, Optional, List, Dict, Union
import numpy as np
import numpy.typing as npt

from .base_parser import BaseParser


class DemandParser(BaseParser):
    """Parser for plpdem.dat format files containing bus demand data."""

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with demand file path.

        Args:
            file_path: Path to plpdem.dat format file (str or Path)
        """
        super().__init__(file_path)
        self.demands: List[Dict[str, Any]] = self._data  # Alias for _data
        self.demand_idx_map: Dict[str, int] = {}  # Maps bus names to indices

    def parse(self) -> None:
        """Parse the demand file and populate the demands structure.

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed

        Example:
            >>> parser = DemandParser("plpdem.dat")
            >>> parser.parse()
            >>> demands = parser.get_demands()
            >>> len(demands)
            2
        """
        self.validate_file()
        lines = self._read_non_empty_lines()

        idx = 0
        num_demands = self._parse_int(lines[idx])
        idx += 1

        try:
            for bus_number in range(1, num_demands):
                # Get bus name
                if idx >= len(lines):
                    raise ValueError("Unexpected end of file while parsing bus names")
                name = lines[idx].strip("'").split()[0]
                idx += 1

                # Get number of demand entries
                if idx >= len(lines):
                    raise ValueError(
                        "Unexpected end of file while parsing block counts"
                    )

                try:
                    num_blocks = self._parse_int(lines[idx].strip().split()[0])
                    idx += 1
                except (ValueError, IndexError) as e:
                    raise ValueError(
                        f"Invalid block count at line {idx+1}: {lines[idx]}"
                    ) from e

                # Initialize numpy arrays for this bus
                blocks = np.empty(num_blocks, dtype=np.int32)
                values = np.empty(num_blocks, dtype=np.float64)

                # Parse demand entries
                for i in range(num_blocks):
                    if idx >= len(lines):
                        raise ValueError(
                            "Unexpected end of file while parsing demand entries"
                        )

                    parts = lines[idx].split()
                    if len(parts) < 3:
                        raise ValueError(f"Invalid demand entry at line {idx+1}")

                    blocks[i] = int(parts[1])  # Block number
                    values[i] = float(parts[2])  # Demand value
                    idx += 1

                # Store complete data for this bus
                idx = len(self.demands)
                self.demands.append(
                    {
                        "number": bus_number,
                        "name": name,
                        "blocks": blocks,
                        "values": values,
                    }
                )
                self.demand_idx_map[name] = idx  # Store index for lookup
        finally:
            lines.clear()
            del lines

    def get_demands(self) -> List[Dict[str, Any]]:
        """Return the parsed demands structure with numpy arrays."""
        return self.demands

    @property
    def num_demands(self) -> int:
        """Return the number of bars in the file."""
        return len(self.demands)

    def get_demand_by_name(
        self, name: str
    ) -> Optional[Dict[str, Union[int, str, npt.NDArray]]]:
        """Get demand data for a specific bus name."""
        return (
            self._data[self.demand_idx_map[name]]
            if name in self.demand_idx_map
            else None
        )
