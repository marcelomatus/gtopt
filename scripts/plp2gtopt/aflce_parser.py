# -*- coding: utf-8 -*-

"""Parser for plpaflce.dat format files containing hydro flow data.

Handles:
- File parsing and validation
- Flow data structure creation
- Flow lookup by central name
"""

from typing import Union, Dict, List
import numpy as np

from .base_parser import BaseParser


class AflceParser(BaseParser):
    """Parser for plpaflce.dat format files containing hydro flow data."""

    @property
    def flows(self) -> List[Dict]:
        """Return the flow entries."""
        return self.get_all()

    @property
    def num_flows(self) -> int:
        """Return the number of flow entries in the file."""
        return len(self.flows)

    def parse(self) -> None:
        """Parse the flow file and populate the data structure."""
        self.validate_file()

        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("The flow file is empty or malformed.")

            idx = self._next_idx(-1, lines)
            # First line: num_centrals num_hydrologies
            parts = lines[idx].split()
            num_centrals = self._parse_int(parts[0])
            num_hydrologies = self._parse_int(parts[-1])  # Last value is hydrologies
            idx += 1

            for _ in range(num_centrals):
                # Get central name
                idx = self._next_idx(idx, lines)
                name = lines[idx].strip("'")
                idx += 1

                # Get number of blocks
                idx = self._next_idx(idx, lines)
                num_blocks = self._parse_int(lines[idx])
                idx += 1

                # Initialize numpy arrays
                blocks = np.empty(num_blocks, dtype=np.int16)
                flows = np.empty((num_blocks, num_hydrologies), dtype=np.float64)

                # Parse flow entries
                for i in range(num_blocks):
                    idx = self._next_idx(idx, lines)
                    parts = lines[idx].split()
                    if len(parts) < 2 + num_hydrologies:
                        raise ValueError(f"Invalid flow entry at line {idx+1}")

                    blocks[i] = self._parse_int(parts[1])  # Block number
                    flows[i] = [self._parse_float(v) for v in parts[2:2+num_hydrologies]]

                    idx += 1

                # Store complete data
                flow = {
                    "name": name,
                    "blocks": blocks,
                    "flows": flows,
                    "num_hydrologies": num_hydrologies,
                }
                self._append(flow)

        finally:
            lines.clear()
            del lines

    def get_flow_by_name(self, name: str) -> Union[Dict, None]:
        """Get flow data for a specific central name."""
        return self.get_item_by_name(name)
