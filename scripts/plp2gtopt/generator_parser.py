#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpcnfce.dat format files containing generator data.

Handles:
- File parsing and validation
- Generator data structure creation
- Generator lookup by bus
- Battery storage identification
"""

import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Union


class GeneratorParser:
    """Parser for plpcnfce.dat format files containing generator data.

    Attributes:
        file_path: Path to the generator file
        generators: List of parsed generator entries
    """

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with generator file path.

        Args:
            file_path: Path to plpcnfce.dat format file (str or Path)
        """
        self.file_path = Path(file_path) if isinstance(file_path, str) else file_path
        self.generators: List[Dict[str, Any]] = []

    def parse(self) -> None:
        """Parse the generator file and populate the generators structure.

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
        """
        if not self.file_path.exists():
            raise FileNotFoundError(f"Generator file not found: {self.file_path}")

        current_gen: Dict[str, Any] = {}

        with open(self.file_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()

                # Skip comments and empty lines
                if not line or line.startswith("#"):
                    continue

                # Generator header line
                if line[0].isdigit():
                    if current_gen:
                        self._finalize_generator(current_gen)

                    parts = line.split()
                    current_gen = {
                        "id": parts[0],
                        "name": parts[1].strip("'"),
                        "bus": "0",  # Default if not found
                        "p_min": 0.0,
                        "p_max": 0.0,
                        "start_cost": 0.0,
                        "variable_cost": 0.0,
                        "efficiency": 1.0,
                        "is_battery": False
                    }
                # Power limits line
                elif line.startswith("PotMin"):
                    if not current_gen:
                        continue
                    # Skip the header line and read the next line for values
                    next_line = next(f).strip()
                    parts = next_line.split()
                    current_gen["p_min"] = float(parts[0])
                    current_gen["p_max"] = float(parts[1])

                # Cost and bus line
                elif line.startswith("CosVar"):
                    if not current_gen:
                        continue
                    # Skip the header line and read the next line for values
                    next_line = next(f).strip()
                    while not next_line:  # Skip empty lines
                        next_line = next(f).strip()
                    parts = next_line.split()
                    if len(parts) >= 5:  # Ensure we have all expected columns
                        current_gen["variable_cost"] = float(parts[0])
                        current_gen["efficiency"] = float(parts[1])
                        current_gen["bus"] = parts[3]  # Barra column

                    # Check for battery in name
                    if "BESS" in current_gen["name"].upper():
                        current_gen["is_battery"] = True
            # Add last generator
            if current_gen:
                self._finalize_generator(current_gen)

    def _finalize_generator(self, gen: Dict[str, Any]) -> None:
        """Validate and add a completed generator to the list."""
        if gen["p_max"] > 0:  # Only add generators with positive capacity
            self.generators.append(gen)

    def get_generators(self) -> List[Dict[str, Any]]:
        """Return the parsed generators structure."""
        return self.generators

    def get_num_generators(self) -> int:
        """Return the number of generators in the file."""
        return len(self.generators)

    def get_generators_by_bus(self, bus_id: str) -> List[Dict[str, Any]]:
        """Get all generators connected to a specific bus."""
        return [g for g in self.generators if g["bus"] == bus_id]


def main(args: Optional[List[str]] = None) -> int:
    """Command line entry point for generator file analysis.

    Args:
        args: Command line arguments (uses sys.argv if None)

    Returns:
        int: Exit status (0 for success)
    """
    if args is None:
        args = sys.argv[1:]

    if len(args) != 1:
        print(f"Usage: {sys.argv[0]} <plpgen.dat file>", file=sys.stderr)
        return 1

    try:
        input_path = Path(args[0])
        if not input_path.exists():
            raise FileNotFoundError(f"Generator file not found: {input_path}")

        parser = GeneratorParser(str(input_path))
        parser.parse()

        print(f"\nGenerator File Analysis: {parser.file_path.name}")
        print("=" * 40)
        print(f"Total generators: {parser.get_num_generators()}")

        generators = parser.get_generators()
        for gen in generators:
            print(f"\nGenerator: {gen['id']}")
            print(f"  Bus: {gen['bus']}")
            print(f"  Pmin: {gen['p_min']}")
            print(f"  Pmax: {gen['p_max']}")

        return 0
    except (FileNotFoundError, ValueError, IndexError) as e:
        print(f"Error: {str(e)}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
