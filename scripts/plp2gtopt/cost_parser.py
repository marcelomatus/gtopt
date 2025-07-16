# -*- coding: utf-8 -*-

"""Parser for plpcosce.dat format files containing central cost data."""

import numpy as np

from .base_parser import BaseParser


class CostParser(BaseParser):
    """Parser for plpcosce.dat format files containing central cost data."""

    @property
    def costs(self):
        """Return the cost entries."""
        return self.get_all()

    @property
    def num_costs(self) -> int:
        """Return the number of cost entries in the file."""
        return len(self.costs)

    def parse(self) -> None:
        """Parse the cost file and populate the costs structure."""
        self.validate_file()

        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("The cost file is empty or malformed.")

            idx = self._next_idx(-1, lines)
            num_costs = self._parse_int(lines[idx])

            for _ in range(num_costs):
                # Get central name
                idx = self._next_idx(idx, lines)
                name = lines[idx].strip("'")

                # Get number of cost entries
                idx = self._next_idx(idx, lines)
                num_stages = self._parse_int(lines[idx].strip().split()[0])

                # Initialize numpy arrays for this central
                stages = np.empty(num_stages, dtype=np.int16)
                costs = np.empty(num_stages, dtype=np.float64)

                # Parse cost entries
                for i in range(num_stages):
                    idx = self._next_idx(idx, lines)
                    parts = lines[idx].split()
                    if len(parts) < 3:  # Expecting month, stage, cost
                        raise ValueError(f"Invalid cost entry at line {idx+1}")

                    stages[i] = self._parse_int(parts[1])  # Stage number
                    costs[i] = self._parse_float(parts[2])  # Cost value

                # Store complete data for the central cost
                cost = {"name": name, "stages": stages, "costs": costs}
                self._append(cost)

        finally:
            lines.clear()
            del lines

    def get_cost_by_name(self, name: str):
        """Get cost data for a specific central name."""
        return self.get_item_by_name(name)
