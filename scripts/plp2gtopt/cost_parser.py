# -*- coding: utf-8 -*-

"""Parser for plpcosce.dat format files containing central cost data."""

from pathlib import Path
from typing import Optional, Dict, Union
import numpy as np
import numpy.typing as npt

from .base_parser import BaseParser


class CostParser(BaseParser):
    """Parser for plpcosce.dat format files containing central cost data."""

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with cost file path.

        Args:
            file_path: Path to plpcosce.dat format file (str or Path)
        """
        super().__init__(file_path)
        self.cost_idx_map: Dict[str, int] = {}  # Maps central names to indices

    @property
    def costs(self):
        """Return the cost entries."""
        return self._data

    @property
    def num_costs(self) -> int:
        """Return the number of cost entries in the file."""
        return len(self._data)

    def parse(self) -> None:
        """Parse the cost file and populate the costs structure.

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed
        """
        self.validate_file()
        lines = self._read_non_empty_lines()

        idx = 0
        self.num_centrals = self._parse_int(lines[idx])
        idx += 1

        try:
            for _ in range(self.num_centrals):
                # Get central name
                if idx >= len(lines):
                    raise ValueError(
                        "Unexpected end of file while parsing central names"
                    )
                name = lines[idx].strip("'").split("#")[0].strip()
                idx += 1

                # Get number of cost entries
                if idx >= len(lines):
                    raise ValueError(
                        "Unexpected end of file while parsing stage counts"
                    )

                # Skip empty lines between name and stage count
                while idx < len(lines) and not lines[idx].strip():
                    idx += 1

                try:
                    num_stages = int(lines[idx].strip().split()[0])
                    idx += 1
                except (ValueError, IndexError) as e:
                    raise ValueError(
                        f"Invalid stage count at line {idx+1}: {lines[idx]}"
                    ) from e

                # Skip empty/comment lines until we find the cost entries
                while idx < len(lines) and (
                    not lines[idx].strip() or lines[idx].strip().startswith("#")
                ):
                    idx += 1

                # Initialize numpy arrays for this central
                stages = np.empty(num_stages, dtype=np.int16)
                costs = np.empty(num_stages, dtype=np.float64)

                # Parse cost entries
                for i in range(num_stages):
                    if idx >= len(lines):
                        raise ValueError(
                            "Unexpected end of file while parsing cost entries"
                        )

                    # Skip empty/comment lines
                    while idx < len(lines) and (
                        not lines[idx].strip() or lines[idx].strip().startswith("#")
                    ):
                        idx += 1
                    if idx >= len(lines):
                        raise ValueError(
                            "Unexpected end of file while parsing cost entries"
                        )

                    parts = lines[idx].split()
                    if len(parts) < 3:  # Expecting month, stage, cost
                        raise ValueError(
                            f"Invalid cost entry at line {idx+1}: expected"
                            "3 values, got {len(parts)}"
                        )

                    stages[i] = int(parts[1])  # Stage number is second column
                    costs[i] = float(parts[2])  # Cost is third column
                    idx += 1

                # Store complete data for this central
                self._data.append({"name": name, "stages": stages, "costs": costs})
                self.cost_idx_map[name] = len(self._data) - 1
        finally:
            lines.clear()
            del lines

    def get_cost_by_name(
        self, name: str
    ) -> Optional[Dict[str, Union[str, npt.NDArray]]]:
        """Get cost data for a specific central name."""
        return (
            self._data[self.cost_idx_map.get(name)]
            if name in self.cost_idx_map
            else None
        )
