"""Parser for plpaflce.dat format files containing hydro flow data.

Handles:
- File parsing and validation
- Flow data structure creation
- Flow lookup by central name
"""

from typing import List, Optional, Dict, Any
import numpy as np


from .base_parser import BaseParser
from .central_parser import CentralParser


class AflceParser(BaseParser):
    """Parser for plpaflce.dat format files containing hyyro flow data."""

    @property
    def flows(self) -> List[Dict[str, Any]]:
        """Get all flow entries."""
        return self.get_all()

    @property
    def num_flows(self) -> int:
        """Get the number of flow entries."""
        return len(self.flows)

    def parse(self, parsers: Optional[dict[str, Any]] = None) -> None:
        """Parse the flow file and populate the data structure.

        Raises:
            ValueError: If file is empty, malformed or contains invalid data.
        """
        self.validate_file()

        central_parser: CentralParser | None = (
            parsers["central_parser"] if parsers else None
        )
        try:
            lines = self._read_non_empty_lines()
            if not lines:
                raise ValueError("The flow file is empty or malformed")

            # Parse header line
            idx = self._next_idx(-1, lines)
            header_parts = lines[idx].split()
            if len(header_parts) < 2:
                raise ValueError(f"Invalid header line at line {idx + 1}: {lines[idx]}")

            num_centrals = self._parse_int(header_parts[0])
            num_hydrologies = self._parse_int(header_parts[1])

            if num_hydrologies <= 0:
                raise ValueError(
                    f"Invalid counts - centrals: {num_centrals}, "
                    f"hydrologies: {num_hydrologies}"
                )

            if num_centrals == 0:
                return  # No stochastic hydrology data – valid for thermal-only cases

            # Parse each central's flow data
            for _ in range(num_centrals):
                # Get central name
                idx = self._next_idx(idx, lines)
                name = self._parse_name(lines[idx])
                central = (
                    central_parser.get_central_by_name(name) if central_parser else None
                )

                # Get number of blocks
                idx = self._next_idx(idx, lines)
                num_blocks = self._parse_int(lines[idx])
                if num_blocks <= 0:
                    continue  # Skip centrals with no blocks

                # Initialize numpy arrays with optimal types
                blocks = np.empty(num_blocks, dtype=np.int32)
                flows = np.empty((num_blocks, num_hydrologies), dtype=np.float64)

                # Parse each block's flow data
                for block_idx in range(num_blocks):
                    idx = self._next_idx(idx, lines)
                    parts = lines[idx].split()

                    if len(parts) < 2 + num_hydrologies:
                        raise ValueError(
                            f"Invalid flow entry at line {idx + 1}: "
                            f"expected {2 + num_hydrologies} values, got {len(parts)}"
                        )

                    blocks[block_idx] = self._parse_int(parts[1])  # Block number
                    flows[block_idx] = [
                        self._parse_float(v) for v in parts[2 : 2 + num_hydrologies]
                    ]

                # scale flows if central is of type 'pasada'
                if central and central["type"] == "pasada":
                    central_pmax = central.get("pmax", 0.0)
                    central_pmax = max(central_pmax, np.max(flows))
                    central["pmax"] = central_pmax
                    flows = flows / central_pmax if central_pmax > 0 else flows

                # Store complete data
                self._append(
                    {
                        "name": name,
                        "block": blocks,
                        "flow": flows,
                        "num_hydrologies": num_hydrologies,
                    }
                )

        finally:
            # Clean up memory
            lines.clear()

    def get_flow_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        """Get flow data for a specific central name."""
        return self.get_item_by_name(name)
