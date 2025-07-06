#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpdem.dat format files containing bus demand data.

Handles:
- File parsing and validation
- Demand data structure creation
- Bus demand lookup
"""

import sys
from pathlib import Path
from typing import Any, Optional, List, Dict, Union, Tuple
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
        self._data: List[Dict[str, Any]] = []
        self.demand_blocks: npt.NDArray[np.int32] = np.array([], dtype=np.int32)
        self.demand_values: npt.NDArray[np.float64] = np.array([], dtype=np.float64)
        self.demand_indices: List[Tuple[int, int]] = []  # (start, end) indices per bus
        self.num_demands: int = 0

    def parse(self) -> None:
        """Parse the demand file and populate the demands structure.

        Uses a two-pass approach:
        1. First pass counts total demand entries to properly size arrays
        2. Second pass reads data directly into pre-allocated numpy arrays

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed
        """
        self.validate_file()
        lines = self._read_non_empty_lines()

        idx = 0
        self.num_demands = self._parse_int(lines[idx])
        idx += 1

        # First pass: count total demand entries to pre-allocate arrays
        total_demand_entries = 0
        count_idx = idx  # Temporary index for counting pass

        for _ in range(self.num_demands):
            count_idx += 1  # Skip name line
            if count_idx >= len(lines):
                raise ValueError("Unexpected end of file while counting demand entries")

            num_blocks = int(lines[count_idx])
            total_demand_entries += num_blocks
            count_idx += num_blocks + 1  # Skip demand entries

        if total_demand_entries == 0:
            raise ValueError("No demand entries found in file")

        # Pre-allocate numpy arrays
        self.demand_blocks = np.empty(total_demand_entries, dtype=np.int32)
        self.demand_values = np.empty(total_demand_entries, dtype=np.float64)

        # Second pass: parse data directly into arrays
        array_pos = 0  # Current position in pre-allocated arrays
        bus_number = 1  # 1-based bus numbering

        try:
            for _ in range(self.num_demands):
                # Get bus name
                if idx >= len(lines):
                    raise ValueError("Unexpected end of file while parsing bus names")
                name = lines[idx].strip("'").split("#")[0].strip()
                idx += 1

                # Get number of demand entries
                if idx >= len(lines):
                    raise ValueError("Unexpected end of file while parsing block counts")
            
                # Skip any empty lines between bus name and block count
                while idx < len(lines) and not lines[idx].strip():
                    idx += 1
                
                try:
                    num_blocks = int(lines[idx].strip().split()[0])  # Take first number only
                    idx += 1
                except (ValueError, IndexError) as e:
                    raise ValueError(
                        f"Invalid block count at line {idx+1}: {lines[idx]}"
                    ) from e

                # Record start index for this bus
                start_idx = array_pos

                # Parse demand entries directly into arrays
                for _ in range(num_blocks):
                if idx >= len(lines):
                    raise ValueError(
                        "Unexpected end of file while parsing demand entries"
                    )

                parts = lines[idx].split()
                if len(parts) < 3:
                    raise ValueError(f"Invalid demand entry at line {idx+1}")

                self.demand_blocks[array_pos] = int(parts[1])  # Block number
                self.demand_values[array_pos] = float(parts[2])  # Demand value
                array_pos += 1
                idx += 1

            # Record indices for this bus
            self.demand_indices.append((start_idx, array_pos))
            self._data.append({"number": bus_number, "name": name})
            bus_number += 1
        finally:
            # Explicitly clear the lines list to free memory
            lines.clear()
            del lines

    def get_demands(self) -> List[Dict[str, Union[int, str, npt.NDArray]]]:
        """Return the parsed demands structure with numpy arrays.

        Returns:
            List of dictionaries where each contains:
            - number: int - Bus number
            - name: str - Bus name
            - blocks: NDArray[np.int32] - Block numbers
            - values: NDArray[np.float64] - Demand values
        """
        demands = []
        for i, data in enumerate(self._data):
            start, end = self.demand_indices[i]
            demands.append(
                {
                    "number": data["number"],
                    "name": data["name"],
                    "blocks": self.demand_blocks[start:end],
                    "values": self.demand_values[start:end],
                }
            )
        return demands

    def get_demand_arrays(
        self,
    ) -> Tuple[npt.NDArray[np.int32], npt.NDArray[np.float64]]:
        """Return the demand blocks and values as numpy arrays.

        Returns:
            Tuple of (blocks_array, values_array) where:
            - blocks_array: int32 array of block numbers
            - values_array: float64 array of demand values
        """
        return self.demand_blocks, self.demand_values

    def get_bus_demands(
        self, bus_idx: int
    ) -> Tuple[npt.NDArray[np.int32], npt.NDArray[np.float64]]:
        """Get demand blocks and values for a specific bus.

        Args:
            bus_idx: Index of the bus (0-based)

        Returns:
            Tuple of (blocks, values) arrays for the specified bus
        """
        start, end = self.demand_indices[bus_idx]
        return self.demand_blocks[start:end], self.demand_values[start:end]

    def get_total_demand_per_block(self) -> Dict[int, float]:
        """Calculate total demand across all buses per block.

        Returns:
            Dictionary mapping block numbers to total demand values
        """
        totals = {}
        unique_blocks = np.unique(self.demand_blocks)
        for block in unique_blocks:
            mask = self.demand_blocks == block
            totals[int(block)] = float(np.sum(self.demand_values[mask]))
        return totals

    def get_demand_stats(self) -> Dict[str, float]:
        """Calculate basic statistics on demand values.

        Returns:
            Dictionary with:
            - min: Minimum demand value
            - max: Maximum demand value
            - mean: Average demand value
            - median: Median demand value
            - std: Standard deviation
        """
        return {
            "min": float(np.min(self.demand_values)),
            "max": float(np.max(self.demand_values)),
            "mean": float(np.mean(self.demand_values)),
            "median": float(np.median(self.demand_values)),
            "std": float(np.std(self.demand_values)),
        }

    def get_num_bars(self) -> int:
        """Return the number of bars in the file."""
        return self.num_demands

    def get_demand_by_name(
        self, name: str
    ) -> Optional[Dict[str, Union[int, str, npt.NDArray]]]:
        """Get demand data for a specific bus name.

        Returns:
            Dictionary with keys:
            - number: Bus number
            - name: Bus name
            - blocks: Block numbers array
            - values: Demand values array
            or None if not found
        """
        for i, data in enumerate(self._data):
            if data["name"] == name:
                start, end = self.demand_indices[i]
                return {
                    "number": data["number"],
                    "name": data["name"],
                    "blocks": self.demand_blocks[start:end],
                    "values": self.demand_values[start:end],
                }
        return None


def main(args: Optional[List[str]] = None) -> int:
    """Command line entry point for demand file analysis.

    Args:
        args: Command line arguments (uses sys.argv if None)

    Returns:
        int: Exit status (0 for success)
    """
    if args is None:
        args = sys.argv[1:]

    if len(args) != 1:
        print(f"Usage: {sys.argv[0]} <plpdem.dat file>", file=sys.stderr)
        return 1

    try:
        input_path = Path(args[0])
        if not input_path.exists():
            raise FileNotFoundError(f"Demand file not found: {input_path}")

        parser = DemandParser(str(input_path))
        parser.parse()

        print(f"\nDemand File Analysis: {parser.file_path.name}")
        print("=" * 40)
        print(f"Total bars: {parser.get_num_bars()}")

        demands = parser.get_demands()
        total_entries = sum(len(d["demands"]) for d in demands)
        print(f"Total demand entries: {total_entries}")

        _print_demand_stats(demands)
        return 0
    except (FileNotFoundError, ValueError, IndexError) as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        return 1


def _print_demand_stats(demands: List[Dict[str, Any]]) -> None:
    """Print formatted demand statistics."""
    bar_stats = []
    for demand in demands:
        demand_list = demand["demands"]
        count = len(demand_list)
        avg = sum(d["demand"] for d in demand_list) / count if count > 0 else 0
        bar_stats.append({"name": demand["name"], "count": count, "avg": avg})

    bar_stats.sort(key=lambda x: x["avg"], reverse=True)
    _print_stats_table(bar_stats)


def _print_stats_table(stats: List[Dict[str, Any]], limit: int = 10) -> None:
    """Print formatted statistics table."""
    print("\nBar Demand Statistics:")
    print("=" * 40)

    if len(stats) <= 2 * limit:
        print("All bars (sorted by average demand):")
        for stat in stats:
            _print_stat_row(stat)
    else:
        print(f"Top {limit} bars by average demand:")
        for stat in stats[:limit]:
            _print_stat_row(stat)

        print(f"\nBottom {limit} bars by average demand:")
        for stat in stats[-limit:]:
            _print_stat_row(stat)


def _print_stat_row(stat: Dict[str, Any]) -> None:
    """Print single statistic row."""
    print(f"\nBar: {stat['name']}")
    print(f"  Demand entries: {stat['count']}")
    print(f"  Average demand: {stat['avg']:.2f}")


if __name__ == "__main__":
    sys.exit(main())
