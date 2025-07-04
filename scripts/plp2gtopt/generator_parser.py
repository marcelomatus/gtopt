#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Parser for plpgen.dat format files containing generator data.

Handles:
- File parsing and validation
- Generator data structure creation
- Generator lookup by bus
"""

import sys
from pathlib import Path
from typing import Any, Optional, List, Dict, Union


class GeneratorParser:
    """Parser for plpgen.dat format files containing generator data.

    Attributes:
        file_path: Path to the generator file
        generators: List of parsed generator entries
        num_generators: Number of generators in the file
    """

    def __init__(self, file_path: Union[str, Path]) -> None:
        """Initialize parser with generator file path.

        Args:
            file_path: Path to plpgen.dat format file (str or Path)
        """
        self.file_path = Path(file_path) if isinstance(file_path, str) else file_path
        self.generators: List[Dict[str, Any]] = []
        self.num_generators: int = 0

    def parse(self) -> None:
        """Parse the generator file and populate the generators structure.

        Raises:
            FileNotFoundError: If input file doesn't exist
            ValueError: If file format is invalid
            IndexError: If file is empty or malformed
        """
        if not self.file_path.exists():
            raise FileNotFoundError(f"Generator file not found: {self.file_path}")

        with open(self.file_path, "r", encoding="utf-8") as f:
            # Skip initial comments and empty lines
            lines = []
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    lines.append(line)

        idx = 0
        self.num_generators = int(lines[idx])
        idx += 1

        for _ in range(self.num_generators):
            parts = lines[idx].split()
            if len(parts) < 4:
                raise ValueError(f"Invalid generator entry at line {idx+1}")

            self.generators.append(
                {
                    "id": parts[0],
                    "bus": parts[1],
                    "p_min": float(parts[2]),
                    "p_max": float(parts[3]),
                    # Add other generator attributes as needed
                }
            )
            idx += 1

    def get_generators(self) -> List[Dict[str, Any]]:
        """Return the parsed generators structure."""
        return self.generators

    def get_num_generators(self) -> int:
        """Return the number of generators in the file."""
        return self.num_generators

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
