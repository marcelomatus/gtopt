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
        self._data: List[Dict[str, Any]] = []  # Will store complete demand data per bus
        self.num_demands: int = 0

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
        self.num_demands = self._parse_int(lines[idx])
        idx += 1

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
                    raise ValueError(
                        "Unexpected end of file while parsing block counts"
                    )

                # Skip empty lines between bus name and block count
                while idx < len(lines) and not lines[idx].strip():
                    idx += 1

                try:
                    num_blocks = int(lines[idx].strip().split()[0])
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
                self._data.append(
                    {
                        "number": bus_number,
                        "name": name,
                        "blocks": blocks,
                        "values": values,
                    }
                )
                bus_number += 1
        finally:
            lines.clear()
            del lines

    def get_demands(self) -> List[Dict[str, Any]]:
        """Return the parsed demands structure with numpy arrays.

        Returns:
            List of demand dictionaries with these keys:
            - number (int): Bus number (1-based)
            - name (str): Bus name
            - blocks (np.ndarray): Block numbers as int32 array
            - values (np.ndarray): Demand values as float64 array

        Example:
            >>> demands = parser.get_demands()
            >>> demands[0]["name"]
            'Coronel066'
            >>> demands[0]["blocks"].shape
            (5,)
        """
        return self._data

    def get_demand_arrays(
        self,
    ) -> Tuple[List[npt.NDArray[np.int32]], List[npt.NDArray[np.float64]]]:
        """Return lists of blocks and values arrays for all buses.

        Returns:
            Tuple of (blocks_list, values_list) where:
            - blocks_list: List of int32 arrays of block numbers
            - values_list: List of float64 arrays of demand values
        """
        blocks = [d["blocks"] for d in self._data]
        values = [d["values"] for d in self._data]
        return blocks, values

    def get_bus_demands(
        self, bus_idx: int
    ) -> Tuple[npt.NDArray[np.int32], npt.NDArray[np.float64]]:
        """Get demand blocks and values for a specific bus.

        Args:
            bus_idx: Index of the bus (0-based)

        Returns:
            Tuple of (blocks, values) arrays for the specified bus
        """
        bus_data = self._data[bus_idx]
        return bus_data["blocks"], bus_data["values"]

    def get_total_demand_per_block(self) -> Dict[int, float]:
        """Calculate total demand across all buses per block.

        Returns:
            Dictionary mapping block numbers to total demand values
        """
        totals = {}
        for bus_data in self._data:
            for block, value in zip(bus_data["blocks"], bus_data["values"]):
                totals[block] = totals.get(block, 0.0) + value
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
        all_values = np.concatenate([d["values"] for d in self._data])
        return {
            "min": float(np.min(all_values)),
            "max": float(np.max(all_values)),
            "mean": float(np.mean(all_values)),
            "median": float(np.median(all_values)),
            "std": float(np.std(all_values)),
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
        for data in self._data:
            if data["name"] == name:
                return {
                    "number": data["number"],
                    "name": data["name"],
                    "blocks": data["blocks"],
                    "values": data["values"],
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
        total_entries = sum(len(d["values"]) for d in demands)
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
        count = len(demand["values"])
        avg = np.mean(demand["values"]) if count > 0 else 0
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
