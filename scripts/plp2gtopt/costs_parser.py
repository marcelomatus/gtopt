#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpcosce.dat format files containing generator cost data."""

import sys
from pathlib import Path
from typing import Any, Optional, List, Dict, Union, Tuple
import numpy as np
import numpy.typing as npt

from .base_parser import BaseParser


class CostsParser(BaseParser):
    """Parser for plpcosce.dat format files containing generator cost data."""

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with cost file path.

        Args:
            file_path: Path to plpcosce.dat format file (str or Path)
        """
        super().__init__(file_path)
        self._data: List[Dict[str, Any]] = []  # Stores cost data per generator
        self.num_generators: int = 0

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
        self.num_generators = self._parse_int(lines[idx])
        idx += 1

        try:
            for _ in range(self.num_generators):
                # Get generator name
                if idx >= len(lines):
                    raise ValueError("Unexpected end of file while parsing generator names")
                name = lines[idx].strip("'").split("#")[0].strip()
                idx += 1

                # Get number of cost entries
                if idx >= len(lines):
                    raise ValueError("Unexpected end of file while parsing stage counts")

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

                # Skip header line
                if idx >= len(lines):
                    raise ValueError("Unexpected end of file after stage count")
                idx += 1

                # Initialize numpy arrays for this generator
                stages = np.empty(num_stages, dtype=np.int32)
                costs = np.empty(num_stages, dtype=np.float64)

                # Parse cost entries
                for i in range(num_stages):
                    if idx >= len(lines):
                        raise ValueError("Unexpected end of file while parsing cost entries")

                    parts = lines[idx].split()
                    if len(parts) < 2:
                        raise ValueError(f"Invalid cost entry at line {idx+1}")

                    stages[i] = int(parts[0])  # Stage number
                    costs[i] = float(parts[1])  # Cost value
                    idx += 1

                # Store complete data for this generator
                self._data.append({
                    "name": name,
                    "stages": stages,
                    "costs": costs
                })
        finally:
            lines.clear()
            del lines

    def get_costs(self) -> List[Dict[str, Any]]:
        """Return the parsed costs structure with numpy arrays.

        Returns:
            List of cost dictionaries with these keys:
            - name (str): Generator name
            - months (np.ndarray): Month numbers as int32 array
            - stages (np.ndarray): Stage numbers as int32 array
            - costs (np.ndarray): Cost values as float64 array
        """
        return self._data

    def get_cost_arrays(
        self,
    ) -> Tuple[List[npt.NDArray[np.int32]], List[npt.NDArray[np.float64]]]:
        """Return lists of stages and costs arrays for all generators.

        Returns:
            Tuple of (stages_list, costs_list) where:
            - stages_list: List of int32 arrays of stage numbers
            - costs_list: List of float64 arrays of cost values
        """
        stages = [d["stages"] for d in self._data]
        costs = [d["costs"] for d in self._data]
        return stages, costs

    def get_generator_costs(
        self, gen_idx: int
    ) -> Tuple[npt.NDArray[np.int32], npt.NDArray[np.float64]]:
        """Get cost data for a specific generator.

        Args:
            gen_idx: Index of the generator (0-based)

        Returns:
            Tuple of (stages, costs) arrays for the specified generator
        """
        gen_data = self._data[gen_idx]
        return gen_data["stages"], gen_data["costs"]

    def get_cost_stats(self) -> Dict[str, float]:
        """Calculate basic statistics on cost values.

        Returns:
            Dictionary with:
            - min: Minimum cost value
            - max: Maximum cost value
            - mean: Average cost value
            - median: Median cost value
            - std: Standard deviation
        """
        all_costs = np.concatenate([d["costs"] for d in self._data])
        return {
            "min": float(np.min(all_costs)),
            "max": float(np.max(all_costs)),
            "mean": float(np.mean(all_costs)),
            "median": float(np.median(all_costs)),
            "std": float(np.std(all_costs)),
        }

    def get_num_generators(self) -> int:
        """Return the number of generators in the file."""
        return self.num_generators

    def get_costs_by_name(
        self, name: str
    ) -> Optional[Dict[str, Union[str, npt.NDArray]]]:
        """Get cost data for a specific generator name.

        Returns:
            Dictionary with keys:
            - name: Generator name
            - months: Month numbers array
            - stages: Stage numbers array
            - costs: Cost values array
            or None if not found
        """
        for data in self._data:
            if data["name"] == name:
                return {
                    "name": data["name"],
                    "months": data["months"],
                    "stages": data["stages"],
                    "costs": data["costs"]
                }
        return None


def main(args: Optional[List[str]] = None) -> int:
    """Command line entry point for cost file analysis.

    Args:
        args: Command line arguments (uses sys.argv if None)

    Returns:
        int: Exit status (0 for success)
    """
    if args is None:
        args = sys.argv[1:]

    if len(args) != 1:
        print(f"Usage: {sys.argv[0]} <plpcosce.dat file>", file=sys.stderr)
        return 1

    try:
        input_path = Path(args[0])
        if not input_path.exists():
            raise FileNotFoundError(f"Cost file not found: {input_path}")

        parser = CostsParser(str(input_path))
        parser.parse()

        print(f"\nCost File Analysis: {parser.file_path.name}")
        print("=" * 40)
        print(f"Total generators: {parser.get_num_generators()}")

        costs = parser.get_costs()
        total_entries = sum(len(d["costs"]) for d in costs)
        print(f"Total cost entries: {total_entries}")

        stats = parser.get_cost_stats()
        print("\nCost Statistics:")
        print(f"  Min: {stats['min']:.2f}")
        print(f"  Max: {stats['max']:.2f}")
        print(f"  Mean: {stats['mean']:.2f}")
        print(f"  Median: {stats['median']:.2f}")
        print(f"  Std Dev: {stats['std']:.2f}")

        return 0
    except (FileNotFoundError, ValueError, IndexError) as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
